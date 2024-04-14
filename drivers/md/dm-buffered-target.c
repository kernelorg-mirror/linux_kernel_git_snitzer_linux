// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019,2023 Red Hat GmbH
 *
 * A test target similar to linear which performs all I/O through dm-bufio
 * for the sake of load testing/demonstrating/learning the dm-bufio API
 * (and potentially gaining some performance with direct I/O).
 *
 * Uses almost? all functions of the dm-bufio API.
 *
 * This file is released under the GPL.
 */

#include <linux/device-mapper.h>
#include <linux/async_tx.h>
#include <linux/dm-bufio.h>
#include <linux/module.h>

#define DM_MSG_PREFIX "buffered"

#define DEFAULT_BUFFERED_BLOCK_SIZE	(SECTOR_SIZE <<  3) /* 4KiB */

/* REMOVEME: development statistics. */
enum { S_BUFFER_SPLITS, S_PREFLUSHS, S_FUA, S_SYNC_WRITES, S_PREFETCHED_READS, S_READS,
       S_PREFETCHED_WRITES, S_BUFFERS_DIRTIED, S_DISCARDS, S_DISCARDS_PASSDOWN, S_END };

/* buffered target context */
struct buffered_c {
	spinlock_t lock; /* Protect following bio list */
	struct bio_list bios;
	struct dm_bufio_client *bufio;
	struct workqueue_struct *buffered_wq;
	struct work_struct buffered_ws;
	struct workqueue_struct *buffered_flush_wq;
	struct delayed_work buffered_flush_ws;
	sector_t start;
	sector_t block_mask;
	unsigned int block_shift;
	unsigned int buffer_size;
	bool async_memcpy;
	bool sync_writes;
	bool discard;
	bool discard_passdown;
	bool write_zeroes;
	struct dm_dev *dev;
	char *table_line;
	/* REMOVEME: stats */
	atomic_t stats[S_END];
};

/* buffer async_memcpy context */
struct bio_c {
	struct async_submit_ctl submit;
	struct buffered_c *bc;
	struct dm_buffer *bp;
	unsigned int buffer_offset;
	unsigned int len;
};

/* Convert sector to bufio block number */
static sector_t _to_block(struct buffered_c *bc, sector_t sector)
{
	return sector >> bc->block_shift;
}

/* Convert block to bufio sector number */
static sector_t _to_sector(struct buffered_c *bc, sector_t block)
{
	return block << bc->block_shift;
}

/* Return sector modulo of block size */
static sector_t _sector_mod(struct buffered_c *bc, sector_t sector)
{
	return sector & bc->block_mask;
}

/* Use unutilized @bio->bi_next as endio reference counter and take out @ref_count number. */
static void _bio_set_references(struct bio *bio, unsigned int ref_count)
{
	atomic_set((atomic_t *)&bio->bi_next, ref_count);
}

/* Use unutilized @bio->bi_next as endio reference counter */
static void _bio_get(struct bio *bio)
{
	atomic_inc((atomic_t *)&bio->bi_next);
}

/* Perform bio endio completion when unreferenced using bio clone's unutilized bi_next member */
static void _bio_put(struct bio *bio)
{
	if (atomic_dec_and_test((atomic_t *)&bio->bi_next))
		bio_endio(bio);
}

/* Flush any dirty buffers of @bc out */
static blk_status_t _buffered_flush(struct buffered_c *bc)
{
	return errno_to_blk_status(dm_bufio_write_dirty_buffers(bc->bufio));
}

/* async_memcpy() completion callback */
static void _complete_memcpy(void *context)
{
	struct bio *bio = context;
	struct bio_c *bio_c = dm_per_bio_data(bio, sizeof(*bio_c));
	struct buffered_c *bc = bio_c->bc;
	struct dm_buffer *bp = bio_c->bp;
	unsigned int len = bio_c->len;

	/* Avoid race with bufio flushing code on async memory copy by dirtying here. */
	if (len) {
		unsigned int buffer_offset = bio_c->buffer_offset;

		/* Superfluous conditional to show both functions dirtying buffers. */
		if (!buffer_offset && len == dm_bufio_get_block_size(bc->bufio))
			dm_bufio_mark_buffer_dirty(bp);
		else
			dm_bufio_mark_partial_buffer_dirty(bp, buffer_offset, buffer_offset + len);

		atomic_inc(&bc->stats[S_BUFFERS_DIRTIED]);
	}

	_bio_put(bio);
	dm_bufio_release(bp);
}

