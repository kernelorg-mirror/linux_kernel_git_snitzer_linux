// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <linux/bvec.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nfs4.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/xdr.h>

#include "nfsd.h"
#include "xdr4.h"

static void nfsd4_bvec_free_page(void *data)
{
	__free_page(data);
}

static struct page *nfsd4_bvec_alloc_page(struct kunit *test)
{
	struct page *page = alloc_page(GFP_KERNEL);

	if (!page)
		return NULL;
	if (kunit_add_action_or_reset(test, nfsd4_bvec_free_page, page))
		return NULL;
	return page;
}

static void nfsd4_bvec_write(struct page *page, unsigned int offset,
			     const void *source, unsigned int length)
{
	void *address = kmap_local_page(page);

	memcpy(address + offset, source, length);
	kunmap_local(address);
}

static void nfsd4_bvec_init_buf(struct xdr_buf *buf, void *head,
				unsigned int head_len, unsigned int page_len)
{
	memset(buf, 0, sizeof(*buf));
	buf->head[0].iov_base = head;
	buf->head[0].iov_len = head_len;
	buf->page_len = page_len;
	buf->len = head_len + page_len;
	buf->buflen = buf->len;
}

#define NFSD4_BVEC_MAX_SEGMENTS	4U

struct nfsd4_bvec_fixture {
	struct page		*pages[NFSD4_BVEC_MAX_SEGMENTS];
	struct bio_vec		bvec[NFSD4_BVEC_MAX_SEGMENTS];
	struct xdr_buf		buf;
	unsigned int		count;
};

struct nfsd4_bvec_decode_context {
	struct auth_ops			 auth;
	struct svc_xprt_class		 xprt_class;
	struct svc_xprt			 xprt;
	struct svc_serv			 serv;
	struct svc_rqst			 rqst;
	struct nfsd_thread_local_info ntli;
	struct nfsd4_compoundargs	*args;
	struct xdr_stream		 stream;
};

static struct nfsd4_bvec_fixture *
nfsd4_bvec_fixture_alloc(struct kunit *test, const void *wire,
			 unsigned int wire_len, const unsigned int *lengths,
			 unsigned int count)
{
	struct nfsd4_bvec_fixture *fixture;
	unsigned int consumed = 0;
	unsigned int i;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_GT(test, count, 0U);
	KUNIT_ASSERT_LE(test, count, NFSD4_BVEC_MAX_SEGMENTS);

	for (i = 0; i < count; i++) {
		unsigned int offset = 17 + i * 23;

		KUNIT_ASSERT_GT(test, lengths[i], 0U);
		KUNIT_ASSERT_LE(test, lengths[i], wire_len - consumed);
		fixture->pages[i] = nfsd4_bvec_alloc_page(test);
		KUNIT_ASSERT_NOT_NULL(test, fixture->pages[i]);
		bvec_set_page(&fixture->bvec[i], fixture->pages[i],
			      lengths[i], offset);
		nfsd4_bvec_write(fixture->pages[i], offset, wire + consumed, lengths[i]);
		consumed += lengths[i];
	}
	KUNIT_ASSERT_EQ(test, consumed, wire_len);

	nfsd4_bvec_init_buf(&fixture->buf, (void *)wire, 0, wire_len);
	fixture->buf.bvec = fixture->bvec;
	fixture->buf.bvec_count = count;
	fixture->buf.page_mode = XDRBUF_PAGE_BVECS;
	fixture->count = count;
	return fixture;
}

static void nfsd4_bvec_fixture_poison(struct nfsd4_bvec_fixture *fixture)
{
	unsigned int i;

	for (i = 0; i < fixture->count; i++) {
		struct bio_vec *bvec = &fixture->bvec[i];
		void *address = kmap_local_page(bvec->bv_page);

		memset(address + bvec->bv_offset, 0xa5, bvec->bv_len);
		kunmap_local(address);
	}
}

static void nfsd4_decode_release(void *data)
{
	struct nfsd4_bvec_decode_context *context = data;

	nfsd4_release_compoundargs(&context->rqst);
	xdr_finish_decode(&context->stream);
}

