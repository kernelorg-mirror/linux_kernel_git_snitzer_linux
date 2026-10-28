// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/device.h>
#include <kunit/test.h>
#include <kunit/skbuff.h>

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <net/sock.h>
#include <linux/percpu_counter.h>
#include <linux/skbuff.h>
#include <linux/sunrpc/msg_prot.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/unaligned.h>

#include <net/net_namespace.h>
#include <net/page_pool/cfg.h>
#include <net/page_pool/helpers.h>
#include <net/rx_loan.h>
#include <net/tcp.h>
#include <net/xdp.h>

#define SVCSOCK_RX_TEST_PROGRAM	100003
#define SVCSOCK_RX_TEST_VERSION	3
#define SVCSOCK_RX_TEST_PROCEDURE	7
/* The default budget each test starts with, so that its records loan. */
#define SVCSOCK_RX_LOAN_BUDGET	1024

/*
 * A WARN_ON_ONCE() site is spent at its first hit, and on an architecture
 * without a bug table (no __WARN_FLAGS, e.g. UML) DO_ONCE_LITE_IF() spends it
 * even when the warning is suppressed, so an earlier test that reached the
 * same site leaves nothing to count.  With a bug table, report_bug() checks
 * suppression before the once flag and the site stays armed.  Count exactly
 * one warning where the site cannot already be spent, and at most one
 * elsewhere; callers assert the result and state of the warning path either
 * way.
 */
#ifdef __WARN_FLAGS
#define SVCSOCK_EXPECT_ONCE_WARNING(test) \
	KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1)
#else
#define SVCSOCK_EXPECT_ONCE_WARNING(test) \
	KUNIT_EXPECT_LE(test, KUNIT_SUPPRESSED_WARNING_COUNT(), 1)
#endif

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

struct svcsock_rx_pp {
	struct page_pool *pool;
	struct page *page;
};

static void svcsock_rx_pp_release(void *data)
{
	struct svcsock_rx_pp *pp = data;

	if (pp->page)
		page_pool_put_full_page(pp->pool, pp->page, false);
	page_pool_destroy(pp->pool);
}

/* One page from a private page pool, returned and destroyed with the test. */
static struct page *svcsock_rx_alloc_pp_page(struct kunit *test,
					     struct svcsock_rx_pp **out)
{
	struct page_pool_params params = {
		.order = 0,
		.pool_size = 16,
		.nid = NUMA_NO_NODE,
	};
	struct svcsock_rx_pp *pp;

	if (!IS_ENABLED(CONFIG_PAGE_POOL))
		kunit_skip(test, "CONFIG_PAGE_POOL is not set");
	pp = kunit_kzalloc(test, sizeof(*pp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pp);
	pp->pool = page_pool_create(&params);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pp->pool));
	pp->page = page_pool_alloc_pages(pp->pool, GFP_KERNEL);
	if (!pp->page)
		page_pool_destroy(pp->pool);
	KUNIT_ASSERT_NOT_NULL(test, pp->page);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, svcsock_rx_pp_release, pp),
			0);
	if (out)
		*out = pp;
	return pp->page;
}

static long svcsock_rx_pp_refs(struct page *page)
{
	return atomic_long_read(netmem_get_pp_ref_count_ref(page_to_netmem(page)));
}

static struct svc_rqst *
svcsock_rx_alloc_request_sized(struct kunit *test,
			       struct svcsock_rx_fixture *fixture,
			       unsigned int entries, unsigned int capacity)
{
	struct svc_rqst *rqstp;
	unsigned int i;

	rqstp = kunit_kzalloc(test, sizeof(*rqstp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rqstp);
	rqstp->rq_pages = kunit_kcalloc(test, capacity,
					sizeof(*rqstp->rq_pages), GFP_KERNEL);
	rqstp->rq_bvec = kunit_kcalloc(test, entries,
				       sizeof(*rqstp->rq_bvec), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_pages);
	KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_bvec);
	for (i = 0; i < capacity; i++) {
		rqstp->rq_pages[i] = svcsock_rx_alloc_page(test);
		KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_pages[i]);
	}

	rqstp->rq_tcp_rx = svc_tcp_rx_state_alloc_entries(entries, capacity,
							  GFP_KERNEL,
							  NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_tcp_rx);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							svcsock_rx_free_state,
						      rqstp->rq_tcp_rx), 0);
	rqstp->rq_xprt = &fixture->svsk->sk_xprt;
	rqstp->rq_server = fixture->svsk->sk_xprt.xpt_server;
	rqstp->rq_maxpages = capacity;
	rqstp->rq_maxbvecs = entries;
	rqstp->rq_arg.head[0].iov_base = page_address(rqstp->rq_pages[0]);
	rqstp->rq_arg.head[0].iov_len = PAGE_SIZE;
	rqstp->rq_arg.pages = rqstp->rq_pages;
	return rqstp;
}

static struct svc_rqst *
svcsock_rx_alloc_request(struct kunit *test,
			 struct svcsock_rx_fixture *fixture,
			 unsigned int capacity)
{
	return svcsock_rx_alloc_request_sized(test, fixture,
					      svc_pages_to_bvecs(capacity),
					      capacity);
}

/* Defined with the credit tests below. */
static void svcsock_rx_credit_forget_action(void *svsk);

static struct svcsock_rx_fixture *
svcsock_rx_alloc_fixture_sized(struct kunit *test, unsigned int entries,
			       unsigned int capacity)
{
	struct svcsock_rx_fixture *fixture;
	struct svc_serv *serv;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->svsk = kunit_kzalloc(test,
				      struct_size(fixture->svsk, sk_pages, capacity),
			GFP_KERNEL);
	if (fixture->svsk)
		svc_tcp_rx_wait_init(fixture->svsk);
	serv = kunit_kzalloc(test, sizeof(*serv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fixture->svsk);
	KUNIT_ASSERT_NOT_NULL(test, serv);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  svcsock_rx_credit_forget_action,
						  fixture->svsk), 0);

	fixture->svsk->sk_rx = svc_tcp_rx_state_alloc_entries(entries, capacity,
							      GFP_KERNEL,
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
	/* Classification looks up the budget of the transport's namespace. */
	fixture->svsk->sk_xprt.xpt_net = &init_net;
	fixture->rqstp = svcsock_rx_alloc_request_sized(test, fixture, entries,
							capacity);
	return fixture;
}

static struct svcsock_rx_fixture *
svcsock_rx_alloc_fixture(struct kunit *test, unsigned int capacity)
{
	return svcsock_rx_alloc_fixture_sized(test, svc_pages_to_bvecs(capacity),
					      capacity);
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

	for (index = 0; index < state->lt.count; index++) {
		const struct bio_vec *bvec = &state->lt.bvec[index];
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

/*
 * svc_xprt_received() asks a transport left with XPT_DATA whether it has
 * write space.  The outer-receive tests expect XPT_CLOSE, which is queued
 * without asking; answering "no" here makes a record that was delivered
 * instead fail those expectations rather than call through a NULL
 * xpt_ops.
 */
static int svcsock_rx_test_has_wspace(struct svc_xprt *xprt)
{
	return 0;
}

static const struct svc_xprt_ops svcsock_rx_test_xprt_ops = {
	.xpo_has_wspace	= svcsock_rx_test_has_wspace,
};

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

	fixture->svsk->sk_xprt.xpt_ops = &svcsock_rx_test_xprt_ops;
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
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 0U);
	svcsock_rx_expect_unpublished(test, fixture->rqstp);
	if (param->kind == SVCSOCK_RX_SPLIT_CLASSIFIER) {
		KUNIT_EXPECT_EQ(test, state->lt.body_bytes, (u32)param->split);
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
	KUNIT_EXPECT_EQ(test, state->lt.body_bytes, (u32)BODY_LEN);
	KUNIT_EXPECT_EQ(test, state->fixed_bytes, 7U * XDR_UNIT);
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 1U);
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

/* A whole record at or below svc_tcp_rx_copy_below is copied, not loaned. */
static void svcsock_rx_small_record_copied_test(struct kunit *test)
{
	enum { BODY_LEN = 7 * XDR_UNIT + 19 };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(record) };
	struct page *page;
	unsigned int refs;
	struct sk_buff *skb;
	int ret;

	svc_tcp_rx_copy_below = BODY_LEN;
	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_page_skb(test, record, sizeof(record), &page,
					&refs);
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0,
				   sizeof(record));
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(record));
	KUNIT_EXPECT_TRUE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_COPY);
	KUNIT_EXPECT_EQ(test, state->reason, (u8)SVC_TCP_RX_REASON_SMALL);
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 0U);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	kunit_kfree_skb(test, skb);
}

/*
 * A small record is copied wherever its marker or call prefix splits across
 * skbs: loaned, it would publish argument pages, and an NFSv4 READ with
 * argument pages is served unconfined.
 */
static void svcsock_rx_small_record_split_test(struct kunit *test)
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

	svc_tcp_rx_copy_below = BODY_LEN;
	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, param->auth);
	skb = svcsock_rx_build_page_skb(test, record, sizeof(record), &page,
					&refs);
	first_len = param->kind == SVCSOCK_RX_SPLIT_CLASSIFIER ?
			sizeof(rpc_fraghdr) + param->split : param->split;

	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0,
				   first_len);
	KUNIT_ASSERT_EQ(test, ret, (int)first_len);
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb,
				   first_len, sizeof(record) - first_len);
	KUNIT_ASSERT_EQ(test, ret, (int)(sizeof(record) - first_len));
	KUNIT_EXPECT_TRUE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_COPY);
	KUNIT_EXPECT_EQ(test, state->reason, (u8)SVC_TCP_RX_REASON_SMALL);
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 0U);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);

	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_publish(fixture->svsk, fixture->rqstp), 0);
	KUNIT_EXPECT_EQ(test, fixture->rqstp->rq_arg.page_len, 0U);
	KUNIT_EXPECT_EQ(test, fixture->rqstp->rq_arg.len, (u32)BODY_LEN);
	KUNIT_EXPECT_MEMEQ(test, page_address(fixture->rqstp->rq_pages[0]),
			   record + sizeof(rpc_fraghdr), BODY_LEN);
	kunit_kfree_skb(test, skb);
}

/*
 * A caller that has consumed the payload returns the loan early; the
 * end-of-RPC release then finds nothing borrowed and puts nothing twice.
 */
static void svcsock_rx_release_payload_test(struct kunit *test)
{
	enum { BODY_LEN = 7 * XDR_UNIT + 19 };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(record) };
	struct page *page;
	unsigned int refs;
	struct sk_buff *skb;
	int ret;

	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_page_skb(test, record, sizeof(record), &page,
					&refs);
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0,
				   sizeof(record));
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(record));
	KUNIT_ASSERT_TRUE(test, actor.complete);
	KUNIT_ASSERT_EQ(test, state->lt.refs_acquired, 1U);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_publish(fixture->svsk, fixture->rqstp), 0);
	kunit_kfree_skb(test, skb);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);

	svc_tcp_rx_release_payload(fixture->svsk, fixture->rqstp);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
	KUNIT_EXPECT_EQ(test, state->lt.refs_released, 1U);
	KUNIT_EXPECT_TRUE(test, state->terminal);
	KUNIT_EXPECT_EQ(test, fixture->rqstp->rq_arg.page_len, 0U);
	/* A second early release is a no-op. */
	svc_tcp_rx_release_payload(fixture->svsk, fixture->rqstp);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);

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
	KUNIT_EXPECT_EQ(test, active->lt.body_bytes, (u32)FIRST_FRAGMENT_LEN);
	KUNIT_EXPECT_EQ(test, active->lt.refs_acquired, 1U);
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
	KUNIT_EXPECT_EQ(test, active->lt.body_bytes, (u32)FIRST_FRAGMENT_LEN);
	svcsock_rx_expect_unpublished(test, fresh_rqstp);

	ret = svcsock_rx_run_actor(fresh_rqstp, &second_actor, &second_desc,
				   second_skb, 0, second_skb->len);
	KUNIT_ASSERT_EQ(test, ret, (int)second_skb->len);
	KUNIT_EXPECT_TRUE(test, second_actor.complete);
	KUNIT_EXPECT_EQ(test, active->lt.body_bytes, (u32)BODY_LEN);
	KUNIT_EXPECT_EQ(test, active->lt.refs_acquired, 2U);
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
	KUNIT_EXPECT_EQ(test, first_rqstp->rq_tcp_rx->lt.refs_acquired, 1U);
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
	KUNIT_EXPECT_EQ(test, second_rqstp->rq_tcp_rx->lt.refs_acquired, 1U);
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

static void svcsock_rx_prefix_only_then_loan_test(struct kunit *test)
{
	enum {
		FIRST_BODY_LEN = 7 * XDR_UNIT,
		SECOND_BODY_LEN = FIRST_BODY_LEN + 11,
		FIRST_RECORD_LEN = sizeof(rpc_fraghdr) + FIRST_BODY_LEN,
		SECOND_RECORD_LEN = sizeof(rpc_fraghdr) + SECOND_BODY_LEN,
	};
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_rqst *rqstp = fixture->rqstp;
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = FIRST_RECORD_LEN + SECOND_RECORD_LEN };
	u8 stream[FIRST_RECORD_LEN + SECOND_RECORD_LEN];
	struct page *page;
	unsigned int refs;
	struct sk_buff *skb;
	int consumed;

	svcsock_rx_fill_record(stream, FIRST_BODY_LEN, true, RPC_CALL,
			       RPC_AUTH_NULL);
	svcsock_rx_fill_record(stream + FIRST_RECORD_LEN, SECOND_BODY_LEN,
			       true, RPC_CALL, RPC_AUTH_NULL);
	skb = svcsock_rx_build_page_skb(test, stream, sizeof(stream), &page,
					&refs);
	consumed = svcsock_rx_run_actor(rqstp, &actor, &desc, skb, 0,
					 skb->len);
	KUNIT_ASSERT_EQ(test, consumed, (int)FIRST_RECORD_LEN);
	KUNIT_ASSERT_TRUE(test, actor.complete);
	KUNIT_ASSERT_EQ(test, state->mode, (u8)SVC_TCP_RX_MIXED);
	KUNIT_EXPECT_EQ(test, state->lt.count, 0U);
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 0U);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_publish(fixture->svsk, rqstp), 0);
	KUNIT_EXPECT_EQ(test, rqstp->rq_arg.page_mode, XDRBUF_PAGE_ARRAY);
	KUNIT_EXPECT_EQ(test, rqstp->rq_arg.page_len, 0U);
	KUNIT_EXPECT_EQ(test, rqstp->rq_arg.head[0].iov_len,
			FIRST_BODY_LEN);
	KUNIT_EXPECT_PTR_EQ(test, rqstp->rq_xprt_ctxt, NULL);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);

	/* A deferral has no context to transfer; this request can receive again. */
	svc_tcp_recv_record_done(fixture->svsk);
	actor.complete = false;
	desc.count = SECOND_RECORD_LEN;
	consumed = svcsock_rx_run_actor(rqstp, &actor, &desc, skb,
					 FIRST_RECORD_LEN, SECOND_RECORD_LEN);
	KUNIT_ASSERT_EQ(test, consumed, (int)SECOND_RECORD_LEN);
	KUNIT_ASSERT_TRUE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 1U);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_publish(fixture->svsk, rqstp), 0);
	KUNIT_EXPECT_PTR_EQ(test, rqstp->rq_xprt_ctxt, state);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
	kunit_kfree_skb(test, skb);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
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
	KUNIT_EXPECT_EQ(test, active->lt.refs_acquired, 1U);
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
	enum { FRAG_LEN = 8 };
	/*
	 * The loan table holds svc_pages_to_bvecs() entries, which grows
	 * with PAGE_SIZE: take the fewest arena pages whose table holds
	 * the three loans the fault parameters release, then overflow
	 * it by one fragment to force the materialize fallback.
	 */
	struct svcsock_rx_fixture *fixture =
		svcsock_rx_alloc_fixture(test,
					 DIV_ROUND_UP(3, PAGE_SIZE / SZ_4K));
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	unsigned int nfrags = state->lt.capacity + 1;
	unsigned int body_len = 7 * XDR_UNIT + nfrags * FRAG_LEN;
	struct page **pages;
	unsigned int *refs;
	struct svc_pool *pool;
	struct sk_buff *skb;
	const u8 *payload;
	unsigned int i;
	u32 acquired;
	u32 released;
	u8 *record;
	int ret;

	if (nfrags > MAX_SKB_FRAGS)
		kunit_skip(test, "needs %u fragments in one skb, MAX_SKB_FRAGS is %u",
			   nfrags, (unsigned int)MAX_SKB_FRAGS);
	record = kunit_kzalloc(test, sizeof(rpc_fraghdr) + body_len, GFP_KERNEL);
	pages = kunit_kcalloc(test, nfrags, sizeof(*pages), GFP_KERNEL);
	refs = kunit_kcalloc(test, nfrags, sizeof(*refs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, record);
	KUNIT_ASSERT_NOT_NULL(test, pages);
	KUNIT_ASSERT_NOT_NULL(test, refs);

	svcsock_rx_fill_record(record, body_len, true, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_linear_skb(test, record,
					  sizeof(rpc_fraghdr) + 7 * XDR_UNIT);
	payload = record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT;
	for (i = 0; i < nfrags; i++) {
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
	KUNIT_EXPECT_EQ(test, acquired, state->lt.capacity);
	KUNIT_EXPECT_EQ(test, released, (u32)param->fail_after);
	KUNIT_EXPECT_EQ(test, acquired - released,
			state->lt.capacity - (u32)param->fail_after);
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
	for (i = 0; i < nfrags; i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), refs[i]);
	kunit_kfree_skb(test, skb);
	for (i = 0; i < nfrags; i++)
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
	KUNIT_EXPECT_EQ(test, state->lt.body_bytes, (u32)BODY_LEN);
	KUNIT_EXPECT_EQ(test, state->fixed_bytes, 7U * XDR_UNIT);
	/*
	 * Only the locked (cloned) head's suffix is copied; the frags
	 * after it are loaned as they are, each bvec holding a reference
	 * (the compound frag straddles a subpage, so it is two bvecs).
	 */
	KUNIT_EXPECT_EQ(test, state->lt.copied_bytes, (u32)HEAD_SUFFIX);
	KUNIT_EXPECT_EQ(test, state->lt.borrowed_bytes,
			(u32)(COMPOUND_LEN + CHILD_LEN + GRANDCHILD_LEN));
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 4U);
	KUNIT_EXPECT_EQ(test, page_ref_count(root_page), root_refs);
	KUNIT_EXPECT_EQ(test, page_ref_count(compound_page), compound_refs + 2);
	KUNIT_EXPECT_EQ(test, page_ref_count(child_page), child_refs + 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(grandchild_page),
			grandchild_refs + 1);

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_publish(fixture->svsk, fixture->rqstp), 0);
	KUNIT_ASSERT_EQ(test, state->mode, (u8)SVC_TCP_RX_PUBLISHED);
	kunit_kfree_skb(test, clone);
	kunit_kfree_skb(test, root);
	/* The published loans keep the frag pages past the skbs. */
	KUNIT_EXPECT_EQ(test, page_ref_count(root_page), root_refs - 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(compound_page), compound_refs + 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(child_page), child_refs);
	KUNIT_EXPECT_EQ(test, page_ref_count(grandchild_page), grandchild_refs);
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

/*
 * Two locked skb heads back to back, as TCP receive-queue collapse
 * produces, then borrowable data.  Both locked heads are copied into the
 * rq_pages arena, one after the other, and the borrowed bytes that follow
 * are loaned beside them, not copied over the arena bytes already there.
 */
static void svcsock_rx_back_to_back_locked_heads_test(struct kunit *test)
{
	enum {
		FIRST_LEN = 9,
		SECOND_LEN = 11,
		BORROWED_LEN = 64,
		BODY_LEN = 7 * XDR_UNIT + FIRST_LEN + SECOND_LEN + BORROWED_LEN,
	};
	struct svcsock_rx_fixture *fixture =
		svcsock_rx_alloc_fixture(test, 16);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(record) };
	struct sk_buff *root, *second, *borrowed;
	struct page *borrowed_page;
	unsigned int borrowed_refs;
	unsigned int cursor = 7 * XDR_UNIT + FIRST_LEN;
	int ret;

	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	/* linear (kmalloc'd) heads are locked: !head_frag */
	root = svcsock_rx_build_linear_skb(test, record,
					   sizeof(rpc_fraghdr) + cursor);
	second = svcsock_rx_build_linear_skb(test,
					     record + sizeof(rpc_fraghdr) + cursor,
					     SECOND_LEN);
	cursor += SECOND_LEN;
	borrowed = svcsock_rx_build_page_skb(test,
					     record + sizeof(rpc_fraghdr) + cursor,
					     BORROWED_LEN, &borrowed_page,
					     &borrowed_refs);
	cursor += BORROWED_LEN;
	KUNIT_ASSERT_EQ(test, cursor, (unsigned int)BODY_LEN);
	KUNIT_ASSERT_TRUE(test, skb_head_is_locked(root));
	KUNIT_ASSERT_TRUE(test, skb_head_is_locked(second));
	KUNIT_ASSERT_FALSE(test, skb_head_is_locked(borrowed));
	svcsock_rx_attach_frag_list(test, second, borrowed);
	svcsock_rx_attach_frag_list(test, root, second);

	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, root, 0,
				   root->len);
	KUNIT_ASSERT_EQ(test, ret, (int)root->len);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_TRUE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_MIXED);
	KUNIT_EXPECT_EQ(test, state->lt.body_bytes, (u32)BODY_LEN);
	/* the arena copy of both heads, then the loan */
	KUNIT_EXPECT_EQ(test, state->lt.count, 2U);
	KUNIT_EXPECT_EQ(test, state->lt.copied_bytes,
			(u32)(FIRST_LEN + SECOND_LEN));
	KUNIT_EXPECT_EQ(test, state->lt.borrowed_bytes, (u32)BORROWED_LEN);
	KUNIT_EXPECT_EQ(test, page_ref_count(borrowed_page), borrowed_refs + 1);

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_publish(fixture->svsk, fixture->rqstp), 0);
	KUNIT_ASSERT_EQ(test, state->mode, (u8)SVC_TCP_RX_PUBLISHED);
	KUNIT_EXPECT_MEMEQ(test, page_address(fixture->rqstp->rq_pages[0]),
			   record + sizeof(rpc_fraghdr), 7 * XDR_UNIT);
	svcsock_rx_expect_bvec_bytes(test, state,
				     record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT,
				     BODY_LEN - 7 * XDR_UNIT);

	kunit_kfree_skb(test, root);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
	KUNIT_EXPECT_EQ(test, page_ref_count(borrowed_page), borrowed_refs - 1);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
}

static struct svc_sock *svcsock_rx_credit_sock(struct kunit *test)
{
	struct svc_sock *svsk = kunit_kzalloc(test, sizeof(*svsk), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, svsk);
	svc_tcp_rx_wait_init(svsk);
	kref_init(&svsk->sk_xprt.xpt_ref);
	svsk->sk_xprt.xpt_net = &init_net;
	svsk->sk_xprt.xpt_locallen = sizeof(struct sockaddr_in);
	svsk->sk_xprt.xpt_remotelen = sizeof(struct sockaddr_in);
	svsk->sk_xprt.xpt_local.ss_family = AF_INET;
	svsk->sk_xprt.xpt_remote.ss_family = AF_INET;
	/* Busy transport: real enqueue observes DATA without needing a pool. */
	set_bit(XPT_BUSY, &svsk->sk_xprt.xpt_flags);
	return svsk;
}

/* Defined with the loan fixtures below. */
static struct svc_tcp_rx_state *
svcsock_rx_alloc_state(struct kunit *test, unsigned int capacity);

/*
 * No budget and no fixed factor.  A factor left fixed would outlive the
 * suite in the loaded sunrpc: every transport set up afterwards would be
 * held to it and not to what its skbs are measured to occupy.
 */
static void svcsock_rx_budget_off(void *unused)
{
	svc_tcp_rx_budget_kunit_configure(0, 0, 0);
}

/* A test that fails must not leave a freed transport on the wait list. */
static void svcsock_rx_credit_forget_action(void *svsk)
{
	svc_tcp_rx_credit_forget(svsk);
}

static struct svc_sock *svcsock_rx_credit_sock_forget(struct kunit *test)
{
	struct svc_sock *svsk = svcsock_rx_credit_sock(test);

	KUNIT_ASSERT_NOT_NULL(test, svsk);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_credit_forget_action, svsk), 0);
	return svsk;
}

/*
 * Budget 40 pages, reserve 10, factor 100: a 10-page record costs 11.  Only
 * the head waiter is admitted, credit returns on release, and the pool is
 * whole once no record holds credit.
 */
static void svcsock_rx_credit_fifo_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *c = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *d = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock(test);
	struct svc_sock *sb = svcsock_rx_credit_sock(test);
	struct svc_sock *sc = svcsock_rx_credit_sock(test);
	struct svc_sock *sd = svcsock_rx_credit_sock(test);
	const u32 len = 10 * PAGE_SIZE;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(40, 10, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_EQ(test, a->cr.credit, 11U);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 8L);
	/* 8 < 11: c waits, then d queues behind it even if credit appears. */
	KUNIT_EXPECT_FALSE(test, svc_tcp_rx_credit_admit(sc, c, len));
	KUNIT_EXPECT_FALSE(test, svc_tcp_rx_credit_admit(sd, d, len));
	svc_tcp_rx_credit_release(a);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 19L);
	KUNIT_EXPECT_FALSE(test, svc_tcp_rx_credit_admit(sd, d, len));
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_admit(sc, c, len));
	KUNIT_EXPECT_TRUE(test, list_empty(&sc->sk_rx_wait.node));
	KUNIT_EXPECT_FALSE(test, list_empty(&sd->sk_rx_wait.node));
	/* Reserve is untouched by ordinary admission. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 10L);
	svc_tcp_rx_credit_forget(sd);
	svc_tcp_rx_credit_release(b);
	svc_tcp_rx_credit_release(c);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 30L);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 10L);
	KUNIT_EXPECT_FALSE(test, a->cr.linked);
	svc_tcp_rx_credit_release(a);	/* exactly once: a second call is a no-op */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 30L);
}

/*
 * With no fixed factor a budget learns what its records hold.  Until one
 * has ended a record is charged all it could hold; then the factor is the
 * most pages a record was lent for each 100 of record, less the page every
 * charge adds: taken at once when higher, kept until two windows of
 * records have stayed below it, left alone by a record that was lent none.
 * A fixed factor overrides it.
 */
static void svcsock_rx_credit_learn_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 256);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 256);
	struct svc_sock *sa = svcsock_rx_credit_sock(test);
	struct svc_sock *sb = svcsock_rx_credit_sock(test);
	const u32 len = 10 * PAGE_SIZE;
	unsigned int ended = 0;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(200, 10, 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 0U);

	/*
	 * Nothing learned: all the record could hold, which is more than
	 * the budget (256 entries on every page size; 64 pages are 64
	 * entries on 4 KiB pages, less than the budget), so the oldest
	 * record takes the whole of it and its reserve.
	 */
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_EQ(test, a->cr.credit, 200U);
	KUNIT_EXPECT_FALSE(test, svc_tcp_rx_credit_admit(sb, b, len));
	svc_tcp_rx_credit_forget(sb);
	a->fixed_bytes = 0;
	a->lt.body_bytes = len;
	b->fixed_bytes = 0;
	b->lt.body_bytes = len;

	/* 16 pages for 10 of record: 150 covers it with the page added. */
	a->lt.region_count = 16;
	svc_tcp_rx_credit_release(a);
	ended++;
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 150U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_avail(), 190L);

	/* Charged by what that one held. */
	a->lt.region_count = 0;
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_EQ(test, a->cr.credit, 16U);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_EXPECT_EQ(test, b->cr.credit, 16U);

	/* More than was charged for: taken at once. */
	a->lt.region_count = 21;
	svc_tcp_rx_credit_release(a);
	ended++;
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 200U);

	/* Fewer does not lower it. */
	b->lt.region_count = 11;
	svc_tcp_rx_credit_release(b);
	ended++;
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 200U);

	/* A record that was lent no region page teaches nothing. */
	a->lt.region_count = 0;
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_EQ(test, a->cr.credit, 21U);
	svc_tcp_rx_credit_release(a);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 200U);

	/* A record cut short is measured by what it received: 6 for 2. */
	a->lt.body_bytes = 2 * PAGE_SIZE;
	a->lt.region_count = 6;
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	svc_tcp_rx_credit_release(a);
	ended++;
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 250U);
	a->lt.body_bytes = len;

	/*
	 * The window that saw 250 ends and the next passes without it: it
	 * still stands.  Only when a window and the one before it stayed
	 * under it does the estimate come down, and never under one region
	 * page to a page of record.
	 */
	a->lt.region_count = 2;
	for (; ended < 2 * RX_LOAN_COST_WINDOW; ended++) {
		KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
		svc_tcp_rx_credit_release(a);
	}
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 250U);
	for (; ended < 3 * RX_LOAN_COST_WINDOW; ended++) {
		KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
		svc_tcp_rx_credit_release(a);
	}
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 100U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_avail(), 190L);

	/* A fixed factor is used as it is, whatever was learned. */
	svc_tcp_rx_budget_kunit_configure(200, 10, 100);
	a->lt.region_count = 0;
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_EQ(test, a->cr.credit, 11U);
	svc_tcp_rx_credit_release(a);
}

/* Read whole skbs of @len bytes in @truesize until a sample is taken. */
static void svcsock_rx_geometry_window(struct svc_sock *svsk,
				       unsigned int truesize, unsigned int len)
{
	do {
		svc_tcp_rx_geometry_account(svsk, truesize, len, len);
	} while (svsk->sk_rx_geo_pay);
}

/*
 * A transport's receive buffer is held by what its skbs occupy for each
 * 100 bytes of payload: the higher of its last two samples, a sample being
 * a window's worth of skbs that carry data.  Until it has one it is held
 * to what another transport of its budget measured, or to 150.  A fixed
 * factor overrides all of it.
 */
static void svcsock_rx_geometry_test(struct kunit *test)
{
	struct svc_sock *sa = svcsock_rx_credit_sock(test);
	struct svc_sock *sb = svcsock_rx_credit_sock(test);

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(0, 0, 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_hold_factor(sa), 150U);

	/* A few skbs are counted and are not yet a sample. */
	svc_tcp_rx_geometry_account(sa, 8448, 7740, 7740);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_hold_factor(sa), 150U);
	/* Half of an skb read is half of its memory. */
	svc_tcp_rx_geometry_account(sa, 8448, 7740, 3870);
	KUNIT_EXPECT_EQ(test, sa->sk_rx_geo_mem, 8448U + 4224U);
	KUNIT_EXPECT_EQ(test, sa->sk_rx_geo_pay, 7740U + 3870U);
	/*
	 * Nothing read of an skb counts for nothing, and neither does an
	 * skb too small to carry data: a call in a receive buffer of its
	 * own would have the transport held to less than a segment.
	 */
	svc_tcp_rx_geometry_account(sa, 8448, 7740, 0);
	svc_tcp_rx_geometry_account(sa, 8448, 100, 100);
	svc_tcp_rx_geometry_account(sa, 8448, 1023, 1023);
	KUNIT_EXPECT_EQ(test, sa->sk_rx_geo_mem, 8448U + 4224U);
	KUNIT_EXPECT_EQ(test, sa->sk_rx_geo_pay, 7740U + 3870U);

	/*
	 * A window of them is a sample: 8448 bytes for 7740 of payload is
	 * 110 to the 100.  The budget's other transports start from it.
	 */
	svcsock_rx_geometry_window(sa, 8448, 7740);
	KUNIT_EXPECT_EQ(test, sa->sk_rx_geo_factor[0], 110U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_hold_factor(sa), 110U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_hold_factor(sb), 110U);

	/* A whole page to a frame: the higher sample is the one in force. */
	svcsock_rx_geometry_window(sa, 16640, 8960);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_hold_factor(sa), 186U);
	svcsock_rx_geometry_window(sa, 8448, 7740);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_hold_factor(sa), 186U);
	svcsock_rx_geometry_window(sa, 8448, 7740);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_hold_factor(sa), 110U);

	/* Small writes, each in a buffer of its own, are measured as they are. */
	svcsock_rx_geometry_window(sb, 8448, 1024);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_hold_factor(sb), 825U);

	/* A fixed factor is used as it is. */
	svc_tcp_rx_budget_kunit_configure(0, 0, 190);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_hold_factor(sa), 190U);
}

/*
 * Only the oldest in-flight record draws on the reserve (and past the
 * budget); a younger record that outgrows its grant pauses.
 */
static void svcsock_rx_credit_reserve_oldest_test(struct kunit *test)
{
	struct svc_tcp_rx_state *old = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *young = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *so = svcsock_rx_credit_sock(test);
	struct svc_sock *sy = svcsock_rx_credit_sock(test);
	const u32 len = 10 * PAGE_SIZE;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(30, 6, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(so, old, len));
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sy, young, len));
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 2L);
	/* The young record needs 14 > 11 + 2: it takes 2 and pauses. */
	KUNIT_EXPECT_FALSE(test, svc_tcp_rx_credit_charge(sy, young, 14));
	KUNIT_EXPECT_EQ(test, young->cr.credit, 13U);
	KUNIT_EXPECT_FALSE(test, list_empty(&sy->sk_rx_wait.node));
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 6L);
	/* The oldest may take the whole reserve and then run past the budget. */
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_charge(so, old, 20));
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 0L);
	KUNIT_EXPECT_EQ(test, old->cr.credit, 20U);
	KUNIT_EXPECT_EQ(test, old->cr.reserve, 6U);
	KUNIT_EXPECT_EQ(test, old->cr.over, 3U);
	/* Release: the reserve refills, overrun is not returned to the pool. */
	svc_tcp_rx_credit_release(old);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 6L);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 11L);
	/* Now oldest, the paused record completes on the reserve if needed. */
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_charge(sy, young, 14));
	KUNIT_EXPECT_TRUE(test, list_empty(&sy->sk_rx_wait.node));
	svc_tcp_rx_credit_release(young);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 24L);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 6L);
}

