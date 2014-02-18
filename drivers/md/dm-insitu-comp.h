#ifndef __DM_INSITU_COMPRESSION_H__
#define __DM_INSITU_COMPRESSION_H__
#include <linux/types.h>

struct insitu_comp_super_block {
	__le64 magic;
	__le64 version;
	__le64 meta_blocks;
	__le64 data_blocks;
	u8 comp_alg;
} __attribute__((packed));

#define INSITU_COMP_SUPER_MAGIC 0x106526c206506c09
#define INSITU_COMP_VERSION 1
#define INSITU_COMP_ALG_LZO 0
#define INSITU_COMP_ALG_ZLIB 1

#ifdef __KERNEL__
struct insitu_comp_compressor_data {
	char *name;
	int (*comp_len)(int comp_len);
};

static inline int lzo_comp_len(int comp_len)
{
	return lzo1x_worst_compress(comp_len);
}

/*
 * Minium logical sector size of this target is 4096 byte, which is a block.
 * Data of a block is compressed. Compressed data is round up to 512B, which is
 * the payload. For each block, we have 5 bits meta data. bit 0 - 3 stands
 * payload length. 0 - 8 sectors. If compressed payload length is 8 sectors, we
 * just store uncompressed data. Actual compressed data length is stored at the
 * last 32 bits of payload if data is compressed. In disk, payload is stored at
 * the begining of logical sector of the block. If IO size is bigger than one
 * block, we store the whole data as an extent. Bit 4 stands tail for an
 * extent. Max allowed extent size is 128k.
 */
#define INSITU_COMP_BLOCK_SIZE 4096
#define INSITU_COMP_BLOCK_SHIFT 12
#define INSITU_COMP_BLOCK_SECTOR_SHIFT (INSITU_COMP_BLOCK_SHIFT - 9)

#define INSITU_COMP_MIN_SIZE 4096
/* Change this should change HASH_LOCK_SHIFT too */
#define INSITU_COMP_MAX_SIZE (128 * 1024)

#define INSITU_COMP_LENGTH_MASK ((1 << 4) - 1)
#define INSITU_COMP_TAIL_MASK (1 << 4)
#define INSITU_COMP_META_BITS 5

#define INSITU_COMP_META_START_SECTOR (INSITU_COMP_BLOCK_SIZE >> 9)

enum INSITU_COMP_WRITE_MODE {
	INSITU_COMP_WRITE_BACK,
	INSITU_COMP_WRITE_THROUGH,
};

/*
 * request can cover one aligned 128k (4k * (1 << 5)) range. Since maxium
 * request size is 128k, we only need take one lock for each request
 */
#define HASH_LOCK_SHIFT 5

#define BITMAP_HASH_SHIFT 9
#define BITMAP_HASH_MASK ((1 << BITMAP_HASH_SHIFT) - 1)
#define BITMAP_HASH_LEN (1 << BITMAP_HASH_SHIFT)

struct insitu_comp_hash_lock {
	int io_running;
	spinlock_t wait_lock;
	struct list_head wait_list;
};

struct insitu_comp_info {
	struct dm_target *ti;
	struct dm_dev *dev;

	int comp_alg;
	struct crypto_comp *tfm[NR_CPUS];

	sector_t data_start;
	u64 data_blocks;

	char *meta_bitmap;
	u64 meta_bitmap_bits;
	u64 meta_bitmap_pages;
	struct insitu_comp_hash_lock bitmap_locks[BITMAP_HASH_LEN];

	enum INSITU_COMP_WRITE_MODE write_mode;
	unsigned int writeback_delay; /* second unit */
	struct task_struct *writeback_tsk;
	struct dm_io_client *io_client;

	atomic64_t compressed_write_size;
	atomic64_t uncompressed_write_size;
	atomic64_t meta_write_size;
};

struct insitu_comp_meta_io {
	struct dm_io_request io_req;
	struct dm_io_region io_region;
	void *data;
	void (*fn)(void *data, unsigned long error);
};

struct insitu_comp_io_range {
	struct dm_io_request io_req;
	struct dm_io_region io_region;
	void *decomp_data;
	unsigned int decomp_len;
	void *comp_data;
	unsigned int comp_len; /* For write, this is estimated */
	struct list_head next;
	struct insitu_comp_req *req;
};

enum INSITU_COMP_REQ_STAGE {
	STAGE_INIT,
	STAGE_READ_EXISTING,
	STAGE_READ_DECOMP,
	STAGE_WRITE_COMP,
	STAGE_DONE,
};

struct insitu_comp_req {
	struct bio *bio;
	struct insitu_comp_info *info;
	struct list_head sibling;

	struct list_head all_io;
	atomic_t io_pending;
	enum INSITU_COMP_REQ_STAGE stage;

	struct insitu_comp_hash_lock *lock;
	int result;

	int cpu;
};

#define insitu_req_start_sector(req) (req->bio->bi_iter.bi_sector)
#define insitu_req_end_sector(req) (bio_end_sector(req->bio))
#define insitu_req_rw(req) (req->bio->bi_rw)
#define insitu_req_sectors(req) (bio_sectors(req->bio))

static inline void insitu_req_endio(struct insitu_comp_req *req, int error)
{
	bio_endio(req->bio, error);
}

struct insitu_comp_io_worker {
	struct list_head pending;
	spinlock_t lock;
	struct work_struct work;
};
#endif

#endif