/* Initialize per bufio buffer async_memcpy() context */
static struct async_submit_ctl *_init_async_memcpy(struct bio *bio,
						   struct buffered_c *bc, struct dm_buffer *bp,
						   unsigned int buffer_offset, unsigned int len)
{
	struct bio_c *bio_c = dm_per_bio_data(bio, sizeof(*bio_c));

	bio_c->bc = bc;
	bio_c->bp = bp;
	bio_c->buffer_offset = buffer_offset;
	bio_c->len = len;
	init_async_submit(&bio_c->submit, 0, NULL, _complete_memcpy, bio, NULL);
	return &bio_c->submit;
}

/* Return total number of blocks for @ti */
static sector_t _buffered_size(struct dm_target *ti)
{
	struct buffered_c *bc = ti->private;

	return _to_block(bc, ti->len - bc->start) + (_sector_mod(bc, ti->len) ? 1 : 0);
}

static void _memcpy(struct bio *bio, struct buffered_c *bc, struct dm_buffer *bp,
		    struct page *dst, struct page *src,
		    loff_t dst_offset, loff_t src_offset, size_t len,
		    struct async_submit_ctl *ctl)
{
	if (bc->async_memcpy) {
		async_memcpy(dst, src, dst_offset, src_offset, len, ctl);
	} else {
		void *d = kmap_local_page(dst);
		void *s = kmap_local_page(src);

		memcpy(d + dst_offset, s + src_offset, len);
		kunmap_local(d);
		kunmap_local(s);
		_complete_memcpy(bio);
	}
}

/*
 * Process @bvec of @bio optionally doing 2 bufio I/Os
 * in case the page of the bio_vec overlaps two buffers.
 */
static void _io(struct buffered_c *bc, struct bio *bio, struct bio_vec *bvec)
{
	bool write = (bio_op(bio) == REQ_OP_WRITE);
	unsigned int buffer_offset, bvec_offset = bvec->bv_offset;
	unsigned int len, total_len = bvec->bv_len;
	unsigned int block_size = dm_bufio_get_block_size(bc->bufio);
	sector_t block, sector = bio->bi_iter.bi_sector;
	void *buffer;
	struct dm_buffer *bp;
	struct page *buffer_page;
	struct bio_c *bio_c = dm_per_bio_data(bio, sizeof(*bio_c));

	while (total_len) {
		block = _to_block(bc, sector);
		buffer_offset = to_bytes(_sector_mod(bc, sector));
		len = min(block_size - buffer_offset, total_len);
		len = min((unsigned int)(PAGE_SIZE - (buffer_offset & ~PAGE_MASK)), len);
		/*
		 * Take an additional reference out for the 2nd buffer I/O
		 * in case the segment is split across 2 buffers.
		 */
		if (len < total_len) {
			_bio_get(bio);
			atomic_inc(&bc->stats[S_BUFFER_SPLITS]);
		}

		buffer = dm_bufio_get(bc->bufio, block, &bp);
		if (!buffer) {
			if (write && !buffer_offset && len == block_size) {
				buffer = dm_bufio_new(bc->bufio, block, &bp);
			} else {
				buffer = dm_bufio_read(bc->bufio, block, &bp);
				atomic_inc(&bc->stats[S_READS]);
			}
		} else {
			atomic_inc(&bc->stats[S_PREFETCHED_READS]);
		}

		if (IS_ERR(buffer)) {
			/* Memorize first error in bio status. */
			if (!bio->bi_status)
				bio->bi_status = errno_to_blk_status(PTR_ERR(buffer));

			_bio_put(bio);
			/* Continue with any split bio payload */

		} else if (write) {
			/*
			 * Take out 2 references to be savely handling any single aysnchronous
			 * write copy to avoid race between copying the data and setting the
			 * partial buffer dirty otherwise leading to premature buffer releases.
			 */
			/* (Superfluous) function consistency check example */
			WARN_ON_ONCE(block != dm_bufio_get_block_number(bp));
			/* Superfluous call to cover the API example */
			buffer = dm_bufio_get_block_data(bp);
			WARN_ON_ONCE(IS_ERR(buffer));
			buffer += buffer_offset;
			buffer_page = unlikely(is_vmalloc_addr(buffer)) ?
				vmalloc_to_page(buffer) : virt_to_page(buffer);
			_memcpy(bio, bc, bp, buffer_page, bvec->bv_page,
				offset_in_page(buffer), bvec_offset, len,
				_init_async_memcpy(bio, bc, bp, buffer_offset, len));
		} else {
			/* (Superfluous) function consistency check example */
			WARN_ON(block != dm_bufio_get_block_number(bp));
			buffer += buffer_offset;
			buffer_page = unlikely(is_vmalloc_addr(buffer)) ?
				vmalloc_to_page(buffer) : virt_to_page(buffer);
			_memcpy(bio, bc, bp, bvec->bv_page, buffer_page,
				bvec_offset, offset_in_page(buffer), len,
				_init_async_memcpy(bio, bc, bp, 0, 0));
		}

		/* Process any additional buffer even in case of I/O error */
		sector += to_sector(len);
		bvec_offset += len;
		total_len -= len;
	}
}