static struct nfsd4_bvec_decode_context *
nfsd4_bvec_decode_context_alloc(struct kunit *test, struct xdr_buf *buf)
{
	struct nfsd4_bvec_decode_context *context;
	void *scratch;
	int ret;

	context = kunit_kzalloc(test, sizeof(*context), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, context);
	context->args = kunit_kzalloc(test, sizeof(*context->args), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, context->args);
	scratch = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, scratch);

	context->auth.flavour = RPC_AUTH_NULL;
	context->xprt_class.xcl_max_payload = PAGE_SIZE * 4;
	context->xprt.xpt_class = &context->xprt_class;
	context->serv.sv_max_payload = PAGE_SIZE * 4;
	context->rqst.rq_authop = &context->auth;
	context->rqst.rq_xprt = &context->xprt;
	context->rqst.rq_server = &context->serv;
	context->rqst.rq_argp = context->args;
	context->rqst.rq_private = &context->ntli;
	xdr_init_decode(&context->stream, buf, NULL, NULL);
	xdr_set_scratch_buffer(&context->stream, scratch, PAGE_SIZE);
	ret = kunit_add_action_or_reset(test, nfsd4_decode_release, context);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return context;
}

static __be32 *nfsd4_bvec_encode_compound_header(__be32 *p, u32 minorversion,
						 u32 opcnt)
{
	*p++ = xdr_zero;
	*p++ = cpu_to_be32(minorversion);
	*p++ = cpu_to_be32(opcnt);
	return p;
}

static __be32 *nfsd4_bvec_encode_stateid(__be32 *p)
{
	memset(p, 0, NFS4_STATEID_SIZE);
	return p + XDR_QUADLEN(NFS4_STATEID_SIZE);
}

static void nfsd4_bvec_setxattr_decode_test(struct kunit *test)
{
	static const char name[] = "alpha";
	static const char expected_name[] = "user.alpha";
	static const u8 value[] = {
		0x10, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
		0x55, 0xaa, 0x31, 0x42, 0x53, 0x64, 0x75, 0x86,
		0x97, 0xa8, 0xb9,
	};
	struct nfsd4_bvec_decode_context *context;
	struct nfsd4_bvec_fixture *fixture;
	struct nfsd4_setxattr *setxattr;
	unsigned int lengths[4];
	unsigned int wire_len;
	__be32 *wire;
	__be32 *p;
	bool decoded;

	wire = kunit_kzalloc(test, 256, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, wire);
	p = nfsd4_bvec_encode_compound_header(wire, 2, 1);
	*p++ = cpu_to_be32(OP_SETXATTR);
	*p++ = cpu_to_be32(SETXATTR4_EITHER);
	p = xdr_encode_opaque(p, name, sizeof(name) - 1);
	*p++ = cpu_to_be32(sizeof(value));
	p = xdr_encode_opaque_fixed(p, value, sizeof(value));
	wire_len = (void *)p - (void *)wire;
	lengths[0] = 3;
	lengths[1] = 5;
	lengths[2] = 11;
	lengths[3] = wire_len - lengths[0] - lengths[1] - lengths[2];

	fixture = nfsd4_bvec_fixture_alloc(test, wire, wire_len, lengths,
					   ARRAY_SIZE(lengths));
	context = nfsd4_bvec_decode_context_alloc(test, &fixture->buf);
	decoded = nfs4svc_decode_compoundargs(&context->rqst, &context->stream);
	KUNIT_ASSERT_TRUE(test, decoded);
	KUNIT_ASSERT_EQ(test, context->args->opcnt, 1U);
	KUNIT_ASSERT_EQ(test, context->args->ops[0].opnum, (u32)OP_SETXATTR);
	KUNIT_ASSERT_EQ(test, (__force u32)context->args->ops[0].status,
			(__force u32)nfs_ok);

	setxattr = &context->args->ops[0].u.setxattr;
	KUNIT_ASSERT_NOT_NULL(test, setxattr->setxa_name);
	KUNIT_ASSERT_NOT_NULL(test, setxattr->setxa_buf);
	KUNIT_EXPECT_STREQ(test, setxattr->setxa_name, expected_name);
	KUNIT_EXPECT_EQ(test, setxattr->setxa_len, (u32)sizeof(value));
	KUNIT_EXPECT_MEMEQ(test, setxattr->setxa_buf, value, sizeof(value));

	nfsd4_bvec_fixture_poison(fixture);
	memset(context->stream.scratch.iov_base, 0x5a,
	       context->stream.scratch.iov_len);
	KUNIT_EXPECT_STREQ(test, setxattr->setxa_name, expected_name);
	KUNIT_EXPECT_MEMEQ(test, setxattr->setxa_buf, value, sizeof(value));
}