/* The window never promises more payload than the locked buffer holds. */
/* Last release disables admission, then every queued transport resumes once. */
static void svcsock_rx_credit_disable_waiters_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *c = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock(test);
	struct svc_sock *sb = svcsock_rx_credit_sock(test);
	struct svc_sock *sc = svcsock_rx_credit_sock(test);
	const u32 len = 10 * PAGE_SIZE;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(10, 0, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_credit_admit(sc, c, len));
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_DATA, &sb->sk_xprt.xpt_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));
	svc_tcp_rx_budget_kunit_configure(0, 0, 150);
	svc_tcp_rx_credit_release(a);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 0L);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_DATA, &sb->sk_xprt.xpt_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));
	KUNIT_EXPECT_EQ(test, kref_read(&sb->sk_xprt.xpt_ref), 1U);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_EXPECT_TRUE(test, list_empty(&sb->sk_rx_wait.node));
	KUNIT_EXPECT_EQ(test, sb->sk_rx_wait.ticket, 0ULL);
	KUNIT_EXPECT_FALSE(test, b->cr.linked);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));
	KUNIT_EXPECT_EQ(test, kref_read(&sc->sk_xprt.xpt_ref), 1U);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sc, c, len));
	KUNIT_EXPECT_TRUE(test, list_empty(&sc->sk_rx_wait.node));
	KUNIT_EXPECT_EQ(test, sc->sk_rx_wait.ticket, 0ULL);
	KUNIT_EXPECT_FALSE(test, c->cr.linked);
}

/*
 * Budget 10 = reserve 10, factor 100: the first record takes the whole
 * reserve.  When its loans cannot be accounted for, it is abandoned with its
 * credit still spent, but it no longer blocks the next record from being the
 * oldest, which then progresses on an overrun.
 */
static void svcsock_rx_credit_orphan_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;

	/*
	 * It orphans a record of the initial namespace's budget: under a
	 * carved region that credit stays spent in the live consumer's share.
	 */
	if (page_pool_rx_region_exists())
		kunit_skip(test, "a region is carved already");
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(10, 10, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_EQ(test, a->cr.credit, 10U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 0L);
	/* A reference with no loan behind it fails the invariants. */
	a->lt.refs_acquired = 1;
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_EQ(test, svc_tcp_rx_release_refs(a), -EINVAL);
	}
	a->lt.refs_acquired = 0;
	KUNIT_EXPECT_FALSE(test, a->cr.linked);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 0L);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 0L);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_EXPECT_TRUE(test, b->cr.linked);
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_charge(sb, b, 5));
	KUNIT_EXPECT_EQ(test, b->cr.over, 5U);
	svc_tcp_rx_credit_release(b);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 0L);
	svc_tcp_rx_credit_release(a);	/* abandoned: nothing to return */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 0L);
}

/*
 * Budget 40, reserve 10, factor 100: a 10-page record costs 11.  A change
 * applies with records in flight; they return their credit to the new
 * budget, and turning admission off lets them finish unthrottled.
 */
static void svcsock_rx_credit_reconfigure_inflight_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(40, 10, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 19L);
	svc_tcp_rx_budget_kunit_configure(80, 10, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 48L);
	svc_tcp_rx_credit_release(a);
	svc_tcp_rx_credit_release(b);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 70L);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 10L);

	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	svc_tcp_rx_budget_kunit_configure(0, 0, 150);
	/* Off: no wait for credit, whatever the record needs. */
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_charge(sa, a, 100));
	KUNIT_EXPECT_TRUE(test, list_empty(&sa->sk_rx_wait.node));
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_EXPECT_FALSE(test, b->cr.linked);
	svc_tcp_rx_credit_release(a);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 0L);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_reserve_avail(), 0L);
}

/*
 * Budget 10, no reserve, factor 100: a 10-page record costs 11, so a second
 * record waits.  The first ends without waking it (as when the pool's
 * last record ends before a change), leaving a waiter and nothing in
 * flight: nothing else will run the pool, so the change itself must wake
 * the waiter, whether it raises the budget or turns admission off.
 */
static void svcsock_rx_credit_reconfigure_wakes_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *c = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sc = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(10, 0, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_credit_admit(sb, b, len));
	svc_tcp_rx_credit_release(a);
	KUNIT_ASSERT_FALSE(test, test_bit(XPT_DATA, &sb->sk_xprt.xpt_flags));

	/* Raised: the waiter is woken and is admitted. */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(20), 0);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_DATA, &sb->sk_xprt.xpt_flags));
	KUNIT_EXPECT_EQ(test, kref_read(&sb->sk_xprt.xpt_ref), 1U);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_EXPECT_TRUE(test, b->cr.linked);

	/* Strand the next waiter the same way, then turn admission off. */
	svc_tcp_rx_budget_kunit_configure(20, 0, 100);
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_credit_admit(sc, c, len));
	svc_tcp_rx_credit_release(b);
	KUNIT_ASSERT_FALSE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(0), 0);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));
	KUNIT_EXPECT_EQ(test, kref_read(&sc->sk_xprt.xpt_ref), 1U);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sc, c, len));
	KUNIT_EXPECT_FALSE(test, c->cr.linked);
	KUNIT_EXPECT_TRUE(test, list_empty(&sc->sk_rx_wait.node));
}

/*
 * Budget 20, no reserve, factor 100: records of 5 and 10 pages cost 6 and
 * 11, so with both in flight 3 is left and a third 10-page record is
 * refused, asking for 11.  The 5-page record's release leaves 9 while the
 * other is still in flight: the waiter is not woken, since it would only be
 * refused again.  The last release wakes it, and it is admitted.
 */
static void svcsock_rx_credit_wake_need_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *c = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sc = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(20, 0, 100);
	/* From here wakes are real. */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(20), 0);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, 5 * PAGE_SIZE));
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_avail(), 3L);
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_credit_admit(sc, c, len));
	KUNIT_EXPECT_EQ(test, sc->sk_rx_wait.need, 11U);
	KUNIT_ASSERT_FALSE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));

	/* Short of what it asked for, with a record in flight: not woken. */
	svc_tcp_rx_credit_release(a);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_avail(), 9L);
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));
	KUNIT_EXPECT_FALSE(test, list_empty(&sc->sk_rx_wait.node));

	/* Nothing in flight: woken, and admitted. */
	svc_tcp_rx_credit_release(b);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_avail(), 20L);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));
	KUNIT_EXPECT_EQ(test, kref_read(&sc->sk_xprt.xpt_ref), 1U);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sc, c, len));
	KUNIT_EXPECT_TRUE(test, list_empty(&sc->sk_rx_wait.node));
	KUNIT_EXPECT_EQ(test, sc->sk_rx_wait.need, 0U);
	svc_tcp_rx_credit_release(c);
}

static void svcsock_rx_noop_data_ready(struct sock *sk)
{
}

/*
 * Budget 10, no reserve, factor 100: a 10-page record costs 11, so a second
 * record waits.  Arriving data does not re-queue the waiting transport (the
 * credit wake does); once it no longer waits, arriving data marks it again.
 */
static void svcsock_rx_credit_data_ready_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = svcsock_rx_credit_sock_forget(test);
	struct sock *sk = kunit_kzalloc(test, sizeof(*sk), GFP_KERNEL);
	const u32 len = 10 * PAGE_SIZE;

	KUNIT_ASSERT_NOT_NULL(test, sk);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	sk->sk_user_data = sb;
	sb->sk_sk = sk;
	sb->sk_odata = svcsock_rx_noop_data_ready;
	svc_tcp_rx_budget_kunit_configure(10, 0, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_credit_admit(sb, b, len));
	svc_data_ready(sk);
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_DATA, &sb->sk_xprt.xpt_flags));
	svc_tcp_rx_credit_forget(sb);
	svc_data_ready(sk);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_DATA, &sb->sk_xprt.xpt_flags));
	svc_tcp_rx_credit_release(a);
}

/*
 * A TCP socket for svc_data_ready(): @queued bytes past the last read, a
 * receive window of @wnd from that read, a 4 MiB receive buffer.
 */
static struct sock *svcsock_rx_want_sk(struct kunit *test,
				       struct svc_sock *svsk, u32 queued,
				       u32 wnd)
{
	struct tcp_sock *tp = kunit_kzalloc(test, sizeof(*tp), GFP_KERNEL);
	struct sock *sk;

	KUNIT_ASSERT_NOT_NULL(test, tp);
	sk = (struct sock *)tp;
	sk->sk_rcvbuf = 4 << 20;
	tp->copied_seq = 1000;
	tp->rcv_wup = 1000;
	tp->rcv_nxt = 1000 + queued;
	tp->rcv_wnd = wnd;
	inet_csk(sk)->icsk_ack.rcv_mss = 7740;
	sk->sk_user_data = svsk;
	svsk->sk_sk = sk;
	svsk->sk_odata = svcsock_rx_noop_data_ready;
	return sk;
}

/*
 * A drained read of a loaned 1 MiB record with 256 KiB of it in: the rest
 * is wanted, capped at half the receive window still open.  Not drained,
 * no credit, complete, or TLS: nothing is wanted.
 */
static void svcsock_rx_set_want_test(struct kunit *test)
{
	struct svc_tcp_rx_state *state = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *svsk = svcsock_rx_credit_sock(test);
	struct sock *sk = svcsock_rx_want_sk(test, svsk, 0, 2 << 20);
	struct tcp_sock *tp = tcp_sk(sk);
	const u32 reclen = 1 << 20;

	svsk->sk_marker = cpu_to_be32(RPC_LAST_STREAM_FRAGMENT | reclen);
	svsk->sk_tcplen = sizeof(rpc_fraghdr) + reclen / 4;
	state->cr.credit = 65;

	svc_tcp_rx_set_want(svsk, state, true);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_want, reclen - reclen / 4);
	svc_tcp_rx_set_want(svsk, state, false);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_want, 0U);

	tp->rcv_wnd = 1 << 20;
	svc_tcp_rx_set_want(svsk, state, true);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_want, 512U << 10);
	/* What the peer may still send is the window less what it sent. */
	tp->rcv_nxt = tp->rcv_wup + (768U << 10);
	svc_tcp_rx_set_want(svsk, state, true);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_want, 128U << 10);
	tp->rcv_nxt = tp->rcv_wup + (2U << 20);
	svc_tcp_rx_set_want(svsk, state, true);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_want, 0U);
	tp->rcv_nxt = tp->rcv_wup;
	svc_tcp_rx_set_want(svsk, NULL, true);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_want, 0U);

	state->cr.credit = 0;
	svc_tcp_rx_set_want(svsk, state, true);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_want, 0U);

	state->cr.credit = 65;
	svsk->sk_tcplen = sizeof(rpc_fraghdr) + reclen;
	svc_tcp_rx_set_want(svsk, state, true);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_want, 0U);

	svsk->sk_tcplen = sizeof(rpc_fraghdr) - 1;
	svc_tcp_rx_set_want(svsk, state, true);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_want, 0U);

	svsk->sk_tcplen = sizeof(rpc_fraghdr) + reclen / 4;
	set_bit(XPT_TLS_SESSION, &svsk->sk_xprt.xpt_flags);
	svc_tcp_rx_set_want(svsk, state, true);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_want, 0U);
}

/*
 * 64 KiB wanted, 16 KiB queued: arriving data marks nothing.  It does once
 * the 64 KiB is queued, under receive memory pressure, with the receive
 * window nearly closed, after a FIN, or with nothing wanted.
 */
static void svcsock_rx_want_data_ready_test(struct kunit *test)
{
	struct svc_sock *svsk = svcsock_rx_credit_sock(test);
	struct sock *sk = svcsock_rx_want_sk(test, svsk, 16 << 10, 1 << 20);
	struct tcp_sock *tp = tcp_sk(sk);
	unsigned long *flags = &svsk->sk_xprt.xpt_flags;

	svsk->sk_rx_want = 64 << 10;
	svc_data_ready(sk);
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_DATA, flags));

	tp->rcv_nxt = 1000 + (64 << 10);
	svc_data_ready(sk);
	KUNIT_EXPECT_TRUE(test, test_and_clear_bit(XPT_DATA, flags));

	tp->rcv_nxt = 1000 + (16 << 10);
	atomic_set(&sk->sk_rmem_alloc, sk->sk_rcvbuf);
	svc_data_ready(sk);
	KUNIT_EXPECT_TRUE(test, test_and_clear_bit(XPT_DATA, flags));
	atomic_set(&sk->sk_rmem_alloc, 0);

	tp->rcv_wnd = (16 << 10) + 7740;
	svc_data_ready(sk);
	KUNIT_EXPECT_TRUE(test, test_and_clear_bit(XPT_DATA, flags));
	tp->rcv_wnd = 1 << 20;

	svc_data_ready(sk);
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_DATA, flags));
	sock_set_flag(sk, SOCK_DONE);
	svc_data_ready(sk);
	KUNIT_EXPECT_TRUE(test, test_and_clear_bit(XPT_DATA, flags));
	sock_reset_flag(sk, SOCK_DONE);

	svsk->sk_rx_want = 0;
	svc_data_ready(sk);
	KUNIT_EXPECT_TRUE(test, test_and_clear_bit(XPT_DATA, flags));
}

static void svcsock_rx_budget_default_off(void *unused)
{
	svc_tcp_rx_budget_kunit_default(0, 0);
}

/*
 * No budget in the initial namespace, a default budget of 20 pages, factor
 * 100: a transport there draws on the default budget.  A 10-page record
 * costs 11, so a second waits; credit returns to the default budget, and
 * the initial namespace's budget is never touched.
 */
static void svcsock_rx_credit_default_budget_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;
	long init_avail;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_default_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(0, 0, 100);
	svc_tcp_rx_budget_kunit_rcvbuf(128 * 1024);
	svc_tcp_rx_budget_kunit_default(20, 0);
	/* Whatever an earlier user left in force there, nothing changes it. */
	init_avail = svc_tcp_rx_budget_kunit_avail();
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_TRUE(test, a->cr.linked);
	KUNIT_EXPECT_EQ(test, a->cr.credit, 11U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_default_avail(), 9L);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), init_avail);
	KUNIT_EXPECT_FALSE(test, svc_tcp_rx_credit_admit(sb, b, len));
	svc_tcp_rx_credit_release(a);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_default_avail(), 20L);
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	svc_tcp_rx_credit_release(b);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_default_avail(), 20L);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), init_avail);
}

/*
 * A default budget of 20 is in use, and the initial namespace's budget of
 * 10 holds two waiters with nothing in flight.  Turning that budget off
 * wakes the head, which then copies its region records rather than being
 * admitted: it must stop waiting and wake the next, or both are stranded
 * (a waiting transport is not re-queued by arriving data).
 */
static void svcsock_rx_credit_copy_unwaits_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *c = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sc = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_default_off, NULL), 0);
	svc_tcp_rx_budget_kunit_default(20, 0);
	svc_tcp_rx_budget_kunit_configure(10, 0, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_credit_admit(sc, c, len));
	svc_tcp_rx_credit_release(a);
	KUNIT_ASSERT_FALSE(test, test_bit(XPT_DATA, &sb->sk_xprt.xpt_flags));

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(0), 0);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_DATA, &sb->sk_xprt.xpt_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_budget_copy(sb));
	KUNIT_EXPECT_TRUE(test, list_empty(&sb->sk_rx_wait.node));
	KUNIT_EXPECT_NULL(test, sb->sk_rx_wait.budget);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));
	KUNIT_EXPECT_EQ(test, kref_read(&sc->sk_xprt.xpt_ref), 1U);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_budget_copy(sc));
	KUNIT_EXPECT_TRUE(test, list_empty(&sc->sk_rx_wait.node));
}

/*
 * No region page is loaned without credit.  With no budget set anywhere a
 * transport's region records are copied, exactly as when another budget is
 * set and its own is 0; a budget it can draw on, its namespace's or the
 * default, is what makes them loans.
 */
static void svcsock_rx_credit_copy_no_budget_test(struct kunit *test)
{
	struct svc_sock *svsk = svcsock_rx_credit_sock_forget(test);

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_default_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(0, 0, 100);
	svc_tcp_rx_budget_kunit_default(0, 0);
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_budget_copy(svsk));

	svc_tcp_rx_budget_kunit_default(20, 0);
	KUNIT_EXPECT_FALSE(test, svc_tcp_rx_budget_copy(svsk));
	svc_tcp_rx_budget_kunit_default(0, 0);
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_budget_copy(svsk));

	svc_tcp_rx_budget_kunit_configure(10, 0, 100);
	KUNIT_EXPECT_FALSE(test, svc_tcp_rx_budget_copy(svsk));
	svc_tcp_rx_budget_kunit_configure(0, 0, 100);
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_budget_copy(svsk));
}

static void svcsock_rx_region_off(void *unused)
{
	svc_tcp_rx_budget_kunit_region(0, 0);
	svc_tcp_rx_budget_kunit_init();
}

static void svcsock_rx_expect_stats(struct kunit *test,
				    struct svc_tcp_rx_budget *c,
				    const char *prefix)
{
	char buf[256];

	svc_tcp_rx_budget_kunit_stats(c, buf, sizeof(buf));
	KUNIT_EXPECT_TRUE_MSG(test, str_has_prefix(buf, prefix), "%s", buf);
}

/*
 * A split region's read partition has its own pool: its budget is the
 * partition, its reserve 0 (no reserve), and neither the receive clamp nor
 * receive admission on a transport moves it.  Unsplit, it holds what the
 * configured budgets leave of the region, a READ on a budgeted transport
 * draws on the budget its receive draws on, and a READ on a transport
 * whose budget is 0 draws on the read pool.
 */
static void svcsock_rx_read_pool_test(struct kunit *test)
{
	struct svc_tcp_rx_budget *read = svc_tcp_rx_budget_kunit_read();
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *svsk = svcsock_rx_credit_sock_forget(test);
	const u32 len = 5 * PAGE_SIZE;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_default_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(0, 0, 100);
	svc_tcp_rx_budget_kunit_default(0, 0);
	/* A clamp below the floor turns receive budgets off, not this one. */
	svc_tcp_rx_budget_kunit_rcvbuf(0);
	svc_tcp_rx_budget_kunit_region(100, 30);
	svc_tcp_rx_budget_kunit_init();
	svcsock_rx_expect_stats(test, read,
		"budget 30 reserve_size 0 avail 30 reserve 0 inflight 0 waiters 0");
	KUNIT_EXPECT_PTR_EQ(test, svc_tcp_rx_budget_read_for(svsk), read);

	svc_tcp_rx_budget_kunit_configure(10, 0, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(svsk, a, len));
	KUNIT_ASSERT_TRUE(test, a->cr.linked);
	KUNIT_EXPECT_PTR_NE(test, (void *)a->cr.budget, read);
	svcsock_rx_expect_stats(test, read,
		"budget 30 reserve_size 0 avail 30 reserve 0 inflight 0");
	svc_tcp_rx_credit_release(a);

	/* The configured total (10) comes from svc_tcp_rx_budget_set(). */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(10), 0);
	svc_tcp_rx_budget_kunit_region(100, 0);
	svc_tcp_rx_budget_kunit_init();
	svcsock_rx_expect_stats(test, read,
		"budget 90 reserve_size 0 avail 90 reserve 0 inflight 0");
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(svsk, a, len));
	KUNIT_ASSERT_TRUE(test, a->cr.linked);
	KUNIT_EXPECT_PTR_EQ(test, svc_tcp_rx_budget_read_for(svsk),
			    (void *)a->cr.budget);
	svc_tcp_rx_credit_release(a);

	/* A transport whose budget is 0 sends its READs to the read pool. */
	svc_tcp_rx_budget_kunit_configure(0, 0, 100);
	KUNIT_EXPECT_PTR_EQ(test, svc_tcp_rx_budget_read_for(svsk), read);
}

/*
 * The read pool needs neither the receive clamp nor an explicit apply: with
 * svc_tcp_rx_rcvbuf at its default 0 it is on as soon as the region is
 * split, and transport class registration (svc_init_xprt_sock()) configures
 * it.
 */
static void svcsock_rx_read_pool_init_test(struct kunit *test)
{
	struct svc_tcp_rx_budget *read = svc_tcp_rx_budget_kunit_read();

	/*
	 * Its first step stands in no region, which is the real one, if any;
	 * a size configured or asked for by a live consumer counts too.
	 */
	if (page_pool_rx_region_size())
		kunit_skip(test, "a region is carved or configured already");
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_default_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(0, 0, 100);
	svc_tcp_rx_budget_kunit_default(0, 0);
	svc_tcp_rx_budget_kunit_rcvbuf(0);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(0), 0);
	svc_tcp_rx_budget_kunit_region(0, 0);
	svc_tcp_rx_budget_kunit_init();
	svcsock_rx_expect_stats(test, read, "budget 0 ");

	svc_tcp_rx_budget_kunit_region(100, 30);
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_budget_on(read));
	svc_tcp_rx_budget_kunit_init();
	svcsock_rx_expect_stats(test, read,
		"budget 30 reserve_size 0 avail 30 reserve 0 inflight 0 waiters 0");
}

/* Publish one loaned record of @body_len bytes; returns its loaned page. */
static struct page *svcsock_rx_publish_loan(struct kunit *test,
					    struct svcsock_rx_fixture *fixture,
					    u8 *record, unsigned int body_len,
					    unsigned int *refs)
{
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(rpc_fraghdr) + body_len };
	struct sk_buff *skb;
	struct page *page;
	int ret;

	/* The bounce looks up the transport's budget. */
	fixture->svsk->sk_xprt.xpt_net = &init_net;
	svcsock_rx_fill_record(record, body_len, true, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_page_skb(test, record,
					sizeof(rpc_fraghdr) + body_len, &page,
					refs);
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0,
				   sizeof(rpc_fraghdr) + body_len);
	KUNIT_ASSERT_EQ(test, ret, (int)(sizeof(rpc_fraghdr) + body_len));
	KUNIT_ASSERT_TRUE(test, actor.complete);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_publish(fixture->svsk, fixture->rqstp), 0);
	kunit_kfree_skb(test, skb);
	return page;
}

/* The WRITE payload view nfsd holds: @len bytes at @offset into the loan. */
static void svcsock_rx_payload_view(struct xdr_buf *payload,
				    struct svc_tcp_rx_state *state,
				    unsigned int offset, unsigned int len)
{
	memset(payload, 0, sizeof(*payload));
	payload->page_mode = XDRBUF_PAGE_BVECS;
	payload->bvec = state->lt.bvec;
	payload->bvec_count = 1;
	payload->bvec_offset = offset;
	payload->page_len = len;
	payload->len = len;
}

/*
 * A loan that holds no region credit has its hole filled in the request's
 * own arena at its record-body offset, returns the loaned page once, and
 * detaches the argument pages; there is nothing to give back afterwards.
 * When that layout cannot meet the alignment nfsd asks for it copies
 * nothing and leaves the loan as it was.
 */
static void svcsock_rx_bounce_arena_test(struct kunit *test)
{
	enum { BODY_LEN = 7 * XDR_UNIT + 300, OFFSET = 8, LEN = 200 };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct bio_vec out[4];
	struct xdr_buf payload;
	unsigned int refs;
	struct page *page;
	int n;

	page = svcsock_rx_publish_loan(test, fixture, record, BODY_LEN, &refs);
	KUNIT_ASSERT_EQ(test, page_ref_count(page), refs);
	KUNIT_ASSERT_FALSE(test, state->cr.linked);
	svcsock_rx_payload_view(&payload, state, OFFSET, LEN);
	/* Memory offset 36 cannot meet a 512-byte alignment at offset 0. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp,
						&payload, 0, 512, out,
						ARRAY_SIZE(out)),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	KUNIT_EXPECT_FALSE(test, state->terminal);
	/*
	 * At file in-page offset 4, body offset 36 meets a 4-byte memory
	 * alignment but not the 512-byte offset alignment nfsd asks for under
	 * a joint boundary above PAGE_SIZE.  The copy's start is checked; a
	 * page-tiled layout puts every later page joint at the same residue.
	 */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp,
						&payload, 4, 512, out,
						ARRAY_SIZE(out)),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	KUNIT_EXPECT_FALSE(test, state->terminal);
	/*
	 * At file in-page offset 36 the same layout lines up with the file
	 * position and meets the 512-byte alignment.
	 */
	n = svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp, &payload,
			      36, 512, out, ARRAY_SIZE(out));
	KUNIT_ASSERT_EQ(test, n, 1);
	KUNIT_EXPECT_PTR_EQ(test, out[0].bv_page, fixture->rqstp->rq_pages[0]);
	KUNIT_EXPECT_EQ(test, out[0].bv_offset, 7U * XDR_UNIT + OFFSET);
	KUNIT_EXPECT_EQ(test, out[0].bv_len, (unsigned int)LEN);
	KUNIT_EXPECT_MEMEQ(test, page_address(out[0].bv_page) + out[0].bv_offset,
			   record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT + OFFSET,
			   LEN);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
	KUNIT_EXPECT_EQ(test, state->lt.refs_released, state->lt.refs_acquired);
	KUNIT_EXPECT_TRUE(test, state->terminal);
	KUNIT_EXPECT_EQ(test, state->bounce_pages, 0U);
	KUNIT_EXPECT_EQ(test, fixture->rqstp->rq_arg.page_len, 0U);
	/* A second bounce, and early release, find nothing left to do. */
	KUNIT_EXPECT_LT(test, svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp,
						&payload, 0, 1, out,
						ARRAY_SIZE(out)),
			0);
	svc_tcp_rx_release_payload(fixture->svsk, fixture->rqstp);
	svc_tcp_rx_bounce_done(fixture->rqstp, out, n);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
}

#if IS_ENABLED(CONFIG_PAGE_POOL)
/* A fixture region: pages the test owns, published as the rx region. */
#define SVCSOCK_RX_REGION_ORDER	4
#define SVCSOCK_RX_REGION_PAGES	(1UL << SVCSOCK_RX_REGION_ORDER)

struct svcsock_rx_region_fixture {
	struct page *base;
	unsigned long pfn;
	unsigned long freed;	/* pages the test already freed */
};

static void svcsock_rx_region_fixture_free(struct svcsock_rx_region_fixture *f)
{
	unsigned long i;

	for (i = 0; i < SVCSOCK_RX_REGION_PAGES; i++)
		if (!test_bit(i, &f->freed))
			__free_page(f->base + i);
}

static void svcsock_rx_region_fixture_remove(void *data)
{
	rx_loan_kunit_region_set_aside(false);
	page_pool_rx_region_kunit_remove(false);
	svcsock_rx_region_fixture_free(data);
}

static struct svcsock_rx_region_fixture *
svcsock_rx_region_fixture(struct kunit *test, unsigned long nr_read)
{
	struct svcsock_rx_region_fixture *f;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	f->base = alloc_pages(GFP_KERNEL, SVCSOCK_RX_REGION_ORDER);
	KUNIT_ASSERT_NOT_NULL(test, f->base);
	/* Every region page has its own reference, as alloc_contig gives. */
	split_page(f->base, SVCSOCK_RX_REGION_ORDER);
	f->pfn = page_to_pfn(f->base);
	ret = page_pool_rx_region_kunit_install(f->base,
						SVCSOCK_RX_REGION_PAGES,
						nr_read);
	if (ret)
		svcsock_rx_region_fixture_free(f);
	if (ret == -EBUSY)
		kunit_skip(test, "a fixed rx region already exists");
	KUNIT_ASSERT_EQ(test, ret, 0);
	rx_loan_kunit_region_set_aside(true);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_fixture_remove, f), 0);
	return f;
}

/* Abandon @state's admitted record as svc_tcp_rx_release_refs() does. */
static void svcsock_rx_credit_orphan(struct kunit *test,
				     struct svc_tcp_rx_state *state)
{
	state->lt.refs_acquired = 1;
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_EQ(test, svc_tcp_rx_release_refs(state), -EINVAL);
	}
	state->lt.refs_acquired = 0;
	KUNIT_EXPECT_FALSE(test, state->cr.linked);
}

/*
 * Budget 34, reserve 1, factor 100: a 10-page record costs 11.  Credit
 * charged with no region holds no region page: abandoned, it goes back to
 * the region's total, so a region carved later does not start with it
 * spent, nor does one carved while it is held.  Credit charged and
 * abandoned while a region is in place stays spent there.  Abandoned
 * credit stays spent in the budget, and a later test asking for the same
 * configuration would find it unchanged: no other test uses it.
 */
static void svcsock_rx_credit_orphan_region_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *c = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sc = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;
	long out, orphaned;

	if (rx_loan_region_size())
		kunit_skip(test, "a fixed rx region already exists");
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(34, 1, 100);
	out = rx_loan_region_out();
	orphaned = rx_loan_region_orphaned();
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_EQ(test, a->cr.credit, 11U);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), out + 11);
	svcsock_rx_credit_orphan(test, a);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), out);
	KUNIT_EXPECT_EQ(test, rx_loan_region_orphaned(), orphaned);

	/* Charged before the region, abandoned under it: still none of its. */
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sc, c, len));
	KUNIT_EXPECT_EQ(test, c->cr.region, 0U);
	svcsock_rx_region_fixture(test, 0);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), 0L);
	svcsock_rx_credit_orphan(test, c);
	/* The fixture set aside the total it was charged to. */
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), -11L);
	KUNIT_EXPECT_EQ(test, rx_loan_region_orphaned(), 0L);

	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_EXPECT_EQ(test, b->cr.credit, 11U);
	KUNIT_EXPECT_EQ(test, b->cr.region, 11U);
	svcsock_rx_credit_orphan(test, b);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), 0L);
	KUNIT_EXPECT_EQ(test, rx_loan_region_orphaned(), 11L);
}

/*
 * Keep the test on the CPU it runs on, so the region's per-CPU caches it
 * fills are the ones it takes from.  Each case runs in its own kthread, so
 * the affinity ends with the case.
 */
static int svcsock_rx_region_pin(struct kunit *test)
{
	int cpu = raw_smp_processor_id();

	KUNIT_ASSERT_EQ(test, set_cpus_allowed_ptr(current, cpumask_of(cpu)),
			0);
	return raw_smp_processor_id();
}

static void svcsock_rx_region_expect_part(struct kunit *test, bool read,
					  unsigned long nfree,
					  unsigned long min_free,
					  unsigned long escaped)
{
	unsigned long st[4];

	page_pool_rx_region_kunit_part(read, st);
	KUNIT_EXPECT_EQ(test, st[0], nfree);
	KUNIT_EXPECT_EQ(test, st[1], min_free);
	KUNIT_EXPECT_EQ(test, st[2], 0UL);	/* take never counts alloc_fail */
	KUNIT_EXPECT_EQ(test, st[3], escaped);
}

/*
 * A split region's partitions, taken and given outside the pools: span,
 * all or nothing, most recently freed first, min_free, a page returning to
 * the partition its address lies in, escape and a page from elsewhere.
 */
static void svcsock_rx_region_take_give_test(struct kunit *test)
{
	struct svcsock_rx_region_fixture *f = svcsock_rx_region_fixture(test, 4);
	struct page *pages[SVCSOCK_RX_REGION_PAGES], *stray;
	unsigned long pfn = 0;

	svcsock_rx_region_pin(test);

	KUNIT_EXPECT_EQ(test, page_pool_rx_region_size(), 16UL);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_read_pages(), 4UL);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_span(false, &pfn), 12UL);
	KUNIT_EXPECT_EQ(test, pfn, f->pfn);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_span(true, &pfn), 4UL);
	KUNIT_EXPECT_EQ(test, pfn, f->pfn + 12);

	KUNIT_EXPECT_EQ(test, page_pool_rx_region_take_pages(true, pages, 5), 0U);
	svcsock_rx_region_expect_part(test, true, 4, 4, 0);
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, 4), 4U);
	KUNIT_EXPECT_EQ(test, page_to_pfn(pages[0]), f->pfn + 15);
	KUNIT_EXPECT_EQ(test, page_to_pfn(pages[3]), f->pfn + 12);
	KUNIT_EXPECT_EQ(test, page_ref_count(pages[0]), 1);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_take_pages(true, pages + 4, 1),
			0U);
	svcsock_rx_region_expect_part(test, true, 0, 0, 0);
	svcsock_rx_region_expect_part(test, false, 12, 12, 0);

	/*
	 * One call gives back pages of both partitions, each to its own.  The
	 * write take refilled this CPU's cache with the other ten pages, which
	 * counts as taken from the shard: min_free 0.
	 */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(false, pages + 4, 2),
			2U);
	KUNIT_EXPECT_EQ(test, page_to_pfn(pages[5]), f->pfn + 10);
	page_pool_rx_region_give_pages(pages, 6);
	svcsock_rx_region_expect_part(test, true, 4, 0, 0);
	svcsock_rx_region_expect_part(test, false, 12, 0, 0);

	/*
	 * A double give into a full partition warns, is not stored or put.
	 * Fewer than 32 pages go back through this CPU's cache, so the second
	 * give is caught when the cache drains into the full shard.
	 */
	page_pool_rx_region_kunit_drain(true);
	page_pool_rx_region_kunit_drain(false);
	kunit_warning_suppress(test) {
		page_pool_rx_region_give_pages(pages, 1);
		page_pool_rx_region_kunit_drain(true);
		SVCSOCK_EXPECT_ONCE_WARNING(test);
	}
	svcsock_rx_region_expect_part(test, true, 4, 0, 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(pages[0]), 1);

	/*
	 * The page given last comes back first.  This take also refills the
	 * cache with the other three, so they still count as free.
	 */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, 1), 1U);
	KUNIT_EXPECT_EQ(test, page_to_pfn(pages[0]), f->pfn + 12);

	/*
	 * Allocate it before the escaped page is freed, or the allocator may
	 * hand that region page straight back.
	 */
	stray = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stray);

	/* Still referenced when given: it escapes, and this reference goes. */
	get_page(pages[0]);
	page_pool_rx_region_give_pages(pages, 1);
	svcsock_rx_region_expect_part(test, true, 3, 0, 2);
	KUNIT_EXPECT_EQ(test, page_ref_count(pages[0]), 1);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_capacity(true), 2UL);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_span(true, &pfn), 4UL);
	__set_bit(page_to_pfn(pages[0]) - f->pfn, &f->freed);
	put_page(pages[0]);

	/* A page from outside the region warns, is put and counts nowhere. */
	kunit_warning_suppress(test) {
		page_pool_rx_region_give_pages(&stray, 1);
		SVCSOCK_EXPECT_ONCE_WARNING(test);
	}
	svcsock_rx_region_expect_part(test, true, 3, 0, 2);
	svcsock_rx_region_expect_part(test, false, 12, 0, 0);
}

/*
 * Unsplit, a read consumer draws on the one shared partition, and a request
 * larger than the partition is refused outright.
 */
static void svcsock_rx_region_unsplit_test(struct kunit *test)
{
	struct svcsock_rx_region_fixture *f = svcsock_rx_region_fixture(test, 0);
	struct page *pages[SVCSOCK_RX_REGION_PAGES + 1];
	unsigned long pfn = 0;

	KUNIT_EXPECT_EQ(test, page_pool_rx_region_read_pages(), 0UL);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_span(true, &pfn), 16UL);
	KUNIT_EXPECT_EQ(test, pfn, f->pfn);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_take_pages(true, pages, 17), 0U);
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, 16),
			16U);
	svcsock_rx_region_expect_part(test, false, 0, 0, 0);
	page_pool_rx_region_give_pages(pages, 16);
	svcsock_rx_region_expect_part(test, false, 16, 0, 0);
}

