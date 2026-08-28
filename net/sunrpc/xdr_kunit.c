// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <linux/bvec.h>
#include <linux/errno.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sunrpc/xdr.h>

static void xdr_bvec_test_free_page(void *data)
{
	__free_page(data);
}

static struct page *xdr_bvec_test_alloc_page(struct kunit *test)
{
	struct page *page;

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return NULL;
	if (kunit_add_action_or_reset(test, xdr_bvec_test_free_page, page))
		return NULL;
	return page;
}

static void xdr_bvec_test_write(struct page *page, unsigned int offset,
				const void *src, unsigned int len)
{
	void *addr = kmap_local_page(page);

	memcpy(addr + offset, src, len);
	kunmap_local(addr);
}

static void xdr_bvec_test_init_buf(struct xdr_buf *buf,
				   struct bio_vec *bvec,
				   unsigned int bvec_count,
				   unsigned int bvec_offset,
				   unsigned int page_len,
				   void *head, unsigned int head_len,
				   void *tail, unsigned int tail_len)
{
	memset(buf, 0, sizeof(*buf));
	buf->head[0].iov_base = head;
	buf->head[0].iov_len = head_len;
	buf->tail[0].iov_base = tail;
	buf->tail[0].iov_len = tail_len;
	buf->bvec = bvec;
	buf->bvec_count = bvec_count;
	buf->bvec_offset = bvec_offset;
	buf->page_len = page_len;
	buf->page_mode = XDRBUF_PAGE_BVECS;
	buf->len = head_len + page_len + tail_len;
	buf->buflen = buf->len;
}

static void xdr_bvec_inline_decode_test(struct kunit *test)
{
	static const u8 expected[] = { 0x10, 0x11, 0x12, 0x13,
					 0x14, 0x15, 0x16, 0x17 };
	struct page *pages[2];
	struct bio_vec bvec[2];
	struct xdr_stream xdr;
	struct xdr_buf buf;
	__be32 scratch[2];
	u8 copied[sizeof(expected)];
	__be32 *p;

	pages[0] = xdr_bvec_test_alloc_page(test);
	pages[1] = xdr_bvec_test_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, pages[0]);
	KUNIT_ASSERT_NOT_NULL(test, pages[1]);
	bvec_set_page(&bvec[0], pages[0], 3, 1);
	bvec_set_page(&bvec[1], pages[1], 5, 7);
	xdr_bvec_test_write(pages[0], 1, expected, 3);
	xdr_bvec_test_write(pages[1], 7, expected + 3, 5);
	xdr_bvec_test_init_buf(&buf, bvec, ARRAY_SIZE(bvec), 0,
			       sizeof(expected), NULL, 0, NULL, 0);

	KUNIT_ASSERT_EQ(test, read_bytes_from_xdr_buf(&buf, 0, copied,
						      sizeof(copied)), 0);
	KUNIT_EXPECT_MEMEQ(test, copied, expected, sizeof(expected));

	xdr_init_decode(&xdr, &buf, NULL, NULL);
	xdr_set_scratch_buffer(&xdr, scratch, sizeof(scratch));
	p = xdr_inline_decode(&xdr, sizeof(*p));
	KUNIT_EXPECT_NOT_NULL(test, p);
	if (!p)
		goto out;
	KUNIT_EXPECT_PTR_EQ(test, p, &scratch[0]);
	KUNIT_EXPECT_EQ(test, (unsigned long)p % __alignof__(*p), 0UL);
	KUNIT_EXPECT_MEMEQ(test, p, expected, sizeof(*p));

	p = xdr_inline_decode(&xdr, sizeof(*p));
	KUNIT_EXPECT_NOT_NULL(test, p);
	if (!p)
		goto out;
	KUNIT_EXPECT_PTR_NE(test, p, &scratch[0]);
	KUNIT_EXPECT_EQ(test, (unsigned long)p % __alignof__(*p), 0UL);
	KUNIT_EXPECT_MEMEQ(test, p, expected + sizeof(*p), sizeof(*p));
	KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), (size_t)0);
out:
	xdr_finish_decode(&xdr);
}

