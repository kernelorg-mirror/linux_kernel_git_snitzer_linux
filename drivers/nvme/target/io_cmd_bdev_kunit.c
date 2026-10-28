// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for which lent bvecs (nvmet_req_set_data_bvec()) a block
 * device namespace takes, and the bios the block layer cuts from them.
 * Included from io-cmd-bdev.c so that the static helpers are reachable.
 */
#include <kunit/test.h>

#define NVMET_BDEV_KUNIT_PAGES	16
/* a TCP segment's worth: not a multiple of any logical block */
#define NVMET_BDEV_KUNIT_MSS	1448

struct nvmet_bdev_kunit {
	struct nvmet_ns		ns;
	struct nvmet_req	req;
	struct queue_limits	lim;
	struct bio_vec		bvec[NVMET_BDEV_KUNIT_PAGES];
	struct page		*pages;
};

static void nvmet_bdev_kunit_free(void *data)
{
	__free_pages(data, ilog2(NVMET_BDEV_KUNIT_PAGES));
}

static int nvmet_bdev_kunit_init(struct kunit *test)
{
	struct nvmet_bdev_kunit *k = kunit_kzalloc(test, sizeof(*k),
						   GFP_KERNEL);

	if (!k)
		return -ENOMEM;
	k->pages = alloc_pages(GFP_KERNEL, ilog2(NVMET_BDEV_KUNIT_PAGES));
	if (!k->pages)
		return -ENOMEM;
	if (kunit_add_action_or_reset(test, nvmet_bdev_kunit_free, k->pages))
		return -ENOMEM;

	/* an SGL device: 4 KiB blocks, 4-byte DMA alignment, as nullb0 */
	k->lim.logical_block_size = 4096;
	k->lim.physical_block_size = 4096;
	k->lim.dma_alignment = 3;
	k->lim.max_segments = 4;
	k->lim.max_segment_size = SZ_64K;
	k->lim.max_fast_segment_size = PAGE_SIZE;
	k->lim.seg_boundary_mask = BLK_SEG_BOUNDARY_MASK;
	k->lim.max_sectors = SZ_64K >> SECTOR_SHIFT;

	k->req.ns = &k->ns;
	k->req.data_bvec_ok = true;
	test->priv = k;
	return 0;
}

/*
 * @n segments of @len bytes (the last one short to make up @total), each
 * at @offset in a page of its own.
 */
static unsigned int nvmet_bdev_kunit_bvec(struct nvmet_bdev_kunit *k,
		unsigned int len, unsigned int offset, unsigned int total)
{
	unsigned int n = 0, done = 0;

	while (done < total && n < NVMET_BDEV_KUNIT_PAGES) {
		unsigned int l = min(len, total - done);

		bvec_set_page(&k->bvec[n], k->pages + n, l, offset);
		done += l;
		n++;
	}
	k->req.transfer_len = total;
	return n;
}

/*
 * Cut the bio over the lent bvec as the queue would: every cut a logical
 * block multiple at the right sector, none a geometry error.  Returns the
 * number of cuts.
 */
static unsigned int nvmet_bdev_kunit_split(struct kunit *test,
		struct nvmet_bdev_kunit *k)
{
	struct bio bio;
	unsigned int done = 0, cuts = 0;
	sector_t sector = 8;

	nvmet_bdev_init_data_bvec_bio(&k->req, &bio, REQ_OP_WRITE, sector);
	while (bio.bi_iter.bi_size) {
		unsigned int segs, bytes;
		int split = bio_split_io_at(&bio, &k->lim, &segs, SZ_64K,
					    k->lim.dma_alignment);

		KUNIT_ASSERT_GE(test, split, 0);
		bytes = split ? split << SECTOR_SHIFT : bio.bi_iter.bi_size;
		KUNIT_EXPECT_EQ(test, bytes % k->lim.logical_block_size, 0);
		KUNIT_EXPECT_LE(test, segs, k->lim.max_segments);
		KUNIT_EXPECT_EQ(test, bio.bi_iter.bi_sector,
				sector + (done >> SECTOR_SHIFT));
		bio_advance(&bio, bytes);
		done += bytes;
		cuts++;
	}
	KUNIT_EXPECT_EQ(test, done, k->req.transfer_len);
	bio_uninit(&bio);
	return cuts;
}

