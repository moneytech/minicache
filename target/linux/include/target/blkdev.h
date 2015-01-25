#ifndef _BLKDEV_H_
#define _BLKDEV_H_

#include <linux/fs.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <libaio.h>
#include <semaphore.h>
#include <mempool.h>

#define MAX_REQUESTS 1024
#define DEFAULT_SSIZE 512 /* lower bound for opened files */

typedef char blkdev_id_t[PATH_MAX]; /* device id is a path */
typedef uint64_t sector_t;
#define PRIsctr PRIu64

typedef void (blkdev_aiocb_t)(int ret, void *argp);

struct blkdev {
  blkdev_id_t dev;
  int fd;
  int mode;
  struct stat fd_stat;
  sector_t size;
  uint32_t ssize;
  struct mempool *reqpool;
  io_context_t ctxp;

  int exclusive;
  unsigned int refcount;
  struct blkdev *_next;
  struct blkdev *_prev;
};

struct _blkdev_req {
  struct mempool_obj *p_obj; /* reference to dependent memory pool object */
  struct blkdev *bd;
  struct iocb aiocb;
  sector_t sector;
  sector_t nb_sectors;
  int write;
  blkdev_aiocb_t *cb;
  void *cb_argp;
};

struct blkdev *open_blkdev(blkdev_id_t id, int mode);
void close_blkdev(struct blkdev *bd);
#define blkdev_refcount(bd) ((bd)->refcount)

int blkdev_id_parse(const char *id, blkdev_id_t *out);
#define blkdev_id_unparse(id, out, maxlen) \
     (snprintf((out), (maxlen), "%s", (id)))
#define blkdev_id_cmp(id0, id1) \
     (strncmp((id0), (id1), PATH_MAX))
#define blkdev_id_cpy(dst, src) \
     (strncpy((dst), (src), PATH_MAX))
#define blkdev_id(bd) ((bd)->dev)
#define blkdev_ioalign(bd) blkdev_ssize((bd))

/**
 * Retrieve device information
 */
#define blkdev_ssize(bd) ((uint32_t) (bd)->ssize)
#define blkdev_size(bd) ((bd)->size * (sector_t) blkdev_ssize((bd)))
#define blkdev_avail_req(bd) mempool_free_count((bd)->reqpool)


/**
 * Async I/O
 *
 * Note: target buffer has to be aligned to device sector size
 */
void _blkdev_io_cb(io_context_t ctx, struct iocb *iocb, long res, long res2);

static inline int blkdev_async_io_nocheck(struct blkdev *bd, sector_t start, sector_t len,
                                          int write, void *buffer, blkdev_aiocb_t *cb, void *cb_argp)
{
  struct mempool_obj *robj;
  struct _blkdev_req *req;

  robj = mempool_pick(bd->reqpool);
  if (unlikely(!robj))
	return -EAGAIN; /* too many requests on queue */

  req = robj->data;
  req->p_obj = robj;

  memset(&req->aiocb, 0, sizeof(req->aiocb));
  req->aiocb.aio_fildes = bd->fd;
  req->aiocb.aio_lio_opcode = write ? IO_CMD_PWRITE : IO_CMD_PREAD;
  //req->aiocb.reqprio = 0;
  req->aiocb.u.c.buf = buffer;
  req->aiocb.u.c.offset = (off_t) (start * blkdev_ssize(bd));
  req->aiocb.u.c.nbytes = len * blkdev_ssize(bd);
  //req->aiocb.aio_cb = _blkdev_async_io_cb;
  req->bd = bd;
  req->sector = start;
  req->nb_sectors = len;
  req->write = write;
  req->cb = cb;
  req->cb_argp = cb_argp;

  blkfront_aio(&(req->aiocb), write);
  return 0;
}
#define blkdev_async_write_nocheck(bd, start, len, buffer, cb, cb_argp) \
	blkdev_async_io_nocheck((bd), (start), (len), 1, (buffer), (cb), (cb_argp))
