#include <linux/device-mapper.h>

#include <linux/module.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/dm-io.h>
#include <linux/dm-kcopyd.h>

#define DM_MSG_PREFIX	"writecache"

/*
 * Persistent memory is not covered with page structures. Thus, when we need to
 * write data that is in the persistent memory, we need to copy them to a
 * temporary location that has struct page. When the macro
 * COPY_TO_PAGES_BEFORE_WRITING is defined, such copying is performed.
 *
 * If the persistent memory were covered with page structures, this macro can be
 * undefined.
 */
#define COPY_TO_PAGES_BEFORE_WRITING


#define BITMAP_GRANULARITY	65536
#if BITMAP_GRANULARITY < PAGE_SIZE
#undef BITMAP_GRANULARITY
#define BITMAP_GRANULARITY	PAGE_SIZE
#endif

struct wc_pmem_holder {
	struct dm_dev *dev;
};

static int persistent_memory_claim(struct dm_target *ti, const char *name,
				   struct wc_pmem_holder *pmem_holder, void **addr, uint64_t *size)
{
	int r;
	loff_t s;
	long da;
	unsigned long pfn;

	r = dm_get_device(ti, name, FMODE_READ | FMODE_WRITE, &pmem_holder->dev);
	if (unlikely(r))
		return r;
	s = i_size_read(pmem_holder->dev->bdev->bd_inode);
	da = bdev_direct_access(pmem_holder->dev->bdev, 0, addr, &pfn, s);
	if (da < 0) {
		dm_put_device(ti, pmem_holder->dev);
		return da;
	}
	if (da != s) {
		dm_put_device(ti, pmem_holder->dev);
		return -EINVAL;
	}
	*size = da;
	return 0;
}

static void persistent_memory_release(struct dm_target *ti, struct wc_pmem_holder *pmem_holder,
				      void *addr, size_t size)
{
	dm_put_device(ti, pmem_holder->dev);
}

static void persistent_memory_flush_all(void)
{
#ifdef CONFIG_X86
	wbinvd();
#endif
}

static void persistent_memory_flush(void *ptr, size_t size)
{
#ifdef CONFIG_X86
	while (1) {
		clwb(ptr);
		if (size <= 64)
			break;
		size -= 64;
		ptr = (char *)ptr + 64;
	}
#endif
}

static void persistent_memory_commit_flushed(void)
{
#ifdef CONFIG_X86
	wmb();
	pcommit_sfence();
#endif
}

#define MEMORY_SUPERBLOCK_MAGIC	0x23489321

struct wc_memory_entry {
	uint64_t original_sector;
	uint64_t seq_count;
};

struct wc_memory_superblock {
	union {
		struct {
			uint32_t magic;
			uint32_t block_size;
			uint64_t n_blocks;
			uint64_t seq_count;
		};
		uint8_t padding[32];
	};
	struct wc_memory_entry entries[0];
};

struct wc_entry {
	struct rb_node rb_node;
	struct list_head lru;
	unsigned short wc_list_contiguous;
	bool write_in_progress:1;
	unsigned long index
#if BITS_PER_LONG == 64
		:47
#endif
	;
};

struct dm_writecache {
	bool pmem_mode;
	struct mutex lock;
	struct rb_root tree;
	struct list_head lru;
	struct list_head writeback_start;
	struct list_head writeback;
	struct list_head freelist;
	size_t freelist_size;
	size_t writeback_size;
	size_t freelist_high_watermark;
	size_t freelist_low_watermark;
	wait_queue_head_t freelist_wait;

	struct dm_target *ti;
	struct dm_dev *dev;
	struct dm_dev *ssd_dev;
	void *memory_map;
	uint64_t memory_map_size;
	sector_t metadata_sectors;
	void *block_start;
	struct wc_entry *entries;
	unsigned block_size;
	unsigned char block_size_bits;
	int error;
	bool high_wm_percent_set;
	bool low_wm_percent_set;

	bool overwrote_committed;

	atomic_t bio_in_progress[2];
	wait_queue_head_t bio_in_progress_wait[2];

	struct dm_io_client *dm_io;

	unsigned writeback_all;
	struct workqueue_struct *writeback_wq;
	struct work_struct writeback_work;

	wait_queue_head_t endio_thread_wait;
	struct list_head endio_list;
	struct task_struct *endio_thread;
	bool endio_thread_terminate;

	struct bio_set *bio_set;
	mempool_t *copy_pool;
#ifdef COPY_TO_PAGES_BEFORE_WRITING
	mempool_t *page_pool;
#endif
	struct wc_pmem_holder pmem_holder;
	const char *memory_name;

	struct dm_kcopyd_client *dm_kcopyd;
	unsigned long *dirty_bitmap;
	unsigned dirty_bitmap_size;
};

#define WB_LIST_INLINE		16

struct writeback_struct {
	struct list_head endio_entry;
	struct dm_writecache *wc;
	struct wc_entry **wc_list;
	unsigned wc_list_n;
#ifdef COPY_TO_PAGES_BEFORE_WRITING
	unsigned page_offset;
	struct page *page;
#endif
	struct wc_entry *wc_list_inline[WB_LIST_INLINE];
	struct bio bio;
};

struct copy_struct {
	struct list_head endio_entry;
	struct dm_writecache *wc;
	struct wc_entry *e;
	int error;
};

DECLARE_DM_KCOPYD_THROTTLE_WITH_MODULE_PARM(dm_writecache_throttle,
					    "A percentage of time allocated for data copying");

static struct wc_memory_superblock *sb(struct dm_writecache *wc)
{
	return wc->memory_map;
}

static struct wc_memory_entry *memory_entry(struct dm_writecache *wc, struct wc_entry *e)
{
	return &sb(wc)->entries[e->index];
}

static void *memory_data(struct dm_writecache *wc, struct wc_entry *e)
{
	return (char *)wc->block_start + (e->index << wc->block_size_bits);
}

static sector_t cache_sector(struct dm_writecache *wc, struct wc_entry *e)
{
	return wc->metadata_sectors +
		((sector_t)e->index << (wc->block_size_bits - SECTOR_SHIFT));
}

