#include <mini-os/os.h>
#include <mini-os/xmalloc.h>

#include "shfs_stats.h"
#include "shfs_tools.h"
#include "shfs.h"
#include "htable.h"
#include "ctldir.h"
#include "blkdev.h"
#include "shell.h"

#ifndef member_size
#define member_size(s, m) sizeof(((s *) 0)->m)
#endif

int shfs_init_mstats(uint32_t nb_bkts, uint32_t ent_per_bkt, uint8_t hlen)
{
	shfs_vol.mstats.el_ht = alloc_htable(nb_bkts, ent_per_bkt, hlen,
	                                     sizeof(struct shfs_el_stats), 0);
	if (!shfs_vol.mstats.el_ht)
		return -errno;
	shfs_vol.mstats.i = 0;
	shfs_vol.mstats.e = 0;

	return 0;
}

void shfs_free_mstats(void)
{
	free_htable(shfs_vol.mstats.el_ht);
}

int shfs_dump_mstats(shfs_dump_el_stats_t dump_el, void *dump_el_argp) {
	int ret;
	struct htable_el *el;

	foreach_htable_el(shfs_vol.mstats.el_ht, el) {
		ret = dump_el(dump_el_argp, *el->h, 0,
		              (struct shfs_el_stats *) el->private);
		if (ret < 0)
			return ret;
	}

	return 0;
}

