// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/errseq.h>
#include <linux/file.h>
#include <linux/iomap.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/bvec.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nfs3.h>
#include <linux/nfs_dio.h>
#include <linux/pagemap.h>
#include <linux/sched/task.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/xdr.h>
#include <linux/uio.h>
#include <net/checksum.h>

#include "cache.h"
#include "nfsd.h"
#include "vfs.h"
#include "xdr3.h"
#include "filecache.h"

#define NFSD_BVEC_Q20_PAYLOAD_LEN	300U
#define NFSD_BVEC_Q20_HEAD_WORDS	7U
#define NFSD_BVEC_Q20_HEAD_LEN		(NFSD_BVEC_Q20_HEAD_WORDS * XDR_UNIT)

struct nfsd_bvec_wire_fixture {
	__be32			head[NFSD_BVEC_Q20_HEAD_WORDS];
	u8			payload[NFSD_BVEC_Q20_PAYLOAD_LEN];
	struct page		*legacy_page;
	struct page		*pages[2];
	struct bio_vec		bvec[2];
	struct xdr_buf		legacy;
	struct xdr_buf		authoritative;
};

struct nfsd_bvec_decode_context {
	struct svc_xprt_class	xprt_class;
	struct svc_xprt	xprt;
	struct svc_serv		serv;
	struct svc_rqst		rqst;
	struct xdr_stream	stream;
};

static void nfsd_bvec_free_page(void *data)
{
	__free_page(data);
}

static struct page *nfsd_bvec_alloc_page(struct kunit *test)
{
	struct page *page = alloc_page(GFP_KERNEL);

	if (!page)
		return NULL;
	if (kunit_add_action_or_reset(test, nfsd_bvec_free_page, page))
		return NULL;
	return page;
}

static void nfsd_bvec_write(struct page *page, unsigned int offset,
			    const void *source, unsigned int length)
{
	void *address = kmap_local_page(page);

	memcpy(address + offset, source, length);
	kunmap_local(address);
}

static void nfsd_bvec_init_buf(struct xdr_buf *buf, void *head,
			       unsigned int head_len, unsigned int page_len)
{
	memset(buf, 0, sizeof(*buf));
	buf->head[0].iov_base = head;
	buf->head[0].iov_len = head_len;
	buf->page_len = page_len;
	buf->len = head_len + page_len;
	buf->buflen = buf->len;
}

static struct nfsd_bvec_wire_fixture *
nfsd_bvec_wire_fixture_alloc(struct kunit *test, unsigned int odd_split)
{
	struct nfsd_bvec_wire_fixture *fixture;
	__be32 *p;
	unsigned int i;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_GT(test, odd_split, 0U);
	KUNIT_ASSERT_LT(test, odd_split, NFSD_BVEC_Q20_PAYLOAD_LEN);

	for (i = 0; i < ARRAY_SIZE(fixture->payload); i++)
		fixture->payload[i] = (u8)(i * 29U + 7U);

	p = fixture->head;
	*p++ = cpu_to_be32(XDR_UNIT);
	*p++ = cpu_to_be32(0x10203040);
	*p++ = cpu_to_be32(0x01020304);
	*p++ = cpu_to_be32(0x05060708);
	*p++ = cpu_to_be32(NFSD_BVEC_Q20_PAYLOAD_LEN);
	*p++ = cpu_to_be32(NFS_UNSTABLE);
	*p = cpu_to_be32(NFSD_BVEC_Q20_PAYLOAD_LEN);

	fixture->legacy_page = nfsd_bvec_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture->legacy_page);
	nfsd_bvec_write(fixture->legacy_page, 13, fixture->payload,
			NFSD_BVEC_Q20_PAYLOAD_LEN);
	nfsd_bvec_init_buf(&fixture->legacy, fixture->head,
			   NFSD_BVEC_Q20_HEAD_LEN,
			   NFSD_BVEC_Q20_PAYLOAD_LEN);
	fixture->legacy.pages = &fixture->legacy_page;
	fixture->legacy.page_base = 13;
	fixture->legacy.page_mode = XDRBUF_PAGE_ARRAY;

	fixture->pages[0] = nfsd_bvec_alloc_page(test);
	fixture->pages[1] = nfsd_bvec_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture->pages[0]);
	KUNIT_ASSERT_NOT_NULL(test, fixture->pages[1]);
	bvec_set_page(&fixture->bvec[0], fixture->pages[0], odd_split, 17);
	bvec_set_page(&fixture->bvec[1], fixture->pages[1],
		      NFSD_BVEC_Q20_PAYLOAD_LEN - odd_split, 31);
	nfsd_bvec_write(fixture->pages[0], 17, fixture->payload, odd_split);
	nfsd_bvec_write(fixture->pages[1], 31,
			fixture->payload + odd_split,
			NFSD_BVEC_Q20_PAYLOAD_LEN - odd_split);
	nfsd_bvec_init_buf(&fixture->authoritative, fixture->head,
			   NFSD_BVEC_Q20_HEAD_LEN,
			   NFSD_BVEC_Q20_PAYLOAD_LEN);
	fixture->authoritative.bvec = fixture->bvec;
	fixture->authoritative.bvec_count = ARRAY_SIZE(fixture->bvec);
	fixture->authoritative.page_mode = XDRBUF_PAGE_BVECS;

	return fixture;
}

static void
nfsd_bvec_decode_context_init(struct nfsd_bvec_decode_context *context,
			      struct xdr_buf *buf,
			      struct nfsd3_writeargs *args)
{
	memset(context, 0, sizeof(*context));
	context->xprt_class.xcl_max_payload = PAGE_SIZE;
	context->xprt.xpt_class = &context->xprt_class;
	context->serv.sv_max_payload = PAGE_SIZE;
	context->rqst.rq_xprt = &context->xprt;
	context->rqst.rq_server = &context->serv;
	context->rqst.rq_argp = args;
	xdr_init_decode(&context->stream, buf, buf->head[0].iov_base, NULL);
}

static bool
nfsd_bvec_decode_write(struct nfsd_bvec_decode_context *context)
{
	return nfs3svc_decode_writeargs(&context->rqst, &context->stream);
}

static void nfsd_bvec_decode_checksum_parity_test(struct kunit *test)
{
	static const unsigned int odd_splits[] = { 1, 3, 5 };
	__be32 scratch[RC_CSUMLEN / sizeof(__be32)];
	unsigned int case_index;

	for (case_index = 0; case_index < ARRAY_SIZE(odd_splits);
	     case_index++) {
		struct nfsd_bvec_decode_context *authoritative_context;
		struct nfsd_bvec_decode_context *legacy_context;
		struct nfsd_bvec_wire_fixture *fixture;
		struct nfsd3_writeargs *authoritative_args;
		struct nfsd3_writeargs *legacy_args;
		const void *authoritative_fh;
		const void *legacy_fh;
		u8 *authoritative_payload;
		u8 *checksum_prefix;
		u8 *legacy_payload;
		size_t fh_size;
		size_t payload_len;
		__wsum actual;
		__wsum expected;
		bool decoded;

		kunit_info(test, "odd split %u", odd_splits[case_index]);
		fixture = nfsd_bvec_wire_fixture_alloc(test,
						       odd_splits[case_index]);
		legacy_context = kunit_kzalloc(test, sizeof(*legacy_context), GFP_KERNEL);
		authoritative_context = kunit_kzalloc(test,
						      sizeof(*authoritative_context),
						      GFP_KERNEL);
		legacy_args = kunit_kzalloc(test, sizeof(*legacy_args), GFP_KERNEL);
		authoritative_args = kunit_kzalloc(test, sizeof(*authoritative_args),
						   GFP_KERNEL);
		legacy_payload = kunit_kmalloc(test, NFSD_BVEC_Q20_PAYLOAD_LEN, GFP_KERNEL);
		authoritative_payload = kunit_kmalloc(test,
						      NFSD_BVEC_Q20_PAYLOAD_LEN,
						      GFP_KERNEL);
		checksum_prefix = kunit_kmalloc(test, RC_CSUMLEN, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, legacy_context);
		KUNIT_ASSERT_NOT_NULL(test, authoritative_context);
		KUNIT_ASSERT_NOT_NULL(test, legacy_args);
		KUNIT_ASSERT_NOT_NULL(test, authoritative_args);
		KUNIT_ASSERT_NOT_NULL(test, legacy_payload);
		KUNIT_ASSERT_NOT_NULL(test, authoritative_payload);
		KUNIT_ASSERT_NOT_NULL(test, checksum_prefix);

		nfsd_bvec_decode_context_init(legacy_context, &fixture->legacy,
					      legacy_args);
		decoded = nfsd_bvec_decode_write(legacy_context);
		KUNIT_ASSERT_TRUE(test, decoded);
		nfsd_bvec_decode_context_init(authoritative_context,
					      &fixture->authoritative,
					      authoritative_args);
		decoded = nfsd_bvec_decode_write(authoritative_context);
		KUNIT_ASSERT_TRUE(test, decoded);

		KUNIT_EXPECT_EQ(test, authoritative_args->offset,
				legacy_args->offset);
		KUNIT_EXPECT_EQ(test, authoritative_args->count,
				legacy_args->count);
		KUNIT_EXPECT_EQ(test, authoritative_args->stable,
				legacy_args->stable);
		KUNIT_EXPECT_EQ(test, authoritative_args->len, legacy_args->len);
		KUNIT_EXPECT_EQ(test, authoritative_args->fh.fh_handle.fh_size,
				legacy_args->fh.fh_handle.fh_size);
		authoritative_fh = &authoritative_args->fh.fh_handle.fh_raw;
		legacy_fh = &legacy_args->fh.fh_handle.fh_raw;
		fh_size = legacy_args->fh.fh_handle.fh_size;
		KUNIT_EXPECT_MEMEQ(test, authoritative_fh, legacy_fh, fh_size);

		KUNIT_ASSERT_EQ(test,
				read_bytes_from_xdr_buf(&legacy_args->payload, 0,
							legacy_payload,
							legacy_args->len),
				0);
		KUNIT_ASSERT_EQ(test,
				read_bytes_from_xdr_buf(&authoritative_args->payload,
							0,
							authoritative_payload,
							authoritative_args->len),
				0);
		payload_len = sizeof(fixture->payload);
		KUNIT_EXPECT_MEMEQ(test, legacy_payload, fixture->payload, payload_len);
		KUNIT_EXPECT_MEMEQ(test, authoritative_payload, legacy_payload, payload_len);

		memcpy(checksum_prefix, fixture->head, NFSD_BVEC_Q20_HEAD_LEN);
		memcpy(checksum_prefix + NFSD_BVEC_Q20_HEAD_LEN,
		       fixture->payload, RC_CSUMLEN - NFSD_BVEC_Q20_HEAD_LEN);
		expected = csum_partial(checksum_prefix, RC_CSUMLEN, 0);
		actual = nfsd_cache_csum(&fixture->legacy, 0,
					 fixture->legacy.len, scratch);
		KUNIT_EXPECT_EQ(test, (__force u32)actual, (__force u32)expected);
		actual = nfsd_cache_csum(&fixture->authoritative, 0,
					 fixture->authoritative.len, scratch);
		KUNIT_EXPECT_EQ(test, (__force u32)actual, (__force u32)expected);
	}
}