/*
 * Segments of a TCP segment's length, DMA aligned, are taken, and the
 * block layer cuts the bio over them into logical block multiples at the
 * right sectors, though no segment is one.
 */
static void nvmet_bdev_unaligned_lengths_test(struct kunit *test)
{
	struct nvmet_bdev_kunit *k = test->priv;
	unsigned int n;

	nvmet_bdev_set_data_bvec(&k->ns, &k->lim);
	n = nvmet_bdev_kunit_bvec(k, NVMET_BDEV_KUNIT_MSS, 64, SZ_16K);
	KUNIT_ASSERT_TRUE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));
	/* four segments to a bio, so one block at a time */
	KUNIT_EXPECT_EQ(test, nvmet_bdev_kunit_split(test, k), 4);
}

/* a segment over several blocks counts once in each */
static void nvmet_bdev_multi_block_test(struct kunit *test)
{
	struct nvmet_bdev_kunit *k = test->priv;

	nvmet_bdev_set_data_bvec(&k->ns, &k->lim);
	bvec_set_page(&k->bvec[0], k->pages, 6144, 0);
	bvec_set_page(&k->bvec[1], k->pages + 2, 2048, 0);
	bvec_set_page(&k->bvec[2], k->pages + 3, 8192, 0);
	k->req.transfer_len = SZ_16K;

	/* block 1 is touched by two segments */
	k->lim.max_segments = 2;
	KUNIT_ASSERT_TRUE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, 3));
	nvmet_bdev_kunit_split(test, k);
	k->lim.max_segments = 1;
	KUNIT_EXPECT_FALSE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, 3));
}

/* refused only for what the queue itself would refuse */
static void nvmet_bdev_refused_test(struct kunit *test)
{
	struct nvmet_bdev_kunit *k = test->priv;
	unsigned int n;

	nvmet_bdev_set_data_bvec(&k->ns, &k->lim);
	/* an offset off the DMA alignment */
	n = nvmet_bdev_kunit_bvec(k, NVMET_BDEV_KUNIT_MSS, 2, SZ_16K);
	KUNIT_EXPECT_FALSE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));
	/* a length off it */
	n = nvmet_bdev_kunit_bvec(k, NVMET_BDEV_KUNIT_MSS + 2, 64, SZ_16K);
	KUNIT_EXPECT_FALSE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));

	/* a block touched by four segments, where a bio may have three */
	n = nvmet_bdev_kunit_bvec(k, NVMET_BDEV_KUNIT_MSS, 64, SZ_16K);
	k->lim.max_segments = 3;
	KUNIT_EXPECT_FALSE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));
	k->lim.max_segments = 4;

	/* a segment the queue would take as two, not one just within it */
	k->lim.max_segment_size = NVMET_BDEV_KUNIT_MSS - 4;
	KUNIT_EXPECT_FALSE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));
	k->lim.max_segment_size = NVMET_BDEV_KUNIT_MSS;
	KUNIT_EXPECT_TRUE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));
	k->lim.max_segment_size = SZ_64K;
	k->lim.seg_boundary_mask = 4095;
	n = nvmet_bdev_kunit_bvec(k, 4096, 0, SZ_16K);
	KUNIT_EXPECT_TRUE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));
	n = nvmet_bdev_kunit_bvec(k, 4096, 2048, SZ_16K);
	KUNIT_EXPECT_FALSE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));
	k->lim.seg_boundary_mask = BLK_SEG_BOUNDARY_MASK;
	KUNIT_EXPECT_TRUE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));

	/* none on a zoned device */
	k->lim.features |= BLK_FEAT_ZONED;
	nvmet_bdev_set_data_bvec(&k->ns, &k->lim);
	KUNIT_EXPECT_FALSE(test, k->ns.data_bvec_ok);
}

