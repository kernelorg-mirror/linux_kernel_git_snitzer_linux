#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/device-mapper.h>
#include <linux/dm-io.h>
#include <linux/crypto.h>
#include <linux/lzo.h>
#include <linux/kthread.h>
#include <linux/page-flags.h>
#include <linux/completion.h>
#include "dm-insitu-comp.h"

#define DM_MSG_PREFIX "dm_insitu_comp"

static struct insitu_comp_compressor_data compressors[] = {
	[INSITU_COMP_ALG_LZO] = {
		.name = "lzo",
		.comp_len = lzo_comp_len,
	},
	[INSITU_COMP_ALG_ZLIB] = {
		.name = "deflate",
	},
};
static int default_compressor;

static struct kmem_cache *insitu_comp_io_range_cachep;
static struct kmem_cache *insitu_comp_meta_io_cachep;

static struct insitu_comp_io_worker insitu_comp_io_workers[NR_CPUS];
static struct workqueue_struct *insitu_comp_wq;

/* each block has 5 bits metadata */
static u8 insitu_comp_get_meta(struct insitu_comp_info *info, u64 block_index)
{
	u64 first_bit = block_index * INSITU_COMP_META_BITS;
	int bits, offset;
	u8 data, ret = 0;

	offset = first_bit & 7;
	bits = min_t(u8, INSITU_COMP_META_BITS, 8 - offset);

	data = info->meta_bitmap[first_bit >> 3];
	ret = (data >> offset) & ((1 << bits) - 1);

	if (bits < INSITU_COMP_META_BITS) {
		data = info->meta_bitmap[(first_bit >> 3) + 1];
		bits = INSITU_COMP_META_BITS - bits;
		ret |= (data & ((1 << bits) - 1)) <<
			(INSITU_COMP_META_BITS - bits);
	}
	return ret;
}

static void insitu_comp_set_meta(struct insitu_comp_info *info,
	u64 block_index, u8 meta, bool dirty_meta)
{
	u64 first_bit = block_index * INSITU_COMP_META_BITS;
	int bits, offset;
	u8 data;
	struct page *page;

	offset = first_bit & 7;
	bits = min_t(u8, INSITU_COMP_META_BITS, 8 - offset);

	data = info->meta_bitmap[first_bit >> 3];
	data &= ~(((1 << bits) - 1) << offset);
	data |= (meta & ((1 << bits) - 1)) << offset;
	info->meta_bitmap[first_bit >> 3] = data;

	/*
	 * For writethrough, we write metadata directly. For writeback, if
	 * request is FUA, we do this too; otherwise we just dirty the page,
	 * which will be flush out in an interval
	 */
	if (info->write_mode == INSITU_COMP_WRITE_BACK) {
		page = vmalloc_to_page(&info->meta_bitmap[first_bit >> 3]);
		if (dirty_meta)
			SetPageDirty(page);
		else
			ClearPageDirty(page);
	}

	if (bits < INSITU_COMP_META_BITS) {
		meta >>= bits;
		data = info->meta_bitmap[(first_bit >> 3) + 1];
		bits = INSITU_COMP_META_BITS - bits;
		data = (data >> bits) << bits;
		data |= meta & ((1 << bits) - 1);
		info->meta_bitmap[(first_bit >> 3) + 1] = data;

		if (info->write_mode == INSITU_COMP_WRITE_BACK) {
			page = vmalloc_to_page(&info->meta_bitmap[
						(first_bit >> 3) + 1]);
			if (dirty_meta)
				SetPageDirty(page);
			else
				ClearPageDirty(page);
		}
	}
}

/*
 * set metadata for an extent since block @block_index, length is
 * @logical_blocks.  The extent uses @data_sectors sectors
 */
static void insitu_comp_set_extent(struct insitu_comp_req *req,
	u64 block_index, u16 logical_blocks, sector_t data_sectors)
{
	int i;
	u8 data;

	for (i = 0; i < logical_blocks; i++) {
		data = min_t(sector_t, data_sectors, 8);
		data_sectors -= data;
		if (i != 0)
			data |= INSITU_COMP_TAIL_MASK;
		/* For FUA, we write out meta data directly */
		insitu_comp_set_meta(req->info, block_index + i, data,
					!(insitu_req_rw(req) & REQ_FUA));
	}
}

/*
 * get metadata for an extent covering block @block_index. @first_block_index
 * returns the first block of the extent. @logical_sectors returns the extent
 * length. @data_sectors returns the sectors the extent uses
 */
static void insitu_comp_get_extent(struct insitu_comp_info *info,
	u64 block_index, u64 *first_block_index, u16 *logical_sectors,
	u16 *data_sectors)
{
	u8 data;

	data = insitu_comp_get_meta(info, block_index);
	while (data & INSITU_COMP_TAIL_MASK) {
		block_index--;
		data = insitu_comp_get_meta(info, block_index);
	}
	*first_block_index = block_index;
	*logical_sectors = INSITU_COMP_BLOCK_SIZE >> 9;
	*data_sectors = data & INSITU_COMP_LENGTH_MASK;
	block_index++;
	while (block_index < info->data_blocks) {
		data = insitu_comp_get_meta(info, block_index);
		if (!(data & INSITU_COMP_TAIL_MASK))
			break;
		*logical_sectors += INSITU_COMP_BLOCK_SIZE >> 9;
		*data_sectors += data & INSITU_COMP_LENGTH_MASK;
		block_index++;
	}
}

static int insitu_comp_access_super(struct insitu_comp_info *info,
	void *addr, int rw)
{
	struct dm_io_region region;
	struct dm_io_request req;
	unsigned long io_error = 0;
	int ret;

	region.bdev = info->dev->bdev;
	region.sector = 0;
	region.count = INSITU_COMP_BLOCK_SIZE >> 9;

	req.bi_rw = rw;
	req.mem.type = DM_IO_KMEM;
	req.mem.offset = 0;
	req.mem.ptr.addr = addr;
	req.notify.fn = NULL;
	req.client = info->io_client;

	ret = dm_io(&req, 1, &region, &io_error);
	if (ret || io_error)
		return -EIO;
	return 0;
}

