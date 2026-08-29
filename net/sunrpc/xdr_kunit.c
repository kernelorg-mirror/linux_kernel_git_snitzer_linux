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

static void xdr_bvec_test_fill(struct page *page, u8 value)
{
	void *addr = kmap_local_page(page);

	memset(addr, value, PAGE_SIZE);
	kunmap_local(addr);
}

static u8 xdr_bvec_test_read_byte(struct page *page, unsigned int offset)
{
	void *addr = kmap_local_page(page);
	u8 value = ((u8 *)addr)[offset];

	kunmap_local(addr);
	return value;
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

static void xdr_bvec_offset_matrix_test(struct kunit *test)
{
	static const unsigned int offsets[] = {
		0, 1, 2, 3, 511, 512, PAGE_SIZE - 1,
	};
	static const u8 expected[] = { 0x11, 0x22, 0x33, 0x44 };
	struct page *pages[2];
	unsigned int i;

	pages[0] = xdr_bvec_test_alloc_page(test);
	pages[1] = xdr_bvec_test_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, pages[0]);
	KUNIT_ASSERT_NOT_NULL(test, pages[1]);

	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		struct bio_vec original[2];
		struct bio_vec bvec[2];
		unsigned int first_len;
		unsigned int count = 1;
		struct xdr_stream xdr;
		struct xdr_buf buf;
		__be32 scratch[2];
		u8 copied[6];
		__be32 *p;

		xdr_bvec_test_fill(pages[0], 0xa5);
		xdr_bvec_test_fill(pages[1], 0xa5);
		first_len = min_t(unsigned int, sizeof(expected),
				  PAGE_SIZE - offsets[i]);
		bvec_set_page(&bvec[0], pages[0], first_len, offsets[i]);
		xdr_bvec_test_write(pages[0], offsets[i], expected, first_len);
		if (first_len < sizeof(expected)) {
			bvec_set_page(&bvec[1], pages[1],
				      sizeof(expected) - first_len, 7);
			xdr_bvec_test_write(pages[1], 7, expected + first_len,
					    sizeof(expected) - first_len);
			count++;
		}
		memcpy(original, bvec, count * sizeof(*bvec));
		xdr_bvec_test_init_buf(&buf, bvec, count, 0,
				       sizeof(expected), NULL, 0, NULL, 0);

		memset(copied, 0x5a, sizeof(copied));
		KUNIT_ASSERT_EQ(test,
				read_bytes_from_xdr_buf(&buf, 0, copied + 1,
							sizeof(expected)), 0);
		KUNIT_EXPECT_EQ(test, copied[0], (u8)0x5a);
		KUNIT_EXPECT_EQ(test, copied[5], (u8)0x5a);
		KUNIT_EXPECT_MEMEQ(test, copied + 1, expected, sizeof(expected));

		xdr_init_decode(&xdr, &buf, NULL, NULL);
		xdr_set_scratch_buffer(&xdr, scratch, sizeof(scratch));
		p = xdr_inline_decode(&xdr, sizeof(expected));
		KUNIT_EXPECT_NOT_NULL(test, p);
		if (!p) {
			xdr_finish_decode(&xdr);
			return;
		}
		if (IS_ALIGNED(offsets[i], __alignof__(*p)) && count == 1)
			KUNIT_EXPECT_PTR_NE(test, p, &scratch[0]);
		else
			KUNIT_EXPECT_PTR_EQ(test, p, &scratch[0]);
		KUNIT_EXPECT_EQ(test, (unsigned long)p % __alignof__(*p), 0UL);
		KUNIT_EXPECT_MEMEQ(test, p, expected, sizeof(expected));
		KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), (size_t)0);
		xdr_finish_decode(&xdr);

		KUNIT_EXPECT_MEMEQ(test, bvec, original,
				   count * sizeof(*bvec));
		if (offsets[i])
			KUNIT_EXPECT_EQ(test,
					xdr_bvec_test_read_byte(pages[0],
								offsets[i] - 1),
					(u8)0xa5);
		if (offsets[i] + first_len < PAGE_SIZE)
			KUNIT_EXPECT_EQ(test,
					xdr_bvec_test_read_byte(pages[0],
								offsets[i] + first_len),
					(u8)0xa5);
		if (count == 2) {
			KUNIT_EXPECT_EQ(test,
					xdr_bvec_test_read_byte(pages[1], 6),
					(u8)0xa5);
			KUNIT_EXPECT_EQ(test,
					xdr_bvec_test_read_byte(pages[1],
								7 + sizeof(expected) -
								first_len),
					(u8)0xa5);
		}
	}
}

