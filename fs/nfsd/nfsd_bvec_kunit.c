// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <linux/bvec.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nfs3.h>
#include <linux/nfs_dio.h>
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
	 * [p718 backport to v7.1.8] the NFSv3 WRITE decoder on this base does
	 * not reject an out-of-range stable_how (that check arrived with
	 * 9ff1cfefd2 in 7.2), so the rejection expectation is not applicable.
	 */
	fixture->head[5] = cpu_to_be32(NFS_FILE_SYNC + 1);
	nfsd_bvec_decode_context_init(context, &fixture->authoritative, args);
	decoded = nfs3svc_decode_writeargs(&context->rqst, &context->stream);
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
	unsigned int	offsets[3];
	unsigned int	lengths[3];
	unsigned int	expected_nsegs;
	unsigned int	expected_direct_mask;
	unsigned int	expected_dontcache_mask;
	unsigned long	expected_counts[3];
};

static void nfsd_bvec_dio_segments_test(struct kunit *test)
{
	static const struct nfsd_bvec_dio_case cases[] = {
		{
			.name = "aligned-first-middle-last",
			.total = 8192,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { 4096, 3584, 512 },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 8192 },
		},
		{
			.name = "later-offset-misaligned",
			.total = 8192,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 3,
			.offsets = { 0, 1, 0 },
			.lengths = { 4096, 3584, 512 },
			.expected_nsegs = 1,
			.expected_counts = { 8192 },
		},
		{
			.name = "later-length-misaligned",
			.total = 8192,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { 4096, 3583, 513 },
			.expected_nsegs = 1,
			.expected_counts = { 8192 },
		},
		{
			/*
			 * A misaligned write whose aligned middle is smaller
			 * than nfsd_direct_misaligned_num_pages (default 2)
			 * pages must not be split: one buffered segment.
			 */
			.name = "small-middle-below-misaligned-gate",
			.position = 128,
			.total = 4096,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 2,
			.offsets = { 128, 0 },
			.lengths = { 3968, 128 },
			.expected_nsegs = 1,
			.expected_counts = { 4096 },
		},
		{
			/*
			 * Middle sized exactly at the misaligned-split gate
			 * (2 pages) so the split happens on any page size.
			 */
			.name = "buffered-prefix-direct-middle-buffered-suffix",
			.position = 128,
			.total = 2 * PAGE_SIZE + 512,
			.mem_align = 512,
			.offset_align = 512,
			.nvecs = 3,
			.offsets = { 128, 0, 0 },
			.lengths = { PAGE_SIZE - 128, PAGE_SIZE, 640 },
			.expected_nsegs = 3,
			.expected_direct_mask = BIT(1),
			.expected_counts = { 384, 2 * PAGE_SIZE, 128 },
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
			.total = 8192,
			.mem_align = 512,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 3,
			.offsets = { 0, 1, 0 },
			.lengths = { 4096, 3584, 512 },
			.expected_nsegs = 1,
			.expected_dontcache_mask = BIT(0),
			.expected_counts = { 8192 },
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
			.total = 8192,
			.mem_align = 4,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { 3584, 512, 4096 },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 8192 },
		},
		{
			/*
			 * A discontinuity at payload byte 4092 is not a
			 * logical-block multiple: a forced split there would
			 * strand a sub-sector residue and the block stack
			 * would reject the bio, so the gate must demote the
			 * segment to buffered.
			 */
			.name = "discontinuity-misaligned-buffered",
			.total = 8192,
			.mem_align = 4,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { 4092, 4, 4096 },
			.expected_nsegs = 1,
			.expected_counts = { 8192 },
		},
		{
			.name = "discontinuity-misaligned-dontcache",
			.total = 8192,
			.mem_align = 4,
			.offset_align = 512,
			.fop_flags = FOP_DONTCACHE,
			.nvecs = 3,
			.lengths = { 4092, 4, 4096 },
			.expected_nsegs = 1,
			.expected_dontcache_mask = BIT(0),
			.expected_counts = { 8192 },
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
			.total = 8192,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE,
			.nvecs = 3,
			.lengths = { 4092, 4, 4096 },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 8192 },
		},
		{
			/* TCP-segment geometry: fragments start mid-page */
			.name = "no-boundary-tcp-segments-direct",
			.total = 4096,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE,
			.nvecs = 3,
			.offsets = { 0, 2048, 512 },
			.lengths = { 1448, 1448, 1200 },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 4096 },
		},
		{
			.name = "default-boundary-tcp-segments-buffered",
			.total = 4096,
			.mem_align = 4,
			.offset_align = 512,
			.nvecs = 3,
			.offsets = { 0, 2048, 512 },
			.lengths = { 1448, 1448, 1200 },
			.expected_nsegs = 1,
			.expected_counts = { 4096 },
		},
		{
			/* no boundary does not relax memory alignment */
			.name = "no-boundary-2-byte-fragment-buffered",
			.total = 8192,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE,
			.nvecs = 3,
			.lengths = { 4094, 2, 4096 },
			.expected_nsegs = 1,
			.expected_counts = { 8192 },
		},
		{
			/*
			 * Joints on 4 KiB multiples at payload byte 3996, not
			 * a logical-block multiple: a 4096-byte boundary
			 * admits them on every page size.
			 */
			.name = "boundary-4096-joint-on-boundary-direct",
			.total = 8192,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = 4096,
			.nvecs = 3,
			.offsets = { 100, 0, 0 },
			.lengths = { 3996, 4096, 100 },
			.expected_nsegs = 1,
			.expected_direct_mask = BIT(0),
			.expected_counts = { 8192 },
		},
		{
			/* the page-sized default admits them only on 4 KiB pages */
			.name = "default-boundary-joint-on-4096",
			.total = 8192,
			.mem_align = 4,
			.offset_align = 512,
			.nvecs = 3,
			.offsets = { 100, 0, 0 },
			.lengths = { 3996, 4096, 100 },
			.expected_nsegs = 1,
			.expected_direct_mask = PAGE_SIZE == 4096 ? BIT(0) : 0,
			.expected_counts = { 8192 },
		},
		{
			.name = "boundary-4096-mid-page-joint-buffered",
			.total = 8192,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = 4096,
			.nvecs = 3,
			.lengths = { 4092, 4, 4096 },
			.expected_nsegs = 1,
			.expected_counts = { 8192 },
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
			 * The middle starts 384 bytes into bvec[0], whose
			 * mid-page end is a joint at middle byte 616, not an
			 * offset_align multiple: no boundary admits it.
			 */
			.name = "skipped-first-no-boundary-mid-page-joint-direct",
			.position = 128,
			.total = 2 * PAGE_SIZE + 512,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE,
			.nvecs = 3,
			.lengths = { 1000, PAGE_SIZE, PAGE_SIZE - 488 },
			.expected_nsegs = 3,
			.expected_direct_mask = BIT(1),
			.expected_counts = { 384, 2 * PAGE_SIZE, 128 },
		},
		{
			.name = "skipped-first-boundary-4096-mid-page-joint-buffered",
			.position = 128,
			.total = 2 * PAGE_SIZE + 512,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = 4096,
			.nvecs = 3,
			.lengths = { 1000, PAGE_SIZE, PAGE_SIZE - 488 },
			.expected_nsegs = 1,
			.expected_counts = { 2 * PAGE_SIZE + 512 },
		},
		{
			.name = "skipped-first-default-boundary-mid-page-joint-buffered",
			.position = 128,
			.total = 2 * PAGE_SIZE + 512,
			.mem_align = 4,
			.offset_align = 512,
			.nvecs = 3,
			.lengths = { 1000, PAGE_SIZE, PAGE_SIZE - 488 },
			.expected_nsegs = 1,
			.expected_counts = { 2 * PAGE_SIZE + 512 },
		},
		{
			/*
			 * bvec[0] (offset 100) ends on a 4 KiB multiple, so
			 * its joint at middle byte 3612 is no gap under a
			 * 4096-byte boundary on any page size; counting the
			 * 384 skipped bytes into its end would make it one.
			 */
			.name = "skipped-first-boundary-4096-joint-on-boundary-direct",
			.position = 128,
			.total = 2 * PAGE_SIZE + 512,
			.mem_align = 4,
			.offset_align = 512,
			.seg_boundary = 4096,
			.nvecs = 3,
			.offsets = { 100, 0, 0 },
			.lengths = { 3996, PAGE_SIZE, PAGE_SIZE - 3484 },
			.expected_nsegs = 3,
			.expected_direct_mask = BIT(1),
			.expected_counts = { 384, 2 * PAGE_SIZE, 128 },
		},
	};
	unsigned int case_index;

	for (case_index = 0; case_index < ARRAY_SIZE(cases); case_index++) {
		const struct nfsd_bvec_dio_case *test_case = &cases[case_index];
		struct nfs_dio_split split = {};
		struct nfs_dio_seg *segments = split.segs;
		struct bio_vec bvec[3] = {};
		/*
		 * The split consults file->f_op->fop_flags for FOP_DONTCACHE,
		 * so the file must carry real file_operations.
		 */
		const struct file_operations fake_fops = {
			.fop_flags = test_case->fop_flags,
		};
		struct file fake_file = {
			.f_op = &fake_fops,
		};
		/*
		 * The policy nfsd_write_dio_iters_init() hands the split for
		 * this file under nfsd's defaults: direct_misaligned_num_pages
		 * of 2 and direct_misaligned_dontcache set.
		 */
		const struct nfs_dio_policy policy = {
			.mem_align = test_case->mem_align,
			.offset_align = test_case->offset_align,
			.seg_boundary = test_case->seg_boundary,
			.min_middle_pages = 2,
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
		 * With DONTCACHE in effect, a split WRITE marks its buffered
		 * prefix and suffix as boundary segments and an unsplit
		 * buffered WRITE marks its partial end pages; nothing is
		 * marked otherwise.
		 */
		for (segment_index = 0; segment_index < nsegs;
		     segment_index++) {
			bool buffered_dontcache =
				!(segments[segment_index].flags & IOCB_DIRECT) &&
				(test_case->fop_flags & FOP_DONTCACHE);

			KUNIT_EXPECT_EQ(test, segments[segment_index].boundary,
					nsegs > 1 && buffered_dontcache);
			KUNIT_EXPECT_EQ(test, segments[segment_index].edges,
					nsegs == 1 && buffered_dontcache);
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
			base_flags = segments[segment_index].flags &
				     ~(IOCB_DIRECT | IOCB_DONTCACHE);
			KUNIT_EXPECT_EQ(test, base_flags, IOCB_DSYNC);
			copied = copy_from_iter(actual, expected, &iter);
			KUNIT_ASSERT_EQ(test, copied, expected);
			KUNIT_EXPECT_MEMEQ(test, actual, source + consumed, expected);
			consumed += expected;
		}
		KUNIT_EXPECT_EQ(test, consumed, test_case->total);
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

static struct kunit_case nfsd_bvec_test_cases[] = {
	KUNIT_CASE(nfsd_bvec_decode_checksum_parity_test),
	KUNIT_CASE(nfsd_bvec_decode_error_test),
	KUNIT_CASE(nfsd_bvec_dio_segments_test),
	KUNIT_CASE(nfsd_bvec_proc_dispatch_parity_test),
	KUNIT_CASE(nfsd_bvec_nfs3_capability_test),
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