/* The boot region parameters, restored when the test ends. */
struct svcsock_rx_region_params {
	unsigned int pages;
	unsigned int read_pages;
	bool failed;
};

static void svcsock_rx_region_bind_off(void *data)
{
	struct svcsock_rx_region_params *saved = data;

	page_pool_rx_region_kunit_remove(true);
	page_pool_rx_region_kunit_config(saved->pages, saved->read_pages,
					 saved->failed);
}

/* Skip @test unless the page allocator can give @nr contiguous pages now. */
static void svcsock_rx_region_contig_probe(struct kunit *test, unsigned long nr)
{
#ifdef CONFIG_CONTIG_ALLOC
	struct page *base = alloc_contig_pages(nr, GFP_KERNEL, numa_mem_id(),
					       NULL);

	if (!base)
		kunit_skip(test, "no %lu contiguous pages", nr);
	free_contig_range(page_to_pfn(base), nr);
#else
	kunit_skip(test, "needs CONFIG_CONTIG_ALLOC");
#endif
}

/*
 * The read split's floor, and bind_reads() with no device listed: nothing
 * without a size, never retried after a failed allocation, else it
 * allocates the region itself.  That allocation needs the contiguous pages
 * the other contig cases need: on 64K pages a pageblock is 512 MiB, more
 * than a small guest can isolate, so the case skips as they do.
 */
static void svcsock_rx_region_bind_reads_test(struct kunit *test)
{
	struct svcsock_rx_region_params *saved;
	struct page *pages[SVCSOCK_RX_REGION_PAGES];
	struct svcsock_rx_region_params now;
	unsigned long pfn = 0;

	/*
	 * bind_reads() carves whatever size is in force, so a live consumer's
	 * demand would carve the real region here.
	 */
	if (page_pool_rx_region_size())
		kunit_skip(test, "a region is carved or configured already");
	svcsock_rx_region_contig_probe(test, SVCSOCK_RX_REGION_PAGES);
	saved = kunit_kzalloc(test, sizeof(*saved), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, saved);
	page_pool_rx_region_kunit_params(&saved->pages, &saved->read_pages,
					 &saved->failed);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_bind_off, saved), 0);

	page_pool_rx_region_kunit_config(0, 0, false);
	KUNIT_EXPECT_FALSE(test, page_pool_rx_region_bind_reads());

	/*
	 * A split must hold one maximum READ and fit the unsigned int it is
	 * kept in: -EINVAL, unchanged.
	 */
	page_pool_rx_region_kunit_config(2 * PAGE_POOL_RX_REGION_READ_MIN, 0,
					 false);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_read_param(
				PAGE_POOL_RX_REGION_READ_MIN - 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_read_pages(), 0UL);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_read_param(
				PAGE_POOL_RX_REGION_READ_MIN), 0);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_read_pages(),
			PAGE_POOL_RX_REGION_READ_MIN);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_read_param(
				(u64)UINT_MAX + 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_read_pages(),
			PAGE_POOL_RX_REGION_READ_MIN);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_read_param(0), 0);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_read_pages(), 0UL);

	page_pool_rx_region_kunit_config(SVCSOCK_RX_REGION_PAGES, 0, true);
	KUNIT_EXPECT_FALSE(test, page_pool_rx_region_bind_reads());
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_span(false, &pfn), 0UL);

	page_pool_rx_region_kunit_config(SVCSOCK_RX_REGION_PAGES, 0, false);
	KUNIT_ASSERT_TRUE(test, page_pool_rx_region_bind_reads());
	KUNIT_EXPECT_TRUE(test, page_pool_rx_region_bind_reads());
	/* The split is fixed once carved: -EBUSY, the parameter unchanged. */
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_read_param(
				PAGE_POOL_RX_REGION_READ_MIN), -EBUSY);
	page_pool_rx_region_kunit_params(&now.pages, &now.read_pages,
					 &now.failed);
	KUNIT_EXPECT_EQ(test, now.read_pages, 0U);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_read_pages(), 0UL);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_size(), SVCSOCK_RX_REGION_PAGES);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_span(true, &pfn),
			SVCSOCK_RX_REGION_PAGES);
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, 16),
			16U);
	page_pool_rx_region_give_pages(pages, 16);
	svcsock_rx_region_expect_part(test, false, 16, 0, 0);
}

/* Link region credit to a published record, as receive does for region pages. */
static struct svc_tcp_rx_budget *
svcsock_rx_bounce_admit(struct kunit *test, struct svcsock_rx_fixture *fixture,
			u32 reclen)
{
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;

	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(fixture->svsk, state,
							reclen));
	KUNIT_ASSERT_TRUE(test, state->cr.linked);
	/* the shared budget is the first member of sunrpc's */
	return (void *)state->cr.budget;
}

/* @c's stats value for @key, or LONG_MIN when absent. */
static long svcsock_rx_budget_stat(struct svc_tcp_rx_budget *c,
				   const char *key)
{
	char buf[384], needle[32], *at;
	long value;

	if (svc_tcp_rx_budget_kunit_stats(c, buf, sizeof(buf)) <= 0)
		return LONG_MIN;
	snprintf(needle, sizeof(needle), " %s ", key);
	if (!strncmp(buf, needle + 1, strlen(needle + 1))) {
		at = buf + strlen(needle + 1);
	} else {
		at = strstr(buf, needle);
		if (at)
			at += strlen(needle);
	}
	if (!at || sscanf(at, "%ld", &value) != 1)
		return LONG_MIN;
	return value;
}

/* Is @page one of the fixture region's? */
static bool svcsock_rx_in_fixture(const struct svcsock_rx_region_fixture *f,
				  const struct page *page)
{
	return page_to_pfn(page) - f->pfn < SVCSOCK_RX_REGION_PAGES;
}

/*
 * A record holding region credit is copied into a region page and its
 * credit is converted to cover it: one unit is lent until the device has
 * the data, the rest returns with the loan, and bounce_done gives back the
 * page and the lent unit.
 */
static void svcsock_rx_bounce_region_credit_test(struct kunit *test)
{
	enum { BODY_LEN = 7 * XDR_UNIT + 300, OFFSET = 8, LEN = 200, ALIGN = 512 };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	struct svcsock_rx_region_fixture *f;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svc_tcp_rx_budget *c;
	struct bio_vec out[4];
	struct xdr_buf payload;
	long bounced;
	unsigned int refs;
	struct page *page;
	int n;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(40, 10, 100);
	f = svcsock_rx_region_fixture(test, 0);
	page = svcsock_rx_publish_loan(test, fixture, record, BODY_LEN, &refs);
	c = svcsock_rx_bounce_admit(test, fixture, sizeof(record));
	KUNIT_ASSERT_EQ(test, state->cr.credit, 2U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), 28L);
	bounced = svcsock_rx_budget_stat(c, "bounced");
	svcsock_rx_payload_view(&payload, state, OFFSET, LEN);
	/* The region copy starts at the file position's in-page offset. */
	n = svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp, &payload, ALIGN,
			      512, out, ARRAY_SIZE(out));
	KUNIT_ASSERT_EQ(test, n, 1);
	KUNIT_EXPECT_TRUE(test, svcsock_rx_in_fixture(f, out[0].bv_page));
	KUNIT_EXPECT_EQ(test, out[0].bv_offset, (unsigned int)ALIGN);
	KUNIT_EXPECT_EQ(test, out[0].bv_len, (unsigned int)LEN);
	KUNIT_EXPECT_MEMEQ(test, page_address(out[0].bv_page) + ALIGN,
			   record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT + OFFSET,
			   LEN);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
	KUNIT_EXPECT_EQ(test, state->bounce_pages, 1U);
	KUNIT_EXPECT_EQ(test, state->bounce_credit, 1U);
	KUNIT_EXPECT_FALSE(test, state->cr.linked);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), 29L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 1L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "bounced"), bounced + 1);
	svcsock_rx_region_expect_part(test, false, 15, 0, 0);
	svc_tcp_rx_bounce_done(fixture->rqstp, out, n);
	KUNIT_EXPECT_EQ(test, state->bounce_pages, 0U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), 30L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 0L);
	svcsock_rx_region_expect_part(test, false, 16, 0, 0);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
}

static void svcsock_rx_consumer_off(void *unused)
{
	svc_tcp_rx_kunit_consumer_stop();
}

/*
 * A case that takes the initial namespace's consumer skips first while a
 * region is carved or configured: a running server's floor is demand that
 * sizes the region, and re-binding or stopping its consumer would take
 * the floor and its threads from it.
 */
static void svcsock_rx_skip_live_consumer(struct kunit *test)
{
	if (page_pool_rx_region_size())
		kunit_skip(test, "a region is carved or configured already");
}

/*
 * The initial namespace's consumer, as a server of @nrthreads starting
 * registers it, stopped at the case's end so that no demand outlives the
 * case.
 */
static struct rx_loan_consumer *
svcsock_rx_own_consumer(struct kunit *test, unsigned int nrthreads,
			unsigned int max_payload)
{
	struct rx_loan_consumer *rc;

	rc = svc_tcp_rx_kunit_consumer(nrthreads, max_payload);
	if (!rc)
		kunit_skip(test, "no initial namespace consumer");
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_consumer_off, NULL), 0);
	return rc;
}

/*
 * The credit a record lends a bounce stays charged to the record's
 * consumer until bounce_done returns it: the record's return refunds the
 * consumer only the rest.
 */
static void svcsock_rx_bounce_consumer_test(struct kunit *test)
{
	enum { BODY_LEN = 7 * XDR_UNIT + 300, OFFSET = 8, LEN = 200, ALIGN = 512 };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct rx_loan_consumer *rc;
	struct bio_vec out[4];
	struct xdr_buf payload;
	unsigned int refs;
	int n;

	svcsock_rx_skip_live_consumer(test);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(40, 10, 100);
	svcsock_rx_region_fixture(test, 0);
	svcsock_rx_publish_loan(test, fixture, record, BODY_LEN, &refs);
	rc = svcsock_rx_own_consumer(test, 1, PAGE_SIZE);
	svcsock_rx_bounce_admit(test, fixture, sizeof(record));
	KUNIT_ASSERT_EQ(test, state->cr.credit, 2U);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&rc->out_write), 2L);
	svcsock_rx_payload_view(&payload, state, OFFSET, LEN);
	n = svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp, &payload, ALIGN,
			      512, out, ARRAY_SIZE(out));
	KUNIT_ASSERT_EQ(test, n, 1);
	KUNIT_ASSERT_EQ(test, state->bounce_credit + state->bounce_reserve, 1U);
	KUNIT_EXPECT_FALSE(test, state->cr.linked);
	KUNIT_EXPECT_PTR_EQ(test, state->bounce_consumer, rc);
	/* The record returned; the lent unit is still the consumer's. */
	KUNIT_EXPECT_EQ(test, atomic_long_read(&rc->out_write), 1L);
	svc_tcp_rx_bounce_done(fixture->rqstp, out, n);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&rc->out_write), 0L);
	KUNIT_EXPECT_NULL(test, state->bounce_consumer);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
}

/*
 * A three-entry loan (head page and two frags) whose payload starts inside
 * the first entry and ends inside the last, bounced at an in-page offset
 * (3500 on 4 KiB pages; every size scales with the page) so it spans three
 * region pages: every byte lands in order, the two consumed loaned pages
 * return during the copy and the straddling one with the rest of the loan,
 * the record's three credits are lent and come back on done.
 */
static void svcsock_rx_bounce_region_multi_test(struct kunit *test)
{
	enum {
		S = PAGE_SIZE / SZ_4K,
		HEAD = 1000 * S, FRAG0 = 3000 * S, FRAG1 = 2000 * S,
		BODY_LEN = 7 * XDR_UNIT + HEAD + FRAG0 + FRAG1,
		RECORD_LEN = sizeof(rpc_fraghdr) + BODY_LEN,
		OFFSET = 200 * S, LEN = HEAD - OFFSET + FRAG0 + 1000 * S,
		ALIGN = 3500 * S,
	};
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 8);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 *record = kunit_kzalloc(test, RECORD_LEN, GFP_KERNEL);
	u8 *copy = kunit_kzalloc(test, LEN, GFP_KERNEL);
	const unsigned int head_len = sizeof(rpc_fraghdr) + 7 * XDR_UNIT + HEAD;
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = RECORD_LEN };
	unsigned int refs[3], i, cursor = 0;
	struct svcsock_rx_region_fixture *f;
	struct svc_tcp_rx_budget *c;
	struct page *pages[3];
	struct bio_vec out[8];
	struct xdr_buf payload;
	struct sk_buff *skb;
	long avail, bounced;
	int n, ret;

	KUNIT_ASSERT_NOT_NULL(test, record);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(40, 10, 100);
	f = svcsock_rx_region_fixture(test, 0);
	fixture->svsk->sk_xprt.xpt_net = &init_net;
	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_page_skb(test, record, head_len, &pages[0],
					&refs[0]);
	pages[1] = svcsock_rx_add_frag(test, skb, 0, record + head_len, FRAG0,
				       0, &refs[1]);
	pages[2] = svcsock_rx_add_frag(test, skb, 1, record + head_len + FRAG0,
				       FRAG1, 0, &refs[2]);
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0,
				   RECORD_LEN);
	KUNIT_ASSERT_EQ(test, ret, (int)RECORD_LEN);
	KUNIT_ASSERT_TRUE(test, actor.complete);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_publish(fixture->svsk, fixture->rqstp),
			0);
	kunit_kfree_skb(test, skb);
	KUNIT_ASSERT_EQ(test, state->lt.count, 3U);
	c = svcsock_rx_bounce_admit(test, fixture, RECORD_LEN);
	KUNIT_ASSERT_EQ(test, state->cr.credit, 3U);
	avail = svcsock_rx_budget_stat(c, "avail");
	bounced = svcsock_rx_budget_stat(c, "bounced");

	svcsock_rx_payload_view(&payload, state, OFFSET, LEN);
	payload.bvec_count = state->lt.count;
	n = svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp, &payload, ALIGN,
			      512, out, ARRAY_SIZE(out));
	KUNIT_ASSERT_EQ(test, n, 3);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_TRUE(test, svcsock_rx_in_fixture(f, out[i].bv_page));
	KUNIT_EXPECT_EQ(test, out[0].bv_offset, (unsigned int)ALIGN);
	KUNIT_EXPECT_EQ(test, out[0].bv_len, (unsigned int)(PAGE_SIZE - ALIGN));
	KUNIT_EXPECT_EQ(test, out[1].bv_offset, 0U);
	KUNIT_EXPECT_EQ(test, out[1].bv_len, (unsigned int)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, out[2].bv_len,
			(unsigned int)(ALIGN + LEN - 2 * PAGE_SIZE));
	for (i = 0; i < 3; i++) {
		memcpy_from_page(copy + cursor, out[i].bv_page,
				 out[i].bv_offset, out[i].bv_len);
		cursor += out[i].bv_len;
	}
	KUNIT_EXPECT_EQ(test, cursor, (unsigned int)LEN);
	KUNIT_EXPECT_MEMEQ(test, copy,
			   record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT + OFFSET,
			   LEN);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), refs[i] - 1);
	KUNIT_EXPECT_EQ(test, state->lt.refs_released, state->lt.refs_acquired);
	KUNIT_EXPECT_TRUE(test, state->terminal);
	KUNIT_EXPECT_NE(test, state->reason, (u8)SVC_TCP_RX_REASON_INVARIANT);
	KUNIT_EXPECT_EQ(test, state->bounce_pages, 3U);
	KUNIT_EXPECT_EQ(test, state->bounce_credit, 3U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), avail);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 3L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "bounced"), bounced + 1);
	svcsock_rx_region_expect_part(test, false, 13, 0, 0);
	svc_tcp_rx_bounce_done(fixture->rqstp, out, n);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), avail + 3);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 0L);
	svcsock_rx_region_expect_part(test, false, 16, 0, 0);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
}

/*
 * A record whose credit cannot cover the bounce (a one-page budget with no
 * reserve gives the oldest record one credit: with nothing learned it is
 * charged all it could hold, cut to what the budget has; an in-page offset
 * of PAGE_SIZE - 96 puts the 200 bytes on two pages) refuses without
 * touching @out, the loan, the region or the credit.
 */
static void svcsock_rx_bounce_credit_short_test(struct kunit *test)
{
	enum {
		BODY_LEN = 7 * XDR_UNIT + 300, OFFSET = 8, LEN = 200,
		ALIGN = PAGE_SIZE - 96,
	};
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct page *marker = fixture->rqstp->rq_pages[3];
	struct svc_tcp_rx_budget *c;
	struct bio_vec out[4];
	struct xdr_buf payload;
	unsigned int refs, i;
	struct page *page;
	long avail, refused;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(1, 0, 0);
	svcsock_rx_region_fixture(test, 0);
	page = svcsock_rx_publish_loan(test, fixture, record, BODY_LEN, &refs);
	c = svcsock_rx_bounce_admit(test, fixture, sizeof(record));
	KUNIT_ASSERT_EQ(test, state->cr.credit, 1U);
	avail = svcsock_rx_budget_stat(c, "avail");
	refused = svcsock_rx_budget_stat(c, "bounce_refused");
	for (i = 0; i < ARRAY_SIZE(out); i++)
		bvec_set_page(&out[i], marker, 1, 0);
	svcsock_rx_payload_view(&payload, state, OFFSET, LEN);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp,
						&payload, ALIGN, 4, out,
						ARRAY_SIZE(out)),
			-ENOSPC);
	for (i = 0; i < ARRAY_SIZE(out); i++)
		KUNIT_EXPECT_PTR_EQ(test, out[i].bv_page, marker);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	KUNIT_EXPECT_FALSE(test, state->terminal);
	KUNIT_EXPECT_EQ(test, state->cr.credit, 1U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), avail);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "bounce_refused"),
			refused + 1);
	svcsock_rx_region_expect_part(test, false, 16, 16, 0);
	svc_tcp_rx_release_payload(fixture->svsk, fixture->rqstp);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
}

/*
 * A write partition short of pages (one left, for a bounce straddling two)
 * refuses before any credit is converted: nothing changes.
 */
static void svcsock_rx_bounce_region_short_test(struct kunit *test)
{
	enum {
		BODY_LEN = 7 * XDR_UNIT + 300, OFFSET = 8, LEN = 200,
		ALIGN = PAGE_SIZE - 96,
		HELD = SVCSOCK_RX_REGION_PAGES - 1,
	};
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct page *marker = fixture->rqstp->rq_pages[3];
	struct page *held[SVCSOCK_RX_REGION_PAGES];
	struct svc_tcp_rx_budget *c;
	struct bio_vec out[4];
	struct xdr_buf payload;
	unsigned int refs, i;
	struct page *page;
	long avail, refused;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(40, 10, 100);
	svcsock_rx_region_fixture(test, 0);
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(false, held, HELD),
			(unsigned int)HELD);
	page = svcsock_rx_publish_loan(test, fixture, record, BODY_LEN, &refs);
	c = svcsock_rx_bounce_admit(test, fixture, sizeof(record));
	KUNIT_ASSERT_EQ(test, state->cr.credit, 2U);
	avail = svcsock_rx_budget_stat(c, "avail");
	refused = svcsock_rx_budget_stat(c, "bounce_refused");
	for (i = 0; i < ARRAY_SIZE(out); i++)
		bvec_set_page(&out[i], marker, 1, 0);
	svcsock_rx_payload_view(&payload, state, OFFSET, LEN);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp,
						&payload, ALIGN, 4, out,
						ARRAY_SIZE(out)),
			-ENOSPC);
	/* take_pages() wrote nothing on failure */
	for (i = 0; i < ARRAY_SIZE(out); i++)
		KUNIT_EXPECT_PTR_EQ(test, out[i].bv_page, marker);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	KUNIT_EXPECT_EQ(test, state->cr.credit, 2U);
	KUNIT_EXPECT_TRUE(test, state->cr.linked);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), avail);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "bounce_refused"),
			refused + 1);
	svcsock_rx_region_expect_part(test, false, 1, 1, 0);
	page_pool_rx_region_give_pages(held, HELD);
	svc_tcp_rx_release_payload(fixture->svsk, fixture->rqstp);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
}

/*
 * A locked skb head between two loaned pages is copied as its own arena
 * bvec (the sliver), and only its bytes count as copied.  Bouncing over the
 * loan, sliver, loan table must leave the bookkeeping valid: everything
 * released, nothing flagged.
 */
static void svcsock_rx_bounce_locked_head_test(struct kunit *test)
{
	enum {
		HEAD = 200, LOCKED = 100, TAIL = 300,
		BODY_LEN = 7 * XDR_UNIT + HEAD + LOCKED + TAIL,
		RECORD_LEN = sizeof(rpc_fraghdr) + BODY_LEN,
	};
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 8);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	const unsigned int head_len = sizeof(rpc_fraghdr) + 7 * XDR_UNIT + HEAD;
	u8 record[RECORD_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = RECORD_LEN };
	struct sk_buff *root, *locked, *tail;
	struct page *root_page, *tail_page;
	unsigned int root_refs, tail_refs;
	struct svcsock_rx_region_fixture *f;
	struct svc_tcp_rx_budget *c;
	struct bio_vec out[8];
	struct xdr_buf payload;
	int n, ret;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(40, 10, 100);
	f = svcsock_rx_region_fixture(test, 0);
	fixture->svsk->sk_xprt.xpt_net = &init_net;
	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	root = svcsock_rx_build_page_skb(test, record, head_len, &root_page,
					 &root_refs);
	locked = svcsock_rx_build_linear_skb(test, record + head_len, LOCKED);
	tail = svcsock_rx_build_page_skb(test, record + head_len + LOCKED, TAIL,
					 &tail_page, &tail_refs);
	svcsock_rx_attach_frag_list(test, locked, tail);
	svcsock_rx_attach_frag_list(test, root, locked);
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, root, 0,
				   RECORD_LEN);
	KUNIT_ASSERT_EQ(test, ret, (int)RECORD_LEN);
	KUNIT_ASSERT_TRUE(test, actor.complete);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_publish(fixture->svsk, fixture->rqstp),
			0);
	KUNIT_ASSERT_EQ(test, state->mode, (u8)SVC_TCP_RX_PUBLISHED);
	KUNIT_ASSERT_GT(test, state->lt.count, 0U);
	/* Loan, the locked head's sliver in the arena, loan. */
	KUNIT_ASSERT_EQ(test, state->lt.count, 3U);
	KUNIT_ASSERT_EQ(test, state->lt.copied_bytes, (u32)LOCKED);
	KUNIT_ASSERT_EQ(test, state->lt.borrowed_bytes, (u32)(HEAD + TAIL));
	KUNIT_ASSERT_EQ(test, state->lt.borrowed_count, 2U);
	kunit_kfree_skb(test, root);
	c = svcsock_rx_bounce_admit(test, fixture, RECORD_LEN);

	payload = (struct xdr_buf){
		.page_mode = XDRBUF_PAGE_BVECS, .bvec = state->lt.bvec,
		.bvec_count = state->lt.count, .page_len = HEAD + LOCKED + TAIL,
		.len = HEAD + LOCKED + TAIL,
	};
	n = svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp, &payload, 0, 512,
			      out, ARRAY_SIZE(out));
	KUNIT_ASSERT_EQ(test, n, 1);
	KUNIT_EXPECT_TRUE(test, svcsock_rx_in_fixture(f, out[0].bv_page));
	KUNIT_EXPECT_EQ(test, out[0].bv_offset, 0U);
	KUNIT_EXPECT_EQ(test, out[0].bv_len, (unsigned int)(HEAD + LOCKED + TAIL));
	KUNIT_EXPECT_EQ(test, state->bounce_pages, 1U);
	KUNIT_EXPECT_MEMEQ(test, page_address(out[0].bv_page),
			   record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT,
			   HEAD + LOCKED + TAIL);
	KUNIT_EXPECT_TRUE(test, state->terminal);
	KUNIT_EXPECT_EQ(test, state->lt.refs_released, state->lt.refs_acquired);
	KUNIT_EXPECT_EQ(test, state->lt.borrowed_count, 0U);
	KUNIT_EXPECT_NE(test, state->reason, (u8)SVC_TCP_RX_REASON_INVARIANT);
	KUNIT_EXPECT_EQ(test, page_ref_count(root_page), root_refs - 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(tail_page), tail_refs - 1);
	svc_tcp_rx_bounce_done(fixture->rqstp, out, n);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 0L);
	svcsock_rx_region_expect_part(test, false, 16, 0, 0);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
}

/*
 * The oldest record admitted partly from the reserve converts reserve credit
 * too, and gives it back to the reserve.  A budget changed while a bounce
 * holds credit, with no record in flight, applies at once by the change, as
 * a live reconfiguration does for records in flight, and the
 * budget is whole once the bounce is done.
 */
static void svcsock_rx_bounce_reserve_test(struct kunit *test)
{
	/* The bounce straddles two pages at any page size. */
	enum {
		BODY_LEN = 7 * XDR_UNIT + 300, OFFSET = 8, LEN = 200,
		ALIGN = PAGE_SIZE - 96,
	};
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svcsock_rx_fixture *other = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svc_tcp_rx_budget *c;
	struct bio_vec out[4];
	struct xdr_buf payload;
	unsigned int refs;
	int n;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(3, 2, 100);
	svcsock_rx_region_fixture(test, 0);
	svcsock_rx_publish_loan(test, fixture, record, BODY_LEN, &refs);
	c = svcsock_rx_bounce_admit(test, fixture, sizeof(record));
	KUNIT_ASSERT_EQ(test, state->cr.credit, 2U);
	KUNIT_ASSERT_EQ(test, state->cr.reserve, 1U);
	svcsock_rx_payload_view(&payload, state, OFFSET, LEN);
	n = svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp, &payload, ALIGN, 4,
			      out, ARRAY_SIZE(out));
	KUNIT_ASSERT_EQ(test, n, 2);
	KUNIT_EXPECT_EQ(test, state->bounce_credit, 1U);
	KUNIT_EXPECT_EQ(test, state->bounce_reserve, 1U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "reserve"), 1L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 2L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "inflight"), 0L);

	/* A new budget and a new admission while the bounce holds credit. */
	svc_tcp_rx_budget_kunit_configure(5, 2, 100);
	svcsock_rx_publish_loan(test, other, record, BODY_LEN, &refs);
	svcsock_rx_bounce_admit(test, other, sizeof(record));
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "budget"), 5L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), 0L);
	svc_tcp_rx_release_payload(other->svsk, other->rqstp);
	svc_tcp_release_ctxt(&other->svsk->sk_xprt, other->rqstp->rq_tcp_rx);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "inflight"), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), 2L);

	svc_tcp_rx_bounce_done(fixture->rqstp, out, n);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "budget"), 5L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), 3L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "reserve"), 2L);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
}

/* Common set-up of the refusal and credit cases: one loan, credit admitted. */
struct svcsock_rx_bounce_env {
	struct svcsock_rx_fixture *fixture;
	struct svc_tcp_rx_state *state;
	struct svc_tcp_rx_budget *c;
	struct xdr_buf payload;
	struct page *page;
	unsigned int refs;
	u8 record[sizeof(rpc_fraghdr) + 7 * XDR_UNIT + 300];
};

enum { BOUNCE_BODY = 7 * XDR_UNIT + 300, BOUNCE_OFFSET = 8, BOUNCE_LEN = 200,
       BOUNCE_ALIGN2 = PAGE_SIZE - 96 };

static struct svcsock_rx_bounce_env *
svcsock_rx_bounce_env(struct kunit *test, long budget, long reserve, long factor)
{
	struct svcsock_rx_bounce_env *env = kunit_kzalloc(test, sizeof(*env),
							  GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, env);
	env->fixture = svcsock_rx_alloc_fixture(test, 4);
	env->state = env->fixture->rqstp->rq_tcp_rx;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(budget, reserve, factor);
	svcsock_rx_region_fixture(test, 0);
	env->page = svcsock_rx_publish_loan(test, env->fixture, env->record,
					    BOUNCE_BODY, &env->refs);
	env->c = svcsock_rx_bounce_admit(test, env->fixture,
					 sizeof(env->record));
	svcsock_rx_payload_view(&env->payload, env->state, BOUNCE_OFFSET,
				BOUNCE_LEN);
	return env;
}

static void svcsock_rx_bounce_env_end(struct svcsock_rx_bounce_env *env)
{
	svc_tcp_rx_release_payload(env->fixture->svsk, env->fixture->rqstp);
	svc_tcp_release_ctxt(&env->fixture->svsk->sk_xprt, env->state);
}

/*
 * More pages than @out holds: a capacity refusal, counted, with the loan,
 * the region and the credit untouched.
 */
static void svcsock_rx_bounce_capacity_test(struct kunit *test)
{
	struct svcsock_rx_bounce_env *env = svcsock_rx_bounce_env(test, 40, 10, 100);
	struct bio_vec out[1];
	long avail = svcsock_rx_budget_stat(env->c, "avail");
	long refused = svcsock_rx_budget_stat(env->c, "bounce_refused");

	/* Two pages are needed at this in-page offset. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_bounce(env->fixture->svsk,
						env->fixture->rqstp,
						&env->payload, BOUNCE_ALIGN2, 4,
						out, ARRAY_SIZE(out)), -ENOSPC);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "bounce_refused"),
			refused + 1);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "avail"), avail);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "lent"), 0L);
	KUNIT_EXPECT_EQ(test, env->state->cr.credit, 2U);
	KUNIT_EXPECT_TRUE(test, env->state->cr.linked);
	KUNIT_EXPECT_EQ(test, page_ref_count(env->page), env->refs);
	svcsock_rx_region_expect_part(test, false, 16, 16, 0);
	svcsock_rx_bounce_env_end(env);
}

/*
 * A payload a later COMPOUND operation may still read, and a deferred
 * request, are not bounced: -EOPNOTSUPP, the loan, the credit and the
 * counters untouched.
 */
static void svcsock_rx_bounce_unsupported_test(struct kunit *test)
{
	struct svcsock_rx_bounce_env *env = svcsock_rx_bounce_env(test, 40, 10, 100);
	struct svc_rqst *rqstp = env->fixture->rqstp;
	struct svc_deferred_req *deferred = kunit_kzalloc(test, sizeof(*deferred),
							  GFP_KERNEL);
	struct bio_vec out[4];
	long refused = svcsock_rx_budget_stat(env->c, "bounce_refused");
	long bounced = svcsock_rx_budget_stat(env->c, "bounced");

	KUNIT_ASSERT_NOT_NULL(test, deferred);
	set_bit(RQ_PAYLOAD_SHARED, &rqstp->rq_flags);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_bounce(env->fixture->svsk, rqstp,
						&env->payload, 512, 512, out,
						ARRAY_SIZE(out)), -EOPNOTSUPP);
	clear_bit(RQ_PAYLOAD_SHARED, &rqstp->rq_flags);
	rqstp->rq_deferred = deferred;
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_bounce(env->fixture->svsk, rqstp,
						&env->payload, 512, 512, out,
						ARRAY_SIZE(out)), -EOPNOTSUPP);
	rqstp->rq_deferred = NULL;
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "bounce_refused"),
			refused);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "bounced"), bounced);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "lent"), 0L);
	KUNIT_EXPECT_EQ(test, env->state->cr.credit, 2U);
	KUNIT_EXPECT_TRUE(test, env->state->cr.linked);
	KUNIT_EXPECT_EQ(test, env->state->bounce_pages, 0U);
	KUNIT_EXPECT_EQ(test, page_ref_count(env->page), env->refs);
	svcsock_rx_bounce_env_end(env);
}

/*
 * Credit drawn past the budget (credit_over) is not credit: with one real
 * unit and five past the budget the record holds six, but a two-page
 * bounce refuses "credit" and changes nothing.
 */
static void svcsock_rx_bounce_credit_over_short_test(struct kunit *test)
{
	/* One-page budget, no reserve: the oldest record holds one unit. */
	struct svcsock_rx_bounce_env *env = svcsock_rx_bounce_env(test, 1, 0, 0);
	struct bio_vec out[4];
	long refused = svcsock_rx_budget_stat(env->c, "bounce_refused");

	KUNIT_ASSERT_EQ(test, env->state->cr.credit, 1U);
	/* Admission off: further charges finish unthrottled, as credit_over. */
	svc_tcp_rx_budget_kunit_configure(0, 0, 0);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_charge(env->fixture->svsk,
							 env->state, 6));
	KUNIT_ASSERT_EQ(test, env->state->cr.credit, 6U);
	KUNIT_ASSERT_EQ(test, env->state->cr.over, 5U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_bounce(env->fixture->svsk,
						env->fixture->rqstp,
						&env->payload, BOUNCE_ALIGN2, 4,
						out, ARRAY_SIZE(out)), -ENOSPC);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "bounce_refused"),
			refused + 1);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "lent"), 0L);
	KUNIT_EXPECT_EQ(test, env->state->cr.credit, 6U);
	KUNIT_EXPECT_EQ(test, env->state->cr.over, 5U);
	KUNIT_EXPECT_TRUE(test, env->state->cr.linked);
	svcsock_rx_region_expect_part(test, false, 16, 16, 0);
	svcsock_rx_bounce_env_end(env);
}

/*
 * A record with credit past the budget converts only its real credit
 * (here the record's two units), and the credit past the budget is never
 * returned to the budget: done returns exactly the two lent units.
 */
static void svcsock_rx_bounce_credit_over_test(struct kunit *test)
{
	struct svcsock_rx_bounce_env *env = svcsock_rx_bounce_env(test, 40, 10, 100);
	struct bio_vec out[4];
	long avail;
	int n;

	KUNIT_ASSERT_EQ(test, env->state->cr.credit, 2U);
	svc_tcp_rx_budget_kunit_configure(0, 0, 0);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_charge(env->fixture->svsk,
							 env->state, 5));
	KUNIT_ASSERT_EQ(test, env->state->cr.over, 3U);
	svc_tcp_rx_budget_kunit_configure(40, 10, 100);
	n = svc_tcp_rx_bounce(env->fixture->svsk, env->fixture->rqstp,
			      &env->payload, BOUNCE_ALIGN2, 4, out,
			      ARRAY_SIZE(out));
	KUNIT_ASSERT_EQ(test, n, 2);
	KUNIT_EXPECT_EQ(test, env->state->bounce_credit +
			      env->state->bounce_reserve, 2U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "lent"), 2L);
	avail = svcsock_rx_budget_stat(env->c, "avail");
	svc_tcp_rx_bounce_done(env->fixture->rqstp, out, n);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "lent"), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "avail"),
			avail + 2);
	svc_tcp_release_ctxt(&env->fixture->svsk->sk_xprt, env->state);
}