static void insitu_comp_meta_io_done(unsigned long error, void *context)
{
	struct insitu_comp_meta_io *meta_io = context;

	meta_io->fn(meta_io->data, error);
	kmem_cache_free(insitu_comp_meta_io_cachep, meta_io);
}

static int insitu_comp_write_meta(struct insitu_comp_info *info,
	u64 start_page, u64 end_page, void *data,
	void (*fn)(void *data, unsigned long error), int rw)
{
	struct insitu_comp_meta_io *meta_io;

	BUG_ON(end_page > info->meta_bitmap_pages);

	meta_io = kmem_cache_alloc(insitu_comp_meta_io_cachep, GFP_NOIO);
	if (!meta_io) {
		fn(data, -ENOMEM);
		return -ENOMEM;
	}
	meta_io->data = data;
	meta_io->fn = fn;

	meta_io->io_region.bdev = info->dev->bdev;
	meta_io->io_region.sector = INSITU_COMP_META_START_SECTOR +
					(start_page << (PAGE_SHIFT - 9));
	meta_io->io_region.count = (end_page - start_page) << (PAGE_SHIFT - 9);

	atomic64_add(meta_io->io_region.count << 9, &info->meta_write_size);

	meta_io->io_req.bi_rw = rw;
	meta_io->io_req.mem.type = DM_IO_VMA;
	meta_io->io_req.mem.offset = 0;
	meta_io->io_req.mem.ptr.addr = info->meta_bitmap +
						(start_page << PAGE_SHIFT);
	meta_io->io_req.notify.fn = insitu_comp_meta_io_done;
	meta_io->io_req.notify.context = meta_io;
	meta_io->io_req.client = info->io_client;

	dm_io(&meta_io->io_req, 1, &meta_io->io_region, NULL);
	return 0;
}

struct writeback_flush_data {
	struct completion complete;
	atomic_t cnt;
};

static void writeback_flush_io_done(void *data, unsigned long error)
{
	struct writeback_flush_data *wb = data;

	if (atomic_dec_return(&wb->cnt))
		return;
	complete(&wb->complete);
}

static void insitu_comp_flush_dirty_meta(struct insitu_comp_info *info,
			struct writeback_flush_data *data)
{
	struct page *page;
	u64 start = 0, index;
	u32 pending = 0, cnt = 0;
	bool dirty;
	struct blk_plug plug;

	blk_start_plug(&plug);
	for (index = 0; index < info->meta_bitmap_pages; index++, cnt++) {
		if (cnt == 256) {
			cnt = 0;
			cond_resched();
		}

		page = vmalloc_to_page(info->meta_bitmap +
					(index << PAGE_SHIFT));
		dirty = TestClearPageDirty(page);

		if (pending == 0 && dirty) {
			start = index;
			pending++;
			continue;
		} else if (pending == 0)
			continue;
		else if (pending > 0 && dirty) {
			pending++;
			continue;
		}

		/* pending > 0 && !dirty */
		atomic_inc(&data->cnt);
		insitu_comp_write_meta(info, start, start + pending, data,
			writeback_flush_io_done, WRITE);
		pending = 0;
	}

	if (pending > 0) {
		atomic_inc(&data->cnt);
		insitu_comp_write_meta(info, start, start + pending, data,
			writeback_flush_io_done, WRITE);
	}
	blkdev_issue_flush(info->dev->bdev, GFP_NOIO, NULL);
	blk_finish_plug(&plug);
}

/* writeback thread flushs all dirty metadata to disk in an interval */
static int insitu_comp_meta_writeback_thread(void *data)
{
	struct insitu_comp_info *info = data;
	struct writeback_flush_data wb;

	atomic_set(&wb.cnt, 1);
	init_completion(&wb.complete);

	while (!kthread_should_stop()) {
		schedule_timeout_interruptible(
			msecs_to_jiffies(info->writeback_delay * 1000));
		insitu_comp_flush_dirty_meta(info, &wb);
	}

	insitu_comp_flush_dirty_meta(info, &wb);

	writeback_flush_io_done(&wb, 0);
	wait_for_completion(&wb.complete);
	return 0;
}

static int insitu_comp_init_meta(struct insitu_comp_info *info, bool new)
{
	struct dm_io_region region;
	struct dm_io_request req;
	unsigned long io_error = 0;
	struct blk_plug plug;
	int ret;
	ssize_t len = DIV_ROUND_UP_ULL(info->meta_bitmap_bits, BITS_PER_LONG);

	len *= sizeof(unsigned long);

	region.bdev = info->dev->bdev;
	region.sector = INSITU_COMP_META_START_SECTOR;
	region.count = (len + 511) >> 9;

	req.mem.type = DM_IO_VMA;
	req.mem.offset = 0;
	req.mem.ptr.addr = info->meta_bitmap;
	req.notify.fn = NULL;
	req.client = info->io_client;

	blk_start_plug(&plug);
	if (new) {
		memset(info->meta_bitmap, 0, len);
		req.bi_rw = WRITE_FLUSH;
		ret = dm_io(&req, 1, &region, &io_error);
	} else {
		req.bi_rw = READ;
		ret = dm_io(&req, 1, &region, &io_error);
	}
	blk_finish_plug(&plug);

	if (ret || io_error) {
		info->ti->error = "Access metadata error";
		return -EIO;
	}

	if (info->write_mode == INSITU_COMP_WRITE_BACK) {
		info->writeback_tsk = kthread_run(
			insitu_comp_meta_writeback_thread,
			info, "insitu_comp_writeback");
		if (!info->writeback_tsk) {
			info->ti->error = "Create writeback thread error";
			return -EINVAL;
		}
	}

	return 0;
}

static int insitu_comp_alloc_compressor(struct insitu_comp_info *info)
{
	int i;

	for_each_possible_cpu(i) {
		info->tfm[i] = crypto_alloc_comp(
			compressors[info->comp_alg].name, 0, 0);
		if (IS_ERR(info->tfm[i])) {
			info->tfm[i] = NULL;
			goto err;
		}
	}
	return 0;
err:
	for_each_possible_cpu(i) {
		if (info->tfm[i]) {
			crypto_free_comp(info->tfm[i]);
			info->tfm[i] = NULL;
		}
	}
	return -ENOMEM;
}

