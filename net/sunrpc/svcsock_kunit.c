// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <kunit/skbuff.h>

#include <linux/bitmap.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/percpu_counter.h>
#include <linux/skbuff.h>
#include <linux/sunrpc/msg_prot.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/unaligned.h>

#include <net/net_namespace.h>

#define SVCSOCK_RX_TEST_PROGRAM	100003
#define SVCSOCK_RX_TEST_VERSION	3
#define SVCSOCK_RX_TEST_PROCEDURE	7

struct svcsock_rx_fixture {
	struct svc_sock *svsk;
	struct svc_rqst *rqstp;
};

struct svcsock_rx_actor_state {
	unsigned int frags;
	bool complete;
};

static void svcsock_rx_free_state(void *data);
static void svcsock_rx_write(struct page *page, unsigned int offset,
			     const void *source, unsigned int len);

static const struct svc_procedure svcsock_rx_test_procedures[] = {
	[SVCSOCK_RX_TEST_PROCEDURE] = {
		.pc_xdr_bvec = 1,
	},
};

static const struct svc_version svcsock_rx_test_version = {
	.vs_vers = SVCSOCK_RX_TEST_VERSION,
	.vs_nproc = ARRAY_SIZE(svcsock_rx_test_procedures),
	.vs_proc = svcsock_rx_test_procedures,
};

static const struct svc_version *svcsock_rx_test_versions[] = {
	[SVCSOCK_RX_TEST_VERSION] = &svcsock_rx_test_version,
};

static struct svc_program svcsock_rx_test_program = {
	.pg_prog = SVCSOCK_RX_TEST_PROGRAM,
	.pg_lovers = SVCSOCK_RX_TEST_VERSION,
	.pg_hivers = SVCSOCK_RX_TEST_VERSION,
	.pg_nvers = ARRAY_SIZE(svcsock_rx_test_versions),
	.pg_vers = svcsock_rx_test_versions,
	.pg_name = "svcsock-rx-kunit",
};

static void svcsock_rx_free_page(void *data)
{
	put_page(data);
}

static struct page *svcsock_rx_alloc_page(struct kunit *test)
{
	struct page *page = alloc_page(GFP_KERNEL);

	if (!page)
		return NULL;
	if (kunit_add_action_or_reset(test, svcsock_rx_free_page, page))
		return NULL;
	return page;
}

static struct svc_rqst *
svcsock_rx_alloc_request(struct kunit *test,
			 struct svcsock_rx_fixture *fixture,
			 unsigned int capacity)
{
	struct svc_rqst *rqstp;
	unsigned int i;

	rqstp = kunit_kzalloc(test, sizeof(*rqstp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rqstp);
	rqstp->rq_pages = kunit_kcalloc(test, capacity,
					sizeof(*rqstp->rq_pages), GFP_KERNEL);
	rqstp->rq_bvec = kunit_kcalloc(test, capacity,
				       sizeof(*rqstp->rq_bvec), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_pages);
	KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_bvec);
	for (i = 0; i < capacity; i++) {
		rqstp->rq_pages[i] = svcsock_rx_alloc_page(test);
		KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_pages[i]);
	}

	rqstp->rq_tcp_rx = svc_tcp_rx_state_alloc(capacity, GFP_KERNEL,
						  NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_tcp_rx);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							svcsock_rx_free_state,
						      rqstp->rq_tcp_rx), 0);
	rqstp->rq_xprt = &fixture->svsk->sk_xprt;
	rqstp->rq_server = fixture->svsk->sk_xprt.xpt_server;
	rqstp->rq_maxpages = capacity;
	rqstp->rq_arg.head[0].iov_base = page_address(rqstp->rq_pages[0]);
	rqstp->rq_arg.head[0].iov_len = PAGE_SIZE;
	rqstp->rq_arg.pages = rqstp->rq_pages;
	return rqstp;
}

static struct svcsock_rx_fixture *
svcsock_rx_alloc_fixture(struct kunit *test, unsigned int capacity)
{
	struct svcsock_rx_fixture *fixture;
	struct svc_serv *serv;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->svsk = kunit_kzalloc(test,
				      struct_size(fixture->svsk, sk_pages, capacity),
			GFP_KERNEL);
	serv = kunit_kzalloc(test, sizeof(*serv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fixture->svsk);
	KUNIT_ASSERT_NOT_NULL(test, serv);

	fixture->svsk->sk_rx = svc_tcp_rx_state_alloc(capacity, GFP_KERNEL,
						      NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, fixture->svsk->sk_rx);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							svcsock_rx_free_state,
						      fixture->svsk->sk_rx), 0);

	serv->sv_max_mesg = capacity * PAGE_SIZE;
	serv->sv_nprogs = 1;
	serv->sv_programs = &svcsock_rx_test_program;
	fixture->svsk->sk_maxpages = capacity;
	fixture->svsk->sk_xprt.xpt_server = serv;
	fixture->rqstp = svcsock_rx_alloc_request(test, fixture, capacity);
	return fixture;
}

static void svcsock_rx_fill_body(u8 *body, unsigned int len, u32 direction,
				 u32 auth)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		body[i] = (u8)(0x31 + i * 17);
	put_unaligned_be32(0x10203040, body);
	put_unaligned_be32(direction, body + XDR_UNIT);
	put_unaligned_be32(RPC_VERSION, body + 2 * XDR_UNIT);
	put_unaligned_be32(SVCSOCK_RX_TEST_PROGRAM, body + 3 * XDR_UNIT);
	put_unaligned_be32(SVCSOCK_RX_TEST_VERSION, body + 4 * XDR_UNIT);
	put_unaligned_be32(SVCSOCK_RX_TEST_PROCEDURE, body + 5 * XDR_UNIT);
	put_unaligned_be32(auth, body + 6 * XDR_UNIT);
}

static void svcsock_rx_fill_record(u8 *record, unsigned int body_len,
				   bool final, u32 direction, u32 auth)
{
	u32 marker = body_len;

	if (final)
		marker |= RPC_LAST_STREAM_FRAGMENT;
	put_unaligned_be32(marker, record);
	svcsock_rx_fill_body(record + sizeof(rpc_fraghdr), body_len,
			     direction, auth);
}

static void svcsock_rx_fill_fragment(u8 *fragment, const u8 *body,
				     unsigned int body_offset,
				     unsigned int fragment_len, bool final)
{
	u32 marker = fragment_len;

	if (final)
		marker |= RPC_LAST_STREAM_FRAGMENT;
	put_unaligned_be32(marker, fragment);
	memcpy(fragment + sizeof(rpc_fraghdr), body + body_offset, fragment_len);
}

static struct sk_buff *
svcsock_rx_build_page_skb(struct kunit *test, const void *bytes,
			  unsigned int len, struct page **head_page,
			  unsigned int *head_refs)
{
	struct sk_buff *skb;
	struct page *page;

	KUNIT_ASSERT_LE(test, len, (unsigned int)SKB_WITH_OVERHEAD(PAGE_SIZE));
	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	get_page(page);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							svcsock_rx_free_page, page), 0);
	skb = build_skb(page_address(page), PAGE_SIZE);
	if (!skb) {
		put_page(page);
		KUNIT_FAIL(test, "build_skb failed");
		return NULL;
	}
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							kunit_action_kfree_skb, skb), 0);
	__skb_put_data(skb, bytes, len);
	*head_page = page;
	*head_refs = page_ref_count(page);
	return skb;
}

static struct sk_buff *
svcsock_rx_build_linear_skb(struct kunit *test, const void *bytes,
			    unsigned int len)
{
	struct sk_buff *skb = alloc_skb(len, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							kunit_action_kfree_skb, skb), 0);
	__skb_put_data(skb, bytes, len);
	return skb;
}

static struct page *
svcsock_rx_add_frag(struct kunit *test, struct sk_buff *skb,
		    unsigned int index, const void *bytes, unsigned int len,
		    unsigned int offset, unsigned int *refs)
{
	struct page *page = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_LE(test, offset + len, (unsigned int)PAGE_SIZE);
	get_page(page);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							svcsock_rx_free_page, page), 0);
	svcsock_rx_write(page, offset, bytes, len);
	skb_add_rx_frag(skb, index, page, offset, len, PAGE_SIZE);
	*refs = page_ref_count(page);
	return page;
}