static void xdr_bvec_component_matrix_test(struct kunit *test)
{
	static const struct {
		u8 head_len;
		u8 first_len;
		u8 second_len;
		u8 tail_len;
	} cases[] = {
		{ 2, 2, 0, 0 },
		{ 0, 2, 2, 0 },
		{ 0, 2, 0, 2 },
		{ 2, 0, 0, 2 },
	};
	static const u8 expected[] = { 0x10, 0x20, 0x30, 0x40 };
	static const u8 encoded_opaque[] = {
		0, 0, 0, 3, 0x51, 0x52, 0x53, 0xee,
	};
	struct page *pages[2];
	unsigned int i;

	pages[0] = xdr_bvec_test_alloc_page(test);
	pages[1] = xdr_bvec_test_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, pages[0]);
	KUNIT_ASSERT_NOT_NULL(test, pages[1]);

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct bio_vec original[2];
		struct bio_vec bvec[2];
		unsigned int page_len;
		unsigned int count = 0;
		unsigned int pos = 0;
		struct xdr_stream xdr;
		struct xdr_buf buf;
		__be32 scratch[2];
		u8 head[4] = { 0 };
		u8 tail[4] = { 0 };
		__be32 *p;

		memcpy(head, expected, cases[i].head_len);
		pos += cases[i].head_len;
		if (cases[i].first_len) {
			bvec_set_page(&bvec[count], pages[0],
				      cases[i].first_len, 3);
			xdr_bvec_test_write(pages[0], 3, expected + pos,
					    cases[i].first_len);
			pos += cases[i].first_len;
			count++;
		}
		if (cases[i].second_len) {
			bvec_set_page(&bvec[count], pages[1],
				      cases[i].second_len, 7);
			xdr_bvec_test_write(pages[1], 7, expected + pos,
					    cases[i].second_len);
			pos += cases[i].second_len;
			count++;
		}
		memcpy(tail, expected + pos, cases[i].tail_len);
		page_len = cases[i].first_len + cases[i].second_len;
		memcpy(original, bvec, count * sizeof(*bvec));
		xdr_bvec_test_init_buf(&buf, count ? bvec : NULL, count, 0,
				       page_len, head, cases[i].head_len,
				       tail, cases[i].tail_len);

		xdr_init_decode(&xdr, &buf, NULL, NULL);
		xdr_set_scratch_buffer(&xdr, scratch, sizeof(scratch));
		p = xdr_inline_decode(&xdr, sizeof(expected));
		KUNIT_EXPECT_NOT_NULL(test, p);
		if (!p) {
			xdr_finish_decode(&xdr);
			return;
		}
		KUNIT_EXPECT_PTR_EQ(test, p, &scratch[0]);
		KUNIT_EXPECT_EQ(test, (unsigned long)p % __alignof__(*p), 0UL);
		KUNIT_EXPECT_MEMEQ(test, p, expected, sizeof(expected));
		KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), (size_t)0);
		xdr_finish_decode(&xdr);
		KUNIT_EXPECT_MEMEQ(test, bvec, original,
				   count * sizeof(*bvec));
	}

	{
		struct bio_vec bvec[2];
		struct xdr_stream xdr;
		struct xdr_buf buf;
		__be32 scratch[2];
		u8 head[2];
		u8 tail[2];
		u8 opaque[3];
		u32 len;
		int ret;

		memcpy(head, encoded_opaque, sizeof(head));
		bvec_set_page(&bvec[0], pages[0], 2, 3);
		bvec_set_page(&bvec[1], pages[1], 2, 7);
		xdr_bvec_test_write(pages[0], 3, encoded_opaque + 2, 2);
		xdr_bvec_test_write(pages[1], 7, encoded_opaque + 4, 2);
		memcpy(tail, encoded_opaque + 6, sizeof(tail));
		xdr_bvec_test_init_buf(&buf, bvec, ARRAY_SIZE(bvec), 0, 4,
				       head, sizeof(head), tail, sizeof(tail));

		xdr_init_decode(&xdr, &buf, NULL, NULL);
		xdr_set_scratch_buffer(&xdr, scratch, sizeof(scratch));
		ret = xdr_stream_decode_u32(&xdr, &len);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			goto out_opaque;
		KUNIT_EXPECT_EQ(test, len, 3U);
		memset(scratch, 0xcc, sizeof(scratch));
		ret = xdr_stream_decode_opaque_fixed(&xdr, opaque,
						     sizeof(opaque));
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			goto out_opaque;
		KUNIT_EXPECT_MEMEQ(test, opaque, encoded_opaque + 4,
				   sizeof(opaque));
		KUNIT_EXPECT_EQ(test, ((u8 *)scratch)[3], (u8)0xcc);
		KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), (size_t)0);