static void insitu_comp_free_compressor(struct insitu_comp_info *info)
{
	int i;

	for_each_possible_cpu(i) {
		if (info->tfm[i]) {
			crypto_free_comp(info->tfm[i]);
			info->tfm[i] = NULL;
		}
	}
}

static int insitu_comp_read_or_create_super(struct insitu_comp_info *info)
{
	void *addr;
	struct insitu_comp_super_block *super;
	u64 total_blocks;
	u64 data_blocks, meta_blocks;
	u32 rem, cnt;
	bool new_super = false;
	int ret;
	ssize_t len;

	total_blocks = i_size_read(info->dev->bdev->bd_inode) >>
					INSITU_COMP_BLOCK_SHIFT;
	data_blocks = total_blocks - 1;
	rem = do_div(data_blocks, INSITU_COMP_BLOCK_SIZE * 8 +
			INSITU_COMP_META_BITS);
	meta_blocks = data_blocks * INSITU_COMP_META_BITS;
	data_blocks *= INSITU_COMP_BLOCK_SIZE * 8;

	cnt = rem;
	rem /= (INSITU_COMP_BLOCK_SIZE * 8 / INSITU_COMP_META_BITS + 1);
	data_blocks += rem * (INSITU_COMP_BLOCK_SIZE * 8 /
				INSITU_COMP_META_BITS);
	meta_blocks += rem;

	cnt %= (INSITU_COMP_BLOCK_SIZE * 8 / INSITU_COMP_META_BITS + 1);
	meta_blocks += 1;
	data_blocks += cnt - 1;

	info->data_blocks = data_blocks;
	info->data_start = (1 + meta_blocks) << INSITU_COMP_BLOCK_SECTOR_SHIFT;

	addr = kzalloc(INSITU_COMP_BLOCK_SIZE, GFP_KERNEL);
	if (!addr) {
		info->ti->error = "Cannot allocate super";
		return -ENOMEM;
	}

	super = addr;
	ret = insitu_comp_access_super(info, addr, READ);
	if (ret)
		goto out;

	if (le64_to_cpu(super->magic) == INSITU_COMP_SUPER_MAGIC) {
		if (le64_to_cpu(super->version) != INSITU_COMP_VERSION ||
		    le64_to_cpu(super->meta_blocks) != meta_blocks ||
		    le64_to_cpu(super->data_blocks) != data_blocks) {
			info->ti->error = "Super is invalid";
			ret = -EINVAL;
			goto out;
		}
		if (!crypto_has_comp(compressors[super->comp_alg].name, 0, 0)) {
			info->ti->error =
					"Compressor algorithm doesn't support";
			ret = -EINVAL;
			goto out;
		}
	} else {
		super->magic = cpu_to_le64(INSITU_COMP_SUPER_MAGIC);
		super->version = cpu_to_le64(INSITU_COMP_VERSION);
		super->meta_blocks = cpu_to_le64(meta_blocks);
		super->data_blocks = cpu_to_le64(data_blocks);
		super->comp_alg = default_compressor;
		ret = insitu_comp_access_super(info, addr, WRITE_FUA);
		if (ret) {
			info->ti->error = "Access super fails";
			goto out;
		}
		new_super = true;
	}

	info->comp_alg = super->comp_alg;
	if (insitu_comp_alloc_compressor(info)) {
		ret = -ENOMEM;
		goto out;
	}

	info->meta_bitmap_bits = data_blocks * INSITU_COMP_META_BITS;
	len = DIV_ROUND_UP_ULL(info->meta_bitmap_bits, BITS_PER_LONG);
	len *= sizeof(unsigned long);
	info->meta_bitmap_pages = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	info->meta_bitmap = vmalloc(info->meta_bitmap_pages * PAGE_SIZE);
	if (!info->meta_bitmap) {
		ret = -ENOMEM;
		goto bitmap_err;
	}

	ret = insitu_comp_init_meta(info, new_super);
	if (ret)
		goto meta_err;

	return 0;
meta_err:
	vfree(info->meta_bitmap);
bitmap_err:
	insitu_comp_free_compressor(info);
out:
	kfree(addr);
	return ret;
}

/*
 * <dev> <writethough>/<writeback> <meta_commit_delay>
 */
static int insitu_comp_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct insitu_comp_info *info;
	char write_mode[15];
	int ret, i;

	if (argc < 2) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		ti->error = "Cannot allocate context";
		return -ENOMEM;
	}
	info->ti = ti;

	if (sscanf(argv[1], "%s", write_mode) != 1) {
		ti->error = "Invalid argument";
		ret = -EINVAL;
		goto err_para;
	}

	if (strcmp(write_mode, "writeback") == 0) {
		if (argc != 3) {
			ti->error = "Invalid argument";
			ret = -EINVAL;
			goto err_para;
		}
		info->write_mode = INSITU_COMP_WRITE_BACK;
		if (sscanf(argv[2], "%u", &info->writeback_delay) != 1) {
			ti->error = "Invalid argument";
			ret = -EINVAL;
			goto err_para;
		}
	} else if (strcmp(write_mode, "writethrough") == 0) {
		info->write_mode = INSITU_COMP_WRITE_THROUGH;
	} else {
		ti->error = "Invalid argument";
		ret = -EINVAL;
		goto err_para;
	}

	if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
							&info->dev)) {
		ti->error = "Can't get device";
		ret = -EINVAL;
		goto err_para;
	}

	info->io_client = dm_io_client_create();
	if (!info->io_client) {
		ti->error = "Can't create io client";
		ret = -EINVAL;
		goto err_ioclient;
	}

	if (bdev_logical_block_size(info->dev->bdev) != 512) {
		ti->error = "Can't logical block size too big";
		ret = -EINVAL;
		goto err_blocksize;
	}

	ret = insitu_comp_read_or_create_super(info);
	if (ret)
		goto err_blocksize;

	for (i = 0; i < BITMAP_HASH_LEN; i++) {
		info->bitmap_locks[i].io_running = 0;
		spin_lock_init(&info->bitmap_locks[i].wait_lock);
		INIT_LIST_HEAD(&info->bitmap_locks[i].wait_list);
	}

	atomic64_set(&info->compressed_write_size, 0);
	atomic64_set(&info->uncompressed_write_size, 0);
	atomic64_set(&info->meta_write_size, 0);
	ti->num_flush_bios = 1;
	/* doesn't support discard yet */
	ti->per_bio_data_size = sizeof(struct insitu_comp_req);
	ti->private = info;
	return 0;