/*
 * A budget turned off while credit is lent: done brings the lent credit to
 * zero and leaves nothing available, the budget being off.
 */
static void svcsock_rx_bounce_budget_off_test(struct kunit *test)
{
	struct svcsock_rx_bounce_env *env = svcsock_rx_bounce_env(test, 40, 10, 100);
	struct bio_vec out[4];
	int n;

	n = svc_tcp_rx_bounce(env->fixture->svsk, env->fixture->rqstp,
			      &env->payload, BOUNCE_ALIGN2, 4, out,
			      ARRAY_SIZE(out));
	KUNIT_ASSERT_EQ(test, n, 2);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "lent"), 2L);
	svc_tcp_rx_budget_kunit_configure(0, 0, 0);
	svc_tcp_rx_bounce_done(env->fixture->rqstp, out, n);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "lent"), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "avail"), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(env->c, "budget"), 0L);
	svcsock_rx_region_expect_part(test, false, 16, 0, 0);
	svc_tcp_release_ctxt(&env->fixture->svsk->sk_xprt, env->state);
}

#ifdef CONFIG_CONTIG_ALLOC
static void svcsock_rx_region_contig_off(void *data)
{
	page_pool_rx_region_kunit_remove(true);
}
#endif

/*
 * Publish a region over @nr contiguous pages of the page allocator, @nr_read
 * of them split off for reads; removed (and the pages freed) when the case
 * ends.  Return its first pfn.
 */
static unsigned long svcsock_rx_region_contig(struct kunit *test,
					      unsigned long nr,
					      unsigned long nr_read)
{
#ifdef CONFIG_CONTIG_ALLOC
	unsigned long pfn = 0;
	struct page *base;
	int ret;

	if (page_pool_rx_region_span(false, &pfn))
		kunit_skip(test, "a fixed rx region already exists");
	base = alloc_contig_pages(nr, GFP_KERNEL, numa_mem_id(), NULL);
	if (!base)
		kunit_skip(test, "no %lu contiguous pages", nr);
	ret = page_pool_rx_region_kunit_install(base, nr, nr_read);
	if (ret)
		free_contig_range(page_to_pfn(base), nr);
	if (ret == -EBUSY)
		kunit_skip(test, "a fixed rx region already exists");
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_contig_off, NULL), 0);
	return page_to_pfn(base);
#else
	kunit_skip(test, "needs CONFIG_CONTIG_ALLOC");
	return 0;
#endif
}

/* The shard count a partition of @nr pages gets. */
static unsigned int svcsock_rx_region_nshards(unsigned long nr)
{
	return clamp_t(unsigned long, nr / 1024, 1,
		       min_t(unsigned int, 16, nr_cpu_ids));
}

/* A partition's free pages (shards and caches); its cached pages in @cached. */
static unsigned long svcsock_rx_region_free(bool read, unsigned long *cached,
					    unsigned int *maxcache,
					    unsigned int *nshards)
{
	unsigned long cap;
	unsigned int foreign;

	return page_pool_rx_region_kunit_shards(read, -1, &cap, &foreign,
						cached, maxcache, nshards);
}

/* Every page in each shard is one the shard owns; @full: and none missing. */
static void svcsock_rx_region_shards_own(struct kunit *test, bool read,
					 bool full)
{
	unsigned long cap, cached, nfree;
	unsigned int i, foreign, maxcache, nshards;

	svcsock_rx_region_free(read, &cached, &maxcache, &nshards);
	for (i = 0; i < nshards; i++) {
		nfree = page_pool_rx_region_kunit_shards(read, i, &cap,
							 &foreign, &cached,
							 &maxcache, &nshards);
		KUNIT_EXPECT_EQ(test, foreign, 0U);
		if (full)
			KUNIT_EXPECT_EQ(test, nfree, cap);
	}
}

/* @n distinct pages, filling a pfn range of @n. */
static bool svcsock_rx_region_distinct(struct page **pages, unsigned long n)
{
	unsigned long lo = ULONG_MAX, i, *seen;
	bool ok = true;

	for (i = 0; i < n; i++)
		lo = min(lo, page_to_pfn(pages[i]));
	seen = bitmap_zalloc(n, GFP_KERNEL);
	if (!seen)
		return false;
	for (i = 0; i < n && ok; i++) {
		unsigned long off = page_to_pfn(pages[i]) - lo;

		ok = off < n && !test_and_set_bit(off, seen);
	}
	bitmap_free(seen);
	return ok;
}

struct svcsock_rx_region_give {
	struct page **pages;
	unsigned int n;
};

/* Give pages back one at a time on another CPU, into its cache. */
static long svcsock_rx_region_give_fn(void *arg)
{
	struct svcsock_rx_region_give *g = arg;
	unsigned int i;

	for (i = 0; i < g->n; i++)
		page_pool_rx_region_give_pages(&g->pages[i], 1);
	return 0;
}

#define SVCSOCK_RX_REGION_BIG	2048UL

/*
 * A partition's per-CPU caches: cached pages count as free, a cache refills
 * by a batch of 32 and holds at most 64, spilling a batch when full (or
 * everything when its home shard is below an eighth); it is not refilled
 * when the shards are down to an eighth of the partition; a taker short of
 * pages drains every CPU's cache before it fails, and a device refill (not
 * exact) drains at most once a jiffy.
 */
static void svcsock_rx_region_cache_test(struct kunit *test)
{
	unsigned long nr = SVCSOCK_RX_REGION_BIG, cached, i;
	unsigned int maxcache, nshards, got, tries;
	struct svcsock_rx_region_give g;
	struct page **pages;
	int cpu;

	svcsock_rx_region_contig(test, nr, 0);
	cpu = svcsock_rx_region_pin(test);
	pages = kunit_kcalloc(test, nr + 1, sizeof(*pages), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pages);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), nr);
	KUNIT_EXPECT_EQ(test, cached, 0UL);
	KUNIT_EXPECT_EQ(test, nshards, svcsock_rx_region_nshards(nr));

	/* Single pages come through this CPU's cache, refilled 32 at a time. */
	for (i = 0; i < 100; i++)
		KUNIT_ASSERT_EQ(test,
				page_pool_rx_region_take_pages(true, &pages[i], 1),
				1U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), nr - 100);
	KUNIT_EXPECT_EQ(test, cached, 32UL);
	for (i = 0; i < 100; i++)
		page_pool_rx_region_give_pages(&pages[i], 1);
	/* 32 + 100 given: spilled a batch at 64 three times. */
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), nr);
	KUNIT_EXPECT_EQ(test, cached, 36UL);
	KUNIT_EXPECT_LE(test, maxcache, 64U);

	/* Beyond the partition: refused outright, nothing drained. */
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_take_pages(true, pages, nr + 1),
			0U);
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, cached, 36UL);

	/* One page too many: none taken, none lost, and the caches drained. */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages,
							     nr - 100),
			(unsigned int)(nr - 100));
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_take_pages(true, pages + nr - 100,
							     101), 0U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), 100UL);
	KUNIT_EXPECT_EQ(test, cached, 0UL);
	page_pool_rx_region_give_pages(pages, nr - 100);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), nr);
	KUNIT_EXPECT_EQ(test, cached, 0UL);

	/* Pages left in another CPU's cache: a taker short drains it. */
	g.pages = pages;
	g.n = 40;
	if (cpumask_any_but(cpu_online_mask, cpu) < nr_cpu_ids) {
		KUNIT_ASSERT_EQ(test,
				page_pool_rx_region_take_pages(true, pages, 40),
				40U);
		work_on_cpu(cpumask_any_but(cpu_online_mask, cpu),
			    svcsock_rx_region_give_fn, &g);
		svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
		KUNIT_EXPECT_EQ(test, cached, 40UL);
	} else {
		kunit_mark_skipped(test, "one CPU: no other CPU's cache to drain");
	}
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, nr),
			(unsigned int)nr);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), 0UL);
	KUNIT_EXPECT_TRUE(test, svcsock_rx_region_distinct(pages, nr));

	/* Full with its home shard empty: the cache spills everything. */
	for (i = 0; i < 64; i++)
		page_pool_rx_region_give_pages(&pages[i], 1);
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, cached, 64UL);
	page_pool_rx_region_give_pages(&pages[64], 1);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), 65UL);
	KUNIT_EXPECT_EQ(test, cached, 1UL);

	/* With the shards refilled, a full cache spills one batch of 32. */
	page_pool_rx_region_give_pages(&pages[65], nr - 64 - 65);
	for (i = nr - 64; i < nr; i++)
		page_pool_rx_region_give_pages(&pages[i], 1);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), nr);
	KUNIT_EXPECT_EQ(test, cached, 33UL);
	svcsock_rx_region_shards_own(test, false, false);
	page_pool_rx_region_kunit_drain(false);
	svcsock_rx_region_shards_own(test, false, true);

	/* No refill once the shards are down to an eighth of the partition. */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages,
							     nr - 200),
			(unsigned int)(nr - 200));
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true,
							     &pages[nr - 1], 1),
			1U);
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, cached, 0UL);
	page_pool_rx_region_give_pages(pages, nr - 200);

	/*
	 * A device refill finding nothing drains once a jiffy.  Leave every
	 * page taken but one, cached here; the first refill in a fresh jiffy
	 * drains and finds it, a second in the same jiffy does not drain.
	 */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, nr - 1),
			(unsigned int)(nr - 1));
	for (tries = 0; tries < 5; tries++) {
		unsigned long j;

		schedule_timeout_uninterruptible(1);
		page_pool_rx_region_give_pages(&pages[nr - 1], 1);
		j = jiffies;
		got = page_pool_rx_region_kunit_take(true, &pages[nr - 1], 64,
						     false);
		KUNIT_ASSERT_EQ(test, got, 1U);
		page_pool_rx_region_give_pages(&pages[nr - 1], 1);
		got = page_pool_rx_region_kunit_take(true, &pages[nr - 1], 64,
						     false);
		if (!got)
			break;
		/* A tick passed between the two: try again. */
		KUNIT_EXPECT_NE(test, jiffies, j);
	}
	KUNIT_EXPECT_LT(test, tries, 5U);
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, cached, 1UL);
	page_pool_rx_region_give_pages(pages, nr - 1);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), nr);
}

/* Shard @want's capacity in a partition of @nr pages over @nshards shards. */
static unsigned long svcsock_rx_region_cap(unsigned long nr,
					   unsigned int nshards,
					   unsigned int want)
{
	unsigned long cap = DIV_ROUND_UP(nr, nshards);

	return min(cap, nr - min(nr, want * cap));
}

/* Install @nr pages unsplit and check its shard count and capacities. */
static void svcsock_rx_region_expect_layout(struct kunit *test,
					    unsigned long nr)
{
	unsigned int i, foreign, maxcache, nshards, want;
	unsigned long cap, cached;

	svcsock_rx_region_contig(test, nr, 0);
	want = svcsock_rx_region_nshards(nr);
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, nshards, want);
	for (i = 0; i < nshards; i++) {
		KUNIT_EXPECT_EQ(test,
				page_pool_rx_region_kunit_shards(false, i, &cap,
								 &foreign,
								 &cached,
								 &maxcache,
								 &nshards),
				svcsock_rx_region_cap(nr, want, i));
		KUNIT_EXPECT_EQ(test, cap, svcsock_rx_region_cap(nr, want, i));
	}
	page_pool_rx_region_kunit_remove(true);
}

/*
 * pfn-range shards: how many (one per 1024 pages, at most 16 and one per
 * CPU), the last one's remainder, home shard first and then round robin,
 * large takes and gives bypassing the caches, every page back in its owner
 * shard, and min_free as the sum of the shards' low-water marks.
 */
static void svcsock_rx_region_shard_test(struct kunit *test)
{
	unsigned long nr = SVCSOCK_RX_REGION_BIG, first, cap, cached, c, st[4];
	unsigned int foreign, maxcache, nshards, home, next, i;
	unsigned long home_cap;
	struct page **pages;
	int cpu;

	svcsock_rx_region_expect_layout(test, 1023);
	svcsock_rx_region_expect_layout(test, 3001);
	first = svcsock_rx_region_contig(test, nr, 0);
	cpu = svcsock_rx_region_pin(test);
	pages = kunit_kcalloc(test, nr, sizeof(*pages), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pages);
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_ASSERT_EQ(test, nshards, svcsock_rx_region_nshards(nr));
	home = (cpu * nshards) / nr_cpu_ids;
	home_cap = svcsock_rx_region_cap(nr, nshards, home);

	/* A bulk take comes from the home shard; min_free sums the shards. */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, 100),
			100U);
	for (i = 0; i < nshards; i++)
		KUNIT_EXPECT_EQ(test,
				page_pool_rx_region_kunit_shards(false, i, &cap,
								 &foreign,
								 &cached,
								 &maxcache,
								 &nshards),
				svcsock_rx_region_cap(nr, nshards, i) -
				(i == home ? 100 : 0));
	page_pool_rx_region_give_pages(pages, 100);
	page_pool_rx_region_kunit_part(false, st);
	KUNIT_EXPECT_EQ(test, st[0], nr);
	KUNIT_EXPECT_EQ(test, st[1], nr - 100);
	page_pool_rx_region_kunit_reset_min_free();
	page_pool_rx_region_kunit_part(false, st);
	KUNIT_EXPECT_EQ(test, st[1], nr);

	/* The home shard emptied, the next one serves. */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages,
							     home_cap),
			(unsigned int)home_cap);
	for (i = 0; i < home_cap; i++)
		KUNIT_EXPECT_EQ(test, (page_to_pfn(pages[i]) - first) /
				DIV_ROUND_UP(nr, nshards), (unsigned long)home);
	if (nshards > 1) {
		next = home + 1 == nshards ? 0 : home + 1;
		KUNIT_ASSERT_EQ(test,
				page_pool_rx_region_take_pages(true,
							       pages + home_cap,
							       40),
				40U);
		for (i = 0; i < 40; i++)
			KUNIT_EXPECT_EQ(test,
					(page_to_pfn(pages[home_cap + i]) -
					 first) / DIV_ROUND_UP(nr, nshards),
					(unsigned long)next);
		page_pool_rx_region_give_pages(pages, home_cap + 40);
	} else {
		page_pool_rx_region_give_pages(pages, home_cap);
	}
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, cached, 0UL);
	svcsock_rx_region_shards_own(test, false, true);

	/* 257 pages (a 1 MiB READ) each way, and a give of 32: no cache. */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, 257),
			257U);
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, cached, 0UL);
	page_pool_rx_region_give_pages(pages, 257);
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, cached, 0UL);
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, 32),
			32U);
	svcsock_rx_region_free(false, &c, &maxcache, &nshards);
	page_pool_rx_region_give_pages(pages, 32);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), nr);
	KUNIT_EXPECT_EQ(test, cached, c);
	page_pool_rx_region_kunit_drain(false);
	svcsock_rx_region_shards_own(test, false, true);
}

/*
 * A split region: each partition has its own caches and shards, a mixed
 * give returns every page to its partition's owner shard, and an escape
 * makes the read partition one page smaller.
 */
static void svcsock_rx_region_split_shards_test(struct kunit *test)
{
	unsigned long nr = 2 * SVCSOCK_RX_REGION_BIG, half = nr / 2, cached;
	unsigned int maxcache, nshards, i;
	struct page **pages, *mixed[40];
	unsigned long st[4];

	svcsock_rx_region_contig(test, nr, half);
	svcsock_rx_region_pin(test);
	pages = kunit_kcalloc(test, nr, sizeof(*pages), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pages);

	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(true, &cached, &maxcache,
						     &nshards), half);
	KUNIT_EXPECT_EQ(test, nshards, svcsock_rx_region_nshards(half));
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), half);
	KUNIT_EXPECT_EQ(test, nshards, svcsock_rx_region_nshards(half));

	/* A small read take fills the read cache only (read_cached). */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, 1),
			1U);
	svcsock_rx_region_free(true, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, cached, 32UL);
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, cached, 0UL);
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(false, pages + 1, 1),
			1U);
	svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_EXPECT_EQ(test, cached, 32UL);

	/* Bulk runs of each partition in one give. */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages + 2, 40),
			40U);
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(false, pages + 42,
							     40),
			40U);
	page_pool_rx_region_give_pages(pages, 82);

	/* Interleaved: runs of one, through the caches. */
	for (i = 0; i < 20; i++) {
		KUNIT_ASSERT_EQ(test,
				page_pool_rx_region_take_pages(true,
							       &mixed[2 * i], 1),
				1U);
		KUNIT_ASSERT_EQ(test,
				page_pool_rx_region_take_pages(false,
							       &mixed[2 * i + 1],
							       1),
				1U);
	}
	page_pool_rx_region_give_pages(mixed, 40);
	page_pool_rx_region_kunit_drain(true);
	page_pool_rx_region_kunit_drain(false);
	svcsock_rx_region_shards_own(test, true, true);
	svcsock_rx_region_shards_own(test, false, true);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(true, &cached, &maxcache,
						     &nshards), half);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards), half);

	/* An escaped read page: the read partition's capacity is one less. */
	KUNIT_ASSERT_EQ(test, page_pool_rx_region_take_pages(true, pages, 1),
			1U);
	get_page(pages[0]);
	page_pool_rx_region_give_pages(pages, 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(pages[0]), 1);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_capacity(true), half - 1);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_capacity(false), half);
	page_pool_rx_region_kunit_part(true, st);
	KUNIT_EXPECT_EQ(test, st[0], half - 1);
	KUNIT_EXPECT_EQ(test, st[3], 1UL);
	page_pool_rx_region_kunit_part(false, st);
	KUNIT_EXPECT_EQ(test, st[3], 0UL);
	/* The escaped page keeps refcount 1; removal frees it with the rest. */
}

/* page_pool.c's bounds on a pool bound to the region. */
#define SVCSOCK_RX_POOL_RING	64
#define SVCSOCK_RX_POOL_REFILL	min_t(unsigned int, PP_ALLOC_CACHE_REFILL, 16)
#define SVCSOCK_RX_POOL_CACHE	(2 * SVCSOCK_RX_POOL_REFILL)
#define SVCSOCK_RX_POOL_HELD	48	/* three refills at most */

struct svcsock_rx_region_pool {
	struct page_pool *pool;
	struct page *pages[SVCSOCK_RX_POOL_HELD];
	unsigned int n;
};

/* Give back the pages the case still holds, then destroy the pool. */
static void svcsock_rx_region_pool_off(void *data)
{
	struct svcsock_rx_region_pool *rp = data;

	while (rp->n)
		page_pool_put_full_page(rp->pool, rp->pages[--rp->n], false);
	page_pool_destroy(rp->pool);
}

/*
 * A pool bound to the region, as every listed device's queue pool is, keeps
 * at most 64 remotely released pages (in 7.3's recycle stack, or in a ring
 * shortened to 64 where the stack could not be allocated), refills 16 pages
 * at a time from the write partition and caches at most 32 (fewer on large
 * pages, as PP_ALLOC_CACHE_REFILL is).
 */
static void svcsock_rx_region_pool_bounds_test(struct kunit *test)
{
	unsigned long nr = SVCSOCK_RX_REGION_BIG, cached, before;
	unsigned int refill = SVCSOCK_RX_POOL_REFILL, maxcache, nshards, i;
	struct svcsock_rx_region_pool *rp;
	struct page *page;

	svcsock_rx_region_contig(test, nr, 0);
	svcsock_rx_region_pin(test);
	rp = kunit_kzalloc(test, sizeof(*rp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rp);
	rp->pool = page_pool_rx_region_kunit_pool();
	KUNIT_ASSERT_FALSE(test, IS_ERR(rp->pool));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_pool_off, rp), 0);
	if (rp->pool->rx_stack)
		KUNIT_EXPECT_EQ(test, rp->pool->rx_stack_size,
				SVCSOCK_RX_POOL_RING);
	else
		KUNIT_EXPECT_EQ(test, rp->pool->ring.size,
				SVCSOCK_RX_POOL_RING);

	/* The first allocation refills the pool from the write partition. */
	before = svcsock_rx_region_free(false, &cached, &maxcache, &nshards);
	KUNIT_ASSERT_EQ(test, before, nr);
	page = page_pool_alloc_pages(rp->pool, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	rp->pages[rp->n++] = page;
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards),
			before - refill);
	KUNIT_EXPECT_EQ(test, rp->pool->alloc.count, refill - 1);

	/* Two more refills; every page held, the pool's cache empty. */
	while (rp->n < 3 * refill) {
		page = page_pool_alloc_pages(rp->pool, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, page);
		rp->pages[rp->n++] = page;
	}
	KUNIT_EXPECT_EQ(test, rp->pool->alloc.count, 0U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards),
			before - 3 * refill);

	/* Direct recycling fills the pool's cache to 32 and refuses the 33rd. */
	for (i = 0; i < SVCSOCK_RX_POOL_CACHE; i++)
		KUNIT_EXPECT_TRUE(test, page_pool_rx_region_kunit_recycle(
					rp->pool, rp->pages[--rp->n]));
	KUNIT_EXPECT_EQ(test, rp->pool->alloc.count, SVCSOCK_RX_POOL_CACHE);
	KUNIT_ASSERT_GT(test, rp->n, 0U);
	KUNIT_EXPECT_FALSE(test, page_pool_rx_region_kunit_recycle(
					rp->pool, rp->pages[rp->n - 1]));
	KUNIT_EXPECT_EQ(test, rp->pool->alloc.count, SVCSOCK_RX_POOL_CACHE);

	/* Destroying the pool returns every page to the write partition. */
	kunit_release_action(test, svcsock_rx_region_pool_off, rp);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards),
			nr);
}

/*
 * A received skb freed on another CPU waits on the defer list of the CPU
 * that allocated it, and only that CPU's receive processing (or the list
 * reaching half of net.core.skb_defer_max) frees it. When that CPU's device
 * has gone, the pool holding the skb's page must still finish its release,
 * and its pages must come back to the region.
 */
static void svcsock_rx_region_pool_defer_test(struct kunit *test)
{
	unsigned long nr = SVCSOCK_RX_REGION_BIG, cached, free;
	unsigned int maxcache, nshards, ms;
	struct svcsock_rx_region_pool *rp;
	struct page *page;

	if (num_online_cpus() < 2)
		kunit_skip(test, "needs a second online CPU");
	svcsock_rx_region_contig(test, nr, 0);
	svcsock_rx_region_pin(test);
	rp = kunit_kzalloc(test, sizeof(*rp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rp);
	rp->pool = page_pool_rx_region_kunit_pool();
	KUNIT_ASSERT_FALSE(test, IS_ERR(rp->pool));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_pool_off, rp), 0);

	page = page_pool_alloc_pages(rp->pool, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_TRUE(test, page_pool_rx_region_kunit_defer(rp->pool, page));

	/* The pool goes with its page still in the deferred skb. */
	kunit_release_action(test, svcsock_rx_region_pool_off, rp);
	KUNIT_EXPECT_EQ(test, svcsock_rx_region_free(false, &cached, &maxcache,
						     &nshards),
			nr - 1);

	/* The release retries every second; allow it three. */
	for (ms = 0; ms < 3500; ms += 100) {
		free = svcsock_rx_region_free(false, &cached, &maxcache,
					      &nshards);
		if (free == nr)
			break;
		msleep(100);
	}
	KUNIT_EXPECT_EQ(test, free, nr);
}
#endif

/*
 * The namespace budgets and the default together may not exceed the write
 * partition, so the read partition never takes receive pressure; unsplit,
 * the whole region is the write partition.
 */
static void svcsock_rx_budget_write_partition_test(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_default_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(0, 0, 100);
	svc_tcp_rx_budget_kunit_default(0, 0);
	svc_tcp_rx_budget_kunit_region(100, 30);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_set(70), 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_set(71), -ENOSPC);
	svc_tcp_rx_budget_kunit_default(20, 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_set(51), -ENOSPC);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_set(50), 0);

	svc_tcp_rx_budget_kunit_region(100, 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_set(81), -ENOSPC);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_set(80), 0);
}

static void svcsock_rx_credit_invalid_reset_test(struct kunit *test)
{
	struct svc_tcp_rx_state *state = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *svsk = svcsock_rx_credit_sock(test);
	u32 credit;
	long avail;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(10, 0, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(svsk, state, 10 * PAGE_SIZE));
	credit = state->cr.credit;
	avail = svc_tcp_rx_budget_kunit_avail();
	state->terminal = false;
	kunit_warning_suppress(test) {
		svc_tcp_rx_reset(state);
		SVCSOCK_EXPECT_ONCE_WARNING(test);
	}
	KUNIT_EXPECT_TRUE(test, state->cr.linked);
	KUNIT_EXPECT_EQ(test, state->cr.credit, credit);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), avail);
	svc_tcp_rx_credit_release(state);
	state->terminal = true;
	svc_tcp_rx_reset(state);
}

static void svcsock_rx_credit_factor_bounds_test(struct kunit *test)
{
	/* The largest and the smallest factor a held buffer can be given. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_scaling_ratio(12800), (u8)1);
	KUNIT_EXPECT_GT(test, svc_tcp_rx_window_clamp(SOCK_MIN_RCVBUF, 12800), 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_scaling_ratio(50), (u8)128);
}

/* Credit stays off while the receive-buffer clamp is below the floor. */
static void svcsock_rx_credit_rcvbuf_floor_test(struct kunit *test)
{
	struct svc_tcp_rx_state *state = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *svsk = svcsock_rx_credit_sock(test);
	const u32 len = 10 * PAGE_SIZE;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(40, 10, 100);
	svc_tcp_rx_budget_kunit_rcvbuf(64 * 1024);
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_admit(svsk, state, len));
	KUNIT_EXPECT_EQ(test, state->cr.credit, 0U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 0L);
	svc_tcp_rx_credit_release(state);
	svc_tcp_rx_budget_kunit_rcvbuf(128 * 1024);
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_admit(svsk, state, len));
	KUNIT_EXPECT_EQ(test, state->cr.credit, 11U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 19L);
	svc_tcp_rx_credit_release(state);
}

static void svcsock_rx_window_clamp_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_window_clamp(262144, 150), 87040);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_window_clamp(65536, 280), 11520);
	/* Under a byte of memory to a byte of payload counts as one. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_window_clamp(65536, 50), 32768);
	/* The same geometry in TCP_RMEM_TO_WIN_SCALE (1/256) units. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_scaling_ratio(150), 85);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_scaling_ratio(280), 45);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_scaling_ratio(50), 128);
}

static void svcsock_rx_sock_release(void *sock)
{
	sock_release(sock);
}

/*
 * A transport that receives region pages holds its receive window through
 * TCP's interfaces: the buffer locked at svc_tcp_rx_rcvbuf, and the ratio
 * TCP turns free buffer into window with capped at the region's geometry.
 * Factor 280 puts the ceiling (45) below TCP's starting ratio (128), so the
 * ratio and the window clamp drop at once; TCP keeps the ceiling for when it
 * re-learns its ratio.  Held once per transport.
 */
static void svcsock_rx_hold_buffer_test(struct kunit *test)
{
	struct svc_sock *svsk = svcsock_rx_credit_sock(test);
	const int rcvbuf = 128 * 1024;
	struct socket *sock;
	struct tcp_sock *tp;
	u8 ratio = svc_tcp_rx_scaling_ratio(280);

	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, AF_INET, SOCK_STREAM,
					       IPPROTO_TCP, &sock), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_sock_release, sock), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svsk->sk_sock = sock;
	svsk->sk_sk = sock->sk;
	tp = tcp_sk(sock->sk);
	svc_tcp_rx_budget_kunit_configure(0, 0, 280);
	svc_tcp_rx_budget_kunit_rcvbuf(rcvbuf);
	KUNIT_ASSERT_EQ(test, tp->scaling_ratio, TCP_DEFAULT_SCALING_RATIO);
	KUNIT_ASSERT_LT(test, ratio, TCP_DEFAULT_SCALING_RATIO);

	svc_tcp_rx_hold_buffer(svsk);
	KUNIT_EXPECT_TRUE(test, svsk->sk_rx_held);
	KUNIT_EXPECT_TRUE(test, sock->sk->sk_userlocks & SOCK_RCVBUF_LOCK);
	KUNIT_EXPECT_EQ(test, sock->sk->sk_rcvbuf, rcvbuf);
	KUNIT_EXPECT_EQ(test, tp->scaling_ratio_max, ratio);
	KUNIT_EXPECT_EQ(test, tp->scaling_ratio, ratio);
	KUNIT_EXPECT_LE(test, tp->window_clamp,
			(u32)svc_tcp_rx_window_clamp(rcvbuf, 280));
	KUNIT_EXPECT_LE(test, tp->window_clamp,
			(u32)tcp_win_from_space(sock->sk, sock->sk->sk_rcvbuf));

	/* Held once: a later call changes nothing. */
	svc_tcp_rx_budget_kunit_configure(0, 0, 150);
	svc_tcp_rx_budget_kunit_rcvbuf(2 * rcvbuf);
	svc_tcp_rx_hold_buffer(svsk);
	KUNIT_EXPECT_EQ(test, sock->sk->sk_rcvbuf, rcvbuf);
	KUNIT_EXPECT_EQ(test, tp->scaling_ratio_max, ratio);
}

/*
 * A transport held before its budget learned its factor (150) and held
 * again at the factor it measured (107, svc_tcp_rx_hold_stale()) gets the
 * window of one held at 107 from the start: the ratio rises to the new
 * ceiling, and rcv_ssthresh, which had grown to the old clamp, by what the
 * clamp rose.  Without that TCP keeps both where the first hold left them.
 */
static void svcsock_rx_hold_again_test(struct kunit *test)
{
	struct svc_sock *svsk = svcsock_rx_credit_sock(test);
	const int rcvbuf = 128 * 1024;
	struct socket *sock;
	struct tcp_sock *tp;

	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, AF_INET, SOCK_STREAM,
					       IPPROTO_TCP, &sock), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_sock_release, sock), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svsk->sk_sock = sock;
	svsk->sk_sk = sock->sk;
	tp = tcp_sk(sock->sk);
	svc_tcp_rx_budget_kunit_rcvbuf(rcvbuf);

	svc_tcp_rx_budget_kunit_configure(0, 0, 150);
	svc_tcp_rx_budget_kunit_rcvbuf(rcvbuf);
	svc_tcp_rx_hold_buffer(svsk);
	KUNIT_ASSERT_TRUE(test, svsk->sk_rx_held);
	KUNIT_ASSERT_EQ(test, tp->scaling_ratio, svc_tcp_rx_scaling_ratio(150));
	KUNIT_ASSERT_EQ(test, tp->window_clamp,
			(u32)svc_tcp_rx_window_clamp(rcvbuf, 150));
	/* The window has grown to the clamp, as it does under load. */
	tp->rcv_ssthresh = tp->window_clamp;

	/* Measured at 107: stale, and held again. */
	svc_tcp_rx_budget_kunit_configure(0, 0, 107);
	svc_tcp_rx_budget_kunit_rcvbuf(rcvbuf);
	svsk->sk_rx_held = false;
	svc_tcp_rx_hold_buffer(svsk);
	KUNIT_EXPECT_TRUE(test, svsk->sk_rx_held);
	KUNIT_EXPECT_EQ(test, svsk->sk_rx_hold_factor, 107U);
	KUNIT_EXPECT_EQ(test, tp->scaling_ratio_max,
			svc_tcp_rx_scaling_ratio(107));
	KUNIT_EXPECT_EQ(test, tp->scaling_ratio, svc_tcp_rx_scaling_ratio(107));
	KUNIT_EXPECT_EQ(test, tp->window_clamp,
			(u32)svc_tcp_rx_window_clamp(rcvbuf, 107));
	KUNIT_EXPECT_EQ(test, tp->rcv_ssthresh, tp->window_clamp);

	/* Measured at 150 again: the window shrinks as TCP shrinks it. */
	svc_tcp_rx_budget_kunit_configure(0, 0, 150);
	svc_tcp_rx_budget_kunit_rcvbuf(rcvbuf);
	svsk->sk_rx_held = false;
	svc_tcp_rx_hold_buffer(svsk);
	KUNIT_EXPECT_EQ(test, tp->scaling_ratio, svc_tcp_rx_scaling_ratio(150));
	KUNIT_EXPECT_LE(test, tp->window_clamp,
			(u32)svc_tcp_rx_window_clamp(rcvbuf, 150));
	KUNIT_EXPECT_LE(test, tp->rcv_ssthresh, tp->window_clamp);
}

static void svcsock_rx_loopback_down(void *dev)
{
	rtnl_lock();
	dev_close(dev);
	rtnl_unlock();
}

/* A built-in KUnit run starts before anything brings up lo. */
static void svcsock_rx_loopback_up(struct kunit *test)
{
	struct net_device *lo = init_net.loopback_dev;
	int err = 0;

	if (netif_running(lo))
		return;
	rtnl_lock();
	err = dev_open(lo, NULL);
	rtnl_unlock();
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_loopback_down, lo), 0);
}

/*
 * TCP sizes a connection's first window from its listener and never
 * shrinks it. From a listener with a large buffer the window is already
 * above what the region clamp allows when the accepted socket is held;
 * from a held listener it opens within the clamp.
 */
static void svcsock_rx_listener_window_test(struct kunit *test)
{
	int want = svc_tcp_rx_window_clamp(SZ_256K, 150);
	u8 ratio = svc_tcp_rx_scaling_ratio(150);
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct socket *lsock, *csock, *ssock;
	struct svc_sock *svsk;
	struct tcp_sock *tp;
	int held;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svcsock_rx_loopback_up(test);
	svc_tcp_rx_budget_kunit_configure(0, 0, 150);
	svc_tcp_rx_budget_kunit_rcvbuf(SZ_256K);

	for (held = 0; held < 2; held++) {
		KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, AF_INET,
				SOCK_STREAM, IPPROTO_TCP, &lsock), 0);
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				svcsock_rx_sock_release, lsock), 0);
		/* A large default receive buffer (tcp_rmem). */
		sock_set_rcvbuf(lsock->sk, SZ_2M);
		addr.sin_port = 0;
		KUNIT_ASSERT_EQ(test, kernel_bind(lsock,
				(struct sockaddr_unsized *)&addr, sizeof(addr)), 0);
		KUNIT_ASSERT_EQ(test, kernel_listen(lsock, 1), 0);
		if (held) {
			svsk = svcsock_rx_credit_sock(test);
			svsk->sk_sock = lsock;
			svsk->sk_sk = lsock->sk;
			svc_tcp_rx_hold_buffer(svsk);
			KUNIT_ASSERT_TRUE(test, svsk->sk_rx_held);
			KUNIT_EXPECT_EQ(test, READ_ONCE(tcp_sk(lsock->sk)->window_clamp),
					(u32)want);
			KUNIT_EXPECT_EQ(test, tcp_sk(lsock->sk)->scaling_ratio_max,
					ratio);
		}
		KUNIT_ASSERT_EQ(test, kernel_getsockname(lsock,
				(struct sockaddr *)&addr), (int)sizeof(addr));
		KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, AF_INET,
				SOCK_STREAM, IPPROTO_TCP, &csock), 0);
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				svcsock_rx_sock_release, csock), 0);
		KUNIT_ASSERT_EQ(test, kernel_connect(csock,
				(struct sockaddr_unsized *)&addr, sizeof(addr), 0), 0);
		KUNIT_ASSERT_EQ(test, kernel_accept(lsock, &ssock, 0), 0);
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				svcsock_rx_sock_release, ssock), 0);
		tp = tcp_sk(ssock->sk);

		if (!held) {
			KUNIT_EXPECT_GT(test, READ_ONCE(tp->rcv_wnd), (u32)want);
			continue;
		}
		KUNIT_EXPECT_EQ(test, READ_ONCE(ssock->sk->sk_rcvbuf), SZ_256K);
		KUNIT_EXPECT_TRUE(test, ssock->sk->sk_userlocks & SOCK_RCVBUF_LOCK);
		/* tcp_init_buffer_space() may trim it to the full space. */
		KUNIT_EXPECT_LE(test, READ_ONCE(tp->window_clamp), (u32)want);
		KUNIT_EXPECT_LE(test, READ_ONCE(tp->rcv_wnd), (u32)want);
		KUNIT_EXPECT_LE(test, tp->scaling_ratio, ratio);
	}
}