out_opaque:
		xdr_finish_decode(&xdr);
	}
}

static void xdr_bvec_nested_subsegment_test(struct kunit *test)
{
	static const u8 expected[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0x10, 0x11, 0x12, 0x13,
	};
	struct bio_vec converted[3];
	struct bio_vec original[3];
	struct bio_vec bvec[3];
	struct page *pages[3];
	struct xdr_buf stream_inner;
	struct xdr_buf stream_subbuf;
	struct xdr_buf direct_subbuf;
	struct xdr_buf inner;
	struct xdr_buf outer;
	struct xdr_stream xdr;
	struct xdr_buf buf;
	__be32 scratch[4];
	u8 head[4];
	u8 tail[4];
	u8 copied[16];
	bool success;
	__be32 *p;
	int ret;

	pages[0] = xdr_bvec_test_alloc_page(test);
	pages[1] = xdr_bvec_test_alloc_page(test);
	pages[2] = xdr_bvec_test_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, pages[0]);
	KUNIT_ASSERT_NOT_NULL(test, pages[1]);
	KUNIT_ASSERT_NOT_NULL(test, pages[2]);
	memcpy(head, expected, sizeof(head));
	memcpy(tail, expected + sizeof(expected) - sizeof(tail), sizeof(tail));
	bvec_set_page(&bvec[0], pages[0], 3, 1);
	bvec_set_page(&bvec[1], pages[1], 4, 7);
	bvec_set_page(&bvec[2], pages[2], 5, 11);
	xdr_bvec_test_write(pages[0], 1, expected + 4, 3);
	xdr_bvec_test_write(pages[1], 7, expected + 7, 4);
	xdr_bvec_test_write(pages[2], 11, expected + 11, 5);
	memcpy(original, bvec, sizeof(bvec));
	xdr_bvec_test_init_buf(&buf, bvec, ARRAY_SIZE(bvec), 0, 12,
			       head, sizeof(head), tail, sizeof(tail));

	ret = xdr_buf_subsegment(&buf, &direct_subbuf, 5, 9);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, direct_subbuf.page_mode, XDRBUF_PAGE_BVECS);
	KUNIT_EXPECT_EQ(test, direct_subbuf.head[0].iov_len, (size_t)0);
	KUNIT_EXPECT_EQ(test, direct_subbuf.page_len, 9U);
	KUNIT_EXPECT_EQ(test, direct_subbuf.tail[0].iov_len, (size_t)0);
	KUNIT_EXPECT_PTR_EQ(test, direct_subbuf.bvec, &bvec[0]);
	KUNIT_EXPECT_EQ(test, direct_subbuf.bvec_count, 3U);
	KUNIT_EXPECT_EQ(test, direct_subbuf.bvec_offset, 1U);
	KUNIT_ASSERT_EQ(test,
			read_bytes_from_xdr_buf(&direct_subbuf, 0, copied, 9),
			0);
	KUNIT_EXPECT_MEMEQ(test, copied, expected + 5, 9);
	ret = xdr_buf_to_bvec(converted, ARRAY_SIZE(converted),
			      &direct_subbuf);
	KUNIT_ASSERT_EQ(test, ret, 3);
	KUNIT_EXPECT_PTR_EQ(test, converted[0].bv_page, pages[0]);
	KUNIT_EXPECT_EQ(test, converted[0].bv_offset, 2U);
	KUNIT_EXPECT_EQ(test, converted[0].bv_len, 2U);
	KUNIT_EXPECT_PTR_EQ(test, converted[1].bv_page, pages[1]);
	KUNIT_EXPECT_EQ(test, converted[1].bv_offset, 7U);
	KUNIT_EXPECT_EQ(test, converted[1].bv_len, 4U);
	KUNIT_EXPECT_PTR_EQ(test, converted[2].bv_page, pages[2]);
	KUNIT_EXPECT_EQ(test, converted[2].bv_offset, 11U);
	KUNIT_EXPECT_EQ(test, converted[2].bv_len, 3U);

	ret = xdr_buf_subsegment(&buf, &outer, 2, 16);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = xdr_buf_subsegment(&outer, &inner, 1, 14);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test,
			read_bytes_from_xdr_buf(&inner, 0, copied, 14), 0);
	KUNIT_EXPECT_MEMEQ(test, copied, expected + 3, 14);

	xdr_init_decode(&xdr, &buf, NULL, NULL);
	xdr_set_scratch_buffer(&xdr, scratch, sizeof(scratch));
	p = xdr_inline_decode(&xdr, sizeof(head));
	KUNIT_EXPECT_NOT_NULL(test, p);
	if (!p)
		goto out_nested;
	KUNIT_EXPECT_MEMEQ(test, p, expected, sizeof(head));
	success = xdr_stream_subsegment(&xdr, &stream_subbuf, 12);
	KUNIT_EXPECT_TRUE(test, success);
	if (!success)
		goto out_nested;
	KUNIT_EXPECT_EQ(test, xdr_stream_pos(&xdr), 16U);
	KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), (size_t)4);
	ret = read_bytes_from_xdr_buf(&stream_subbuf, 0, copied, 12);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_nested;
	KUNIT_EXPECT_MEMEQ(test, copied, expected + 4, 12);
	ret = xdr_buf_subsegment(&stream_subbuf, &stream_inner, 1, 10);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_nested;
	ret = read_bytes_from_xdr_buf(&stream_inner, 0, copied, 10);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_nested;
	KUNIT_EXPECT_MEMEQ(test, copied, expected + 5, 10);
	p = xdr_inline_decode(&xdr, sizeof(tail));
	KUNIT_EXPECT_NOT_NULL(test, p);
	if (!p)
		goto out_nested;
	KUNIT_EXPECT_MEMEQ(test, p, expected + 16, sizeof(tail));
	KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), (size_t)0);