static void nfsd4_bvec_compound_boundaries_test(struct kunit *test)
{
	static const u8 payload[] = {
		0x02, 0x13, 0x24, 0x35, 0x46, 0x57, 0x68,
	};
	struct nfsd4_bvec_decode_context *context;
	struct nfsd4_bvec_fixture *fixture;
	struct nfsd4_write *write;
	unsigned int after_start;
	unsigned int lengths[4];
	unsigned int payload_start;
	unsigned int wire_len;
	u8 actual[sizeof(payload)];
	__be32 *wire;
	__be32 *p;
	bool decoded;
	int ret;

	wire = kunit_kzalloc(test, 256, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, wire);
	p = nfsd4_bvec_encode_compound_header(wire, 2, 3);
	*p++ = cpu_to_be32(OP_ACCESS);
	*p++ = cpu_to_be32(NFS4_ACCESS_READ);
	*p++ = cpu_to_be32(OP_WRITE);
	p = nfsd4_bvec_encode_stateid(p);
	p = xdr_encode_hyper(p, 0x0102030405060708ULL);
	*p++ = cpu_to_be32(NFS_DATA_SYNC);
	*p++ = cpu_to_be32(sizeof(payload));
	payload_start = (void *)p - (void *)wire;
	p = xdr_encode_opaque_fixed(p, payload, sizeof(payload));
	after_start = (void *)p - (void *)wire;
	*p++ = cpu_to_be32(OP_ACCESS);
	*p++ = cpu_to_be32(NFS4_ACCESS_LOOKUP);
	wire_len = (void *)p - (void *)wire;

	lengths[0] = 5;
	lengths[1] = payload_start + 2 - lengths[0];
	lengths[2] = after_start + 2 - lengths[0] - lengths[1];
	lengths[3] = wire_len - lengths[0] - lengths[1] - lengths[2];
	fixture = nfsd4_bvec_fixture_alloc(test, wire, wire_len, lengths,
					   ARRAY_SIZE(lengths));
	context = nfsd4_bvec_decode_context_alloc(test, &fixture->buf);
	decoded = nfs4svc_decode_compoundargs(&context->rqst, &context->stream);
	KUNIT_ASSERT_TRUE(test, decoded);
	KUNIT_ASSERT_EQ(test, context->args->opcnt, 3U);
	KUNIT_EXPECT_EQ(test, context->args->ops[0].opnum, (u32)OP_ACCESS);
	KUNIT_EXPECT_EQ(test, context->args->ops[1].opnum, (u32)OP_WRITE);
	KUNIT_EXPECT_EQ(test, context->args->ops[2].opnum, (u32)OP_ACCESS);
	KUNIT_EXPECT_EQ(test, (__force u32)context->args->ops[0].status,
			(__force u32)nfs_ok);
	KUNIT_EXPECT_EQ(test, (__force u32)context->args->ops[1].status,
			(__force u32)nfs_ok);
	KUNIT_EXPECT_EQ(test, (__force u32)context->args->ops[2].status,
			(__force u32)nfs_ok);

	write = &context->args->ops[1].u.write;
	KUNIT_EXPECT_EQ(test, write->wr_buflen, (u32)sizeof(payload));
	KUNIT_EXPECT_EQ(test, write->wr_payload.page_mode, XDRBUF_PAGE_BVECS);
	ret = read_bytes_from_xdr_buf(&write->wr_payload, 0, actual,
				      write->wr_buflen);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, actual, payload, sizeof(payload));
}

