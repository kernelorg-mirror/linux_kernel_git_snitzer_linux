/* SPDX-License-Identifier: GPL-2.0 */
#ifndef WB_THROTTLE_H
#define WB_THROTTLE_H

#include <linux/blk_types.h>

static inline bool wbt_should_throttle(struct bio *bio)
{
	switch (bio_op(bio)) {
	case REQ_OP_WRITE:
		/*
		 * Don't throttle WRITE_ODIRECT
		 */
		if ((bio->bi_opf & (REQ_SYNC | REQ_IDLE)) ==
		    (REQ_SYNC | REQ_IDLE))
			return false;
		fallthrough;
	case REQ_OP_DISCARD:
		return true;
	default:
		return false;
	}
}

#ifdef CONFIG_BLK_WBT

void wbt_init_enable_default(struct gendisk *disk);
void wbt_disable_default(struct gendisk *disk);
void wbt_enable_default(struct gendisk *disk);

u64 wbt_get_min_lat(struct request_queue *q);
bool wbt_disabled(struct request_queue *q);
int wbt_set_lat(struct gendisk *disk, s64 val);

#else

static inline void wbt_init_enable_default(struct gendisk *disk)
{
}

static inline void wbt_disable_default(struct gendisk *disk)
{
}
static inline void wbt_enable_default(struct gendisk *disk)
{
}

#endif /* CONFIG_BLK_WBT */

#endif