out_nested:
	xdr_finish_decode(&xdr);
	KUNIT_EXPECT_MEMEQ(test, bvec, original, sizeof(bvec));
}

enum xdr_bvec_bad_geometry {
	XDR_BVEC_BAD_SHORT_COUNT,
	XDR_BVEC_BAD_LONG_COUNT,
	XDR_BVEC_BAD_LONG_LENGTH,
	XDR_BVEC_BAD_NULL_ARRAY,
	XDR_BVEC_BAD_EMPTY_COUNT,
	XDR_BVEC_BAD_NULL_PAGE,
	XDR_BVEC_BAD_ZERO_ENTRY,
	XDR_BVEC_BAD_PAGE_BASE,
	XDR_BVEC_BAD_PAGE_MODE,
	XDR_BVEC_BAD_LEN_OVER_STORAGE,
	XDR_BVEC_BAD_STORAGE_OVERFLOW,
	XDR_BVEC_BAD_OFFSET_PAST_PAGE,
	XDR_BVEC_BAD_MULTIPAGE,
	XDR_BVEC_BAD_FIRST_OFFSET,
	XDR_BVEC_BAD_ZERO_PAGE_EXTRA,
	XDR_BVEC_BAD_GEOMETRY_COUNT,
};

static void xdr_bvec_malformed_matrix_test(struct kunit *test)
{
	struct page *pages[2];
	enum xdr_bvec_bad_geometry kind;

	pages[0] = xdr_bvec_test_alloc_page(test);
	pages[1] = xdr_bvec_test_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, pages[0]);
	KUNIT_ASSERT_NOT_NULL(test, pages[1]);

	for (kind = 0; kind < XDR_BVEC_BAD_GEOMETRY_COUNT; kind++) {
		struct bio_vec converted[2];
		struct bio_vec bvec[2];
		struct xdr_stream xdr;
		struct xdr_buf buf;
		u8 copied;

		bvec_set_page(&bvec[0], pages[0], 4, 1);
		bvec_set_page(&bvec[1], pages[1], 4, 7);
		xdr_bvec_test_init_buf(&buf, bvec, ARRAY_SIZE(bvec), 0, 8,
				       NULL, 0, NULL, 0);
		switch (kind) {
		case XDR_BVEC_BAD_SHORT_COUNT:
			buf.bvec_count = 1;
			break;
		case XDR_BVEC_BAD_LONG_COUNT:
			buf.page_len = 4;
			buf.len = 4;
			buf.buflen = 4;
			break;
		case XDR_BVEC_BAD_LONG_LENGTH:
			buf.page_len = 9;
			buf.len = 9;
			buf.buflen = 9;
			break;
		case XDR_BVEC_BAD_NULL_ARRAY:
			buf.bvec = NULL;
			break;
		case XDR_BVEC_BAD_EMPTY_COUNT:
			buf.bvec_count = 0;
			break;
		case XDR_BVEC_BAD_NULL_PAGE:
			bvec[0].bv_page = NULL;
			break;
		case XDR_BVEC_BAD_ZERO_ENTRY:
			bvec[0].bv_len = 0;
			break;
		case XDR_BVEC_BAD_PAGE_BASE:
			buf.page_base = 1;
			break;
		case XDR_BVEC_BAD_PAGE_MODE:
			buf.page_mode = (enum xdr_buf_page_mode)-1;
			break;
		case XDR_BVEC_BAD_LEN_OVER_STORAGE:
			buf.len = 9;
			break;
		case XDR_BVEC_BAD_STORAGE_OVERFLOW:
			buf.head[0].iov_len = SIZE_MAX;
			buf.tail[0].iov_len = 1;
			buf.len = 1;
			buf.buflen = 1;
			break;
		case XDR_BVEC_BAD_OFFSET_PAST_PAGE:
			bvec[0].bv_offset = PAGE_SIZE + 1;
			bvec[0].bv_len = 1;
			break;
		case XDR_BVEC_BAD_MULTIPAGE:
			bvec[0].bv_offset = PAGE_SIZE - 1;
			bvec[0].bv_len = 2;
			break;
		case XDR_BVEC_BAD_FIRST_OFFSET:
			buf.bvec_offset = bvec[0].bv_len;
			break;
		case XDR_BVEC_BAD_ZERO_PAGE_EXTRA:
			buf.page_len = 0;
			buf.len = 0;
			buf.buflen = 0;
			break;
		case XDR_BVEC_BAD_GEOMETRY_COUNT:
			break;
		}

		KUNIT_EXPECT_EQ(test,
				read_bytes_from_xdr_buf(&buf, 0, &copied, 1), -1);
		KUNIT_EXPECT_EQ(test,
				xdr_buf_to_bvec(converted, ARRAY_SIZE(converted),
						&buf),
				-EINVAL);
		xdr_init_decode(&xdr, &buf, NULL, NULL);
		KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), (size_t)0);
		KUNIT_EXPECT_PTR_EQ(test,
				    xdr_inline_decode(&xdr, sizeof(__be32)), NULL);
		xdr_finish_decode(&xdr);
	}
}