static struct page *
svcsock_rx_add_compound_frag(struct kunit *test, struct sk_buff *skb,
			     unsigned int index, const void *bytes,
			     unsigned int len, unsigned int offset,
			     unsigned int *refs)
{
	struct page *page;
	unsigned int first;

	KUNIT_ASSERT_GT(test, offset + len, (unsigned int)PAGE_SIZE);
	KUNIT_ASSERT_LE(test, offset + len, 2U * PAGE_SIZE);
	page = alloc_pages(GFP_KERNEL | __GFP_COMP, 1);
	KUNIT_ASSERT_NOT_NULL(test, page);
	get_page(page);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							svcsock_rx_free_page, page), 0);
	first = PAGE_SIZE - offset;
	svcsock_rx_write(page, offset, bytes, first);
	svcsock_rx_write(page + 1, 0, bytes + first, len - first);
	skb_add_rx_frag(skb, index, page, offset, len, 2 * PAGE_SIZE);
	*refs = page_ref_count(page);
	return page;
}

static void svcsock_rx_attach_frag_list(struct kunit *test,
					struct sk_buff *parent,
					struct sk_buff *child)
{
	KUNIT_ASSERT_PTR_EQ(test, skb_shinfo(parent)->frag_list, NULL);
	skb_shinfo(parent)->frag_list = child;
	parent->len += child->len;
	parent->data_len += child->len;
	parent->truesize += child->truesize;
	kunit_remove_action(test, kunit_action_kfree_skb, child);
}

static void svcsock_rx_expect_bvec_bytes(struct kunit *test,
					 struct svc_tcp_rx_state *state,
					const u8 *expected, unsigned int len)
{
	unsigned int index, cursor = 0;

	for (index = 0; index < state->count; index++) {
		const struct bio_vec *bvec = &state->bvec[index];
		void *address;

		KUNIT_ASSERT_LE(test, cursor + bvec->bv_len, len);
		address = kmap_local_page(bvec->bv_page);
		KUNIT_EXPECT_MEMEQ(test, address + bvec->bv_offset,
				   expected + cursor, bvec->bv_len);
		kunmap_local(address);
		cursor += bvec->bv_len;
	}
	KUNIT_EXPECT_EQ(test, cursor, len);
}

static void svcsock_rx_expect_unpublished(struct kunit *test,
					  const struct svc_rqst *rqstp)
{
	KUNIT_EXPECT_NE(test, rqstp->rq_tcp_rx->mode,
			(u8)SVC_TCP_RX_PUBLISHED);
	KUNIT_EXPECT_PTR_EQ(test, rqstp->rq_xprt_ctxt, NULL);
	KUNIT_EXPECT_PTR_EQ(test, rqstp->rq_arg.bvec, NULL);
	KUNIT_EXPECT_EQ(test, rqstp->rq_arg.bvec_count, 0U);
}

static int svcsock_rx_run_actor(struct svc_rqst *rqstp,
				struct svcsock_rx_actor_state *ctx,
				read_descriptor_t *desc,
				struct sk_buff *skb, unsigned int offset,
				unsigned int len)
{
	return svc_tcp_recv_actor_kunit(rqstp, &ctx->frags,
					 &ctx->complete, desc, skb, offset, len);
}

struct svcsock_rx_read_sock_io {
	struct sk_buff *skb;
	unsigned int offset;
	unsigned int len;
};

struct svcsock_rx_socket_owner {
	struct socket *sock;
	const struct proto_ops *ops;
};

static int svcsock_rx_test_read_sock(struct sock *sk, read_descriptor_t *desc,
				     sk_read_actor_t actor)
{
	struct svcsock_rx_read_sock_io *io = sk->sk_user_data;

	return actor(desc, io->skb, io->offset, io->len);
}

static const struct proto_ops svcsock_rx_test_proto_ops = {
	.read_sock = svcsock_rx_test_read_sock,
};

static void svcsock_rx_release_socket(void *data)
{
	struct svcsock_rx_socket_owner *owner = data;

	owner->sock->ops = owner->ops;
	sock_release(owner->sock);
}

static void svcsock_rx_destroy_pool_counter(void *data)
{
	percpu_counter_destroy(data);
}

static struct svc_pool *
svcsock_rx_prepare_outer_receive(struct kunit *test,
				 struct svcsock_rx_fixture *fixture,
				 struct sk_buff *skb)
{
	struct svcsock_rx_read_sock_io *io;
	struct svcsock_rx_socket_owner *owner;
	struct svc_pool *pool;
	struct socket *sock;
	int ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &sock);
	KUNIT_ASSERT_EQ(test, ret, 0);
	owner = kunit_kmalloc(test, sizeof(*owner), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, owner);
	owner->sock = sock;
	owner->ops = sock->ops;
	sock->ops = &svcsock_rx_test_proto_ops;
	ret = kunit_add_action_or_reset(test, svcsock_rx_release_socket, owner);
	KUNIT_ASSERT_EQ(test, ret, 0);

	io = kunit_kzalloc(test, sizeof(*io), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, io);
	io->skb = skb;
	io->len = skb->len;
	sock->sk->sk_user_data = io;
	fixture->svsk->sk_sock = sock;
	fixture->svsk->sk_sk = sock->sk;

	pool = kunit_kzalloc(test, sizeof(*pool), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pool);
	lwq_init(&pool->sp_xprts);
	init_llist_head(&pool->sp_idle_threads);
	ret = percpu_counter_init(&pool->sp_sockets_queued, 0, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test,
					svcsock_rx_destroy_pool_counter,
					&pool->sp_sockets_queued);
	KUNIT_ASSERT_EQ(test, ret, 0);
	fixture->rqstp->rq_server->sv_pools = pool;

	kref_init(&fixture->svsk->sk_xprt.xpt_ref);
	INIT_LIST_HEAD(&fixture->svsk->sk_xprt.xpt_list);
	INIT_LIST_HEAD(&fixture->svsk->sk_xprt.xpt_deferred);
	INIT_LIST_HEAD(&fixture->svsk->sk_xprt.xpt_users);
	set_bit(XPT_BUSY, &fixture->svsk->sk_xprt.xpt_flags);
	return pool;
}

static void svcsock_rx_expect_outer_close(struct kunit *test,
					  struct svcsock_rx_fixture *fixture,
					  struct svc_pool *pool)
{
	struct svc_xprt *queued;

	KUNIT_EXPECT_TRUE(test,
			  test_bit(XPT_CLOSE,
				   &fixture->svsk->sk_xprt.xpt_flags));
	queued = lwq_dequeue(&pool->sp_xprts, struct svc_xprt, xpt_ready);
	KUNIT_EXPECT_PTR_EQ(test, queued, &fixture->svsk->sk_xprt);
	if (queued)
		percpu_counter_dec(&pool->sp_sockets_queued);
	KUNIT_EXPECT_TRUE(test, lwq_empty(&pool->sp_xprts));
	clear_bit(XPT_BUSY, &fixture->svsk->sk_xprt.xpt_flags);
}

static void svcsock_rx_clear_fault(void *unused)
{
	(void)unused;
	svc_tcp_rx_kunit_fault_set(SVC_TCP_RX_KUNIT_FAULT_NONE, 0);
}

enum svcsock_rx_split_kind {
	SVCSOCK_RX_SPLIT_CLASSIFIER,
	SVCSOCK_RX_SPLIT_MARKER,
};

struct svcsock_rx_split_param {
	enum svcsock_rx_split_kind kind;
	unsigned int split;
	u32 auth;
};

#define SVCSOCK_RX_CLASSIFIER_SPLIT(_split, _auth) \
	{ SVCSOCK_RX_SPLIT_CLASSIFIER, (_split), (_auth) }
#define SVCSOCK_RX_MARKER_SPLIT(_split, _auth) \
	{ SVCSOCK_RX_SPLIT_MARKER, (_split), (_auth) }