err_blocksize:
	dm_io_client_destroy(info->io_client);
err_ioclient:
	dm_put_device(ti, info->dev);
err_para:
	kfree(info);
	return ret;
}

static void insitu_comp_dtr(struct dm_target *ti)
{
	struct insitu_comp_info *info = ti->private;

	if (info->write_mode == INSITU_COMP_WRITE_BACK)
		kthread_stop(info->writeback_tsk);
	insitu_comp_free_compressor(info);
	vfree(info->meta_bitmap);
	dm_io_client_destroy(info->io_client);
	dm_put_device(ti, info->dev);
	kfree(info);
}

static u64 insitu_comp_sector_to_block(sector_t sect)
{
	return sect >> INSITU_COMP_BLOCK_SECTOR_SHIFT;
}

static struct insitu_comp_hash_lock *
insitu_comp_block_hash_lock(struct insitu_comp_info *info, u64 block_index)
{
	return &info->bitmap_locks[(block_index >> HASH_LOCK_SHIFT) &
			BITMAP_HASH_MASK];
}

static struct insitu_comp_hash_lock *
insitu_comp_trylock_block(struct insitu_comp_info *info,
	struct insitu_comp_req *req, u64 block_index)
{
	struct insitu_comp_hash_lock *hash_lock;

	hash_lock = insitu_comp_block_hash_lock(req->info, block_index);

	spin_lock_irq(&hash_lock->wait_lock);
	if (!hash_lock->io_running) {
		hash_lock->io_running = 1;
		spin_unlock_irq(&hash_lock->wait_lock);
		return hash_lock;
	}
	list_add_tail(&req->sibling, &hash_lock->wait_list);
	spin_unlock_irq(&hash_lock->wait_lock);
	return NULL;
}

static void insitu_comp_queue_req_list(struct insitu_comp_info *info,
	struct list_head *list);
static void insitu_comp_unlock_block(struct insitu_comp_info *info,
	struct insitu_comp_req *req, struct insitu_comp_hash_lock *hash_lock)
{
	LIST_HEAD(pending_list);
	unsigned long flags;

	spin_lock_irqsave(&hash_lock->wait_lock, flags);
	/* wakeup all pending reqs to avoid live lock */
	list_splice_init(&hash_lock->wait_list, &pending_list);
	hash_lock->io_running = 0;
	spin_unlock_irqrestore(&hash_lock->wait_lock, flags);

	insitu_comp_queue_req_list(info, &pending_list);
}

static void insitu_comp_unlock_req_range(struct insitu_comp_req *req)
{
	insitu_comp_unlock_block(req->info, req, req->lock);
}

/* Check comments of HASH_LOCK_SHIFT. each request only need take one lock */
static int insitu_comp_lock_req_range(struct insitu_comp_req *req)
{
	u64 block_index, tmp;

	block_index = insitu_comp_sector_to_block(insitu_req_start_sector(req));
	tmp = insitu_comp_sector_to_block(insitu_req_end_sector(req) - 1);
	BUG_ON(insitu_comp_block_hash_lock(req->info, block_index) !=
			insitu_comp_block_hash_lock(req->info, tmp));

	req->lock = insitu_comp_trylock_block(req->info, req, block_index);
	if (!req->lock)
		return 0;

	return 1;
}

static void insitu_comp_queue_req(struct insitu_comp_info *info,
	struct insitu_comp_req *req)
{
	unsigned long flags;
	struct insitu_comp_io_worker *worker =
		&insitu_comp_io_workers[req->cpu];

	spin_lock_irqsave(&worker->lock, flags);
	list_add_tail(&req->sibling, &worker->pending);
	spin_unlock_irqrestore(&worker->lock, flags);

	queue_work_on(req->cpu, insitu_comp_wq, &worker->work);
}

static void insitu_comp_queue_req_list(struct insitu_comp_info *info,
	struct list_head *list)
{
	struct insitu_comp_req *req;
	while (!list_empty(list)) {
		req = list_first_entry(list, struct insitu_comp_req, sibling);
		list_del_init(&req->sibling);
		insitu_comp_queue_req(info, req);
	}
}

static void insitu_comp_get_req(struct insitu_comp_req *req)
{
	atomic_inc(&req->io_pending);
}

static void insitu_comp_free_io_range(struct insitu_comp_io_range *io)
{
	kfree(io->decomp_data);
	kfree(io->comp_data);
	kmem_cache_free(insitu_comp_io_range_cachep, io);
}

static void insitu_comp_put_req(struct insitu_comp_req *req)
{
	struct insitu_comp_io_range *io;

	if (atomic_dec_return(&req->io_pending))
		return;

	if (req->stage == STAGE_INIT) /* waiting for locking */
		return;

	if (req->stage == STAGE_READ_DECOMP ||
	    req->stage == STAGE_WRITE_COMP ||
	    req->result)
		req->stage = STAGE_DONE;

	if (req->stage != STAGE_DONE) {
		insitu_comp_queue_req(req->info, req);
		return;
	}

	while (!list_empty(&req->all_io)) {
		io = list_entry(req->all_io.next, struct insitu_comp_io_range,
			next);
		list_del(&io->next);
		insitu_comp_free_io_range(io);
	}

	insitu_comp_unlock_req_range(req);

	insitu_req_endio(req, req->result);
}

static void insitu_comp_io_range_done(unsigned long error, void *context)
{
	struct insitu_comp_io_range *io = context;

	if (error)
		io->req->result = error;
	insitu_comp_put_req(io->req);
}

static inline int insitu_comp_compressor_len(struct insitu_comp_info *info,
	int len)
{
	if (compressors[info->comp_alg].comp_len)
		return compressors[info->comp_alg].comp_len(len);
	return len;
}

/*
 * caller should set region.sector, region.count. bi_rw. IO always to/from
 * comp_data
 */