static void xdr_bvec_bounds_and_scratch_test(struct kunit *test)
{
	static const u8 expected[] = {
		0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
	};
	struct page *pages[2];
	struct bio_vec bvec[2];
	struct xdr_stream xdr;
	struct xdr_buf subbuf;
	struct xdr_buf buf;
	struct page **page_ptr;
	struct kvec *iov;
	__be32 scratch[2];
	__be32 *p_before;
	__be32 *end_before;
	unsigned int nwords;
	size_t remaining;
	unsigned int pos;
	__be32 *p;

	pages[0] = xdr_bvec_test_alloc_page(test);
	pages[1] = xdr_bvec_test_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, pages[0]);
	KUNIT_ASSERT_NOT_NULL(test, pages[1]);
	xdr_bvec_test_fill(pages[0], 0xa5);
	xdr_bvec_test_fill(pages[1], 0xa5);
	bvec_set_page(&bvec[0], pages[0], 2, 1);
	bvec_set_page(&bvec[1], pages[1], 6, 7);
	xdr_bvec_test_write(pages[0], 1, expected, 2);
	xdr_bvec_test_write(pages[1], 7, expected + 2, 6);
	xdr_bvec_test_init_buf(&buf, bvec, ARRAY_SIZE(bvec), 0, 8,
			       NULL, 0, NULL, 0);

	KUNIT_EXPECT_EQ(test,
			read_bytes_from_xdr_buf(&buf, 7, scratch, 2), -1);
	KUNIT_EXPECT_EQ(test,
			xdr_buf_subsegment(&buf, &subbuf, UINT_MAX, 1), -1);
	KUNIT_EXPECT_EQ(test,
			xdr_buf_subsegment(&buf, &subbuf, 1, UINT_MAX), -1);

	xdr_init_decode(&xdr, &buf, NULL, NULL);
	xdr_set_scratch_buffer(&xdr, scratch, sizeof(__be32) - 1);
	pos = xdr_stream_pos(&xdr);
	remaining = xdr_stream_remaining(&xdr);
	p_before = xdr.p;
	end_before = xdr.end;
	page_ptr = xdr.page_ptr;
	iov = xdr.iov;
	nwords = xdr.nwords;
	KUNIT_EXPECT_PTR_EQ(test,
			    xdr_inline_decode(&xdr, sizeof(__be32)), NULL);
	KUNIT_EXPECT_EQ(test, xdr_stream_pos(&xdr), pos);
	KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), remaining);
	KUNIT_EXPECT_PTR_EQ(test, xdr.p, p_before);
	KUNIT_EXPECT_PTR_EQ(test, xdr.end, end_before);
	KUNIT_EXPECT_PTR_EQ(test, xdr.page_ptr, page_ptr);
	KUNIT_EXPECT_PTR_EQ(test, xdr.iov, iov);
	KUNIT_EXPECT_EQ(test, xdr.nwords, nwords);

	xdr_set_scratch_buffer(&xdr, (u8 *)scratch + 1, sizeof(__be32));
	KUNIT_EXPECT_PTR_EQ(test,
			    xdr_inline_decode(&xdr, sizeof(__be32)), NULL);
	KUNIT_EXPECT_EQ(test, xdr_stream_pos(&xdr), pos);
	KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), remaining);
	KUNIT_EXPECT_PTR_EQ(test, xdr.p, p_before);
	KUNIT_EXPECT_PTR_EQ(test, xdr.end, end_before);
	KUNIT_EXPECT_PTR_EQ(test, xdr.page_ptr, page_ptr);
	KUNIT_EXPECT_PTR_EQ(test, xdr.iov, iov);
	KUNIT_EXPECT_EQ(test, xdr.nwords, nwords);

	xdr_set_scratch_buffer(&xdr, scratch, sizeof(__be32));
	p = xdr_inline_decode(&xdr, sizeof(__be32));
	KUNIT_EXPECT_NOT_NULL(test, p);
	if (!p)
		goto out_scratch;
	KUNIT_EXPECT_PTR_EQ(test, p, &scratch[0]);
	KUNIT_EXPECT_MEMEQ(test, p, expected, sizeof(__be32));
	pos = xdr_stream_pos(&xdr);
	remaining = xdr_stream_remaining(&xdr);
	KUNIT_EXPECT_PTR_EQ(test, xdr_inline_decode(&xdr, SIZE_MAX), NULL);
	KUNIT_EXPECT_EQ(test, xdr_stream_pos(&xdr), pos);
	KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), remaining);
	p = xdr_inline_decode(&xdr, sizeof(__be32));
	KUNIT_EXPECT_NOT_NULL(test, p);
	if (!p)
		goto out_scratch;
	KUNIT_EXPECT_PTR_EQ(test, p, &scratch[0]);
	KUNIT_EXPECT_MEMEQ(test, p, expected + sizeof(__be32),
			   sizeof(__be32));
	KUNIT_EXPECT_EQ(test, xdr_stream_remaining(&xdr), (size_t)0);