static void svcsock_rx_capacity_plus_one_test(struct kunit *test)
{
	enum { FRAG_LEN = 8 };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 2);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	/*
	 * One loaned fragment more than the table holds.  An skb takes
	 * MAX_SKB_FRAGS of them, so where the table is larger than that (64 KiB
	 * pages) the record continues in further skbs and the table fills in
	 * a later one, with the loans of the earlier ones already taken.
	 */
	unsigned int nfrags = state->lt.capacity + 1;
	unsigned int nskbs = DIV_ROUND_UP(nfrags, MAX_SKB_FRAGS);
	unsigned int body_len = 7 * XDR_UNIT + nfrags * FRAG_LEN;
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = {};
	struct sk_buff **skbs;
	struct page **pages;
	unsigned int *refs;
	const u8 *payload;
	u8 *record;
	unsigned int i;
	int ret;

	record = kunit_kzalloc(test, sizeof(rpc_fraghdr) + body_len, GFP_KERNEL);
	pages = kunit_kcalloc(test, nfrags, sizeof(*pages), GFP_KERNEL);
	refs = kunit_kcalloc(test, nfrags, sizeof(*refs), GFP_KERNEL);
	skbs = kunit_kcalloc(test, nskbs, sizeof(*skbs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, record);
	KUNIT_ASSERT_NOT_NULL(test, pages);
	KUNIT_ASSERT_NOT_NULL(test, refs);
	KUNIT_ASSERT_NOT_NULL(test, skbs);
	desc.count = sizeof(rpc_fraghdr) + body_len;

	svcsock_rx_fill_record(record, body_len, true, RPC_CALL, RPC_AUTH_UNIX);
	payload = record + sizeof(rpc_fraghdr) + 7 * XDR_UNIT;
	for (i = 0; i < nfrags; i++) {
		unsigned int s = i / MAX_SKB_FRAGS;
		/* The first skb leads with the record marker and RPC header. */
		unsigned int head = s ? 0 : sizeof(rpc_fraghdr) + 7 * XDR_UNIT;

		if (i % MAX_SKB_FRAGS == 0)
			skbs[s] = svcsock_rx_build_linear_skb(test, record,
							      head);
		pages[i] = svcsock_rx_add_frag(test, skbs[s], i % MAX_SKB_FRAGS,
					       payload + i * FRAG_LEN, FRAG_LEN,
					       31 + i * 19, &refs[i]);
	}

	for (i = 0; i < nskbs; i++) {
		ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc,
					   skbs[i], 0, skbs[i]->len);
		KUNIT_ASSERT_EQ(test, ret, (int)skbs[i]->len);
		KUNIT_EXPECT_EQ(test, actor.complete, i == nskbs - 1);
	}
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_COPY);
	KUNIT_EXPECT_EQ(test, state->reason, (u8)SVC_TCP_RX_REASON_CAPACITY);
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, nfrags - 1);
	KUNIT_EXPECT_EQ(test, state->lt.refs_released, nfrags - 1);
	KUNIT_EXPECT_EQ(test, state->materialized_bytes,
			(nfrags - 1) * FRAG_LEN);
	KUNIT_EXPECT_EQ(test, state->lt.count, 0U);
	KUNIT_EXPECT_MEMEQ(test, page_address(fixture->rqstp->rq_pages[0]),
			   record + sizeof(rpc_fraghdr), body_len);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_publish(fixture->svsk, fixture->rqstp), 0);
	for (i = 0; i < nskbs; i++)
		kunit_kfree_skb(test, skbs[i]);
	for (i = 0; i < nfrags; i++)
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
	KUNIT_EXPECT_EQ(test, request_state->lt.refs_acquired, 1U);
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

static void svcsock_rx_spare_restore_max(void *data)
{
	svc_tcp_spare_kunit_set_max((unsigned long)data);
}

/* Bound the pages sockets keep for exchange to @bytes for this test. */
static void svcsock_rx_spare_set_max(struct kunit *test, unsigned long bytes)
{
	unsigned long old = svc_tcp_spare_kunit_set_max(bytes);

	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  svcsock_rx_spare_restore_max,
						  (void *)old),
			0);
}

/*
 * Park a one-page partial record on the socket, then give the thread a
 * page of its own in place of the one it parked, as svc_alloc_arg()
 * would.  The caller restores the record into that thread.
 */
static struct sk_buff *
svcsock_rx_spare_park(struct kunit *test, struct svcsock_rx_fixture *fixture,
		      struct page **own, unsigned int *own_refs)
{
	enum { BODY_LEN = 7 * XDR_UNIT + 12 };
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
	KUNIT_ASSERT_FALSE(test, actor.complete);
	KUNIT_ASSERT_EQ(test, svc_tcp_save_pages(fixture->svsk, fixture->rqstp), 0);
	KUNIT_ASSERT_NULL(test, fixture->rqstp->rq_pages[0]);

	/* The test's reference keeps the page across a free by the code. */
	*own = svcsock_rx_alloc_page(test);
	KUNIT_ASSERT_NOT_NULL(test, *own);
	get_page(*own);
	*own_refs = page_ref_count(*own);
	fixture->rqstp->rq_pages[0] = *own;
	return skb;
}

/* End the restored record and close the socket's receive state. */
static void svcsock_rx_spare_finish(struct kunit *test,
				    struct svcsock_rx_fixture *fixture,
				    struct sk_buff *skb)
{
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_abort(fixture->svsk, fixture->rqstp,
					 fixture->rqstp->rq_tcp_rx,
					 SVC_TCP_RX_ERROR,
					 SVC_TCP_RX_REASON_COPY_FAULT), 0);
	kunit_kfree_skb(test, skb);
	svc_tcp_recv_record_done(fixture->svsk);
	svc_tcp_clear_pages(fixture->svsk);
}

/*
 * With no room under svc_tcp_spare_bytes_max, a restore frees the
 * thread's pages, as it did before sockets kept any.
 */
static void svcsock_rx_spare_none_test(struct kunit *test)
{
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	unsigned int own_refs;
	struct sk_buff *skb;
	struct page *own;
	long base;

	svcsock_rx_spare_set_max(test, 0);
	skb = svcsock_rx_spare_park(test, fixture, &own, &own_refs);
	base = svc_tcp_spare_kunit_pages();

	KUNIT_ASSERT_EQ(test, svc_tcp_restore_pages(fixture->svsk,
						    fixture->rqstp), 0);
	KUNIT_EXPECT_NULL(test, fixture->svsk->sk_pages[0]);
	KUNIT_EXPECT_PTR_NE(test, fixture->rqstp->rq_pages[0], own);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_spare_pages, 0U);
	KUNIT_EXPECT_EQ(test, svc_tcp_spare_kunit_pages(), base);
	KUNIT_ASSERT_EQ(test,
			folio_batch_count(&fixture->rqstp->rq_fbatch), 1U);
	folio_batch_release(&fixture->rqstp->rq_fbatch);
	KUNIT_EXPECT_EQ(test, page_ref_count(own), own_refs - 1);

	svcsock_rx_spare_finish(test, fixture, skb);
	KUNIT_EXPECT_EQ(test, svc_tcp_spare_kunit_pages(), base);
}

/*
 * With room, a restore keeps the thread's pages on the socket and counts
 * them; a save takes them back, and the close frees them, each giving
 * the count back.
 */
static void svcsock_rx_spare_kept_test(struct kunit *test)
{
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	unsigned int own_refs;
	struct sk_buff *skb;
	struct page *own;
	long base;

	svcsock_rx_spare_set_max(test, ULONG_MAX);
	skb = svcsock_rx_spare_park(test, fixture, &own, &own_refs);
	base = svc_tcp_spare_kunit_pages();

	KUNIT_ASSERT_EQ(test, svc_tcp_restore_pages(fixture->svsk,
						    fixture->rqstp), 0);
	KUNIT_EXPECT_PTR_EQ(test, fixture->svsk->sk_pages[0], own);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_spare_pages, 1U);
	KUNIT_EXPECT_EQ(test, svc_tcp_spare_kunit_pages(), base + 1);
	KUNIT_EXPECT_EQ(test,
			folio_batch_count(&fixture->rqstp->rq_fbatch), 0U);

	/* The next save takes the kept page in exchange. */
	KUNIT_ASSERT_EQ(test, svc_tcp_save_pages(fixture->svsk,
						 fixture->rqstp), 0);
	KUNIT_EXPECT_PTR_EQ(test, fixture->rqstp->rq_pages[0], own);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_spare_pages, 0U);
	KUNIT_EXPECT_EQ(test, svc_tcp_spare_kunit_pages(), base);

	/* Kept again, and freed when the socket closes. */
	KUNIT_ASSERT_EQ(test, svc_tcp_restore_pages(fixture->svsk,
						    fixture->rqstp), 0);
	KUNIT_EXPECT_PTR_EQ(test, fixture->svsk->sk_pages[0], own);
	KUNIT_EXPECT_EQ(test, svc_tcp_spare_kunit_pages(), base + 1);
	svcsock_rx_spare_finish(test, fixture, skb);
	KUNIT_EXPECT_NULL(test, fixture->svsk->sk_pages[0]);
	KUNIT_EXPECT_EQ(test, fixture->svsk->sk_spare_pages, 0U);
	KUNIT_EXPECT_EQ(test, svc_tcp_spare_kunit_pages(), base);
	KUNIT_EXPECT_EQ(test, page_ref_count(own), own_refs - 1);
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
		bool no_budget;
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
		{ "no-budget", RPC_CALL, RPC_AUTH_UNIX, SVC_TCP_RX_COPY,
		  SVC_TCP_RX_REASON_NO_BUDGET, false, false, false, true },
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
		if (cases[i].no_budget) {
			svc_tcp_rx_budget_kunit_configure(0, 0, 0);
			svc_tcp_rx_budget_kunit_default(0, 0);
		}

		skb_len = skb->len;
		ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, skb, 0, skb_len);
		svc_tcp_rx_budget_kunit_default(SVCSOCK_RX_LOAN_BUDGET, 0);
		if (cases[i].unreadable) {
			KUNIT_EXPECT_EQ(test, ret, (int)sizeof(rpc_fraghdr));
			KUNIT_EXPECT_EQ(test, desc.error, -EFAULT);
			KUNIT_EXPECT_FALSE(test, actor.complete);
			KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 0U);
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
			KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 1U);
			ret = svc_tcp_rx_publish(fixture->svsk, fixture->rqstp);
			KUNIT_ASSERT_EQ(test, ret, 0);
			kunit_kfree_skb(test, skb);
			svcsock_rx_expect_bvec_bytes(test, state,
						     payload,
				BODY_LEN - 7 * XDR_UNIT);
			svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
		} else {
			KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 0U);
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
	state->lt.body_bytes = 28;
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
	KUNIT_EXPECT_EQ(test, state->lt.count, 1U);
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, 1U);
	KUNIT_EXPECT_EQ(test, page_ref_count(source), source_refs + 1);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_append_copied(state, arena, 40,
						       sizeof(copied)), 0);

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_materialize(state, pages), 0);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_COPY);
	KUNIT_EXPECT_EQ(test, state->lt.count, 0U);
	KUNIT_EXPECT_EQ(test, state->lt.borrowed_bytes, 0U);
	KUNIT_EXPECT_EQ(test, state->lt.copied_bytes,
			(u32)(sizeof(borrowed) + sizeof(copied)));
	KUNIT_EXPECT_EQ(test, state->materialized_bytes,
			(u32)sizeof(borrowed));
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, state->lt.refs_released);
	KUNIT_EXPECT_EQ(test, page_ref_count(source), source_refs);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->lt.borrowed,
					     state->lt.capacity));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->lt.page_pool,
					     state->lt.capacity));

	address = kmap_local_page(arena);
	KUNIT_EXPECT_MEMEQ(test, address + 28, borrowed, sizeof(borrowed));
	KUNIT_EXPECT_MEMEQ(test, address + 40, copied, sizeof(copied));
	kunmap_local(address);
}

static void svcsock_rx_capacity_release_test(struct kunit *test)
{
	struct svc_tcp_rx_state *state = svcsock_rx_alloc_state(test, 1);
	struct page *first = svcsock_rx_alloc_pp_page(test, NULL);
	struct page *second = svcsock_rx_alloc_page(test);
	unsigned int first_refs;
	unsigned int second_refs;
	unsigned int i;
	long first_pp_refs;

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, second);
	first_refs = page_ref_count(first);
	first_pp_refs = svcsock_rx_pp_refs(first);
	second_refs = page_ref_count(second);

	/* fill the table; gaps keep each fragment its own entry */
	for (i = 0; i < state->lt.capacity; i++)
		KUNIT_ASSERT_EQ(test,
				svc_tcp_rx_append_borrowed(state, first,
							   1 + i * 8, 3, true),
				0);
	KUNIT_EXPECT_EQ(test,
			svc_tcp_rx_append_borrowed(state, second, 5, 7, false),
			-ENOSPC);
	KUNIT_EXPECT_EQ(test, page_ref_count(first), first_refs);
	KUNIT_EXPECT_EQ(test, svcsock_rx_pp_refs(first),
			first_pp_refs + state->lt.capacity);
	KUNIT_EXPECT_EQ(test, page_ref_count(second), second_refs);
	KUNIT_EXPECT_TRUE(test, test_bit(0, state->lt.page_pool));

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(first), first_refs);
	KUNIT_EXPECT_EQ(test, svcsock_rx_pp_refs(first), first_pp_refs);
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, state->lt.refs_released);
}

/*
 * The loan table is sized for 4 KiB fragments, not pages: a loan takes
 * one entry per fragment, and fragments do not grow with PAGE_SIZE.
 */
static void svcsock_rx_capacity_sizing_test(struct kunit *test)
{
	struct svc_tcp_rx_state *state = svcsock_rx_alloc_state(test, 2);
	struct page *page = svcsock_rx_alloc_page(test);
	unsigned int refs;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_EXPECT_EQ(test, (unsigned long)state->lt.capacity,
			svc_pages_to_bvecs(2));
	KUNIT_EXPECT_EQ(test, state->arena_pages, 2U);
	refs = page_ref_count(page);

	for (i = 0; i < state->lt.capacity; i++)
		KUNIT_ASSERT_EQ(test,
				svc_tcp_rx_append_borrowed(state, page,
							   i * 8, 4, false),
				0);
	KUNIT_EXPECT_EQ(test, state->lt.count, state->lt.capacity);
	KUNIT_EXPECT_EQ(test,
			svc_tcp_rx_append_borrowed(state, page,
						   state->lt.capacity * 8, 4,
						   false),
			-ENOSPC);

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
}

/*
 * When the table holds more entries than the arena has pages (pages
 * larger than 4 KiB), materializing a record longer than the arena must
 * fail at the arena bound instead of indexing past the page array.
 */
static void svcsock_rx_arena_bound_test(struct kunit *test)
{
	struct svc_tcp_rx_state *state = svcsock_rx_alloc_state(test, 1);
	struct page *arena = svcsock_rx_alloc_page(test);
	struct page *first = svcsock_rx_alloc_page(test);
	struct page *second = svcsock_rx_alloc_page(test);
	struct page *pages[1];
	unsigned int first_refs, second_refs;

	if (svc_pages_to_bvecs(1) == 1)
		kunit_skip(test, "table and arena are the same size on %lu-byte pages",
			   PAGE_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, arena);
	KUNIT_ASSERT_NOT_NULL(test, first);
	KUNIT_ASSERT_NOT_NULL(test, second);
	pages[0] = arena;
	first_refs = page_ref_count(first);
	second_refs = page_ref_count(second);

	/* 28 fixed bytes plus a full page: the record outgrows one page */
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, first, 0,
						   PAGE_SIZE / 2, false), 0);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, second, 0,
						   PAGE_SIZE / 2, false), 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_materialize(state, pages), -EFAULT);

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(first), first_refs);
	KUNIT_EXPECT_EQ(test, page_ref_count(second), second_refs);
}

/*
 * The skb drops its page-pool reference while the loan still holds the
 * page; releasing the loan recycles the page into the same pool's ring
 * instead of the page allocator.
 */
/*
 * Stand-in for the fixed receive region: page_pool_page_in_rx_region() only
 * asks whether a pool is bound to one, and the region code is never reached
 * while the test holds the binding (it is cleared before any page returns).
 */
static int svcsock_rx_fake_region;

/*
 * A pool bound to the fake region holds page_pool_rx_region_key on, as a
 * real region does, so that region pages are looked for; unbinding twice
 * (by hand, then as the test's cleanup) lets it go once.
 */
static void svcsock_rx_unbind_region(void *data)
{
	struct page_pool *pool = data;

	if (!pool->rx_region)
		return;
	pool->rx_region = NULL;
	static_branch_dec(&page_pool_rx_region_key);
}

static void svcsock_rx_bind_region(struct kunit *test, struct page_pool *pool)
{
	if (!pool->rx_region)
		static_branch_inc(&page_pool_rx_region_key);
	pool->rx_region = (struct page_pool_rx_region *)&svcsock_rx_fake_region;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_unbind_region, pool), 0);
}

/*
 * Only pages from a pool bound to the region count toward region credit:
 * a plain head, a plain frag and a plain page-pool frag add nothing, a
 * region frag adds its pages, in the parent and through frag_list alike.
 */
static void svcsock_rx_skb_region_pages_test(struct kunit *test)
{
	static const u8 bytes[64];
	struct sk_buff *skb = svcsock_rx_build_linear_skb(test, bytes, 16);
	struct sk_buff *child = svcsock_rx_build_linear_skb(test, bytes, 8);
	struct svcsock_rx_pp *plain_pp, *region_pp, *child_pp;
	struct page *plain_page, *region_page, *child_page;
	unsigned int refs;

	plain_page = svcsock_rx_alloc_pp_page(test, &plain_pp);
	region_page = svcsock_rx_alloc_pp_page(test, &region_pp);
	child_page = svcsock_rx_alloc_pp_page(test, &child_pp);
	svcsock_rx_add_frag(test, skb, 0, bytes, 32, 0, &refs);
	get_page(plain_page);
	skb_add_rx_frag(skb, 1, plain_page, 0, 32, PAGE_SIZE);
	get_page(region_page);
	skb_add_rx_frag(skb, 2, region_page, 100, 40, PAGE_SIZE);
	get_page(child_page);
	skb_add_rx_frag(child, 0, child_page, 0, 24, PAGE_SIZE);
	svcsock_rx_attach_frag_list(test, skb, child);

	/* Nothing is bound to the region: nothing counts. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_skb_region_pages(skb, 0, 0, skb->len),
			0U);

	svcsock_rx_bind_region(test, region_pp->pool);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_skb_region_pages(skb, 0, 0, skb->len),
			1U);
	/* Bytes before the region frag, and after it, count nothing. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_skb_region_pages(skb, 0, 0, 80), 0U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_skb_region_pages(skb, 0, 120,
							   skb->len), 0U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_skb_region_pages(skb, 0, 90, 100), 1U);

	svcsock_rx_bind_region(test, child_pp->pool);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_skb_region_pages(skb, 0, 0, skb->len),
			2U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_skb_region_pages(skb, 0, 120,
							   skb->len), 1U);

	/*
	 * A second frame in the region page: two fragments, one page.  Where
	 * a page holds several frames the charge follows the pages.
	 */
	get_page(region_page);
	skb_add_rx_frag(skb, 3, region_page, 400, 40, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_skb_region_pages(skb, 0, 0, skb->len),
			2U);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_skb_region_pages(skb, 0, 80, 160),
			1U);
}

/*
 * A loan of a region page raises the record's region count; a loan of a
 * plain page-pool page does not.  Charge measures what a record holds by it.
 */
static void svcsock_rx_region_count_test(struct kunit *test)
{
	struct svc_tcp_rx_state *state = svcsock_rx_alloc_state(test, 4);
	struct svcsock_rx_pp *plain_pp, *region_pp;
	struct page *plain_page = svcsock_rx_alloc_pp_page(test, &plain_pp);
	struct page *region_page = svcsock_rx_alloc_pp_page(test, &region_pp);

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, plain_page, 0, 8, true),
			0);
	KUNIT_EXPECT_EQ(test, state->lt.region_count, 0U);
	svcsock_rx_bind_region(test, region_pp->pool);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, region_page, 0, 8, true),
			0);
	KUNIT_EXPECT_EQ(test, state->lt.region_count, 1U);
	KUNIT_EXPECT_EQ(test, state->lt.borrowed_count, 2U);
	/* Another fragment of the same region page is one more loan, no more pages. */
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, region_page, 64, 8, true),
			0);
	KUNIT_EXPECT_EQ(test, state->lt.region_count, 1U);
	KUNIT_EXPECT_EQ(test, state->lt.borrowed_count, 3U);
	svcsock_rx_unbind_region(region_pp->pool);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
}

/*
 * Budget 10, no reserve, factor 100: a record holds it all, so a transport
 * is refused at its record's first skb that carries a region page, and a
 * second transport queues behind it.  The first record ends, leaving two
 * waiters and nothing in flight.  The oldest waiter's unread bytes then come
 * from an skb with no region page, as after tcp_collapse(): it draws no
 * credit and is never admitted, so reading on must itself take it off the
 * wait list and wake the next waiter, or both are stranded.
 */
static void svcsock_rx_credit_collapsed_unwaits_test(struct kunit *test)
{
	enum {
		HEAD_LEN = sizeof(rpc_fraghdr) + 7 * XDR_UNIT,
		TAIL_LEN = 64,
		BODY_LEN = 7 * XDR_UNIT + TAIL_LEN,
	};
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *c = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = fixture->svsk;
	struct svc_sock *sc = svcsock_rx_credit_sock_forget(test);
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(record) };
	const u32 len = 10 * PAGE_SIZE;
	struct sk_buff *region_skb, *plain_skb;
	struct svcsock_rx_pp *region_pp;
	struct page *region_page, *page;
	unsigned int refs;
	int ret;

	/* As svcsock_rx_credit_sock(): the budget and the wake need these. */
	kref_init(&sb->sk_xprt.xpt_ref);
	sb->sk_xprt.xpt_net = &init_net;
	sb->sk_xprt.xpt_locallen = sizeof(struct sockaddr_in);
	sb->sk_xprt.xpt_remotelen = sizeof(struct sockaddr_in);
	sb->sk_xprt.xpt_local.ss_family = AF_INET;
	sb->sk_xprt.xpt_remote.ss_family = AF_INET;
	set_bit(XPT_BUSY, &sb->sk_xprt.xpt_flags);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_credit_forget_action, sb), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);

	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	region_page = svcsock_rx_alloc_pp_page(test, &region_pp);
	svcsock_rx_bind_region(test, region_pp->pool);
	region_skb = svcsock_rx_build_linear_skb(test, record, HEAD_LEN);
	svcsock_rx_write(region_page, 0, record + HEAD_LEN, TAIL_LEN);
	get_page(region_page);
	skb_add_rx_frag(region_skb, 0, region_page, 0, TAIL_LEN, PAGE_SIZE);

	svc_tcp_rx_budget_kunit_configure(10, 0, 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 0L);

	/* Refused at the region page: only the prefix before it is taken. */
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, region_skb,
				   0, region_skb->len);
	KUNIT_ASSERT_EQ(test, ret, (int)HEAD_LEN);
	KUNIT_ASSERT_FALSE(test, actor.complete);
	KUNIT_ASSERT_EQ(test, state->mode, (u8)SVC_TCP_RX_MIXED);
	KUNIT_ASSERT_FALSE(test, state->cr.linked);
	KUNIT_ASSERT_FALSE(test, list_empty(&sb->sk_rx_wait.node));
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_credit_admit(sc, c, len));

	/* The record in flight ends unseen; from here wakes are real. */
	svc_tcp_rx_credit_release(a);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(10), 0);
	KUNIT_ASSERT_FALSE(test, list_empty(&sb->sk_rx_wait.node));
	KUNIT_ASSERT_FALSE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));

	/* The same bytes, no longer in the region. */
	kunit_kfree_skb(test, region_skb);
	plain_skb = svcsock_rx_build_page_skb(test, record, sizeof(record),
					      &page, &refs);
	desc.count = sizeof(record) - HEAD_LEN;
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, plain_skb,
				   HEAD_LEN, sizeof(record) - HEAD_LEN);
	KUNIT_ASSERT_EQ(test, ret, (int)(sizeof(record) - HEAD_LEN));
	KUNIT_EXPECT_TRUE(test, actor.complete);
	KUNIT_EXPECT_FALSE(test, state->cr.linked);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_avail(), 10L);
	KUNIT_EXPECT_TRUE(test, list_empty(&sb->sk_rx_wait.node));
	KUNIT_EXPECT_EQ(test, sb->sk_rx_wait.ticket, 0ULL);
	KUNIT_EXPECT_NULL(test, sb->sk_rx_wait.budget);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_DATA, &sc->sk_xprt.xpt_flags));
	KUNIT_EXPECT_EQ(test, kref_read(&sc->sk_xprt.xpt_ref), 1U);
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_admit(sc, c, len));
	KUNIT_EXPECT_TRUE(test, list_empty(&sc->sk_rx_wait.node));
	svc_tcp_rx_credit_release(c);

	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_publish(fixture->svsk, fixture->rqstp), 0);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
	kunit_kfree_skb(test, plain_skb);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs - 1);
}

/*
 * A record's first skb has a locked head, so its bytes past the prefix are
 * copied into the arena.  The budget is turned off before the next skb,
 * which carries a region page, so the record is materialized: the locked
 * head's bytes are counted as copied once, and nothing stays borrowed.
 * (With no budget when it is classified, a record is copied from its start.)
 */
static void svcsock_rx_locked_head_then_materialize_test(struct kunit *test)
{
	enum {
		PREFIX_LEN = 7 * XDR_UNIT,
		HEAD_LEN = sizeof(rpc_fraghdr) + PREFIX_LEN,
		MERGE_LEN = 40,
		TAIL_LEN = 64,
		BODY_LEN = PREFIX_LEN + MERGE_LEN + TAIL_LEN,
	};
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = sizeof(record) };
	struct sk_buff *locked_skb, *region_skb;
	struct svcsock_rx_pp *region_pp;
	struct page *region_page;
	int ret;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_default_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(0, 0, 100);

	svcsock_rx_fill_record(record, BODY_LEN, true, RPC_CALL, RPC_AUTH_UNIX);
	locked_skb = svcsock_rx_build_linear_skb(test, record,
						 HEAD_LEN + MERGE_LEN);
	KUNIT_ASSERT_TRUE(test, skb_head_is_locked(locked_skb));
	region_page = svcsock_rx_alloc_pp_page(test, &region_pp);
	svcsock_rx_bind_region(test, region_pp->pool);
	region_skb = svcsock_rx_build_linear_skb(test, record, 0);
	svcsock_rx_write(region_page, 0, record + HEAD_LEN + MERGE_LEN,
			 TAIL_LEN);
	get_page(region_page);
	skb_add_rx_frag(region_skb, 0, region_page, 0, TAIL_LEN, PAGE_SIZE);

	/* The locked head's bytes past the prefix land in the arena. */
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, locked_skb,
				   0, locked_skb->len);
	KUNIT_ASSERT_EQ(test, ret, (int)(HEAD_LEN + MERGE_LEN));
	KUNIT_ASSERT_FALSE(test, actor.complete);
	KUNIT_ASSERT_EQ(test, state->mode, (u8)SVC_TCP_RX_MIXED);
	KUNIT_ASSERT_EQ(test, state->lt.count, 1U);
	KUNIT_ASSERT_EQ(test, state->lt.borrowed_count, 0U);
	KUNIT_ASSERT_EQ(test, state->lt.borrowed_bytes, 0U);
	KUNIT_ASSERT_EQ(test, state->lt.copied_bytes, (u32)MERGE_LEN);

	/* A region page and no budget: the record is copied. */
	svc_tcp_rx_budget_kunit_default(0, 0);
	desc.count = TAIL_LEN;
	ret = svcsock_rx_run_actor(fixture->rqstp, &actor, &desc, region_skb,
				   0, region_skb->len);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_EQ(test, ret, (int)TAIL_LEN);
	KUNIT_EXPECT_TRUE(test, actor.complete);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_COPY);
	KUNIT_EXPECT_EQ(test, state->reason,
			(u8)SVC_TCP_RX_REASON_NO_BUDGET);
	KUNIT_EXPECT_EQ(test, state->lt.borrowed_count, 0U);
	KUNIT_EXPECT_EQ(test, state->lt.borrowed_bytes, 0U);
	KUNIT_EXPECT_EQ(test, state->lt.copied_bytes,
			(u32)(MERGE_LEN + TAIL_LEN));
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, state->lt.refs_released);
	KUNIT_EXPECT_MEMEQ(test, page_address(fixture->rqstp->rq_pages[0]),
			   record + sizeof(rpc_fraghdr), BODY_LEN);

	kunit_kfree_skb(test, region_skb);
	kunit_kfree_skb(test, locked_skb);
}

static void svcsock_rx_page_pool_recycle_test(struct kunit *test)
{
	struct svc_tcp_rx_state *state = svcsock_rx_alloc_state(test, 1);
	struct svcsock_rx_pp *pp;
	struct page *page = svcsock_rx_alloc_pp_page(test, &pp);
	unsigned int refs = page_ref_count(page);

	KUNIT_ASSERT_NOT_NULL(test, state);
	state->mode = SVC_TCP_RX_MIXED;
	state->fixed_bytes = 28;
	state->lt.body_bytes = 28;
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, page, 1, 3, true), 0);

	pp->page = NULL;
	page_pool_put_full_page(pp->pool, page, false);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	KUNIT_EXPECT_TRUE(test, page_pool_page_is_pp(page));
	KUNIT_EXPECT_TRUE(test, ptr_ring_empty(&pp->pool->ring));

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	KUNIT_EXPECT_TRUE(test, page_pool_page_is_pp(page));
	pp->page = netmem_to_page((__force netmem_ref)
				  ptr_ring_consume_bh(&pp->pool->ring));
	KUNIT_EXPECT_PTR_EQ(test, pp->page, page);
}

/* More than one full batch of one pool's pages, and a remainder. */
#define SVCSOCK_RX_TEST_BULK_PAGES	(4 * XDP_BULK_QUEUE_SIZE + 1)

struct svcsock_rx_pp_set {
	struct page_pool *pool;
	unsigned int held;	/* pages[0..held) are the test's to put */
	struct page *pages[SVCSOCK_RX_TEST_BULK_PAGES];
};

static void svcsock_rx_pp_set_release(void *data)
{
	struct svcsock_rx_pp_set *set = data;

	while (set->held)
		page_pool_put_full_page(set->pool, set->pages[--set->held],
					false);
	page_pool_destroy(set->pool);
}

/*
 * A record whose page-pool pages it alone holds gives them all back to
 * their pool when it is released.  The pages go to the pool in batches;
 * page_pool_put_netmem_bulk() takes no more than XDP_BULK_QUEUE_SIZE at
 * a time, so a record of more pages than that, all of one pool and all
 * dropping their last reference, must be split before it gets there.
 */
static void svcsock_rx_page_pool_bulk_release_test(struct kunit *test)
{
	struct page_pool_params params = {
		.order = 0,
		.pool_size = 2 * SVCSOCK_RX_TEST_BULK_PAGES,
		.nid = NUMA_NO_NODE,
	};
	struct svc_tcp_rx_state *state;
	struct svcsock_rx_pp_set *set;
	unsigned int i, refs;
	netmem_ref netmem;

	if (!IS_ENABLED(CONFIG_PAGE_POOL))
		kunit_skip(test, "CONFIG_PAGE_POOL is not set");
	set = kunit_kzalloc(test, sizeof(*set), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, set);
	set->pool = page_pool_create(&params);
	KUNIT_ASSERT_FALSE(test, IS_ERR(set->pool));
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  svcsock_rx_pp_set_release,
						  set),
			0);
	state = svcsock_rx_alloc_state(test, SVCSOCK_RX_TEST_BULK_PAGES);
	KUNIT_ASSERT_NOT_NULL(test, state);

	for (i = 0; i < SVCSOCK_RX_TEST_BULK_PAGES; i++) {
		struct page *page = page_pool_alloc_pages(set->pool,
							  GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, page);
		set->pages[set->held++] = page;
		KUNIT_ASSERT_EQ(test,
				svc_tcp_rx_append_borrowed(state, page, 1, 3,
							   true), 0);
	}
	KUNIT_EXPECT_EQ(test, state->lt.count, SVCSOCK_RX_TEST_BULK_PAGES);
	refs = page_ref_count(set->pages[0]);

	/* The record now holds each page's last reference. */
	while (set->held)
		page_pool_put_full_page(set->pool, set->pages[--set->held],
					false);
	KUNIT_EXPECT_TRUE(test, ptr_ring_empty(&set->pool->ring));

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_EQ(test, state->lt.refs_acquired, state->lt.refs_released);
	KUNIT_EXPECT_EQ(test, state->lt.borrowed_count, 0U);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->lt.borrowed,
					     state->lt.capacity));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->lt.page_pool,
					     state->lt.capacity));

	/* Every page went back to the pool, none to the page allocator. */
	while (set->held < SVCSOCK_RX_TEST_BULK_PAGES &&
	       (netmem = (__force netmem_ref)
			ptr_ring_consume_bh(&set->pool->ring))) {
		struct page *page = netmem_to_page(netmem);

		set->pages[set->held++] = page;
		KUNIT_EXPECT_TRUE(test, page_pool_page_is_pp(page));
		KUNIT_EXPECT_EQ(test, page_ref_count(page), refs);
	}
	KUNIT_EXPECT_EQ(test, set->held, SVCSOCK_RX_TEST_BULK_PAGES);
	KUNIT_EXPECT_TRUE(test, ptr_ring_empty(&set->pool->ring));
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
		SVCSOCK_EXPECT_ONCE_WARNING(test);
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
		SVCSOCK_EXPECT_ONCE_WARNING(test);
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
	request->rq_arg.bvec = request_state->lt.bvec;
	request->rq_arg.bvec_count = request_state->lt.count;
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
		SVCSOCK_EXPECT_ONCE_WARNING(test);
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
	struct page *pp_page;
	unsigned int refs;
	long pp_refs;

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, page);
	refs = page_ref_count(page);
	state->mode = SVC_TCP_RX_COPY;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->lt.borrowed,
					     state->lt.capacity));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->lt.page_pool,
					     state->lt.capacity));
	svc_tcp_rx_reset(state);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->lt.borrowed,
					     state->lt.capacity));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->lt.page_pool,
					     state->lt.capacity));

	state->mode = SVC_TCP_RX_MIXED;
	state->fixed_bytes = 28;
	state->lt.body_bytes = 28;
	pp_page = svcsock_rx_alloc_pp_page(test, NULL);
	pp_refs = svcsock_rx_pp_refs(pp_page);
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, pp_page, 1, 3, true), 0);
	KUNIT_EXPECT_EQ(test, svcsock_rx_pp_refs(pp_page), pp_refs + 1);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_release_refs(state), 0);
	KUNIT_EXPECT_EQ(test, svcsock_rx_pp_refs(pp_page), pp_refs);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->lt.borrowed,
					     state->lt.capacity));
	KUNIT_EXPECT_TRUE(test, bitmap_empty(state->lt.page_pool,
					     state->lt.capacity));
	svc_tcp_rx_reset(state);

	state->mode = SVC_TCP_RX_MIXED;
	state->fixed_bytes = 28;
	state->lt.body_bytes = 28;
	KUNIT_ASSERT_EQ(test,
			svc_tcp_rx_append_borrowed(state, page, 5, 7, false), 0);
	KUNIT_EXPECT_TRUE(test, test_bit(0, state->lt.borrowed));
	KUNIT_EXPECT_FALSE(test, test_bit(0, state->lt.page_pool));
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