struct nfsd4_savemem_context {
	struct svc_rqst			rqst;
	struct nfsd4_compoundargs	args;
	struct xdr_stream		xdr;
};

static void nfsd4_savemem_release(void *data)
{
	struct nfsd4_savemem_context *context = data;

	nfsd4_release_compoundargs(&context->rqst);
}

static void nfsd4_bvec_savemem_lifetime_test(struct kunit *test)
{
	static const u8 expected[] = { 0x21, 0x43, 0x65, 0x87, 0xa9 };
	struct nfsd4_savemem_context *context;
	u8 *direct;
	u8 *saved;
	u8 *scratch;
	int ret;

	context = kunit_kzalloc(test, sizeof(*context), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, context);
	direct = kunit_kmalloc(test, sizeof(expected), GFP_KERNEL);
	scratch = kunit_kmalloc(test, sizeof(expected), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, direct);
	KUNIT_ASSERT_NOT_NULL(test, scratch);
	memcpy(direct, expected, sizeof(expected));
	memcpy(scratch, expected, sizeof(expected));
	context->rqst.rq_argp = &context->args;
	context->args.xdr = &context->xdr;
	context->xdr.scratch.iov_base = scratch;
	context->xdr.scratch.iov_len = sizeof(expected);
	ret = kunit_add_action_or_reset(test, nfsd4_savemem_release, context);
	KUNIT_ASSERT_EQ(test, ret, 0);

	saved = nfsd4_kunit_savemem(&context->args, (__be32 *)scratch,
				    sizeof(expected));
	KUNIT_ASSERT_NOT_NULL(test, saved);
	KUNIT_EXPECT_PTR_NE(test, saved, scratch);
	memset(scratch, 0x5a, sizeof(expected));
	KUNIT_EXPECT_MEMEQ(test, saved, expected, sizeof(expected));

	saved = nfsd4_kunit_savemem(&context->args, (__be32 *)direct,
				    sizeof(expected));
	KUNIT_EXPECT_PTR_EQ(test, saved, direct);
}