static void nfsd_bvec_decode_error_test(struct kunit *test)
{
	struct nfsd_bvec_decode_context *context;
	struct nfsd_bvec_wire_fixture *fixture;
	struct nfsd3_writeargs *args;
	struct xdr_buf short_buf;
	bool decoded;

	fixture = nfsd_bvec_wire_fixture_alloc(test, 3);
	context = kunit_kzalloc(test, sizeof(*context), GFP_KERNEL);
	args = kunit_kzalloc(test, sizeof(*args), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, context);
	KUNIT_ASSERT_NOT_NULL(test, args);

	/*
	 * The selected public decoder rejects an out-of-range stable_how;
	 * keep that provider behavior explicit while checking bvec errors.
	 */
	fixture->head[5] = cpu_to_be32(NFS_FILE_SYNC + 1);
	nfsd_bvec_decode_context_init(context, &fixture->authoritative, args);
	decoded = nfs3svc_decode_writeargs(&context->rqst, &context->stream);
	KUNIT_EXPECT_FALSE(test, decoded);
	fixture->head[5] = cpu_to_be32(NFS_UNSTABLE);

	memset(args, 0, sizeof(*args));
	fixture->head[4] = cpu_to_be32(NFSD_BVEC_Q20_PAYLOAD_LEN - 1);
	nfsd_bvec_decode_context_init(context, &fixture->legacy, args);
	decoded = nfs3svc_decode_writeargs(&context->rqst, &context->stream);
	KUNIT_EXPECT_FALSE(test, decoded);
	fixture->head[4] = cpu_to_be32(NFSD_BVEC_Q20_PAYLOAD_LEN);

	memset(args, 0, sizeof(*args));
	short_buf = fixture->authoritative;
	short_buf.page_len--;
	short_buf.len--;
	short_buf.buflen--;
	nfsd_bvec_decode_context_init(context, &short_buf, args);
	decoded = nfs3svc_decode_writeargs(&context->rqst, &context->stream);
	KUNIT_EXPECT_FALSE(test, decoded);
}


struct nfsd_bvec_dio_case {
	const char	*name;
	loff_t		position;
	unsigned long	total;
	u32		mem_align;
	u32		offset_align;
	u32		seg_boundary;
	fop_flags_t	fop_flags;
	unsigned int	nvecs;
	unsigned int	offsets[4];
	unsigned int	lengths[4];
	unsigned int	expected_nsegs;
	unsigned int	expected_direct_mask;
	unsigned int	expected_dontcache_mask;
	unsigned long	expected_counts[3];
};