#define writecache_error(wc, msg, arg...)	\
	(ACCESS_ONCE((wc)->error) = 1, wake_up(&wc->freelist_wait), DMERR(msg, ##arg))

static void writecache_flush_all(struct dm_writecache *wc)
{
	if (wc->pmem_mode)
		persistent_memory_flush_all();
	else
		memset(wc->dirty_bitmap, -1, wc->dirty_bitmap_size);
}

static void writecache_flush_region(struct dm_writecache *wc, void *ptr, size_t size)
{
	if (wc->pmem_mode)
		persistent_memory_flush(ptr, size);
	else
		__set_bit(((char *)ptr - (char *)wc->memory_map) / BITMAP_GRANULARITY,
			  wc->dirty_bitmap);
}

static void writecache_disk_flush(struct dm_writecache *wc, struct dm_dev *dev);

struct io_notify {
	struct dm_writecache *wc;
	struct completion c;
	atomic_t count;
};

void writecache_notify_io(unsigned long error, void *context)
{
	struct io_notify *endio = context;

	if (unlikely(error != 0))
		writecache_error(endio->wc, "error writing metadata");
	BUG_ON(atomic_read(&endio->count) <= 0);
	if (atomic_dec_and_test(&endio->count))
		complete(&endio->c);
}

static void ssd_commit_flushed(struct dm_writecache *wc)
{
	int r;
	struct dm_io_region region;
	struct dm_io_request req;
	struct io_notify endio = {
		wc,
		COMPLETION_INITIALIZER_ONSTACK(endio.c),
		ATOMIC_INIT(1),
	};
	unsigned bitmap_bits = wc->dirty_bitmap_size * BITS_PER_LONG;
	unsigned i = 0;

	while (1) {
		unsigned j;
		i = find_next_bit(wc->dirty_bitmap, bitmap_bits, i);
		if (unlikely(i == bitmap_bits))
			break;
		j = find_next_zero_bit(wc->dirty_bitmap, bitmap_bits, i);

		region.bdev = wc->ssd_dev->bdev;
		region.sector = (sector_t)i * (BITMAP_GRANULARITY >> SECTOR_SHIFT);
		region.count = (sector_t)(j - i) * (BITMAP_GRANULARITY >> SECTOR_SHIFT);

		if (unlikely(region.sector >= wc->metadata_sectors))
			break;
		if (unlikely(region.sector + region.count > wc->metadata_sectors))
			region.count = wc->metadata_sectors - region.sector;

		atomic_inc(&endio.count);
		req.bi_rw = WRITE;
		req.mem.type = DM_IO_VMA;
		req.mem.ptr.vma = (char *)wc->memory_map + (size_t)i * BITMAP_GRANULARITY;
		req.client = wc->dm_io;
		req.notify.fn = writecache_notify_io;
		req.notify.context = &endio;

		r = dm_io(&req, 1, &region, NULL);
		if (unlikely(r))
			/* FIXME: need more graceful failure! */
			panic(DM_NAME ": " DM_MSG_PREFIX ": dm io error %d", r);
		i = j;
	}

	writecache_notify_io(0, &endio);
	wait_for_completion_io(&endio.c);

	writecache_disk_flush(wc, wc->ssd_dev);

	memset(wc->dirty_bitmap, 0, wc->dirty_bitmap_size);
}

static void writecache_commit_flushed(struct dm_writecache *wc)
{
	if (wc->pmem_mode)
		persistent_memory_commit_flushed();
	else
		ssd_commit_flushed(wc);
}

static void writecache_disk_flush(struct dm_writecache *wc, struct dm_dev *dev)
{
	int r;
	struct dm_io_region region;
	struct dm_io_request req;

	region.bdev = dev->bdev;
	region.sector = 0;
	region.count = 0;
	req.bi_rw = WRITE_FLUSH;
	req.mem.type = DM_IO_KMEM;
	req.mem.ptr.addr = NULL;
	req.client = wc->dm_io;
	req.notify.fn = NULL;

	r = dm_io(&req, 1, &region, NULL);
	if (unlikely(r))
		writecache_error(wc, "error flushing metadata: %d", r);
}

static void writecache_wait_for_ios(struct dm_writecache *wc, int direction)
{
	wait_event(wc->bio_in_progress_wait[direction],
		   !atomic_read(&wc->bio_in_progress[direction]));
}

#define WFE_RETURN_FOLLOWING	1
#define WFE_LOWEST_SEQ		2

static struct wc_entry *writecache_find_entry(struct dm_writecache *wc, uint64_t block, int flags)
{
	struct wc_entry *e;
	struct rb_node *node = wc->tree.rb_node;

	if (unlikely(!node))
		return NULL;

	while (1) {
		e = container_of(node, struct wc_entry, rb_node);
		if (memory_entry(wc, e)->original_sector == block)
			break;
		node = (memory_entry(wc, e)->original_sector >= block ?
			e->rb_node.rb_left : e->rb_node.rb_right);
		if (unlikely(!node)) {
			if (!(flags & WFE_RETURN_FOLLOWING))
				return NULL;
			if (memory_entry(wc, e)->original_sector >= block) {
				break;
			} else {
				node = rb_next(&e->rb_node);
				if (unlikely(!node))
					return NULL;
				e = container_of(node, struct wc_entry, rb_node);
				break;
			}
		}
	}

	while (1) {
		struct wc_entry *e2;
		if (flags & WFE_LOWEST_SEQ)
			node = rb_prev(&e->rb_node);
		else
			node = rb_next(&e->rb_node);
		if (!node)
			return e;
		e2 = container_of(node, struct wc_entry, rb_node);
		if (memory_entry(wc, e2)->original_sector != block)
			return e;
		e = e2;
	}
}

static void writecache_insert_entry(struct dm_writecache *wc, struct wc_entry *ins)
{
	struct wc_entry *e;
	struct rb_node **node = &wc->tree.rb_node, *parent = NULL;

	while (*node) {
		e = container_of(*node, struct wc_entry, rb_node);
		parent = &e->rb_node;
		node = (memory_entry(wc, e)->original_sector > memory_entry(wc, ins)->original_sector ?
			&parent->rb_left : &parent->rb_right);
	}
	rb_link_node(&ins->rb_node, parent, node);
	rb_insert_color(&ins->rb_node, &wc->tree);
	list_add(&ins->lru, &wc->lru);
}

static void writecache_unlink(struct dm_writecache *wc, struct wc_entry *e)
{
	list_del(&e->lru);
	rb_erase(&e->rb_node, &wc->tree);
}

static void writecache_add_to_freelist(struct dm_writecache *wc, struct wc_entry *e)
{
	list_add_tail(&e->lru, &wc->freelist);
	wc->freelist_size++;
}

struct wc_entry *writecache_pop_from_freelist(struct dm_writecache *wc)
{
	struct wc_entry *e;

	if (unlikely(list_empty(&wc->freelist)))
		return NULL;
	e = container_of(wc->freelist.next, struct wc_entry, lru);
	list_del(&e->lru);
	wc->freelist_size--;
	if (unlikely(wc->freelist_size <= wc->freelist_high_watermark))
		queue_work(wc->writeback_wq, &wc->writeback_work);

	return e;
}

static void writecache_free_entry(struct dm_writecache *wc, struct wc_entry *e)
{
	writecache_unlink(wc, e);
	writecache_add_to_freelist(wc, e);
	ACCESS_ONCE(memory_entry(wc, e)->seq_count) = -1;
	writecache_flush_region(wc, memory_entry(wc, e), sizeof(struct wc_memory_entry));
	if (unlikely(waitqueue_active(&wc->freelist_wait)))
		wake_up(&wc->freelist_wait);
}

static void writecache_wait_on_freelist(struct dm_writecache *wc)
{
	DECLARE_WAITQUEUE(wait, current);

	add_wait_queue(&wc->freelist_wait, &wait);
	set_current_state(TASK_UNINTERRUPTIBLE);
	mutex_unlock(&wc->lock);
	io_schedule();
	mutex_lock(&wc->lock);
	remove_wait_queue(&wc->freelist_wait, &wait);
}

static void writecache_poison_lists(struct dm_writecache *wc)
{
	/*
	 * Catch incorrect access to these values while the device is suspended.
	 */
	memset(&wc->tree, -1, sizeof wc->tree);
	wc->lru.next = LIST_POISON1;
	wc->lru.prev = LIST_POISON2;
	wc->writeback_start.next = LIST_POISON1;
	wc->writeback_start.prev = LIST_POISON2;
	wc->writeback.next = LIST_POISON1;
	wc->writeback.prev = LIST_POISON2;
	wc->freelist.next = LIST_POISON1;
	wc->freelist.prev = LIST_POISON2;
}

static void writecache_flush_entry(struct dm_writecache *wc, struct wc_entry *e)
{
	writecache_flush_region(wc, memory_entry(wc, e), sizeof(struct wc_memory_entry));
	if (wc->pmem_mode)
		writecache_flush_region(wc, memory_data(wc, e), wc->block_size);
}

static bool writecache_entry_is_committed(struct dm_writecache *wc, struct wc_entry *e)
{
	return memory_entry(wc, e)->seq_count < sb(wc)->seq_count;
}

static void writecache_flush(struct dm_writecache *wc)
{
	struct wc_entry *e, *e2;

	if (list_empty(&wc->lru))
		return;

	e = container_of(wc->lru.next, struct wc_entry, lru);
	if (writecache_entry_is_committed(wc, e)) {
		if (wc->overwrote_committed) {
			writecache_wait_for_ios(wc, WRITE);
			writecache_disk_flush(wc, wc->ssd_dev);
			wc->overwrote_committed = false;
		}
		return;
	}
	while (1) {
		writecache_flush_entry(wc, e);
		if (unlikely(e->lru.next == &wc->lru))
			break;
		e2 = container_of(e->lru.next, struct wc_entry, lru);
		if (writecache_entry_is_committed(wc, e2))
			break;
		e = e2;
		cond_resched();
	}
	writecache_commit_flushed(wc);

	writecache_wait_for_ios(wc, WRITE);

	ACCESS_ONCE(sb(wc)->seq_count) = sb(wc)->seq_count + 1;
	writecache_flush_region(wc, &sb(wc)->seq_count, sizeof sb(wc)->seq_count);
	writecache_commit_flushed(wc);

	wc->overwrote_committed = false;

	while (1) {
		/* Free another committed entry with lower seq-count */
		struct rb_node *rb_node = rb_prev(&e->rb_node);

		if (rb_node) {
			e2 = container_of(rb_node, struct wc_entry, rb_node);
			if (memory_entry(wc, e2)->original_sector == memory_entry(wc, e)->original_sector &&
			    likely(!e2->write_in_progress))
				writecache_free_entry(wc, e2);
		}
		if (unlikely(e->lru.prev == &wc->lru))
			break;
		e = container_of(e->lru.prev, struct wc_entry, lru);
		cond_resched();
	}
}

static void writecache_discard(struct dm_writecache *wc, sector_t start, sector_t end)
{
	struct wc_entry *e;
	bool discarded_something = false;

	e = writecache_find_entry(wc, start, WFE_RETURN_FOLLOWING | WFE_LOWEST_SEQ);
	if (unlikely(!e))
		return;

	while (memory_entry(wc, e)->original_sector < end) {
		struct rb_node *node = rb_next(&e->rb_node);

		if (likely(!e->write_in_progress)) {
			if (!discarded_something) {
				writecache_wait_for_ios(wc, READ);
				writecache_wait_for_ios(wc, WRITE);
				discarded_something = true;
			}
			writecache_free_entry(wc, e);
		}

		if (!node)
			break;

		e = container_of(node, struct wc_entry, rb_node);
	}

	if (discarded_something)
		writecache_commit_flushed(wc);
}

static bool writecache_wait_for_writeback(struct dm_writecache *wc)
{
	if (!list_empty(&wc->writeback_start) || !list_empty(&wc->writeback)) {
		writecache_wait_on_freelist(wc);
		return true;
	}
	return false;
}

static void writecache_suspend(struct dm_target *ti)
{
	struct dm_writecache *wc = ti->private;

	flush_workqueue(wc->writeback_wq);

	mutex_lock(&wc->lock);
	writecache_flush(wc);
	while (writecache_wait_for_writeback(wc)) ;
	mutex_unlock(&wc->lock);

	writecache_poison_lists(wc);
}

static void writecache_resume(struct dm_target *ti)
{
	struct dm_writecache *wc = ti->private;
	uint64_t b;
	bool need_flush = false;

	mutex_lock(&wc->lock);

	wc->tree = RB_ROOT;
	INIT_LIST_HEAD(&wc->lru);
	INIT_LIST_HEAD(&wc->writeback_start);
	INIT_LIST_HEAD(&wc->writeback);
	INIT_LIST_HEAD(&wc->freelist);
	wc->freelist_size = 0;

	for (b = 0; b < sb(wc)->n_blocks; b++) {
		struct wc_entry *e = &wc->entries[b];
		e->index = b;
		e->write_in_progress = false;
		if (!writecache_entry_is_committed(wc, e)) {
			if (memory_entry(wc, e)->seq_count != -1) {
erase_this:
				ACCESS_ONCE(memory_entry(wc, e)->original_sector) = -1;
				ACCESS_ONCE(memory_entry(wc, e)->seq_count) = -1;
				need_flush = true;
			}
			writecache_add_to_freelist(wc, e);

		} else {
			struct wc_entry *old;

			old = writecache_find_entry(wc, memory_entry(wc, e)->original_sector, 0);
			if (unlikely(!old)) {
				writecache_insert_entry(wc, e);
			} else {
				if (unlikely(memory_entry(wc, old)->seq_count == memory_entry(wc, e)->seq_count)) {
					if (!ACCESS_ONCE((wc)->error))
						writecache_error(wc, "two identical entries, position %llu, sector %llu, sequence %llu",
								 (unsigned long long)b, (unsigned long long)memory_entry(wc, e)->original_sector,
								 (unsigned long long)memory_entry(wc, e)->seq_count);
				}
				if (memory_entry(wc, old)->seq_count > memory_entry(wc, e)->seq_count) {
					goto erase_this;
				} else {
					writecache_free_entry(wc, old);
					writecache_insert_entry(wc, e);
					need_flush = true;
				}
			}
		}
		cond_resched();
	}

	if (need_flush) {
		writecache_flush_all(wc);
		writecache_commit_flushed(wc);
	}

	mutex_unlock(&wc->lock);
}

static int process_flush_mesg(unsigned argc, char **argv, struct dm_writecache *wc)
{
	uint64_t seq;
	struct wc_entry *e;

	if (argc != 1)
		return -EINVAL;

	mutex_lock(&wc->lock);
	if (unlikely(dm_suspended(wc->ti))) {
		mutex_unlock(&wc->lock);
		return -EBUSY;
	}
	if (unlikely(wc->error)) {
		mutex_unlock(&wc->lock);
		return -EIO;
	}

	writecache_flush(wc);
	seq = ACCESS_ONCE(sb(wc)->seq_count);
	wc->writeback_all++;
	mutex_unlock(&wc->lock);

	queue_work(wc->writeback_wq, &wc->writeback_work);
	flush_workqueue(wc->writeback_wq);

	mutex_lock(&wc->lock);
	wc->writeback_all--;
 waited:
	if (unlikely(dm_suspended(wc->ti))) {
		mutex_unlock(&wc->lock);
		return -EBUSY;
	}
	if (unlikely(wc->error)) {
		mutex_unlock(&wc->lock);
		return -EIO;
	}
	e = NULL;
	if (!list_empty(&wc->writeback))
		e = container_of(wc->writeback.prev, struct wc_entry, lru);
	else if (!list_empty(&wc->writeback_start))
		e = container_of(wc->writeback_start.prev, struct wc_entry, lru);
	else if (!list_empty(&wc->lru))
		e = container_of(wc->lru.prev, struct wc_entry, lru);
	if (e && memory_entry(wc, e)->seq_count < seq) {
		writecache_wait_on_freelist(wc);
		goto waited;
	}
	mutex_unlock(&wc->lock);

	return 0;
}

static int writecache_message(struct dm_target *ti, unsigned argc, char **argv)
{
	int r = -EINVAL;
	struct dm_writecache *wc = ti->private;

	if (!strcasecmp(argv[0], "flush"))
		r = process_flush_mesg(argc, argv, wc);
	else
		DMWARN("unrecognised message received: %s", argv[0]);

	return r;
}

static void bio_copy_block(struct dm_writecache *wc, struct bio *bio, void *data, bool write)
{
	void *buf;
	unsigned long flags;
	unsigned size;
	unsigned remaining_size = wc->block_size;

	do {
		buf = bio_kmap_irq(bio, &flags, &size);
		if (unlikely(size > remaining_size))
			size = remaining_size;

		if (write) {
			flush_dcache_page(bio_page(bio));
			memcpy(data, buf, size);
			flush_dcache_page(bio_page(bio));
		} else {
			memcpy(buf, data, size);
			flush_dcache_page(bio_page(bio));
		}

		bio_kunmap_irq(buf, &flags);

		data = (char *)data + size;
		remaining_size -= size;
		bio_advance(bio, size);
	} while (unlikely(remaining_size));
}

static int writecache_map(struct dm_target *ti, struct bio *bio)
{
	struct wc_entry *e;
	struct dm_writecache *wc = ti->private;

	bio->bi_private = NULL;

	mutex_lock(&wc->lock);

	if (unlikely(bio->bi_rw & REQ_FLUSH)) {
		if (unlikely(wc->error))
			goto unlock_error;
		writecache_flush(wc);
		goto unlock_ok;
	}

	bio->bi_iter.bi_sector = dm_target_offset(ti, bio->bi_iter.bi_sector);

	if (unlikely((bio->bi_iter.bi_sector & (wc->block_size / 512 - 1)) != 0) ||
	    unlikely((bio->bi_iter.bi_size & (wc->block_size - 1)) != 0)) {
		DMWARN("I/O is not aligned, sector %llu, size %u, block size %u",
			(unsigned long long)bio->bi_iter.bi_sector,
			bio->bi_iter.bi_size, wc->block_size);
		goto unlock_error;
	}

	if (unlikely(bio->bi_rw & REQ_DISCARD)) {
		if (unlikely(wc->error))
			goto unlock_error;
		writecache_discard(wc, bio->bi_iter.bi_sector,
				   bio->bi_iter.bi_sector + (bio->bi_iter.bi_size >> SECTOR_SHIFT));
		goto unlock_remap_origin;
	}

	if (bio_data_dir(bio) == READ) {
next_block:
		e = writecache_find_entry(wc, bio->bi_iter.bi_sector, WFE_RETURN_FOLLOWING);
		if (e && memory_entry(wc, e)->original_sector == bio->bi_iter.bi_sector) {
			if (wc->pmem_mode) {
				bio_copy_block(wc, bio, memory_data(wc, e), false);
				if (bio->bi_iter.bi_size)
					goto next_block;
				goto unlock_ok;
			} else {
				dm_accept_partial_bio(bio, wc->block_size >> SECTOR_SHIFT);
				bio->bi_bdev = wc->ssd_dev->bdev;
				bio->bi_iter.bi_sector = cache_sector(wc, e);
				if (!writecache_entry_is_committed(wc, e))
					writecache_wait_for_ios(wc, WRITE);
				goto unlock_remap;
			}
		} else {
			if (e) {
				sector_t next_boundary =
					memory_entry(wc, e)->original_sector - bio->bi_iter.bi_sector;
				if (next_boundary < bio->bi_iter.bi_size >> SECTOR_SHIFT) {
					dm_accept_partial_bio(bio, next_boundary);
				}
			}
			goto unlock_remap_origin;
		}
	} else {
		do {
			if (unlikely(wc->error))
				goto unlock_error;
			e = writecache_find_entry(wc, bio->bi_iter.bi_sector, 0);
			if (e) {
				if (!writecache_entry_is_committed(wc, e))
					goto bio_copy;
				if (!wc->pmem_mode && !e->write_in_progress) {
					wc->overwrote_committed = true;
					goto bio_copy;
				}
			}
			e = writecache_pop_from_freelist(wc);
			if (unlikely(!e)) {
				writecache_wait_on_freelist(wc);
				continue;
			}
			memory_entry(wc, e)->original_sector = bio->bi_iter.bi_sector;
			ACCESS_ONCE(memory_entry(wc, e)->original_sector) = bio->bi_iter.bi_sector;
			ACCESS_ONCE(memory_entry(wc, e)->seq_count) = sb(wc)->seq_count;
			writecache_insert_entry(wc, e);
bio_copy:
			if (wc->pmem_mode) {
				bio_copy_block(wc, bio, memory_data(wc, e), true);
			} else {
				dm_accept_partial_bio(bio, wc->block_size >> SECTOR_SHIFT);
				bio->bi_bdev = wc->ssd_dev->bdev;
				bio->bi_iter.bi_sector = cache_sector(wc, e);
				goto unlock_remap;
			}
		} while (bio->bi_iter.bi_size);

		goto unlock_ok;
	}

unlock_remap_origin:
	bio->bi_bdev = wc->dev->bdev;
	mutex_unlock(&wc->lock);
	return DM_MAPIO_REMAPPED;

unlock_remap:
	bio->bi_private = (void *)1;
	atomic_inc(&wc->bio_in_progress[bio_data_dir(bio)]);
	mutex_unlock(&wc->lock);
	return DM_MAPIO_REMAPPED;

unlock_ok:
	mutex_unlock(&wc->lock);
	bio_endio(bio);
	return DM_MAPIO_SUBMITTED;

unlock_error:
	mutex_unlock(&wc->lock);
	bio_io_error(bio);
	return DM_MAPIO_SUBMITTED;
}

static int writecache_end_io(struct dm_target *ti, struct bio *bio, int error)
{
	struct dm_writecache *wc = ti->private;

	if (bio->bi_private != NULL) {
		int dir = bio_data_dir(bio);
		if (atomic_dec_and_test(&wc->bio_in_progress[dir]))
			if (unlikely(waitqueue_active(&wc->bio_in_progress_wait[dir])))
				wake_up(&wc->bio_in_progress_wait[dir]);
	}
	return 0;
}

static int writecache_iterate_devices(struct dm_target *ti, iterate_devices_callout_fn fn, void *data)
{
	struct dm_writecache *wc = ti->private;

	return fn(ti, wc->dev, 0, ti->len, data);
}

static void writecache_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct dm_writecache *wc = ti->private;

	if (limits->logical_block_size < wc->block_size)
		limits->logical_block_size = wc->block_size;

	if (limits->physical_block_size < wc->block_size)
		limits->physical_block_size = wc->block_size;

	if (limits->io_min < wc->block_size)
		limits->io_min = wc->block_size;
}


static void writecache_writeback_endio(struct bio *bio)
{
	struct writeback_struct *wb = container_of(bio, struct writeback_struct, bio);
	struct dm_writecache *wc = wb->wc;
	unsigned long flags;

	spin_lock_irqsave(&wc->endio_thread_wait.lock, flags);
	list_add_tail(&wb->endio_entry, &wc->endio_list);
	wake_up_locked(&wc->endio_thread_wait);
	spin_unlock_irqrestore(&wc->endio_thread_wait.lock, flags);
}

static void writecache_copy_endio(int read_err, unsigned long write_err, void *ptr)
{
	struct copy_struct *c = ptr;
	struct dm_writecache *wc = c->wc;
	unsigned long flags;

	c->error = likely(!(read_err | write_err)) ? 0 : -EIO;

	spin_lock_irqsave(&wc->endio_thread_wait.lock, flags);	/* !!! TODO: use spin_lock_irq */
	list_add_tail(&c->endio_entry, &wc->endio_list);
	wake_up_locked(&wc->endio_thread_wait);
	spin_unlock_irqrestore(&wc->endio_thread_wait.lock, flags);
}

static int writecache_endio_thread(void *data)
{
	struct dm_writecache *wc = data;

	while (1) {
		DECLARE_WAITQUEUE(wait, current);
		struct list_head list;

		spin_lock_irq(&wc->endio_thread_wait.lock);
continue_locked:
		if (!list_empty(&wc->endio_list))
			goto pop_from_list;
		if (unlikely(wc->endio_thread_terminate)) {
			spin_unlock_irq(&wc->endio_thread_wait.lock);
			break;
		}
		__set_current_state(TASK_INTERRUPTIBLE);
		__add_wait_queue(&wc->endio_thread_wait, &wait);
		spin_unlock_irq(&wc->endio_thread_wait.lock);

		schedule();

		spin_lock_irq(&wc->endio_thread_wait.lock);
		__remove_wait_queue(&wc->endio_thread_wait, &wait);
		goto continue_locked;

pop_from_list:
		list = wc->endio_list;
		list.next->prev = list.prev->next = &list;
		INIT_LIST_HEAD(&wc->endio_list);
		spin_unlock_irq(&wc->endio_thread_wait.lock);

		writecache_disk_flush(wc, wc->dev);

		mutex_lock(&wc->lock);
		// FIXME: this control structure needs serious help
		if (wc->pmem_mode) do {
			unsigned i;
			struct writeback_struct *wb =
				list_entry(list.next, struct writeback_struct, endio_entry);

			list_del(&wb->endio_entry);

#ifdef COPY_TO_PAGES_BEFORE_WRITING
			{
				struct bio_vec *bv;
				bio_for_each_segment_all(bv, &wb->bio, i)
					mempool_free(bv->bv_page, wc->page_pool);
			}
#endif
			if (unlikely(wb->bio.bi_error))
				writecache_error(wc, "write error %d", wb->bio.bi_error);
			i = 0;
			do {
				struct wc_entry *e = wb->wc_list[i];
				BUG_ON(!e->write_in_progress);
				e->write_in_progress = false;
				if (likely(!wc->error))
					writecache_free_entry(wc, e);
				wc->writeback_size--;
			} while (++i < wb->wc_list_n);
			if (wb->wc_list != wb->wc_list_inline)
				kfree(wb->wc_list);
			bio_put(&wb->bio);
		} while (!list_empty(&list)); else do {
			struct copy_struct *c = list_entry(list.next, struct copy_struct, endio_entry);
			struct wc_entry *e;

			list_del(&c->endio_entry);

			if (unlikely(c->error))
				writecache_error(wc, "copy error");

			e = c->e;
			BUG_ON(!e->write_in_progress);
			e->write_in_progress = false;

			if (likely(!wc->error))
				writecache_free_entry(wc, e);

			wc->writeback_size--;
			mempool_free(c, wc->copy_pool);
		} while (!list_empty(&list));

		writecache_wait_for_ios(wc, READ);
		writecache_commit_flushed(wc);

		mutex_unlock(&wc->lock);
	}

	return 0;
}

static bool wc_add_block(struct writeback_struct *wb, struct wc_entry *e, gfp_t gfp)
{
	unsigned block_size = wb->wc->block_size;

#ifndef COPY_TO_PAGES_BEFORE_WRITING
	void *address = memory_data(wb->wc, e);
	persistent_memory_flush_cache(address, block_size);
	return bio_add_page(&wb->bio, persistent_memory_page(address),
			    block_size, persistent_memory_page_offset(address)) != 0;
#else
	if (wb->page_offset == PAGE_SIZE) {
		wb->page = mempool_alloc(wb->wc->page_pool, gfp);
		if (unlikely(!wb->page))
			return false;
		wb->page_offset = 0;
	}
	memcpy((char *)page_address(wb->page) + wb->page_offset,
	       memory_data(wb->wc, e), block_size);
	if (unlikely(!bio_add_page(&wb->bio, wb->page, block_size, wb->page_offset))) {
		if (!wb->page_offset) {
			mempool_free(wb->page, wb->wc->page_pool);
			wb->page_offset = PAGE_SIZE;
		}
		return false;
	}
	wb->page_offset += block_size;
	return true;
#endif
}

static void writecache_writeback(struct work_struct *work)
{
	struct dm_writecache *wc = container_of(work, struct dm_writecache, writeback_work);
	struct blk_plug plug;
	struct wc_entry *e, *f;
	struct rb_node *node;
	struct list_head skipped;

	mutex_lock(&wc->lock);
restart:
	if (unlikely(wc->error) || unlikely(dm_suspended(wc->ti))) {
		mutex_unlock(&wc->lock);
		return;
	}

	if (unlikely(wc->writeback_all)) {
		if (writecache_wait_for_writeback(wc))
			goto restart;
	}

	if (wc->overwrote_committed)
		writecache_wait_for_ios(wc, WRITE);

	INIT_LIST_HEAD(&skipped);
	while (!list_empty(&wc->lru) &&
	       (wc->writeback_all ||
		wc->freelist_size + wc->writeback_size <= wc->freelist_high_watermark)) {
		e = container_of(wc->lru.prev, struct wc_entry, lru);
		BUG_ON(e->write_in_progress);
		if (unlikely(!writecache_entry_is_committed(wc, e)))
			writecache_flush(wc);
		node = rb_prev(&e->rb_node);
		if (node) {
			f = container_of(node, struct wc_entry, rb_node);
			if (unlikely(memory_entry(wc, f)->original_sector ==
				     memory_entry(wc, e)->original_sector)) {
				BUG_ON(!f->write_in_progress);
				list_del(&e->lru);
				list_add(&e->lru, &skipped);
				continue;
			}
		}
		wc->writeback_size++;
		list_del(&e->lru);
		list_add(&e->lru, &wc->writeback_start);
		e->write_in_progress = true;
		e->wc_list_contiguous = 1;

		f = e;
		/* Only coalesce if we are on pmem */
		// FIXME: again, kill this non-standard control structure
		if (wc->pmem_mode) while (1) {
			struct rb_node *next;
			struct wc_entry *g;
			next = rb_next(&f->rb_node);
			if (unlikely(!next))
				break;
			g = container_of(next, struct wc_entry, rb_node);
			if (memory_entry(wc, g)->original_sector ==
			    memory_entry(wc, f)->original_sector) {
				f = g;
				continue;
			}
			if (memory_entry(wc, g)->original_sector !=
			    memory_entry(wc, f)->original_sector + (wc->block_size >> SECTOR_SHIFT))
				break;
			if (unlikely(g->write_in_progress))
				break;
			if (unlikely(!writecache_entry_is_committed(wc, g)))
				break;

			wc->writeback_size++;
			list_del(&g->lru);
			list_add(&g->lru, &wc->writeback_start);
			g->write_in_progress = true;
			g->wc_list_contiguous = BIO_MAX_PAGES;
			f = g;
			e->wc_list_contiguous++;
			if (unlikely(e->wc_list_contiguous == BIO_MAX_PAGES))
				break;
		}
		cond_resched();
	}

	list_splice_tail(&skipped, &wc->lru);

	mutex_unlock(&wc->lock);

	blk_start_plug(&plug);
	mutex_lock(&wc->lock);

	// FIXME: non-standard control structure must go
	if (wc->pmem_mode) while (!list_empty(&wc->writeback_start)) {
		struct bio *bio;
		struct writeback_struct *wb;
		unsigned max_pages;

		e = container_of(wc->writeback_start.prev, struct wc_entry, lru);
		list_del(&e->lru);
		list_add(&e->lru, &wc->writeback);

		mutex_unlock(&wc->lock);

		max_pages = e->wc_list_contiguous;

		bio = bio_alloc_bioset(GFP_NOIO, max_pages, wc->bio_set);
		wb = container_of(bio, struct writeback_struct, bio);
		wb->wc = wc;
		wb->bio.bi_end_io = writecache_writeback_endio;
		wb->bio.bi_bdev = wc->dev->bdev;
		wb->bio.bi_iter.bi_sector = memory_entry(wc, e)->original_sector;
#ifdef COPY_TO_PAGES_BEFORE_WRITING
		wb->page_offset = PAGE_SIZE;
#endif
		if (max_pages > WB_LIST_INLINE) {
			wb->wc_list = kmalloc(max_pages * sizeof(struct wc_entry *),
					      GFP_NOIO | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN);
			if (unlikely(!wb->wc_list))
				goto use_inline_list;
		} else {
use_inline_list:
			wb->wc_list = wb->wc_list_inline;
			max_pages = WB_LIST_INLINE;
		}

		BUG_ON(!wc_add_block(wb, e, GFP_NOIO));

		wb->wc_list[0] = e;
		wb->wc_list_n = 1;

		while (!list_empty(&wc->writeback_start) && wb->wc_list_n < max_pages) {
			f = container_of(wc->writeback_start.prev, struct wc_entry, lru);
			if (memory_entry(wc, f)->original_sector !=
			    memory_entry(wc, e)->original_sector + (wc->block_size >> SECTOR_SHIFT))
				break;
			if (!wc_add_block(wb, f, GFP_NOWAIT | __GFP_NOWARN))
				break;
			list_del(&f->lru);
			list_add(&f->lru, &wc->writeback);
			wb->wc_list[wb->wc_list_n++] = f;
			e = f;
		}
		submit_bio(WRITE, &wb->bio);
		cond_resched();

		mutex_lock(&wc->lock);
	} else while (!list_empty(&wc->writeback_start)) {
		struct dm_io_region from, to;
		struct copy_struct *c;

		e = container_of(wc->writeback_start.prev, struct wc_entry, lru);
		list_del(&e->lru);
		list_add(&e->lru, &wc->writeback);
		mutex_unlock(&wc->lock);

		from.bdev = wc->ssd_dev->bdev;
		from.sector = cache_sector(wc, e);
		from.count = wc->block_size >> SECTOR_SHIFT;
		to.bdev = wc->dev->bdev;
		to.sector = memory_entry(wc, e)->original_sector;
		to.count = wc->block_size >> SECTOR_SHIFT;

		c = mempool_alloc(wc->copy_pool, GFP_NOIO);
		c->wc = wc;
		c->e = e;

		dm_kcopyd_copy(wc->dm_kcopyd, &from, 1, &to, 0, writecache_copy_endio, c);

		mutex_lock(&wc->lock);
	}

	mutex_unlock(&wc->lock);
	blk_finish_plug(&plug);
}

static int calculate_memory_size(uint64_t device_size, unsigned block_size,
				 uint64_t *n_blocks_p, uint64_t *n_metadata_blocks_p)
{
	uint64_t n_blocks, offset;
	struct wc_entry e;

	n_blocks = device_size;
	do_div(n_blocks, block_size + sizeof(struct wc_memory_entry));

	while (1) {
		if (!n_blocks)
			return -ENOSPC;
		// FIXME: this (size_t)-sizeof()/sizeof() looks flawed...
		if (n_blocks >= (size_t)-sizeof(struct wc_memory_superblock) / sizeof(struct wc_memory_entry))
			return -EFBIG;
		offset = offsetof(struct wc_memory_superblock, entries[n_blocks]);
		offset = (offset + block_size - 1) & ~(uint64_t)(block_size - 1);
		if (offset + n_blocks * block_size <= device_size)
			break;
		n_blocks--;
	}

	/* check if the bit field overflows */
	e.index = n_blocks;
	if (e.index != n_blocks)
		return -EFBIG;

	if (n_blocks_p)
		*n_blocks_p = n_blocks;
	if (n_metadata_blocks_p)
		*n_metadata_blocks_p = offset >> __ffs(block_size);
	return 0;
}

static int init_memory(struct dm_writecache *wc)
{
	uint64_t b, n_blocks;
	int r;

	r = calculate_memory_size(wc->memory_map_size, wc->block_size, &n_blocks, NULL);
	if (r)
		return r;

	memset(sb(wc), 0, sizeof(struct wc_memory_superblock));
	sb(wc)->block_size = wc->block_size;
	sb(wc)->n_blocks = n_blocks;
	sb(wc)->seq_count = 0;

	for (b = 0; b < n_blocks; b++) {
		sb(wc)->entries[b].original_sector = -1;
		sb(wc)->entries[b].seq_count = -1;
	}

	writecache_flush_all(wc);
	writecache_commit_flushed(wc);
	sb(wc)->magic = MEMORY_SUPERBLOCK_MAGIC;
	writecache_flush_region(wc, &sb(wc)->magic, sizeof sb(wc)->magic);
	writecache_commit_flushed(wc);

	return 0;
}

static void writecache_dtr(struct dm_target *ti)
{
	struct dm_writecache *wc = ti->private;

	if (!wc)
		return;

	if (wc->endio_thread) {
		spin_lock_irq(&wc->endio_thread_wait.lock);
		wc->endio_thread_terminate = true;
		wake_up_locked(&wc->endio_thread_wait);
		spin_unlock_irq(&wc->endio_thread_wait.lock);
		kthread_stop(wc->endio_thread);
	}

#ifdef COPY_TO_PAGES_BEFORE_WRITING
	if (wc->page_pool)
		mempool_destroy(wc->page_pool);
#endif

	if (wc->bio_set)
		bioset_free(wc->bio_set);

	if (wc->copy_pool)
		mempool_destroy(wc->copy_pool);

	if (wc->writeback_wq)
		destroy_workqueue(wc->writeback_wq);

	if (wc->dev)
		dm_put_device(ti, wc->dev);

	if (wc->ssd_dev)
		dm_put_device(ti, wc->ssd_dev);

	if (wc->entries)
		vfree(wc->entries);

	if (wc->memory_map) {
		if (wc->pmem_mode)
			persistent_memory_release(ti, &wc->pmem_holder,
						  wc->memory_map, wc->memory_map_size);
		else
			vfree(wc->memory_map);
	}

	if (wc->memory_name)
		kfree(wc->memory_name);

	if (wc->dm_kcopyd)
		dm_kcopyd_client_destroy(wc->dm_kcopyd);

	if (wc->dm_io)
		dm_io_client_destroy(wc->dm_io);

	if (wc->dirty_bitmap)
		vfree(wc->dirty_bitmap);

	kfree(wc);
}

static int writecache_ctr(struct dm_target *ti, unsigned argc, char **argv)
{
	struct dm_writecache *wc;
	struct dm_arg_set as;
	const char *string;
	unsigned opt_params;
	size_t offset, data_size;
	int i, r;
	char dummy;
	int high_wm_percent = 95;
	int low_wm_percent = 90;
	uint64_t x;

	static struct dm_arg _args[] = {
		{0, 4, "Invalid number of feature args"},
	};

	as.argc = argc;
	as.argv = argv;

	wc = kzalloc(sizeof(struct dm_writecache), GFP_KERNEL);
	if (!wc) {
		ti->error = "Cannot allocate writecache structure";
		r = -ENOMEM;
		goto bad;
	}
	ti->private = wc;
	wc->ti = ti;

	mutex_init(&wc->lock);
	writecache_poison_lists(wc);
	init_waitqueue_head(&wc->freelist_wait);

	for (i = 0; i < 2; i++) {
		atomic_set(&wc->bio_in_progress[i], 0);
		init_waitqueue_head(&wc->bio_in_progress_wait[i]);
	}

	wc->dm_io = dm_io_client_create();
	if (!wc->dm_io) {
		r = -ENOMEM;
		ti->error = "Unable to allocate dm-io client";
		goto bad;
	}

	wc->writeback_wq = alloc_workqueue("writecache-writeabck",
					   WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
	if (!wc->writeback_wq) {
		r = -ENOMEM;
		ti->error = "Could not allocate writeback workqueue";
		goto bad;
	}
	INIT_WORK(&wc->writeback_work, writecache_writeback);

	init_waitqueue_head(&wc->endio_thread_wait);
	INIT_LIST_HEAD(&wc->endio_list);
	wc->endio_thread = kthread_create(writecache_endio_thread, wc, "writecache_endio");
	if (IS_ERR(wc->endio_thread)) {
		r = PTR_ERR(wc->endio_thread);
		wc->endio_thread = NULL;
		ti->error = "Couldn't spawn endio thread";
		goto bad;
	}
	wake_up_process(wc->endio_thread);

	string = dm_shift_arg(&as);
	if (!string)
		goto bad_arguments;

	if (!strcasecmp(string, "p"))
		wc->pmem_mode = true;
	else if (!strcasecmp(string, "s"))
		wc->pmem_mode = false;
	else
		goto bad_arguments;

	if (wc->pmem_mode) {
		wc->bio_set = bioset_create(BIO_POOL_SIZE,
					    offsetof(struct writeback_struct, bio));
		if (!wc->bio_set) {
			r = -ENOMEM;
			ti->error = "Could not allocate bio set";
			goto bad;
		}
	} else {
		wc->copy_pool = mempool_create_kmalloc_pool(1, sizeof(struct copy_struct));
		if (!wc->copy_pool) {
			r = -ENOMEM;
			ti->error = "Could not allocate mempool";
			goto bad;
		}
	}
#ifdef COPY_TO_PAGES_BEFORE_WRITING
	if (wc->pmem_mode) {
		wc->page_pool = mempool_create_page_pool(BIO_POOL_SIZE, 0);
		if (!wc->page_pool) {
			r = -ENOMEM;
			ti->error = "Could not allocate page mempool";
			goto bad;
		}
	}
#endif
	string = dm_shift_arg(&as);
	if (!string)
		goto bad_arguments;
	r = dm_get_device(ti, string, dm_table_get_mode(ti->table), &wc->dev);
	if (r) {
		ti->error = "Data device lookup failed";
		goto bad;
	}

	string = dm_shift_arg(&as);
	if (!string)
		goto bad_arguments;
	if (wc->pmem_mode) {
		wc->memory_name = kstrdup(string, GFP_KERNEL);
		if (!wc->memory_name) {
			r = -ENOMEM;
			ti->error = "Could not allocate string";
			goto bad;
		}

		r = persistent_memory_claim(ti, wc->memory_name, &wc->pmem_holder,
					    &wc->memory_map, &wc->memory_map_size);
		if (r) {
			ti->error = "Unable to map persistent memory";
			goto bad;
		}
	} else {
		r = dm_get_device(ti, string, dm_table_get_mode(ti->table), &wc->ssd_dev);
		if (r) {
			ti->error = "Data device lookup failed";
			goto bad;
		}
		wc->memory_map_size = i_size_read(wc->ssd_dev->bdev->bd_inode);
	}

	string = dm_shift_arg(&as);
	if (!string)
		goto bad_arguments;
	if (sscanf(string, "%u%c", &wc->block_size, &dummy) != 1 ||
	    wc->block_size < 512 || wc->block_size > PAGE_SIZE ||
	    (wc->block_size & (wc->block_size - 1))) {
		r = -EINVAL;
		ti->error = "Invalid block size";
		goto bad;
	}
	wc->block_size_bits = __ffs(wc->block_size);

	r = dm_read_arg_group(_args, &as, &opt_params, &ti->error);
	if (r)
		goto bad;

	while (opt_params) {
		string = dm_shift_arg(&as);
		if (!strcasecmp(string, "high-watermark")) {
			string = dm_shift_arg(&as);
			if (!string)
				goto invalid_optional;
			if (sscanf(string, "%d%%%c", &high_wm_percent, &dummy) == 1) {
				if (high_wm_percent < 0 || high_wm_percent > 100)
					goto invalid_optional;
				wc->high_wm_percent_set = true;
			} else
				goto invalid_optional;
		} else if (!strcasecmp(string, "low-watermark")) {
			string = dm_shift_arg(&as);
			if (!string)
				goto invalid_optional;
			if (sscanf(string, "%d%%%c", &low_wm_percent, &dummy) == 1) {
				if (low_wm_percent < 0 || low_wm_percent > 100)
					goto invalid_optional;
				wc->low_wm_percent_set = true;
			} else
				goto invalid_optional;
		} else {
invalid_optional:
			r = -EINVAL;
			ti->error = "Invalid optional argument";
			goto bad;
		}
	}

	if (!wc->pmem_mode) {
		struct dm_io_region region;
		struct dm_io_request req;
		uint64_t n_blocks, n_metadata_blocks, n_bitmap_bits;

		r = calculate_memory_size(wc->memory_map_size, wc->block_size,
					  &n_blocks, &n_metadata_blocks);
		if (r) {
			ti->error = "Invalid device size";
			goto bad;
		}

		n_bitmap_bits = ((n_metadata_blocks << wc->block_size_bits) + BITMAP_GRANULARITY - 1) / BITMAP_GRANULARITY;
		/* this is limitation of test_bit functions */
		if (n_bitmap_bits > 1U << 31) {
			r = -EFBIG;
			ti->error = "Invalid device size";
		}

		wc->memory_map = vmalloc(n_metadata_blocks << wc->block_size_bits);
		if (!wc->memory_map) {
			r = -ENOMEM;
			ti->error = "Unable to allocate memory for metadata";
			goto bad;
		}

		wc->dm_kcopyd = dm_kcopyd_client_create(&dm_kcopyd_throttle);
		if (!wc->dm_kcopyd) {
			r = -ENOMEM;
			ti->error = "Unable to allocate dm-kcopyd client";
			goto bad;
		}

		wc->metadata_sectors = n_metadata_blocks << (wc->block_size_bits - SECTOR_SHIFT);
		wc->dirty_bitmap_size = (n_bitmap_bits + BITS_PER_LONG - 1) / BITS_PER_LONG * sizeof(unsigned long);
		wc->dirty_bitmap = vzalloc(wc->dirty_bitmap_size);
		if (!wc->dirty_bitmap) {
			r = -ENOMEM;
			ti->error = "Unable to allocate dirty bitmap";
			goto bad;
		}

		region.bdev = wc->ssd_dev->bdev;
		region.sector = 0;
		region.count = wc->metadata_sectors;
		req.bi_rw = READ;
		req.mem.type = DM_IO_VMA;
		req.mem.ptr.vma = (char *)wc->memory_map;
		req.client = wc->dm_io;
		req.notify.fn = NULL;

		r = dm_io(&req, 1, &region, NULL);
		if (unlikely(r)) {
			ti->error = "Unable to read metadata";
			goto bad;
		}
	}

	if (!sb(wc)->magic) {
		r = init_memory(wc);
		if (r) {
			ti->error = "Unable to initialize device";
			goto bad;
		}
	}

	if (sb(wc)->magic != MEMORY_SUPERBLOCK_MAGIC) {
		ti->error = "Invalid magic in the superblock";
		r = -EINVAL;
		goto bad;
	}

	if (sb(wc)->block_size != wc->block_size) {
		ti->error = "Block size does not match";
		r = -EINVAL;
		goto bad;
	}

	offset = sb(wc)->n_blocks * sizeof(struct wc_memory_entry);
	if (offset / sizeof(struct wc_memory_entry) != sb(wc)->n_blocks) {
// FIXME: eliminate these gotos somehow
overflow:
		ti->error = "Overflow in size calculation";
		r = -EINVAL;
		goto bad;
	}
	offset += sizeof(struct wc_memory_superblock);
	if (offset < sizeof(struct wc_memory_superblock))
		goto overflow;
	offset = (offset + wc->block_size - 1) & ~(size_t)(wc->block_size - 1);
	if (!offset)
		goto overflow;
	data_size = sb(wc)->n_blocks * sb(wc)->block_size;
	if (data_size / sb(wc)->block_size != sb(wc)->n_blocks)
		goto overflow;
	if (offset + data_size < offset)
		goto overflow;
	if (offset + data_size > wc->memory_map_size) {
		ti->error = "Memory area is too small";
		r = -EINVAL;
		goto bad;
	}

	wc->metadata_sectors = offset >> SECTOR_SHIFT;
	wc->block_start = (char *)sb(wc) + offset;

	x = sb(wc)->n_blocks * (100 - high_wm_percent);
	do_div(x, 100);
	wc->freelist_high_watermark = x;
	x = sb(wc)->n_blocks * (100 - low_wm_percent);
	do_div(x, 100);
	wc->freelist_low_watermark = x;

	wc->entries = vmalloc(sizeof(struct wc_entry) * sb(wc)->n_blocks);
	if (unlikely(!wc->entries)) {
		ti->error = "Cannot allocate memory";
		r = -ENOMEM;
		goto bad;
	}

	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->discard_zeroes_data_unsupported = true;

	return 0;

bad_arguments:
	r = -EINVAL;
	ti->error = "Bad arguments";
bad:
	writecache_dtr(ti);
	return r;
}

static void writecache_status(struct dm_target *ti, status_type_t type,
			      unsigned status_flags, char *result, unsigned maxlen)
{
	struct dm_writecache *wc = ti->private;
	unsigned extra_args;
	unsigned sz = 0;
	uint64_t x;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%llu %llu %llu", (unsigned long long)sb(wc)->n_blocks,
		       (unsigned long long)wc->freelist_size, (unsigned long long)wc->writeback_size);
		break;
	case STATUSTYPE_TABLE:
		if (wc->pmem_mode)
			DMEMIT("p %s %s %u ", wc->dev->name, wc->memory_name, wc->block_size);
		else
			DMEMIT("s %s %s %u ", wc->dev->name, wc->ssd_dev->name, wc->block_size);
		extra_args = 0;
		if (wc->high_wm_percent_set)
			extra_args += 2;
		if (wc->low_wm_percent_set)
			extra_args += 2;
		DMEMIT("%u", extra_args);
		if (wc->high_wm_percent_set) {
			x = (uint64_t)wc->freelist_high_watermark * 100;
			do_div(x, (size_t)sb(wc)->n_blocks);
			DMEMIT(" high-watermark %u%%", 100 - (unsigned)x);
		}
		if (wc->low_wm_percent_set) {
			x = (uint64_t)wc->freelist_low_watermark * 100;
			do_div(x, (size_t)sb(wc)->n_blocks);
			DMEMIT(" low-watermark %u%%", 100 - (unsigned)x);
		}
		break;
	}
}

static struct target_type writecache_target = {
	.name			= "writecache",
	.version		= {1, 0, 0},
	.module			= THIS_MODULE,
	.ctr			= writecache_ctr,
	.dtr			= writecache_dtr,
	.status			= writecache_status,
	.postsuspend		= writecache_suspend,
	.resume			= writecache_resume,
	.message		= writecache_message,
	.map			= writecache_map,
	.end_io			= writecache_end_io,
	.iterate_devices	= writecache_iterate_devices,
	.io_hints		= writecache_io_hints,
};

static int __init dm_writecache_init(void)
{
	int r;

	r = dm_register_target(&writecache_target);
	if (r < 0) {
		DMERR("register failed %d", r);
		return r;
	}

	return 0;
}

static void __exit dm_writecache_exit(void)
{
	dm_unregister_target(&writecache_target);
}

module_init(dm_writecache_init);
module_exit(dm_writecache_exit);

MODULE_DESCRIPTION(DM_NAME " writecache target");
MODULE_AUTHOR("Mikulas Patocka <mpatocka@redhat.com>");
MODULE_LICENSE("GPL");