/* behind a virtual boundary a gap may fall only on a block boundary */
static void nvmet_bdev_virt_boundary_test(struct kunit *test)
{
	struct nvmet_bdev_kunit *k = test->priv;
	unsigned int n;

	k->lim.virt_boundary_mask = 4095;
	nvmet_bdev_set_data_bvec(&k->ns, &k->lim);
	n = nvmet_bdev_kunit_bvec(k, NVMET_BDEV_KUNIT_MSS, 0, SZ_16K);
	KUNIT_EXPECT_FALSE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));

	/* header split: mid-page start, whole pages, mid-page end */
	bvec_set_page(&k->bvec[0], k->pages, 3584, 512);
	bvec_set_page(&k->bvec[1], k->pages + 1, 4096, 0);
	bvec_set_page(&k->bvec[2], k->pages + 2, 4096, 0);
	bvec_set_page(&k->bvec[3], k->pages + 3, 2048, 0);
	k->req.transfer_len = 3584 + 4096 + 4096 + 2048;
	k->lim.logical_block_size = 512;
	KUNIT_ASSERT_TRUE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, 4));
	nvmet_bdev_kunit_split(test, k);

	/* a gap at 2048: on a 512-byte block boundary, not a 4 KiB one */
	bvec_set_page(&k->bvec[0], k->pages, 2048, 0);
	bvec_set_page(&k->bvec[1], k->pages + 1, 2048, 0);
	k->req.transfer_len = SZ_4K;
	KUNIT_ASSERT_TRUE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, 2));
	KUNIT_EXPECT_EQ(test, nvmet_bdev_kunit_split(test, k), 2);
	k->lim.logical_block_size = 4096;
	KUNIT_EXPECT_FALSE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, 2));

	/* the same where the gap is only in the next segment's offset */
	bvec_set_page(&k->bvec[0], k->pages, 2048, 2048);
	bvec_set_page(&k->bvec[1], k->pages + 1, 2048, 1024);
	KUNIT_EXPECT_FALSE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, 2));
	k->lim.logical_block_size = 512;
	KUNIT_ASSERT_TRUE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, 2));
	KUNIT_EXPECT_EQ(test, nvmet_bdev_kunit_split(test, k), 2);
}

/*
 * A direct I/O file namespace with no queue to go by (or a file system
 * offset alignment above its device's block) takes only segments meeting
 * the offset alignment.
 */
static void nvmet_bdev_file_no_queue_test(struct kunit *test)
{
	struct nvmet_bdev_kunit *k = test->priv;
	unsigned int n;

	k->ns.data_bvec_addr_mask = 3;
	k->ns.data_bvec_len_mask = 3 | (4096 - 1);
	k->ns.data_bvec_lim = NULL;
	n = nvmet_bdev_kunit_bvec(k, NVMET_BDEV_KUNIT_MSS, 64, SZ_16K);
	KUNIT_EXPECT_FALSE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));
	n = nvmet_bdev_kunit_bvec(k, 4096, 64, SZ_16K);
	KUNIT_EXPECT_TRUE(test, nvmet_req_set_data_bvec(&k->req, k->bvec, n));
}

static struct kunit_case nvmet_bdev_data_bvec_cases[] = {
	KUNIT_CASE(nvmet_bdev_unaligned_lengths_test),
	KUNIT_CASE(nvmet_bdev_multi_block_test),
	KUNIT_CASE(nvmet_bdev_refused_test),
	KUNIT_CASE(nvmet_bdev_virt_boundary_test),
	KUNIT_CASE(nvmet_bdev_file_no_queue_test),
	{}
};

static struct kunit_suite nvmet_bdev_data_bvec_suite = {
	.name = "nvmet-bdev-data-bvec",
	.init = nvmet_bdev_kunit_init,
	.test_cases = nvmet_bdev_data_bvec_cases,
};
kunit_test_suite(nvmet_bdev_data_bvec_suite);