static const struct svcsock_rx_split_param svcsock_rx_split_params[] = {
	SVCSOCK_RX_CLASSIFIER_SPLIT(0, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(1, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(2, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(3, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(4, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(5, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(6, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(7, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(8, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(9, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(10, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(11, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(12, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(13, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(14, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(15, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(16, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(17, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(18, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(19, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(20, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(21, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(22, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(23, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(24, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(25, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(26, RPC_AUTH_NULL),
	SVCSOCK_RX_CLASSIFIER_SPLIT(27, RPC_AUTH_UNIX),
	SVCSOCK_RX_CLASSIFIER_SPLIT(28, RPC_AUTH_NULL),
	SVCSOCK_RX_MARKER_SPLIT(1, RPC_AUTH_NULL),
	SVCSOCK_RX_MARKER_SPLIT(2, RPC_AUTH_UNIX),
	SVCSOCK_RX_MARKER_SPLIT(3, RPC_AUTH_NULL),
};

static void
svcsock_rx_split_param_desc(const struct svcsock_rx_split_param *param,
			    char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s-%u-auth-%s",
		 param->kind == SVCSOCK_RX_SPLIT_CLASSIFIER ?
			"classifier" : "marker",
		 param->split,
		 param->auth == RPC_AUTH_NULL ? "null" : "sys");
}

KUNIT_ARRAY_PARAM(svcsock_rx_split, svcsock_rx_split_params,
		  svcsock_rx_split_param_desc);

static void svcsock_rx_actor_split_test(struct kunit *test)
{
	const struct svcsock_rx_split_param *param = test->param_value;
	enum { BODY_LEN = 7 * XDR_UNIT + 19 };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(record) };
	struct page *page;
	unsigned int refs;
	struct sk_buff *skb;
	unsigned int first_len;
	int ret;

	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, param->auth);
	skb = svcsock_rx_build_page_skb(test, record, sizeof(record), &page,
					&refs);
	first_len = param->kind == SVCSOCK_RX_SPLIT_CLASSIFIER ?
			sizeof(rpc_fraghdr) + param->split : param->split;

	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0,
				   first_len);
	KUNIT_ASSERT_EQ(test, ret, (int)first_len);
	KUNIT_EXPECT_FALSE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_EQ(test, state->refs_acquired, 0U);
	svcsock_rx_expect_unpublished(test, fixture->rqstp);
	if (param->kind == SVCSOCK_RX_SPLIT_CLASSIFIER) {
		KUNIT_EXPECT_EQ(test, state->body_bytes, (u32)param->split);
		KUNIT_EXPECT_EQ(test, state->fixed_bytes, (u32)param->split);
	} else {
		KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
		KUNIT_EXPECT_EQ(test, fixture->svsk->sk_tcplen,
				(u32)param->split);
	}
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);

	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb,
				   first_len, sizeof(record) - first_len);
	KUNIT_ASSERT_EQ(test, ret, (int)(sizeof(record) - first_len));
	KUNIT_EXPECT_TRUE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_MIXED);
	KUNIT_EXPECT_EQ(test, state->reason, (u8)SVC_TCP_RX_REASON_NONE);
	KUNIT_EXPECT_EQ(test, state->body_bytes, (u32)BODY_LEN);
	KUNIT_EXPECT_EQ(test, state->fixed_bytes, 7U * XDR_UNIT);
	KUNIT_EXPECT_EQ(test, state->refs_acquired, 1U);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs + 1);
	svcsock_rx_expect_unpublished(test, fixture->rqstp);

	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_publish(fixture->svsk, fixture->rqstp), 0);
	kunit_kfree_skb(test, skb);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	KUNIT_EXPECT_MEMEQ(test, page_address(fixture->rqstp->rq_pages[0]),
			   record + sizeof(rpc_fraghdr), 7 * XDR_UNIT);
	svcsock_rx_expect_bvec_bytes(test, state,
				     record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT,
				     BODY_LEN - 7 * XDR_UNIT);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
}

static void svcsock_rx_multifragment_fresh_request_test(struct kunit *test)
{
	enum {
		BODY_LEN = 7 * XDR_UNIT + 23,
		FIRST_FRAGMENT_LEN = 7 * XDR_UNIT + 7,
		SECOND_FRAGMENT_LEN = BODY_LEN - FIRST_FRAGMENT_LEN,
	};
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_rqst *first_rqstp = fixture->rqstp;
	struct svc_rqst *fresh_rqstp =
		svcsock_rx_alloc_request(test, fixture, 4);
	struct svc_tcp_rx_state *active = first_rqstp->rq_tcp_rx;
	u8 first_fragment[sizeof(rpc_fraghdr) + FIRST_FRAGMENT_LEN];
	u8 second_fragment[sizeof(rpc_fraghdr) + SECOND_FRAGMENT_LEN];
	u8 body[BODY_LEN];
	struct svcsock_rx_actor_state first_actor = {};
	struct svcsock_rx_actor_state second_actor = {};
	read_descriptor_t first_desc = { .count = sizeof(first_fragment) };
	read_descriptor_t second_desc = { .count = sizeof(second_fragment) };
	struct page *first_page, *second_page;
	unsigned int first_refs, second_refs;
	struct sk_buff *first_skb, *second_skb;
	int ret;

	svcsock_rx_fill_body(body, sizeof(body), RPC_CALL, RPC_AUTH_UNIX);
	svcsock_rx_fill_fragment(first_fragment, body, 0,
				 FIRST_FRAGMENT_LEN, false);
	svcsock_rx_fill_fragment(second_fragment, body, FIRST_FRAGMENT_LEN,
				 SECOND_FRAGMENT_LEN, true);
	first_skb = svcsock_rx_build_page_skb(test, first_fragment,
					      sizeof(first_fragment), &first_page,
					      &first_refs);
	second_skb = svcsock_rx_build_page_skb(test, second_fragment,
					       sizeof(second_fragment), &second_page,
					       &second_refs);

	ret = svcsock_rx_run_actor(first_rqstp, &first_actor, &first_desc,
				   first_skb, 0, first_skb->len);
	KUNIT_ASSERT_EQ(test, ret, (int)first_skb->len);
	KUNIT_EXPECT_FALSE(test, first_actor.complete);
	KUNIT_EXPECT_EQ(test, first_actor.frags, 1U);
	KUNIT_EXPECT_EQ(test, active->mode, (u8)SVC_TCP_RX_MIXED);
	KUNIT_EXPECT_EQ(test, active->body_bytes, (u32)FIRST_FRAGMENT_LEN);
	KUNIT_EXPECT_EQ(test, active->refs_acquired, 1U);
	svcsock_rx_expect_unpublished(test, first_rqstp);
	KUNIT_EXPECT_EQ(test, page_ref_count(first_page), first_refs + 1);

	ret = svc_tcp_save_pages(fixture->svsk, first_rqstp);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, fixture->svsk->sk_rx, active);
	KUNIT_EXPECT_EQ(test, page_ref_count(first_page), first_refs + 1);
	svcsock_rx_expect_unpublished(test, first_rqstp);

	kunit_release_action(test, svcsock_rx_free_page,
			     fresh_rqstp->rq_pages[0]);
	fresh_rqstp->rq_pages[0] = NULL;
	ret = svc_tcp_restore_pages(fixture->svsk, fresh_rqstp);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, fresh_rqstp->rq_tcp_rx, active);
	KUNIT_EXPECT_EQ(test, active->body_bytes, (u32)FIRST_FRAGMENT_LEN);
	svcsock_rx_expect_unpublished(test, fresh_rqstp);

	ret = svcsock_rx_run_actor(fresh_rqstp, &second_actor, &second_desc,
				   second_skb, 0, second_skb->len);
	KUNIT_ASSERT_EQ(test, ret, (int)second_skb->len);
	KUNIT_EXPECT_TRUE(test, second_actor.complete);
	KUNIT_EXPECT_EQ(test, active->body_bytes, (u32)BODY_LEN);
	KUNIT_EXPECT_EQ(test, active->refs_acquired, 2U);
	KUNIT_EXPECT_EQ(test, page_ref_count(second_page), second_refs + 1);
	svcsock_rx_expect_unpublished(test, fresh_rqstp);
	ret = svc_tcp_rx_publish(fixture->svsk, fresh_rqstp);
	KUNIT_ASSERT_EQ(test, ret, 0);

	kunit_kfree_skb(test, first_skb);
	kunit_kfree_skb(test, second_skb);
	KUNIT_EXPECT_EQ(test, page_ref_count(first_page), first_refs);
	KUNIT_EXPECT_EQ(test, page_ref_count(second_page), second_refs);
	KUNIT_EXPECT_MEMEQ(test, page_address(fresh_rqstp->rq_pages[0]), body,
			   7 * XDR_UNIT);
	svcsock_rx_expect_bvec_bytes(test, active, body + 7 * XDR_UNIT,
				     BODY_LEN - 7 * XDR_UNIT);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, active);
	KUNIT_EXPECT_EQ(test, page_ref_count(first_page), first_refs - 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(second_page), second_refs - 1);
	KUNIT_EXPECT_EQ(test, active->mode, (u8)SVC_TCP_RX_EMPTY);
	svc_tcp_recv_record_done(fixture->svsk);
	svc_tcp_clear_pages(fixture->svsk);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_datalen, 0U);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_tcplen, 0U);
}

static void svcsock_rx_two_records_one_skb_test(struct kunit *test)
{
	enum {
		FIRST_BODY_LEN = 7 * XDR_UNIT + 11,
		SECOND_BODY_LEN = 7 * XDR_UNIT + 17,
		FIRST_RECORD_LEN = sizeof(rpc_fraghdr) + FIRST_BODY_LEN,
		SECOND_RECORD_LEN = sizeof(rpc_fraghdr) + SECOND_BODY_LEN,
		STREAM_LEN = FIRST_RECORD_LEN + SECOND_RECORD_LEN,
	};
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_rqst *first_rqstp = fixture->rqstp;
	struct svc_rqst *second_rqstp =
		svcsock_rx_alloc_request(test, fixture, 4);
	u8 stream[STREAM_LEN];
	struct svcsock_rx_actor_state first_actor = {};
	struct svcsock_rx_actor_state second_actor = {};
	read_descriptor_t first_desc = { .count = sizeof(stream) };
	read_descriptor_t second_desc = { .count = SECOND_RECORD_LEN };
	struct page *page;
	unsigned int refs;
	struct sk_buff *skb;
	int consumed;

	svcsock_rx_fill_record(stream, FIRST_BODY_LEN, true, RPC_CALL,
			       RPC_AUTH_NULL);
	svcsock_rx_fill_record(stream + FIRST_RECORD_LEN, SECOND_BODY_LEN,
			       true, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_page_skb(test, stream, sizeof(stream), &page,
					&refs);

	consumed = svcsock_rx_run_actor(first_rqstp, &first_actor, &first_desc,
					skb, 0, skb->len);
	KUNIT_ASSERT_EQ(test, consumed, (int)FIRST_RECORD_LEN);
	KUNIT_EXPECT_TRUE(test, first_actor.complete);
	KUNIT_EXPECT_EQ(test, first_rqstp->rq_tcp_rx->refs_acquired, 1U);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs + 1);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_publish(fixture->svsk, first_rqstp), 0);
	KUNIT_EXPECT_MEMEQ(test, page_address(first_rqstp->rq_pages[0]),
			   stream + sizeof(rpc_fraghdr), 7 * XDR_UNIT);
	svcsock_rx_expect_bvec_bytes(test, first_rqstp->rq_tcp_rx,
				     stream + sizeof(rpc_fraghdr) + 7 * XDR_UNIT,
				     FIRST_BODY_LEN - 7 * XDR_UNIT);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt,
			     first_rqstp->rq_tcp_rx);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	svc_tcp_recv_record_done(fixture->svsk);

	consumed = svcsock_rx_run_actor(second_rqstp, &second_actor,
					&second_desc, skb, FIRST_RECORD_LEN,
					SECOND_RECORD_LEN);
	KUNIT_ASSERT_EQ(test, consumed, (int)SECOND_RECORD_LEN);
	KUNIT_EXPECT_TRUE(test, second_actor.complete);
	KUNIT_EXPECT_EQ(test, second_rqstp->rq_tcp_rx->refs_acquired, 1U);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_rx_sequence, 2ULL);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_publish(fixture->svsk, second_rqstp), 0);

	kunit_kfree_skb(test, skb);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	KUNIT_EXPECT_MEMEQ(test, page_address(second_rqstp->rq_pages[0]),
			   stream + FIRST_RECORD_LEN + sizeof(rpc_fraghdr),
			   7 * XDR_UNIT);
	svcsock_rx_expect_bvec_bytes(test, second_rqstp->rq_tcp_rx,
				     stream + FIRST_RECORD_LEN + sizeof(rpc_fraghdr) + 7 * XDR_UNIT,
		SECOND_BODY_LEN - 7 * XDR_UNIT);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt,
			     second_rqstp->rq_tcp_rx);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
	KUNIT_EXPECT_EQ(test, second_rqstp->rq_tcp_rx->mode,
			(u8)SVC_TCP_RX_EMPTY);
}

struct svcsock_rx_clear_param {
	const char *event;
};

static const struct svcsock_rx_clear_param svcsock_rx_clear_params[] = {
	{ .event = "disconnect" },
	{ .event = "shutdown" },
};

static void
svcsock_rx_clear_param_desc(const struct svcsock_rx_clear_param *param,
			    char *desc)
{
	strscpy(desc, param->event, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(svcsock_rx_clear, svcsock_rx_clear_params,
		  svcsock_rx_clear_param_desc);

static void svcsock_rx_socket_owned_clear_test(struct kunit *test)
{
	const struct svcsock_rx_clear_param *param = test->param_value;
	enum { BODY_LEN = 7 * XDR_UNIT + 12 };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *active = fixture->rqstp->rq_tcp_rx;
	u8 fragment[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(fragment) };
	struct page *source_page;
	unsigned int source_refs;
	struct sk_buff *skb;
	unsigned int i;
	unsigned int npages;
	int ret;

	kunit_info(test, "socket-owned clear event: %s\n", param->event);
	svc_tcp_clear_pages(fixture->svsk);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_datalen, 0U);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_tcplen, 0U);

	svcsock_rx_fill_record(fragment, BODY_LEN, false, RPC_CALL,
			       RPC_AUTH_UNIX);
	skb = svcsock_rx_build_page_skb(test, fragment, sizeof(fragment),
					&source_page, &source_refs);
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0,
				   skb->len);
	KUNIT_ASSERT_EQ(test, ret, (int)skb->len);
	KUNIT_EXPECT_FALSE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, active->refs_acquired, 1U);
	svcsock_rx_expect_unpublished(test, fixture->rqstp);
	ret = svc_tcp_save_pages(fixture->svsk, fixture->rqstp);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, fixture->svsk->sk_rx, active);
	KUNIT_EXPECT_EQ(test, page_ref_count(source_page), source_refs + 1);

	npages = DIV_ROUND_UP(fixture->svsk->sk_datalen, PAGE_SIZE);
	for (i = 0; i < npages; i++)
		kunit_remove_action(test, svcsock_rx_free_page,
				    fixture->svsk->sk_pages[i]);
	svc_tcp_clear_pages(fixture->svsk);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_datalen, 0U);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_tcplen, 0U);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_rx->mode,
			(u8)SVC_TCP_RX_EMPTY);
	KUNIT_EXPECT_EQ(test, page_ref_count(source_page), source_refs);
	for (i = 0; i < npages; i++)
		KUNIT_EXPECT_PTR_EQ(test, fixture->svsk->sk_pages[i], NULL);
	svcsock_rx_expect_unpublished(test, fixture->rqstp);
	kunit_kfree_skb(test, skb);
	KUNIT_EXPECT_EQ(test, page_ref_count(source_page), source_refs - 1);
}

struct svcsock_rx_fault_param {
	unsigned int fail_after;
};

static const struct svcsock_rx_fault_param svcsock_rx_fault_params[] = {
	{ .fail_after = 0 },
	{ .fail_after = 1 },
	{ .fail_after = 2 },
};

static void
svcsock_rx_fault_param_desc(const struct svcsock_rx_fault_param *param,
			    char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "after-%u", param->fail_after);
}