static void nfsd_bvec_dio_segments_test(struct kunit *test)
{
	static const struct nfsd_bvec_dio_case cases[] = {
		{
			/*
			 * The fixed geometries below are whole pages, so that on any
			 * page size the write has a middle and the memory checks are
			 * what decides it: a write that holds no whole page is
			 * buffered before they are consulted.
			 */
			.name = "aligned-first-middle-last",
			.total = 2 * PAGE_SIZE,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { PAGE_SIZE, PAGE_SIZE - 512, 512 },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			.name = "later-offset-misaligned",
			.total = 2 * PAGE_SIZE,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 3,
			.offsets = { 0, 1, 0 },
			.lengths = { PAGE_SIZE, PAGE_SIZE - 512, 512 },
			.expected_nsegs = 1,
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			.name = "later-length-misaligned",
			.total = 2 * PAGE_SIZE,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { PAGE_SIZE, PAGE_SIZE - 513, 513 },
			.expected_nsegs = 1,
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			/*
			 * A misaligned write whose direct middle is smaller
			 * than the misaligned-split gate (one page, under a
			 * gate of two) must not be split: one buffered segment.
			 */
			.name = "small-middle-below-misaligned-gate",
			.position = 128,
			.total = 2 * PAGE_SIZE,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 3,
			.offsets = { 128, 0, 0 },
			.lengths = { PAGE_SIZE - 128, PAGE_SIZE, 128 },
			.expected_nsegs = 1,
			.expected_counts = { 2 * PAGE_SIZE },
		},		{
			/*
			 * Both ends misaligned: each buffered end runs to the
			 * page boundary inside the write, not to the 512-byte
			 * one, so the middle is whole pages.  Two pages are at
			 * the misaligned-split gate, so the write is split.
			 */
			.name = "buffered-prefix-direct-middle-buffered-suffix",
			.position = 128,
			.total = 3 * PAGE_SIZE + 512,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 4,
			.offsets = { 128, 0, 0, 0 },
			.lengths = { PAGE_SIZE - 128, PAGE_SIZE, PAGE_SIZE, 640 },
			.expected_nsegs = 3,
			.expected_direct_mask = BIT(1),
			.expected_counts = { PAGE_SIZE - 128, 2 * PAGE_SIZE, 640 },
		},
		{
			.name = "no-memory-alignment",
			.total = 4096,
			.offset_align = 512,
			.nvecs = 1,
			.lengths = { 4096 },
			.expected_nsegs = 1,
			.expected_counts = { 4096 },
		},
		{
			.name = "no-memory-alignment-dontcache",
			.total = 4096,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 1,
			.lengths = { 4096 },
			.expected_nsegs = 1,
			.expected_dontcache_mask = BIT(0),
			.expected_counts = { 4096 },
		},
		{
			.name = "later-offset-misaligned-dontcache",
			.total = 2 * PAGE_SIZE,
			.mem_align = 512,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 3,
			.offsets = { 0, 1, 0 },
			.lengths = { PAGE_SIZE, PAGE_SIZE - 512, 512 },
			.expected_nsegs = 1,
			.expected_dontcache_mask = BIT(0),
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			/*
			 * Interior discontinuities (fragments ending mid-page)
			 * at logical-block-aligned payload positions: the
			 * block stack can split there, so direct I/O stands.
			 * mem_align is smaller than offset_align, as on NVMe
			 * (dma_alignment=3, lbs=512).
			 */
			.name = "discontinuity-aligned-direct",
			.total = 2 * PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { PAGE_SIZE - 512, 512, PAGE_SIZE },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			/*
			 * A discontinuity four bytes short of a page is not at
			 * a logical-block multiple: a forced split there would
			 * strand a sub-sector residue and the block stack
			 * would reject the bio, so the gate must demote the
			 * segment to buffered.
			 */
			.name = "discontinuity-misaligned-buffered",
			.total = 2 * PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { PAGE_SIZE - 4, 4, PAGE_SIZE },
			.expected_nsegs = 1,
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			.name = "discontinuity-misaligned-dontcache",
			.total = 2 * PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 3,
			.lengths = { PAGE_SIZE - 4, 4, PAGE_SIZE },
			.expected_nsegs = 1,
			.expected_dontcache_mask = BIT(0),
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			.name = "short-write",
			.total = 511,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 1,
			.lengths = { 511 },
			.expected_nsegs = 1,
			.expected_counts = { 511 },
		},
		{
			/*
			 * A device with no segment boundary takes the mid-page
			 * joint that discontinuity-misaligned-buffered demotes.
			 */
			.name = "no-boundary-mid-page-joint-direct",
			.total = 2 * PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE,
			.nvecs = 3,
			.lengths = { PAGE_SIZE - 4, 4, PAGE_SIZE },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			/* TCP-segment geometry: fragments start mid-page */
			.name = "no-boundary-tcp-segments-direct",
			.total = PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE,
			.nvecs = 3,
			.offsets = { 0, 2048, 512 },
			.lengths = { 1448, 1448, PAGE_SIZE - 2896 },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { PAGE_SIZE },
		},
		{
			.name = "default-boundary-tcp-segments-buffered",
			.total = PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 512,
			.nvecs = 3,
			.offsets = { 0, 2048, 512 },
			.lengths = { 1448, 1448, PAGE_SIZE - 2896 },
			.expected_nsegs = 1,
			.expected_counts = { PAGE_SIZE },
		},
		{
			/* no boundary does not relax memory alignment */
			.name = "no-boundary-2-byte-fragment-buffered",
			.total = 2 * PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE,
			.nvecs = 3,
			.lengths = { PAGE_SIZE - 2, 2, PAGE_SIZE },
			.expected_nsegs = 1,
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			/*
			 * Joints on 4 KiB multiples, the first at payload byte
			 * 3996, not a logical-block multiple: a 4096-byte
			 * boundary admits them on every page size.
			 */
			.name = "boundary-4096-joint-on-boundary-direct",
			.total = 2 * PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = 4096,
			.nvecs = 3,
			.offsets = { 100, 0, 0 },
			.lengths = { 3996, PAGE_SIZE, PAGE_SIZE - 3996 },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			/* the page-sized default admits them only on 4 KiB pages */
			.name = "default-boundary-joint-on-4096",
			.total = 2 * PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 512,
			.nvecs = 3,
			.offsets = { 100, 0, 0 },
			.lengths = { 3996, PAGE_SIZE, PAGE_SIZE - 3996 },
			.expected_nsegs = 1,
			.expected_direct_mask = PAGE_SIZE == 4096 ? BIT(0) : 0,
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			.name = "boundary-4096-mid-page-joint-buffered",
			.total = 2 * PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = 4096,
			.nvecs = 3,
			.lengths = { PAGE_SIZE - 4, 4, PAGE_SIZE },
			.expected_nsegs = 1,
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			/*
			 * A boundary larger than a page makes the page joint
			 * of a page-tiled payload a gap, at a position that is
			 * not an offset_align multiple.
			 */
			.name = "boundary-above-page-page-tiled-buffered",
			.total = 2 * PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 2 * PAGE_SIZE,
			.seg_boundary = 2 * PAGE_SIZE,
			.nvecs = 2,
			.lengths = { PAGE_SIZE, PAGE_SIZE },
			.expected_nsegs = 1,
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			.name = "default-boundary-page-tiled-direct",
			.total = 2 * PAGE_SIZE,
			.mem_align = 4,
			.offset_align = 2 * PAGE_SIZE,
			.nvecs = 2,
			.lengths = { PAGE_SIZE, PAGE_SIZE },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 2 * PAGE_SIZE },
		},
		{
			/*
			 * The prefix runs to the page boundary, so the middle
			 * starts PAGE_SIZE - 128 bytes into bvec[0], whose
			 * mid-page end is a joint at middle byte 100, not an
			 * offset_align multiple: no boundary admits it.
			 */
			.name = "skipped-first-no-boundary-mid-page-joint-direct",
			.position = 128,
			.total = 3 * PAGE_SIZE - 28,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE,
			.nvecs = 3,
			.lengths = { PAGE_SIZE - 28, PAGE_SIZE, PAGE_SIZE },
			.expected_nsegs = 3,
			.expected_direct_mask = BIT(1),
			.expected_counts = { PAGE_SIZE - 128, 2 * PAGE_SIZE, 100 },
		},
		{
			.name = "skipped-first-boundary-4096-mid-page-joint-buffered",
			.position = 128,
			.total = 3 * PAGE_SIZE - 28,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = 4096,
			.nvecs = 3,
			.lengths = { PAGE_SIZE - 28, PAGE_SIZE, PAGE_SIZE },
			.expected_nsegs = 1,
			.expected_counts = { 3 * PAGE_SIZE - 28 },
		},
		{
			.name = "skipped-first-default-boundary-mid-page-joint-buffered",
			.position = 128,
			.total = 3 * PAGE_SIZE - 28,
			.mem_align = 4,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { PAGE_SIZE - 28, PAGE_SIZE, PAGE_SIZE },
			.expected_nsegs = 1,
			.expected_counts = { 3 * PAGE_SIZE - 28 },
		},
		{
			/*
			 * bvec[0] (offset 100) ends on a page boundary, a
			 * 4 KiB multiple on any page size, so its joint at
			 * middle byte 28 is no gap under a 4096-byte boundary;
			 * counting the PAGE_SIZE - 128 skipped bytes into its
			 * end would make it one.
			 */
			.name = "skipped-first-boundary-4096-joint-on-boundary-direct",
			.position = 128,
			.total = 3 * PAGE_SIZE - 100,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = 4096,
			.nvecs = 3,
			.offsets = { 100, 0, 0 },
			.lengths = { PAGE_SIZE - 100, PAGE_SIZE, PAGE_SIZE },
			.expected_nsegs = 3,
			.expected_direct_mask = BIT(1),
			.expected_counts = { PAGE_SIZE - 128, 2 * PAGE_SIZE, 28 },
		},
		{
			/*
			 * A misaligned start with DONTCACHE: the prefix is cut
			 * at the page boundary and claims its shared page.
			 * The direct middle carries DONTCACHE too, for a file
			 * system that falls back to buffered I/O.
			 */
			.name = "dontcache-misaligned-start-cut-at-page",
			.position = 128,
			.total = 3 * PAGE_SIZE - 128,
			.mem_align = 512,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 3,
			.offsets = { 128, 0, 0 },
			.lengths = { PAGE_SIZE - 128, PAGE_SIZE, PAGE_SIZE },
			.expected_nsegs = 2,
			.expected_direct_mask = BIT(1),
			.expected_dontcache_mask = BIT(0) | BIT(1),
			.expected_counts = { PAGE_SIZE - 128, 2 * PAGE_SIZE },
		},
		{
			/* The cut does not depend on DONTCACHE. */
			.name = "cached-misaligned-start-cut-at-page",
			.position = 128,
			.total = 3 * PAGE_SIZE - 128,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 3,
			.offsets = { 128, 0, 0 },
			.lengths = { PAGE_SIZE - 128, PAGE_SIZE, PAGE_SIZE },
			.expected_nsegs = 2,
			.expected_direct_mask = BIT(1),
			.expected_counts = { PAGE_SIZE - 128, 2 * PAGE_SIZE },
		},
		{
			/* A misaligned end: the suffix starts on a page boundary. */
			.name = "dontcache-misaligned-end-cut-at-page",
			.total = 2 * PAGE_SIZE + 640,
			.mem_align = 512,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 3,
			.lengths = { PAGE_SIZE, PAGE_SIZE, 640 },
			.expected_nsegs = 2,
			.expected_direct_mask = BIT(0),
			.expected_dontcache_mask = BIT(0) | BIT(1),
			.expected_counts = { 2 * PAGE_SIZE, 640 },
		},
		{
			.name = "cached-misaligned-end-cut-at-page",
			.total = 2 * PAGE_SIZE + 640,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { PAGE_SIZE, PAGE_SIZE, 640 },
			.expected_nsegs = 2,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 2 * PAGE_SIZE, 640 },
		},
		{
			/*
			 * Both ends are offset_align multiples and neither is
			 * on a page boundary.  An aligned end needs no buffered
			 * segment, so a write aligned to the device is direct
			 * throughout, whatever the page size.
			 */
			.name = "dontcache-aligned-ends-mid-page-all-direct",
			.position = 512,
			.total = 1024,
			.mem_align = 512,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 1,
			.offsets = { 512 },
			.lengths = { 1024 },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_dontcache_mask = BIT(0),
			.expected_counts = { 1024 },
		},
		{
			/*
			 * Each end is cut on its own: the aligned mid-page start
			 * has no prefix and the middle starts there, while the
			 * misaligned end is cut back to the page boundary.
			 */
			.name = "dontcache-aligned-start-misaligned-end",
			.position = 512,
			.total = 3 * PAGE_SIZE + 128,
			.mem_align = 512,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 4,
			.offsets = { 512, 0, 0, 0 },
			.lengths = { PAGE_SIZE - 512, PAGE_SIZE, PAGE_SIZE, 640 },
			.expected_nsegs = 2,
			.expected_direct_mask = BIT(0),
			.expected_dontcache_mask = BIT(0) | BIT(1),
			.expected_counts = { 3 * PAGE_SIZE - 512, 640 },
		},
		{
			/*
			 * An aligned mid-page end has no suffix: the middle runs
			 * to it.  This write shares the page it ends in with a
			 * neighbour that may be buffered there, which is why the
			 * caller must not issue it for asynchronous completion,
			 * see nfs_dio_write_shares_page().
			 */
			.name = "dontcache-aligned-mid-page-end-direct",
			.total = 2 * PAGE_SIZE + 512,
			.mem_align = 512,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 3,
			.lengths = { PAGE_SIZE, PAGE_SIZE, 512 },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_dontcache_mask = BIT(0),
			.expected_counts = { 2 * PAGE_SIZE + 512 },
		},
		{
			/*
			 * A misaligned write with no whole page inside it: the
			 * two cuts meet, there is no middle, and the single
			 * buffered segment is exactly the write.
			 */
			.name = "dontcache-misaligned-no-whole-page",
			.position = 128,
			.total = PAGE_SIZE,
			.mem_align = 512,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 2,
			.offsets = { 128, 0 },
			.lengths = { PAGE_SIZE - 128, 128 },
			.expected_nsegs = 1,
			.expected_dontcache_mask = BIT(0),
			.expected_counts = { PAGE_SIZE },
		},
	};
	unsigned int case_index;

	for (case_index = 0; case_index < ARRAY_SIZE(cases); case_index++) {
		const struct nfsd_bvec_dio_case *test_case = &cases[case_index];
		struct nfs_dio_split split = {};
		struct nfs_dio_seg *segments = split.segs;
		struct bio_vec bvec[4] = {};
		/*
		 * The split consults file->f_op->fop_flags for FOP_DONTCACHE,
		 * so the file must carry real file_operations.
		 */
		const struct file_operations fake_fops = {
			.fop_flags = test_case->fop_flags,
		};
		/* Its size: no test write starts beyond EOF. */
		static struct inode fake_inode = {
			.i_size = MAX_LFS_FILESIZE,
		};
		struct file fake_file = {
			.f_inode = &fake_inode,
			.f_op = &fake_fops,
		};
		/*
		 * The policy nfsd_write_dio_iters_init() hands the split for
		 * this file with direct_misaligned_min_bytes at two pages, its
		 * default on 4 KiB pages, and direct_misaligned_dontcache set.
		 */
		const struct nfs_dio_policy policy = {
			.mem_align = test_case->mem_align,
			.offset_align = test_case->offset_align,
			.seg_boundary = test_case->seg_boundary,
			.min_middle_bytes = 2 * PAGE_SIZE,
			.dontcache = true,
		};
		unsigned long consumed = 0;
		unsigned int segment_index;
		unsigned int vector_index;
		unsigned int nvecs = test_case->nvecs;
		unsigned int nsegs;
		unsigned long total = test_case->total;
		u8 *actual;
		u8 *source;

		kunit_info(test, "%s", test_case->name);
		source = kunit_kmalloc(test, test_case->total, GFP_KERNEL);
		actual = kunit_kmalloc(test, test_case->total, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, source);
		KUNIT_ASSERT_NOT_NULL(test, actual);
		for (vector_index = 0; vector_index < test_case->total;
		     vector_index++)
			source[vector_index] = (u8)(vector_index * 17U + 3U);

		for (vector_index = 0; vector_index < test_case->nvecs;
		     vector_index++) {
			struct page *page = nfsd_bvec_alloc_page(test);

			KUNIT_ASSERT_NOT_NULL(test, page);
			bvec_set_page(&bvec[vector_index], page,
				      test_case->lengths[vector_index],
				      test_case->offsets[vector_index]);
			nfsd_bvec_write(page, test_case->offsets[vector_index],
					source + consumed,
					test_case->lengths[vector_index]);
			consumed += test_case->lengths[vector_index];
		}
		KUNIT_ASSERT_EQ(test, consumed, test_case->total);

		nfs_dio_split(&fake_file, &policy, ITER_SOURCE, bvec, nvecs,
			      test_case->position, total, IOCB_DSYNC, &split);
		nsegs = split.nsegs;
		KUNIT_EXPECT_EQ(test, nsegs, test_case->expected_nsegs);
		if (nsegs != test_case->expected_nsegs)
			continue;

		/*
		 * With DONTCACHE in effect, a buffered segment claims the
		 * pages it shares with the neighbouring writes
		 * (nfs_dio_seg_hold()), split or not: a split WRITE's
		 * ends are cut at page boundaries, so its prefix
		 * and suffix always can.  The direct middle never does.
		 */
		for (segment_index = 0; segment_index < nsegs;
		     segment_index++) {
			bool buffered_dontcache =
				!(segments[segment_index].flags & IOCB_DIRECT) &&
				(test_case->fop_flags & FOP_DONTCACHE);

			KUNIT_EXPECT_EQ(test, segments[segment_index].claim,
					buffered_dontcache);
		}

		consumed = 0;
		for (segment_index = 0; segment_index < nsegs;
		     segment_index++) {
			struct iov_iter iter = segments[segment_index].iter;
			unsigned long expected =
				test_case->expected_counts[segment_index];
			bool direct;
			bool expected_direct;
			bool dontcache;
			bool expected_dontcache;
			int base_flags;
			size_t copied;

			KUNIT_EXPECT_EQ(test, iov_iter_count(&iter), expected);
			direct = segments[segment_index].flags & IOCB_DIRECT;
			expected_direct = test_case->expected_direct_mask &
					  BIT(segment_index);
			KUNIT_EXPECT_EQ(test, direct, expected_direct);
			dontcache = segments[segment_index].flags &
				    IOCB_DONTCACHE;
			expected_dontcache = test_case->expected_dontcache_mask &
					     BIT(segment_index);
			KUNIT_EXPECT_EQ(test, dontcache, expected_dontcache);
			/*
			 * A buffered segment that claims its shared pages
			 * completes them itself (IOCB_DONTCACHE_DEFER); the
			 * direct middle never does.
			 */
			base_flags = segments[segment_index].flags &
				     ~(IOCB_DIRECT | IOCB_DONTCACHE);
			KUNIT_EXPECT_EQ(test, base_flags,
					IOCB_DSYNC |
					(!direct && dontcache ?
					 IOCB_DONTCACHE_DEFER : 0));
			copied = copy_from_iter(actual, expected, &iter);
			KUNIT_ASSERT_EQ(test, copied, expected);
			KUNIT_EXPECT_MEMEQ(test, actual, source + consumed, expected);
			consumed += expected;
		}
		KUNIT_EXPECT_EQ(test, consumed, test_case->total);
	}
}