out_scratch:
	xdr_finish_decode(&xdr);

	KUNIT_EXPECT_EQ(test, xdr_bvec_test_read_byte(pages[0], 0),
			(u8)0xa5);
	KUNIT_EXPECT_EQ(test, xdr_bvec_test_read_byte(pages[0], 3),
			(u8)0xa5);
	KUNIT_EXPECT_EQ(test, xdr_bvec_test_read_byte(pages[1], 6),
			(u8)0xa5);
	KUNIT_EXPECT_EQ(test, xdr_bvec_test_read_byte(pages[1], 13),
			(u8)0xa5);
}

static void xdr_bvec_mutation_rejection_test(struct kunit *test)
{
	static const u8 expected[] = {
		0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	};
	struct page *page = xdr_bvec_test_alloc_page(test);
	struct xdr_buf original_buf;
	struct bio_vec original;
	struct xdr_stream decode;
	struct xdr_stream encode;
	struct bio_vec bvec;
	struct xdr_buf buf;
	u8 replacement[sizeof(expected)] = { 0 };
	u8 copied[sizeof(expected)];
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, page);
	bvec_set_page(&bvec, page, sizeof(expected), 3);
	xdr_bvec_test_write(page, 3, expected, sizeof(expected));
	xdr_bvec_test_init_buf(&buf, &bvec, 1, 0, sizeof(expected),
			       NULL, 0, NULL, 0);
	original = bvec;
	original_buf = buf;

	KUNIT_EXPECT_EQ(test, xdr_alloc_bvec(&buf, GFP_KERNEL),
			-EOPNOTSUPP);
	xdr_free_bvec(&buf);
	ret = write_bytes_to_xdr_buf(&buf, 0, replacement,
				     sizeof(replacement));
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, xdr_encode_word(&buf, 0, 0), -EOPNOTSUPP);
	xdr_buf_trim(&buf, 1);
	xdr_inline_pages(&buf, 1, &page, 0, 1);
	xdr_terminate_string(&buf, 0);

	xdr_init_decode(&decode, &buf, NULL, NULL);
	KUNIT_EXPECT_EQ(test,
			xdr_stream_zero(&decode, 0, sizeof(expected)), 0U);
	KUNIT_EXPECT_EQ(test,
			xdr_stream_move_subsegment(&decode, 0, 4, 4), 0U);
	KUNIT_EXPECT_EQ(test, xdr_read_pages(&decode, 4), 0U);
	xdr_set_pagelen(&decode, 4);
	xdr_enter_page(&decode, 4);
	xdr_truncate_decode(&decode, 4);
	KUNIT_EXPECT_EQ(test, xdr_restrict_buflen(&decode, 4),
			-EOPNOTSUPP);
	xdr_write_pages(&decode, &page, 0, 4);
	xdr_finish_decode(&decode);

	xdr_init_encode(&encode, &buf, NULL, NULL);
	KUNIT_EXPECT_PTR_EQ(test, encode.p, NULL);
	KUNIT_EXPECT_PTR_EQ(test, encode.end, NULL);
	KUNIT_EXPECT_EQ(test, xdr_reserve_space_vec(&encode, 4),
			-EOPNOTSUPP);
	xdr_truncate_encode(&encode, 0);
	xdr_commit_encode(&encode);
	xdr_init_encode_pages(&encode, &buf);
	KUNIT_EXPECT_PTR_EQ(test, encode.p, NULL);
	KUNIT_EXPECT_PTR_EQ(test, encode.end, NULL);

	KUNIT_EXPECT_MEMEQ(test, &buf, &original_buf, sizeof(buf));
	KUNIT_EXPECT_MEMEQ(test, &bvec, &original, sizeof(bvec));
	KUNIT_ASSERT_EQ(test,
			read_bytes_from_xdr_buf(&buf, 0, copied, sizeof(copied)),
			0);
	KUNIT_EXPECT_MEMEQ(test, copied, expected, sizeof(expected));
}