static struct insitu_comp_io_range *
insitu_comp_create_io_range(struct insitu_comp_req *req, int comp_len,
	int decomp_len)
{
	struct insitu_comp_io_range *io;

	io = kmem_cache_alloc(insitu_comp_io_range_cachep, GFP_NOIO);
	if (!io)
		return NULL;

	io->comp_data = kmalloc(insitu_comp_compressor_len(req->info, comp_len),
								GFP_NOIO);
	io->decomp_data = kmalloc(decomp_len, GFP_NOIO);
	if (!io->decomp_data || !io->comp_data) {
		kfree(io->decomp_data);
		kfree(io->comp_data);
		kmem_cache_free(insitu_comp_io_range_cachep, io);
		return NULL;
	}

	io->io_req.notify.fn = insitu_comp_io_range_done;
	io->io_req.notify.context = io;
	io->io_req.client = req->info->io_client;
	io->io_req.mem.type = DM_IO_KMEM;
	io->io_req.mem.ptr.addr = io->comp_data;
	io->io_req.mem.offset = 0;

	io->io_region.bdev = req->info->dev->bdev;

	io->decomp_len = decomp_len;
	io->comp_len = comp_len;
	io->req = req;
	return io;
}

static void insitu_comp_req_copy(struct insitu_comp_req *req, off_t req_off, void *buf,
		ssize_t len, bool to_buf)
{
	struct bio *bio = req->bio;
	struct bvec_iter iter;
	off_t buf_off = 0;
	ssize_t size;
	void *addr;

	iter = bio->bi_iter;
	bio_advance_iter(bio, &iter, req_off);

	while (len) {
		addr = kmap_atomic(bio_iter_page(bio, iter));
		size = min_t(ssize_t, len, bio_iter_len(bio, iter));
		if (to_buf)
			memcpy(buf + buf_off, addr + bio_iter_offset(bio, iter),
				size);
		else
			memcpy(addr + bio_iter_offset(bio, iter), buf + buf_off,
				size);
		kunmap_atomic(addr);

		buf_off += size;
		len -= size;

		bio_advance_iter(bio, &iter, size);
	}
}

/*
 * return value:
 * < 0 : error
 * == 0 : ok
 * == 1 : ok, but comp/decomp is skipped
 * Compressed data size is roundup of 512, which makes the payload.
 * We store the actual compressed length in the last u32 of the payload.
 * If there is no free space, we add 512 to the payload size.
 */
static int insitu_comp_io_range_comp(struct insitu_comp_info *info,
	void *comp_data, unsigned int *comp_len, void *decomp_data,
	unsigned int decomp_len, bool do_comp)
{
	struct crypto_comp *tfm;
	u32 *addr;
	unsigned int actual_comp_len;
	int ret;

	if (do_comp) {
		actual_comp_len = *comp_len;

		tfm = info->tfm[get_cpu()];
		ret = crypto_comp_compress(tfm, decomp_data, decomp_len,
			comp_data, &actual_comp_len);
		put_cpu();

		atomic64_add(decomp_len, &info->uncompressed_write_size);
		if (ret || decomp_len < actual_comp_len + sizeof(u32) + 512) {
			*comp_len = decomp_len;
			atomic64_add(*comp_len, &info->compressed_write_size);
			return 1;
		}

		*comp_len = round_up(actual_comp_len, 512);
		if (*comp_len - actual_comp_len < sizeof(u32))
			*comp_len += 512;
		atomic64_add(*comp_len, &info->compressed_write_size);
		addr = comp_data + *comp_len;
		addr--;
		*addr = cpu_to_le32(actual_comp_len);
	} else {
		if (*comp_len == decomp_len)
			return 1;
		addr = comp_data + *comp_len;
		addr--;
		actual_comp_len = le32_to_cpu(*addr);

		tfm = info->tfm[get_cpu()];
		ret = crypto_comp_decompress(tfm, comp_data, actual_comp_len,
			decomp_data, &decomp_len);
		put_cpu();
		if (ret)
			return -EINVAL;
	}
	return 0;
}

/*
 * compressed data is updated. We decompress it and fill req. If there is no
 * valid compressed data, we just zero req
 */
static void insitu_comp_handle_read_decomp(struct insitu_comp_req *req)
{
	struct insitu_comp_io_range *io;
	off_t req_off = 0;
	int ret;

	req->stage = STAGE_READ_DECOMP;

	if (req->result)
		return;

	list_for_each_entry(io, &req->all_io, next) {
		ssize_t dst_off = 0, src_off = 0, len;

		io->io_region.sector -= req->info->data_start;

		/* Do decomp here */
		ret = insitu_comp_io_range_comp(req->info, io->comp_data,
			&io->comp_len, io->decomp_data, io->decomp_len, false);
		if (ret < 0) {
			req->result = -EIO;
			return;
		}

		if (io->io_region.sector >= insitu_req_start_sector(req))
			dst_off = (io->io_region.sector - insitu_req_start_sector(req))
				<< 9;
		else
			src_off = (insitu_req_start_sector(req) - io->io_region.sector)
				<< 9;
		len = min_t(ssize_t, io->decomp_len - src_off,
			(insitu_req_sectors(req) << 9) - dst_off);

		/* io range in all_io list is ordered for read IO */
		while (req_off != dst_off) {
			ssize_t size = min_t(ssize_t, PAGE_SIZE,
					dst_off - req_off);
			insitu_comp_req_copy(req, req_off,
				empty_zero_page, size, false);
			req_off += size;
		}

		if (ret == 1) /* uncompressed, valid data is in .comp_data */
			insitu_comp_req_copy(req, dst_off,
					io->comp_data + src_off, len, false);
		else
			insitu_comp_req_copy(req, dst_off,
					io->decomp_data + src_off, len, false);
		req_off = dst_off + len;
	}

	while (req_off != (insitu_req_sectors(req) << 9)) {
		ssize_t size = min_t(ssize_t, PAGE_SIZE,
			(insitu_req_sectors(req) << 9) - req_off);
		insitu_comp_req_copy(req, req_off, empty_zero_page,
			size, false);
		req_off += size;
	}
}

/*
 * read one extent data from disk. The extent starts from block @block and has
 * @data_sectors data
 */