static void nfsd_bvec_dio_seg_boundary_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs_dio_seg_boundary(false, 0), 0U);
	KUNIT_EXPECT_EQ(test, nfs_dio_seg_boundary(false, 4096), 0U);
	KUNIT_EXPECT_EQ(test, nfs_dio_seg_boundary(true, 0),
			NFS_DIO_SEG_BOUNDARY_NONE);
	KUNIT_EXPECT_EQ(test, nfs_dio_seg_boundary(true, 4096), 4096U);
	KUNIT_EXPECT_EQ(test, nfs_dio_seg_boundary(true, 65536), 65536U);
	KUNIT_EXPECT_EQ(test, nfs_dio_seg_boundary(true, 3000), 0U);

	/* what the split enforces, and the tracepoint reports */
	KUNIT_EXPECT_EQ(test, nfs_dio_joint_boundary(0), (u32)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test,
			nfs_dio_joint_boundary(NFS_DIO_SEG_BOUNDARY_NONE), 0U);
	KUNIT_EXPECT_EQ(test, nfs_dio_joint_boundary(4096), 4096U);
	KUNIT_EXPECT_EQ(test, nfs_dio_joint_boundary(2 * PAGE_SIZE),
			(u32)(2 * PAGE_SIZE));
}

/*
 * Which of its dispositions the split reports for a payload refused direct
 * I/O, as the nfsd_write_dio_* tracepoints show it: a fragment that breaks
 * the memory alignment is NFS_DIO_MEM_MISALIGNED, an interior gap at an
 * offset that is not an offset_align multiple is NFS_DIO_GAP_MISALIGNED, on
 * a device whose segment boundary makes it a gap at all.
 */
static void nfsd_bvec_dio_disposition_test(struct kunit *test)
{
	static const struct {
		const char	*name;
		u32		seg_boundary;
		unsigned int	nvecs;
		unsigned int	lengths[3];
		enum nfs_dio_disposition expected;
	} cases[] = {
		{ "contiguous", 0, 2, { PAGE_SIZE, PAGE_SIZE },
		  NFS_DIO_DIRECT },
		{ "fragment-off-mem-align", 0, 3,
		  { PAGE_SIZE, PAGE_SIZE - 100, 100 }, NFS_DIO_MEM_MISALIGNED },
		{ "gap-off-offset-align", 0, 3,
		  { PAGE_SIZE, 512, PAGE_SIZE - 512 }, NFS_DIO_GAP_MISALIGNED },
		{ "gap-on-offset-align", 0, 3,
		  { PAGE_SIZE, 1024, PAGE_SIZE - 1024 }, NFS_DIO_DIRECT },
		{ "gap-off-offset-align-no-boundary", NFS_DIO_SEG_BOUNDARY_NONE,
		  3, { PAGE_SIZE, 512, PAGE_SIZE - 512 }, NFS_DIO_DIRECT },
	};
	static const struct file_operations fake_fops;
	static struct inode fake_inode = {
		.i_size = MAX_LFS_FILESIZE,
	};
	struct file fake_file = {
		.f_inode = &fake_inode,
		.f_op = &fake_fops,
	};
	unsigned int case_index;

	for (case_index = 0; case_index < ARRAY_SIZE(cases); case_index++) {
		const struct nfs_dio_policy policy = {
			.mem_align = 512,
			.offset_align = 1024,
			.seg_boundary = cases[case_index].seg_boundary,
		};
		struct nfs_dio_split split = {};
		struct bio_vec bvec[3] = {};
		unsigned int vector_index;
		bool direct;

		kunit_info(test, "%s", cases[case_index].name);
		for (vector_index = 0; vector_index < cases[case_index].nvecs;
		     vector_index++) {
			struct page *page = nfsd_bvec_alloc_page(test);

			KUNIT_ASSERT_NOT_NULL(test, page);
			bvec_set_page(&bvec[vector_index], page,
				      cases[case_index].lengths[vector_index], 0);
		}

		nfs_dio_split(&fake_file, &policy, ITER_SOURCE, bvec,
			      cases[case_index].nvecs, 0, 2 * PAGE_SIZE, 0,
			      &split);
		KUNIT_EXPECT_EQ(test, split.disposition,
				cases[case_index].expected);
		KUNIT_ASSERT_EQ(test, split.nsegs, 1U);
		direct = split.segs[0].flags & IOCB_DIRECT;
		KUNIT_EXPECT_EQ(test, direct,
				cases[case_index].expected == NFS_DIO_DIRECT);
		KUNIT_EXPECT_EQ(test, iov_iter_count(&split.segs[0].iter),
				2 * PAGE_SIZE);
	}
}