KUNIT_ARRAY_PARAM(svcsock_rx_fault, svcsock_rx_fault_params,
		  svcsock_rx_fault_param_desc);

static void svcsock_rx_append_fault_outer_close_test(struct kunit *test)
{
	const struct svcsock_rx_fault_param *param = test->param_value;
	enum { FRAG_LEN = 8, BODY_LEN = 7 * XDR_UNIT + 3 * FRAG_LEN };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct page *pages[3];
	unsigned int refs[3];
	struct svc_pool *pool;
	struct sk_buff *skb;
	const u8 *payload;
	unsigned int i;
	u32 acquired;
	u32 released;
	int ret;

	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_linear_skb(test, record,
					  sizeof(rpc_fraghdr) + 7 * XDR_UNIT);
	payload = record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT;
	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = svcsock_rx_add_frag(test, skb, i,
					       payload + i * FRAG_LEN, FRAG_LEN,
					       37 + i * 23, &refs[i]);
	}
	pool = svcsock_rx_prepare_outer_receive(test, fixture, skb);
	ret = kunit_add_action_or_reset(test, svcsock_rx_clear_fault, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	svc_tcp_rx_kunit_fault_set(SVC_TCP_RX_KUNIT_FAULT_APPEND,
				   param->fail_after);

	ret = svc_tcp_recvfrom(fixture->rqstp);
	KUNIT_EXPECT_EQ(test, ret, 0);
	svc_tcp_rx_kunit_fault_observed(&acquired, &released);
	KUNIT_EXPECT_EQ(test, acquired, (u32)param->fail_after);
	KUNIT_EXPECT_EQ(test, released, 0U);
	KUNIT_EXPECT_EQ(test, acquired - released,
			(u32)param->fail_after);
	svcsock_rx_expect_outer_close(test, fixture, pool);
	KUNIT_EXPECT_EQ(test, fixture->rqstp->rq_tcp_rx->mode,
			(u8)SVC_TCP_RX_EMPTY);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_datalen, 0U);
	svcsock_rx_expect_unpublished(test, fixture->rqstp);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), refs[i]);
	kunit_kfree_skb(test, skb);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), refs[i] - 1);
}