/* Check for and process any buffer flush requests. */
static blk_status_t __process_any_flush(struct buffered_c *bc, struct bio *bio)
{
	bool flush = false;
	blk_status_t r = BLK_STS_OK;

	if (bio_op(bio) == REQ_OP_WRITE) {
		if (bio->bi_opf & REQ_FUA) {
			atomic_inc(&bc->stats[S_FUA]);
			flush = true;
		} else if (bc->sync_writes) {
			atomic_inc(&bc->stats[S_SYNC_WRITES]);
			flush = true;
		}
	}

	if (flush) {
		r = _buffered_flush(bc);
		if (r && !bio->bi_status)
			bio->bi_status = r;
	}

	return r;
}

/*
 * Issue discards to a block range defined by @bio.
 *
 * Avoid issueing to partial blocks on the edges.
 *
 *
 * Definition of block range [start..end] of 4K block size examples:
 *
 * 12 -> start=0, end=0, discard=false
 * 45 -> start=0 end=0 -> start++(1), discard=false
 * 78 -> start=0 end=0 -> start++(1), discard=false
 * 4567 89 -> start=0 end=1 -> start++(1), end--(0), discard=false
 * 1234567 -> start=0 end=0 -> discard=true
 * 1234567 89ABCDEF -> start=0 end=1 -> discard=true
 */
static void _discard_blocks(struct buffered_c *bc, struct bio *bio)
{
	sector_t start = _to_block(bc, bio->bi_iter.bi_sector);
	sector_t end = _to_block(bc, bio_end_sector(bio));
	sector_t n_blocks;

	if (_sector_mod(bc, bio->bi_iter.bi_sector))
		start++;

	if (unlikely(start >= end))
		return;

	n_blocks = end - start;

	/* ADDRESSME: dirty buffers won't be forgotten! */
	dm_bufio_forget_buffers(bc->bufio, start, n_blocks);
	atomic_inc(&bc->stats[S_DISCARDS]);

	if (bc->discard_passdown) {
		dm_bufio_issue_discard(bc->bufio, start, n_blocks);
		atomic_inc(&bc->stats[S_DISCARDS_PASSDOWN]);
	}
}

/* Process REQ_OP_WRITE_ZEROES on @bio */
/* FIXME: very slow, hence ti->num_write_zeroes_bios = 0 in the constructor. */
static void _write_zeroes(struct buffered_c *bc, struct bio *bio)
{
	loff_t b_offset, e_offset;
	sector_t block, mod;
	sector_t sector = bio->bi_iter.bi_sector;
	sector_t end_sector = bio_end_sector(bio);
	sector_t sectors_per_block = to_sector(bc->buffer_size);
	void *buffer;
	struct dm_buffer *bp;

	while (sector < end_sector) {
		block = _to_block(bc, sector);
		mod = _sector_mod(bc, sector);

		buffer = dm_bufio_get(bc->bufio, block, &bp);
		if (!buffer)
			buffer = (mod ? dm_bufio_read : dm_bufio_new)(bc->bufio, block, &bp);

		if (IS_ERR(buffer)) {
			bio->bi_status = errno_to_blk_status(PTR_ERR(buffer));
			break;
		}

		b_offset = to_bytes(mod);
		if (end_sector - sector < sectors_per_block)
			e_offset = b_offset + to_bytes(end_sector - sector);
		else
			e_offset = bc->buffer_size;

		memset(buffer + b_offset, 0, e_offset - b_offset);
		dm_bufio_mark_partial_buffer_dirty(bp, b_offset, e_offset);
		dm_bufio_release(bp);
		sector += sectors_per_block - mod;
	}
}