#define blkdev_async_read_nocheck(bd, start, len, buffer, cb, cb_argp) \
	blkdev_async_io_nocheck((bd), (start), (len), 0, (buffer), (cb), (cb_argp))

static inline int blkdev_async_io(struct blkdev *bd, sector_t start, sector_t len,
                                  int write, void *buffer, blkdev_aiocb_t *cb, void *cb_argp)
{
	if (unlikely(write && !(bd->mode & (O_WRONLY | O_RDWR)))) {
		/* write access on non-writable device or read access on non-readable device */
		return -EACCES;
	}

	return blkdev_async_io_nocheck(bd, start, len, write, buffer, cb, cb_argp);
}
#define blkdev_async_write(bd, start, len, buffer, cb, cb_argp)	  \
	blkdev_async_io((bd), (start), (len), 1, (buffer), (cb), (cb_argp))
#define blkdev_async_read(bd, start, len, buffer, cb, cb_argp)	  \
	blkdev_async_io((bd), (start), (len), 0, (buffer), (cb), (cb_argp))

#define blkdev_poll_req(bd) blkfront_aio_poll((bd)->dev);

/**
 * Sync I/O
 */
void _blkdev_sync_io_cb(int ret, void *argp);

struct _blkdev_sync_io_sync {
	sem_t sem;
	int ret;
};

static inline int blkdev_sync_io_nocheck(struct blkdev *bd, sector_t start, sector_t len,
                                             int write, void *target)
{
	struct _blkdev_sync_io_sync iosync;
	int ret;

	init_SEMAPHORE(&iosync.sem, 0);
	ret = blkdev_async_io_nocheck(bd, start, len, write, target,
	                              _blkdev_sync_io_cb, &iosync);
	while (ret == -EAGAIN) {
		/* try again, queue was full */
		blkdev_poll_req(bd);
		schedule();
		ret = blkdev_async_io_nocheck(bd, start, len, write, target,
		                              _blkdev_sync_io_cb, &iosync);
	}
	if (ret < 0)
		return ret;

	/* wait for I/O completion */
	blkdev_poll_req(bd);
	while (trydown(&iosync.sem) == 0) {
		schedule(); /* yield CPU */
		blkdev_poll_req(bd);
	}

	return iosync.ret;
}
#define blkdev_sync_write_nocheck(bd, start, len, buffer)	  \
	blkdev_sync_io_nocheck((bd), (start), (len), 1, (buffer))
#define blkdev_sync_read_nocheck(bd, start, len, buffer)	  \
	blkdev_sync_io_nocheck((bd), (start), (len), 0, (buffer))

static inline int blkdev_sync_io(struct blkdev *bd, sector_t start, sector_t len,
                                 int write, void *target)
{
	struct _blkdev_sync_io_sync iosync;
	int ret;

	init_SEMAPHORE(&iosync.sem, 0);
	ret = blkdev_async_io(bd, start, len, write, target,
	                      _blkdev_sync_io_cb, &iosync);
	while (ret == -EAGAIN) {
		/* try again, queue was full */
		blkdev_poll_req(bd);
		schedule();
		ret = blkdev_async_io(bd, start, len, write, target,
		                      _blkdev_sync_io_cb, &iosync);
	}
	if (ret < 0)
		return ret;

	/* wait for I/O completion */
	blkdev_poll_req(bd);
	while (trydown(&iosync.sem) == 0) {
		schedule(); /* yield CPU */
		blkdev_poll_req(bd);
	}

	return iosync.ret;
}
#define blkdev_sync_write(bd, start, len, buffer)	  \
	blkdev_sync_io((bd), (start), (len), 1, (buffer))
#define blkdev_sync_read(bd, start, len, buffer)	  \
	blkdev_sync_io((bd), (start), (len), 0, (buffer))

#endif /* _BLKDEV_H_ */