static void svcsock_rx_materialize_fault_outer_close_test(struct kunit *test)
{
	const struct svcsock_rx_fault_param *param = test->param_value;
	enum { FRAG_LEN = 8, BODY_LEN = 7 * XDR_UNIT + 4 * FRAG_LEN };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 3);
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct page *pages[4];
	unsigned int refs[4];
	struct svc_pool *pool;
	struct sk_buff *skb;
	const u8 *payload;
	unsigned int i;
	u32 acquired;
	u32 released;
	int ret;

	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_linear_skb(test, record,
					  sizeof(rpc_fraghdr) + 7 * XDR_UNIT);
	payload = record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT;
	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = svcsock_rx_add_frag(test, skb, i,
					       payload + i * FRAG_LEN, FRAG_LEN,
					       29 + i * 17, &refs[i]);
	}
	pool = svcsock_rx_prepare_outer_receive(test, fixture, skb);
	ret = kunit_add_action_or_reset(test, svcsock_rx_clear_fault, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	svc_tcp_rx_kunit_fault_set(SVC_TCP_RX_KUNIT_FAULT_MATERIALIZE,
				   param->fail_after);

	ret = svc_tcp_recvfrom(fixture->rqstp);
	KUNIT_EXPECT_EQ(test, ret, 0);
	svc_tcp_rx_kunit_fault_observed(&acquired, &released);
	KUNIT_EXPECT_EQ(test, acquired, 3U);
	KUNIT_EXPECT_EQ(test, released, (u32)param->fail_after);
	KUNIT_EXPECT_EQ(test, acquired - released,
			3U - (u32)param->fail_after);
	svcsock_rx_expect_outer_close(test, fixture, pool);
	KUNIT_EXPECT_EQ(test, fixture->rqstp->rq_tcp_rx->mode,
			(u8)SVC_TCP_RX_EMPTY);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_datalen, 0U);
	svcsock_rx_expect_unpublished(test, fixture->rqstp);
	for (i = 0; i < param->fail_after; i++) {
		void *arena = page_address(fixture->rqstp->rq_pages[0]);

		KUNIT_EXPECT_MEMEQ(test,
				   arena + 7 * XDR_UNIT + i * FRAG_LEN,
				   payload + i * FRAG_LEN, FRAG_LEN);
	}
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), refs[i]);
	kunit_kfree_skb(test, skb);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), refs[i] - 1);
}

static void svcsock_rx_mixed_geometry_lifetime_test(struct kunit *test)
{
	enum {
		HEAD_SUFFIX = 9,
		COMPOUND_LEN = 19,
		CHILD_LEN = 11,
		GRANDCHILD_LEN = 13,
		BODY_LEN = 7 * XDR_UNIT + HEAD_SUFFIX + COMPOUND_LEN +
			   CHILD_LEN + GRANDCHILD_LEN,
	};
	struct svcsock_rx_fixture *fixture =
		svcsock_rx_alloc_fixture(test, 16);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(record) };
	struct sk_buff *root, *clone, *child, *grandchild;
	struct page *root_page, *compound_page, *child_page, *grandchild_page;
	unsigned int root_refs, compound_refs, child_refs, grandchild_refs;
	unsigned int cursor = 7 * XDR_UNIT + HEAD_SUFFIX;
	int ret;

	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	root = svcsock_rx_build_page_skb(test, record,
					 sizeof(rpc_fraghdr) + cursor,
					 &root_page, &root_refs);
	compound_page = svcsock_rx_add_compound_frag(test, root, 0,
						     record + sizeof(rpc_fraghdr) + cursor,
						     COMPOUND_LEN, PAGE_SIZE - 7,
						     &compound_refs);
	cursor += COMPOUND_LEN;
	child = svcsock_rx_build_page_skb(test,
					  record + sizeof(rpc_fraghdr) + cursor,
					  CHILD_LEN, &child_page, &child_refs);
	cursor += CHILD_LEN;
	grandchild = svcsock_rx_build_page_skb(test,
					       record + sizeof(rpc_fraghdr) + cursor,
					       GRANDCHILD_LEN, &grandchild_page,
					       &grandchild_refs);
	cursor += GRANDCHILD_LEN;
	KUNIT_ASSERT_EQ(test, cursor, (unsigned int)BODY_LEN);
	svcsock_rx_attach_frag_list(test, child, grandchild);
	svcsock_rx_attach_frag_list(test, root, child);
	clone = skb_clone(root, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, clone);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							kunit_action_kfree_skb, clone), 0);

	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, clone, 0,
				   clone->len);
	KUNIT_ASSERT_EQ(test, ret, (int)clone->len);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_TRUE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_MIXED);
	KUNIT_EXPECT_EQ(test, state->body_bytes, (u32)BODY_LEN);
	KUNIT_EXPECT_EQ(test, state->fixed_bytes, 7U * XDR_UNIT);
	/*
	 * The locked-head copy starts a merge page and every following
	 * borrowable byte of this small record tops it up
	 * (svc_tcp_rx_copy_segment_merged()): the whole body publishes
	 * from one owned page -- all bytes accounted as copied, no
	 * loan references taken on the frag pages.
	 */
	KUNIT_EXPECT_EQ(test, state->copied_bytes,
			(u32)(BODY_LEN - 7 * XDR_UNIT));
	KUNIT_EXPECT_EQ(test, state->borrowed_bytes, 0U);
	KUNIT_EXPECT_EQ(test, state->refs_acquired, 1U);
	KUNIT_EXPECT_EQ(test, page_ref_count(root_page), root_refs);
	KUNIT_EXPECT_EQ(test, page_ref_count(compound_page), compound_refs);
	KUNIT_EXPECT_EQ(test, page_ref_count(child_page), child_refs);
	KUNIT_EXPECT_EQ(test, page_ref_count(grandchild_page), grandchild_refs);

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_publish(fixture->svsk, fixture->rqstp), 0);
	KUNIT_ASSERT_EQ(test, state->mode, (u8)SVC_TCP_RX_PUBLISHED);
	kunit_kfree_skb(test, clone);
	kunit_kfree_skb(test, root);
	KUNIT_EXPECT_EQ(test, page_ref_count(root_page), root_refs - 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(compound_page), compound_refs - 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(child_page), child_refs - 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(grandchild_page), grandchild_refs - 1);
	KUNIT_EXPECT_MEMEQ(test, page_address(fixture->rqstp->rq_pages[0]),
			   record + sizeof(rpc_fraghdr), 7 * XDR_UNIT);
	svcsock_rx_expect_bvec_bytes(test, state,
				     record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT,
				     BODY_LEN - 7 * XDR_UNIT);

	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
	KUNIT_EXPECT_EQ(test, page_ref_count(compound_page), compound_refs - 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(child_page), child_refs - 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(grandchild_page), grandchild_refs - 1);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
}