/*
 * The loan tables and rq_bvec are sized for a maximum message in 9000-byte
 * MTU frames, in one slab object each: never a larger object than the
 * page-based sizing takes (260, 272 and 320 entries for 1 MiB on 4 KiB,
 * 16 KiB and 64 KiB pages). The sizes are logged for the record.
 */
static void svcsock_rx_loan_table_fit_test(struct kunit *test)
{
	struct svc_serv serv = { .sv_max_mesg = SZ_1M + PAGE_SIZE };
	unsigned long frames = DIV_ROUND_UP(serv.sv_max_mesg, 8948) + 1;
	unsigned long pages = svc_serv_maxpages(&serv);
	unsigned long entries = svc_serv_maxbvecs(&serv);
	unsigned long by_pages = svc_pages_to_bvecs(pages);
	struct bio_vec *fit, *old;
	struct svc_tcp_rx_state *state;
	size_t fit_obj, old_obj;

	/* Every frame split at every page it can span, plus head and tail. */
	KUNIT_EXPECT_GE(test, entries,
			frames * (DIV_ROUND_UP(8948, PAGE_SIZE) + 1) + 2);
	KUNIT_EXPECT_GE(test, entries, pages);
	if (PAGE_SIZE == SZ_4K)
		KUNIT_EXPECT_EQ(test, entries, 479UL);
	if (PAGE_SIZE == SZ_16K)
		KUNIT_EXPECT_EQ(test, entries, 245UL);
	if (PAGE_SIZE == SZ_64K)
		KUNIT_EXPECT_EQ(test, entries, 255UL);

	fit = kcalloc(entries, sizeof(*fit), GFP_KERNEL);
	old = kcalloc(by_pages, sizeof(*old), GFP_KERNEL);
	state = svc_tcp_rx_state_alloc_entries(entries, pages, GFP_KERNEL,
					       NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, fit);
	KUNIT_ASSERT_NOT_NULL(test, old);
	KUNIT_ASSERT_NOT_NULL(test, state);
	fit_obj = ksize(fit);
	old_obj = ksize(old);
	KUNIT_EXPECT_EQ(test, (unsigned long)state->lt.capacity, entries);
	KUNIT_EXPECT_EQ(test, (unsigned long)state->arena_pages, pages);
	KUNIT_EXPECT_EQ(test, ksize(state->lt.bvec), fit_obj);
	/* One slab object: 8 KiB on 4 KiB pages, 4 KiB on larger ones. */
	KUNIT_EXPECT_LE(test, fit_obj,
			PAGE_SIZE == SZ_4K ? (size_t)SZ_8K : (size_t)SZ_4K);
	KUNIT_EXPECT_LE(test, fit_obj, old_obj);
	kunit_info(test, "P3SIZES page=%lu max_mesg=%u maxpages=%lu fit_entries=%lu fit_bytes=%zu fit_obj=%zu pages_entries=%lu pages_bytes=%zu pages_obj=%zu bitmap_obj=%zu state_obj=%zu",
		   (unsigned long)PAGE_SIZE, serv.sv_max_mesg, pages, entries,
		   entries * sizeof(*fit), fit_obj, by_pages,
		   by_pages * sizeof(*old), old_obj, ksize(state->lt.borrowed),
		   ksize(state));
	svc_tcp_rx_state_free(state);
	kfree(old);
	kfree(fit);
}

static void svcsock_rx_vfree(void *data)
{
	vfree(data);
}

/*
 * A 1 MiB WRITE at a 9000-byte MTU, at production table sizes: each frame
 * arrives as its own skb, its 8948 payload bytes in page fragments filled
 * from the worst in-page offset, so every frame splits at every page it
 * can span. The record completes as a loan: no capacity fallback, every
 * byte in place.
 */
static void svcsock_rx_loan_1m_mtu9000_test(struct kunit *test)
{
	enum { FRAME = 8948, FIRST_LEN = 596 };
	struct svc_serv serv = { .sv_max_mesg = SZ_1M + PAGE_SIZE };
	const unsigned int body_len = 7 * XDR_UNIT + SZ_1M;
	const unsigned int record_len = sizeof(rpc_fraghdr) + body_len;
	const unsigned int head_len = sizeof(rpc_fraghdr) + 7 * XDR_UNIT;
	struct svcsock_rx_fixture *fixture =
		svcsock_rx_alloc_fixture_sized(test, svc_serv_maxbvecs(&serv),
					       svc_serv_maxpages(&serv));
	struct svc_rqst *rqstp = fixture->rqstp;
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;
	struct svcsock_rx_actor_state actor = {};
	read_descriptor_t desc = { .count = record_len };
	unsigned int at, frags = 0, index, cursor;
	struct sk_buff *skb;
	u8 *record;
	int ret;

	record = vmalloc(record_len);
	KUNIT_ASSERT_NOT_NULL(test, record);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, svcsock_rx_vfree,
							record), 0);
	svcsock_rx_fill_record(record, body_len, true, RPC_CALL, RPC_AUTH_UNIX);
	skb = svcsock_rx_build_linear_skb(test, record, head_len);
	ret = svcsock_rx_run_actor(rqstp, &actor, &desc, skb, 0, skb->len);
	KUNIT_ASSERT_EQ(test, ret, (int)head_len);
	for (at = head_len; at < record_len; ) {
		unsigned int frame = min_t(unsigned int, FRAME, record_len - at);
		unsigned int offset = PAGE_SIZE - FIRST_LEN, slot = 0, refs;

		skb = svcsock_rx_build_linear_skb(test, record, 0);
		while (frame) {
			unsigned int len = min_t(unsigned int, frame,
						 PAGE_SIZE - offset);

			svcsock_rx_add_frag(test, skb, slot++, record + at, len,
					    offset, &refs);
			at += len;
			frame -= len;
			offset = 0;
		}
		frags += slot;
		ret = svcsock_rx_run_actor(rqstp, &actor, &desc, skb, 0,
					   skb->len);
		KUNIT_ASSERT_EQ(test, ret, (int)skb->len);
	}
	KUNIT_ASSERT_TRUE(test, actor.complete);
	kunit_info(test, "P3COVER page=%lu frags=%u capacity=%u arena_pages=%u mode=%u(MIXED=2,COPY=3) reason=%u(NONE=0,CAPACITY=12) materialized_bytes=%u count=%u LOAN=%d",
		   (unsigned long)PAGE_SIZE, frags, state->lt.capacity,
		   state->arena_pages, state->mode, state->reason,
		   state->materialized_bytes, state->lt.count,
		   state->mode == SVC_TCP_RX_MIXED &&
		   state->reason == SVC_TCP_RX_REASON_NONE &&
		   !state->materialized_bytes && state->lt.count == frags);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_MIXED);
	KUNIT_EXPECT_EQ(test, state->reason, (u8)SVC_TCP_RX_REASON_NONE);
	KUNIT_EXPECT_EQ(test, state->materialized_bytes, 0U);
	KUNIT_EXPECT_EQ(test, state->lt.count, frags);
	KUNIT_EXPECT_LE(test, state->lt.count, state->lt.capacity);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_publish(fixture->svsk, rqstp), 0);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_PUBLISHED);
	for (index = 0, cursor = head_len; index < state->lt.count; index++) {
		const struct bio_vec *bvec = &state->lt.bvec[index];

		KUNIT_ASSERT_TRUE(test, test_bit(index, state->lt.borrowed));
		KUNIT_ASSERT_EQ(test, memcmp(page_address(bvec->bv_page) +
					     bvec->bv_offset,
					     record + cursor, bvec->bv_len), 0);
		cursor += bvec->bv_len;
	}
	KUNIT_EXPECT_EQ(test, cursor, record_len);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
	KUNIT_EXPECT_EQ(test, state->mode, (u8)SVC_TCP_RX_EMPTY);
}

/* ---- NFSD READ region confinement (svc_tcp_rx_region_read) ---- */

static struct auth_ops svcsock_region_auth = {
	.name		= "kunit",
	.flavour	= RPC_AUTH_UNIX,
};

static void svcsock_region_release_ctxt(struct svc_xprt *xprt, void *ctxt)
{
}

/* What svc_revisit() and free_deferred() need of a dead transport. */
static const struct svc_xprt_ops svcsock_region_ops = {
	.xpo_release_ctxt	= svcsock_region_release_ctxt,
	.xpo_rx_region_unconfine = svc_tcp_rx_region_unconfine_op,
	.xpo_rx_region_park_free = svc_tcp_rx_region_park_free,
};

struct svcsock_region {
	struct svcsock_rx_fixture	*fx;
	struct page			**region;
	struct page			**resp;
	struct page			*orig[16];
	unsigned int			nregion;
	u8				args[64];
};

#define SVCSOCK_REGION_RESP	12

static void svcsock_region_off(void *unused)
{
	svc_tcp_rx_region_kunit_reads(false);
	svc_tcp_rx_budget_kunit_region(0, 0);
	svc_tcp_rx_budget_kunit_init();
	svc_tcp_rx_region_kunit_drain_waiters();
	svc_tcp_rx_region_kunit_flush();
	svc_tcp_rx_region_kunit_read_region(NULL, 0);
}

/*
 * A request on a TCP transport in the initial namespace, with @nregion
 * stand-in READ-partition pages and an initial-namespace budget of @budget
 * pages (no reserve: the oldest READ starts by overrun, 1b).  The share is
 * 10 pages (twice a 4-page maximum READ, each with its alignment page).
 * The topology is shared unless a test forces the split.
 */
static struct svcsock_region *svcsock_region_alloc(struct kunit *test,
						   unsigned int nregion,
						   unsigned int budget)
{
	struct svcsock_region *r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	struct svc_sock *svsk;
	struct svc_rqst *rqstp;
	struct svc_xprt *xprt;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, r);
	r->fx = svcsock_rx_alloc_fixture(test, 8);
	rqstp = r->fx->rqstp;
	svsk = r->fx->svsk;
	xprt = &svsk->sk_xprt;
	kref_init(&xprt->xpt_ref);
	spin_lock_init(&xprt->xpt_lock);
	INIT_LIST_HEAD(&xprt->xpt_deferred);
	set_bit(XPT_BUSY, &xprt->xpt_flags);
	xprt->xpt_ops = &svcsock_region_ops;
	xprt->xpt_net = &init_net;
	xprt->xpt_server->sv_max_payload = 4 * PAGE_SIZE;
	spin_lock_init(&svsk->sk_rx_region_lock);
	INIT_LIST_HEAD(&svsk->sk_rx_region_waiters);

	r->resp = kunit_kcalloc(test, SVCSOCK_REGION_RESP + 1,
				sizeof(*r->resp), GFP_KERNEL);
	rqstp->rq_rx_region_pages = kunit_kcalloc(test, SVCSOCK_REGION_RESP,
						  sizeof(struct page *),
						  GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_rx_region_pages);
	rqstp->rq_rx_region_saved = kunit_kcalloc(test, SVCSOCK_REGION_RESP,
						  sizeof(struct page *),
						  GFP_KERNEL);
	rqstp->rq_rx_region_map = kunit_kcalloc(test,
						BITS_TO_LONGS(SVCSOCK_REGION_RESP),
						sizeof(long), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r->resp);
	KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_rx_region_saved);
	KUNIT_ASSERT_NOT_NULL(test, rqstp->rq_rx_region_map);
	for (i = 0; i < SVCSOCK_REGION_RESP; i++) {
		r->resp[i] = svcsock_rx_alloc_page(test);
		KUNIT_ASSERT_NOT_NULL(test, r->resp[i]);
		r->orig[i] = r->resp[i];
	}
	rqstp->rq_maxpages = SVCSOCK_REGION_RESP;
	rqstp->rq_respages = r->resp;
	rqstp->rq_next_page = r->resp + 1;
	rqstp->rq_page_end = r->resp + SVCSOCK_REGION_RESP;
	rqstp->rq_res.pages = r->resp + 1;
	rqstp->rq_authop = &svcsock_region_auth;
	rqstp->rq_server = xprt->xpt_server;
	rqstp->rq_arg.head[0].iov_base = r->args;
	rqstp->rq_arg.head[0].iov_len = sizeof(r->args);
	rqstp->rq_arg.len = sizeof(r->args);
	rqstp->rq_arg.page_len = 0;

	r->nregion = nregion;
	r->region = kunit_kcalloc(test, nregion, sizeof(*r->region), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r->region);
	for (i = 0; i < nregion; i++) {
		r->region[i] = svcsock_rx_alloc_page(test);
		KUNIT_ASSERT_NOT_NULL(test, r->region[i]);
	}
	/* Registered first, run last: the pages outlive every action. */
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_default_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_region_off, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(budget, 0, 100);
	svc_tcp_rx_region_kunit_read_region(r->region, nregion);
	svc_tcp_rx_region_kunit_reads(true);
	return r;
}

/* One field of @c's stats line, as the module parameter prints it. */
static long svcsock_budget_stat(struct kunit *test, struct svc_tcp_rx_budget *c,
				const char *name)
{
	size_t n = strlen(name);
	char buf[512];
	const char *p = buf;
	long v;

	memset(buf, 0, sizeof(buf));
	svc_tcp_rx_budget_kunit_stats(c, buf, sizeof(buf));
	while (p && *p) {
		if (!strncmp(p, name, n) && p[n] == ' ') {
			KUNIT_ASSERT_EQ(test, sscanf(p + n + 1, "%ld", &v), 1);
			return v;
		}
		p = strchr(p, ' ');
		if (p)
			p++;
	}
	KUNIT_FAIL(test, "no field %s in the budget stats", name);
	return -1;
}

/* One field of a budget's stats line, as the module parameter prints it. */
static long svcsock_region_stat_of(struct kunit *test, struct svc_sock *svsk,
				   enum svc_tcp_rx_kunit_stats which,
				   const char *name)
{
	return svcsock_budget_stat(test,
				   svc_tcp_rx_region_kunit_budget(svsk, which),
				   name);
}

/* A field of the budget READs on @r's transport draw on. */
static long svcsock_region_stat(struct kunit *test,
				struct svcsock_region *r, const char *name)
{
	return svcsock_region_stat_of(test, r->fx->svsk,
				      SVC_TCP_RX_STATS_READ, name);
}

/* Credit the READ budget can still give: what is available plus the reserve. */
static long svcsock_region_credit(struct kunit *test, struct svcsock_region *r)
{
	return svcsock_region_stat(test, r, "avail") +
	       svcsock_region_stat(test, r, "reserve");
}

/* Is @page one of @r's region pages? */
static bool svcsock_region_page(const struct svcsock_region *r,
				const struct page *page)
{
	unsigned int i;

	for (i = 0; i < r->nregion; i++)
		if (r->region[i] == page)
			return true;
	return false;
}

/* What a direct READ does: fill its next @n slots and move past them. */
static void svcsock_region_read(struct kunit *test, struct svc_rqst *rqstp,
				unsigned int n)
{
	KUNIT_ASSERT_TRUE(test, svc_rqst_rx_region_fill(rqstp, n));
	rqstp->rq_next_page += n;
}

/* Send the reply and make the thread ready for its next request. */
static struct ubuf_info *svcsock_region_send(struct svcsock_region *r,
					     unsigned int skbs)
{
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct ubuf_info *ubuf = svc_tcp_rx_region_kunit_send(rqstp, skbs);

	svc_rqst_rx_region_restore(rqstp);
	rqstp->rq_next_page = r->resp + 1;
	return ubuf;
}

/* Take the oldest request replayed on @r's transport, as svc_recv() would. */
static struct svc_deferred_req *svcsock_region_replay(struct kunit *test,
						      struct svcsock_region *r)
{
	struct svc_xprt *xprt = &r->fx->svsk->sk_xprt;
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct svc_deferred_req *dr;

	KUNIT_ASSERT_FALSE(test, list_empty(&xprt->xpt_deferred));
	dr = list_last_entry(&xprt->xpt_deferred, struct svc_deferred_req,
			     handle.recent);
	list_del_init(&dr->handle.recent);
	rqstp->rq_deferred = dr;
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	rqstp->rq_next_page = r->resp + 1;
	return dr;
}

/* Free a parked request the way free_deferred() does. */
static void svcsock_region_free_dr(struct svcsock_region *r,
				   struct svc_deferred_req *dr)
{
	if (r->fx->rqstp->rq_deferred == dr)
		r->fx->rqstp->rq_deferred = NULL;
	if (dr->rx_region.parked)
		svc_tcp_rx_region_park_free(&dr->rx_region);
	kfree(dr);
}

/* Run releases until nothing more is queued (grant chains run in work). */
static void svcsock_region_settle(void)
{
	int i;

	for (i = 0; i < 16; i++)
		svc_tcp_rx_region_kunit_flush();
}

/*
 * K1.  Admission takes pages and credit; each READ installs only what it
 * reads, in its own slots.  The pages and credit return exactly once, when
 * TCP frees the reply's last socket buffer, and the request's own pages go
 * back into their slots.
 */
static void svcsock_rx_region_admit_send_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 8);
	struct svc_rqst *rqstp = r->fx->rqstp;
	long admitted = svcsock_region_stat(test, r, "read_admitted");
	long completed = svcsock_region_stat(test, r, "read_completed");
	struct ubuf_info *ubuf;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 4), 0);
	KUNIT_ASSERT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_used(rqstp));
	KUNIT_EXPECT_PTR_EQ(test, r->resp[1], r->orig[1]);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 4U);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_admitted"),
			admitted + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 4);

	/* Two READs of one COMPOUND: 2 pages, then 1, each in its slots. */
	svcsock_region_read(test, rqstp, 2);
	svcsock_region_read(test, rqstp, 1);
	for (i = 1; i <= 3; i++) {
		KUNIT_EXPECT_TRUE(test, svcsock_region_page(r, r->resp[i]));
		KUNIT_EXPECT_PTR_EQ(test, rqstp->rq_rx_region_saved[i],
				    r->orig[i]);
	}
	KUNIT_EXPECT_PTR_EQ(test, r->resp[4], r->orig[4]);
	/* A third finds one page left, too few for 2: it keeps its own. */
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_fill(rqstp, 2));

	/* Sent: two socket buffers still queued. */
	ubuf = svc_tcp_rx_region_kunit_send(rqstp, 2);
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(rqstp));
	for (i = 1; i <= 3; i++)
		KUNIT_EXPECT_NULL(test, r->resp[i]);
	svc_tcp_rx_region_kunit_flush();
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 4U);

	/* TCP frees one, then (purged, copied) the last. */
	ubuf->ops->complete(NULL, ubuf, true);
	svc_tcp_rx_region_kunit_flush();
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 4U);
	ubuf->ops->complete(NULL, ubuf, false);
	svc_tcp_rx_region_kunit_flush();
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_completed"),
			completed + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 8);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->fx->svsk->sk_rx_region_inflight),
			0);

	/* The thread gets its displaced pages back after release. */
	svc_rqst_rx_region_restore(rqstp);
	for (i = 1; i <= 3; i++)
		KUNIT_EXPECT_PTR_EQ(test, r->resp[i], r->orig[i]);
}

/*
 * K2.  Unconfined before any READ used the pages (a file without direct
 * I/O, or a reply never sent): pages, credit and the thread's pages back at
 * once.
 */
static void svcsock_rx_region_unconfine_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 8);
	struct svc_rqst *rqstp = r->fx->rqstp;
	long unconfined = svcsock_region_stat(test, r, "read_unconfined");

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	svcsock_region_read(test, rqstp, 2);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 6U);
	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_PTR_EQ(test, r->resp[1], r->orig[1]);
	KUNIT_EXPECT_PTR_EQ(test, r->resp[2], r->orig[2]);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 8);
	/* A second release finds nothing to return. */
	svc_rqst_rx_region_unconfine(rqstp, true);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_unconfined"),
			unconfined);
	/* Served from ordinary pages after admission: counted (m1). */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	svc_rqst_rx_region_unconfine(rqstp, true);
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_unconfined"),
			unconfined + 1);
}

/*
 * K3.  A frag downgraded from managed holds a page reference past the
 * reply's completion: the pages wait for it rather than leaving the region.
 */
static void svcsock_rx_region_busy_page_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 4, 4);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct page *held;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	svcsock_region_read(test, rqstp, 2);
	held = r->resp[1];
	get_page(held);
	svc_tcp_rx_region_kunit_send(rqstp, 0);
	svc_tcp_rx_region_kunit_flush();
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 2U);
	put_page(held);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 4U);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
}

/*
 * K4.  A READ without credit parks off-thread, and a younger READ queues
 * behind it even though it would fit.  Returned credit is granted oldest
 * first, before the replay, which takes the pages.  There is no reserve
 * (1b): B needs 3 of the 1 credit left, C needs 2, and A's return of 3
 * grants B only, because C then waits behind a READ that holds credit.
 */
static void svcsock_rx_region_park_grant_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 4);
	struct svc_rqst *rqstp = r->fx->rqstp;
	long parked = svcsock_region_stat(test, r, "read_parked");
	long granted = svcsock_region_stat(test, r, "read_granted");
	struct svc_deferred_req *b, *c;
	struct ubuf_info *ubuf;

	/* A takes 3 of 4. */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), 0);
	svcsock_region_read(test, rqstp, 3);
	ubuf = svcsock_region_send(r, 1);

	/* B wants 3 of the 1 credit left: parked, no reply. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), -EAGAIN);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_DROPME, &rqstp->rq_flags));
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(rqstp));
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	/* C wants 2, which is not there either, and B is older. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_parked"),
			parked + 2);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), 2);
	KUNIT_EXPECT_TRUE(test, list_empty(&r->fx->svsk->sk_xprt.xpt_deferred));

	/*
	 * A's reply is freed: B, the oldest, is granted credit and queued for
	 * replay; C, behind a READ that holds credit, waits.  Pages are taken
	 * by the replay, not the grant.
	 */
	ubuf->ops->complete(NULL, ubuf, true);
	svc_tcp_rx_region_kunit_flush();
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_granted"),
			granted + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), 1);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);

	/* B replays and takes its 3 pages; its release grants C. */
	b = svcsock_region_replay(test, r);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), 0);
	KUNIT_EXPECT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 5U);
	svcsock_region_read(test, rqstp, 3);
	KUNIT_EXPECT_TRUE(test, svcsock_region_page(r, r->resp[1]));
	KUNIT_EXPECT_TRUE(test, svcsock_region_page(r, r->resp[3]));
	svc_tcp_rx_region_unconfine(rqstp);
	svcsock_region_free_dr(r, b);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_granted"),
			granted + 2);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);

	c = svcsock_region_replay(test, r);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	KUNIT_EXPECT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	svc_tcp_rx_region_unconfine(rqstp);
	svcsock_region_free_dr(r, c);

	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 4);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), 0);
}

/* K5.  A granted READ whose request is dropped before replay returns its credit. */
static void svcsock_rx_region_park_drop_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 4);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct svc_deferred_req *dr;
	struct ubuf_info *ubuf;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 4), 0);
	ubuf = svcsock_region_send(r, 1);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	ubuf->ops->complete(NULL, ubuf, true);
	svc_tcp_rx_region_kunit_flush();
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 1);

	/* The drop returns the credit at once; its kick runs in work. */
	dr = svcsock_region_replay(test, r);
	svcsock_region_free_dr(r, dr);
	svc_tcp_rx_region_kunit_flush();
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 4);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
}

/*
 * K6.  Many READs parked on a transport that dies: each grant's revisit
 * frees the request, whose credit returns through the kick work, which
 * grants the next.  The chain runs iteratively, not by recursion, and
 * everything comes back.
 */
static void svcsock_rx_region_dead_chain_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 8);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct ubuf_info *ubuf;
	unsigned int i;

	/* A large share: the READs wait for credit, not for the connection. */
	r->fx->svsk->sk_xprt.xpt_server->sv_max_payload = 64 * PAGE_SIZE;
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 8), 0);
	ubuf = svcsock_region_send(r, 1);
	for (i = 0; i < 6; i++) {
		KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2),
				-EAGAIN);
		clear_bit(RQ_DROPME, &rqstp->rq_flags);
	}
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), 6);
	set_bit(XPT_DEAD, &r->fx->svsk->sk_xprt.xpt_flags);
	svc_tcp_rx_region_kunit_conn_drain(r->fx->svsk);
	ubuf->ops->complete(NULL, ubuf, true);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 8);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->fx->svsk->sk_rx_region_inflight),
			0);
	KUNIT_EXPECT_TRUE(test, list_empty(&r->fx->svsk->sk_xprt.xpt_deferred));
}

/*
 * K7.  The connection share is a hard bound: a READ that would take the
 * connection past it waits on the connection, and is admitted afresh once a
 * reply returns.  A closing connection drops its waiters.
 */
static void svcsock_rx_region_share_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 11, 11);
	struct svc_sock *svsk = r->fx->svsk;
	struct svc_rqst *rqstp = r->fx->rqstp;
	long stalls = svcsock_region_stat(test, r, "share_stalls");
	struct svc_deferred_req *dr;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 7), 0);
	KUNIT_EXPECT_FALSE(test, svsk->sk_rx_region_stalled);
	/* 7 + 4 > 10: B waits on the connection, not the pool. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 4), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	KUNIT_EXPECT_EQ(test,
			list_count_nodes(&svsk->sk_rx_region_waiters), 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&svsk->sk_rx_region_inflight), 7);

	/* A returns: B replays and is admitted afresh. */
	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_TRUE(test, list_empty(&svsk->sk_rx_region_waiters));
	dr = svcsock_region_replay(test, r);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 4), 0);
	KUNIT_EXPECT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_EQ(test, atomic_read(&svsk->sk_rx_region_inflight), 4);
	svc_tcp_rx_region_unconfine(rqstp);
	svcsock_region_free_dr(r, dr);

	/*
	 * At the share the connection takes no new request, but it still
	 * replays deferred ones: a READ granted credit while parked holds
	 * credit and share, and only its reply returns them (else every
	 * parked READ waits for ever).  has_wspace reads the socket's flags
	 * once past the stall check, so a stand-in socket covers the checks.
	 */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 10), 0);
	KUNIT_EXPECT_TRUE(test, svsk->sk_rx_region_stalled);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "share_stalls"),
			stalls + 1);
	{
		struct socket *saved = svsk->sk_sock, *sock;
		LIST_HEAD(deferred);
		int held;

		sock = kunit_kzalloc(test, sizeof(*sock), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, sock);
		svsk->sk_sock = sock;
		/* XPT_DEFERRED alone (left set after a replay) admits nothing. */
		set_bit(XPT_DEFERRED, &svsk->sk_xprt.xpt_flags);
		KUNIT_EXPECT_TRUE(test, list_empty(&svsk->sk_xprt.xpt_deferred));
		KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_has_wspace(svsk),
				0);
		list_add(&deferred, &svsk->sk_xprt.xpt_deferred);
		KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_has_wspace(svsk),
				1);
		list_del(&deferred);
		clear_bit(XPT_DEFERRED, &svsk->sk_xprt.xpt_flags);

		/*
		 * The reply returning the last pages can run between a READ's
		 * reservation and its stall flag, and see no stall: the flag
		 * is checked again once set, so it never outlives the pages.
		 * Neither recheck counts a stall that did not stick twice.
		 */
		held = atomic_read(&svsk->sk_rx_region_inflight);
		WRITE_ONCE(svsk->sk_rx_region_stalled, false);
		atomic_sub(held, &svsk->sk_rx_region_inflight);
		svc_rx_region_share_stall(svsk, 10, held, 0);
		KUNIT_EXPECT_FALSE(test, svsk->sk_rx_region_stalled);
		KUNIT_EXPECT_EQ(test,
				svcsock_region_stat(test, r, "share_stalls"),
				stalls + 1);
		atomic_add(held, &svsk->sk_rx_region_inflight);
		svc_rx_region_share_stall(svsk, 10, held, 0);
		KUNIT_EXPECT_TRUE(test, svsk->sk_rx_region_stalled);
		KUNIT_EXPECT_EQ(test,
				svcsock_region_stat(test, r, "share_stalls"),
				stalls + 2);
		svsk->sk_sock = saved;
	}
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 1), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	set_bit(XPT_DEAD, &svsk->sk_xprt.xpt_flags);
	svc_tcp_rx_region_kunit_conn_drain(svsk);
	KUNIT_EXPECT_TRUE(test, list_empty(&svsk->sk_rx_region_waiters));
	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_FALSE(test, svsk->sk_rx_region_stalled);
	KUNIT_EXPECT_EQ(test, atomic_read(&svsk->sk_rx_region_inflight), 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 11U);
}

/*
 * Park READs of @sizes pages on @r's transport behind a receive waiter, so
 * they reserve the share and wait for credit, then return the credit.  The
 * connection is at its share; every READ, granted while parked, still
 * replays and is admitted, and the share and credit come back whole.
 */
static void svcsock_region_park_at_share(struct kunit *test,
					 struct svcsock_region *r,
					 const unsigned int *sizes,
					 unsigned int n)
{
	struct svc_tcp_rx_state *s1 = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *s2 = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sr = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *svsk = r->fx->svsk;
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct socket *saved = svsk->sk_sock, *sock;
	struct svc_deferred_req *dr;
	unsigned int i;

	sock = kunit_kzalloc(test, sizeof(*sock), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sock);
	/* Budget 12: a 7-page record (cost 8) admits, a second one waits. */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(12), 0);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sr, s1, 7 * PAGE_SIZE));
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_credit_admit(sr, s2, 7 * PAGE_SIZE));
	KUNIT_ASSERT_FALSE(test, list_empty(&sr->sk_rx_wait.node));

	for (i = 0; i < n; i++) {
		KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, sizes[i]),
				-EAGAIN);
		clear_bit(RQ_DROPME, &rqstp->rq_flags);
	}
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), n);
	KUNIT_EXPECT_TRUE(test, svsk->sk_rx_region_stalled);

	/* The credit returns; the woken receive takes and returns its own. */
	svc_tcp_rx_credit_release(s1);
	svcsock_region_settle();
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sr, s2, 7 * PAGE_SIZE));
	svc_tcp_rx_credit_release(s2);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), 0);
	KUNIT_EXPECT_EQ(test, list_count_nodes(&svsk->sk_xprt.xpt_deferred), n);

	/* Still stalled, the connection must hand its grants to a thread. */
	svsk->sk_sock = sock;
	KUNIT_EXPECT_TRUE(test, svsk->sk_rx_region_stalled);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_has_wspace(svsk), 1);
	svsk->sk_sock = saved;
	for (i = 0; i < n; i++) {
		dr = svcsock_region_replay(test, r);
		KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, sizes[i]),
				0);
		KUNIT_EXPECT_TRUE(test, svc_rqst_rx_region_held(rqstp));
		svc_tcp_rx_region_unconfine(rqstp);
		svcsock_region_free_dr(r, dr);
	}
	svcsock_region_settle();
	KUNIT_EXPECT_FALSE(test, svsk->sk_rx_region_stalled);
	KUNIT_EXPECT_EQ(test, atomic_read(&svsk->sk_rx_region_inflight), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 12);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), r->nregion);
}

/*
 * K20 (review-p4-r1 M1).  Two maximum READs park for credit and together
 * reach the share exactly; a READ larger than a small share (a connection
 * holding nothing may take one) parks the same way.  Once granted, the
 * stalled connection still replays them; before the fix nothing woke it.
 */
static void svcsock_rx_region_share_park_test(struct kunit *test)
{
	static const unsigned int two_max[] = { 5, 5 }, one_large[] = { 7 };
	struct svcsock_region *r = svcsock_region_alloc(test, 12, 12);

	/* sv_max_payload 4 pages: a maximum READ is 5, the share 10. */
	svcsock_region_park_at_share(test, r, two_max, ARRAY_SIZE(two_max));
	/* sv_max_payload 2 pages: the share is 6, the READ 7. */
	r->fx->svsk->sk_xprt.xpt_server->sv_max_payload = 2 * PAGE_SIZE;
	svcsock_region_park_at_share(test, r, one_large, ARRAY_SIZE(one_large));
}

/*
 * K21 (review-p4-r1 m4).  A READ that fits the partition but not what it
 * can still lend once pages escaped is served unconfined, without parking;
 * one that fits the shrunk capacity is still admitted.
 */
