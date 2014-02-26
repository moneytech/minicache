/*
 * Simon's HashFS (SHFS) for Mini-OS
 *
 * Copyright(C) 2013-2014 NEC Laboratories Europe. All rights reserved.
 *                        Simon Kuenzer <simon.kuenzer@neclab.eu>
 */
#ifndef _SHFS_H_
#define _SHFS_H_

#include <mini-os/types.h>
#include <stdint.h>
#include <semaphore.h>
#include "blkdev.h"

#include "shfs_defs.h"

#define MAX_NB_TRY_BLKDEVS 64

struct vol_member {
	struct blkdev *bd;
	uuid_t uuid;
	sector_t sfactor;
};

struct vol_info {
	uuid_t uuid;
	char volname[17];
	uint32_t chunksize;
	chk_t volsize;

	uint8_t nb_members;
	struct vol_member member[SHFS_MAX_NB_MEMBERS];
	uint32_t stripesize;

	chk_t htable_ref;
	chk_t htable_bak_ref;
	chk_t htable_len;
	uint32_t htable_nb_buckets;
	uint32_t htable_nb_entries;
	uint32_t htable_nb_entries_per_bucket;
	uint32_t htable_nb_entries_per_chunk;
	uint8_t hlen;
};

extern struct vol_info shfs_vol;
extern struct semaphore shfs_mount_lock;
extern volatile int shfs_mounted;

int init_shfs(void);
int mount_shfs(unsigned int vbd_id[], unsigned int count);
void umount_shfs(void);
void exit_shfs(void);

static inline void shfs_poll_blkdevs(void) {
	unsigned int i;

	if (likely(shfs_mounted))
		for(i = 0; i < shfs_vol.nb_members; ++i)
			blkdev_poll_req(shfs_vol.member[i].bd);
}

/**
 * Slow read: sequential & sync read of chunks
 */
int shfs_sync_read_chunk(chk_t start, chk_t len, void *buffer);

#endif /* _SHFS_H_ */