static void svcsock_rx_capacity_plus_one_test(struct kunit *test)
{
	enum { FRAG_LEN = 8, BODY_LEN = 7 * XDR_UNIT + 3 * FRAG_LEN };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 2);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(record) };
	struct page *pages[3];
	unsigned int refs[3];
	struct sk_buff *skb;
	const u8 *payload;
	unsigned int i;
	int ret;

	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_linear_skb(test, record,
					  sizeof(rpc_fraghdr) + 7 * XDR_UNIT);
	payload = record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT;
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		pages[i] = svcsock_rx_add_frag(test, skb, i,
					       payload + i * FRAG_LEN, FRAG_LEN,
					       31 + i * 19, &refs[i]);

	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0,
				   skb->len);
	KUNIT_ASSERT_EQ(test, ret, (int)skb->len);
	KUNIT_EXPECT_TRUE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_COPY);
	KUNIT_EXPECT_EQ(test, state->reason, (u8)SVC_TCP_RX_REASON_CAPACITY);
	KUNIT_EXPECT_EQ(test, state->refs_acquired, 2U);
	KUNIT_EXPECT_EQ(test, state->refs_released, 2U);
	KUNIT_EXPECT_EQ(test, state->materialized_bytes, 2U * FRAG_LEN);
	KUNIT_EXPECT_EQ(test, state->count, 0U);
	KUNIT_EXPECT_MEMEQ(test, page_address(fixture->rqstp->rq_pages[0]),
			   record + sizeof(rpc_fraghdr), BODY_LEN);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_publish(fixture->svsk, fixture->rqstp), 0);
	kunit_kfree_skb(test, skb);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), refs[i] - 1);
}

static void svcsock_rx_partial_save_restore_test(struct kunit *test)
{
	enum { BODY_LEN = 7 * XDR_UNIT + 12 };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *request_state = fixture->rqstp->rq_tcp_rx;
	struct svc_tcp_rx_state *socket_state = fixture->svsk->sk_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(record) };
	struct page *head_page;
	unsigned int head_refs;
	struct sk_buff *skb;
	int ret;

	svcsock_rx_fill_record(record, BODY_LEN, false, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_page_skb(test, record, sizeof(record),
					&head_page, &head_refs);
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0,
				   skb->len);
	KUNIT_ASSERT_EQ(test, ret, (int)skb->len);
	KUNIT_EXPECT_FALSE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, request_state->refs_acquired, 1U);
	ret = svc_tcp_save_pages(fixture->svsk, fixture->rqstp);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, fixture->svsk->sk_rx, request_state);
	KUNIT_EXPECT_PTR_EQ(test, fixture->rqstp->rq_tcp_rx, socket_state);
	ret = svc_tcp_restore_pages(fixture->svsk, fixture->rqstp);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, fixture->rqstp->rq_tcp_rx, request_state);
	KUNIT_EXPECT_PTR_EQ(test, fixture->svsk->sk_rx, socket_state);
	ret = svc_tcp_rx_abort(fixture->svsk, fixture->rqstp,
			       request_state, SVC_TCP_RX_ERROR,
			       SVC_TCP_RX_REASON_COPY_FAULT);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, request_state->mode, (u8)SVC_TCP_RX_EMPTY);
	kunit_kfree_skb(test, skb);
	KUNIT_EXPECT_EQ(test, page_ref_count(head_page), head_refs - 1);
}

static void svcsock_rx_exclusion_table_test(struct kunit *test)
{
	static const struct {
		const char *name;
		u32 direction;
		u32 auth;
		u8 mode;
		u8 reason;
		bool tls;
		bool managed;
		bool unreadable;
		bool disabled;
	} cases[] = {
		{ "auth-sys", RPC_CALL, RPC_AUTH_UNIX, SVC_TCP_RX_MIXED,
		  SVC_TCP_RX_REASON_NONE },
		{ "gss", RPC_CALL, RPC_AUTH_GSS, SVC_TCP_RX_COPY,
		  SVC_TCP_RX_REASON_AUTH },
		{ "reply", RPC_REPLY, RPC_AUTH_UNIX, SVC_TCP_RX_COPY,
		  SVC_TCP_RX_REASON_DIRECTION },
		{ "ktls", RPC_CALL, RPC_AUTH_UNIX, SVC_TCP_RX_COPY,
		  SVC_TCP_RX_REASON_TLS, true },
		{ "managed", RPC_CALL, RPC_AUTH_UNIX, SVC_TCP_RX_COPY,
		  SVC_TCP_RX_REASON_MANAGED_FRAGS, false, true },
		{ "unreadable", RPC_CALL, RPC_AUTH_UNIX, SVC_TCP_RX_MIXED,
		  SVC_TCP_RX_REASON_UNREADABLE, false, false, true },
		{ "disabled", RPC_CALL, RPC_AUTH_UNIX, SVC_TCP_RX_COPY,
		  SVC_TCP_RX_REASON_DISABLED, false, false, false, true },
	};
	enum { BODY_LEN = 7 * XDR_UNIT + 12 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct svcsock_rx_fixture *fixture =
			svcsock_rx_alloc_fixture(test, 4);
		struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
		u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
		struct svcsock_rx_actor_state actor = {};
		read_descriptor_t desc = { .count = sizeof(record) };
		struct page *page;
		unsigned int refs;
		unsigned int skb_len;
		struct sk_buff *skb;
		const u8 *payload;
		int ret;

		kunit_info(test, "exclusion case: %s\n", cases[i].name);
		svcsock_rx_fill_record(record, BODY_LEN, true,
				       cases[i].direction, cases[i].auth);
		payload = record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT;
		if (cases[i].unreadable) {
			skb = svcsock_rx_build_linear_skb(test, record,
							  sizeof(rpc_fraghdr) +
						      7 * XDR_UNIT);
			page = svcsock_rx_add_frag(test, skb, 0,
						   payload,
				BODY_LEN - 7 * XDR_UNIT, 61, &refs);
			skb->unreadable = true;
		} else {
			skb = svcsock_rx_build_page_skb(test, record,
							sizeof(record), &page, &refs);
		}
		if (cases[i].tls)
			set_bit(XPT_TLS_SESSION, &fixture->svsk->sk_xprt.xpt_flags);
		if (cases[i].managed)
			skb_shinfo(skb)->flags |= SKBFL_MANAGED_FRAG_REFS;
		if (cases[i].disabled)
			svc_tcp_rx_loan_pages = false;

		skb_len = skb->len;
		ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0, skb_len);
		svc_tcp_rx_loan_pages = true;
		if (cases[i].unreadable) {
			KUNIT_EXPECT_EQ(test, ret, (int)sizeof(rpc_fraghdr));
			KUNIT_EXPECT_EQ(test, desc.error, -EFAULT);
			KUNIT_EXPECT_FALSE(test, actor.complete);
			KUNIT_EXPECT_EQ(test, state->refs_acquired, 0U);
			KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
			KUNIT_EXPECT_EQ(test, state->reason, cases[i].reason);
			ret = svc_tcp_rx_abort(fixture->svsk, fixture->rqstp,
					       state, SVC_TCP_RX_ERROR,
					       SVC_TCP_RX_REASON_COPY_FAULT);
			KUNIT_ASSERT_EQ(test, ret, 0);
			kunit_kfree_skb(test, skb);
			KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
			continue;
		}

		KUNIT_ASSERT_EQ(test, ret, (int)skb->len);
		KUNIT_EXPECT_EQ(test, desc.error, 0);
		KUNIT_EXPECT_TRUE(test, actor.complete);
		KUNIT_EXPECT_EQ(test, state->mode, cases[i].mode);
		KUNIT_EXPECT_EQ(test, state->reason, cases[i].reason);
		if (cases[i].mode == SVC_TCP_RX_MIXED) {
			KUNIT_EXPECT_EQ(test, state->refs_acquired, 1U);
			ret = svc_tcp_rx_publish(fixture->svsk, fixture->rqstp);
			KUNIT_ASSERT_EQ(test, ret, 0);
			kunit_kfree_skb(test, skb);
			svcsock_rx_expect_bvec_bytes(test, state,
						     payload,
				BODY_LEN - 7 * XDR_UNIT);
			svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
		} else {
			KUNIT_EXPECT_EQ(test, state->refs_acquired, 0U);
			KUNIT_EXPECT_MEMEQ(test,
					   page_address(fixture->rqstp->rq_pages[0]),
				record + sizeof(rpc_fraghdr), BODY_LEN);
			ret = svc_tcp_rx_publish(fixture->svsk, fixture->rqstp);
			KUNIT_ASSERT_EQ(test, ret, 0);
			kunit_kfree_skb(test, skb);
		}
		KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
	}
}