static void svcsock_rx_region_escaped_capacity_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 8);
	struct svc_rqst *rqstp = r->fx->rqstp;
	long unconfined = svcsock_region_stat(test, r, "read_unconfined");
	long parked = svcsock_region_stat(test, r, "read_parked");

	r->fx->svsk->sk_xprt.xpt_server->sv_max_payload = 8 * PAGE_SIZE;
	svc_tcp_rx_region_kunit_escape(3);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 6), 0);
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_unconfined"),
			unconfined + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_parked"),
			parked);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->fx->svsk->sk_rx_region_inflight),
			0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 5), 0);
	KUNIT_EXPECT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 8);
}

/*
 * K8.  Off, a call that cannot be parked, a READ larger than the reply
 * pages, and a READ larger than the region's read partition (1b: no longer
 * larger than the budget, which the oldest READ overruns) are served
 * unconfined.
 */
static void svcsock_rx_region_unconfined_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 4);
	struct svc_rqst *rqstp = r->fx->rqstp;
	long unconfined = svcsock_region_stat(test, r, "read_unconfined");

	svc_tcp_rx_region_kunit_reads(false);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(rqstp));
	svc_tcp_rx_region_kunit_reads(true);
	rqstp->rq_arg.page_len = PAGE_SIZE;	/* a WRITE payload rides along */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(rqstp));
	rqstp->rq_arg.page_len = 0;
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp,
						     SVCSOCK_REGION_RESP), 0);
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(rqstp));
	/* 9 pages fit the reply but not the 8-page partition. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 9), 0);
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_unconfined"),
			unconfined + 3);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->fx->svsk->sk_rx_region_inflight),
			0);
}

/*
 * K9.  The send picks zero-copy bvecs by reply slot, never by address: with
 * a head, a tail, ordinary pages around the region page, and a pages base
 * other than the first payload slot.
 */
static void svcsock_rx_region_send_map_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 4, 4);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct xdr_buf *res = &rqstp->rq_res;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 1), 0);
	rqstp->rq_next_page = r->resp + 2;
	KUNIT_ASSERT_TRUE(test, svc_rqst_rx_region_fill(rqstp, 1));	/* slot 2 */

	/* marker, head, pages in slots 1..3, tail */
	res->head[0].iov_len = 100;
	res->pages = r->resp + 1;
	res->page_len = 3 * PAGE_SIZE;
	res->tail[0].iov_len = 4;
	KUNIT_EXPECT_FALSE(test, svc_rx_region_send_bvec(rqstp, 6, 0));
	KUNIT_EXPECT_FALSE(test, svc_rx_region_send_bvec(rqstp, 6, 1));
	KUNIT_EXPECT_FALSE(test, svc_rx_region_send_bvec(rqstp, 6, 2));
	KUNIT_EXPECT_TRUE(test, svc_rx_region_send_bvec(rqstp, 6, 3));
	KUNIT_EXPECT_FALSE(test, svc_rx_region_send_bvec(rqstp, 6, 4));
	KUNIT_EXPECT_FALSE(test, svc_rx_region_send_bvec(rqstp, 6, 5));

	/* No head, and pages starting at slot 2. */
	res->head[0].iov_len = 0;
	res->pages = r->resp + 2;
	res->page_len = 2 * PAGE_SIZE;
	KUNIT_EXPECT_FALSE(test, svc_rx_region_send_bvec(rqstp, 4, 0));
	KUNIT_EXPECT_TRUE(test, svc_rx_region_send_bvec(rqstp, 4, 1));
	KUNIT_EXPECT_FALSE(test, svc_rx_region_send_bvec(rqstp, 4, 2));
	KUNIT_EXPECT_FALSE(test, svc_rx_region_send_bvec(rqstp, 4, 3));

	res->head[0].iov_len = 0;
	res->tail[0].iov_len = 0;
	res->page_len = 0;
	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 4U);
}

/*
 * K10.  Every connection waiter is replayed when a reply returns, and each
 * that fits is admitted; a READ parking on a dead connection is dropped at
 * once.
 */
static void svcsock_rx_region_conn_wake_test(struct kunit *test)
{
	/* Credit to spare: only the connection share holds READs back. */
	struct svcsock_region *r = svcsock_region_alloc(test, 11, 20);
	struct svc_sock *svsk = r->fx->svsk;
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct svc_deferred_req *b, *c;
	struct ubuf_info *ubuf;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 7), 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 4), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 4), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	KUNIT_EXPECT_EQ(test,
			list_count_nodes(&svsk->sk_rx_region_waiters), 2);

	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_TRUE(test, list_empty(&svsk->sk_rx_region_waiters));
	KUNIT_EXPECT_EQ(test, list_count_nodes(&svsk->sk_xprt.xpt_deferred), 2);
	b = svcsock_region_replay(test, r);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 4), 0);
	KUNIT_EXPECT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	/* Hold B's reply; C fits beside it (4 + 4 <= 10). */
	ubuf = svc_tcp_rx_region_kunit_send(rqstp, 1);
	svcsock_region_free_dr(r, b);
	c = svcsock_region_replay(test, r);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 4), 0);
	KUNIT_EXPECT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_EQ(test, atomic_read(&svsk->sk_rx_region_inflight), 8);
	svc_tcp_rx_region_unconfine(rqstp);
	svcsock_region_free_dr(r, c);

	/* The connection dies with B's reply out: a new waiter is dropped. */
	set_bit(XPT_DEAD, &svsk->sk_xprt.xpt_flags);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 7), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	KUNIT_EXPECT_TRUE(test, list_empty(&svsk->sk_rx_region_waiters));
	KUNIT_EXPECT_TRUE(test, list_empty(&svsk->sk_xprt.xpt_deferred));

	/* B's reply is freed: everything is back. */
	ubuf->ops->complete(NULL, ubuf, true);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&svsk->sk_rx_region_inflight), 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 11U);
}

/*
 * K11.  A page still referenced at the busy limit leaves with the reply:
 * the reply's credit and share return, so the pool does not stay short.
 */
static void svcsock_rx_region_busy_escape_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 4, 4);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct ubuf_info *ubuf;
	struct page *held;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	svcsock_region_read(test, rqstp, 2);
	held = r->resp[1];
	get_page(held);
	ubuf = svc_tcp_rx_region_kunit_send(rqstp, 1);
	svc_tcp_rx_region_kunit_age(ubuf);
	ubuf->ops->complete(NULL, ubuf, true);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 4);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->fx->svsk->sk_rx_region_inflight),
			0);
	put_page(held);
}

/*
 * K12.  Shared topology: a READ draws the budget its transport's namespace
 * has, else the default budget, else (Q-A, no budget anywhere) the global
 * read pool.  Only the initial namespace can be driven here: another
 * namespace's budget needs a real net, so its case is covered by the same
 * svc_tcp_rx_budget_for() selection with the initial namespace's budget
 * off.
 */
static void svcsock_rx_region_shared_budget_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 8);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct svc_sock *svsk = r->fx->svsk;
	long init = svcsock_region_stat_of(test, svsk, SVC_TCP_RX_STATS_INIT,
					   "read_admitted");
	long dflt = svcsock_region_stat_of(test, svsk,
					   SVC_TCP_RX_STATS_DEFAULT,
					   "read_admitted");
	long pool = svcsock_region_stat_of(test, svsk, SVC_TCP_RX_STATS_POOL,
					   "read_admitted");

	/* The namespace's own budget. */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	KUNIT_ASSERT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_INIT, "read_inflight"), 1);
	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_INIT, "read_admitted"), init + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_DEFAULT, "read_admitted"), dflt);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_POOL, "read_admitted"), pool);

	/* No budget there: the default one. */
	svc_tcp_rx_budget_kunit_configure(0, 0, 100);
	svc_tcp_rx_budget_kunit_rcvbuf(128 * 1024);
	svc_tcp_rx_budget_kunit_default(6, 0);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	KUNIT_ASSERT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_DEFAULT, "read_inflight"), 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_DEFAULT, "avail"), 4);
	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_DEFAULT, "read_admitted"), dflt + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_INIT, "read_admitted"), init + 1);

	/* No budget anywhere: the global read pool, unmetered. */
	svc_tcp_rx_budget_kunit_default(0, 0);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	KUNIT_ASSERT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_POOL, "read_admitted"), pool + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_DEFAULT, "read_admitted"), dflt + 1);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
}

/*
 * K13.  RQ_SECURE is saved when a READ first parks and kept by every later
 * park of the same request, whatever the thread running it has set.  The
 * restore itself (svc_deferred_recv()) is static in svc_xprt.c and not
 * reachable from here: this checks what it restores from.
 */
static void svcsock_rx_region_park_secure_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 20);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct svc_sock *svsk = r->fx->svsk;
	struct svc_deferred_req *dr;
	struct ubuf_info *ua, *uc;

	/* Share 4: A (2) and C (2) fill it; B (3) waits on the connection. */
	svsk->sk_xprt.xpt_server->sv_max_payload = PAGE_SIZE;
	set_bit(RQ_SECURE, &rqstp->rq_flags);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	ua = svcsock_region_send(r, 1);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	/* Later requests come in on a thread without the bit. */
	clear_bit(RQ_SECURE, &rqstp->rq_flags);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	uc = svcsock_region_send(r, 1);

	/* A returns: B is replayed, still marked secure, and parks again. */
	ua->ops->complete(NULL, ua, true);
	svcsock_region_settle();
	dr = svcsock_region_replay(test, r);
	KUNIT_EXPECT_TRUE(test, dr->secure);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	KUNIT_EXPECT_NULL(test, rqstp->rq_deferred);
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_SECURE, &rqstp->rq_flags));
	KUNIT_EXPECT_TRUE(test, dr->secure);
	KUNIT_EXPECT_EQ(test,
			list_count_nodes(&svsk->sk_rx_region_waiters), 1);

	/* C returns: the second replay is admitted. */
	uc->ops->complete(NULL, uc, true);
	svcsock_region_settle();
	KUNIT_ASSERT_PTR_EQ(test, svcsock_region_replay(test, r), dr);
	KUNIT_EXPECT_TRUE(test, dr->secure);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), 0);
	KUNIT_EXPECT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	svc_tcp_rx_region_unconfine(rqstp);
	svcsock_region_free_dr(r, dr);
	KUNIT_EXPECT_EQ(test, atomic_read(&svsk->sk_rx_region_inflight), 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
}

/*
 * K14.  Split topology: READs draw the global read pool, sized to the
 * region's read partition and unaffected by writes to the receive budgets;
 * its stats line carries the READ fields.
 */
static void svcsock_rx_region_split_pool_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 20);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct svc_sock *svsk = r->fx->svsk;
	long init = svcsock_region_stat_of(test, svsk, SVC_TCP_RX_STATS_INIT,
					   "read_admitted");
	long pool = svcsock_region_stat_of(test, svsk, SVC_TCP_RX_STATS_POOL,
					   "read_admitted");
	long done = svcsock_region_stat_of(test, svsk, SVC_TCP_RX_STATS_POOL,
					   "read_completed");
	struct ubuf_info *ubuf;

	/* A 64-page region with its last 8 pages split off for reads. */
	svc_tcp_rx_budget_kunit_region(64, 8);
	/* The suite's default budget alone would overrun its write partition. */
	svc_tcp_rx_budget_kunit_default(0, 0);
	svc_tcp_rx_budget_kunit_init();
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), 0);
	KUNIT_ASSERT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	ubuf = svcsock_region_send(r, 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "budget"), 8);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "reserve_size"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "avail"), 5);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_POOL, "read_admitted"), pool + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_INIT, "read_admitted"), init);

	/* The receive budget moves; the read pool does not. */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(12), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_INIT, "budget"), 12);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "budget"), 8);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "avail"), 5);

	ubuf->ops->complete(NULL, ubuf, true);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "avail"), 8);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat_of(test, svsk,
			 SVC_TCP_RX_STATS_POOL, "read_completed"), done + 1);
}

/*
 * K15.  With no reserve the oldest READ with nothing in flight starts by
 * overrun, and is counted; a younger READ does not overrun but waits; and
 * the reply returns only real credit, so the pool is never inflated.
 */
static void svcsock_rx_region_overrun_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 2);
	struct svc_rqst *rqstp = r->fx->rqstp;
	long overrun = svcsock_region_stat(test, r, "overrun");
	struct svc_deferred_req *dr;
	struct ubuf_info *ubuf;

	/* 3 pages on a budget of 2: the oldest starts, 1 page over. */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), 0);
	KUNIT_ASSERT_TRUE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "overrun"),
			overrun + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 0);
	ubuf = svcsock_region_send(r, 1);

	/* A younger READ waits for credit instead of overrunning. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 1), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "overrun"),
			overrun + 1);

	/* A's reply returns its 2 real pages of credit, not 3: B holds 1. */
	ubuf->ops->complete(NULL, ubuf, true);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 1);
	dr = svcsock_region_replay(test, r);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 1), 0);
	svc_tcp_rx_region_unconfine(rqstp);
	svcsock_region_free_dr(r, dr);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 2);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
}

/*
 * K16.  A live budget change with READs in flight moves the free count by
 * the change (it may go negative) and, once every READ has returned, leaves
 * it exactly at the new budget.
 */
static void svcsock_rx_region_live_change_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 6);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct ubuf_info *ua, *ub;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), 0);
	ua = svcsock_region_send(r, 1);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	ub = svcsock_region_send(r, 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 1);

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(10), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "budget"), 10);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 5);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(4), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "budget"), 4);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), -1);

	ua->ops->complete(NULL, ua, true);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 2);
	ub->ops->complete(NULL, ub, true);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 4);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
}

/*
 * K17.  A granted park dropped (its transport died) returns its credit at
 * once and passes it on from the budget's own kick work, which the test
 * flushes as net_exit does before freeing the budget: the next waiter is
 * granted and nothing is left pending.  (The net_exit flush itself needs a
 * real namespace and is not driven here.)
 */
static void svcsock_rx_region_drop_kick_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 4);
	struct svc_rqst *rqstp = r->fx->rqstp;
	long granted = svcsock_region_stat(test, r, "read_granted");
	struct svc_deferred_req *b, *c;
	struct ubuf_info *ubuf;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), 0);
	ubuf = svcsock_region_send(r, 1);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);

	/* A returns: B granted (3 of 4), C (2) still short. */
	ubuf->ops->complete(NULL, ubuf, true);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_granted"),
			granted + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), 1);

	/* B is dropped unreplayed; the kick grants C. */
	b = svcsock_region_replay(test, r);
	svcsock_region_free_dr(r, b);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_granted"),
			granted + 2);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 1);

	c = svcsock_region_replay(test, r);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	svc_tcp_rx_region_unconfine(rqstp);
	svcsock_region_free_dr(r, c);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 4);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
}

/*
 * K18.  A receive admit is denied while an older READ is parked on the
 * budget, queues behind it, and is woken once the READ is granted.
 */
static void svcsock_rx_region_recv_behind_read_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 6);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct svc_tcp_rx_state *st = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sr = svcsock_rx_credit_sock_forget(test);
	struct svc_deferred_req *dr;
	struct ubuf_info *ubuf;

	/* Same budget, with transport wakes on (the helper mutes them). */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_set(6), 0);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), 0);
	ubuf = svcsock_region_send(r, 1);
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 4), -EAGAIN);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);

	/* The record would fit (cost 2, 3 free) but the READ is older. */
	KUNIT_EXPECT_FALSE(test, svc_tcp_rx_credit_admit(sr, st, PAGE_SIZE));
	KUNIT_EXPECT_FALSE(test, list_empty(&sr->sk_rx_wait.node));
	KUNIT_EXPECT_FALSE(test, st->cr.linked);
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_DATA, &sr->sk_xprt.xpt_flags));

	/* A's credit returns: the READ is granted, and the transport woken. */
	ubuf->ops->complete(NULL, ubuf, true);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_waiters"), 0);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_DATA, &sr->sk_xprt.xpt_flags));
	KUNIT_EXPECT_EQ(test, kref_read(&sr->sk_xprt.xpt_ref), 1U);

	dr = svcsock_region_replay(test, r);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 4), 0);
	svc_tcp_rx_region_unconfine(rqstp);
	svcsock_region_free_dr(r, dr);

	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_admit(sr, st, PAGE_SIZE));
	KUNIT_EXPECT_TRUE(test, st->cr.linked);
	svc_tcp_rx_credit_release(st);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 6);
}

/*
 * K19 (Q-D).  A partition short of pages despite credit (device rings and
 * caches hold pages beyond the budget): the READ's credit and share go
 * back at once, it is counted as short and unconfined, and the reply that
 * holds the pages is untouched.
 */
static void svcsock_rx_region_read_short_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 4, 12);
	struct svc_rqst *rqstp = r->fx->rqstp;
	long shorts = svcsock_region_stat(test, r, "read_short");
	long unconfined = svcsock_region_stat(test, r, "read_unconfined");
	struct ubuf_info *ubuf;

	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), 0);
	ubuf = svcsock_region_send(r, 1);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 1U);

	/* 3 pages of credit are there, 1 page is. */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(rqstp, 3), 0);
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(rqstp));
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_short"),
			shorts + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_unconfined"),
			unconfined + 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 9);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->fx->svsk->sk_rx_region_inflight),
			3);

	ubuf->ops->complete(NULL, ubuf, true);
	svcsock_region_settle();
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 4U);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), 12);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->fx->svsk->sk_rx_region_inflight),
			0);
}

/*
 * Per-CPU credit stocks.  The cases pin themselves to the CPU they run on,
 * so the stock they fill is the one they take from.
 */
static int svcsock_rx_stock_pin(struct kunit *test)
{
	int cpu = raw_smp_processor_id();

	KUNIT_ASSERT_EQ(test, set_cpus_allowed_ptr(current, cpumask_of(cpu)),
			0);
	return raw_smp_processor_id();
}

/* Another online CPU than @cpu, or -1 with only one. */
static int svcsock_rx_stock_other(int cpu)
{
	unsigned int other = cpumask_any_but(cpu_online_mask, cpu);

	return other < nr_cpu_ids ? (int)other : -1;
}

static long svcsock_rx_stock(struct svc_tcp_rx_budget *c,
			     enum svc_tcp_rx_kunit_stock_op op, long n)
{
	return svc_tcp_rx_budget_kunit_stock(c, op, n);
}

/*
 * Port of the old stock case, on the budget READs on the fixture's
 * transport draw on: a refill stocks a batch; takes and puts move credit
 * without the lock; a put beyond 64 spills all but a batch.  The first
 * waiter's drain returns every stock and stops stocking, so a READ takes
 * the budget's ordered path, and its return with nobody waiting turns
 * stocking back on.  The budget's credit (stocked included) never changes.
 */
static void svcsock_rx_credit_stock_test(struct kunit *test)
{
	struct svcsock_region *r = svcsock_region_alloc(test, 8, 256);
	struct svc_rqst *rqstp = r->fx->rqstp;
	struct svc_tcp_rx_budget *c;
	long full, v[8];

	svcsock_rx_stock_pin(test);
	c = svc_tcp_rx_region_kunit_budget(r->fx->svsk, SVC_TCP_RX_STATS_READ);
	/* The first READ applies the configuration. */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 1), 0);
	svc_tcp_rx_region_unconfine(rqstp);
	full = svcsock_region_credit(test, r);
	KUNIT_ASSERT_EQ(test, full, 256);
	/* Start from empty stocks with stocking on. */
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_DRAIN, 0);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	KUNIT_ASSERT_FALSE(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_CONTENDED, 0));

	v[0] = svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0);
	v[1] = svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_FILL, 0);
	v[2] = svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_TAKE, 20);
	v[3] = svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_TAKE, 20);
	v[4] = svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_PUT, 20);
	/* 60 held while refills top the stock up: returning them spills. */
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_TAKE, 30);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_FILL, 0);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_TAKE, 30);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_FILL, 0);
	v[5] = svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0);
	v[6] = svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_PUT, 60);
	v[7] = svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0);
	KUNIT_EXPECT_EQ(test, v[0], 0L);
	KUNIT_EXPECT_EQ(test, v[1], 32L);	/* a batch */
	KUNIT_EXPECT_EQ(test, v[2], 1L);
	KUNIT_EXPECT_EQ(test, v[3], 0L);	/* 12 left */
	KUNIT_EXPECT_EQ(test, v[4], 0L);	/* back to 32, no spill */
	KUNIT_EXPECT_EQ(test, v[5], 36L);	/* 2 + 32, less 30, + 32 */
	KUNIT_EXPECT_EQ(test, v[6], 64L);	/* 96 spills all but a batch */
	KUNIT_EXPECT_EQ(test, v[7], 32L);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), full);

	/* The first waiter's drain: every stock back, stocking off. */
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_DRAIN, 0);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 0L);
	KUNIT_EXPECT_TRUE(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_CONTENDED, 0));
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_FILL, 0), 0L);
	KUNIT_EXPECT_FALSE(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_TAKE, 1));
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), full);

	/* A READ takes the ordered path; its return, nobody waiting, ends it. */
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 1), 0);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 1);
	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_FALSE(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_CONTENDED, 0));
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), full);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);

	/* A READ from the stock: lockless, counted, and its credit back. */
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_FILL, 0);
	v[0] = svcsock_region_stat(test, r, "read_admitted");
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_region_read(rqstp, 2), 0);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 30L);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 1);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_admitted"),
			v[0] + 1);
	svc_tcp_rx_region_unconfine(rqstp);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 32L);
	KUNIT_EXPECT_EQ(test, svcsock_region_stat(test, r, "read_inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_region_credit(test, r), full);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_kunit_read_free(), 8U);
}

static struct svc_tcp_rx_budget *svcsock_rx_stock_init_budget(void)
{
	return svc_tcp_rx_region_kunit_budget(NULL, SVC_TCP_RX_STATS_INIT);
}

/*
 * A receive record admitted from a stock is off the in-flight list but
 * counted in the inflight and admitted stats.  A charge beyond its stock
 * links it as the youngest record.  While a stocked record holds credit no
 * other record is the oldest, so the reserve is refused to a newer one
 * until it returns.  An orphaned stocked record keeps its credit spent and
 * leaves the in-flight list intact.
 */
static void svcsock_rx_credit_stock_record_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *big = svcsock_rx_alloc_state(test, 300);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sc = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;		/* costs 11 */
	struct svc_tcp_rx_budget *c;
	long admitted, orphaned;

	/*
	 * It orphans a record of the initial namespace's budget: under a
	 * carved region that credit stays spent in the live consumer's share.
	 */
	if (page_pool_rx_region_exists())
		kunit_skip(test, "a region is carved already");
	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	KUNIT_ASSERT_NOT_NULL(test, big);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svcsock_rx_stock_pin(test);
	svc_tcp_rx_budget_kunit_configure(256, 16, 100);
	c = svcsock_rx_stock_init_budget();
	KUNIT_ASSERT_NOT_NULL(test, c);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	admitted = svcsock_budget_stat(test, c, "admitted");

	/* Stocking off: b is admitted in order, onto the in-flight list. */
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_DRAIN, 0);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_EXPECT_FALSE(test, b->cr.stocked);
	KUNIT_EXPECT_GT(test, b->cr.ticket, 0ULL);

	/* Its admission turned stocking back on: a comes from the stock. */
	KUNIT_ASSERT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_FILL, 0), 32L);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_TRUE(test, a->cr.stocked);
	KUNIT_EXPECT_TRUE(test, a->cr.linked);
	KUNIT_EXPECT_EQ(test, a->cr.credit, 11U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 21L);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "inflight"), 2);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "admitted"),
			admitted + 2);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "avail"),
			256 - 16 - 22);

	/* A charge the stock still covers stays lockless. */
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_charge(sa, a, 20));
	KUNIT_EXPECT_TRUE(test, a->cr.stocked);
	KUNIT_EXPECT_EQ(test, a->cr.credit, 20U);
	/* Beyond it: an ordinary record, the youngest. */
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_charge(sa, a, 50));
	KUNIT_EXPECT_FALSE(test, a->cr.stocked);
	KUNIT_EXPECT_TRUE(test, a->cr.linked);
	KUNIT_EXPECT_EQ(test, a->cr.credit, 50U);
	KUNIT_EXPECT_GT(test, a->cr.ticket, b->cr.ticket);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "inflight"), 2);
	svc_tcp_rx_credit_release(a);
	svc_tcp_rx_credit_release(b);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "avail") +
			svcsock_budget_stat(test, c, "reserve"), 256);

	/*
	 * A stocked record holds credit and the list is empty: a newer record
	 * that avail does not cover is not the oldest and gets no reserve.
	 */
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_FILL, 0);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_ASSERT_TRUE(test, a->cr.stocked);
	KUNIT_EXPECT_FALSE(test, svc_tcp_rx_credit_admit(sc, big,
							 250 * PAGE_SIZE));
	KUNIT_EXPECT_FALSE(test, list_empty(&sc->sk_rx_wait.node));
	/* The waiter drained every stock and turned stocking off. */
	KUNIT_EXPECT_TRUE(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_CONTENDED, 0));
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 0L);
	/* Once it returns, the waiter is the oldest and starts on the reserve. */
	svc_tcp_rx_credit_release(a);
	KUNIT_EXPECT_TRUE(test, svc_tcp_rx_credit_admit(sc, big,
							250 * PAGE_SIZE));
	KUNIT_EXPECT_TRUE(test, big->cr.linked);
	KUNIT_EXPECT_GT(test, big->cr.reserve, 0U);
	KUNIT_EXPECT_TRUE(test, list_empty(&sc->sk_rx_wait.node));
	svc_tcp_rx_credit_release(big);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "avail") +
			svcsock_budget_stat(test, c, "reserve"), 256);

	/* An orphaned stocked record: credit kept spent, list untouched. */
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_FILL, 0);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_ASSERT_TRUE(test, b->cr.stocked);
	orphaned = svcsock_budget_stat(test, c, "orphaned");
	b->lt.refs_acquired = 1;
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_EQ(test, svc_tcp_rx_release_refs(b), -EINVAL);
	}
	b->lt.refs_acquired = 0;
	KUNIT_EXPECT_FALSE(test, b->cr.linked);
	KUNIT_EXPECT_FALSE(test, b->cr.stocked);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "orphaned"),
			orphaned + 11);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "avail") +
			svcsock_budget_stat(test, c, "reserve") + 11, 256);
	/* The list is intact: an ordinary record still links and returns. */
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_DRAIN, 0);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_FALSE(test, a->cr.stocked);
	svc_tcp_rx_credit_release(a);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "inflight"), 0);
}

/*
 * 7.3 port of the stocks: the cost estimate is learned, not a fixed factor.
 * Until RX_LOAN_COST_WARMUP records have ended on the locked path no
 * record takes stocked credit, even where the stock would cover it.  Once
 * warm, a record released to its stock without the lock leaves its sample
 * on the CPU, and the next refill folds it in under the lock.
 */
static void svcsock_rx_credit_stock_learn_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;
	struct svc_tcp_rx_budget *c;
	unsigned int i;
	long whole;

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svcsock_rx_stock_pin(test);
	svc_tcp_rx_budget_kunit_configure(256, 16, 0);	/* learned cost */
	c = svcsock_rx_stock_init_budget();
	KUNIT_ASSERT_NOT_NULL(test, c);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 0U);
	/*
	 * Credit orphaned by earlier cases stays spent (credit_orphan), so
	 * the budget need not be whole here: what is must be kept.
	 */
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "inflight"), 0);
	whole = svcsock_budget_stat(test, c, "avail") +
		svcsock_budget_stat(test, c, "reserve");

	/* Each record held 11 region pages for 10 pages: sample 100. */
	for (i = 0; i < RX_LOAN_COST_WARMUP; i++) {
		svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_FILL, 0);
		KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
		KUNIT_EXPECT_FALSE(test, a->cr.stocked);
		a->fixed_bytes = 0;
		a->lt.body_bytes = len;
		a->lt.region_count = 11;
		svc_tcp_rx_credit_release(a);
		a->lt.region_count = 0;
	}
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 100U);

	/* Warm: the stock covers the learned cost, 10 + 1. */
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_FILL, 0);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_TRUE(test, a->cr.stocked);
	KUNIT_EXPECT_EQ(test, a->cr.credit, 11U);
	/* It held 31 pages: sample 300, kept on this CPU, not yet learned. */
	a->fixed_bytes = 0;
	a->lt.body_bytes = len;
	a->lt.region_count = 31;
	svc_tcp_rx_credit_release(a);
	a->lt.region_count = 0;
	/*
	 * Admission still charges 100 (it reads the estimate as last folded,
	 * svc_tcp_rx_credit_cost()); svc_tcp_rx_budget_cost_factor() would
	 * fold first, so ask admission and the stats line instead.
	 */
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "cost_factor"), 100);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_TRUE(test, a->cr.stocked);
	KUNIT_EXPECT_EQ(test, a->cr.credit, 11U);
	svc_tcp_rx_credit_release(a);		/* no region page: no sample */
	/* The refill takes the lock and folds it in: now 30 + 1. */
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_FILL, 0);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "cost_factor"), 300);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_cost_factor(), 300U);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_EXPECT_TRUE(test, a->cr.stocked);
	KUNIT_EXPECT_EQ(test, a->cr.credit, 31U);
	svc_tcp_rx_credit_release(a);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "inflight"), 0);
	/*
	 * Nothing is lost: the line's avail already counts what is stocked
	 * (svc_tcp_rx_budget_format()), so with nothing in flight it and the
	 * reserve make up what they did before the first record, stocked
	 * credit included once.
	 */
	KUNIT_EXPECT_GT(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "avail") +
			svcsock_budget_stat(test, c, "reserve"), whole);
}

/*
 * A record admitted from a stock bounces: what it does not lend goes back
 * to the stock without the lock, the lent unit is counted in "lent" and the
 * record stays counted in flight until bounce_done returns the unit to the
 * stock.  The budget's credit (stocked included) never changes.
 */
static void svcsock_rx_bounce_stocked_test(struct kunit *test)
{
	enum { BODY_LEN = 7 * XDR_UNIT + 300, OFFSET = 8, LEN = 200, ALIGN = 512 };
	struct svcsock_rx_fixture *fixture = svcsock_rx_alloc_fixture(test, 4);
	struct svc_tcp_rx_state *state = fixture->rqstp->rq_tcp_rx;
	u8 record[sizeof(rpc_fraghdr) + BODY_LEN];
	struct svc_tcp_rx_budget *c;
	struct bio_vec out[4];
	struct xdr_buf payload;
	unsigned int refs;
	long avail;
	int n;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svcsock_rx_stock_pin(test);
	svc_tcp_rx_budget_kunit_configure(256, 16, 100);
	svcsock_rx_region_fixture(test, 0);
	svcsock_rx_publish_loan(test, fixture, record, BODY_LEN, &refs);
	c = svcsock_rx_stock_init_budget();
	KUNIT_ASSERT_NOT_NULL(test, c);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	KUNIT_ASSERT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_FILL, 0), 32L);
	avail = svcsock_rx_budget_stat(c, "avail");
	KUNIT_ASSERT_PTR_EQ(test, svcsock_rx_bounce_admit(test, fixture,
							  sizeof(record)), c);
	KUNIT_ASSERT_TRUE(test, state->cr.stocked);
	KUNIT_ASSERT_EQ(test, state->cr.credit, 2U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), avail - 2);

	svcsock_rx_payload_view(&payload, state, OFFSET, LEN);
	n = svc_tcp_rx_bounce(fixture->svsk, fixture->rqstp, &payload, ALIGN,
			      512, out, ARRAY_SIZE(out));
	KUNIT_ASSERT_EQ(test, n, 1);
	KUNIT_EXPECT_TRUE(test, state->bounce_stocked);
	KUNIT_EXPECT_EQ(test, state->bounce_credit, 1U);
	KUNIT_EXPECT_FALSE(test, state->cr.linked);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), avail - 1);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 1L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "inflight"), 1L);
	/* Not lent from the lock's side: the stock got the rest back. */
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 31L);

	svc_tcp_rx_bounce_done(fixture->rqstp, out, n);
	KUNIT_EXPECT_FALSE(test, state->bounce_stocked);
	KUNIT_EXPECT_EQ(test, state->bounce_pages, 0U);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "avail"), avail);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "lent"), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_budget_stat(c, "inflight"), 0L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 32L);
	svc_tcp_release_ctxt(&fixture->svsk->sk_xprt, state);
}

/*
 * R1: a live change with stocked holders in flight drains the stocks and
 * moves avail by the change rather than resetting it, so once every holder
 * returns, avail plus the reserve is exactly the new budget.
 */
static void svcsock_rx_credit_stock_live_change_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_tcp_rx_state *b = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	struct svc_sock *sb = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;		/* costs 11 */
	struct svc_tcp_rx_budget *c;

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	svcsock_rx_stock_pin(test);
	svc_tcp_rx_budget_kunit_configure(256, 8, 100);
	c = svcsock_rx_stock_init_budget();
	KUNIT_ASSERT_NOT_NULL(test, c);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	KUNIT_ASSERT_EQ(test, svcsock_rx_stock(c,
				SVC_TCP_RX_KUNIT_STOCK_FILL, 0), 32L);
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sb, b, len));
	KUNIT_ASSERT_TRUE(test, a->cr.stocked && b->cr.stocked);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "avail"), 248 - 22);

	/* Up by 44 with stocked holders only: a delta, not a reset. */
	svc_tcp_rx_budget_kunit_configure(300, 8, 100);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "budget"), 300);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "avail"), 292 - 22);
	svc_tcp_rx_credit_release(a);
	/* Down by 100 with one left, its reserve moved too. */
	svc_tcp_rx_budget_kunit_configure(200, 4, 100);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "avail"), 196 - 11);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "reserve"), 4);
	svc_tcp_rx_credit_release(b);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "inflight"), 0);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "avail") +
			svcsock_budget_stat(test, c, "reserve"), 200);
	/* With nothing in flight a change resets: still exactly the budget. */
	svc_tcp_rx_budget_kunit_configure(256, 0, 100);
	svcsock_rx_stock(c, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, c, "avail"), 256);
}

struct svcsock_rx_stock_put {
	struct svc_tcp_rx_budget *c;
	struct svc_tcp_rx_state *state;
	long n;
};

/* On another CPU: release a record, or put @n pages to @c's stock there. */
static long svcsock_rx_stock_put_fn(void *data)
{
	struct svcsock_rx_stock_put *p = data;

	if (p->state)
		svc_tcp_rx_credit_release(p->state);
	else
		svc_tcp_rx_budget_kunit_stock(p->c, SVC_TCP_RX_KUNIT_STOCK_PUT,
					      p->n);
	return 0;
}

static void svcsock_rx_stock_free(void *c)
{
	svc_tcp_rx_budget_kunit_free(c);
}

/*
 * Each budget stocks separately, and credit goes back to the budget it
 * came from even when it returns on another CPU.  A namespace budget's
 * teardown (net_exit's) finds no stocked holder and warns nothing.
 */