static void xdr_bvec_subsegment_test(struct kunit *test)
{
	static const u8 first[] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25 };
	static const u8 second[] = { 0x30, 0x31, 0x32, 0x33,
				       0x34, 0x35, 0x36 };
	static const u8 expected[] = { 0x25, 0x30, 0x31,
					 0x32, 0x33, 0x34 };
	__be32 head = 0;
	__be32 tail = 0;
	struct bio_vec converted[2];
	struct bio_vec original[2];
	struct page *pages[2];
	struct bio_vec bvec[2];
	struct xdr_buf stream_subbuf;
	struct xdr_buf subbuf;
	struct xdr_stream xdr;
	struct xdr_buf buf;
	u8 copied[sizeof(expected)];
	bool success;
	__be32 *p;
	int ret;

	pages[0] = xdr_bvec_test_alloc_page(test);
	pages[1] = xdr_bvec_test_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, pages[0]);
	KUNIT_ASSERT_NOT_NULL(test, pages[1]);
	bvec_set_page(&bvec[0], pages[0], sizeof(first), 5);
	bvec_set_page(&bvec[1], pages[1], sizeof(second), 9);
	xdr_bvec_test_write(pages[0], 5, first, sizeof(first));
	xdr_bvec_test_write(pages[1], 9, second, sizeof(second));
	memcpy(original, bvec, sizeof(bvec));
	xdr_bvec_test_init_buf(&buf, bvec, ARRAY_SIZE(bvec), 2, 11,
			       &head, sizeof(head), &tail, sizeof(tail));

	ret = xdr_buf_subsegment(&buf, &subbuf, sizeof(head) + 3,
				 sizeof(expected));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, subbuf.page_mode, XDRBUF_PAGE_BVECS);
	KUNIT_EXPECT_PTR_EQ(test, subbuf.bvec, &bvec[0]);
	KUNIT_EXPECT_EQ(test, subbuf.bvec_offset, 5U);
	KUNIT_EXPECT_EQ(test, subbuf.bvec_count, 2U);
	KUNIT_EXPECT_EQ(test, subbuf.page_len, (unsigned int)sizeof(expected));

	KUNIT_ASSERT_EQ(test, read_bytes_from_xdr_buf(&subbuf, 0, copied,
						      sizeof(copied)), 0);
	KUNIT_EXPECT_MEMEQ(test, copied, expected, sizeof(expected));
	ret = xdr_buf_to_bvec(converted, ARRAY_SIZE(converted), &subbuf);
	KUNIT_ASSERT_EQ(test, ret, 2);
	KUNIT_EXPECT_PTR_EQ(test, converted[0].bv_page, pages[0]);
	KUNIT_EXPECT_EQ(test, converted[0].bv_offset, 10U);
	KUNIT_EXPECT_EQ(test, converted[0].bv_len, 1U);
	KUNIT_EXPECT_PTR_EQ(test, converted[1].bv_page, pages[1]);
	KUNIT_EXPECT_EQ(test, converted[1].bv_offset, 9U);
	KUNIT_EXPECT_EQ(test, converted[1].bv_len, 5U);
	KUNIT_EXPECT_MEMEQ(test, bvec, original, sizeof(bvec));

	xdr_init_decode(&xdr, &buf, NULL, NULL);
	p = xdr_inline_decode(&xdr, sizeof(head));
	KUNIT_EXPECT_NOT_NULL(test, p);
	if (p) {
		success = xdr_stream_subsegment(&xdr, &stream_subbuf, 8);
		KUNIT_EXPECT_TRUE(test, success);
		if (success) {
			KUNIT_EXPECT_EQ(test, stream_subbuf.page_mode,
					XDRBUF_PAGE_BVECS);
			KUNIT_EXPECT_EQ(test, stream_subbuf.page_len, 8U);
			KUNIT_EXPECT_EQ(test, stream_subbuf.bvec_offset, 2U);
			KUNIT_EXPECT_EQ(test, stream_subbuf.bvec_count, 2U);
		}
	}
	xdr_finish_decode(&xdr);
}

static void xdr_bvec_rejection_test(struct kunit *test)
{
	static const u8 expected[] = { 0x40, 0x41, 0x42, 0x43 };
	struct page *page = xdr_bvec_test_alloc_page(test);
	struct bio_vec original;
	struct bio_vec bvec;
	struct xdr_stream xdr;
	struct xdr_buf buf;
	u8 replacement[sizeof(expected)] = { 0 };
	u8 copied[sizeof(expected)];
	unsigned int len;

	KUNIT_ASSERT_NOT_NULL(test, page);

	bvec_set_page(&bvec, page, sizeof(expected), 3);
	xdr_bvec_test_write(page, 3, expected, sizeof(expected));
	original = bvec;
	xdr_bvec_test_init_buf(&buf, &bvec, 1, 0, sizeof(expected),
			       NULL, 0, NULL, 0);
	len = buf.len;

	KUNIT_EXPECT_EQ(test, xdr_alloc_bvec(&buf, GFP_KERNEL),
			-EOPNOTSUPP);
	xdr_free_bvec(&buf);
	KUNIT_EXPECT_PTR_EQ(test, buf.bvec, &bvec);
	KUNIT_EXPECT_MEMEQ(test, &bvec, &original, sizeof(bvec));
	KUNIT_EXPECT_EQ(test, write_bytes_to_xdr_buf(&buf, 0, replacement,
						     sizeof(replacement)),
			-EOPNOTSUPP);
	xdr_init_decode(&xdr, &buf, NULL, NULL);
	KUNIT_EXPECT_EQ(test, xdr_stream_zero(&xdr, 0, sizeof(expected)), 0U);
	xdr_finish_decode(&xdr);
	xdr_buf_trim(&buf, 1);
	KUNIT_EXPECT_EQ(test, buf.len, len);
	KUNIT_ASSERT_EQ(test, read_bytes_from_xdr_buf(&buf, 0, copied,
						      sizeof(copied)), 0);
	KUNIT_EXPECT_MEMEQ(test, copied, expected, sizeof(expected));

	bvec.bv_offset = PAGE_SIZE - 1;
	bvec.bv_len = 2;
	buf.page_len = 2;
	buf.len = 2;
	buf.buflen = 2;
	KUNIT_EXPECT_EQ(test, read_bytes_from_xdr_buf(&buf, 0, copied, 2), -1);
	xdr_init_decode(&xdr, &buf, NULL, NULL);
	KUNIT_EXPECT_PTR_EQ(test, xdr_inline_decode(&xdr, sizeof(__be32)), NULL);
	xdr_finish_decode(&xdr);
}

static struct kunit_case xdr_bvec_test_cases[] = {
	KUNIT_CASE(xdr_bvec_inline_decode_test),
	KUNIT_CASE(xdr_bvec_subsegment_test),
	KUNIT_CASE(xdr_bvec_rejection_test),
	{}
};

static struct kunit_suite xdr_bvec_test_suite = {
	.name = "sunrpc-xdr-bvec",
	.test_cases = xdr_bvec_test_cases,
};

kunit_test_suite(xdr_bvec_test_suite);

MODULE_DESCRIPTION("KUnit tests for SUNRPC XDR authoritative bvecs");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