static void xdr_bvec_legacy_parity_test(struct kunit *test)
{
	static const u8 expected[] = {
		0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
		0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
	};
	struct bio_vec authoritative_converted[4];
	struct bio_vec legacy_converted[4];
	struct bio_vec authoritative_original[2];
	struct bio_vec authoritative_bvec[2];
	struct page *pages[2];
	struct xdr_buf authoritative;
	struct xdr_buf legacy;
	u8 authoritative_bytes[18];
	u8 legacy_bytes[18];
	u8 *head;
	u8 *tail;
	int ret;

	pages[0] = xdr_bvec_test_alloc_page(test);
	pages[1] = xdr_bvec_test_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, pages[0]);
	KUNIT_ASSERT_NOT_NULL(test, pages[1]);
	head = kunit_kmalloc(test, XDR_UNIT, GFP_KERNEL);
	tail = kunit_kmalloc(test, XDR_UNIT, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, head);
	KUNIT_ASSERT_NOT_NULL(test, tail);
	xdr_bvec_test_fill(pages[0], 0xa5);
	xdr_bvec_test_fill(pages[1], 0xa5);
	memcpy(head, expected, XDR_UNIT);
	memcpy(tail, expected + 12, XDR_UNIT);
	xdr_bvec_test_write(pages[0], PAGE_SIZE - 3, expected + 4, 3);
	xdr_bvec_test_write(pages[1], 0, expected + 7, 5);

	memset(&legacy, 0, sizeof(legacy));
	legacy.head[0].iov_base = head;
	legacy.head[0].iov_len = XDR_UNIT;
	legacy.pages = pages;
	legacy.page_base = PAGE_SIZE - 3;
	legacy.page_len = 8;
	legacy.tail[0].iov_base = tail;
	legacy.tail[0].iov_len = XDR_UNIT;
	legacy.page_mode = XDRBUF_PAGE_ARRAY;
	legacy.len = sizeof(expected);
	legacy.buflen = sizeof(expected);

	bvec_set_page(&authoritative_bvec[0], pages[0], 3,
		      PAGE_SIZE - 3);
	bvec_set_page(&authoritative_bvec[1], pages[1], 5, 0);
	memcpy(authoritative_original, authoritative_bvec,
	       sizeof(authoritative_bvec));
	xdr_bvec_test_init_buf(&authoritative, authoritative_bvec,
			       ARRAY_SIZE(authoritative_bvec), 0, 8,
			       head, XDR_UNIT, tail, XDR_UNIT);

	memset(legacy_bytes, 0x5a, sizeof(legacy_bytes));
	memset(authoritative_bytes, 0x5a, sizeof(authoritative_bytes));
	ret = read_bytes_from_xdr_buf(&legacy, 0, legacy_bytes + 1,
				      sizeof(expected));
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = read_bytes_from_xdr_buf(&authoritative, 0,
				      authoritative_bytes + 1,
				      sizeof(expected));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, legacy_bytes[0], (u8)0x5a);
	KUNIT_EXPECT_EQ(test, legacy_bytes[17], (u8)0x5a);
	KUNIT_EXPECT_EQ(test, authoritative_bytes[0], (u8)0x5a);
	KUNIT_EXPECT_EQ(test, authoritative_bytes[17], (u8)0x5a);
	KUNIT_EXPECT_MEMEQ(test, legacy_bytes + 1, expected,
			   sizeof(expected));
	KUNIT_EXPECT_MEMEQ(test, authoritative_bytes + 1, legacy_bytes + 1,
			   sizeof(expected));

	ret = xdr_buf_to_bvec(legacy_converted,
			      ARRAY_SIZE(legacy_converted), &legacy);
	KUNIT_ASSERT_EQ(test, ret, 4);
	ret = xdr_buf_to_bvec(authoritative_converted,
			      ARRAY_SIZE(authoritative_converted),
			      &authoritative);
	KUNIT_ASSERT_EQ(test, ret, 4);
	KUNIT_EXPECT_MEMEQ(test, authoritative_converted, legacy_converted,
			   sizeof(legacy_converted));

	KUNIT_EXPECT_EQ(test, xdr_buf_pagecount(&legacy), (size_t)2);
	KUNIT_ASSERT_EQ(test, xdr_alloc_bvec(&legacy, GFP_KERNEL), 0);
	KUNIT_ASSERT_NOT_NULL(test, legacy.bvec);
	KUNIT_EXPECT_EQ(test, legacy.bvec_count, 0U);
	KUNIT_EXPECT_PTR_EQ(test, legacy.bvec[0].bv_page, pages[0]);
	KUNIT_EXPECT_EQ(test, legacy.bvec[0].bv_offset, 0U);
	KUNIT_EXPECT_EQ(test, legacy.bvec[0].bv_len, (unsigned int)PAGE_SIZE);
	KUNIT_EXPECT_PTR_EQ(test, legacy.bvec[1].bv_page, pages[1]);
	KUNIT_EXPECT_EQ(test, legacy.bvec[1].bv_offset, 0U);
	KUNIT_EXPECT_EQ(test, legacy.bvec[1].bv_len, (unsigned int)PAGE_SIZE);
	xdr_free_bvec(&legacy);
	KUNIT_EXPECT_PTR_EQ(test, legacy.bvec, NULL);
	xdr_free_bvec(&legacy);
	KUNIT_EXPECT_PTR_EQ(test, legacy.bvec, NULL);

	KUNIT_EXPECT_EQ(test, xdr_alloc_bvec(&authoritative, GFP_KERNEL),
			-EOPNOTSUPP);
	xdr_free_bvec(&authoritative);
	KUNIT_EXPECT_PTR_EQ(test, authoritative.bvec,
			    &authoritative_bvec[0]);
	KUNIT_EXPECT_MEMEQ(test, authoritative_bvec,
			   authoritative_original,
			   sizeof(authoritative_bvec));
	KUNIT_EXPECT_EQ(test, xdr_bvec_test_read_byte(pages[0], PAGE_SIZE - 4),
			(u8)0xa5);
	KUNIT_EXPECT_EQ(test, xdr_bvec_test_read_byte(pages[1], 5),
			(u8)0xa5);
}

static struct kunit_case xdr_bvec_test_cases[] = {
	KUNIT_CASE(xdr_bvec_offset_matrix_test),
	KUNIT_CASE(xdr_bvec_component_matrix_test),
	KUNIT_CASE(xdr_bvec_nested_subsegment_test),
	KUNIT_CASE(xdr_bvec_malformed_matrix_test),
	KUNIT_CASE(xdr_bvec_bounds_and_scratch_test),
	KUNIT_CASE(xdr_bvec_mutation_rejection_test),
	KUNIT_CASE(xdr_bvec_legacy_parity_test),
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