static void svcsock_rx_credit_stock_netns_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *sa = svcsock_rx_credit_sock_forget(test);
	const u32 len = 10 * PAGE_SIZE;		/* costs 11 */
	struct svc_tcp_rx_budget *ci, *cd, *cx;
	struct svcsock_rx_stock_put put = {};
	long avail_i, avail_d;
	int cpu, other;

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_default_off, NULL), 0);
	cpu = svcsock_rx_stock_pin(test);
	other = svcsock_rx_stock_other(cpu);
	svc_tcp_rx_budget_kunit_configure(256, 0, 100);
	svc_tcp_rx_budget_kunit_default(256, 0);
	ci = svcsock_rx_stock_init_budget();
	cd = svc_tcp_rx_region_kunit_budget(NULL, SVC_TCP_RX_STATS_DEFAULT);
	KUNIT_ASSERT_NOT_NULL(test, ci);
	svcsock_rx_stock(ci, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	svcsock_rx_stock(cd, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	KUNIT_ASSERT_EQ(test, svcsock_rx_stock(ci,
				SVC_TCP_RX_KUNIT_STOCK_FILL, 0), 32L);
	KUNIT_ASSERT_EQ(test, svcsock_rx_stock(cd,
				SVC_TCP_RX_KUNIT_STOCK_FILL, 0), 32L);
	avail_i = svcsock_budget_stat(test, ci, "avail");
	avail_d = svcsock_budget_stat(test, cd, "avail");

	/* The record draws on its namespace's stock only. */
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(sa, a, len));
	KUNIT_ASSERT_TRUE(test, a->cr.stocked);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(ci,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 21L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(cd,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 32L);

	/* Released on another CPU: back to that CPU's stock of the same budget. */
	if (other >= 0) {
		put.state = a;
		work_on_cpu(other, svcsock_rx_stock_put_fn, &put);
	} else {
		svc_tcp_rx_credit_release(a);
		kunit_mark_skipped(test, "one CPU: no release elsewhere");
	}
	KUNIT_EXPECT_FALSE(test, a->cr.linked);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(ci,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 32L);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(cd,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 32L);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, ci, "avail"), avail_i);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, cd, "avail"), avail_d);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, ci, "inflight"), 0);

	/* A namespace's budget, stocked on two CPUs, then torn down. */
	cx = svc_tcp_rx_budget_kunit_alloc();
	KUNIT_ASSERT_NOT_NULL(test, cx);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_stock_free, cx), 0);
	svcsock_rx_stock(cx, SVC_TCP_RX_KUNIT_STOCK_APPLY, 0);
	KUNIT_ASSERT_EQ(test, svcsock_rx_stock(cx,
				SVC_TCP_RX_KUNIT_STOCK_FILL, 0), 32L);
	KUNIT_ASSERT_TRUE(test, svcsock_rx_stock(cx,
				SVC_TCP_RX_KUNIT_STOCK_TAKE, 20));
	put.c = cx;
	put.state = NULL;
	put.n = 20;
	if (other >= 0)
		work_on_cpu(other, svcsock_rx_stock_put_fn, &put);
	else
		svcsock_rx_stock_put_fn(&put);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(cx,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 32L);
	KUNIT_EXPECT_EQ(test, svcsock_budget_stat(test, cx, "avail"), 256);
	KUNIT_EXPECT_EQ(test, svcsock_rx_stock(ci,
				SVC_TCP_RX_KUNIT_STOCK_STOCKED, 0), 32L);
	kunit_warning_suppress(test) {
		kunit_release_action(test, svcsock_rx_stock_free, cx);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 0);
	}
}

#define SVCSOCK_RX_CONSUMER_GRANTS	16

/*
 * The server and a second consumer (a stand-in for nvmet-tcp's port) take
 * exact grants from the read pool in turn until both are refused: the
 * region's outstanding total never passes the region, and neither holds
 * more than its share beyond one oldest-record overrun.
 */
static void svcsock_rx_second_consumer_test(struct kunit *test)
{
	struct rx_loan_waiter *ws, *wo;
	struct rx_loan_consumer *rc, *other;
	struct svc_tcp_rx_budget *c;
	const long region = 64, step = 4;
	long out_s, out_o, share;
	int ns = 0, no = 0, i;
	bool more_s = true, more_o = true;

	/*
	 * It restarts and then unregisters the initial namespace's consumer,
	 * and counts what that consumer holds against the share: it must not
	 * touch a live server's.
	 */
	svcsock_rx_skip_live_consumer(test);
	ws = kunit_kcalloc(test, SVCSOCK_RX_CONSUMER_GRANTS, sizeof(*ws), GFP_KERNEL);
	wo = kunit_kcalloc(test, SVCSOCK_RX_CONSUMER_GRANTS, sizeof(*wo), GFP_KERNEL);
	other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ws);
	KUNIT_ASSERT_NOT_NULL(test, wo);
	KUNIT_ASSERT_NOT_NULL(test, other);
	svc_tcp_rx_budget_kunit_region(region, region / 2);
	c = svc_tcp_rx_budget_kunit_read();
	KUNIT_ASSERT_NOT_NULL(test, c);
	rc = svcsock_rx_own_consumer(test, 1, PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, rx_loan_consumer_register(other, rx_loan_net(&init_net),
							"standin", 0), 0);
	share = rx_loan_consumer_share(rc);
	KUNIT_EXPECT_EQ(test, (long)rx_loan_consumer_share(other), share);

	for (i = 0; i < SVCSOCK_RX_CONSUMER_GRANTS && (more_s || more_o); i++) {
		if (more_s) {
			more_s = !svc_tcp_rx_budget_kunit_grant(c, rc, &ws[ns], step);
			ns += more_s;
		}
		if (more_o) {
			more_o = !svc_tcp_rx_budget_kunit_grant(c, other, &wo[no], step);
			no += more_o;
		}
		KUNIT_EXPECT_LE(test, rx_loan_region_out(), region);
	}
	out_s = atomic_long_read(&rc->out_read) + atomic_long_read(&rc->out_write);
	out_o = atomic_long_read(&other->out_read) +
		atomic_long_read(&other->out_write);
	KUNIT_EXPECT_GT(test, ns, 0);
	KUNIT_EXPECT_GT(test, no, 0);
	KUNIT_EXPECT_LE(test, out_s + out_o, region);
	KUNIT_EXPECT_LE(test, out_s, share +
			(atomic_long_read(&rc->overrun_share) ? step : 0));
	KUNIT_EXPECT_LE(test, out_o, share +
			(atomic_long_read(&other->overrun_share) ? step : 0));

	while (ns--)
		svc_tcp_rx_budget_kunit_grant_return(c, &ws[ns]);
	while (no--)
		svc_tcp_rx_budget_kunit_grant_return(c, &wo[no]);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&rc->out_read), 0L);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&other->out_read), 0L);
	rx_loan_consumer_unregister(other);
	rx_loan_consumer_unregister(rc);
}

/*
 * The large-server shape's floor, @threads of 1 MiB payloads: four records per
 * thread, each of the payload's pages and its alignment page.
 */
static unsigned long svcsock_rx_m8a_floor(unsigned long threads)
{
	return threads * 4 * ((SZ_1M >> PAGE_SHIFT) + 1);
}

/*
 * kNFSd's floor is four records of the largest payload, each with its
 * alignment page, per thread; the large-server shape (192 threads, 1 MiB) asks
 * 197376 pages on 4 KiB pages (49920 on 16 KiB, 13056 on 64 KiB), which
 * the region rounds to 262144 (65536, 16384).
 */
static void svcsock_rx_default_sizing_test(struct kunit *test)
{
	unsigned long floor = svc_tcp_rx_consumer_floor(192, 1U << 20);

	KUNIT_EXPECT_EQ(test, svc_tcp_rx_consumer_floor(0, 1U << 20), 0UL);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_consumer_floor(1, PAGE_SIZE),
			8UL);
	KUNIT_EXPECT_EQ(test, floor, svcsock_rx_m8a_floor(192));
	if (PAGE_SIZE == SZ_4K) {
		KUNIT_EXPECT_EQ(test, floor, 197376UL);
		KUNIT_EXPECT_EQ(test, roundup_pow_of_two(floor), 262144UL);
	}
	/* The region is that floor rounded up to a power of two. */
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_size_for(0, floor,
								 1UL << 30),
			roundup_pow_of_two(floor));
}

/*
 * The large-server shape, with the floor set after registration: nfsd registers as
 * it starts, before its threads, so registering asks nothing of the
 * region and carves nothing; the threads set the floor, and the region
 * carved then (at the first connection, READ or NIC pool) is its floor
 * rounded up to a power of two: 262144 pages on 4 KiB pages, 65536 on
 * 16 KiB, 16384 on 64 KiB.
 */
static void svcsock_rx_floor_follows_threads_test(struct kunit *test)
{
	const unsigned long ram = 1UL << 30;	/* pages: no RAM cap */
	const unsigned long m8a = roundup_pow_of_two(svcsock_rx_m8a_floor(192));
	struct rx_loan_consumer *rc;
	unsigned long base;

	svcsock_rx_skip_live_consumer(test);
	rc = svcsock_rx_own_consumer(test, 0, 1U << 20);
	base = page_pool_rx_region_kunit_carve_size(ram);
	if (base != page_pool_rx_region_kunit_size_for(0, 0, ram))
		kunit_skip(test, "other receive demand is registered");
	KUNIT_EXPECT_TRUE(test, rc->registered);
	KUNIT_EXPECT_FALSE(test, page_pool_rx_region_exists());

	svc_tcp_rx_kunit_threads(192, 1U << 20);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_carve_size(ram), m8a);
	KUNIT_EXPECT_FALSE(test, page_pool_rx_region_exists());

	svc_tcp_rx_kunit_threads(0, 1U << 20);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_carve_size(ram), base);
	rx_loan_consumer_unregister(rc);
}

/*
 * nfsd registers its consumer as it starts a server, not with a listener.
 * A 1-thread server started first, then a new 192-thread one, re-binds
 * the consumer: its floor is the new server's, and the region carved then
 * is that floor rounded up to a power of two (262144 pages on 4 KiB
 * pages, 65536 on 16 KiB, 16384 on 64 KiB).  Stopping the server
 * unregisters the consumer.
 */
static void svcsock_rx_consumer_rebind_test(struct kunit *test)
{
	const unsigned long ram = 1UL << 30;	/* pages: no RAM cap */
	const unsigned long m8a = roundup_pow_of_two(svcsock_rx_m8a_floor(192));
	struct rx_loan_consumer *rc;
	unsigned long base;

	svcsock_rx_skip_live_consumer(test);
	base = page_pool_rx_region_kunit_carve_size(ram);
	if (base != page_pool_rx_region_kunit_size_for(0, 0, ram))
		kunit_skip(test, "other receive demand is registered");
	rc = svcsock_rx_own_consumer(test, 1, 1U << 20);
	KUNIT_EXPECT_TRUE(test, rc->registered);
	/* one thread: 4 records of 1 MiB, each with its alignment page */
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_carve_size(ram),
			page_pool_rx_region_kunit_size_for(0,
					svcsock_rx_m8a_floor(1), ram));

	KUNIT_EXPECT_PTR_EQ(test, svc_tcp_rx_kunit_consumer(192, 1U << 20), rc);
	KUNIT_EXPECT_TRUE(test, rc->registered);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_carve_size(ram), m8a);
	KUNIT_EXPECT_FALSE(test, page_pool_rx_region_exists());

	svc_tcp_rx_kunit_consumer_stop();
	KUNIT_EXPECT_FALSE(test, rc->registered);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_carve_size(ram), base);
}

static void svcsock_rx_pool_destroy(void *pool)
{
	page_pool_destroy(pool);
}

/*
 * The region is off unless rx_region_dev is set or
 * svc_tcp_rx_region_reads=Y; autosize sizes a region once opted in, it
 * does not enable one.  With neither set, a one-thread server's demand
 * sizes a region (small enough to carve, so that a default that enabled
 * it would show), yet a pool created on a device, the carve
 * svc_tcp_accept() makes and a READ leave none carved.
 */
static void svcsock_rx_region_default_off_test(struct kunit *test)
{
	const unsigned long ram = 1UL << 30;	/* pages: no RAM cap */
	struct page_pool_params params = {
		.order = 0,
		.pool_size = 16,
		.nid = NUMA_NO_NODE,
	};
	struct svcsock_rx_fixture *fixture;
	struct page_pool *pool;
	char *buf;

	svcsock_rx_skip_live_consumer(test);
	buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	/* The defaults: no device listed, an auto size, READs unconfined. */
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("dev", NULL, buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "\n");
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("pages", NULL, buf), 0);
	KUNIT_EXPECT_NOT_NULL(test, strstr(buf, " (auto)"));
	KUNIT_ASSERT_FALSE(test, svc_tcp_rx_region_kunit_reads_on());

	svcsock_rx_own_consumer(test, 1, 1U << 20);
	KUNIT_EXPECT_GT(test, page_pool_rx_region_kunit_carve_size(ram), 0UL);

	params.dev = kunit_device_register(test, "rx-region-default-off");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, params.dev);
	pool = page_pool_create(&params);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pool);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, svcsock_rx_pool_destroy,
						  pool),
			0);
	KUNIT_EXPECT_FALSE(test, page_pool_rx_region_exists());

	rx_loan_region_carve();
	KUNIT_EXPECT_FALSE(test, page_pool_rx_region_exists());

	fixture = svcsock_rx_alloc_fixture(test, 8);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_region_read(fixture->rqstp, 2), 0);
	KUNIT_EXPECT_FALSE(test, svc_rqst_rx_region_held(fixture->rqstp));
	KUNIT_EXPECT_FALSE(test, page_pool_rx_region_exists());
}

/*
 * The initial namespace's sunrpc budget is rx_loan_net()'s write budget,
 * and unconfigured it lends the region's write share.
 */
static void svcsock_rx_computed_default_test(struct kunit *test)
{
	struct rx_loan_budget *lb = NULL;
	unsigned long pages;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	/* the receive clamp at the loan minimum, as a loaning server runs */
	svc_tcp_rx_budget_kunit_rcvbuf(128 * 1024);
	svc_tcp_rx_budget_kunit_region(64, 16);
	pages = svc_tcp_rx_kunit_computed_default(true, &lb);
	svc_tcp_rx_kunit_computed_default(false, &lb);
	svc_tcp_rx_budget_kunit_region(0, 0);
	if (!lb)
		kunit_skip(test, "no initial namespace budget");
	KUNIT_EXPECT_PTR_EQ(test, lb, &rx_loan_net(&init_net)->write);
	KUNIT_EXPECT_EQ(test, pages, 48UL);
}

static void svcsock_rx_netns_own_action(void *unused)
{
	svc_tcp_rx_kunit_netns_alias(false);
}

/*
 * With the namespace aliasing in place, as outside KUnit: a record the
 * initial namespace admits takes its credit from rx_loan_net(&init_net)'s
 * write budget and the region, and a READ grant from its READ budget and
 * the region; both give it back.
 */
static void svcsock_rx_netns_lend_test(struct kunit *test)
{
	struct svc_tcp_rx_state *a = svcsock_rx_alloc_state(test, 64);
	struct svc_sock *svsk = svcsock_rx_credit_sock_forget(test);
	struct svc_tcp_rx_budget *read;
	struct rx_loan_consumer *rc;
	struct rx_loan_waiter w;
	long free0, out0, taken;
	bool aliased;

	svcsock_rx_skip_live_consumer(test);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_default_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_off, NULL), 0);
	svc_tcp_rx_kunit_netns_alias(true);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_netns_own_action, NULL), 0);
	svc_tcp_rx_budget_kunit_configure(0, 0, 100);
	svc_tcp_rx_budget_kunit_default(0, 0);
	svc_tcp_rx_budget_kunit_rcvbuf(0);
	svc_tcp_rx_budget_kunit_region(100, 30);
	svc_tcp_rx_budget_kunit_init();
	svc_tcp_rx_budget_kunit_configure(10, 0, 100);
	/* configuration applies at a budget's next locked op: apply it now */
	svc_tcp_rx_kunit_netns_alias(true);

	/* a record, through the normal admit path */
	free0 = svc_tcp_rx_kunit_netns_free(false, &aliased);
	if (!aliased)
		kunit_skip(test, "no initial namespace budget");
	KUNIT_EXPECT_EQ(test, free0, 10L);
	out0 = rx_loan_region_out();
	KUNIT_ASSERT_TRUE(test, svc_tcp_rx_credit_admit(svsk, a, 5 * PAGE_SIZE));
	KUNIT_ASSERT_TRUE(test, a->cr.linked);
	KUNIT_EXPECT_PTR_EQ(test, a->cr.budget, &rx_loan_net(&init_net)->write);
	taken = (long)a->cr.credit + a->cr.reserve;
	KUNIT_EXPECT_GT(test, taken, 0L);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_kunit_netns_free(false, &aliased),
			free0 - taken);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), out0 + taken);
	svc_tcp_rx_credit_release(a);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_kunit_netns_free(false, &aliased),
			free0);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), out0);

	/* a READ grant, from the read pool */
	read = svc_tcp_rx_budget_kunit_read();
	KUNIT_ASSERT_NOT_NULL(test, read);
	rc = svcsock_rx_own_consumer(test, 1, PAGE_SIZE);
	free0 = svc_tcp_rx_kunit_netns_free(true, &aliased);
	KUNIT_EXPECT_TRUE(test, aliased);
	out0 = rx_loan_region_out();
	KUNIT_ASSERT_EQ(test, svc_tcp_rx_budget_kunit_grant(read, rc, &w, 4), 0);
	taken = w.credit + w.reserve;
	KUNIT_EXPECT_EQ(test, taken, 4L);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_kunit_netns_free(true, &aliased),
			free0 - taken);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), out0 + taken);
	svc_tcp_rx_budget_kunit_grant_return(read, &w);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_kunit_netns_free(true, &aliased),
			free0);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), out0);
}

static void svcsock_rx_computed_default_off(void *unused)
{
	struct rx_loan_budget *lb;

	svc_tcp_rx_kunit_computed_default(false, &lb);
}

/*
 * With no svc_tcp_rx_rcvbuf given, the clamp is SVC_TCP_RX_RCVBUF_DEFAULT
 * while a region exists and the computed default is in force, so that the
 * default budget lends the write share; with no region it is 0.
 */
static void svcsock_rx_rcvbuf_default_test(struct kunit *test)
{
	struct rx_loan_budget *lb = NULL;
	unsigned int bytes;
	unsigned long pages;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_budget_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_off, NULL), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_computed_default_off, NULL), 0);
	svcsock_rx_skip_live_consumer(test);
	svcsock_rx_own_consumer(test, 1, PAGE_SIZE);
	svc_tcp_rx_budget_kunit_region(64, 16);
	svc_tcp_rx_kunit_computed_default(true, &lb);
	bytes = svc_tcp_rx_kunit_rcvbuf_ungiven();
	pages = svc_tcp_rx_kunit_computed_default(true, &lb);
	KUNIT_EXPECT_EQ(test, bytes, 256U * 1024);
	KUNIT_EXPECT_EQ(test, pages, 48UL);

	svc_tcp_rx_budget_kunit_region(0, 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_kunit_rcvbuf_ungiven(), 0U);
}

/* A setting of the initial namespace's, put back however the case ends. */
struct svcsock_rx_param_saved {
	const char *name;
	char *line;
};

static void svcsock_rx_param_restore(void *data)
{
	struct svcsock_rx_param_saved *saved = data;

	svc_tcp_rx_budget_kunit_param(saved->name, saved->line, NULL);
}

static void svcsock_rx_param_save(struct kunit *test, const char *name)
{
	struct svcsock_rx_param_saved *saved;

	saved = kunit_kzalloc(test, sizeof(*saved), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, saved);
	saved->name = name;
	saved->line = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, saved->line);
	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param(name, NULL,
							    saved->line), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_param_restore, saved), 0);
}

/* the stand-in region: 1000 pages beyond the other configured budgets */
static unsigned long svcsock_rx_param_region;
#define SVCSOCK_RX_PARAM_REGION	svcsock_rx_param_region

/*
 * The initial namespace's budget is @pages in force and read back as
 * @line, and the configured budgets, which the shared region's read pool
 * leaves to the budgets, are @base (the others') plus @counted.
 */
static void svcsock_rx_param_expect(struct kunit *test, char *buf,
				    const char *line, unsigned long pages,
				    unsigned long base, unsigned long counted)
{
	unsigned int read_pool;

	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("pages", NULL, buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, line);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_in_force(), pages);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_configured(&read_pool),
			base + counted);
	KUNIT_EXPECT_EQ(test, (unsigned long)read_pool,
			SVCSOCK_RX_PARAM_REGION - base - counted);
}

/*
 * The initial namespace's budget and the connection share, through the
 * ops the module parameters, sysctls and debugfs files share: 0 is auto
 * (the region's write share), N is manual N, "off" a manual 0, and back
 * to 0 is auto again.  A budget given before the namespace's budget
 * exists (kernel command line, modprobe line) is applied as a run-time
 * write is: counted in the configured budgets, which the shared region's
 * read pool leaves to them, and refused, staying auto, past the write
 * partition.
 */
static void svcsock_rx_param_auto_manual_test(struct kunit *test)
{
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	char autoline[32], over[24];
	unsigned int read_pool;
	unsigned long base;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	svcsock_rx_param_save(test, "pages");
	svcsock_rx_param_save(test, "reserve");
	svcsock_rx_param_save(test, "conn_share");
	/* auto configures nothing: what is configured is the others' */
	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("pages", "0", buf), 0);
	base = svc_tcp_rx_budget_kunit_configured(&read_pool);
	svcsock_rx_param_region = base + 1000;
	snprintf(autoline, sizeof(autoline), "%lu (auto)\n", SVCSOCK_RX_PARAM_REGION);
	snprintf(over, sizeof(over), "%lu", SVCSOCK_RX_PARAM_REGION + 1);
	/* a shared region of a known size, with a clamp that lets it lend */
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			svcsock_rx_region_off, NULL), 0);
	svc_tcp_rx_budget_kunit_rcvbuf(128 * 1024);
	svc_tcp_rx_budget_kunit_region(SVCSOCK_RX_PARAM_REGION, 0);
	svc_tcp_rx_budget_kunit_init();
	/* auto lends the region's write share */
	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("pages", "0", buf), 0);
	svcsock_rx_param_expect(test, buf, autoline,
				SVCSOCK_RX_PARAM_REGION, base, 0);

	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("pages", "3", buf), 0);
	svcsock_rx_param_expect(test, buf, "3 (manual)\n", 3, base, 3);

	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("pages", "0", buf), 0);
	svcsock_rx_param_expect(test, buf, autoline,
				SVCSOCK_RX_PARAM_REGION, base, 0);

	/* a manual 0 is no budget: nothing lent */
	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("pages", "off", buf), 0);
	svcsock_rx_param_expect(test, buf, "0 (manual)\n", 0, base, 0);

	/* past the write partition: refused, the budget as it was */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_param("pages", over, NULL),
			-ENOSPC);
	svcsock_rx_param_expect(test, buf, "0 (manual)\n", 0, base, 0);

	/* a line read back restores what it says */
	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("pages", "3 (auto)", buf), 0);
	svcsock_rx_param_expect(test, buf, autoline,
				SVCSOCK_RX_PARAM_REGION, base, 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_param("pages", "3 (later)", NULL), -EINVAL);

	/* given at boot, before the budget exists: in force and counted */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_boot("300"), 0);
	svcsock_rx_param_expect(test, buf, "300 (manual)\n", 300, base, 300);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_boot("off"), 0);
	svcsock_rx_param_expect(test, buf, "0 (manual)\n", 0, base, 0);
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_boot("auto"), 0);
	svcsock_rx_param_expect(test, buf, autoline,
				SVCSOCK_RX_PARAM_REGION, base, 0);
	/* given at boot past the write partition: refused, so auto */
	KUNIT_EXPECT_EQ(test, svc_tcp_rx_budget_kunit_boot(over), 0);
	svcsock_rx_param_expect(test, buf, autoline,
				SVCSOCK_RX_PARAM_REGION, base, 0);

	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("reserve", "5", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "5 (manual)\n");
	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("reserve", "0", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "0 (auto)\n");

	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("conn_share", "7", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "7 (manual)\n");
	KUNIT_ASSERT_GT(test, svc_tcp_rx_budget_kunit_param("conn_share", "0", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "0 (auto)\n");
}

static struct kunit_case svcsock_rx_test_cases[] = {
	KUNIT_CASE(svcsock_rx_param_auto_manual_test),
	KUNIT_CASE_PARAM(svcsock_rx_actor_split_test,
			 svcsock_rx_split_gen_params),
	KUNIT_CASE(svcsock_rx_multifragment_fresh_request_test),
	KUNIT_CASE(svcsock_rx_two_records_one_skb_test),
	KUNIT_CASE(svcsock_rx_prefix_only_then_loan_test),
	KUNIT_CASE_PARAM(svcsock_rx_socket_owned_clear_test,
			 svcsock_rx_clear_gen_params),
	KUNIT_CASE_PARAM(svcsock_rx_append_fault_outer_close_test,
			 svcsock_rx_fault_gen_params),
	KUNIT_CASE_PARAM(svcsock_rx_materialize_fault_outer_close_test,
			 svcsock_rx_fault_gen_params),
	KUNIT_CASE(svcsock_rx_mixed_geometry_lifetime_test),
	KUNIT_CASE(svcsock_rx_back_to_back_locked_heads_test),
	KUNIT_CASE(svcsock_rx_release_payload_test),
	KUNIT_CASE(svcsock_rx_small_record_copied_test),
	KUNIT_CASE_PARAM(svcsock_rx_small_record_split_test,
			 svcsock_rx_split_gen_params),
	KUNIT_CASE(svcsock_rx_credit_fifo_test),
	KUNIT_CASE(svcsock_rx_credit_learn_test),
	KUNIT_CASE(svcsock_rx_geometry_test),
	KUNIT_CASE(svcsock_rx_credit_reserve_oldest_test),
	KUNIT_CASE(svcsock_rx_window_clamp_test),
	KUNIT_CASE(svcsock_rx_listener_window_test),
	KUNIT_CASE(svcsock_rx_hold_buffer_test),
	KUNIT_CASE(svcsock_rx_hold_again_test),
	KUNIT_CASE(svcsock_rx_credit_rcvbuf_floor_test),
	KUNIT_CASE(svcsock_rx_credit_disable_waiters_test),
	KUNIT_CASE(svcsock_rx_credit_invalid_reset_test),
	KUNIT_CASE(svcsock_rx_credit_orphan_test),
	KUNIT_CASE(svcsock_rx_credit_orphan_region_test),
	KUNIT_CASE(svcsock_rx_credit_reconfigure_inflight_test),
	KUNIT_CASE(svcsock_rx_credit_reconfigure_wakes_test),
	KUNIT_CASE(svcsock_rx_credit_wake_need_test),
	KUNIT_CASE(svcsock_rx_credit_copy_unwaits_test),
	KUNIT_CASE(svcsock_rx_credit_collapsed_unwaits_test),
	KUNIT_CASE(svcsock_rx_credit_copy_no_budget_test),
	KUNIT_CASE(svcsock_rx_credit_data_ready_test),
	KUNIT_CASE(svcsock_rx_set_want_test),
	KUNIT_CASE(svcsock_rx_want_data_ready_test),
	KUNIT_CASE(svcsock_rx_credit_default_budget_test),
	KUNIT_CASE(svcsock_rx_read_pool_test),
	KUNIT_CASE(svcsock_rx_read_pool_init_test),
#if IS_ENABLED(CONFIG_PAGE_POOL)
	KUNIT_CASE(svcsock_rx_region_take_give_test),
	KUNIT_CASE(svcsock_rx_region_unsplit_test),
	KUNIT_CASE(svcsock_rx_region_bind_reads_test),
	KUNIT_CASE(svcsock_rx_bounce_region_credit_test),
	KUNIT_CASE(svcsock_rx_bounce_region_multi_test),
	KUNIT_CASE(svcsock_rx_bounce_credit_short_test),
	KUNIT_CASE(svcsock_rx_bounce_consumer_test),
	KUNIT_CASE(svcsock_rx_bounce_region_short_test),
	KUNIT_CASE(svcsock_rx_bounce_locked_head_test),
	KUNIT_CASE(svcsock_rx_bounce_reserve_test),
	KUNIT_CASE(svcsock_rx_bounce_capacity_test),
	KUNIT_CASE(svcsock_rx_bounce_unsupported_test),
	KUNIT_CASE(svcsock_rx_bounce_credit_over_short_test),
	KUNIT_CASE(svcsock_rx_bounce_credit_over_test),
	KUNIT_CASE(svcsock_rx_bounce_budget_off_test),
	KUNIT_CASE(svcsock_rx_region_cache_test),
	KUNIT_CASE(svcsock_rx_region_shard_test),
	KUNIT_CASE(svcsock_rx_region_split_shards_test),
	KUNIT_CASE(svcsock_rx_region_pool_bounds_test),
	KUNIT_CASE(svcsock_rx_region_pool_defer_test),
#endif
	KUNIT_CASE(svcsock_rx_bounce_arena_test),
	KUNIT_CASE(svcsock_rx_budget_write_partition_test),
	KUNIT_CASE(svcsock_rx_credit_factor_bounds_test),
	KUNIT_CASE(svcsock_rx_capacity_plus_one_test),
	KUNIT_CASE(svcsock_rx_partial_save_restore_test),
	KUNIT_CASE(svcsock_rx_spare_none_test),
	KUNIT_CASE(svcsock_rx_spare_kept_test),
	KUNIT_CASE(svcsock_rx_exclusion_table_test),
	KUNIT_CASE(svcsock_rx_materialize_test),
	KUNIT_CASE(svcsock_rx_locked_head_then_materialize_test),
	KUNIT_CASE(svcsock_rx_capacity_release_test),
	KUNIT_CASE(svcsock_rx_loan_table_fit_test),
	KUNIT_CASE(svcsock_rx_loan_1m_mtu9000_test),
	KUNIT_CASE(svcsock_rx_capacity_sizing_test),
	KUNIT_CASE(svcsock_rx_arena_bound_test),
	KUNIT_CASE(svcsock_rx_page_pool_recycle_test),
	KUNIT_CASE(svcsock_rx_page_pool_bulk_release_test),
	KUNIT_CASE(svcsock_rx_skb_region_pages_test),
	KUNIT_CASE(svcsock_rx_region_count_test),
	KUNIT_CASE(svcsock_rx_duplicate_cleanup_test),
	KUNIT_CASE(svcsock_rx_concurrent_release_test),
	KUNIT_CASE(svcsock_rx_exchange_test),
	KUNIT_CASE(svcsock_rx_max_capacity_reset_test),
	KUNIT_CASE(svcsock_rx_wrong_partial_owner_test),
	KUNIT_CASE(svcsock_rx_region_admit_send_test),
	KUNIT_CASE(svcsock_rx_region_unconfine_test),
	KUNIT_CASE(svcsock_rx_region_busy_page_test),
	KUNIT_CASE(svcsock_rx_region_park_grant_test),
	KUNIT_CASE(svcsock_rx_region_park_drop_test),
	KUNIT_CASE(svcsock_rx_region_dead_chain_test),
	KUNIT_CASE(svcsock_rx_region_share_test),
	KUNIT_CASE(svcsock_rx_region_unconfined_test),
	KUNIT_CASE(svcsock_rx_region_send_map_test),
	KUNIT_CASE(svcsock_rx_region_conn_wake_test),
	KUNIT_CASE(svcsock_rx_region_busy_escape_test),
	KUNIT_CASE(svcsock_rx_region_shared_budget_test),
	KUNIT_CASE(svcsock_rx_region_park_secure_test),
	KUNIT_CASE(svcsock_rx_region_split_pool_test),
	KUNIT_CASE(svcsock_rx_region_overrun_test),
	KUNIT_CASE(svcsock_rx_region_live_change_test),
	KUNIT_CASE(svcsock_rx_region_drop_kick_test),
	KUNIT_CASE(svcsock_rx_region_recv_behind_read_test),
	KUNIT_CASE(svcsock_rx_region_read_short_test),
	KUNIT_CASE(svcsock_rx_region_share_park_test),
	KUNIT_CASE(svcsock_rx_region_escaped_capacity_test),
	KUNIT_CASE(svcsock_rx_credit_stock_test),
	KUNIT_CASE(svcsock_rx_credit_stock_record_test),
	KUNIT_CASE(svcsock_rx_credit_stock_learn_test),
	KUNIT_CASE(svcsock_rx_bounce_stocked_test),
	KUNIT_CASE(svcsock_rx_credit_stock_live_change_test),
	KUNIT_CASE(svcsock_rx_credit_stock_netns_test),
	KUNIT_CASE(svcsock_rx_second_consumer_test),
	KUNIT_CASE(svcsock_rx_default_sizing_test),
	KUNIT_CASE(svcsock_rx_floor_follows_threads_test),
	KUNIT_CASE(svcsock_rx_consumer_rebind_test),
	KUNIT_CASE(svcsock_rx_region_default_off_test),
	KUNIT_CASE(svcsock_rx_computed_default_test),
	KUNIT_CASE(svcsock_rx_netns_lend_test),
	KUNIT_CASE(svcsock_rx_rcvbuf_default_test),
	{}
};

/*
 * The suite exercises the loan paths directly, so every test starts with
 * a default budget to loan against, whatever the host has set; the suite
 * puts the host's budgets back when it ends.  The fixtures' pages are not
 * the region's, so the budget is never charged; a test about credit sets
 * the budgets it needs itself.
 */
static unsigned int svcsock_rx_saved_copy_below;

static int svcsock_rx_test_init(struct kunit *test)
{
	svcsock_rx_saved_copy_below = svc_tcp_rx_copy_below;
	svc_tcp_rx_budget_kunit_default(SVCSOCK_RX_LOAN_BUDGET, 0);
	svc_tcp_rx_copy_below = 0;	/* the loan tests use small records */
	return 0;
}

static void svcsock_rx_test_exit(struct kunit *test)
{
	svc_tcp_rx_copy_below = svcsock_rx_saved_copy_below;
}

static int svcsock_rx_suite_init(struct kunit_suite *suite)
{
	svc_tcp_rx_kunit_save();
	return 0;
}

static void svcsock_rx_suite_exit(struct kunit_suite *suite)
{
	svc_tcp_rx_kunit_restore();
}

static struct kunit_suite svcsock_rx_test_suite = {
	.name = "sunrpc-svcsock-rx",
	.suite_init = svcsock_rx_suite_init,
	.suite_exit = svcsock_rx_suite_exit,
	.init = svcsock_rx_test_init,
	.exit = svcsock_rx_test_exit,
	.test_cases = svcsock_rx_test_cases,
};

kunit_test_suite(svcsock_rx_test_suite);

MODULE_DESCRIPTION("KUnit tests for SUNRPC TCP receive ownership");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
