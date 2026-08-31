// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <linux/bvec.h>
#include <linux/cred.h>
#include <linux/highmem.h>
#include <linux/in.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nfs4.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/xdr.h>
#include <net/net_namespace.h>

#include "nfsd.h"
#include "state.h"
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

#define NFSD4_BVEC_MAX_SEGMENTS	8U
#define NFSD4_Q35_MAX_OPS	6U
#define NFSD4_Q35_MAX_WIRE	1024U
#define NFSD4_Q35_LEGACY_OFFSET	13U

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
	struct kunit			*test;
	u32				 dispatch_order[NFSD4_Q35_MAX_OPS];
	u8				 dispatch_payload[64];
	unsigned int			 dispatch_count;
	unsigned int			 dispatch_payload_len;
	bool				 released;
};

struct nfsd4_q35_matched_fixture {
	struct page		*legacy_page;
	struct page		*legacy_pages[1];
	struct page		*bvec_pages[NFSD4_BVEC_MAX_SEGMENTS];
	struct bio_vec		 bvec[NFSD4_BVEC_MAX_SEGMENTS];
	struct xdr_buf		 legacy;
	struct xdr_buf		 authoritative;
	unsigned int		 lengths[NFSD4_BVEC_MAX_SEGMENTS];
	unsigned int		 count;
	unsigned int		 wire_len;
};

struct nfsd4_q35_page_owner {
	struct page		*pages[NFSD4_BVEC_MAX_SEGMENTS + 1];
	unsigned int		 baseline[NFSD4_BVEC_MAX_SEGMENTS + 1];
	unsigned int		 count;
	unsigned int		 releases;
	bool			 held;
};

struct nfsd4_q35_xprt {
	struct svc_xprt_class		 xprt_class;
	struct svc_xprt		 xprt;
};

struct nfsd4_q35_session_owner {
	struct nfsd4_q35_xprt		*xprt;
	struct svc_serv			*serv;
	struct svc_rqst			 cleanup_rqst;
	struct nfsd4_compound_state cleanup_cstate;
	union nfsd4_op_u		 cleanup_op;
	struct nfs4_sessionid		 sessionid;
	clientid_t			 clientid;
	bool				 session_live;
	bool				 client_live;
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

static struct nfsd4_q35_matched_fixture *
nfsd4_q35_matched_fixture_alloc(struct kunit *test, const void *wire,
				unsigned int wire_len,
				const unsigned int *lengths,
				unsigned int count)
{
	struct nfsd4_q35_matched_fixture *fixture;
	unsigned int consumed = 0;
	unsigned int i;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_GT(test, wire_len, 0U);
	KUNIT_ASSERT_LE(test, wire_len,
			PAGE_SIZE - NFSD4_Q35_LEGACY_OFFSET);
	KUNIT_ASSERT_GT(test, count, 0U);
	KUNIT_ASSERT_LE(test, count, NFSD4_BVEC_MAX_SEGMENTS);

	fixture->legacy_page = nfsd4_bvec_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture->legacy_page);
	fixture->legacy_pages[0] = fixture->legacy_page;
	nfsd4_bvec_write(fixture->legacy_page, NFSD4_Q35_LEGACY_OFFSET,
			 wire, wire_len);
	nfsd4_bvec_init_buf(&fixture->legacy, (void *)wire, 0, wire_len);
	fixture->legacy.pages = fixture->legacy_pages;
	fixture->legacy.page_base = NFSD4_Q35_LEGACY_OFFSET;
	fixture->legacy.page_mode = XDRBUF_PAGE_ARRAY;

	for (i = 0; i < count; i++) {
		unsigned int offset = 17 + i * 29;

		KUNIT_ASSERT_GT(test, lengths[i], 0U);
		KUNIT_ASSERT_LE(test, lengths[i], wire_len - consumed);
		fixture->bvec_pages[i] = nfsd4_bvec_alloc_page(test);
		KUNIT_ASSERT_NOT_NULL(test, fixture->bvec_pages[i]);
		fixture->lengths[i] = lengths[i];
		bvec_set_page(&fixture->bvec[i], fixture->bvec_pages[i],
			      lengths[i], offset);
		nfsd4_bvec_write(fixture->bvec_pages[i], offset,
				 wire + consumed, lengths[i]);
		consumed += lengths[i];
	}
	KUNIT_ASSERT_EQ(test, consumed, wire_len);
	nfsd4_bvec_init_buf(&fixture->authoritative, (void *)wire, 0,
			    wire_len);
	fixture->authoritative.bvec = fixture->bvec;
	fixture->authoritative.bvec_count = count;
	fixture->authoritative.page_mode = XDRBUF_PAGE_BVECS;
	fixture->count = count;
	fixture->wire_len = wire_len;
	return fixture;
}

static void
nfsd4_q35_matched_fixture_poison_authoritative(struct nfsd4_q35_matched_fixture *fixture)
{
	unsigned int i;

	for (i = 0; i < fixture->count; i++) {
		struct bio_vec *bvec = &fixture->bvec[i];
		void *address = kmap_local_page(bvec->bv_page);

		memset(address + bvec->bv_offset, 0xa5, bvec->bv_len);
		kunmap_local(address);
	}
}

static void
nfsd4_q35_page_owner_put(void *data)
{
	put_page(data);
}

static void
nfsd4_q35_page_owner_init(struct kunit *test,
			  struct nfsd4_q35_page_owner *owner,
			  struct nfsd4_q35_matched_fixture *fixture,
			  bool include_legacy)
{
	unsigned int i;
	int ret;

	memset(owner, 0, sizeof(*owner));
	if (include_legacy)
		owner->pages[owner->count++] = fixture->legacy_page;
	for (i = 0; i < fixture->count; i++)
		owner->pages[owner->count++] = fixture->bvec_pages[i];
	for (i = 0; i < owner->count; i++) {
		owner->baseline[i] = page_ref_count(owner->pages[i]);
		get_page(owner->pages[i]);
		ret = kunit_add_action_or_reset(test, nfsd4_q35_page_owner_put,
						owner->pages[i]);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}
	owner->held = true;
}

static void
nfsd4_q35_page_owner_expect_held(struct kunit *test,
				 struct nfsd4_q35_page_owner *owner)
{
	unsigned int i;

	KUNIT_EXPECT_TRUE(test, owner->held);
	for (i = 0; i < owner->count; i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(owner->pages[i]),
				owner->baseline[i] + 1);
}

static void
nfsd4_q35_page_owner_release(struct kunit *test,
			     struct nfsd4_q35_page_owner *owner)
{
	unsigned int i;

	KUNIT_ASSERT_TRUE(test, owner->held);
	for (i = 0; i < owner->count; i++)
		kunit_release_action(test, nfsd4_q35_page_owner_put,
				     owner->pages[i]);
	owner->held = false;
	owner->releases++;
	KUNIT_EXPECT_EQ(test, owner->releases, 1U);
	for (i = 0; i < owner->count; i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(owner->pages[i]),
				owner->baseline[i]);
}

static void nfsd4_q35_xprt_free(struct svc_xprt *xprt)
{
	struct nfsd4_q35_xprt *owner;

	owner = container_of(xprt, struct nfsd4_q35_xprt, xprt);
	kfree(owner);
}

static const struct svc_xprt_ops nfsd4_q35_xprt_ops = {
	.xpo_free = nfsd4_q35_xprt_free,
};

static void nfsd4_q35_xprt_release(void *data)
{
	svc_xprt_put(data);
}

static void nfsd4_q35_session_cleanup(void *data)
{
	struct nfsd4_q35_session_owner *owner = data;

	if (owner->session_live) {
		memset(&owner->cleanup_op, 0, sizeof(owner->cleanup_op));
		owner->cleanup_op.destroy_session.sessionid = owner->sessionid;
		memset(&owner->cleanup_cstate, 0, sizeof(owner->cleanup_cstate));
		nfsd4_kunit_destroy_session(&owner->cleanup_rqst,
					    &owner->cleanup_cstate,
					    &owner->cleanup_op);
		owner->session_live = false;
	}
	if (owner->client_live) {
		memset(&owner->cleanup_op, 0, sizeof(owner->cleanup_op));
		owner->cleanup_op.destroy_clientid.clientid = owner->clientid;
		memset(&owner->cleanup_cstate, 0, sizeof(owner->cleanup_cstate));
		nfsd4_kunit_destroy_clientid(&owner->cleanup_rqst,
					     &owner->cleanup_cstate,
					     &owner->cleanup_op);
		owner->client_live = false;
	}
}