#if IS_ENABLED(CONFIG_NFSD_PNFS)
static void nfsd4_bvec_layoutreturn_lifetime_test(struct kunit *test)
{
	static const u8 body[] = { 0xde, 0xad, 0xbe, 0xef, 0x31, 0x42, 0x53 };
	static const char lookup[] = "later";
	struct nfsd4_bvec_decode_context *context;
	struct nfsd4_bvec_fixture *fixture;
	struct nfsd4_layoutreturn *layoutreturn;
	unsigned int body_start;
	unsigned int lengths[3];
	unsigned int lookup_start;
	unsigned int wire_len;
	__be32 *wire;
	__be32 *p;
	bool decoded;

	wire = kunit_kzalloc(test, 256, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, wire);
	p = nfsd4_bvec_encode_compound_header(wire, 1, 2);
	*p++ = cpu_to_be32(OP_LAYOUTRETURN);
	*p++ = xdr_zero;
	*p++ = cpu_to_be32(LAYOUT_NFSV4_1_FILES);
	*p++ = cpu_to_be32(IOMODE_ANY);
	*p++ = cpu_to_be32(RETURN_FILE);
	p = xdr_encode_hyper(p, 0);
	p = xdr_encode_hyper(p, NFS4_MAX_UINT64);
	p = nfsd4_bvec_encode_stateid(p);
	*p++ = cpu_to_be32(sizeof(body));
	body_start = (void *)p - (void *)wire;
	p = xdr_encode_opaque_fixed(p, body, sizeof(body));
	*p++ = cpu_to_be32(OP_LOOKUP);
	*p++ = cpu_to_be32(sizeof(lookup) - 1);
	lookup_start = (void *)p - (void *)wire;
	p = xdr_encode_opaque_fixed(p, lookup, sizeof(lookup) - 1);
	wire_len = (void *)p - (void *)wire;

	lengths[0] = body_start + 2;
	lengths[1] = lookup_start + 1 - lengths[0];
	lengths[2] = wire_len - lengths[0] - lengths[1];
	fixture = nfsd4_bvec_fixture_alloc(test, wire, wire_len, lengths,
					   ARRAY_SIZE(lengths));
	context = nfsd4_bvec_decode_context_alloc(test, &fixture->buf);
	decoded = nfs4svc_decode_compoundargs(&context->rqst, &context->stream);
	KUNIT_ASSERT_TRUE(test, decoded);
	KUNIT_ASSERT_EQ(test, context->args->opcnt, 2U);
	KUNIT_ASSERT_EQ(test, context->args->ops[0].opnum,
			(u32)OP_LAYOUTRETURN);
	KUNIT_ASSERT_EQ(test, context->args->ops[1].opnum, (u32)OP_LOOKUP);
	KUNIT_ASSERT_EQ(test, (__force u32)context->args->ops[0].status,
			(__force u32)nfs_ok);
	KUNIT_ASSERT_EQ(test, (__force u32)context->args->ops[1].status,
			(__force u32)nfs_ok);

	layoutreturn = &context->args->ops[0].u.layoutreturn;
	KUNIT_ASSERT_NOT_NULL(test, layoutreturn->lrf_body);
	KUNIT_EXPECT_EQ(test, layoutreturn->lrf_body_len, (u32)sizeof(body));
	KUNIT_EXPECT_MEMEQ(test, layoutreturn->lrf_body, body, sizeof(body));
	nfsd4_bvec_fixture_poison(fixture);
	memset(context->stream.scratch.iov_base, 0x5a,
	       context->stream.scratch.iov_len);
	KUNIT_EXPECT_MEMEQ(test, layoutreturn->lrf_body, body, sizeof(body));
}
#endif

static void nfsd_bvec_nfs4_capability_test(struct kunit *test)
{
	const struct svc_procedure *compound;
	const struct svc_procedure *null;
	bool capable;

	null = nfsd4_procedure(NFSPROC4_NULL);
	compound = nfsd4_procedure(NFSPROC4_COMPOUND);
	KUNIT_ASSERT_NOT_NULL(test, null);
	KUNIT_ASSERT_NOT_NULL(test, compound);
	KUNIT_EXPECT_FALSE(test, null->pc_xdr_bvec);
	KUNIT_EXPECT_TRUE(test, compound->pc_xdr_bvec);
	capable = svc_proc_accepts_xdr_bvec(compound, RPC_AUTH_NULL);
	KUNIT_EXPECT_TRUE(test, capable);
	capable = svc_proc_accepts_xdr_bvec(compound, RPC_AUTH_UNIX);
	KUNIT_EXPECT_TRUE(test, capable);
	capable = svc_proc_accepts_xdr_bvec(compound, RPC_AUTH_GSS);
	KUNIT_EXPECT_FALSE(test, capable);
	KUNIT_EXPECT_PTR_EQ(test, nfsd4_procedure(NFSPROC4_COMPOUND + 1), NULL);
}

static struct kunit_case nfsd4_bvec_test_cases[] = {
	KUNIT_CASE(nfsd4_bvec_setxattr_decode_test),
	KUNIT_CASE(nfsd4_bvec_compound_boundaries_test),
	KUNIT_CASE(nfsd4_bvec_savemem_lifetime_test),
#if IS_ENABLED(CONFIG_NFSD_PNFS)
	KUNIT_CASE(nfsd4_bvec_layoutreturn_lifetime_test),
#endif
	KUNIT_CASE(nfsd_bvec_nfs4_capability_test),
	{}
};

static struct kunit_suite nfsd4_bvec_test_suite = {
	.name = "nfsd4-receive-bvec",
	.test_cases = nfsd4_bvec_test_cases,
};

kunit_test_suite(nfsd4_bvec_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for NFSDv4 immutable receive bvec consumers");
MODULE_LICENSE("GPL");