static void insitu_comp_read_one_extent(struct insitu_comp_req *req, u64 block,
	u16 logical_sectors, u16 data_sectors)
{
	struct insitu_comp_io_range *io;

	io = insitu_comp_create_io_range(req, data_sectors << 9,
		logical_sectors << 9);
	if (!io) {
		req->result = -EIO;
		return;
	}

	insitu_comp_get_req(req);
	list_add_tail(&io->next, &req->all_io);

	io->io_region.sector = (block << INSITU_COMP_BLOCK_SECTOR_SHIFT) +
				req->info->data_start;
	io->io_region.count = data_sectors;

	io->io_req.bi_rw = READ;
	dm_io(&io->io_req, 1, &io->io_region, NULL);
}

static void insitu_comp_handle_read_read_existing(struct insitu_comp_req *req)
{
	u64 block_index, first_block_index;
	u16 logical_sectors, data_sectors;

	req->stage = STAGE_READ_EXISTING;

	block_index = insitu_comp_sector_to_block(insitu_req_start_sector(req));
again:
	insitu_comp_get_extent(req->info, block_index, &first_block_index,
		&logical_sectors, &data_sectors);
	if (data_sectors > 0)
		insitu_comp_read_one_extent(req, first_block_index,
			logical_sectors, data_sectors);

	if (req->result)
		return;

	block_index = first_block_index + (logical_sectors >>
				INSITU_COMP_BLOCK_SECTOR_SHIFT);
	/* the request might cover several extents */
	if ((block_index << INSITU_COMP_BLOCK_SECTOR_SHIFT) <
			insitu_req_end_sector(req))
		goto again;

	/* A shortcut if all data is in already */
	if (list_empty(&req->all_io))
		insitu_comp_handle_read_decomp(req);
}

static void insitu_comp_handle_read_request(struct insitu_comp_req *req)
{
	insitu_comp_get_req(req);

	if (req->stage == STAGE_INIT) {
		if (!insitu_comp_lock_req_range(req)) {
			insitu_comp_put_req(req);
			return;
		}

		insitu_comp_handle_read_read_existing(req);
	} else if (req->stage == STAGE_READ_EXISTING)
		insitu_comp_handle_read_decomp(req);

	insitu_comp_put_req(req);
}

static void insitu_comp_write_meta_done(void *context, unsigned long error)
{
	struct insitu_comp_req *req = context;
	insitu_comp_put_req(req);
}

static u64 insitu_comp_block_meta_page_index(u64 block, bool end)
{
	u64 bits = block * INSITU_COMP_META_BITS - !!end;
	/* (1 << 3) bits per byte */
	return bits >> (3 + PAGE_SHIFT);
}

/*
 * the request covers some extents partially. Decompress data of the extents,
 * compress remaining valid data, and finally write them out
 */
static int insitu_comp_handle_write_modify(struct insitu_comp_io_range *io,
	u64 *meta_start, u64 *meta_end, bool *handle_req)
{
	struct insitu_comp_req *req = io->req;
	sector_t start, count;
	unsigned int comp_len;
	off_t offset;
	u64 page_index;
	int ret;

	io->io_region.sector -= req->info->data_start;

	/* decompress original data */
	ret = insitu_comp_io_range_comp(req->info, io->comp_data, &io->comp_len,
			io->decomp_data, io->decomp_len, false);
	if (ret < 0) {
		req->result = -EINVAL;
		return -EIO;
	}

	start = io->io_region.sector;
	count = io->decomp_len >> 9;
	if (start < insitu_req_start_sector(req) && start + count >
					insitu_req_end_sector(req)) {
		/* we don't split an extent */
		if (ret == 1) {
			memcpy(io->decomp_data, io->comp_data, io->decomp_len);
			insitu_comp_req_copy(req, 0,
			   io->decomp_data + ((insitu_req_start_sector(req) - start) <<
			   9), insitu_req_sectors(req) << 9, true);
		} else {
			insitu_comp_req_copy(req, 0,
			   io->decomp_data + ((insitu_req_start_sector(req) - start) <<
			   9), insitu_req_sectors(req) << 9, true);
			kfree(io->comp_data);
			/* New compressed len might be bigger */
			io->comp_data = kmalloc(insitu_comp_compressor_len(
				req->info, io->decomp_len), GFP_NOIO);
			io->comp_len = io->decomp_len;
			if (!io->comp_data) {
				req->result = -ENOMEM;
				return -EIO;
			}
			io->io_req.mem.ptr.addr = io->comp_data;
		}
		/* need compress data */
		ret = 0;
		offset = 0;
		*handle_req = false;
	} else if (start < insitu_req_start_sector(req)) {
		count = insitu_req_start_sector(req) - start;
		offset = 0;
	} else {
		offset = insitu_req_end_sector(req) - start;
		start = insitu_req_end_sector(req);
		count = count - offset;
	}

	/* Original data is uncompressed, we don't need writeback */
	if (ret == 1) {
		comp_len = count << 9;
		goto handle_meta;
	}

	/* assume compress less data uses less space (at least 4k lsess data) */
	comp_len = io->comp_len;
	ret = insitu_comp_io_range_comp(req->info, io->comp_data, &comp_len,
		io->decomp_data + (offset << 9), count << 9, true);
	if (ret < 0) {
		req->result = -EIO;
		return -EIO;
	}

	insitu_comp_get_req(req);
	if (ret == 1)
		io->io_req.mem.ptr.addr = io->decomp_data + (offset << 9);
	io->io_region.count = comp_len >> 9;
	io->io_region.sector = start + req->info->data_start;

	io->io_req.bi_rw = insitu_req_rw(req);
	dm_io(&io->io_req, 1, &io->io_region, NULL);
handle_meta:
	insitu_comp_set_extent(req, start >> INSITU_COMP_BLOCK_SECTOR_SHIFT,
		count >> INSITU_COMP_BLOCK_SECTOR_SHIFT, comp_len >> 9);

	page_index = insitu_comp_block_meta_page_index(start >>
					INSITU_COMP_BLOCK_SECTOR_SHIFT, false);
	if (*meta_start > page_index)
		*meta_start = page_index;
	page_index = insitu_comp_block_meta_page_index(
		(start + count) >> INSITU_COMP_BLOCK_SECTOR_SHIFT, true);
	if (*meta_end < page_index)
		*meta_end = page_index;
	return 0;
}