static void svcsock_rx_free_state(void *data)
{
	struct svc_tcp_rx_state *state = data;

	if (state->mode != SVC_TCP_RX_EMPTY && !state->terminal &&
	    svc_tcp_rx_release_refs(state))
		return;
	if (state->terminal)
		svc_tcp_rx_reset(state);
	svc_tcp_rx_state_free(state);
}

static struct svc_tcp_rx_state *
svcsock_rx_alloc_state(struct kunit *test, unsigned int capacity)
{
	struct svc_tcp_rx_state *state;

	state = svc_tcp_rx_state_alloc(capacity, GFP_KERNEL, NUMA_NO_NODE);
	if (!state)
		return NULL;
	if (kunit_add_action_or_reset(test, svcsock_rx_free_state, state))
		return NULL;
	state->mode = SVC_TCP_RX_MIXED;
	state->fixed_bytes = 28;
	state->body_bytes = 28;
	return state;
}

static void svcsock_rx_write(struct page *page, unsigned int offset,
			     const void *source, unsigned int len)
{
	void *address = kmap_local_page(page);

	memcpy(address + offset, source, len);
	kunmap_local(address);
}

static void svcsock_rx_materialize_test(struct kunit *test)
{
	static const u8 borrowed[] = {
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
		0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
	};
	static const u8 copied[] = { 0xc0, 0xc1, 0xc2, 0xc3 };
	struct svc_tcp_rx_state *state = svcsock_rx_alloc_state(test, 4);
	struct page *source = svcsock_rx_alloc_page(test);
	struct page *arena = svcsock_rx_alloc_page(test);
	struct page *pages[] = { arena };
	unsigned int source_refs;
	void *address;

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, arena);
	source_refs = page_ref_count(source);

	svcsock_rx_write(source, 7, borrowed, sizeof(borrowed));
	svcsock_rx_write(arena, 40, copied, sizeof(copied));

	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, source, 7, 8, false), 0);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, source, 15, 4, false), 0);
	KUNIT_EXPECT_EQ(test, state->count, 1U);
	KUNIT_EXPECT_EQ(test, state->refs_acquired, 1U);
	KUNIT_EXPECT_EQ(test, page_ref_count(source), source_refs + 1);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_append_copied(state, arena, 40,
						       sizeof(copied)), 0);

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_materialize(state, pages), 0);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_COPY);
	KUNIT_EXPECT_EQ(test, state->count, 0U);
	KUNIT_EXPECT_EQ(test, state->borrowed_bytes, 0U);
	KUNIT_EXPECT_EQ(test, state->copied_bytes,
			(u32)(sizeof(borrowed) + sizeof(copied)));
	KUNIT_EXPECT_EQ(test, state->materialized_bytes,
			(u32)sizeof(borrowed));
	KUNIT_EXPECT_EQ(test, state->refs_acquired, state->refs_released);
	KUNIT_EXPECT_EQ(test, page_ref_count(source), source_refs);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->borrowed, state->capacity));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->page_pool, state->capacity));

	address = kmap_local_page(arena);
	KUNIT_EXPECT_MEMEQ(test, address + 28, borrowed, sizeof(borrowed));
	KUNIT_EXPECT_MEMEQ(test, address + 40, copied, sizeof(copied));
	kunmap_local(address);
}

static void svcsock_rx_capacity_release_test(struct kunit *test)
{
	struct svc_tcp_rx_state *state = svcsock_rx_alloc_state(test, 1);
	struct page *first = svcsock_rx_alloc_page(test);
	struct page *second = svcsock_rx_alloc_page(test);
	unsigned int first_refs;
	unsigned int second_refs;

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, first);
	KUNIT_ASSERT_NOT_NULL(test, second);
	first_refs = page_ref_count(first);
	second_refs = page_ref_count(second);

	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, first, 1, 3, true), 0);
	KUNIT_EXPECT_EQ(test,
			svc_tcp_rx_append_borrowed(state, second, 5, 7, false),
			-ENOSPC);
	KUNIT_EXPECT_EQ(test, page_ref_count(first), first_refs + 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(second), second_refs);
	KUNIT_EXPECT_TRUE(test, test_bit(0, state->page_pool));

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(first), first_refs);
	KUNIT_EXPECT_EQ(test, state->refs_acquired, state->refs_released);
}

static void svcsock_rx_duplicate_cleanup_test(struct kunit *test)
{
	struct svc_tcp_rx_state *release_state = svcsock_rx_alloc_state(test, 1);
	struct svc_tcp_rx_state *abort_state = svcsock_rx_alloc_state(test, 1);
	struct svc_sock *svsk;
	struct page *page = svcsock_rx_alloc_page(test);

	KUNIT_ASSERT_NOT_NULL(test, release_state);
	KUNIT_ASSERT_NOT_NULL(test, abort_state);
	KUNIT_ASSERT_NOT_NULL(test, page);
	svsk = kunit_kzalloc(test, sizeof(*svsk), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, svsk);

	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(release_state, page, 1, 3,
						   false), 0);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(release_state), 0);
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_EQ(test, svc_tcp_rx_release_refs(release_state),
				-EINVAL);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}

	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_abort(svsk, NULL, abort_state, SVC_TCP_RX_ABORT,
					 SVC_TCP_RX_REASON_NONE), 0);
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_EQ(test,
				svc_tcp_rx_abort(svsk, NULL, abort_state,
						 SVC_TCP_RX_ABORT,
						 SVC_TCP_RX_REASON_NONE),
				-EINVAL);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
}