static void nfsd_bvec_proc_dispatch_parity_test(struct kunit *test)
{
	static const struct svc_procedure procedures[2] = {
		[1] = {
			.pc_argzero = sizeof(u32),
			.pc_ressize = sizeof(u32),
			.pc_xdr_bvec = true,
		},
	};
	static const struct svc_version version = {
		.vs_vers = 3,
		.vs_nproc = ARRAY_SIZE(procedures),
		.vs_proc = procedures,
	};
	static const struct svc_version *versions[4] = {
		[3] = &version,
	};
	static const struct svc_program program = {
		.pg_nvers = ARRAY_SIZE(versions),
		.pg_vers = versions,
	};
	static const struct svc_program empty_program;
	static const struct {
		const struct svc_program *program;
		u32 version;
		u32 procedure;
		__be32 expected;
		const struct svc_procedure *expected_procedure;
	} cases[] = {
		{ &program, 3, 1, rpc_success, &procedures[1] },
		{ &empty_program, 0, 0, rpc_prog_mismatch, NULL },
		{ &program, 2, 0, rpc_prog_mismatch, NULL },
		{ &program, 4, 0, rpc_prog_mismatch, NULL },
		{ &program, 3, 2, rpc_proc_unavail, NULL },
	};
	unsigned int case_index;

	for (case_index = 0; case_index < ARRAY_SIZE(cases); case_index++) {
		const struct svc_program *case_program = cases[case_index].program;
		const struct svc_procedure *procedure = ERR_PTR(-EUCLEAN);
		const struct svc_version *found_version = ERR_PTR(-EUCLEAN);
		const struct svc_procedure *sentinel = &procedures[0];
		struct svc_process_info *process;
		struct svc_rqst *rqst;
		struct svc_serv *serv;
		u32 argument = U32_MAX;
		u32 response = U32_MAX;
		__be32 lookup_status;
		__be32 dispatch_status;

		process = kunit_kzalloc(test, sizeof(*process), GFP_KERNEL);
		rqst = kunit_kzalloc(test, sizeof(*rqst), GFP_KERNEL);
		serv = kunit_kzalloc(test, sizeof(*serv), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, process);
		KUNIT_ASSERT_NOT_NULL(test, rqst);
		KUNIT_ASSERT_NOT_NULL(test, serv);
		rqst->rq_server = serv;
		rqst->rq_procinfo = sentinel;
		rqst->rq_vers = cases[case_index].version;
		rqst->rq_proc = cases[case_index].procedure;
		rqst->rq_argp = &argument;
		rqst->rq_resp = &response;
		lookup_status = svc_proc_lookup(case_program,
						cases[case_index].version,
						cases[case_index].procedure,
						&found_version, &procedure);
		KUNIT_EXPECT_EQ(test, (__force u32)lookup_status,
				(__force u32)cases[case_index].expected);
		KUNIT_EXPECT_PTR_EQ(test, procedure,
				    cases[case_index].expected_procedure);
		KUNIT_EXPECT_PTR_EQ(test, rqst->rq_procinfo, sentinel);

		rqst->rq_procinfo = NULL;
		dispatch_status = svc_generic_init_request(rqst, case_program, process);
		KUNIT_EXPECT_EQ(test, (__force u32)dispatch_status,
				(__force u32)lookup_status);
		KUNIT_EXPECT_PTR_EQ(test, rqst->rq_procinfo,
				    cases[case_index].expected_procedure);
		if (dispatch_status == rpc_success) {
			KUNIT_EXPECT_EQ(test, argument, 0U);
			KUNIT_EXPECT_EQ(test, response, 0U);
		}
	}
}

static void nfsd_bvec_nfs3_capability_test(struct kunit *test)
{
	const struct svc_procedure *procedure;
	unsigned int capable = 0;
	unsigned int i;

	for (i = 0; i <= NFS3PROC_COMMIT; i++) {
		procedure = nfsd3_procedure(i);
		KUNIT_ASSERT_NOT_NULL(test, procedure);
		capable += procedure->pc_xdr_bvec;
	}
	KUNIT_EXPECT_EQ(test, capable, 1U);
	procedure = nfsd3_procedure(NFS3PROC_WRITE);
	KUNIT_ASSERT_NOT_NULL(test, procedure);
	KUNIT_EXPECT_TRUE(test,
			svc_proc_accepts_xdr_bvec(procedure, RPC_AUTH_NULL));
	KUNIT_EXPECT_TRUE(test,
			svc_proc_accepts_xdr_bvec(procedure, RPC_AUTH_UNIX));
	KUNIT_EXPECT_FALSE(test,
			svc_proc_accepts_xdr_bvec(procedure, RPC_AUTH_GSS));
	KUNIT_EXPECT_PTR_EQ(test, nfsd3_procedure(NFS3PROC_COMMIT + 1), NULL);
}

/* These tests call the real block owners, not an eligibility/error model. */
static void nfsd_dio_geometry_status_test(struct kunit *test)
{
	static const struct {
		int error;
		blk_status_t status;
	} ordinary[] = {
		{ 0, BLK_STS_OK },
		{ -EINVAL, BLK_STS_INVAL },
		{ -EIO, BLK_STS_IOERR },
		{ -ENOSPC, BLK_STS_NOSPC },
		{ -EAGAIN, BLK_STS_AGAIN },
	};
	unsigned int i;

	KUNIT_EXPECT_EQ(test, EDIOALIGN, 532);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)BLK_STS_DIOALIGN, 20U);
	KUNIT_EXPECT_EQ(test, errno_to_blk_status(-EDIOALIGN),
			BLK_STS_DIOALIGN);
	KUNIT_EXPECT_EQ(test, blk_status_to_errno_raw(BLK_STS_DIOALIGN),
			-EDIOALIGN);
	KUNIT_EXPECT_EQ(test, blk_status_to_errno(BLK_STS_DIOALIGN), -EINVAL);
	KUNIT_EXPECT_FALSE(test, blk_path_error(BLK_STS_DIOALIGN));
	for (i = 0; i < ARRAY_SIZE(ordinary); i++) {
		KUNIT_EXPECT_EQ(test, errno_to_blk_status(ordinary[i].error),
				ordinary[i].status);
		KUNIT_EXPECT_EQ(test, blk_status_to_errno(ordinary[i].status),
				ordinary[i].error);
		KUNIT_EXPECT_EQ(test, blk_status_to_errno_raw(ordinary[i].status),
				ordinary[i].error);
	}
}

static void nfsd_dio_geometry_split_test(struct kunit *test)
{
	struct queue_limits limits = {
		.max_segments = 256,
		.max_hw_sectors = 256,
		.max_sectors = 256,
		.max_segment_size = UINT_MAX,
		.max_fast_segment_size = UINT_MAX,
		.seg_boundary_mask = ULONG_MAX,
		.virt_boundary_mask = 4095,
		.dma_alignment = 3,
		.logical_block_size = 512,
		.physical_block_size = 512,
	};
	static const struct {
		blk_opf_t op;
		int expected;
	} refusals[] = {
		{ REQ_OP_WRITE, -EDIOALIGN },
		{ REQ_OP_READ, -EDIOALIGN },
		{ REQ_OP_WRITE | REQ_ATOMIC, -EINVAL },
		{ REQ_OP_WRITE | REQ_NOWAIT, -EAGAIN },
		{ REQ_OP_ZONE_APPEND, -EINVAL },
	};
	struct bio_vec vectors[3];
	struct bio bio;
	unsigned int segments = 0;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(vectors); i++) {
		struct page *page = nfsd_bvec_alloc_page(test);

		KUNIT_ASSERT_NOT_NULL(test, page);
		bvec_set_page(&vectors[i], page, 0, 0);
	}
	/* PRP-compatible joints; 3996 and 100 need not be sector multiples. */
	vectors[0].bv_offset = 100;
	vectors[0].bv_len = 3996;
	vectors[1].bv_len = 4096;
	vectors[2].bv_len = 100;
	bio_init(&bio, NULL, vectors, ARRAY_SIZE(vectors), REQ_OP_WRITE);
	bio.bi_vcnt = ARRAY_SIZE(vectors);
	bio.bi_iter.bi_size = 8192;
	ret = bio_split_io_at(&bio, &limits, &segments, 8192, 3);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, segments, 3U);
	KUNIT_EXPECT_EQ(test, bio.bi_iter.bi_size, 8192U);
	bio_uninit(&bio);

	/* Historical remainder: 104 bytes before a mid-page successor. */
	vectors[0].bv_offset = 3992;
	vectors[0].bv_len = 104;
	vectors[1].bv_offset = 64;
	vectors[1].bv_len = 408;
	for (i = 0; i < ARRAY_SIZE(refusals); i++) {
		bio_init(&bio, NULL, vectors, 2, refusals[i].op);
		bio.bi_vcnt = 2;
		bio.bi_iter.bi_size = 512;
		ret = bio_split_io_at(&bio, &limits, &segments, 512, 3);
		KUNIT_EXPECT_EQ_MSG(test, ret, refusals[i].expected,
				    "op=%#x", refusals[i].op);
		ret = bio_split_rw_at(&bio, &limits, &segments, 512);
		KUNIT_EXPECT_EQ(test, ret, refusals[i].expected == -EDIOALIGN ?
				-EINVAL : refusals[i].expected);
		bio_uninit(&bio);
	}

	/* The other emitter is the actual per-vector DMA/length mask check. */
	vectors[0].bv_offset = 2;
	vectors[0].bv_len = 512;
	bio_init(&bio, NULL, vectors, 1, REQ_OP_WRITE);
	bio.bi_vcnt = 1;
	bio.bi_iter.bi_size = 512;
	ret = bio_split_io_at(&bio, &limits, &segments, 512, 3);
	KUNIT_EXPECT_EQ(test, ret, -EDIOALIGN);
	bio_uninit(&bio);

	/* Segment exhaustion can also leave less than one legal block. */
	limits.virt_boundary_mask = 0;
	limits.max_segments = 1;
	vectors[0].bv_offset = 0;
	vectors[0].bv_len = 104;
	vectors[1].bv_offset = 0;
	vectors[1].bv_len = 408;
	bio_init(&bio, NULL, vectors, 2, REQ_OP_WRITE);
	bio.bi_vcnt = 2;
	bio.bi_iter.bi_size = 512;
	ret = bio_split_io_at(&bio, &limits, &segments, 512, 3);
	KUNIT_EXPECT_EQ(test, ret, -EDIOALIGN);
	bio_uninit(&bio);
	vectors[0].bv_offset = 0;
	vectors[0].bv_len = 510;
	vectors[1].bv_offset = 0;
	vectors[1].bv_len = 2;
	bio_init(&bio, NULL, vectors, 2, REQ_OP_WRITE);
	bio.bi_vcnt = 2;
	bio.bi_iter.bi_size = 512;
	ret = bio_split_io_at(&bio, &limits, &segments, 512, 3);
	KUNIT_EXPECT_EQ(test, ret, -EDIOALIGN);
	bio_uninit(&bio);
}