/* Compress data and write it out */
static void insitu_comp_handle_write_comp(struct insitu_comp_req *req)
{
	struct insitu_comp_io_range *io;
	sector_t count;
	unsigned int comp_len;
	u64 meta_start = -1L, meta_end = 0, page_index;
	int ret;
	bool handle_req = true;

	req->stage = STAGE_WRITE_COMP;

	if (req->result)
		return;

	list_for_each_entry(io, &req->all_io, next) {
		if (insitu_comp_handle_write_modify(io, &meta_start, &meta_end,
						&handle_req))
			return;
	}

	if (!handle_req)
		goto update_meta;

	count = insitu_req_sectors(req);
	io = insitu_comp_create_io_range(req, count << 9, count << 9);
	if (!io) {
		req->result = -EIO;
		return;
	}
	insitu_comp_req_copy(req, 0, io->decomp_data, count << 9, true);

	/* compress data */
	comp_len = io->comp_len;
	ret = insitu_comp_io_range_comp(req->info, io->comp_data, &comp_len,
		io->decomp_data, count << 9, true);
	if (ret < 0) {
		insitu_comp_free_io_range(io);
		req->result = -EIO;
		return;
	}

	insitu_comp_get_req(req);
	list_add_tail(&io->next, &req->all_io);
	io->io_region.sector = insitu_req_start_sector(req) + req->info->data_start;
	if (ret == 1)
		io->io_req.mem.ptr.addr = io->decomp_data;
	io->io_region.count = comp_len >> 9;
	io->io_req.bi_rw = insitu_req_rw(req);
	dm_io(&io->io_req, 1, &io->io_region, NULL);
	insitu_comp_set_extent(req,
		insitu_req_start_sector(req) >> INSITU_COMP_BLOCK_SECTOR_SHIFT,
		count >> INSITU_COMP_BLOCK_SECTOR_SHIFT, comp_len >> 9);

	page_index = insitu_comp_block_meta_page_index(
		insitu_req_start_sector(req) >> INSITU_COMP_BLOCK_SECTOR_SHIFT, false);
	if (meta_start > page_index)
		meta_start = page_index;
	page_index = insitu_comp_block_meta_page_index(
		(insitu_req_start_sector(req) + count) >> INSITU_COMP_BLOCK_SECTOR_SHIFT,
		true);
	if (meta_end < page_index)
		meta_end = page_index;
update_meta:
	if (req->info->write_mode == INSITU_COMP_WRITE_THROUGH ||
						(insitu_req_rw(req) & REQ_FUA)) {
		insitu_comp_get_req(req);
		insitu_comp_write_meta(req->info, meta_start, meta_end + 1, req,
			insitu_comp_write_meta_done, insitu_req_rw(req));
	}
}

/* request might cover some extents partially, read them first */
static void insitu_comp_handle_write_read_existing(struct insitu_comp_req *req)
{
	u64 block_index, first_block_index;
	u16 logical_sectors, data_sectors;

	req->stage = STAGE_READ_EXISTING;

	block_index = insitu_comp_sector_to_block(insitu_req_start_sector(req));
	insitu_comp_get_extent(req->info, block_index, &first_block_index,
		&logical_sectors, &data_sectors);
	if (data_sectors > 0 && (first_block_index < block_index ||
	    first_block_index + insitu_comp_sector_to_block(logical_sectors) >
	    insitu_comp_sector_to_block(insitu_req_end_sector(req))))
		insitu_comp_read_one_extent(req, first_block_index,
			logical_sectors, data_sectors);

	if (req->result)
		return;

	if (first_block_index + insitu_comp_sector_to_block(logical_sectors) >=
	    insitu_comp_sector_to_block(insitu_req_end_sector(req)))
		goto out;

	block_index = insitu_comp_sector_to_block(insitu_req_end_sector(req)) - 1;
	insitu_comp_get_extent(req->info, block_index, &first_block_index,
		&logical_sectors, &data_sectors);
	if (data_sectors > 0 &&
	    first_block_index + insitu_comp_sector_to_block(logical_sectors) >
	    block_index + 1)
		insitu_comp_read_one_extent(req, first_block_index,
			logical_sectors, data_sectors);

	if (req->result)
		return;
out:
	if (list_empty(&req->all_io))
		insitu_comp_handle_write_comp(req);
}

static void insitu_comp_handle_write_request(struct insitu_comp_req *req)
{
	insitu_comp_get_req(req);

	if (req->stage == STAGE_INIT) {
		if (!insitu_comp_lock_req_range(req)) {
			insitu_comp_put_req(req);
			return;
		}

		insitu_comp_handle_write_read_existing(req);
	} else if (req->stage == STAGE_READ_EXISTING)
		insitu_comp_handle_write_comp(req);

	insitu_comp_put_req(req);
}

/* For writeback mode */
static void insitu_comp_handle_flush_request(struct insitu_comp_req *req)
{
	struct writeback_flush_data wb;

	atomic_set(&wb.cnt, 1);
	init_completion(&wb.complete);

	insitu_comp_flush_dirty_meta(req->info, &wb);

	writeback_flush_io_done(&wb, 0);
	wait_for_completion(&wb.complete);

	insitu_req_endio(req, 0);
}

static void insitu_comp_handle_request(struct insitu_comp_req *req)
{
	if (insitu_req_rw(req) & REQ_FLUSH)
		insitu_comp_handle_flush_request(req);
	else if (insitu_req_rw(req) & REQ_WRITE)
		insitu_comp_handle_write_request(req);
	else
		insitu_comp_handle_read_request(req);
}