/* Process I/O on a single @bio */
static void __process_bio(struct buffered_c *bc, struct bio *bio)
{
	struct bio_vec bvec;
	blk_status_t r, rio;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		/*
		 * Take references per segment out and add an
		 * additional one to prevent an endio race.
		 */
		_bio_set_references(bio, bio_segments(bio) + 1);

		if (bio->bi_opf & REQ_PREFLUSH) {
			atomic_inc(&bc->stats[S_PREFLUSHS]);
			r = _buffered_flush(bc);
			if (unlikely(r)) {
				bio->bi_status = r;
				goto err;
			}
		}

		bio_for_each_segment(bvec, bio, bio->bi_iter) {
			_io(bc, bio, &bvec);
			cond_resched();
		}

		/* Try processing any REQ_FUA, ... even in case there's a previous I/O error.*/
		rio = bio->bi_status;
		r = __process_any_flush(bc, bio);
		if (unlikely(r || rio))
			goto err;
		break;
	case REQ_OP_DISCARD:
		/*
		 * Try forgetting buffers on discard (least we can do
		 * with lag of discard passdown in dm-bufio).
		 */
		_bio_set_references(bio, 1);
		if (bc->discard)
			_discard_blocks(bc, bio);
		break;
	case REQ_OP_WRITE_ZEROES:
		_bio_set_references(bio, 1);
		_write_zeroes(bc, bio);
		break;
	default:
		_bio_set_references(bio, 1);
		/* Return error for unsupported operation */
		bio->bi_status = errno_to_blk_status(-EOPNOTSUPP);
	}

	goto out;
err:
	/* On error, reset refecount to 1 for _bio_put() to endio */
	_bio_set_references(bio, 1);
out:
	_bio_put(bio); /* Release (additional) reference */
}

/* Process I/O on a bio prefetching buffers on a single @bio in a worker. */
static void _process_bio(struct work_struct *work)
{
	struct buffered_c *bc = container_of(work, struct buffered_c, buffered_ws);
	struct bio *bio;
	bool queue, write;
	sector_t blocks, sectors, start, end;

	spin_lock(&bc->lock);
	bio = bio_list_pop(&bc->bios);
	spin_unlock(&bc->lock);

	if (!bio)
		return;

	write = false;
	start = _to_block(bc, bio->bi_iter.bi_sector);

	/* Prefetch read and partial write buffers */
	switch (bio_op(bio)) {
	case REQ_OP_READ:
		sectors = bio_end_sector(bio) - _to_sector(bc, start);
		blocks = _to_block(bc, sectors) + _sector_mod(bc, sectors) ? 1 : 0;
		dm_bufio_prefetch(bc->bufio, start, blocks);
		break;
	case REQ_OP_WRITE:
		write = true;
		/* Beware of partial block updates. */
		if (_sector_mod(bc, bio->bi_iter.bi_sector)) {
			dm_bufio_prefetch(bc->bufio, start, 1);
			atomic_inc(&bc->stats[S_PREFETCHED_WRITES]);
		}
		if (_sector_mod(bc, bio_end_sector(bio))) {
			end = _to_block(bc, bio_end_sector(bio));
			if (start != end) {
				dm_bufio_prefetch(bc->bufio, start, 1);
				atomic_inc(&bc->stats[S_PREFETCHED_WRITES]);
			}
		}
		break;
	case REQ_OP_WRITE_ZEROES:
		write = true;
		break;
	default:
		break;
	}

	__process_bio(bc, bio);

	/* Use spinlock here also as in map function to avoid any memory access reordering issues. */
	spin_lock(&bc->lock);
	queue = !bio_list_empty(&bc->bios);
	spin_unlock(&bc->lock);

	if (queue)
		queue_work(bc->buffered_wq, &bc->buffered_ws);

	/* Reschedule the flush thread in case of new write(s). */
	if (write)
		schedule_delayed_work(&bc->buffered_flush_ws, 2 * HZ);

	cond_resched();
}

static void _process_flushs(struct work_struct *work)
{
	struct buffered_c *bc = container_of(to_delayed_work(work),
					     struct buffered_c, buffered_flush_ws);
	_buffered_flush(bc);
}

