// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for REQ_NOWAIT on IOCB_NOWAIT iomap direct reads.
 *
 * The block layer never splits a REQ_NOWAIT bio, so a NOWAIT read keeps
 * REQ_NOWAIT only when the whole read is one bio that fits one request of
 * its device (blk-mq) or of a member (dm, md): no longer than a request of
 * its queue, not crossing its chunk boundary (counted from the start of the
 * disk) and inside one extent.  Other reads are issued blocking, as on
 * stock.  Writes are unchanged.
 */
#include <kunit/test.h>
#include <linux/blkdev.h>
#include <linux/iomap.h>
#include <linux/module.h>
#include <linux/sizes.h>

struct dio_nowait_dev {
	struct block_device bdev;
	struct request_queue q;
};

static struct block_device *dio_nowait_bdev(struct kunit *test, bool bio_based,
					    unsigned int max_sectors,
					    unsigned int chunk_sectors,
					    sector_t start_sect)
{
	struct dio_nowait_dev *dev = kunit_kzalloc(test, sizeof(*dev),
						   GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dev);
	dev->q.limits.max_sectors = max_sectors;
	dev->q.limits.chunk_sectors = chunk_sectors;
	dev->bdev.bd_queue = &dev->q;
	dev->bdev.bd_start_sect = start_sect;
	if (bio_based)
		atomic_set(&dev->bdev.__bd_flags, BD_HAS_SUBMIT_BIO);
	return &dev->bdev;
}

/* Identity mapping: file offset @pos is byte @pos of @bdev. */
static bool dio_nowait_extent(struct block_device *bdev, bool is_write,
			      loff_t pos, u64 len, u64 extent)
{
	struct iomap_iter iter = {
		.pos = pos,
		.len = len,
		.iomap = {
			.bdev = bdev,
			.type = IOMAP_MAPPED,
			.length = extent,
		},
	};

	return iomap_dio_kunit_read_fits_one_request(&iter, is_write);
}

static bool dio_nowait(struct block_device *bdev, bool is_write, loff_t pos,
		       u64 len)
{
	return dio_nowait_extent(bdev, is_write, pos, len, SZ_1G);
}

static void dio_nowait_dm_fits_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, true, 256, 0, 0);

	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, 0, SZ_128K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, SZ_1M, SZ_4K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, 0, 512));
}

/* The measured defect: dm-stripe over NVMe with max_sectors_kb=128. */
static void dio_nowait_dm_too_long_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, true, 256, 0, 0);

	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, SZ_1M));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, SZ_128K + 512));
}

/* 512 KiB chunk, queue limit 1 MiB. */
static void dio_nowait_dm_chunk_cross_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, true, 2048, 1024, 0);

	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, SZ_1M));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, SZ_4K, SZ_512K));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, SZ_512K - SZ_4K,
					    SZ_64K));
}

static void dio_nowait_dm_chunk_inside_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, true, 2048, 1024, 0);

	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, 0, SZ_512K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, SZ_512K, SZ_64K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, SZ_512K - SZ_4K, SZ_4K));
}

/* A partition starting 4 KiB into the disk shifts the chunk boundary. */
static void dio_nowait_partition_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, true, 2048, 1024, 8);

	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, SZ_512K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, 0, SZ_512K - SZ_4K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, SZ_512K - SZ_4K,
					   SZ_512K));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, SZ_512K - SZ_4K,
					    SZ_512K + 512));
}

/* 192 KiB chunk: not a power of two. */
static void dio_nowait_odd_chunk_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, true, 2048, 384, 0);

	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, 0, 3 * SZ_64K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, 3 * SZ_64K, 3 * SZ_64K));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, SZ_128K, SZ_128K));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, 3 * SZ_64K + 512));
}

/* A read spanning two extents needs two bios. */
static void dio_nowait_multi_extent_test(struct kunit *test)
{
	struct block_device *dm = dio_nowait_bdev(test, true, 2048, 0, 0);
	struct block_device *mq = dio_nowait_bdev(test, false, 2048, 0, 0);

	KUNIT_EXPECT_FALSE(test, dio_nowait_extent(dm, false, 0, SZ_128K,
						   SZ_64K));
	KUNIT_EXPECT_TRUE(test, dio_nowait_extent(dm, false, 0, SZ_64K,
						  SZ_64K));
	KUNIT_EXPECT_FALSE(test, dio_nowait_extent(mq, false, 0, SZ_128K,
						   SZ_64K));
	KUNIT_EXPECT_TRUE(test, dio_nowait_extent(mq, false, 0, SZ_64K,
						  SZ_64K));
}