static struct nfsd4_q35_session_owner *
nfsd4_q35_session_owner_alloc(struct kunit *test)
{
	struct nfsd4_q35_session_owner *owner;
	struct nfsd4_q35_xprt *xprt;
	int ret;

	owner = kunit_kzalloc(test, sizeof(*owner), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, owner);
	owner->serv = nfsd4_kunit_serv(&init_net);
	KUNIT_ASSERT_NOT_NULL(test, owner->serv);
	xprt = kzalloc_obj(*xprt);
	KUNIT_ASSERT_NOT_NULL(test, xprt);
	xprt->xprt_class.xcl_name = "q35-session";
	xprt->xprt_class.xcl_owner = THIS_MODULE;
	xprt->xprt_class.xcl_ops = &nfsd4_q35_xprt_ops;
	xprt->xprt_class.xcl_max_payload = PAGE_SIZE * 4;
	__module_get(THIS_MODULE);
	svc_xprt_init(&init_net, &xprt->xprt_class, &xprt->xprt,
		      owner->serv);
	xprt->xprt.xpt_cred = get_current_cred();
	owner->xprt = xprt;
	ret = kunit_add_action_or_reset(test, nfsd4_q35_xprt_release,
					&xprt->xprt);
	KUNIT_ASSERT_EQ(test, ret, 0);
	owner->cleanup_rqst.rq_xprt = &xprt->xprt;
	owner->cleanup_rqst.rq_server = owner->serv;
	ret = kunit_add_action_or_reset(test, nfsd4_q35_session_cleanup,
					owner);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return owner;
}

static void nfsd4_q35_free_cred(void *data)
{
	free_svc_cred(data);
}

static void
nfsd4_q35_session_context_prepare(struct kunit *test,
				  struct nfsd4_bvec_decode_context *context,
				  struct nfsd4_q35_session_owner *owner)
{
	struct sockaddr_in *remote = (struct sockaddr_in *)&context->rqst.rq_addr;
	struct sockaddr_in *local = (struct sockaddr_in *)&context->rqst.rq_daddr;
	int ret;