/* Process optional arguments setting bool values ("no_" prefix indicates don't set) */
static int _process_arg(struct buffered_c *bc, const char *arg)
{
	struct {
		const char *arg;
		bool *value;
	} args[] = {
		{ "async_memcpy",     &bc->async_memcpy },
		{ "discard",	      &bc->discard },
		{ "discard_passdown", &bc->discard_passdown },
		{ "sync_writes",      &bc->sync_writes },
		{ "write_zeroes",     &bc->write_zeroes },
	}, *p = args, *end = args + ARRAY_SIZE(args);
	bool set;
	const char no_str[] = "no_";
	ssize_t no_str_len = sizeof(no_str) - 1;

	if (!arg)
		return 0;

	for (; p < end; p++) {
		set = !!strncasecmp(arg, no_str, no_str_len);
		if (!strcasecmp(set ? arg : arg + no_str_len, p->arg)) {
			*p->value = set;
			return 0;
		}
	}

	return -EINVAL;
}

static void _define_optional_args(struct buffered_c *bc)
{
	bc->async_memcpy = true;
	bc->sync_writes = false;
	bc->buffer_size = DEFAULT_BUFFERED_BLOCK_SIZE;
	bc->discard = true;
	bc->discard_passdown = true;
	bc->write_zeroes = false;
}

/* Allocate memory and store the ctr table line for status output. */
static int _store_table_line(struct buffered_c *bc, unsigned int argc, char **argv)
{
	ssize_t sz = 0;
	unsigned int i;

	for (i = 0; i < argc; i++)
		sz += strlen(argv[i]) + 1;

	bc->table_line = kzalloc(sz, GFP_KERNEL);
	if (!bc->table_line)
		return -ENOMEM;

	for (i = 0; i < argc; i++) {
		strcat(bc->table_line, argv[i]);
		if (i < argc - 1)
			strcat(bc->table_line, " ");
	}

	return 0;
}

static int _process_block_size_arg(struct buffered_c *bc, const char *arg)
{
	if (!arg)
		return 0;

	if (!strcasecmp(arg, "-")) {
		bc->buffer_size = DEFAULT_BUFFERED_BLOCK_SIZE;
		return 0;
	}

	return kstrtouint(arg, 10, &bc->buffer_size) ||
		!is_power_of_2(bc->buffer_size) ||
		bc->buffer_size < SECTOR_SIZE ? -EINVAL : 0;
}

static void buffered_dtr(struct dm_target *ti)
{
	struct buffered_c *bc = ti->private;

	if (bc->buffered_wq)
		destroy_workqueue(bc->buffered_wq);
	if (bc->buffered_flush_wq)
		destroy_workqueue(bc->buffered_flush_wq);
	if (bc->bufio && !IS_ERR(bc->bufio))
		dm_bufio_client_destroy(bc->bufio);
	if (bc->dev)
		dm_put_device(ti, bc->dev);
	if (bc->table_line)
		kfree(bc->table_line);
	kfree(bc);
}

/*
 * Mapping parameters:
 *
 *    First 3 must be in that order!
 *
 *    <dev path>     : full pathname of the buffered device
 *    <offset>       : offset in sectors into the device
 *    [<block size>] : optional size of bufio buffers in bytes
 *    [<no_async_memcpy|async_memcpy>]
 *		     : select to (not) perform asynchronous memory copies
 *		       between bvec pages and buffers
 *    [<no_discard|discard>]
 *		     : select to (not) perform discards
 *    [<no_discard_passdown|discard_passdown>]
 *		     : select to (not) perform passing down discards to the buffered device
 *    [<no_sync_writes|sync_writes>]
 *		     : select to (not) perform synchronous writes of dirty buffers
 *    [<no_write_zeroes|write_zeroes>]
 *		     : select to (not) perform write zeroes
 *
 */