static void svcsock_rx_concurrent_release_test(struct kunit *test)
{
	struct svc_tcp_rx_state *request_state = svcsock_rx_alloc_state(test, 1);
	struct svc_tcp_rx_state *socket_state = svcsock_rx_alloc_state(test, 1);
	struct svc_tcp_rx_state socket_snapshot;
	struct svc_rqst *request;
	struct svc_sock *svsk;
	struct page *request_page = svcsock_rx_alloc_page(test);
	struct page *socket_page = svcsock_rx_alloc_page(test);
	unsigned int request_refs;
	unsigned int socket_refs;

	KUNIT_ASSERT_NOT_NULL(test, request_state);
	KUNIT_ASSERT_NOT_NULL(test, socket_state);
	KUNIT_ASSERT_NOT_NULL(test, request_page);
	KUNIT_ASSERT_NOT_NULL(test, socket_page);
	request = kunit_kzalloc(test, sizeof(*request), GFP_KERNEL);
	svsk = kunit_kzalloc(test, sizeof(*svsk), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, request);
	KUNIT_ASSERT_NOT_NULL(test, svsk);
	request_refs = page_ref_count(request_page);
	socket_refs = page_ref_count(socket_page);

	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(request_state, request_page,
						   1, 3, false), 0);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(socket_state, socket_page,
						   5, 7, false), 0);
	request_state->mode = SVC_TCP_RX_PUBLISHED;
	request_state->rqstp = request;
	request->rq_tcp_rx = request_state;
	request->rq_xprt_ctxt = request_state;
	request->rq_arg.page_mode = XDRBUF_PAGE_BVECS;
	request->rq_arg.bvec = request_state->bvec;
	request->rq_arg.bvec_count = request_state->count;
	svsk->sk_rx = socket_state;
	socket_snapshot = *socket_state;

	svc_tcp_release_ctxt(&svsk->sk_xprt, request_state);
	KUNIT_EXPECT_EQ(test, page_ref_count(request_page), request_refs);
	KUNIT_EXPECT_EQ(test, request_state->mode, (u8)SVC_TCP_RX_EMPTY);
	KUNIT_EXPECT_EQ(test, request->rq_arg.page_mode,
			(u8)XDRBUF_PAGE_ARRAY);
	KUNIT_EXPECT_PTR_EQ(test, request->rq_arg.bvec, NULL);
	KUNIT_EXPECT_PTR_EQ(test, svsk->sk_rx, socket_state);
	KUNIT_EXPECT_MEMEQ(test, socket_state, &socket_snapshot,
			   sizeof(socket_snapshot));
	KUNIT_EXPECT_EQ(test, page_ref_count(socket_page), socket_refs + 1);

	kunit_warning_suppress(test) {
		svc_tcp_release_ctxt(&svsk->sk_xprt, request_state);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	KUNIT_EXPECT_EQ(test, page_ref_count(request_page), request_refs);
	KUNIT_EXPECT_MEMEQ(test, socket_state, &socket_snapshot,
			   sizeof(socket_snapshot));
	KUNIT_EXPECT_EQ(test, page_ref_count(socket_page), socket_refs + 1);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_abort(svsk, NULL, socket_state, SVC_TCP_RX_ABORT,
					 SVC_TCP_RX_REASON_NONE), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(socket_page), socket_refs);
	KUNIT_EXPECT_EQ(test, socket_state->mode, (u8)SVC_TCP_RX_EMPTY);
}

static void svcsock_rx_exchange_test(struct kunit *test)
{
	struct svc_tcp_rx_state *active = svcsock_rx_alloc_state(test, 2);
	struct svc_tcp_rx_state *empty;
	struct svc_tcp_rx_state *original_active = active;
	struct svc_tcp_rx_state *original_empty;

	KUNIT_ASSERT_NOT_NULL(test, active);

	empty = svc_tcp_rx_state_alloc(2, GFP_KERNEL, NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, empty);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, svcsock_rx_free_state, empty),
			0);
	original_empty = empty;
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_exchange(&active, &empty), 0);
	KUNIT_EXPECT_PTR_EQ(test, active, original_empty);
	KUNIT_EXPECT_PTR_EQ(test, empty, original_active);
}

#define SVCSOCK_RX_TEST_MAX_CAPACITY	1028

static void svcsock_rx_max_capacity_reset_test(struct kunit *test)
{
	struct svc_tcp_rx_state *state =
		svcsock_rx_alloc_state(test, SVCSOCK_RX_TEST_MAX_CAPACITY);
	struct page *page = svcsock_rx_alloc_page(test);
	unsigned int refs;

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, page);
	refs = page_ref_count(page);
	state->mode = SVC_TCP_RX_COPY;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->borrowed, state->capacity));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->page_pool, state->capacity));
	svc_tcp_rx_reset(state);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->borrowed, state->capacity));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->page_pool, state->capacity));

	state->mode = SVC_TCP_RX_MIXED;
	state->fixed_bytes = 28;
	state->body_bytes = 28;
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, page, 1, 3, true), 0);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->borrowed, state->capacity));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->page_pool, state->capacity));
	svc_tcp_rx_reset(state);

	state->mode = SVC_TCP_RX_MIXED;
	state->fixed_bytes = 28;
	state->body_bytes = 28;
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, page, 5, 7, false), 0);
	KUNIT_EXPECT_TRUE(test, test_bit(0, state->borrowed));
	KUNIT_EXPECT_FALSE(test, test_bit(0, state->page_pool));
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
}

static void svcsock_rx_wrong_partial_owner_test(struct kunit *test)
{
	struct svc_tcp_rx_state *first = svcsock_rx_alloc_state(test, 2);
	struct svc_tcp_rx_state *second = svcsock_rx_alloc_state(test, 2);
	struct svc_tcp_rx_state *original_first = first;
	struct svc_tcp_rx_state *original_second = second;

	KUNIT_ASSERT_NOT_NULL(test, first);
	KUNIT_ASSERT_NOT_NULL(test, second);
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_EQ(test, svc_tcp_rx_exchange(&first, &second),
				-EINVAL);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	KUNIT_EXPECT_PTR_EQ(test, first, original_first);
	KUNIT_EXPECT_PTR_EQ(test, second, original_second);

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(first), 0);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(second), 0);
	svc_tcp_rx_reset(first);
	svc_tcp_rx_reset(second);
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_EQ(test, svc_tcp_rx_exchange(&first, &second),
				-EINVAL);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
}

static struct kunit_case svcsock_rx_test_cases[] = {
	KUNIT_CASE_PARAM(svcsock_rx_actor_split_test,
			 svcsock_rx_split_gen_params),
	KUNIT_CASE(svcsock_rx_multifragment_fresh_request_test),
	KUNIT_CASE(svcsock_rx_two_records_one_skb_test),
	KUNIT_CASE_PARAM(svcsock_rx_socket_owned_clear_test,
			 svcsock_rx_clear_gen_params),
	KUNIT_CASE_PARAM(svcsock_rx_append_fault_outer_close_test,
			 svcsock_rx_fault_gen_params),
	KUNIT_CASE_PARAM(svcsock_rx_materialize_fault_outer_close_test,
			 svcsock_rx_fault_gen_params),
	KUNIT_CASE(svcsock_rx_mixed_geometry_lifetime_test),
	KUNIT_CASE(svcsock_rx_capacity_plus_one_test),
	KUNIT_CASE(svcsock_rx_partial_save_restore_test),
	KUNIT_CASE(svcsock_rx_exclusion_table_test),
	KUNIT_CASE(svcsock_rx_materialize_test),
	KUNIT_CASE(svcsock_rx_capacity_release_test),
	KUNIT_CASE(svcsock_rx_duplicate_cleanup_test),
	KUNIT_CASE(svcsock_rx_concurrent_release_test),
	KUNIT_CASE(svcsock_rx_exchange_test),
	KUNIT_CASE(svcsock_rx_max_capacity_reset_test),
	KUNIT_CASE(svcsock_rx_wrong_partial_owner_test),
	{}
};

/*
 * The suite exercises the loan paths directly, so it must run with the
 * runtime loan switch on regardless of how the host is configured;
 * every test restores the host's setting on exit.
 */
static bool svcsock_rx_saved_loan_pages;

static int svcsock_rx_test_init(struct kunit *test)
{
	svcsock_rx_saved_loan_pages = svc_tcp_rx_loan_pages;
	svc_tcp_rx_loan_pages = true;
	return 0;
}

static void svcsock_rx_test_exit(struct kunit *test)
{
	svc_tcp_rx_loan_pages = svcsock_rx_saved_loan_pages;
}

static struct kunit_suite svcsock_rx_test_suite = {
	.name = "sunrpc-svcsock-rx",
	.init = svcsock_rx_test_init,
	.exit = svcsock_rx_test_exit,
	.test_cases = svcsock_rx_test_cases,
};

kunit_test_suite(svcsock_rx_test_suite);

MODULE_DESCRIPTION("KUnit tests for SUNRPC TCP receive ownership");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