struct nfsd_dio_chain_result {
	unsigned int calls;
	blk_status_t status;
};

static void nfsd_dio_chain_complete(struct bio *bio)
{
	struct nfsd_dio_chain_result *result = bio->bi_private;

	result->calls++;
	result->status = bio->bi_status;
}

static void nfsd_dio_geometry_chain_test(struct kunit *test)
{
	static const blk_status_t real[] = {
		BLK_STS_INVAL, BLK_STS_IOERR, BLK_STS_NOSPC,
	};
	unsigned int i, order;

	for (i = 0; i < ARRAY_SIZE(real); i++) {
		for (order = 0; order < 2; order++) {
			struct nfsd_dio_chain_result result = {};
			struct bio *parent, *geometry, *failure;

			parent = bio_alloc(NULL, 0, REQ_OP_WRITE, GFP_KERNEL);
			KUNIT_ASSERT_NOT_NULL(test, parent);
			geometry = bio_alloc(NULL, 0, REQ_OP_WRITE, GFP_KERNEL);
			if (!geometry) {
				bio_put(parent);
				KUNIT_FAIL(test, "geometry child allocation failed");
				return;
			}
			failure = bio_alloc(NULL, 0, REQ_OP_WRITE, GFP_KERNEL);
			if (!failure) {
				bio_put(geometry);
				bio_put(parent);
				KUNIT_FAIL(test, "real-error child allocation failed");
				return;
			}
			parent->bi_private = &result;
			parent->bi_end_io = nfsd_dio_chain_complete;
			geometry->bi_status = BLK_STS_DIOALIGN;
			failure->bi_status = real[i];
			bio_chain(geometry, parent);
			bio_chain(failure, parent);
			bio_endio(order ? failure : geometry);
			KUNIT_EXPECT_EQ(test, result.calls, 0U);
			bio_endio(order ? geometry : failure);
			KUNIT_EXPECT_EQ(test, result.calls, 0U);
			bio_endio(parent);
			KUNIT_EXPECT_EQ(test, result.calls, 1U);
			KUNIT_EXPECT_EQ(test, result.status, real[i]);
			bio_put(parent);
		}
	}
}

static void nfsd_dio_geometry_parent_status_test(struct kunit *test)
{
	static const blk_status_t real[] = {
		BLK_STS_INVAL, BLK_STS_IOERR, BLK_STS_NOSPC,
	};
	unsigned int i, role, order;

	for (i = 0; i < ARRAY_SIZE(real); i++) {
		for (role = 0; role < 2; role++) {
			for (order = 0; order < 2; order++) {
				struct nfsd_dio_chain_result result = {};
				blk_status_t own = BLK_STS_DIOALIGN;
				blk_status_t child_status = real[i];
				struct bio *parent, *child;

				parent = bio_alloc(NULL, 0, REQ_OP_WRITE, GFP_KERNEL);
				KUNIT_ASSERT_NOT_NULL(test, parent);
				child = bio_alloc(NULL, 0, REQ_OP_WRITE, GFP_KERNEL);
				if (!child) {
					bio_put(parent);
					KUNIT_FAIL(test, "parent-status child allocation failed");
					return;
				}
				parent->bi_private = &result;
				parent->bi_end_io = nfsd_dio_chain_complete;
				if (role) {
					own = real[i];
					child_status = BLK_STS_DIOALIGN;
				}
				bio_chain(child, parent);
				if (order)
					bio_endio_status(child, child_status);
				else
					bio_endio_status(parent, own);
				KUNIT_EXPECT_EQ(test, result.calls, 0U);
				if (order)
					bio_endio_status(parent, own);
				else
					bio_endio_status(child, child_status);
				KUNIT_EXPECT_EQ(test, result.calls, 1U);
				KUNIT_EXPECT_EQ(test, result.status, real[i]);
				bio_put(parent);
			}
		}
	}
}

struct nfsd_dio_iomap_thread {
	struct iomap_dio *dio;
	struct completion *start;
	struct completion done;
	int error;
};

static int nfsd_dio_iomap_error_thread(void *data)
{
	struct nfsd_dio_iomap_thread *thread = data;

	wait_for_completion(thread->start);
	iomap_dio_kunit_set_error(thread->dio, thread->error);
	complete(&thread->done);
	return 0;
}

static void nfsd_dio_normalizer_complete(struct kiocb *iocb, long result)
{
	unsigned int *calls = iocb->private;

	(*calls)++;
	(void)result;
}

static void nfsd_dio_geometry_iomap_test(struct kunit *test)
{
	static const int real[] = { -EINVAL, -EIO, -ENOSPC };
	struct kiocb iocb = { .ki_flags = IOCB_DIRECT | IOCB_DIO_GEOMETRY };
	unsigned int completions = 0;
	unsigned int i, order;

	KUNIT_EXPECT_EQ(test, IOCB_DIO_GEOMETRY, 1 << 25);
	KUNIT_EXPECT_EQ(test, iomap_dio_kunit_result(&iocb, -EDIOALIGN),
			-EDIOALIGN);
	KUNIT_EXPECT_EQ(test, iomap_dio_kunit_result(&iocb, LONG_MAX),
			(ssize_t)LONG_MAX);
	iocb.ki_complete = nfsd_dio_normalizer_complete;
	iocb.private = &completions;
	KUNIT_EXPECT_EQ(test, iomap_dio_kunit_result(&iocb, -EDIOALIGN), -EINVAL);
	KUNIT_EXPECT_EQ(test, iomap_dio_kunit_result(&iocb, LONG_MAX),
			(ssize_t)LONG_MAX);
	KUNIT_EXPECT_EQ(test, completions, 0U);
	iocb.ki_complete = NULL;
	iocb.private = NULL;
	iocb.ki_flags &= ~IOCB_DIO_GEOMETRY;
	KUNIT_EXPECT_EQ(test, iomap_dio_kunit_result(&iocb, -EDIOALIGN),
			-EINVAL);
	for (i = 0; i < ARRAY_SIZE(real); i++) {
		KUNIT_EXPECT_EQ(test, iomap_dio_kunit_result(&iocb, real[i]),
				real[i]);
		for (order = 0; order < 2; order++) {
			struct iomap_dio *dio = iomap_dio_kunit_alloc(&iocb, true);

			KUNIT_ASSERT_NOT_NULL(test, dio);
			iomap_dio_kunit_set_error(dio, order ? real[i] : -EDIOALIGN);
			iomap_dio_kunit_set_error(dio, order ? -EDIOALIGN : real[i]);
			iomap_dio_kunit_set_error(dio, 0);
			KUNIT_EXPECT_EQ(test, iomap_dio_kunit_read_error(dio), real[i]);
			iomap_dio_kunit_free(dio);
		}
	}
	/* Geometry must not replace the existing NOWAIT admission outcomes. */
	iocb.ki_flags |= IOCB_NOWAIT;
	for (i = 0; i < 2; i++) {
		int admission = i ? -EOPNOTSUPP : -EAGAIN;
		struct iomap_dio *dio = iomap_dio_kunit_alloc(&iocb, true);

		KUNIT_ASSERT_NOT_NULL(test, dio);
		iomap_dio_kunit_set_error(dio, admission);
		iomap_dio_kunit_set_error(dio, -EDIOALIGN);
		KUNIT_EXPECT_EQ(test, iomap_dio_kunit_read_error(dio), admission);
		iomap_dio_kunit_set_error(dio, -EIO);
		KUNIT_EXPECT_EQ(test, iomap_dio_kunit_read_error(dio), -EIO);
		iomap_dio_kunit_free(dio);
	}
}

static void nfsd_dio_geometry_iomap_race_test(struct kunit *test)
{
	struct kiocb iocb = {};
	struct completion start;
	struct nfsd_dio_iomap_thread arguments[2];
	struct task_struct *tasks[2];
	struct iomap_dio *dio;
	unsigned int count = 0, i;

	dio = iomap_dio_kunit_alloc(&iocb, true);
	KUNIT_ASSERT_NOT_NULL(test, dio);
	init_completion(&start);
	for (i = 0; i < ARRAY_SIZE(arguments); i++) {
		arguments[i].dio = dio;
		arguments[i].start = &start;
		arguments[i].error = i ? -EIO : -EDIOALIGN;
		init_completion(&arguments[i].done);
		tasks[i] = kthread_run(nfsd_dio_iomap_error_thread,
				       &arguments[i], "nfsd-dio-error-%u", i);
		if (IS_ERR(tasks[i])) {
			KUNIT_FAIL(test, "error worker %u failed: %ld", i,
				   PTR_ERR(tasks[i]));
			break;
		}
		get_task_struct(tasks[i]);
		count++;
	}
	complete_all(&start);
	for (i = 0; i < count; i++) {
		KUNIT_EXPECT_NE(test,
				wait_for_completion_timeout(&arguments[i].done, 5 * HZ), 0UL);
		kthread_stop(tasks[i]);
		put_task_struct(tasks[i]);
	}
	if (count == ARRAY_SIZE(arguments))
		KUNIT_EXPECT_EQ(test, iomap_dio_kunit_read_error(dio), -EIO);
	iomap_dio_kunit_free(dio);
}