int shfs_dump_hstats(shfs_dump_el_stats_t dump_el, void *dump_el_argp) {
	int ret;
	struct htable_el *el;

	foreach_htable_el(shfs_vol.bt, el) {
		ret = dump_el(dump_el_argp, *el->h, 1,
		              shfs_stats_from_bentry((struct shfs_bentry *) el->private));
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* -------------------------------------------------------------------
 * SHFS stats exporter
 * ------------------------------------------------------------------- */
struct _stats_dev {
	struct blkdev *bd;
	void *buf;
	size_t seek;
	size_t flushed;

	struct semaphore lock;
};

static struct _stats_dev *_stats_dev = NULL;

/* stats print */
static int _shcmd_shfs_print_el_stats(void *argp, hash512_t h, int available, struct shfs_el_stats *stats)
{
	FILE *cio = (FILE *) argp;
	char str_hash[(shfs_vol.hlen * 2) + 1];
	char str_date[20];
#if defined SHFS_STATS_HTTP && defined SHFS_STATS_HTTP_DPC
	register unsigned int i;
#endif

	if (stats->laccess) {
		hash_unparse(h, shfs_vol.hlen, str_hash);
		strftimestamp_s(str_date, sizeof(str_date),
		                "%b %e, %g %H:%M", stats->laccess);
#ifdef SHFS_STATS_HTTP
		fprintf(cio, "%c%s %c%c %6u [ %6u | ",
		        SFHS_HASH_INDICATOR_PREFIX,
		        str_hash,
		        available ? 'I' : ' ',
		        available ? 'N' : ' ',
		        stats->h, /* hits */
		        stats->c ); /* completed file request */
#ifdef SHFS_STATS_HTTP_DPC
		for (i=0; i<SHFS_STATS_HTTP_DPCR; ++i)
			fprintf(cio, "%6u ", stats->p[i]);
#endif
		fprintf(cio, "] %6u %-16s\n",
			stats->m, /* missed */
		        str_date);
#else
		fprintf(cio, "%c%s %c%c %8u %8u %-16s\n",
		        SFHS_HASH_INDICATOR_PREFIX,
		        str_hash,
		        available ? 'I' : ' ',
		        available ? 'N' : ' ',
		        stats->h, /* hits */
		        stats->m, /* missed */
		        str_date);
#endif
	}

	return 0;
}

static int shcmd_shfs_stats(FILE *cio, int argc, char *argv[])
{
	int ret = 0;

	down(&shfs_mount_lock);
	if (!shfs_mounted) {
		fprintf(cio, "No SHFS filesystem mounted\n");
		ret = -1;
		goto out;
	}

	shfs_dump_stats(_shcmd_shfs_print_el_stats, cio);
	if (shfs_vol.mstats.i)
		fprintf(cio, "Invalid element requests: %8lu\n", shfs_vol.mstats.i);
	if (shfs_vol.mstats.e)
		fprintf(cio, "Errors on requests:       %8lu\n", shfs_vol.mstats.e);

 out:
	up(&shfs_mount_lock);
	return ret;
}


/* stats export */
#define _stats_dev_reset() do { _stats_dev->seek = 0; _stats_dev->flushed = 0; } while(0)

static int _stats_dev_flush(void)
{
	/* Note lock has to be held by caller! */
	register sector_t sec;
	register size_t bleft, bpos;
	int ret;

	if (_stats_dev->seek <= _stats_dev->flushed)
		return 0; /* nothing to write */


	/* calculate address */
	sec = _stats_dev->seek / blkdev_ssize(_stats_dev->bd);
	if (_stats_dev->seek % blkdev_ssize(_stats_dev->bd) == 0) {
		/* correct sec because buffer points to previous
		 * that is filled up completely */
		--sec;
	} else {
		/* fillup rest of buffer with zeros */
		bpos = _stats_dev->seek % blkdev_ssize(_stats_dev->bd); /* pos on buffer */
		bleft = blkdev_ssize(_stats_dev->bd) - bpos; /* left bytes on buffer */
		memset((uint8_t *) _stats_dev->buf + bpos, 0, bleft);
	}

	/* write */
	ret = blkdev_sync_io(_stats_dev->bd, sec, 1, 1, _stats_dev->buf); /* yields CPU */
	if (ret >= 0)
		_stats_dev->flushed = _stats_dev->seek;
	return ret;
}


static int _stats_dev_write(void *data, size_t len)
{
	/* Note lock has to be held by caller! */
	register size_t bleft;
	register size_t bpos, dpos;
	register size_t clen;
	int ret;

	dpos = 0;
	while (len) {
		bpos = _stats_dev->seek % blkdev_ssize(_stats_dev->bd); /* pos on buffer */
		bleft = blkdev_ssize(_stats_dev->bd) - bpos; /* left bytes on buffer */
		clen = min(bleft, len);

		memcpy((uint8_t *) _stats_dev->buf + bpos, (uint8_t *) data + dpos, clen);
		_stats_dev->seek += clen;

		if (bpos + clen == blkdev_ssize(_stats_dev->bd)) {
			/* write buffer to disk and clear it */
			ret = _stats_dev_flush();
			if (ret < 0) {
				_stats_dev->seek -= clen; /* revert increasing seek */
				return ret;
			}
		}

		dpos += clen;
		len  -= clen;
	}

	return 0;
}

static int _shcmd_shfs_export_el_stats(void *argp, hash512_t h, int available, struct shfs_el_stats *stats)
{
#if defined SHFS_STATS_HTTP && defined SHFS_STATS_HTTP_DPC
	register unsigned int i;
#endif
	char sbuf[256];
	size_t slen;
	int ret = 0;

	hash_unparse(h, shfs_vol.hlen, sbuf);
	ret = _stats_dev_write(sbuf, shfs_vol.hlen * 2);
	if (unlikely(ret < 0))
		goto out;

	slen = snprintf(sbuf, sizeof(sbuf), ";%lu;%lu;%lu",
	                stats->laccess,
	                stats->h,
	                stats->m);
	ret = _stats_dev_write(sbuf, slen);
	if (unlikely(ret < 0))
		goto out;

#ifdef SHFS_STATS_HTTP
	slen = snprintf(sbuf, sizeof(sbuf), ";%lu", stats->c);
	ret = _stats_dev_write(sbuf, slen);
	if (unlikely(ret < 0))
		goto out;

#ifdef SHFS_STATS_HTTP_DPC
	for (i=0; i<SHFS_STATS_HTTP_DPCR; ++i) {
		slen = snprintf(sbuf, sizeof(sbuf), ";%lu", stats->p[i]);
		ret = _stats_dev_write(sbuf, slen);
		if (unlikely(ret < 0))
			goto out;
	}
#endif
#endif
	_stats_dev_write("\n", 1); /* exclusive terminating zero */
	if (unlikely(ret < 0))
		goto out;

 out:
	return ret;
}

static int shcmd_shfs_stats_export(FILE *cio, int argc, char *argv[])
{
#if defined SHFS_STATS_HTTP && defined SHFS_STATS_HTTP_DPC
	register unsigned int i;
#endif
	char sbuf[128];
	int slen;
	int ret = 0;

	down(&shfs_mount_lock);
	if (!shfs_mounted) {
		fprintf(cio, "No SHFS filesystem mounted\n");
		ret = -1;
		goto out;
	}

	_stats_dev_reset();

	slen = snprintf(sbuf, sizeof(sbuf),
	                "x%uk(hash);u%ug(laccess);u%us(hits);u%us(miss)",
	                shfs_vol.hlen,
	                member_size(struct shfs_el_stats, laccess),
	                member_size(struct shfs_el_stats, h),
	                member_size(struct shfs_el_stats, m));
	ret = _stats_dev_write(sbuf, slen);
	if (unlikely(ret < 0))
		goto out;


#ifdef SHFS_STATS_HTTP
	slen = snprintf(sbuf, sizeof(sbuf), ";u%us(completed)",
	                member_size(struct shfs_el_stats, c));
	ret = _stats_dev_write(sbuf, slen);
	if (unlikely(ret < 0))
		goto out;

#ifdef SHFS_STATS_HTTP_DPC
	for (i=0; i<SHFS_STATS_HTTP_DPCR; ++i) {
		slen = snprintf(sbuf, sizeof(sbuf), ";u%us(%d%%)",
		                member_size(struct shfs_el_stats, p[i]),
		                SHFS_STATS_HTTP_DPC_THRESHOLD_PERCENTAGE(i));
		ret = _stats_dev_write(sbuf, slen);
		if (unlikely(ret < 0))
			goto out;
	}
#endif
#endif
	ret = _stats_dev_write("\n", 1); /* exclusive terminating zero */
	if (unlikely(ret < 0))
		goto out;

	ret = shfs_dump_stats(_shcmd_shfs_export_el_stats, NULL);
	if (unlikely(ret < 0))
		goto out;

	ret = _stats_dev_write("", 1); /* terminating zero */
	if (unlikely(ret < 0))
		goto out;

	ret = _stats_dev_flush();
	if (unlikely(ret < 0))
		goto out;

	ret = 0;
 out:
	up(&shfs_mount_lock);
	return ret;
}

int register_shfs_stats_tools(struct ctldir *cd)
{
	shell_register_cmd("stats", shcmd_shfs_stats);

	if (_stats_dev) {
		/* register export-stats only when export device was opened */
		if (cd)
			ctldir_register_shcmd(cd, "export-stats", shcmd_shfs_stats_export);
		shell_register_cmd("export-stats", shcmd_shfs_stats_export);
	}

	return 0;
}

int init_shfs_stats_export(unsigned int vbd_id)
{
	int ret;

	_stats_dev = _xmalloc(sizeof(*_stats_dev), 0);
	if (!_stats_dev) {
		ret = -ENOMEM;
		goto err_out;
	}
	/* exclusively open stats device for write only */
	_stats_dev->bd = open_blkdev(vbd_id, (O_WRONLY | O_EXCL));
	if (!_stats_dev->bd) {
		ret = -errno;
		goto err_free_stats_dev;
	}
	_stats_dev->buf = _xmalloc(blkdev_ssize(_stats_dev->bd), blkdev_ssize(_stats_dev->bd));
	if (!_stats_dev->buf) {
		ret = -ENOMEM;
		goto err_close_bd;
	}

	init_SEMAPHORE(&_stats_dev->lock, 1); /* serializes exports */
	_stats_dev->seek = 0;
	return 0;

 err_close_bd:
	close_blkdev(_stats_dev->bd);
 err_free_stats_dev:
	xfree(_stats_dev);
 err_out:
	return ret;
}

void exit_shfs_stats_export(void)
{
	if (_stats_dev) {
		down(&_stats_dev->lock);

		xfree(_stats_dev->buf);
		close_blkdev(_stats_dev->bd);
		xfree(_stats_dev);
	}
}