static void dio_nowait_write_test(struct kunit *test)
{
	struct block_device *dm = dio_nowait_bdev(test, true, 256, 1024, 8);
	struct block_device *mq = dio_nowait_bdev(test, false, 256, 1024, 8);

	KUNIT_EXPECT_TRUE(test, dio_nowait(dm, true, SZ_4K, SZ_1M));
	KUNIT_EXPECT_TRUE(test, dio_nowait_extent(dm, true, 0, SZ_1M, SZ_64K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(mq, true, SZ_4K, SZ_1M));
}

/*
 * md RAID0 publishes its chunk as both max_hw_sectors and chunk_sectors
 * (raid0_set_limits()): 512 KiB chunk over members with a 1 MiB limit.
 */
static void dio_nowait_md_raid0_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, true, 1024, 1024, 0);

	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, SZ_1M));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, SZ_256K, SZ_512K));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, SZ_512K - SZ_4K,
					    SZ_64K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, 0, SZ_512K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, SZ_512K, SZ_64K));
}

/* The same array over members with max_sectors_kb=128. */
static void dio_nowait_md_raid0_member_limit_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, true, 256, 1024, 0);

	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, SZ_1M));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, SZ_256K));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, SZ_512K - SZ_4K,
					    SZ_64K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, SZ_512K - SZ_4K, SZ_4K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, SZ_512K, SZ_128K));
}

/* A queue limit of 1 MiB keeps NOWAIT for a 1 MiB read. */
static void dio_nowait_mq_fits_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, false, 2048, 0, 0);

	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, 0, SZ_1M));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, SZ_4K, SZ_4K));
}

/* The measured defect: plain NVMe with max_sectors_kb=128. */
static void dio_nowait_mq_too_long_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, false, 256, 0, 0);

	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, SZ_1M));
	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, SZ_128K + 512));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, 0, SZ_128K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, SZ_1M, SZ_4K));
}

/* A device chunk (NVMe NOIOB) on a partition starting 4 KiB in. */
static void dio_nowait_mq_partition_chunk_test(struct kunit *test)
{
	struct block_device *b = dio_nowait_bdev(test, false, 2048, 1024, 8);

	KUNIT_EXPECT_FALSE(test, dio_nowait(b, false, 0, SZ_512K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, 0, SZ_512K - SZ_4K));
	KUNIT_EXPECT_TRUE(test, dio_nowait(b, false, SZ_512K - SZ_4K,
					   SZ_512K));
}

static void dio_nowait_no_bdev_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, dio_nowait(NULL, false, 0, SZ_1M));
	KUNIT_EXPECT_TRUE(test, dio_nowait(NULL, true, 0, SZ_1M));
}

static struct kunit_case dio_nowait_test_cases[] = {
	KUNIT_CASE(dio_nowait_dm_fits_test),
	KUNIT_CASE(dio_nowait_dm_too_long_test),
	KUNIT_CASE(dio_nowait_dm_chunk_cross_test),
	KUNIT_CASE(dio_nowait_dm_chunk_inside_test),
	KUNIT_CASE(dio_nowait_partition_test),
	KUNIT_CASE(dio_nowait_odd_chunk_test),
	KUNIT_CASE(dio_nowait_multi_extent_test),
	KUNIT_CASE(dio_nowait_write_test),
	KUNIT_CASE(dio_nowait_md_raid0_test),
	KUNIT_CASE(dio_nowait_md_raid0_member_limit_test),
	KUNIT_CASE(dio_nowait_mq_fits_test),
	KUNIT_CASE(dio_nowait_mq_too_long_test),
	KUNIT_CASE(dio_nowait_mq_partition_chunk_test),
	KUNIT_CASE(dio_nowait_no_bdev_test),
	{}
};

static struct kunit_suite dio_nowait_test_suite = {
	.name = "iomap-dio-nowait",
	.test_cases = dio_nowait_test_cases,
};

kunit_test_suite(dio_nowait_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for REQ_NOWAIT on iomap direct reads");
MODULE_LICENSE("GPL");