static void insitu_comp_do_request_work(struct work_struct *work)
{
	struct insitu_comp_io_worker *worker = container_of(work,
			struct insitu_comp_io_worker, work);
	LIST_HEAD(list);
	struct insitu_comp_req *req;
	struct blk_plug plug;
	bool repeat;

	blk_start_plug(&plug);
again:
	spin_lock_irq(&worker->lock);
	list_splice_init(&worker->pending, &list);
	spin_unlock_irq(&worker->lock);

	repeat = !list_empty(&list);
	while (!list_empty(&list)) {
		req = list_first_entry(&list, struct insitu_comp_req, sibling);
		list_del(&req->sibling);

		insitu_comp_handle_request(req);
	}
	if (repeat)
		goto again;
	blk_finish_plug(&plug);
}

static int insitu_comp_map(struct dm_target *ti, struct bio *bio)
{
	struct insitu_comp_info *info = ti->private;
	struct insitu_comp_req *req;

	req = dm_per_bio_data(bio, sizeof(struct insitu_comp_req));

	if ((bio->bi_rw & REQ_FLUSH) &&
			info->write_mode == INSITU_COMP_WRITE_THROUGH) {
		bio->bi_bdev = info->dev->bdev;
		return DM_MAPIO_REMAPPED;
	}

	req->bio = bio;
	req->info = info;
	atomic_set(&req->io_pending, 0);
	INIT_LIST_HEAD(&req->all_io);
	req->result = 0;
	req->stage = STAGE_INIT;

	req->cpu = raw_smp_processor_id();
	insitu_comp_queue_req(info, req);

	return DM_MAPIO_SUBMITTED;
}

/*
 * INFO: uncompressed_data_size compressed_data_size metadata_size
 * TABLE: writethrough/writeback commit_delay
 */
static void insitu_comp_status(struct dm_target *ti, status_type_t type,
			  unsigned status_flags, char *result, unsigned maxlen)
{
	struct insitu_comp_info *info = ti->private;
	unsigned int sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%lu %lu %lu",
			atomic64_read(&info->uncompressed_write_size),
			atomic64_read(&info->compressed_write_size),
			atomic64_read(&info->meta_write_size));
		break;
	case STATUSTYPE_TABLE:
		if (info->write_mode == INSITU_COMP_WRITE_BACK)
			DMEMIT("%s %s %d", info->dev->name, "writeback",
				info->writeback_delay);
		else
			DMEMIT("%s %s", info->dev->name, "writethrough");
		break;
	}
}

static int insitu_comp_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	struct insitu_comp_info *info = ti->private;

	return fn(ti, info->dev, info->data_start,
		info->data_blocks << INSITU_COMP_BLOCK_SECTOR_SHIFT, data);
}

static void insitu_comp_io_hints(struct dm_target *ti,
			    struct queue_limits *limits)
{
	/* No blk_limits_logical_block_size */
	limits->logical_block_size = limits->physical_block_size =
		limits->io_min = INSITU_COMP_BLOCK_SIZE;
	blk_limits_max_hw_sectors(limits, INSITU_COMP_MAX_SIZE >> 9);
}

static int insitu_comp_merge(struct dm_target *ti, struct bvec_merge_data *bvm,
			struct bio_vec *biovec, int max_size)
{
	/* Guarantee request can only cover one aligned 128k range */
	return min_t(int, max_size, INSITU_COMP_MAX_SIZE - bvm->bi_size -
			((bvm->bi_sector << 9) % INSITU_COMP_MAX_SIZE));
}

static struct target_type insitu_comp_target = {
	.name   = "insitu_comp",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr    = insitu_comp_ctr,
	.dtr    = insitu_comp_dtr,
	.map    = insitu_comp_map,
	.status = insitu_comp_status,
	.iterate_devices = insitu_comp_iterate_devices,
	.io_hints = insitu_comp_io_hints,
	.merge = insitu_comp_merge,
};

static int __init insitu_comp_init(void)
{
	int r;

	for (r = 0; r < ARRAY_SIZE(compressors); r++)
		if (crypto_has_comp(compressors[r].name, 0, 0))
			break;
	if (r >= ARRAY_SIZE(compressors)) {
		DMWARN("No crypto compressors are supported");
		return -EINVAL;
	}

	default_compressor = r;

	r = -ENOMEM;
	insitu_comp_io_range_cachep = kmem_cache_create("insitu_comp_io_range",
		sizeof(struct insitu_comp_io_range), 0, 0, NULL);
	if (!insitu_comp_io_range_cachep) {
		DMWARN("Can't create io_range cache");
		goto err;
	}

	insitu_comp_meta_io_cachep = kmem_cache_create("insitu_comp_meta_io",
		sizeof(struct insitu_comp_meta_io), 0, 0, NULL);
	if (!insitu_comp_meta_io_cachep) {
		DMWARN("Can't create meta_io cache");
		goto err;
	}

	insitu_comp_wq = alloc_workqueue("insitu_comp_io",
		WQ_UNBOUND|WQ_MEM_RECLAIM|WQ_CPU_INTENSIVE, 0);
	if (!insitu_comp_wq) {
		DMWARN("Can't create io workqueue");
		goto err;
	}

	r = dm_register_target(&insitu_comp_target);
	if (r < 0) {
		DMWARN("target registration failed");
		goto err;
	}

	for_each_possible_cpu(r) {
		INIT_LIST_HEAD(&insitu_comp_io_workers[r].pending);
		spin_lock_init(&insitu_comp_io_workers[r].lock);
		INIT_WORK(&insitu_comp_io_workers[r].work,
			insitu_comp_do_request_work);
	}
	return 0;
err:
	if (insitu_comp_io_range_cachep)
		kmem_cache_destroy(insitu_comp_io_range_cachep);
	if (insitu_comp_meta_io_cachep)
		kmem_cache_destroy(insitu_comp_meta_io_cachep);
	if (insitu_comp_wq)
		destroy_workqueue(insitu_comp_wq);

	return r;
}

static void __exit insitu_comp_exit(void)
{
	dm_unregister_target(&insitu_comp_target);
	kmem_cache_destroy(insitu_comp_io_range_cachep);
	kmem_cache_destroy(insitu_comp_meta_io_cachep);
	destroy_workqueue(insitu_comp_wq);
}

module_init(insitu_comp_init);
module_exit(insitu_comp_exit);

MODULE_AUTHOR("Shaohua Li <shli@kernel.org>");
MODULE_DESCRIPTION(DM_NAME " target with insitu data compression for SSD");
MODULE_LICENSE("GPL");