static int buffered_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct buffered_c *bc;
	int i, r;
	struct dm_arg_set as = { .argc = argc, .argv = argv };
	const char *arg, *dev_path = dm_shift_arg(&as);

	if (argc < 2 || argc > 8) {
		ti->error = "Requires 2 - 8 arguments";
		return -EINVAL;
	}

	bc = kzalloc(sizeof(*bc), GFP_KERNEL);
	if (!bc) {
		ti->error = "Cannot allocate context";
		return -ENOMEM;
	}

	ti->private = bc;

	/* Store constructor table line for status output. */
	r = _store_table_line(bc, argc, argv);
	if (r) {
		ti->error = "Cannot allocate table line memory";
		goto bad;
	}

	/* Processs sector offset. */
	arg = dm_shift_arg(&as);
	r = kstrtoull(arg, 10, (u64 *)&bc->start) ? -EINVAL : 0;
	if (r) {
		ti->error = "Invalid sector offset";
		goto bad;
	}

	_define_optional_args(bc);

	arg = dm_shift_arg(&as);
	r = _process_block_size_arg(bc, arg);
	if (r) {
		ti->error = "Invalid block size";
		goto bad;
	}

	/* Process any other arguments. */
	do {
		arg = dm_shift_arg(&as);
		r = _process_arg(bc, arg);
		if (r) {
			ti->error = "Invalid argument";
			goto bad;
		}
	} while (arg);

	r = dm_get_device(ti, dev_path, dm_table_get_mode(ti->table), &bc->dev);
	if (r) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	bc->buffered_wq = create_workqueue("dm-" DM_MSG_PREFIX "-io");
	if (!bc->buffered_wq) {
		ti->error = "Couldn't start dm-" DM_MSG_PREFIX "-io";
		r = -ENOMEM;
		goto bad;
	}

	bc->buffered_flush_wq = create_singlethread_workqueue("dm-" DM_MSG_PREFIX "-flush");
	if (!bc->buffered_flush_wq) {
		ti->error = "Couldn't start dm-" DM_MSG_PREFIX "-flush";
		r = -ENOMEM;
		goto bad;
	}

	bc->bufio = dm_bufio_client_create(bc->dev->bdev, bc->buffer_size, 1, 0, NULL, NULL, 0);
	if (IS_ERR(bc->bufio)) {
		ti->error = "Couldn't create bufio client";
		r = PTR_ERR(bc->bufio);
		goto bad;
	}

	/* Check plausible discard settings. */
	if (!bc->discard && bc->discard_passdown) {
		ti->error = "Discard passdown without discard enabled";
		r = -EINVAL;
		goto bad;
	}

	dm_bufio_set_sector_offset(bc->bufio, bc->start);
	spin_lock_init(&bc->lock);
	bio_list_init(&bc->bios);
	INIT_WORK(&bc->buffered_ws, _process_bio);
	INIT_DELAYED_WORK(&bc->buffered_flush_ws, _process_flushs);
	bc->block_shift = __ffs(bc->buffer_size) - SECTOR_SHIFT;
	bc->block_mask = ~(~((sector_t)0) << bc->block_shift);

	/* REMOVEME: development statistics */
	for (i = 0; i < ARRAY_SIZE(bc->stats); i++)
		atomic_set(&bc->stats[i], 0);

	/* Define target settings for flush, discard, ... */
	ti->flush_supported = true;
	ti->num_flush_bios = 1;

	if (bc->discard) {
		ti->discards_supported = true;
		ti->num_discard_bios = 1;
	}

	if (bc->write_zeroes)
		ti->num_write_zeroes_bios = 1;

	ti->per_io_data_size = sizeof(struct bio_c);
	return 0;
bad:
	buffered_dtr(ti);
	return r;
}

static int buffered_map(struct dm_target *ti, struct bio *bio)
{
	struct buffered_c *bc = ti->private;
	bool queue;

	bio->bi_iter.bi_sector = dm_target_offset(ti, bio->bi_iter.bi_sector);

	spin_lock(&bc->lock);
	queue = bio_list_empty(&bc->bios);
	bio_list_add(&bc->bios, bio);
	spin_unlock(&bc->lock);

	if (queue)
		queue_work(bc->buffered_wq, &bc->buffered_ws);

	return DM_MAPIO_SUBMITTED;
}

static void buffered_postsuspend(struct dm_target *ti)
{
	struct buffered_c *bc = ti->private;

	flush_workqueue(bc->buffered_wq);
	cancel_delayed_work_sync(&bc->buffered_flush_ws);
	flush_workqueue(bc->buffered_flush_wq);
	dm_bufio_write_dirty_buffers(bc->bufio);
}

/*
 * Count and/or forget buffers.
 *
 * Mind count is subject to forget/release/move races.
 */
/* FIXME: count dirty buffers */
static int __process_buffers(struct dm_target *ti, sector_t *n_buffers, bool forget)
{
	struct buffered_c *bc = ti->private;
	sector_t block, end = _buffered_size(ti);
	struct dm_buffer *bp;
	void *buffer;

	if (!n_buffers && !forget)
		return -EINVAL;

	for (block = 0; block < end; block++) {
		buffer = dm_bufio_get(bc->bufio, block, &bp);
		if (buffer) {
			dm_bufio_release(bp);
			if (forget)
				dm_bufio_forget(bc->bufio, block);
			if (n_buffers)
				(*n_buffers)++;
		}
	}

	return 0;
}