	context->rqst.rq_xprt = &owner->xprt->xprt;
	context->rqst.rq_server = owner->serv;
	context->rqst.rq_prot = IPPROTO_TCP;
	remote->sin_family = AF_INET;
	remote->sin_port = htons(20549);
	remote->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	context->rqst.rq_addrlen = sizeof(*remote);
	*local = *remote;
	context->rqst.rq_daddrlen = sizeof(*local);
	context->rqst.rq_cred.cr_uid = current_uid();
	context->rqst.rq_cred.cr_gid = current_gid();
	context->rqst.rq_cred.cr_group_info = get_current_groups();
	context->rqst.rq_cred.cr_flavor = RPC_AUTH_UNIX;
	ret = kunit_add_action_or_reset(test, nfsd4_q35_free_cred,
					&context->rqst.rq_cred);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static void nfsd4_decode_release(void *data)
{
	struct nfsd4_bvec_decode_context *context = data;

	if (context->released)
		return;
	nfsd4_release_compoundargs(&context->rqst);
	xdr_finish_decode(&context->stream);
	context->released = true;
}

static struct nfsd4_bvec_decode_context *
nfsd4_bvec_decode_context_alloc(struct kunit *test, struct xdr_buf *buf)
{
	struct nfsd4_bvec_decode_context *context;
	void *scratch;
	int ret;

	context = kunit_kzalloc(test, sizeof(*context), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, context);
	context->test = test;
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

static void
nfsd4_bvec_decode_context_release(struct nfsd4_bvec_decode_context *context)
{
	nfsd4_decode_release(context);
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

enum nfsd4_q35_op_kind {
	NFSD4_Q35_ACCESS,
	NFSD4_Q35_LOOKUP,
	NFSD4_Q35_WRITE,
};

struct nfsd4_q35_case {
	const char		*name;
	enum nfsd4_q35_op_kind ops[NFSD4_Q35_MAX_OPS];
	unsigned int		 opcnt;
};

struct nfsd4_q35_wire {
	u8		*bytes;
	unsigned int	 len;
	unsigned int	 write_start[2];
	unsigned int	 write_len[2];
	unsigned int	 writes;
};

enum nfsd4_q35_dispatch_mode {
	NFSD4_Q35_DISPATCH_PRODUCTION,
	NFSD4_Q35_DISPATCH_TEST_ALL,
	NFSD4_Q35_DISPATCH_TEST_AFTER_SEQUENCE,
};

struct nfsd4_q35_dispatch_result {
	__be32		 status;
	u32		 opcnt;
	u32		 stop_op;
	unsigned int	 len;
};

static __be32 *
nfsd4_q35_encode_write(__be32 *p, const u8 *payload, unsigned int payload_len,
		       u64 offset, unsigned int *payload_start, const void *wire)
{
	*p++ = cpu_to_be32(OP_WRITE);
	p = nfsd4_bvec_encode_stateid(p);
	p = xdr_encode_hyper(p, offset);
	*p++ = cpu_to_be32(NFS_FILE_SYNC);
	*p++ = cpu_to_be32(payload_len);
	*payload_start = (void *)p - wire;
	return xdr_encode_opaque_fixed(p, payload, payload_len);
}

static struct nfsd4_q35_wire *
nfsd4_q35_wire_alloc(struct kunit *test, const struct nfsd4_q35_case *test_case)
{
	static const u8 payloads[2][19] = {
		{ 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98,
		  0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f, 0x55, 0xaa, 0x7d },
		{ 0x81, 0x72, 0x63, 0x54, 0x45, 0x36, 0x27, 0x18, 0x09,
		  0xfa, 0xeb, 0xdc, 0xcd, 0xbe, 0xaf, 0x90, 0x44, 0x33, 0x22 },
	};
	static const char lookup[] = "after-write";
	struct nfsd4_q35_wire *wire;
	__be32 *p;
	unsigned int i;

	wire = kunit_kzalloc(test, sizeof(*wire), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, wire);
	wire->bytes = kunit_kzalloc(test, NFSD4_Q35_MAX_WIRE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, wire->bytes);
	p = nfsd4_bvec_encode_compound_header((__be32 *)wire->bytes, 0,
					      test_case->opcnt);
	for (i = 0; i < test_case->opcnt; i++) {
		switch (test_case->ops[i]) {
		case NFSD4_Q35_ACCESS:
			*p++ = cpu_to_be32(OP_ACCESS);
			*p++ = cpu_to_be32(NFS4_ACCESS_READ | NFS4_ACCESS_LOOKUP);
			break;
		case NFSD4_Q35_LOOKUP:
			*p++ = cpu_to_be32(OP_LOOKUP);
			p = xdr_encode_opaque(p, lookup, sizeof(lookup) - 1);
			break;
		case NFSD4_Q35_WRITE:
			KUNIT_ASSERT_LT(test, wire->writes, ARRAY_SIZE(payloads));
			wire->write_len[wire->writes] = sizeof(payloads[0]);
			p = nfsd4_q35_encode_write(p, payloads[wire->writes],
						   sizeof(payloads[0]),
						    0x1020304050607080ULL +
						    wire->writes,
						    &wire->write_start[wire->writes],
						    wire->bytes);
			wire->writes++;
			break;
		}
	}
	wire->len = (void *)p - (void *)wire->bytes;
	KUNIT_ASSERT_LE(test, wire->len, NFSD4_Q35_MAX_WIRE);
	return wire;
}

static unsigned int
nfsd4_q35_bvec_index(const struct nfsd4_q35_matched_fixture *fixture,
		     unsigned int offset, unsigned int *inner)
{
	unsigned int i;

	for (i = 0; i < fixture->count; i++) {
		if (offset < fixture->lengths[i]) {
			*inner = offset;
			return i;
		}
		offset -= fixture->lengths[i];
	}
	*inner = 0;
	return fixture->count;
}

static unsigned int
nfsd4_q35_bvec_span(const struct nfsd4_q35_matched_fixture *fixture,
		    unsigned int index, unsigned int inner,
		    unsigned int length)
{
	unsigned int count = 0;

	while (length) {
		unsigned int available;

		if (index >= fixture->count)
			return 0;
		available = fixture->lengths[index] - inner;
		length -= min(length, available);
		inner = 0;
		index++;
		count++;
	}
	return count;
}

static unsigned int
nfsd4_q35_lengths_for_wire(const struct nfsd4_q35_wire *wire,
			   unsigned int *lengths)
{
	unsigned int points[NFSD4_BVEC_MAX_SEGMENTS - 1];
	unsigned int point_count = 0;
	unsigned int consumed = 0;
	unsigned int count = 0;
	unsigned int i;

	points[point_count++] = 1;
	points[point_count++] = 4;
	points[point_count++] = 9;
	if (wire->writes) {
		points[point_count++] = wire->write_start[0] + 1;
		points[point_count++] = wire->write_start[0] + 7;
		if (wire->writes > 1) {
			points[point_count++] = wire->write_start[1] + 2;
			points[point_count++] = wire->write_start[1] + 11;
		} else {
			points[point_count++] = wire->write_start[0] + 15;
		}
	}
	for (i = 0; i < point_count; i++) {
		if (points[i] <= consumed || points[i] >= wire->len)
			continue;
		lengths[count++] = points[i] - consumed;
		consumed = points[i];
	}
	lengths[count++] = wire->len - consumed;
	return count;
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

static u32 nfsd4_q35_expected_opnum(enum nfsd4_q35_op_kind kind)
{
	switch (kind) {
	case NFSD4_Q35_ACCESS:
		return OP_ACCESS;
	case NFSD4_Q35_LOOKUP:
		return OP_LOOKUP;
	case NFSD4_Q35_WRITE:
		return OP_WRITE;
	}
	return OP_ILLEGAL;
}

static void
nfsd4_q35_compare_decoded(struct kunit *test,
			  const struct nfsd4_q35_case *test_case,
			  const struct nfsd4_q35_wire *wire,
			  const struct nfsd4_q35_matched_fixture *fixture,
			  struct nfsd4_bvec_decode_context *legacy,
			  struct nfsd4_bvec_decode_context *authoritative)
{
	static const char lookup[] = "after-write";
	unsigned int write_index = 0;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, authoritative->args->minorversion,
			legacy->args->minorversion);
	KUNIT_EXPECT_EQ(test, authoritative->args->client_opcnt,
			legacy->args->client_opcnt);
	KUNIT_EXPECT_EQ(test, authoritative->args->opcnt, legacy->args->opcnt);
	KUNIT_ASSERT_EQ(test, authoritative->args->opcnt, test_case->opcnt);
	for (i = 0; i < test_case->opcnt; i++) {
		struct nfsd4_op *authoritative_op = &authoritative->args->ops[i];
		struct nfsd4_op *legacy_op = &legacy->args->ops[i];
		u32 expected_opnum = nfsd4_q35_expected_opnum(test_case->ops[i]);

		KUNIT_EXPECT_EQ(test, authoritative_op->opnum, expected_opnum);
		KUNIT_EXPECT_EQ(test, authoritative_op->opnum, legacy_op->opnum);
		KUNIT_EXPECT_EQ(test, (__force u32)authoritative_op->status,
				(__force u32)legacy_op->status);
		KUNIT_EXPECT_PTR_EQ(test, authoritative_op->opdesc,
				    legacy_op->opdesc);
		switch (test_case->ops[i]) {
		case NFSD4_Q35_ACCESS:
			KUNIT_EXPECT_EQ(test,
					authoritative_op->u.access.ac_req_access,
				legacy_op->u.access.ac_req_access);
			break;
		case NFSD4_Q35_LOOKUP:
			KUNIT_EXPECT_EQ(test, authoritative_op->u.lookup.lo_len,
					legacy_op->u.lookup.lo_len);
			KUNIT_EXPECT_EQ(test, authoritative_op->u.lookup.lo_len,
					(u32)sizeof(lookup) - 1);
			KUNIT_EXPECT_MEMEQ(test,
					   authoritative_op->u.lookup.lo_name,
					lookup, sizeof(lookup) - 1);
			KUNIT_EXPECT_MEMEQ(test, legacy_op->u.lookup.lo_name,
					   lookup, sizeof(lookup) - 1);
			break;
		case NFSD4_Q35_WRITE: {
			struct nfsd4_write *authoritative_write =
				&authoritative_op->u.write;
			struct nfsd4_write *legacy_write = &legacy_op->u.write;
			unsigned int expected_count;
			unsigned int inner;
			unsigned int vector;
			u8 authoritative_bytes[19];
			u8 legacy_bytes[19];
			int ret;

			KUNIT_ASSERT_LT(test, write_index, wire->writes);
			KUNIT_ASSERT_EQ(test, wire->write_len[write_index],
					sizeof(authoritative_bytes));
			KUNIT_EXPECT_EQ(test, authoritative_write->wr_offset,
					legacy_write->wr_offset);
			KUNIT_EXPECT_EQ(test, authoritative_write->wr_stable_how,
					legacy_write->wr_stable_how);
			KUNIT_EXPECT_EQ(test, authoritative_write->wr_buflen,
					legacy_write->wr_buflen);
			ret = read_bytes_from_xdr_buf(&authoritative_write->wr_payload,
						      0, authoritative_bytes,
						      sizeof(authoritative_bytes));
			KUNIT_ASSERT_EQ(test, ret, 0);
			ret = read_bytes_from_xdr_buf(&legacy_write->wr_payload, 0,
						      legacy_bytes,
						      sizeof(legacy_bytes));
			KUNIT_ASSERT_EQ(test, ret, 0);
			KUNIT_EXPECT_MEMEQ(test, authoritative_bytes,
					   wire->bytes + wire->write_start[write_index],
					   sizeof(authoritative_bytes));
			KUNIT_EXPECT_MEMEQ(test, authoritative_bytes, legacy_bytes,
					   sizeof(authoritative_bytes));

			KUNIT_EXPECT_EQ(test, legacy_write->wr_payload.page_mode,
					XDRBUF_PAGE_ARRAY);
			KUNIT_EXPECT_PTR_EQ(test, legacy_write->wr_payload.pages,
					    (struct page **)fixture->legacy_pages);
			KUNIT_EXPECT_EQ(test, legacy_write->wr_payload.page_base,
					NFSD4_Q35_LEGACY_OFFSET +
					wire->write_start[write_index]);
			KUNIT_EXPECT_EQ(test, legacy_write->wr_payload.page_len,
					wire->write_len[write_index]);

			vector = nfsd4_q35_bvec_index(fixture,
						      wire->write_start[write_index],
						       &inner);
			KUNIT_ASSERT_LT(test, vector, fixture->count);
			expected_count = nfsd4_q35_bvec_span(fixture, vector,
							     inner,
								 wire->write_len[write_index]);
			KUNIT_ASSERT_GT(test, expected_count, 1U);
			KUNIT_EXPECT_EQ(test,
					authoritative_write->wr_payload.page_mode,
					XDRBUF_PAGE_BVECS);
			KUNIT_EXPECT_PTR_EQ(test,
					    authoritative_write->wr_payload.bvec,
					&fixture->bvec[vector]);
			KUNIT_EXPECT_EQ(test,
					authoritative_write->wr_payload.bvec_offset,
					inner);
			KUNIT_EXPECT_EQ(test,
					authoritative_write->wr_payload.bvec_count,
					expected_count);
			KUNIT_EXPECT_EQ(test,
					authoritative_write->wr_payload.page_len,
					wire->write_len[write_index]);
			KUNIT_EXPECT_EQ(test,
					authoritative_write->wr_payload.head[0].iov_len,
					0UL);
			KUNIT_EXPECT_EQ(test,
					authoritative_write->wr_payload.tail[0].iov_len,
					0UL);
			write_index++;
			break;
		}
		}
	}
	KUNIT_EXPECT_EQ(test, write_index, wire->writes);
}

static void
nfsd4_q35_expect_dispatch_pair(struct kunit *test,
			       const struct nfsd4_q35_case *test_case,
			       struct nfsd4_bvec_decode_context *legacy,
			       struct nfsd4_bvec_decode_context *authoritative,
			       __be32 expected_status,
			       unsigned int expected_dispatches);

static void nfsd4_q35_decode_matrix_test(struct kunit *test)
{
	static const struct nfsd4_q35_case cases[] = {
		{
			.name = "no-write",
			.ops = { NFSD4_Q35_ACCESS, NFSD4_Q35_LOOKUP },
			.opcnt = 2,
		},
		{
			.name = "write-first",
			.ops = { NFSD4_Q35_WRITE, NFSD4_Q35_ACCESS,
				 NFSD4_Q35_LOOKUP },
			.opcnt = 3,
		},
		{
			.name = "write-middle",
			.ops = { NFSD4_Q35_ACCESS, NFSD4_Q35_WRITE,
				 NFSD4_Q35_LOOKUP },
			.opcnt = 3,
		},
		{
			.name = "write-last",
			.ops = { NFSD4_Q35_LOOKUP, NFSD4_Q35_ACCESS,
				 NFSD4_Q35_WRITE },
			.opcnt = 3,
		},
		{
			.name = "two-writes",
			.ops = { NFSD4_Q35_WRITE, NFSD4_Q35_ACCESS,
				 NFSD4_Q35_WRITE },
			.opcnt = 3,
		},
	};
	unsigned int case_index;

	for (case_index = 0; case_index < ARRAY_SIZE(cases); case_index++) {
		struct nfsd4_bvec_decode_context *authoritative;
		struct nfsd4_bvec_decode_context *legacy;
		struct nfsd4_q35_matched_fixture *fixture;
		struct nfsd4_q35_page_owner owner;
		struct nfsd4_q35_wire *wire;
		unsigned int lengths[NFSD4_BVEC_MAX_SEGMENTS];
		unsigned int count;
		bool decoded;

		kunit_info(test, "%s", cases[case_index].name);
		wire = nfsd4_q35_wire_alloc(test, &cases[case_index]);
		count = nfsd4_q35_lengths_for_wire(wire, lengths);
		fixture = nfsd4_q35_matched_fixture_alloc(test, wire->bytes,
							  wire->len, lengths, count);
		nfsd4_q35_page_owner_init(test, &owner, fixture, true);
		legacy = nfsd4_bvec_decode_context_alloc(test, &fixture->legacy);
		decoded = nfs4svc_decode_compoundargs(&legacy->rqst,
						      &legacy->stream);
		KUNIT_ASSERT_TRUE(test, decoded);
		authoritative = nfsd4_bvec_decode_context_alloc(test,
								&fixture->authoritative);
		decoded = nfs4svc_decode_compoundargs(&authoritative->rqst,
						      &authoritative->stream);
		KUNIT_ASSERT_TRUE(test, decoded);
		nfsd4_q35_compare_decoded(test, &cases[case_index], wire, fixture,
					  legacy, authoritative);
		nfsd4_q35_expect_dispatch_pair(test, &cases[case_index], legacy,
					       authoritative, nfs_ok,
					       cases[case_index].opcnt);
		nfsd4_q35_page_owner_expect_held(test, &owner);
		nfsd4_bvec_decode_context_release(authoritative);
		nfsd4_bvec_decode_context_release(legacy);
		nfsd4_q35_page_owner_expect_held(test, &owner);
		nfsd4_q35_page_owner_release(test, &owner);
	}
}

static void
nfsd4_q35_expect_post_write_bad_xdr(struct kunit *test,
				    const struct nfsd4_q35_case *test_case,
				    struct xdr_buf *legacy_buf,
				    struct xdr_buf *authoritative_buf,
				    struct nfsd4_q35_page_owner *owner)
{
	struct nfsd4_bvec_decode_context *authoritative;
	struct nfsd4_bvec_decode_context *legacy;
	bool authoritative_decoded;
	bool legacy_decoded;

	legacy = nfsd4_bvec_decode_context_alloc(test, legacy_buf);
	legacy_decoded = nfs4svc_decode_compoundargs(&legacy->rqst,
						     &legacy->stream);
	authoritative = nfsd4_bvec_decode_context_alloc(test, authoritative_buf);
	authoritative_decoded = nfs4svc_decode_compoundargs(&authoritative->rqst,
							    &authoritative->stream);
	KUNIT_EXPECT_EQ(test, authoritative_decoded, legacy_decoded);
	KUNIT_ASSERT_TRUE(test, authoritative_decoded);
	KUNIT_ASSERT_EQ(test, legacy->args->opcnt, 3U);
	KUNIT_ASSERT_EQ(test, authoritative->args->opcnt, 3U);
	KUNIT_EXPECT_EQ(test, legacy->args->ops[2].opnum, (u32)OP_LOOKUP);
	KUNIT_EXPECT_EQ(test, authoritative->args->ops[2].opnum,
			(u32)OP_LOOKUP);
	KUNIT_EXPECT_EQ(test, (__force u32)legacy->args->ops[2].status,
			(__force u32)nfserr_bad_xdr);
	KUNIT_EXPECT_EQ(test,
			(__force u32)authoritative->args->ops[2].status,
			(__force u32)nfserr_bad_xdr);
	nfsd4_q35_expect_dispatch_pair(test, test_case, legacy, authoritative,
				       nfserr_bad_xdr, 2);
	nfsd4_q35_page_owner_expect_held(test, owner);
	nfsd4_bvec_decode_context_release(authoritative);
	nfsd4_bvec_decode_context_release(legacy);
}

static void nfsd4_q35_malformed_post_write_test(struct kunit *test)
{
	static const struct nfsd4_q35_case test_case = {
		.name = "post-write-errors",
		.ops = { NFSD4_Q35_ACCESS, NFSD4_Q35_WRITE,
			 NFSD4_Q35_LOOKUP },
		.opcnt = 3,
	};
	struct nfsd4_bvec_decode_context *bad_geometry;
	struct nfsd4_q35_matched_fixture *fixture;
	struct nfsd4_q35_matched_fixture *malformed_fixture;
	struct nfsd4_q35_page_owner *owner;
	struct nfsd4_q35_page_owner *malformed_owner;
	struct nfsd4_q35_wire *wire;
	struct xdr_buf *authoritative_short;
	struct xdr_buf *legacy_short;
	struct xdr_buf *invalid_geometry;
	struct bio_vec *invalid_bvec;
	unsigned int lengths[NFSD4_BVEC_MAX_SEGMENTS];
	unsigned int count;
	u8 *malformed_wire;
	bool decoded;

	wire = nfsd4_q35_wire_alloc(test, &test_case);
	owner = kunit_kzalloc(test, sizeof(*owner), GFP_KERNEL);
	malformed_owner = kunit_kzalloc(test, sizeof(*malformed_owner),
					GFP_KERNEL);
	authoritative_short = kunit_kzalloc(test,
					    sizeof(*authoritative_short),
					    GFP_KERNEL);
	legacy_short = kunit_kzalloc(test, sizeof(*legacy_short), GFP_KERNEL);
	invalid_geometry = kunit_kzalloc(test, sizeof(*invalid_geometry),
					 GFP_KERNEL);
	invalid_bvec = kunit_kcalloc(test, NFSD4_BVEC_MAX_SEGMENTS,
				     sizeof(*invalid_bvec), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, owner);
	KUNIT_ASSERT_NOT_NULL(test, malformed_owner);
	KUNIT_ASSERT_NOT_NULL(test, authoritative_short);
	KUNIT_ASSERT_NOT_NULL(test, legacy_short);
	KUNIT_ASSERT_NOT_NULL(test, invalid_geometry);
	KUNIT_ASSERT_NOT_NULL(test, invalid_bvec);
	count = nfsd4_q35_lengths_for_wire(wire, lengths);
	fixture = nfsd4_q35_matched_fixture_alloc(test, wire->bytes, wire->len,
						  lengths, count);
	nfsd4_q35_page_owner_init(test, owner, fixture, true);
	*legacy_short = fixture->legacy;
	*authoritative_short = fixture->authoritative;
	legacy_short->page_len -= 2;
	legacy_short->len -= 2;
	legacy_short->buflen -= 2;
	authoritative_short->page_len -= 2;
	authoritative_short->len -= 2;
	authoritative_short->buflen -= 2;
	nfsd4_q35_expect_post_write_bad_xdr(test, &test_case, legacy_short,
					    authoritative_short, owner);

	memcpy(invalid_bvec, fixture->bvec,
	       sizeof(*invalid_bvec) * NFSD4_BVEC_MAX_SEGMENTS);
	invalid_bvec[count - 1].bv_len--;
	*invalid_geometry = fixture->authoritative;
	invalid_geometry->bvec = invalid_bvec;
	bad_geometry = nfsd4_bvec_decode_context_alloc(test,
						       invalid_geometry);
	decoded = nfs4svc_decode_compoundargs(&bad_geometry->rqst,
					      &bad_geometry->stream);
	KUNIT_EXPECT_FALSE(test, decoded);
	nfsd4_bvec_decode_context_release(bad_geometry);
	nfsd4_q35_page_owner_expect_held(test, owner);
	nfsd4_q35_page_owner_release(test, owner);

	malformed_wire = kunit_kmalloc(test, wire->len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, malformed_wire);
	memcpy(malformed_wire, wire->bytes, wire->len);
	*(__be32 *)(malformed_wire + wire->len - 16) = cpu_to_be32(100);
	malformed_fixture = nfsd4_q35_matched_fixture_alloc(test,
							    malformed_wire,
							     wire->len,
							     lengths, count);
	nfsd4_q35_page_owner_init(test, malformed_owner, malformed_fixture, true);
	nfsd4_q35_expect_post_write_bad_xdr(test, &test_case,
					    &malformed_fixture->legacy,
					    &malformed_fixture->authoritative,
					    malformed_owner);
	nfsd4_q35_page_owner_release(test, malformed_owner);
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

static __be32 *
nfsd4_q35_encode_tracked_opaque(__be32 *p, const void *value,
				unsigned int length, unsigned int *start,
			       const void *wire)
{
	*p++ = cpu_to_be32(length);
	*start = (void *)p - wire;
	return xdr_encode_opaque_fixed(p, value, length);
}

static void nfsd4_q35_saved_values_lifetime_test(struct kunit *test)
{
	static const u8 verifier[NFS4_VERIFIER_SIZE] = {
		0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
	};
	static const char tag[] = "q35-tag";
	static const char client[] = "q35-client";
	static const char netid[] = "tcp6";
	static const char address[] = "2001:db8::35.8.1";
	static const char source[] = "source-name";
	static const char target[] = "target-name";
	struct nfsd4_bvec_decode_context *authoritative;
	struct nfsd4_bvec_decode_context *legacy;
	struct nfsd4_q35_matched_fixture *fixture;
	struct nfsd4_q35_page_owner owner;
	struct nfsd4_setclientid *authoritative_setclientid;
	struct nfsd4_setclientid *legacy_setclientid;
	struct nfsd4_rename *authoritative_rename;
	struct nfsd4_rename *legacy_rename;
	unsigned int starts[6];
	unsigned int lengths[NFSD4_BVEC_MAX_SEGMENTS];
	unsigned int consumed = 0;
	unsigned int count = 0;
	unsigned int wire_len;
	unsigned int i;
	u8 *wire;
	__be32 *p;
	bool decoded;

	wire = kunit_kzalloc(test, NFSD4_Q35_MAX_WIRE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, wire);
	p = (__be32 *)wire;
	p = nfsd4_q35_encode_tracked_opaque(p, tag, sizeof(tag) - 1,
					    &starts[0], wire);
	*p++ = xdr_zero;
	*p++ = cpu_to_be32(2);
	*p++ = cpu_to_be32(OP_SETCLIENTID);
	memcpy(p, verifier, sizeof(verifier));
	p += XDR_QUADLEN(sizeof(verifier));
	p = nfsd4_q35_encode_tracked_opaque(p, client, sizeof(client) - 1,
					    &starts[1], wire);
	*p++ = cpu_to_be32(0x10203040);
	p = nfsd4_q35_encode_tracked_opaque(p, netid, sizeof(netid) - 1,
					    &starts[2], wire);
	p = nfsd4_q35_encode_tracked_opaque(p, address, sizeof(address) - 1,
					    &starts[3], wire);
	*p++ = cpu_to_be32(0x50607080);
	*p++ = cpu_to_be32(OP_RENAME);
	p = nfsd4_q35_encode_tracked_opaque(p, source, sizeof(source) - 1,
					    &starts[4], wire);
	p = nfsd4_q35_encode_tracked_opaque(p, target, sizeof(target) - 1,
					    &starts[5], wire);
	wire_len = (void *)p - (void *)wire;

	for (i = 0; i < ARRAY_SIZE(starts); i++) {
		unsigned int boundary = starts[i] + 1;

		KUNIT_ASSERT_GT(test, boundary, consumed);
		lengths[count++] = boundary - consumed;
		consumed = boundary;
	}
	lengths[count++] = wire_len - consumed;
	fixture = nfsd4_q35_matched_fixture_alloc(test, wire, wire_len,
						  lengths, count);
	nfsd4_q35_page_owner_init(test, &owner, fixture, false);
	legacy = nfsd4_bvec_decode_context_alloc(test, &fixture->legacy);
	decoded = nfs4svc_decode_compoundargs(&legacy->rqst, &legacy->stream);
	KUNIT_ASSERT_TRUE(test, decoded);
	authoritative = nfsd4_bvec_decode_context_alloc(test,
							&fixture->authoritative);
	decoded = nfs4svc_decode_compoundargs(&authoritative->rqst,
					      &authoritative->stream);
	KUNIT_ASSERT_TRUE(test, decoded);
	KUNIT_ASSERT_EQ(test, legacy->args->opcnt, 2U);
	KUNIT_ASSERT_EQ(test, authoritative->args->opcnt, 2U);
	KUNIT_ASSERT_EQ(test, legacy->args->taglen, (u32)sizeof(tag) - 1);
	KUNIT_ASSERT_EQ(test, authoritative->args->taglen,
			(u32)sizeof(tag) - 1);

	legacy_setclientid = &legacy->args->ops[0].u.setclientid;
	authoritative_setclientid =
		&authoritative->args->ops[0].u.setclientid;
	legacy_rename = &legacy->args->ops[1].u.rename;
	authoritative_rename = &authoritative->args->ops[1].u.rename;
	KUNIT_EXPECT_MEMEQ(test, legacy->args->tag, tag, sizeof(tag) - 1);
	KUNIT_EXPECT_MEMEQ(test, authoritative->args->tag, tag,
			   sizeof(tag) - 1);
	KUNIT_EXPECT_EQ(test, authoritative_setclientid->se_name.len,
			legacy_setclientid->se_name.len);
	KUNIT_EXPECT_MEMEQ(test, authoritative_setclientid->se_name.data,
			   client, sizeof(client) - 1);
	KUNIT_EXPECT_MEMEQ(test, legacy_setclientid->se_name.data, client,
			   sizeof(client) - 1);
	KUNIT_EXPECT_MEMEQ(test,
			   authoritative_setclientid->se_callback_netid_val,
			   netid, sizeof(netid) - 1);
	KUNIT_EXPECT_MEMEQ(test,
			   authoritative_setclientid->se_callback_addr_val,
			   address, sizeof(address) - 1);
	KUNIT_EXPECT_MEMEQ(test, authoritative_rename->rn_sname, source,
			   sizeof(source) - 1);
	KUNIT_EXPECT_MEMEQ(test, authoritative_rename->rn_tname, target,
			   sizeof(target) - 1);
	KUNIT_EXPECT_MEMEQ(test, legacy_rename->rn_sname, source,
			   sizeof(source) - 1);
	KUNIT_EXPECT_MEMEQ(test, legacy_rename->rn_tname, target,
			   sizeof(target) - 1);

	nfsd4_q35_matched_fixture_poison_authoritative(fixture);
	memset(authoritative->stream.scratch.iov_base, 0x5a,
	       authoritative->stream.scratch.iov_len);
	KUNIT_EXPECT_MEMEQ(test, authoritative->args->tag, tag,
			   sizeof(tag) - 1);
	KUNIT_EXPECT_MEMEQ(test, authoritative_setclientid->se_name.data,
			   client, sizeof(client) - 1);
	KUNIT_EXPECT_MEMEQ(test,
			   authoritative_setclientid->se_callback_netid_val,
			   netid, sizeof(netid) - 1);
	KUNIT_EXPECT_MEMEQ(test,
			   authoritative_setclientid->se_callback_addr_val,
			   address, sizeof(address) - 1);
	KUNIT_EXPECT_MEMEQ(test, authoritative_rename->rn_sname, source,
			   sizeof(source) - 1);
	KUNIT_EXPECT_MEMEQ(test, authoritative_rename->rn_tname, target,
			   sizeof(target) - 1);
	nfsd4_q35_page_owner_expect_held(test, &owner);
	nfsd4_bvec_decode_context_release(authoritative);
	nfsd4_bvec_decode_context_release(legacy);
	KUNIT_EXPECT_PTR_EQ(test, authoritative->args->to_free, NULL);
	nfsd4_q35_page_owner_expect_held(test, &owner);
	nfsd4_q35_page_owner_release(test, &owner);
}

static unsigned int
nfsd4_q35_run_dispatch(struct kunit *test,
		       struct nfsd4_bvec_decode_context *context,
		       enum nfsd4_q35_dispatch_mode mode,
		       u8 *output, unsigned int output_capacity,
		       struct nfsd4_q35_dispatch_result *result);

static struct nfsd4_bvec_decode_context *
nfsd4_q35_session_decode(struct kunit *test,
			 struct nfsd4_q35_session_owner *owner,
			 const void *wire, unsigned int wire_len)
{
	struct nfsd4_bvec_decode_context *context;
	struct nfsd4_bvec_fixture *fixture;
	unsigned int lengths[3] = { 3, 5, wire_len - 8 };
	bool decoded;

	KUNIT_ASSERT_GT(test, wire_len, 8U);
	fixture = nfsd4_bvec_fixture_alloc(test, wire, wire_len, lengths,
					   ARRAY_SIZE(lengths));
	context = nfsd4_bvec_decode_context_alloc(test, &fixture->buf);
	nfsd4_q35_session_context_prepare(test, context, owner);
	decoded = nfs4svc_decode_compoundargs(&context->rqst, &context->stream);
	KUNIT_ASSERT_TRUE(test, decoded);
	return context;
}

static __be32 *nfsd4_q35_encode_channel_attrs(__be32 *p)
{
	*p++ = xdr_zero;
	*p++ = cpu_to_be32(PAGE_SIZE);
	*p++ = cpu_to_be32(PAGE_SIZE);
	*p++ = cpu_to_be32(PAGE_SIZE);
	*p++ = cpu_to_be32(4);
	*p++ = xdr_one;
	*p++ = xdr_zero;
	return p;
}

static void nfsd4_q35_session_replay_test(struct kunit *test)
{
	static const char client_name[] = "q35-production-session";
	static const u8 verifier[NFS4_VERIFIER_SIZE] = {
		0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
	};
	struct nfsd4_bvec_decode_context *authoritative;
	struct nfsd4_bvec_decode_context *context;
	struct nfsd4_bvec_decode_context *legacy;
	struct nfsd4_q35_dispatch_result *authoritative_result;
	struct nfsd4_q35_dispatch_result *legacy_result;
	struct nfsd4_q35_dispatch_result *result;
	struct nfsd4_q35_matched_fixture *fixture;
	struct nfsd4_q35_page_owner *page_owner;
	struct nfsd4_q35_session_owner *session_owner;
	unsigned int lengths[NFSD4_BVEC_MAX_SEGMENTS];
	unsigned int wire_len;
	u8 *authoritative_output;
	u8 *legacy_output;
	u8 *output;
	u8 *wire;
	__be32 *p;
	bool decoded;

	session_owner = nfsd4_q35_session_owner_alloc(test);
	result = kunit_kzalloc(test, sizeof(*result), GFP_KERNEL);
	legacy_result = kunit_kzalloc(test, sizeof(*legacy_result), GFP_KERNEL);
	authoritative_result = kunit_kzalloc(test,
					     sizeof(*authoritative_result),
					     GFP_KERNEL);
	page_owner = kunit_kzalloc(test, sizeof(*page_owner), GFP_KERNEL);
	wire = kunit_kzalloc(test, NFSD4_Q35_MAX_WIRE, GFP_KERNEL);
	output = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	legacy_output = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	authoritative_output = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, session_owner);
	KUNIT_ASSERT_NOT_NULL(test, result);
	KUNIT_ASSERT_NOT_NULL(test, legacy_result);
	KUNIT_ASSERT_NOT_NULL(test, authoritative_result);
	KUNIT_ASSERT_NOT_NULL(test, page_owner);
	KUNIT_ASSERT_NOT_NULL(test, wire);
	KUNIT_ASSERT_NOT_NULL(test, output);
	KUNIT_ASSERT_NOT_NULL(test, legacy_output);
	KUNIT_ASSERT_NOT_NULL(test, authoritative_output);

	p = nfsd4_bvec_encode_compound_header((__be32 *)wire, 1, 1);
	*p++ = cpu_to_be32(OP_EXCHANGE_ID);
	memcpy(p, verifier, sizeof(verifier));
	p += XDR_QUADLEN(sizeof(verifier));
	*p++ = cpu_to_be32(sizeof(client_name) - 1);
	p = xdr_encode_opaque_fixed(p, client_name, sizeof(client_name) - 1);
	*p++ = xdr_zero;
	*p++ = cpu_to_be32(SP4_NONE);
	*p++ = xdr_zero;
	wire_len = (void *)p - (void *)wire;
	context = nfsd4_q35_session_decode(test, session_owner, wire, wire_len);
	nfsd4_q35_run_dispatch(test, context, NFSD4_Q35_DISPATCH_PRODUCTION,
			       output, PAGE_SIZE, result);
	if (result->status == nfs_ok) {
		session_owner->clientid =
			context->args->ops[0].u.exchange_id.clientid;
		session_owner->client_live = true;
	}
	KUNIT_ASSERT_EQ(test, (__force u32)result->status,
			(__force u32)nfs_ok);
	KUNIT_ASSERT_EQ(test, result->opcnt, 1U);
	KUNIT_ASSERT_EQ(test, result->stop_op, (u32)OP_EXCHANGE_ID);

	memset(wire, 0, NFSD4_Q35_MAX_WIRE);
	p = nfsd4_bvec_encode_compound_header((__be32 *)wire, 1, 1);
	*p++ = cpu_to_be32(OP_CREATE_SESSION);
	memcpy(p, &session_owner->clientid, sizeof(session_owner->clientid));
	p += XDR_QUADLEN(sizeof(session_owner->clientid));
	*p++ = cpu_to_be32(context->args->ops[0].u.exchange_id.seqid);
	*p++ = xdr_zero;
	p = nfsd4_q35_encode_channel_attrs(p);
	p = nfsd4_q35_encode_channel_attrs(p);
	*p++ = xdr_zero;
	*p++ = xdr_one;
	*p++ = cpu_to_be32(RPC_AUTH_NULL);
	wire_len = (void *)p - (void *)wire;
	context = nfsd4_q35_session_decode(test, session_owner, wire, wire_len);
	memset(result, 0, sizeof(*result));
	nfsd4_q35_run_dispatch(test, context, NFSD4_Q35_DISPATCH_PRODUCTION,
			       output, PAGE_SIZE, result);
	if (result->status == nfs_ok) {
		session_owner->sessionid =
			context->args->ops[0].u.create_session.sessionid;
		session_owner->session_live = true;
	}
	KUNIT_ASSERT_EQ(test, (__force u32)result->status,
			(__force u32)nfs_ok);
	KUNIT_ASSERT_EQ(test, result->opcnt, 1U);
	KUNIT_ASSERT_EQ(test, result->stop_op, (u32)OP_CREATE_SESSION);

	memset(wire, 0, NFSD4_Q35_MAX_WIRE);
	p = nfsd4_bvec_encode_compound_header((__be32 *)wire, 1, 2);
	*p++ = cpu_to_be32(OP_SEQUENCE);
	memcpy(p, &session_owner->sessionid, sizeof(session_owner->sessionid));
	p += XDR_QUADLEN(sizeof(session_owner->sessionid));
	*p++ = xdr_one;
	*p++ = xdr_zero;
	*p++ = xdr_zero;
	*p++ = xdr_one;
	*p++ = cpu_to_be32(OP_ACCESS);
	*p++ = cpu_to_be32(NFS4_ACCESS_READ | NFS4_ACCESS_LOOKUP);
	wire_len = (void *)p - (void *)wire;
	KUNIT_ASSERT_GT(test, wire_len, 45U);
	lengths[0] = 1;
	lengths[1] = 4;
	lengths[2] = 14;
	lengths[3] = 8;
	lengths[4] = 7;
	lengths[5] = 5;
	lengths[6] = 6;
	lengths[7] = wire_len - 45;
	fixture = nfsd4_q35_matched_fixture_alloc(test, wire, wire_len,
						  lengths,
						  ARRAY_SIZE(lengths));
	nfsd4_q35_page_owner_init(test, page_owner, fixture, true);
	legacy = nfsd4_bvec_decode_context_alloc(test, &fixture->legacy);
	nfsd4_q35_session_context_prepare(test, legacy, session_owner);
	decoded = nfs4svc_decode_compoundargs(&legacy->rqst, &legacy->stream);
	KUNIT_ASSERT_TRUE(test, decoded);
	authoritative = nfsd4_bvec_decode_context_alloc(test,
							&fixture->authoritative);
	nfsd4_q35_session_context_prepare(test, authoritative, session_owner);
	decoded = nfs4svc_decode_compoundargs(&authoritative->rqst,
					      &authoritative->stream);
	KUNIT_ASSERT_TRUE(test, decoded);
	KUNIT_ASSERT_EQ(test, legacy->args->opcnt, 2U);
	KUNIT_ASSERT_EQ(test, authoritative->args->opcnt, 2U);
	KUNIT_EXPECT_MEMEQ(test, &legacy->args->ops[0].u.sequence,
			   &authoritative->args->ops[0].u.sequence,
			   sizeof(struct nfsd4_sequence));
	KUNIT_EXPECT_MEMEQ(test,
			   &authoritative->args->ops[0].u.sequence.sessionid,
			   &session_owner->sessionid,
			   sizeof(session_owner->sessionid));
	KUNIT_EXPECT_EQ(test, authoritative->args->ops[0].u.sequence.seqid,
			1U);
	KUNIT_EXPECT_EQ(test, authoritative->args->ops[0].u.sequence.slotid,
			0U);
	KUNIT_EXPECT_EQ(test, authoritative->args->ops[0].u.sequence.maxslots,
			1U);
	KUNIT_EXPECT_EQ(test, authoritative->args->ops[0].u.sequence.cachethis,
			1U);
	nfsd4_q35_matched_fixture_poison_authoritative(fixture);
	memset(authoritative->stream.scratch.iov_base, 0x5a,
	       authoritative->stream.scratch.iov_len);
	nfsd4_q35_page_owner_expect_held(test, page_owner);
	nfsd4_q35_run_dispatch(test, legacy,
			       NFSD4_Q35_DISPATCH_TEST_AFTER_SEQUENCE,
				legacy_output, PAGE_SIZE, legacy_result);
	/* [p718] the first run must have executed ACCESS for real and populated the slot cache */
	KUNIT_EXPECT_EQ(test, (__force u32)legacy_result->status, (__force u32)nfs_ok);
	KUNIT_EXPECT_EQ(test, legacy->dispatch_count, 1U);
	KUNIT_EXPECT_EQ(test, legacy->dispatch_order[0], (u32)OP_ACCESS);
	nfsd4_q35_page_owner_expect_held(test, page_owner);
	/*
	 * [p718 backport to v7.1.8] The SEQUENCE result is encoded live on a
	 * replay (RFC 8881 lets highest_slotid and target_highest_slotid track
	 * the session), and on this base the slot table grows after the first
	 * request, so a replay never matches the first reply byte for byte.
	 * Compare the authoritative replay against a second legacy replay
	 * instead: that is the property under test (both decode paths replay
	 * identically from the slot cache) and it does not depend on the
	 * slot-growth policy of the base.
	 */
	legacy = nfsd4_bvec_decode_context_alloc(test, &fixture->legacy);
	nfsd4_q35_session_context_prepare(test, legacy, session_owner);
	KUNIT_ASSERT_TRUE(test, nfs4svc_decode_compoundargs(&legacy->rqst, &legacy->stream));
	memset(legacy_result, 0, sizeof(*legacy_result));
	nfsd4_q35_run_dispatch(test, legacy, NFSD4_Q35_DISPATCH_TEST_AFTER_SEQUENCE, legacy_output, PAGE_SIZE, legacy_result);
	KUNIT_EXPECT_EQ(test, legacy->dispatch_count, 0U);
	nfsd4_q35_run_dispatch(test, authoritative,
			       NFSD4_Q35_DISPATCH_TEST_AFTER_SEQUENCE,
				authoritative_output, PAGE_SIZE,
				authoritative_result);
	KUNIT_EXPECT_EQ(test, (__force u32)legacy_result->status,
			(__force u32)nfs_ok);
	KUNIT_EXPECT_EQ(test, (__force u32)authoritative_result->status,
			(__force u32)nfs_ok);
	KUNIT_EXPECT_EQ(test, legacy_result->opcnt, 2U);
	KUNIT_EXPECT_EQ(test, authoritative_result->opcnt, 2U);
	KUNIT_EXPECT_EQ(test, legacy_result->stop_op, (u32)OP_ACCESS);
	KUNIT_EXPECT_EQ(test, authoritative_result->stop_op, (u32)OP_ACCESS);
	KUNIT_ASSERT_EQ(test, authoritative_result->len, legacy_result->len);
	KUNIT_EXPECT_MEMEQ(test, authoritative_output, legacy_output,
			   legacy_result->len);
	/* [p718] both contexts above are replay runs; the first run's dispatch was checked right after it ran */
	KUNIT_EXPECT_EQ(test, authoritative->dispatch_count, 0U);
	nfsd4_q35_page_owner_expect_held(test, page_owner);
	nfsd4_bvec_decode_context_release(authoritative);
	nfsd4_bvec_decode_context_release(legacy);
	nfsd4_q35_page_owner_release(test, page_owner);

	memset(wire, 0, NFSD4_Q35_MAX_WIRE);
	p = nfsd4_bvec_encode_compound_header((__be32 *)wire, 1, 2);
	*p++ = cpu_to_be32(OP_SEQUENCE);
	memcpy(p, &session_owner->sessionid, sizeof(session_owner->sessionid));
	p += XDR_QUADLEN(sizeof(session_owner->sessionid));
	*p++ = cpu_to_be32(2);
	*p++ = xdr_zero;
	*p++ = xdr_zero;
	*p++ = xdr_one;
	*p++ = cpu_to_be32(OP_ACCESS);
	*p++ = cpu_to_be32(NFS4_ACCESS_READ | NFS4_ACCESS_LOOKUP);
	wire_len = (void *)p - (void *)wire;
	lengths[7] = wire_len - 45;
	fixture = nfsd4_q35_matched_fixture_alloc(test, wire, wire_len,
						  lengths,
						  ARRAY_SIZE(lengths));
	nfsd4_q35_page_owner_init(test, page_owner, fixture, true);
	authoritative = nfsd4_bvec_decode_context_alloc(test,
							&fixture->authoritative);
	nfsd4_q35_session_context_prepare(test, authoritative, session_owner);
	decoded = nfs4svc_decode_compoundargs(&authoritative->rqst,
					      &authoritative->stream);
	KUNIT_ASSERT_TRUE(test, decoded);
	legacy = nfsd4_bvec_decode_context_alloc(test, &fixture->legacy);
	nfsd4_q35_session_context_prepare(test, legacy, session_owner);
	decoded = nfs4svc_decode_compoundargs(&legacy->rqst, &legacy->stream);
	KUNIT_ASSERT_TRUE(test, decoded);
	KUNIT_EXPECT_EQ(test, authoritative->args->ops[0].u.sequence.seqid,
			2U);
	nfsd4_q35_matched_fixture_poison_authoritative(fixture);
	memset(authoritative->stream.scratch.iov_base, 0x5a,
	       authoritative->stream.scratch.iov_len);
	memset(authoritative_result, 0, sizeof(*authoritative_result));
	memset(legacy_result, 0, sizeof(*legacy_result));
	memset(authoritative_output, 0, PAGE_SIZE);
	memset(legacy_output, 0, PAGE_SIZE);
	nfsd4_q35_page_owner_expect_held(test, page_owner);
	nfsd4_q35_run_dispatch(test, authoritative,
			       NFSD4_Q35_DISPATCH_TEST_AFTER_SEQUENCE,
				authoritative_output, PAGE_SIZE,
				authoritative_result);
	nfsd4_q35_page_owner_expect_held(test, page_owner);
	nfsd4_q35_run_dispatch(test, legacy,
			       NFSD4_Q35_DISPATCH_TEST_AFTER_SEQUENCE,
				legacy_output, PAGE_SIZE, legacy_result);
	KUNIT_EXPECT_EQ(test, (__force u32)authoritative_result->status,
			(__force u32)nfs_ok);
	KUNIT_EXPECT_EQ(test, (__force u32)legacy_result->status,
			(__force u32)nfs_ok);
	KUNIT_EXPECT_EQ(test, authoritative_result->opcnt, 2U);
	KUNIT_EXPECT_EQ(test, legacy_result->opcnt, 2U);
	KUNIT_ASSERT_EQ(test, legacy_result->len, authoritative_result->len);
	KUNIT_EXPECT_MEMEQ(test, legacy_output, authoritative_output,
			   authoritative_result->len);
	KUNIT_EXPECT_EQ(test, authoritative->dispatch_count, 1U);
	KUNIT_EXPECT_EQ(test, authoritative->dispatch_order[0],
			(u32)OP_ACCESS);
	KUNIT_EXPECT_EQ(test, legacy->dispatch_count, 0U);
	nfsd4_q35_page_owner_expect_held(test, page_owner);
	nfsd4_bvec_decode_context_release(legacy);
	nfsd4_bvec_decode_context_release(authoritative);
	nfsd4_q35_page_owner_release(test, page_owner);

	memset(wire, 0, NFSD4_Q35_MAX_WIRE);
	p = nfsd4_bvec_encode_compound_header((__be32 *)wire, 1, 1);
	*p++ = cpu_to_be32(OP_DESTROY_SESSION);
	memcpy(p, &session_owner->sessionid, sizeof(session_owner->sessionid));
	p += XDR_QUADLEN(sizeof(session_owner->sessionid));
	wire_len = (void *)p - (void *)wire;
	context = nfsd4_q35_session_decode(test, session_owner, wire, wire_len);
	memset(result, 0, sizeof(*result));
	nfsd4_q35_run_dispatch(test, context, NFSD4_Q35_DISPATCH_PRODUCTION,
			       output, PAGE_SIZE, result);
	if (result->status == nfs_ok)
		session_owner->session_live = false;
	KUNIT_ASSERT_EQ(test, (__force u32)result->status,
			(__force u32)nfs_ok);
	KUNIT_ASSERT_EQ(test, result->stop_op, (u32)OP_DESTROY_SESSION);

	memset(wire, 0, NFSD4_Q35_MAX_WIRE);
	p = nfsd4_bvec_encode_compound_header((__be32 *)wire, 1, 1);
	*p++ = cpu_to_be32(OP_DESTROY_CLIENTID);
	memcpy(p, &session_owner->clientid, sizeof(session_owner->clientid));
	p += XDR_QUADLEN(sizeof(session_owner->clientid));
	wire_len = (void *)p - (void *)wire;
	context = nfsd4_q35_session_decode(test, session_owner, wire, wire_len);
	memset(result, 0, sizeof(*result));
	nfsd4_q35_run_dispatch(test, context, NFSD4_Q35_DISPATCH_PRODUCTION,
			       output, PAGE_SIZE, result);
	if (result->status == nfs_ok)
		session_owner->client_live = false;
	KUNIT_ASSERT_EQ(test, (__force u32)result->status,
			(__force u32)nfs_ok);
	KUNIT_ASSERT_EQ(test, result->stop_op, (u32)OP_DESTROY_CLIENTID);
}

static __be32
nfsd4_q35_dispatch_observer(struct svc_rqst *rqstp,
			    struct nfsd4_compound_state *cstate,
			    union nfsd4_op_u *u)
{
	struct nfsd4_bvec_decode_context *context;
	struct nfsd4_compoundargs *args = rqstp->rq_argp;
	struct nfsd4_op *op = NULL;
	unsigned int i;

	context = container_of(rqstp, struct nfsd4_bvec_decode_context, rqst);
	for (i = 0; i < args->opcnt; i++) {
		if (&args->ops[i].u == u) {
			op = &args->ops[i];
			break;
		}
	}
	KUNIT_EXPECT_NOT_NULL(context->test, op);
	if (!op)
		return nfserr_serverfault;
	KUNIT_EXPECT_LT(context->test, context->dispatch_count,
			NFSD4_Q35_MAX_OPS);
	if (context->dispatch_count >= NFSD4_Q35_MAX_OPS)
		return nfserr_resource;
	context->dispatch_order[context->dispatch_count++] = op->opnum;
	if (op->opnum == OP_ACCESS) {
		u->access.ac_supported = u->access.ac_req_access;
		u->access.ac_resp_access = u->access.ac_req_access;
	} else if (op->opnum == OP_WRITE) {
		KUNIT_EXPECT_LE(context->test, u->write.wr_buflen,
				sizeof(context->dispatch_payload));
		if (u->write.wr_buflen > sizeof(context->dispatch_payload))
			return nfserr_resource;
		if (read_bytes_from_xdr_buf(&u->write.wr_payload, 0,
					    context->dispatch_payload,
					    u->write.wr_buflen))
			return nfserr_bad_xdr;
		context->dispatch_payload_len = u->write.wr_buflen;
		u->write.wr_bytes_written = u->write.wr_buflen;
		u->write.wr_how_written = NFS_FILE_SYNC;
		memset(&u->write.wr_verifier, 0, sizeof(u->write.wr_verifier));
	}
	return nfs_ok;
}

static const struct nfsd4_operation nfsd4_q35_dispatch_op = {
	.op_func = nfsd4_q35_dispatch_observer,
	.op_flags = ALLOWED_WITHOUT_FH | ALLOWED_ON_ABSENT_FS,
	.op_name = "OP_Q35_OBSERVER",
};

static unsigned int
nfsd4_q35_run_dispatch(struct kunit *test,
		       struct nfsd4_bvec_decode_context *context,
		       enum nfsd4_q35_dispatch_mode mode,
		       u8 *output, unsigned int output_capacity,
		       struct nfsd4_q35_dispatch_result *result)
{
	const struct svc_procedure *compound;
	struct nfsd4_compoundres *resp;
	struct page *response_pages[3];
	unsigned int i;
	__be32 status;
	bool encoded;

	resp = kunit_kzalloc(test, sizeof(*resp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, resp);
	for (i = 0; i < ARRAY_SIZE(response_pages); i++) {
		response_pages[i] = nfsd4_bvec_alloc_page(test);
		KUNIT_ASSERT_NOT_NULL(test, response_pages[i]);
	}
	compound = nfsd4_procedure(NFSPROC4_COMPOUND);
	KUNIT_ASSERT_NOT_NULL(test, compound);
	KUNIT_ASSERT_NOT_NULL(test, compound->pc_func);
	KUNIT_ASSERT_NOT_NULL(test, compound->pc_encode);
	KUNIT_ASSERT_NOT_NULL(test, result);
	if (mode != NFSD4_Q35_DISPATCH_PRODUCTION) {
		unsigned int first = mode == NFSD4_Q35_DISPATCH_TEST_ALL ? 0 : 1;

		for (i = first; i < context->args->opcnt; i++)
			context->args->ops[i].opdesc = &nfsd4_q35_dispatch_op;
	}
	context->xprt.xpt_net = &init_net;
	context->rqst.rq_procinfo = compound;
	context->rqst.rq_resp = resp;
	context->rqst.rq_res.head[0].iov_base = page_address(response_pages[0]);
	context->rqst.rq_res.head[0].iov_len = 0;
	context->rqst.rq_res.pages = &response_pages[1];
	context->rqst.rq_res.page_mode = XDRBUF_PAGE_ARRAY;
	context->rqst.rq_respages = &response_pages[1];
	context->rqst.rq_next_page = &response_pages[1];
	context->rqst.rq_page_end = &response_pages[3];
	svcxdr_init_encode(&context->rqst);

	status = compound->pc_func(&context->rqst);
	KUNIT_ASSERT_EQ(test, (__force u32)status, (__force u32)rpc_success);
	encoded = compound->pc_encode(&context->rqst,
				      &context->rqst.rq_res_stream);
	KUNIT_ASSERT_TRUE(test, encoded);
	KUNIT_ASSERT_LE(test, context->rqst.rq_res.len, output_capacity);
	KUNIT_ASSERT_EQ(test,
			read_bytes_from_xdr_buf(&context->rqst.rq_res, 0, output,
						context->rqst.rq_res.len),
			0);
	result->status = resp->cstate.status;
	result->opcnt = resp->opcnt;
	result->stop_op = resp->opcnt ?
		context->args->ops[resp->opcnt - 1].opnum : OP_ILLEGAL;
	result->len = context->rqst.rq_res.len;
	return result->len;
}

static unsigned int
nfsd4_q35_expected_response(const struct nfsd4_q35_case *test_case,
			    __be32 final_status, __be32 *response)
{
	__be32 *p = response;
	unsigned int i;

	*p++ = final_status;
	*p++ = xdr_zero;
	*p++ = cpu_to_be32(test_case->opcnt);
	for (i = 0; i < test_case->opcnt; i++) {
		__be32 op_status = i + 1 == test_case->opcnt ?
			final_status : nfs_ok;

		*p++ = cpu_to_be32(nfsd4_q35_expected_opnum(test_case->ops[i]));
		*p++ = op_status;
		if (op_status)
			break;
		switch (test_case->ops[i]) {
		case NFSD4_Q35_ACCESS:
			*p++ = cpu_to_be32(NFS4_ACCESS_READ |
					     NFS4_ACCESS_LOOKUP);
			*p++ = cpu_to_be32(NFS4_ACCESS_READ |
					     NFS4_ACCESS_LOOKUP);
			break;
		case NFSD4_Q35_WRITE:
			*p++ = cpu_to_be32(19);
			*p++ = cpu_to_be32(NFS_FILE_SYNC);
			*p++ = xdr_zero;
			*p++ = xdr_zero;
			break;
		case NFSD4_Q35_LOOKUP:
			break;
		}
	}
	return (void *)p - (void *)response;
}

static void
nfsd4_q35_expect_dispatch_pair(struct kunit *test,
			       const struct nfsd4_q35_case *test_case,
			       struct nfsd4_bvec_decode_context *legacy,
			       struct nfsd4_bvec_decode_context *authoritative,
			       __be32 expected_status,
			       unsigned int expected_dispatches)
{
	struct nfsd4_q35_dispatch_result *authoritative_result;
	struct nfsd4_q35_dispatch_result *legacy_result;
	u8 *authoritative_output;
	__be32 *expected;
	u8 *legacy_output;
	unsigned int expected_len;
	unsigned int i;

	authoritative_result = kunit_kzalloc(test,
					     sizeof(*authoritative_result),
					     GFP_KERNEL);
	legacy_result = kunit_kzalloc(test, sizeof(*legacy_result), GFP_KERNEL);
	authoritative_output = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	legacy_output = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	expected = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, authoritative_result);
	KUNIT_ASSERT_NOT_NULL(test, legacy_result);
	KUNIT_ASSERT_NOT_NULL(test, authoritative_output);
	KUNIT_ASSERT_NOT_NULL(test, legacy_output);
	KUNIT_ASSERT_NOT_NULL(test, expected);
	expected_len = nfsd4_q35_expected_response(test_case, expected_status,
						   expected);
	nfsd4_q35_run_dispatch(test, legacy, NFSD4_Q35_DISPATCH_TEST_ALL,
			       legacy_output, PAGE_SIZE, legacy_result);
	nfsd4_q35_run_dispatch(test, authoritative,
			       NFSD4_Q35_DISPATCH_TEST_ALL,
				authoritative_output, PAGE_SIZE,
				authoritative_result);
	KUNIT_EXPECT_EQ(test, (__force u32)legacy_result->status,
			(__force u32)expected_status);
	KUNIT_EXPECT_EQ(test, (__force u32)authoritative_result->status,
			(__force u32)expected_status);
	KUNIT_EXPECT_EQ(test, legacy_result->opcnt, test_case->opcnt);
	KUNIT_EXPECT_EQ(test, authoritative_result->opcnt, test_case->opcnt);
	KUNIT_EXPECT_EQ(test, legacy_result->stop_op,
			nfsd4_q35_expected_opnum(test_case->ops[test_case->opcnt - 1]));
	KUNIT_EXPECT_EQ(test, authoritative_result->stop_op,
			legacy_result->stop_op);
	KUNIT_ASSERT_EQ(test, legacy_result->len, expected_len);
	KUNIT_ASSERT_EQ(test, authoritative_result->len, expected_len);
	KUNIT_EXPECT_MEMEQ(test, legacy_output, expected, expected_len);
	KUNIT_EXPECT_MEMEQ(test, authoritative_output, expected, expected_len);
	KUNIT_EXPECT_MEMEQ(test, authoritative_output, legacy_output,
			   expected_len);
	KUNIT_ASSERT_EQ(test, legacy->dispatch_count, expected_dispatches);
	KUNIT_ASSERT_EQ(test, authoritative->dispatch_count,
			expected_dispatches);
	for (i = 0; i < expected_dispatches; i++) {
		u32 expected_op = nfsd4_q35_expected_opnum(test_case->ops[i]);

		KUNIT_EXPECT_EQ(test, legacy->dispatch_order[i], expected_op);
		KUNIT_EXPECT_EQ(test, authoritative->dispatch_order[i],
				expected_op);
	}
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
	KUNIT_CASE(nfsd4_q35_decode_matrix_test),
	KUNIT_CASE(nfsd4_q35_malformed_post_write_test),
	KUNIT_CASE(nfsd4_bvec_savemem_lifetime_test),
	KUNIT_CASE(nfsd4_q35_saved_values_lifetime_test),
	KUNIT_CASE(nfsd4_q35_session_replay_test),
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