#if IS_REACHABLE(CONFIG_BTRFS_FS)
/* CONFIG_KUNIT accessors invoke btrfs_bio_end_io and actual clone cleanup. */
extern void *btrfs_split_kunit_alloc(void (*complete)(blk_status_t, void *),
				     void *data);
extern void btrfs_split_kunit_end_io(void *opaque, bool is_clone,
				     blk_status_t status);
extern void btrfs_split_kunit_free(void *opaque);

static void nfsd_dio_btrfs_complete(blk_status_t status, void *data)
{
	struct nfsd_dio_chain_result *result = data;

	result->calls++;
	result->status = status;
}

static void nfsd_dio_geometry_btrfs_test(struct kunit *test)
{
	static const struct {
		blk_status_t original;
		blk_status_t clone;
		blk_status_t expected;
	} cases[] = {
		{ BLK_STS_DIOALIGN, BLK_STS_IOERR, BLK_STS_IOERR },
		{ BLK_STS_IOERR, BLK_STS_DIOALIGN, BLK_STS_IOERR },
		{ BLK_STS_DIOALIGN, BLK_STS_INVAL, BLK_STS_INVAL },
		{ BLK_STS_INVAL, BLK_STS_DIOALIGN, BLK_STS_INVAL },
		{ BLK_STS_DIOALIGN, BLK_STS_NOSPC, BLK_STS_NOSPC },
		{ BLK_STS_NOSPC, BLK_STS_DIOALIGN, BLK_STS_NOSPC },
		{ BLK_STS_OK, BLK_STS_IOERR, BLK_STS_IOERR },
		{ BLK_STS_IOERR, BLK_STS_NOSPC, BLK_STS_IOERR },
		{ BLK_STS_DIOALIGN, BLK_STS_OK, BLK_STS_DIOALIGN },
		{ BLK_STS_OK, BLK_STS_DIOALIGN, BLK_STS_DIOALIGN },
		{ BLK_STS_DIOALIGN, BLK_STS_DIOALIGN, BLK_STS_DIOALIGN },
		{ BLK_STS_OK, BLK_STS_OK, BLK_STS_OK },
	};
	unsigned int i, order;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		for (order = 0; order < 2; order++) {
			struct nfsd_dio_chain_result result = {};
			blk_status_t first = cases[i].original;
			blk_status_t second = cases[i].clone;
			void *fixture = btrfs_split_kunit_alloc(nfsd_dio_btrfs_complete,
							       &result);

			KUNIT_ASSERT_NOT_NULL(test, fixture);
			if (order) {
				first = cases[i].clone;
				second = cases[i].original;
			}
			btrfs_split_kunit_end_io(fixture, order, first);
			KUNIT_EXPECT_EQ(test, result.calls, 0U);
			btrfs_split_kunit_end_io(fixture, !order, second);
			KUNIT_EXPECT_EQ(test, result.calls, 1U);
			KUNIT_EXPECT_EQ_MSG(test, result.status, cases[i].expected,
					    "case=%u order=%u", i, order);
			btrfs_split_kunit_free(fixture);
		}
	}
}
#endif

struct nfsd_dio_replay_case {
	const char *name;
	ssize_t responses[4];
	unsigned int calls;
	unsigned long count;
	loff_t position;
	int error;
	int fsync_error;
	bool split;
	bool wb_before;
	bool wb_after;
	bool reissue;
	bool fsync;
};

struct nfsd_dio_replay {
	struct kunit *test;
	const struct nfsd_dio_replay_case *scenario;
	struct file *file;
	u8 *source;
	u8 *actual;
	u8 *scratch;
	unsigned long total;
	loff_t start;
	errseq_t since;
	unsigned int calls;
	unsigned int reissues;
	unsigned int fsyncs;
	unsigned long synced;
	int *requested_flags;
	int flags[4];
	loff_t positions[4];
	size_t counts[4];
};

/*
 * Controlled leaf returns exercise actual NFSD/VFS decisions. These bytes
 * model leaf progress; they are not a physical-I/O or drain proof.
 */
static ssize_t nfsd_dio_replay_write(struct kiocb *iocb, struct iov_iter *iter)
{
	struct nfsd_dio_replay *fixture = iocb->ki_filp->private_data;
	unsigned int call = fixture->calls++;
	size_t count = iov_iter_count(iter), copied = 0;
	ssize_t result;
	loff_t offset = iocb->ki_pos - fixture->start;

	if (call >= ARRAY_SIZE(fixture->flags)) {
		KUNIT_FAIL(fixture->test, "unexpected extra write attempt");
		return -EIO;
	}
	fixture->flags[call] = iocb->ki_flags;
	fixture->positions[call] = iocb->ki_pos;
	fixture->counts[call] = count;
	result = fixture->scenario->responses[call];
	if (result == LONG_MAX)
		result = count;
	if (result == -EDIOALIGN)
		copied = min_t(size_t, count, 4096);
	else if (result > 0)
		copied = result;
	if (offset < 0 || (u64)offset > fixture->total ||
	    copied > count || copied > fixture->total - offset) {
		KUNIT_FAIL(fixture->test, "invalid controlled write bounds");
		return -EIO;
	}
	if (copied && copy_from_iter(fixture->actual + offset, copied, iter) != copied) {
		KUNIT_FAIL(fixture->test, "controlled payload capture was short");
		return -EFAULT;
	}
	if (result > 0) {
		iocb->ki_pos += result;
		/* Like a file system, persist a stable write before returning. */
		if (iocb->ki_flags & IOCB_DSYNC) {
			fixture->synced = iocb->ki_pos - fixture->start;
			result = generic_write_sync(iocb, result);
		}
	}
	if (result == -EDIOALIGN && fixture->scenario->wb_before)
		mapping_set_error(iocb->ki_filp->f_mapping, -EIO);
	return result;
}

static int nfsd_dio_replay_fsync(struct file *file, loff_t start, loff_t end,
				 int datasync)
{
	struct nfsd_dio_replay *fixture = file->private_data;

	fixture->fsyncs++;
	KUNIT_EXPECT_EQ(fixture->test, start, fixture->start);
	KUNIT_EXPECT_EQ(fixture->test, end,
			fixture->start + fixture->synced - 1);
	KUNIT_EXPECT_EQ(fixture->test, datasync,
			!(*fixture->requested_flags & IOCB_SYNC));
	if (fixture->scenario->fsync_error)
		return fixture->scenario->fsync_error;
	return filemap_check_wb_err(file->f_mapping, fixture->since);
}

static const struct file_operations nfsd_dio_replay_ops = {
	.write_iter = nfsd_dio_replay_write,
	.fsync = nfsd_dio_replay_fsync,
};

static int nfsd_dio_replay_init_fs_context(struct fs_context *fc)
{
	return init_pseudo(fc, ANON_INODE_FS_MAGIC) ? 0 : -ENOMEM;
}

/* The superblock pins this module until the last fixture file is released. */
static struct file_system_type nfsd_dio_replay_fs_type = {
	.owner = THIS_MODULE,
	.name = "nfsd_dio_kunit",
	.init_fs_context = nfsd_dio_replay_init_fs_context,
	.kill_sb = kill_anon_super,
};

static void nfsd_dio_replay_unmount(void *mnt)
{
	kern_unmount(mnt);
}

/*
 * Give each case its own anonymous inode, and so its own writeback-error
 * cursor, on an internal mount. Unlike anon_inode_create_getfile(), this
 * S_PRIVATE inode leaves creation and access independent of host LSM policy.
 */
static struct file *nfsd_dio_replay_open(struct vfsmount *mnt, void *data)
{
	struct inode *inode = alloc_anon_inode(mnt->mnt_sb);
	struct file *file;

	if (IS_ERR(inode))
		return ERR_CAST(inode);
	file = alloc_file_pseudo(inode, mnt, "nfsd-dio-kunit", O_RDWR,
				 &nfsd_dio_replay_ops);
	if (IS_ERR(file)) {
		iput(inode);
		return file;
	}
	file->private_data = data;
	return file;
}

static void nfsd_dio_replay_fput(void *file)
{
	fput(file);
}

static void nfsd_dio_replay_before(struct kiocb *iocb,
				   const struct iov_iter *saved, void *data)
{
	struct nfsd_dio_replay *fixture = data;
	struct iov_iter copy = *saved;
	size_t count = fixture->scenario->split ? 2 * PAGE_SIZE : fixture->total;
	unsigned int prefix = fixture->scenario->split ? PAGE_SIZE - 128 : 0;

	fixture->reissues++;
	KUNIT_EXPECT_EQ(fixture->test, iocb->ki_pos, fixture->start + prefix);
	KUNIT_EXPECT_EQ(fixture->test, iov_iter_count(saved), count);
	KUNIT_EXPECT_TRUE(fixture->test, iocb->ki_flags & IOCB_DIRECT);
	KUNIT_EXPECT_TRUE(fixture->test, iocb->ki_flags & IOCB_DIO_GEOMETRY);
	KUNIT_EXPECT_EQ(fixture->test, copy_from_iter(fixture->scratch, count, &copy),
			count);
	KUNIT_EXPECT_MEMEQ(fixture->test, fixture->scratch,
			   fixture->source + prefix, count);
	if (fixture->scenario->wb_after)
		mapping_set_error(iocb->ki_filp->f_mapping, -EIO);
}