static void buffered_status(struct dm_target *ti, status_type_t type,
			    unsigned int status_flags, char *result, unsigned int maxlen)
{
	struct buffered_c *bc = ti->private;
	int i, sz = 0;

	switch (type) {
	case STATUSTYPE_TABLE:
		DMEMIT("%s", bc->table_line);
		break;
	case STATUSTYPE_INFO:
#define B(s) s ? "true" : "false"
		DMEMIT("device_size=%llu buffer_size=%u async_memcpy=%s sync_writes=%s discard=%s discard_passdown=%s write_zeroes=%s ",
		       (unsigned long long)dm_bufio_get_device_size(bc->bufio), bc->buffer_size,
		       B(bc->async_memcpy), B(bc->sync_writes), B(bc->discard), B(bc->discard_passdown), B(bc->write_zeroes));
#undef B
		for (i = 0; i < ARRAY_SIZE(bc->stats); i++) {
			DMEMIT("%u", atomic_read(bc->stats + i));
			if (i < ARRAY_SIZE(bc->stats) - 1)
				DMEMIT("/");
		}
		break;
	case STATUSTYPE_IMA:
		*result = '\0';
	}
}

static int buffered_prepare_ioctl(struct dm_target *ti, struct block_device **bdev)
{
	struct buffered_c *bc = ti->private;

	*bdev = bc->dev->bdev;

	/* Only pass down ioctls on precise size matches */
	return !!(bc->start || ti->len != bdev_nr_sectors(bc->dev->bdev));
}

static int buffered_iterate_devices(struct dm_target *ti,
				    iterate_devices_callout_fn fn, void *data)
{
	struct buffered_c *bc = ti->private;

	return fn(ti, bc->dev, bc->start, ti->len, data);
}

static int buffered_message(struct dm_target *ti, unsigned int argc, char **argv,
			    char *result, unsigned int maxlen)
{
	struct buffered_c *bc = ti->private;
	sector_t n;

	if (!strcasecmp(argv[0], "async_flush")) {
		if (argc != 1)
			return -EINVAL;
		dm_bufio_write_dirty_buffers_async(bc->bufio);
		return 0;
	}

	if (!strcasecmp(argv[0], "buffers")) {
		if (argc != 2)
			return -EINVAL;
		if (kstrtoull(argv[1], 10, (u64 *)&n) || !n ||
		    n >= _buffered_size(ti) || n > UINT_MAX)
			return -EINVAL;
		dm_bufio_set_minimum_buffers(bc->bufio, (unsigned int)n);
		return 0;
	}

	return (argc == 1) ? __process_buffers(ti, NULL, !strcasecmp(argv[0], "forget")) : -EINVAL;
}

static void buffered_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct buffered_c *bc = ti->private;

	limits->logical_block_size = to_bytes(1);
	blk_limits_io_min(limits, limits->logical_block_size);
	blk_limits_io_opt(limits, bc->buffer_size);
	if (ti->num_write_zeroes_bios)
		limits->max_write_zeroes_sectors = UINT_MAX;
}

static struct target_type buffered_target = {
	.name		 = "buffered",
	.version	 = {1, 0, 0},
	.module		 = THIS_MODULE,
	.ctr		 = buffered_ctr,
	.dtr		 = buffered_dtr,
	.map		 = buffered_map,
	.status		 = buffered_status,
	.postsuspend	 = buffered_postsuspend,
	.iterate_devices = buffered_iterate_devices,
	.prepare_ioctl	 = buffered_prepare_ioctl,
	.message	 = buffered_message,
	.io_hints	 = buffered_io_hints
};

static int __init dm_buffered_init(void)
{
	return dm_register_target(&buffered_target);
}

static void __exit dm_buffered_exit(void)
{
	dm_unregister_target(&buffered_target);
}

/* Module hooks */
module_init(dm_buffered_init);
module_exit(dm_buffered_exit);

MODULE_DESCRIPTION(DM_NAME " buffered test target");
MODULE_AUTHOR("Heinz Mauelshagen <dm-devel@redhat.com>");
MODULE_LICENSE("GPL");