static void nfsd_dio_geometry_replay_test(struct kunit *test)
{
	static const struct nfsd_dio_replay_case cases[] = {
		{ .name = "real-einval-no-replay", .responses = { -EINVAL },
		  .calls = 1, .error = -EINVAL },
		{ .name = "real-eio-no-replay", .responses = { -EIO },
		  .calls = 1, .error = -EIO },
		{ .name = "real-enospc-no-replay", .responses = { -ENOSPC },
		  .calls = 1, .error = -ENOSPC },
		{ .name = "geometry-identical-full-replay",
		  .responses = { -EDIOALIGN, LONG_MAX }, .calls = 2,
		  .count = 8192, .position = 8192, .reissue = true, .fsync = true },
		{ .name = "positive-short-no-replay", .responses = { 4096 },
		  .calls = 1, .count = 4096, .position = 4096, .fsync = true },
		{ .name = "geometry-buffered-short-once",
		  .responses = { -EDIOALIGN, 4096 }, .calls = 2,
		  .count = 4096, .position = 4096, .reissue = true, .fsync = true },
		{ .name = "geometry-then-real-error-once",
		  .responses = { -EDIOALIGN, -EIO }, .calls = 2,
		  .error = -EIO, .reissue = true },
		/* A WRITE issued whole as direct I/O is persisted by its own write. */
		{ .name = "full-data-fsync-error-no-replay", .responses = { LONG_MAX },
		  .calls = 1, .count = 0, .position = 8192,
		  .error = -EIO, .fsync_error = -EIO, .fsync = true },
		/* A misaligned WRITE is cut at page boundaries. */
		{ .name = "prefix-wb-error-before-replay-decision",
		  .responses = { LONG_MAX, -EDIOALIGN }, .calls = 2,
		  .count = PAGE_SIZE - 128, .position = PAGE_SIZE, .error = -EIO,
		  .split = true, .wb_before = true },
		{ .name = "prefix-wb-error-after-replay-decision",
		  .responses = { LONG_MAX, -EDIOALIGN, LONG_MAX, LONG_MAX },
		  .calls = 4, .count = 3 * PAGE_SIZE,
		  .position = 3 * PAGE_SIZE + 128, .error = -EIO,
		  .split = true, .wb_after = true, .reissue = true, .fsync = true },
	};
	struct vfsmount *mnt;
	unsigned int i;

	mnt = kern_mount(&nfsd_dio_replay_fs_type);
	KUNIT_ASSERT_FALSE(test, IS_ERR(mnt));
	if (kunit_add_action_or_reset(test, nfsd_dio_replay_unmount, mnt))
		return;
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct nfsd_dio_replay_case *scenario = &cases[i];
		struct nfsd_dio_replay *fixture;
		struct svc_rqst *request;
		struct svc_fh *handle;
		struct nfsd_file *nf;
		struct bio_vec vectors[4];
		struct kiocb iocb;
		struct nfsd_dio_kunit_hooks hooks;
		unsigned long count, consumed = 0;
		unsigned int nvecs, v;
		int flags = IOCB_DSYNC;
		int ret;

		kunit_info(test, "%s", scenario->name);
		fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
		request = kunit_kzalloc(test, sizeof(*request), GFP_KERNEL);
		handle = kunit_kzalloc(test, sizeof(*handle), GFP_KERNEL);
		nf = kunit_kzalloc(test, sizeof(*nf), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, fixture);
		KUNIT_ASSERT_NOT_NULL(test, request);
		KUNIT_ASSERT_NOT_NULL(test, handle);
		KUNIT_ASSERT_NOT_NULL(test, nf);
		fixture->test = test;
		fixture->scenario = scenario;
		/* The split's direct middle is 2 pages, at least min_middle_bytes. */
		fixture->total = scenario->split ? 3 * PAGE_SIZE : 8192;
		fixture->start = scenario->split ? 128 : 0;
		fixture->source = kunit_kmalloc(test, fixture->total, GFP_KERNEL);
		fixture->actual = kunit_kzalloc(test, fixture->total, GFP_KERNEL);
		fixture->scratch = kunit_kmalloc(test, fixture->total, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, fixture->source);
		KUNIT_ASSERT_NOT_NULL(test, fixture->actual);
		KUNIT_ASSERT_NOT_NULL(test, fixture->scratch);
		for (v = 0; v < fixture->total; v++)
			fixture->source[v] = (u8)(v * 17U + 3U);
		fixture->file = nfsd_dio_replay_open(mnt, fixture);
		KUNIT_ASSERT_FALSE(test, IS_ERR(fixture->file));
		if (kunit_add_action_or_reset(test, nfsd_dio_replay_fput, fixture->file))
			return;
		fixture->since = READ_ONCE(fixture->file->f_wb_err);
		fixture->synced = scenario->count;
		count = fixture->total;
		nvecs = scenario->split ? 4 : DIV_ROUND_UP(count, PAGE_SIZE);
		for (v = 0; v < nvecs; v++) {
			struct page *page = nfsd_bvec_alloc_page(test);
			size_t length;

			KUNIT_ASSERT_NOT_NULL(test, page);
			if (scenario->split)
				length = v == 0 ? 1000 : v < 3 ? PAGE_SIZE : PAGE_SIZE - 1000;
			else
				length = min_t(size_t, count - consumed, PAGE_SIZE);
			bvec_set_page(&vectors[v], page, length, 0);
			nfsd_bvec_write(page, 0, fixture->source + consumed, length);
			consumed += length;
		}
		KUNIT_ASSERT_EQ(test, consumed, count);
		request->rq_bvec = vectors;
		handle->fh_dentry = fixture->file->f_path.dentry;
		nf->nf_file = fixture->file;
		nf->nf_inode = file_inode(fixture->file);
		nf->nf_dio_mem_align = 4;
		nf->nf_dio_offset_align = 512;
		nf->nf_dio_seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE;
		init_sync_kiocb(&iocb, fixture->file);
		iocb.ki_pos = fixture->start;
		iocb.ki_flags |= flags;
		fixture->requested_flags = &flags;
		hooks.before_reissue = nfsd_dio_replay_before;
		hooks.data = fixture;
		ret = nfsd_direct_write_kunit(request, handle, nf, nvecs, &count,
					      &iocb, fixture->since, &hooks);
		KUNIT_EXPECT_EQ(test, ret, scenario->error);
		KUNIT_EXPECT_EQ(test, count, scenario->count);
		KUNIT_EXPECT_EQ(test, iocb.ki_pos, scenario->position);
		KUNIT_EXPECT_EQ(test, fixture->calls, scenario->calls);
		KUNIT_EXPECT_EQ(test, fixture->reissues, (unsigned int)scenario->reissue);
		KUNIT_EXPECT_EQ(test, fixture->fsyncs, (unsigned int)scenario->fsync);
		for (v = 0; v < fixture->calls && v < ARRAY_SIZE(fixture->flags); v++) {
			bool direct = scenario->split ? v == 1 : v == 0;

			KUNIT_EXPECT_EQ(test, !!(fixture->flags[v] & IOCB_DIRECT), direct);
			KUNIT_EXPECT_EQ(test, !!(fixture->flags[v] & IOCB_DIO_GEOMETRY), direct);
		}
		if (scenario->reissue) {
			unsigned int direct = scenario->split ? 1 : 0;

			KUNIT_EXPECT_EQ(test, fixture->positions[direct],
					fixture->positions[direct + 1]);
			KUNIT_EXPECT_EQ(test, fixture->counts[direct],
					fixture->counts[direct + 1]);
		}
		if (!scenario->error)
			KUNIT_EXPECT_MEMEQ(test, fixture->actual, fixture->source, count);
		{
			struct iov_iter original;

			iov_iter_bvec(&original, ITER_SOURCE, vectors, nvecs, fixture->total);
			KUNIT_EXPECT_EQ(test,
					copy_from_iter(fixture->scratch, fixture->total, &original),
					(size_t)fixture->total);
			KUNIT_EXPECT_MEMEQ(test, fixture->scratch, fixture->source, fixture->total);
		}
		for (v = 0; v < nvecs; v++)
			KUNIT_EXPECT_EQ(test, page_count(vectors[v].bv_page), 1);
	}
}

static struct kunit_case nfsd_bvec_test_cases[] = {
	KUNIT_CASE(nfsd_bvec_decode_checksum_parity_test),
	KUNIT_CASE(nfsd_bvec_decode_error_test),
	KUNIT_CASE(nfsd_bvec_dio_segments_test),
	KUNIT_CASE(nfsd_bvec_dio_seg_boundary_test),
	KUNIT_CASE(nfsd_bvec_dio_disposition_test),
	KUNIT_CASE(nfsd_bvec_proc_dispatch_parity_test),
	KUNIT_CASE(nfsd_bvec_nfs3_capability_test),
	KUNIT_CASE(nfsd_dio_geometry_status_test),
	KUNIT_CASE(nfsd_dio_geometry_split_test),
	KUNIT_CASE(nfsd_dio_geometry_chain_test),
	KUNIT_CASE(nfsd_dio_geometry_parent_status_test),
	KUNIT_CASE(nfsd_dio_geometry_iomap_test),
	KUNIT_CASE(nfsd_dio_geometry_iomap_race_test),
	KUNIT_CASE(nfsd_dio_geometry_replay_test),
#if IS_REACHABLE(CONFIG_BTRFS_FS)
	KUNIT_CASE(nfsd_dio_geometry_btrfs_test),
#endif
	{}
};

static struct kunit_suite nfsd_bvec_test_suite = {
	.name = "nfsd-receive-bvec",
	.test_cases = nfsd_bvec_test_cases,
};

kunit_test_suite(nfsd_bvec_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for NFSD immutable receive bvec consumers");
MODULE_LICENSE("GPL");
