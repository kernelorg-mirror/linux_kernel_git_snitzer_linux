// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the SUNRPC server run-to-completion work item: the offer
 * hook, the hand-back decision, the serialized send queue and teardown.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/xdr.h>
#include <net/net_namespace.h>

struct rtc_fixture {
	struct svc_serv serv;
	struct svc_xprt xprt;
};

static unsigned int rtc_want_calls;
static bool rtc_want_result;

static bool rtc_want(struct svc_xprt *xprt)
{
	rtc_want_calls++;
	return rtc_want_result;
}

static const struct svc_rtc_ops rtc_ops = {
	.want = rtc_want,
};

static struct rtc_fixture *rtc_fixture_new(struct kunit *test, bool attach)
{
	struct rtc_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	kref_init(&f->xprt.xpt_ref);
	mutex_init(&f->xprt.xpt_mutex);
	f->xprt.xpt_server = &f->serv;
	set_bit(XPT_TEMP, &f->xprt.xpt_flags);
	set_bit(XPT_DATA, &f->xprt.xpt_flags);
	set_bit(XPT_BUSY, &f->xprt.xpt_flags);
	KUNIT_ASSERT_EQ(test, svc_rtc_serv_prepare(&f->serv), 0);
	if (attach)
		KUNIT_ASSERT_EQ(test, svc_rtc_xprt_attach(&f->xprt, GFP_KERNEL), 0);
	rtc_want_calls = 0;
	rtc_want_result = false;
	return f;
}

static void rtc_fixture_done(struct kunit *test, struct rtc_fixture *f)
{
	if (rcu_access_pointer(f->serv.sv_rtc))
		svc_rtc_unregister(&f->serv);
	svc_rtc_xprt_stop(&f->xprt);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->serv.sv_rtc_active), 0);
	KUNIT_EXPECT_EQ(test, kref_read(&f->xprt.xpt_ref), 1);
	svc_rtc_xprt_detach(&f->xprt);
	KUNIT_EXPECT_NULL(test, f->xprt.xpt_rtc);
	svc_rtc_serv_destroy(&f->serv);
	KUNIT_EXPECT_NULL(test, f->serv.sv_rtc_wq);
}

/* No hook registered at accept: no work item, svc_xprt_enqueue() unchanged. */
static void rtc_no_work_item_no_divert(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, false);

	KUNIT_ASSERT_EQ(test, svc_rtc_register(&f->serv, &rtc_ops), 0);
	KUNIT_EXPECT_FALSE(test, svc_rtc_should_divert(&f->xprt));
	KUNIT_EXPECT_EQ(test, rtc_want_calls, 0);
	rtc_fixture_done(test, f);
}

/* Work item but hook gone (unregistered): no divert; a queued run hands back. */
static void rtc_no_hook(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, true);
	enum svc_rtc_reason reason = SVC_RTC_NR_REASONS;

	KUNIT_EXPECT_FALSE(test, svc_rtc_should_divert(&f->xprt));
	KUNIT_EXPECT_EQ(test, svc_rtc_recv_step(&f->xprt, &reason, NULL),
			SVC_RTC_THREAD);
	KUNIT_EXPECT_EQ(test, reason, SVC_RTC_FB_NO_HOOK);
	rtc_fixture_done(test, f);
}

/* A declining hook: diverted, asked once, handed to a thread. */
static void rtc_hook_declines(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, true);
	enum svc_rtc_reason reason = SVC_RTC_NR_REASONS;

	KUNIT_ASSERT_EQ(test, svc_rtc_register(&f->serv, &rtc_ops), 0);
	KUNIT_EXPECT_TRUE(test, svc_rtc_should_divert(&f->xprt));
	KUNIT_EXPECT_EQ(test, svc_rtc_recv_step(&f->xprt, &reason, NULL),
			SVC_RTC_THREAD);
	KUNIT_EXPECT_EQ(test, reason, SVC_RTC_FB_DECLINED);
	KUNIT_EXPECT_EQ(test, rtc_want_calls, 1);
	/* XPT_BUSY is still held: it passes to the thread with the xprt. */
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_BUSY, &f->xprt.xpt_flags));
	rtc_fixture_done(test, f);
}

/* A wanting hook on a transport that cannot peek hands back unread. */
static void rtc_hook_wants_no_peek_hands_back(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, true);
	enum svc_rtc_reason reason = SVC_RTC_NR_REASONS;

	KUNIT_ASSERT_EQ(test, svc_rtc_register(&f->serv, &rtc_ops), 0);
	rtc_want_result = true;
	KUNIT_EXPECT_EQ(test, svc_rtc_recv_step(&f->xprt, &reason, NULL),
			SVC_RTC_THREAD);
	KUNIT_EXPECT_EQ(test, reason, SVC_RTC_FB_NO_PEEK);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_BUSY, &f->xprt.xpt_flags));
	rtc_fixture_done(test, f);
}

/* Connect, close, handshake and deferred work never reach the hook. */
static void rtc_xprt_state_goes_to_thread(struct kunit *test)
{
	static const int bits[] = { XPT_CONN, XPT_CLOSE, XPT_HANDSHAKE,
				    XPT_DEFERRED };
	struct rtc_fixture *f = rtc_fixture_new(test, true);
	int i;

	KUNIT_ASSERT_EQ(test, svc_rtc_register(&f->serv, &rtc_ops), 0);
	for (i = 0; i < ARRAY_SIZE(bits); i++) {
		enum svc_rtc_reason reason = SVC_RTC_NR_REASONS;

		set_bit(bits[i], &f->xprt.xpt_flags);
		KUNIT_EXPECT_FALSE(test, svc_rtc_should_divert(&f->xprt));
		KUNIT_EXPECT_EQ(test, svc_rtc_recv_step(&f->xprt, &reason, NULL),
				SVC_RTC_THREAD);
		KUNIT_EXPECT_EQ(test, reason, SVC_RTC_FB_XPRT_STATE);
		clear_bit(bits[i], &f->xprt.xpt_flags);
	}
	KUNIT_EXPECT_EQ(test, rtc_want_calls, 0);
	rtc_fixture_done(test, f);
}

/* A dead transport is neither diverted nor handed on; XPT_BUSY stays set. */
static void rtc_dead_xprt_idle(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, true);
	enum svc_rtc_reason reason = SVC_RTC_NR_REASONS;

	KUNIT_ASSERT_EQ(test, svc_rtc_register(&f->serv, &rtc_ops), 0);
	set_bit(XPT_DEAD, &f->xprt.xpt_flags);
	KUNIT_EXPECT_FALSE(test, svc_rtc_should_divert(&f->xprt));
	KUNIT_EXPECT_EQ(test, svc_rtc_recv_step(&f->xprt, &reason, NULL),
			SVC_RTC_IDLE);
	KUNIT_EXPECT_EQ(test, reason, SVC_RTC_FB_STOPPING);
	KUNIT_EXPECT_EQ(test, rtc_want_calls, 0);
	rtc_fixture_done(test, f);
}

struct rtc_test_send {
	struct svc_rtc_send send;
	int id;
	int *log;
	int *nlog;
	bool *dead_seen;
	bool *locked_seen;
};

static void rtc_test_send_fn(struct svc_xprt *xprt, struct svc_rtc_send *send,
			     bool dead)
{
	struct rtc_test_send *t = container_of(send, struct rtc_test_send, send);

	t->log[(*t->nlog)++] = t->id;
	t->dead_seen[t->id] = dead;
	t->locked_seen[t->id] = mutex_is_locked(&xprt->xpt_mutex);
}

/* More than SVC_RTC_SEND_BUDGET, less than two batches. */
#define RTC_NSENDS (SVC_RTC_SEND_BUDGET + 2)

struct rtc_send_set {
	struct rtc_test_send sends[RTC_NSENDS];
	int log[RTC_NSENDS];
	int nlog;
	bool dead[RTC_NSENDS];
	bool locked[RTC_NSENDS];
};

static void rtc_send_set_init(struct rtc_send_set *set)
{
	int i;

	memset(set, 0, sizeof(*set));
	for (i = 0; i < RTC_NSENDS; i++) {
		set->sends[i].send.fn = rtc_test_send_fn;
		set->sends[i].id = i;
		set->sends[i].log = set->log;
		set->sends[i].nlog = &set->nlog;
		set->sends[i].dead_seen = set->dead;
		set->sends[i].locked_seen = set->locked;
	}
}

/* Replies go out in queue order, at most a budget per xpt_mutex hold. */
static void rtc_send_fifo_budget(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, true);
	struct rtc_send_set *set = kunit_kzalloc(test, sizeof(*set), GFP_KERNEL);
	struct svc_rtc_conn *conn;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, set);
	conn = f->xprt.xpt_rtc;
	rtc_send_set_init(set);
	for (i = 0; i < RTC_NSENDS - 1; i++)
		llist_add(&set->sends[i].send.node, &conn->sendq);
	KUNIT_EXPECT_EQ(test, svc_rtc_drain_sends(conn, false),
			SVC_RTC_SEND_BUDGET);
	KUNIT_EXPECT_NOT_NULL(test, conn->pending);
	/* A reply queued now goes out after the ones already taken. */
	llist_add(&set->sends[RTC_NSENDS - 1].send.node, &conn->sendq);
	KUNIT_EXPECT_EQ(test, svc_rtc_drain_sends(conn, false),
			RTC_NSENDS - SVC_RTC_SEND_BUDGET);
	KUNIT_EXPECT_NULL(test, conn->pending);
	KUNIT_EXPECT_EQ(test, set->nlog, RTC_NSENDS);
	for (i = 0; i < RTC_NSENDS; i++) {
		KUNIT_EXPECT_EQ(test, set->log[i], i);
		KUNIT_EXPECT_FALSE(test, set->dead[i]);
		KUNIT_EXPECT_TRUE(test, set->locked[i]);
	}
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&f->xprt.xpt_mutex));
	KUNIT_EXPECT_EQ(test, svc_rtc_drain_sends(conn, false), 0);
	rtc_fixture_done(test, f);
}

/* svc_rtc_queue_send() runs the work item, which sends and drops its refs. */
static void rtc_queue_send_runs_work(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, true);
	struct rtc_send_set *set = kunit_kzalloc(test, sizeof(*set), GFP_KERNEL);
	int i;

	KUNIT_ASSERT_NOT_NULL(test, set);
	rtc_send_set_init(set);
	for (i = 0; i < RTC_NSENDS; i++)
		svc_rtc_queue_send(&f->xprt, &set->sends[i].send);
	/* Passes over the budget requeue themselves; wait for the last. */
	for (i = 0; i < 2 * RTC_NSENDS && set->nlog < RTC_NSENDS; i++)
		flush_work(&f->xprt.xpt_rtc->work);
	KUNIT_EXPECT_EQ(test, set->nlog, RTC_NSENDS);
	KUNIT_EXPECT_NULL(test, f->xprt.xpt_rtc->pending);
	for (i = 0; i < RTC_NSENDS; i++) {
		KUNIT_EXPECT_EQ(test, set->log[i], i);
		KUNIT_EXPECT_FALSE(test, set->dead[i]);
	}
	/* Only a receive kick records a CPU; a send kick runs where it is. */
	KUNIT_EXPECT_EQ(test, f->xprt.xpt_rtc->cpu, -1);
	rtc_fixture_done(test, f);
}

/* Teardown releases queued replies unsent and stops later diversion. */
static void rtc_stop_releases_sends(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, true);
	struct rtc_send_set *set = kunit_kzalloc(test, sizeof(*set), GFP_KERNEL);
	int i;

	KUNIT_ASSERT_NOT_NULL(test, set);
	KUNIT_ASSERT_EQ(test, svc_rtc_register(&f->serv, &rtc_ops), 0);
	rtc_send_set_init(set);
	for (i = 0; i < RTC_NSENDS; i++)
		llist_add(&set->sends[i].send.node, &f->xprt.xpt_rtc->sendq);
	set_bit(XPT_DEAD, &f->xprt.xpt_flags);
	svc_rtc_xprt_stop(&f->xprt);
	KUNIT_EXPECT_EQ(test, set->nlog, RTC_NSENDS);
	for (i = 0; i < RTC_NSENDS; i++) {
		KUNIT_EXPECT_TRUE(test, set->dead[i]);
		KUNIT_EXPECT_FALSE(test, set->locked[i]);
	}
	clear_bit(XPT_DEAD, &f->xprt.xpt_flags);
	KUNIT_EXPECT_FALSE(test, svc_rtc_should_divert(&f->xprt));
	rtc_fixture_done(test, f);
}

/* A send queued after stop is released by its own pass, not sent. */
static void rtc_send_after_stop_released(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, true);
	struct rtc_send_set *set = kunit_kzalloc(test, sizeof(*set), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, set);
	rtc_send_set_init(set);
	set_bit(XPT_DEAD, &f->xprt.xpt_flags);
	svc_rtc_xprt_stop(&f->xprt);
	svc_rtc_queue_send(&f->xprt, &set->sends[0].send);
	flush_work(&f->xprt.xpt_rtc->work);
	KUNIT_EXPECT_EQ(test, set->nlog, 1);
	KUNIT_EXPECT_TRUE(test, set->dead[0]);
	rtc_fixture_done(test, f);
}

/* One hook per service; unregister returns with no run outstanding. */
static void rtc_register_once(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, true);

	KUNIT_EXPECT_EQ(test, svc_rtc_register(&f->serv, &rtc_ops), 0);
	KUNIT_EXPECT_EQ(test, svc_rtc_register(&f->serv, &rtc_ops), -EBUSY);
	svc_rtc_unregister(&f->serv);
	KUNIT_EXPECT_NULL(test, rcu_access_pointer(f->serv.sv_rtc));
	KUNIT_EXPECT_FALSE(test, svc_rtc_should_divert(&f->xprt));
	KUNIT_EXPECT_EQ(test, svc_rtc_register(&f->serv, &rtc_ops), 0);
	rtc_fixture_done(test, f);
}

/*
 * End-to-end: a real svc_serv, a fake TCP-like transport and a fake
 * program whose procedure RTC_PROC_FAST stands in for NFSv3 READ.  The
 * same canned call is served once on the thread path and once on the
 * fast path; the replies must be byte-identical.
 */
#define RTC_PROG	0x20000999
#define RTC_VERS	3
#define RTC_PROC_SLOW	1
#define RTC_PROC_FAST	6
#define RTC_REPLY_WORDS	16
#define RTC_REPLY_MAX	512

struct rtc_args {
	u32 offset;
	u32 count;
};

struct rtc_res {
	u32 offset;
	u32 count;
};

struct rtc_e2e {
	struct svc_serv *serv;
	struct svc_xprt xprt;
	u32 proc;
	unsigned int recvs;
	unsigned int capture;
	unsigned int reply_len[2];
	u8 reply[2][RTC_REPLY_MAX];
	u32 flavor;		/* peeked cred flavor */
	int mode;		/* RTC_M_*: what the proc does on a work item */
	bool empty;		/* receive finds no record */
	struct svc_rqst *pending;	/* async call awaiting completion */
	int region_held;	/* fake RX-region admissions outstanding */
	bool freed;
};

static struct rtc_e2e *rtc_e2e_cur;

enum { RTC_M_SYNC, RTC_M_MISS, RTC_M_ASYNC, RTC_M_EARLY, RTC_M_REGION };

static __be32 rtc_proc_func(struct svc_rqst *rqstp)
{
	struct rtc_args *a = rqstp->rq_argp;
	struct rtc_res *r = rqstp->rq_resp;

	r->offset = a->offset;
	r->count = min_t(u32, a->count, RTC_REPLY_WORDS);
	if (!test_bit(RQ_RTC_TRY, &rqstp->rq_flags))
		return rpc_success;
	switch (rtc_e2e_cur->mode) {
	case RTC_M_REGION:
		/* As if svc_rqst_rx_region_read() admitted the payload. */
		rqstp->rq_rx_region_tx =
			(struct svc_rx_region_tx *)&rtc_e2e_cur->region_held;
		rtc_e2e_cur->region_held++;
		fallthrough;
	case RTC_M_MISS:
		svc_rtc_miss(rqstp, SVC_RTC_FB_MISS);
		break;
	case RTC_M_ASYNC:
		set_bit(RQ_RTC_ASYNC, &rqstp->rq_flags);
		rtc_e2e_cur->pending = rqstp;
		break;
	case RTC_M_EARLY:
		/* Completion before the submitter returns. */
		set_bit(RQ_RTC_ASYNC, &rqstp->rq_flags);
		rtc_e2e_cur->pending = rqstp;
		svc_rtc_complete(rqstp);
		break;
	}
	return rpc_success;
}

static bool rtc_proc_decode(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct rtc_args *a = rqstp->rq_argp;

	return !xdr_stream_decode_u32(xdr, &a->offset) &&
	       !xdr_stream_decode_u32(xdr, &a->count);
}

static bool rtc_proc_encode(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct rtc_res *r = rqstp->rq_resp;
	u32 i;

	if (xdr_stream_encode_u32(xdr, r->count) < 0)
		return false;
	for (i = 0; i < r->count; i++)
		if (xdr_stream_encode_u32(xdr, r->offset * 2654435761u + i) < 0)
			return false;
	return true;
}

static int rtc_dispatch(struct svc_rqst *rqstp)
{
	const struct svc_procedure *proc = rqstp->rq_procinfo;

	if (!proc->pc_decode(rqstp, &rqstp->rq_arg_stream)) {
		*rqstp->rq_accept_statp = rpc_garbage_args;
		return 1;
	}
	*rqstp->rq_accept_statp = proc->pc_func(rqstp);
	if (test_bit(RQ_RTC_ASYNC, &rqstp->rq_flags) ||
	    test_bit(RQ_RTC_MISS, &rqstp->rq_flags))
		return 1;
	if (*rqstp->rq_accept_statp != rpc_success)
		return 1;
	return proc->pc_encode(rqstp, &rqstp->rq_res_stream) ? 1 : 0;
}

#define RTC_PROC_ENT {						\
	.pc_func	= rtc_proc_func,			\
	.pc_decode	= rtc_proc_decode,			\
	.pc_encode	= rtc_proc_encode,			\
	.pc_argsize	= sizeof(struct rtc_args),		\
	.pc_argzero	= sizeof(struct rtc_args),		\
	.pc_ressize	= sizeof(struct rtc_res),		\
	.pc_xdrressize	= RTC_REPLY_WORDS + 8,			\
	.pc_name	= "RTC",				\
}

static const struct svc_procedure rtc_procs[RTC_PROC_FAST + 1] = {
	[0 ... RTC_PROC_FAST] = RTC_PROC_ENT,
};

static const struct svc_version rtc_version = {
	.vs_vers	= RTC_VERS,
	.vs_nproc	= ARRAY_SIZE(rtc_procs),
	.vs_proc	= rtc_procs,
	.vs_xdrsize	= 64,
	.vs_dispatch	= rtc_dispatch,
};

static const struct svc_version *rtc_versions[RTC_VERS + 1] = {
	[RTC_VERS] = &rtc_version,
};

static enum svc_auth_status rtc_authenticate(struct svc_rqst *rqstp)
{
	return SVC_OK;
}

static struct svc_program rtc_program = {
	.pg_prog		= RTC_PROG,
	.pg_lovers		= RTC_VERS,
	.pg_hivers		= RTC_VERS,
	.pg_nvers		= ARRAY_SIZE(rtc_versions),
	.pg_vers		= rtc_versions,
	.pg_name		= "rtctest",
	.pg_class		= "rtctest",
	.pg_authenticate	= rtc_authenticate,
	.pg_init_request	= svc_generic_init_request,
};

/* One AUTH_NULL call, delivered whole, as svc_tcp_recvfrom() would. */
static int rtc_x_recvfrom(struct svc_rqst *rqstp)
{
	struct rtc_e2e *e = rtc_e2e_cur;
	__be32 *p = rqstp->rq_arg.head[0].iov_base;
	const u32 words[] = { 0x1234abcd, RPC_CALL, RPC_VERSION, RTC_PROG,
			      RTC_VERS, e->proc, 0, 0, 0, 0, 7, 12 };
	unsigned int i;

	if (e->empty) {
		clear_bit(XPT_DATA, &e->xprt.xpt_flags);
		svc_xprt_received(&e->xprt);
		return 0;
	}
	for (i = 0; i < ARRAY_SIZE(words); i++)
		p[i] = cpu_to_be32(words[i]);
	rqstp->rq_arg.head[0].iov_len = sizeof(words);
	rqstp->rq_arg.page_len = 0;
	rqstp->rq_arg.len = sizeof(words);
	rqstp->rq_xprt_ctxt = NULL;
	rqstp->rq_prot = IPPROTO_TCP;
	e->recvs++;
	clear_bit(XPT_DATA, &e->xprt.xpt_flags);
	svc_xprt_received(&e->xprt);
	return sizeof(words);
}

static int rtc_x_sendto(struct svc_rqst *rqstp)
{
	struct rtc_e2e *e = rtc_e2e_cur;
	struct xdr_buf *xb = &rqstp->rq_res;
	unsigned int i = e->capture;

	if (xb->len > RTC_REPLY_MAX ||
	    read_bytes_from_xdr_buf(xb, 0, e->reply[i], xb->len))
		return -EIO;
	e->reply_len[i] = xb->len;
	return xb->len;
}

static int rtc_x_peek(struct svc_xprt *xprt, u32 *prog, u32 *vers, u32 *proc,
		      u32 *flavor)
{
	*flavor = rtc_e2e_cur->flavor;
	*prog = RTC_PROG;
	*vers = RTC_VERS;
	*proc = rtc_e2e_cur->proc;
	return 0;
}

static void rtc_x_release_ctxt(struct svc_xprt *xprt, void *ctxt)
{
}

static int rtc_x_has_wspace(struct svc_xprt *xprt)
{
	return 1;
}

static void rtc_x_free(struct svc_xprt *xprt)
{
	container_of(xprt, struct rtc_e2e, xprt)->freed = true;
}

/* The transport's region release: returns the share, grant and pages. */
static void rtc_x_rx_region_unconfine(struct svc_rqst *rqstp, bool served)
{
	rtc_e2e_cur->region_held--;
	rqstp->rq_rx_region_tx = NULL;
}

static const struct svc_xprt_ops rtc_x_ops = {
	.xpo_rx_region_unconfine = rtc_x_rx_region_unconfine,
	.xpo_recvfrom		= rtc_x_recvfrom,
	.xpo_sendto		= rtc_x_sendto,
	.xpo_sendto_locked	= rtc_x_sendto,
	.xpo_peek_call		= rtc_x_peek,
	.xpo_release_ctxt	= rtc_x_release_ctxt,
	.xpo_has_wspace		= rtc_x_has_wspace,
	.xpo_free		= rtc_x_free,
};

static struct svc_xprt_class rtc_x_class = {
	.xcl_name		= "rtctest",
	.xcl_ops		= &rtc_x_ops,
	.xcl_max_payload	= 65536,
};

static bool rtc_e2e_want(struct svc_xprt *xprt)
{
	return true;
}

static bool rtc_e2e_want_call(struct svc_xprt *xprt, u32 prog, u32 vers,
			      u32 proc)
{
	return prog == RTC_PROG && vers == RTC_VERS && proc == RTC_PROC_FAST;
}

static int rtc_e2e_complete(struct svc_rqst *rqstp)
{
	return rtc_proc_encode(rqstp, &rqstp->rq_res_stream) ? 1 : 0;
}

static const struct svc_rtc_ops rtc_e2e_ops = {
	.flavor		= RPC_AUTH_NULL,
	.complete	= rtc_e2e_complete,
	.want		= rtc_e2e_want,
	.want_call	= rtc_e2e_want_call,
};

static struct rtc_e2e *rtc_e2e_new(struct kunit *test, u32 proc)
{
	struct rtc_e2e *e = kunit_kzalloc(test, sizeof(*e), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, e);
	e->proc = proc;
	rtc_e2e_cur = e;
	e->serv = svc_create(&rtc_program, 65536, NULL);
	KUNIT_ASSERT_NOT_NULL(test, e->serv);
	KUNIT_ASSERT_EQ(test, svc_rtc_register(e->serv, &rtc_e2e_ops), 0);
	/* No threads: the pool is sized by hand. */
	KUNIT_ASSERT_EQ(test, svc_rtc_pool_set(e->serv, 2), 0);
	svc_xprt_init(&init_net, &rtc_x_class, &e->xprt, e->serv);
	KUNIT_ASSERT_EQ(test, svc_rtc_xprt_attach(&e->xprt, GFP_KERNEL), 0);
	/* svc_xprt_init() leaves XPT_BUSY set, as after xpo_create. */
	clear_bit(XPT_BUSY, &e->xprt.xpt_flags);
	return e;
}

static void rtc_e2e_done(struct kunit *test, struct rtc_e2e *e)
{
	svc_rtc_xprt_stop(&e->xprt);
	drain_workqueue(e->serv->sv_rtc_wq);
	/* Every pool rqst is back: none pins the idle connection. */
	KUNIT_EXPECT_EQ(test, atomic_read(&e->serv->sv_rtc_active), 0);
	svc_rtc_unregister(e->serv);
	KUNIT_EXPECT_EQ(test, kref_read(&e->xprt.xpt_ref), 1);
	svc_xprt_put(&e->xprt);
	KUNIT_EXPECT_TRUE(test, e->freed);
	svc_destroy(&e->serv);
}

/* Serve the transport (XPT_BUSY held) on the thread path, into reply[i]. */
static void rtc_e2e_thread_serve(struct kunit *test, struct rtc_e2e *e,
				 unsigned int i)
{
	struct svc_rqst *rqstp;

	rqstp = svc_rqst_alloc(e->serv, &e->serv->sv_pools[0], NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, rqstp);
	e->capture = i;
	KUNIT_EXPECT_TRUE(test, svc_rtc_thread_serve(rqstp, &e->xprt));
	svc_rqst_free(rqstp);
}

/* The fast path's READ reply is the thread path's reply, byte for byte. */
static void rtc_e2e_read_reply_identical(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b;

	set_bit(XPT_DATA, &e->xprt.xpt_flags);
	set_bit(XPT_BUSY, &e->xprt.xpt_flags);
	rtc_e2e_thread_serve(test, e, 0);
	KUNIT_EXPECT_EQ(test, e->recvs, 1);
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_BUSY, &e->xprt.xpt_flags));

	svc_rtc_stats_sum(&a);
	e->capture = 1;
	set_bit(XPT_DATA, &e->xprt.xpt_flags);
	svc_xprt_enqueue(&e->xprt);
	drain_workqueue(e->serv->sv_rtc_wq);
	svc_rtc_stats_sum(&b);

	KUNIT_EXPECT_EQ(test, e->recvs, 2);
	KUNIT_EXPECT_EQ(test, b.read_offered - a.read_offered, 1);
	KUNIT_EXPECT_EQ(test, b.fast_completed - a.fast_completed, 1);
	KUNIT_EXPECT_GT(test, e->reply_len[0], 4 * (RTC_REPLY_WORDS / 2));
	KUNIT_EXPECT_EQ(test, e->reply_len[0], e->reply_len[1]);
	KUNIT_EXPECT_EQ(test, memcmp(e->reply[0], e->reply[1], e->reply_len[0]), 0);
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_BUSY, &e->xprt.xpt_flags));
	rtc_e2e_done(test, e);
}

/* A call the hook does not serve reaches a thread queue unreceived. */
static void rtc_e2e_other_call_to_thread(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_SLOW);
	struct svc_rtc_stats a, b;
	struct svc_xprt *x;

	svc_rtc_stats_sum(&a);
	set_bit(XPT_DATA, &e->xprt.xpt_flags);
	svc_xprt_enqueue(&e->xprt);
	drain_workqueue(e->serv->sv_rtc_wq);
	svc_rtc_stats_sum(&b);

	KUNIT_EXPECT_EQ(test, e->recvs, 0);
	KUNIT_EXPECT_EQ(test, b.fallback[SVC_RTC_FB_NOT_FAST] -
			      a.fallback[SVC_RTC_FB_NOT_FAST], 1);
	KUNIT_EXPECT_EQ(test, b.read_offered - a.read_offered, 0);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_BUSY, &e->xprt.xpt_flags));
	x = lwq_dequeue(&e->serv->sv_pools[0].sp_xprts, struct svc_xprt,
			xpt_ready);
	KUNIT_EXPECT_PTR_EQ(test, x, &e->xprt);

	/* The thread's receive is the only one. */
	rtc_e2e_thread_serve(test, e, 0);
	KUNIT_EXPECT_EQ(test, e->recvs, 1);
	KUNIT_EXPECT_GT(test, e->reply_len[0], 0);
	rtc_e2e_done(test, e);
}

/* Thread-path reference reply into reply[0]. */
static void rtc_e2e_ref(struct kunit *test, struct rtc_e2e *e)
{
	set_bit(XPT_DATA, &e->xprt.xpt_flags);
	set_bit(XPT_BUSY, &e->xprt.xpt_flags);
	rtc_e2e_thread_serve(test, e, 0);
	KUNIT_EXPECT_GT(test, e->reply_len[0], 0);
}

/* A receive kick on the work item, capturing into reply[1]. */
static void rtc_e2e_kick(struct rtc_e2e *e, struct svc_rtc_stats *a,
			 struct svc_rtc_stats *b)
{
	svc_rtc_stats_sum(a);
	e->capture = 1;
	set_bit(XPT_DATA, &e->xprt.xpt_flags);
	svc_xprt_enqueue(&e->xprt);
	drain_workqueue(e->serv->sv_rtc_wq);
	svc_rtc_stats_sum(b);
}

#define RTC_FB(a, b, r)	((b).fallback[r] - (a).fallback[r])

static void rtc_e2e_expect_identical(struct kunit *test, struct rtc_e2e *e)
{
	KUNIT_EXPECT_EQ(test, e->reply_len[0], e->reply_len[1]);
	KUNIT_EXPECT_EQ(test, memcmp(e->reply[0], e->reply[1], e->reply_len[0]), 0);
}

/* The transport went to a thread queue with nothing received. */
static void rtc_e2e_expect_thread_unreceived(struct kunit *test,
					     struct rtc_e2e *e)
{
	struct svc_xprt *x;

	KUNIT_EXPECT_EQ(test, e->recvs, 0);
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_BUSY, &e->xprt.xpt_flags));
	x = lwq_dequeue(&e->serv->sv_pools[0].sp_xprts, struct svc_xprt,
			xpt_ready);
	KUNIT_EXPECT_PTR_EQ(test, x, &e->xprt);
	rtc_e2e_thread_serve(test, e, 0);
	KUNIT_EXPECT_EQ(test, e->recvs, 1);
}

/* A non-AUTH_NULL (here: AUTH_UNIX) call is peeked and left for a thread. */
static void rtc_e2e_auth_to_thread(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b;

	e->flavor = RPC_AUTH_UNIX;
	rtc_e2e_kick(e, &a, &b);
	KUNIT_EXPECT_EQ(test, RTC_FB(a, b, SVC_RTC_FB_AUTH), 1);
	KUNIT_EXPECT_EQ(test, b.read_offered - a.read_offered, 1);
	rtc_e2e_expect_thread_unreceived(test, e);
	rtc_e2e_done(test, e);
}

/* No free pool rqst: the call is left for a thread, unreceived. */
static void rtc_e2e_pool_empty(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b;

	KUNIT_ASSERT_EQ(test, svc_rtc_pool_set(e->serv, 0), 0);
	rtc_e2e_kick(e, &a, &b);
	KUNIT_EXPECT_EQ(test, RTC_FB(a, b, SVC_RTC_FB_POOL_EMPTY), 1);
	rtc_e2e_expect_thread_unreceived(test, e);
	rtc_e2e_done(test, e);
}

/* The receive finds no record: the pool rqst goes straight back. */
static void rtc_e2e_recv_empty(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b;

	e->empty = true;
	rtc_e2e_kick(e, &a, &b);
	KUNIT_EXPECT_EQ(test, b.recv_empty - a.recv_empty, 1);
	KUNIT_EXPECT_EQ(test, e->reply_len[1], 0);
	KUNIT_EXPECT_FALSE(test, test_bit(XPT_BUSY, &e->xprt.xpt_flags));
	rtc_e2e_done(test, e);
}

/* A miss after receive: a thread reruns the received call, same reply. */
static void rtc_e2e_miss_handoff(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b;
	struct svc_rqst *thr;

	rtc_e2e_ref(test, e);
	e->mode = RTC_M_MISS;
	rtc_e2e_kick(e, &a, &b);
	KUNIT_EXPECT_EQ(test, e->recvs, 2);
	KUNIT_EXPECT_EQ(test, RTC_FB(a, b, SVC_RTC_FB_MISS), 1);
	KUNIT_EXPECT_EQ(test, e->reply_len[1], 0);
	KUNIT_EXPECT_FALSE(test, lwq_empty(&e->serv->sv_pools[0].sp_rtc_rqsts));

	thr = svc_rqst_alloc(e->serv, &e->serv->sv_pools[0], NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, thr);
	KUNIT_EXPECT_TRUE(test, svc_rtc_thread_run(thr));
	KUNIT_EXPECT_FALSE(test, svc_rtc_thread_run(thr));
	svc_rqst_free(thr);
	/* No second receive: the thread ran the record the work item took. */
	KUNIT_EXPECT_EQ(test, e->recvs, 2);
	rtc_e2e_expect_identical(test, e);
	rtc_e2e_done(test, e);
}

/*
 * A call handed back to a pool that has lost its threads: a thread of the
 * pool serving it steals and runs it, with the same reply.
 */
static void rtc_e2e_miss_steal(struct kunit *test)
{
	static const unsigned int threads[] = { 1, 0 };
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b;
	struct svc_rtc_rqst *r;
	struct svc_pool_ext *ext;
	struct svc_pool *pools;
	struct svc_serv *serv;
	struct svc_rqst *thr;
	unsigned int i;

	rtc_e2e_ref(test, e);
	e->mode = RTC_M_MISS;
	rtc_e2e_kick(e, &a, &b);
	KUNIT_EXPECT_EQ(test, RTC_FB(a, b, SVC_RTC_FB_MISS), 1);

	/* An unpooled service has no sibling to take the call from. */
	thr = svc_rqst_alloc(e->serv, &e->serv->sv_pools[0], NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, thr);
	KUNIT_EXPECT_FALSE(test, svc_rtc_thread_steal(thr));
	svc_rqst_free(thr);

	/* Two pools on one node; pool 1 has no threads, pool 0 serves it. */
	pools = kunit_kcalloc(test, 2, sizeof(*pools), GFP_KERNEL);
	ext = kunit_kcalloc(test, 2, sizeof(*ext), GFP_KERNEL);
	serv = kunit_kzalloc(test, sizeof(*serv), GFP_KERNEL);
	thr = kunit_kzalloc(test, sizeof(*thr), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pools);
	KUNIT_ASSERT_NOT_NULL(test, ext);
	KUNIT_ASSERT_NOT_NULL(test, serv);
	KUNIT_ASSERT_NOT_NULL(test, thr);
	for (i = 0; i < 2; i++) {
		pools[i].sp_id = i;
		pools[i].sp_nrthrmax = threads[i];
		lwq_init(&pools[i].sp_xprts);
		lwq_init(&pools[i].sp_rtc_rqsts);
		init_llist_head(&pools[i].sp_idle_threads);
		KUNIT_ASSERT_EQ(test, percpu_counter_init(&ext[i].steals, 0,
							  GFP_KERNEL), 0);
	}
	svc_pool_ext_layout(pools, ext, 2);
	serv->sv_is_pooled = true;
	serv->sv_pools = pools;
	serv->sv_pool_ext = ext;
	thr->rq_server = serv;
	thr->rq_pool = &pools[0];

	r = lwq_dequeue(&e->serv->sv_pools[0].sp_rtc_rqsts,
			struct svc_rtc_rqst, qnode);
	KUNIT_ASSERT_NOT_NULL(test, r);
	lwq_enqueue(&r->qnode, &pools[1].sp_rtc_rqsts);
	KUNIT_EXPECT_TRUE(test, svc_rtc_thread_steal(thr));
	KUNIT_EXPECT_FALSE(test, svc_rtc_thread_steal(thr));
	KUNIT_EXPECT_TRUE(test, lwq_empty(&pools[1].sp_rtc_rqsts));
	KUNIT_EXPECT_EQ(test, percpu_counter_sum(&ext[0].steals), 1);
	for (i = 0; i < 2; i++)
		percpu_counter_destroy(&ext[i].steals);
	/* No second receive: the thief ran the record the work item took. */
	KUNIT_EXPECT_EQ(test, e->recvs, 2);
	rtc_e2e_expect_identical(test, e);
	rtc_e2e_done(test, e);
}

/* Async: no reply until the completion; then the thread path's bytes. */
static void rtc_e2e_async_complete(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b, c;

	rtc_e2e_ref(test, e);
	e->mode = RTC_M_ASYNC;
	rtc_e2e_kick(e, &a, &b);
	KUNIT_EXPECT_EQ(test, b.async_submitted - a.async_submitted, 1);
	KUNIT_EXPECT_EQ(test, e->reply_len[1], 0);
	KUNIT_ASSERT_NOT_NULL(test, e->pending);
	KUNIT_EXPECT_EQ(test, atomic_read(&e->serv->sv_rtc_active), 1);

	svc_rtc_complete(e->pending);
	drain_workqueue(e->serv->sv_rtc_wq);
	svc_rtc_stats_sum(&c);
	KUNIT_EXPECT_EQ(test, c.fast_completed - a.fast_completed, 1);
	rtc_e2e_expect_identical(test, e);
	rtc_e2e_done(test, e);
}

static unsigned long rtc_vs_count(struct rtc_e2e *e)
{
	unsigned long n = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		n += per_cpu_ptr(e->serv->sv_stats->vs_count[RTC_VERS],
				 cpu)[RTC_PROC_FAST];
	return n;
}

/* vs_count: once per call, whichever path or paths it took. */
static void rtc_e2e_vs_count_once(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_stat *st = kunit_kzalloc(test, sizeof(*st), GFP_KERNEL);
	struct svc_rtc_stats a, b;
	struct svc_rqst *thr;

	KUNIT_ASSERT_NOT_NULL(test, st);
	st->program = &rtc_program;
	KUNIT_ASSERT_EQ(test, svc_stat_alloc_counts(st), 0);
	e->serv->sv_stats = st;

	rtc_e2e_ref(test, e);			/* thread path */
	KUNIT_EXPECT_EQ(test, rtc_vs_count(e), 1);
	rtc_e2e_kick(e, &a, &b);		/* synchronous fast hit */
	KUNIT_EXPECT_EQ(test, b.fast_completed - a.fast_completed, 1);
	KUNIT_EXPECT_EQ(test, rtc_vs_count(e), 2);

	e->mode = RTC_M_MISS;			/* hand-off: the rerun counts */
	rtc_e2e_kick(e, &a, &b);
	KUNIT_EXPECT_EQ(test, RTC_FB(a, b, SVC_RTC_FB_MISS), 1);
	KUNIT_EXPECT_EQ(test, rtc_vs_count(e), 2);
	thr = svc_rqst_alloc(e->serv, &e->serv->sv_pools[0], NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, thr);
	KUNIT_EXPECT_TRUE(test, svc_rtc_thread_run(thr));
	svc_rqst_free(thr);
	KUNIT_EXPECT_EQ(test, rtc_vs_count(e), 3);

	e->mode = RTC_M_ASYNC;			/* counted when it is sent */
	e->pending = NULL;
	rtc_e2e_kick(e, &a, &b);
	KUNIT_EXPECT_EQ(test, rtc_vs_count(e), 3);
	KUNIT_ASSERT_NOT_NULL(test, e->pending);
	svc_rtc_complete(e->pending);
	drain_workqueue(e->serv->sv_rtc_wq);
	KUNIT_EXPECT_EQ(test, rtc_vs_count(e), 4);

	rtc_e2e_done(test, e);
	svc_stat_free_counts(st);
}

/* A completion that beats its submitter's return is ordered after it. */
static void rtc_e2e_early_complete(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b;

	rtc_e2e_ref(test, e);
	e->mode = RTC_M_EARLY;
	rtc_e2e_kick(e, &a, &b);
	KUNIT_EXPECT_EQ(test, b.async_submitted - a.async_submitted, 1);
	KUNIT_EXPECT_EQ(test, b.fast_completed - a.fast_completed, 1);
	rtc_e2e_expect_identical(test, e);
	rtc_e2e_done(test, e);
}

/* Submitted, then the connection closes, then the I/O completes. */
static void rtc_e2e_teardown_inflight(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b, c;

	e->mode = RTC_M_ASYNC;
	rtc_e2e_kick(e, &a, &b);
	KUNIT_ASSERT_NOT_NULL(test, e->pending);
	set_bit(XPT_DEAD, &e->xprt.xpt_flags);
	svc_rtc_xprt_stop(&e->xprt);
	svc_rtc_complete(e->pending);
	drain_workqueue(e->serv->sv_rtc_wq);
	svc_rtc_stats_sum(&c);
	/* Released unsent; the pool rqst and its xprt ref are returned. */
	KUNIT_EXPECT_EQ(test, e->reply_len[1], 0);
	KUNIT_EXPECT_EQ(test, c.fast_completed - a.fast_completed, 0);
	rtc_e2e_done(test, e);
}

/* M2: a miss hands off with its region credit returned; the rerun admits. */
static void rtc_e2e_region_handoff_balanced(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b;
	struct svc_rqst *thr;

	rtc_e2e_ref(test, e);
	e->mode = RTC_M_REGION;
	rtc_e2e_kick(e, &a, &b);
	KUNIT_EXPECT_EQ(test, RTC_FB(a, b, SVC_RTC_FB_MISS), 1);
	/* Released before the thread could see it: no double admission. */
	KUNIT_EXPECT_EQ(test, e->region_held, 0);
	thr = svc_rqst_alloc(e->serv, &e->serv->sv_pools[0], NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, thr);
	KUNIT_EXPECT_TRUE(test, svc_rtc_thread_run(thr));
	svc_rqst_free(thr);
	KUNIT_EXPECT_EQ(test, e->region_held, 0);
	rtc_e2e_expect_identical(test, e);
	rtc_e2e_done(test, e);
}

/* Receive pages not available (GFP_NOWAIT): a thread takes it, unreceived. */
static void rtc_e2e_no_mem(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b;

	WRITE_ONCE(svc_rtc_kunit_no_mem, true);
	rtc_e2e_kick(e, &a, &b);
	WRITE_ONCE(svc_rtc_kunit_no_mem, false);
	KUNIT_EXPECT_EQ(test, RTC_FB(a, b, SVC_RTC_FB_NO_MEM), 1);
	rtc_e2e_expect_thread_unreceived(test, e);
	rtc_e2e_done(test, e);
}

/* Per-connection request limit reached: a thread takes it, unreceived. */
static void rtc_e2e_no_slot(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_stats a, b;

	WRITE_ONCE(svc_rtc_kunit_no_slot, true);
	rtc_e2e_kick(e, &a, &b);
	WRITE_ONCE(svc_rtc_kunit_no_slot, false);
	KUNIT_EXPECT_EQ(test, RTC_FB(a, b, SVC_RTC_FB_NO_SLOT), 1);
	rtc_e2e_expect_thread_unreceived(test, e);
	rtc_e2e_done(test, e);
}

static struct {
	int n, active;
	unsigned int ref;
} rtc_pub;

static void rtc_pub_hook(struct svc_xprt *xprt)
{
	rtc_pub.active = atomic_read(&xprt->xpt_server->sv_rtc_active);
	rtc_pub.ref = kref_read(&xprt->xpt_ref);
	rtc_pub.n++;
}

static void rtc_pub_unhook(void *unused)
{
	WRITE_ONCE(svc_rtc_kunit_published, NULL);
}

static void rtc_pub_send_fn(struct svc_xprt *xprt, struct svc_rtc_send *send,
			    bool dead)
{
}

/*
 * M1, deterministic: from the instant svc_rtc_complete() or
 * svc_rtc_queue_send() publishes to conn->done or conn->sendq, a pass may
 * consume the entry and release the rqst's xprt ref and sv_rtc_active.
 * The publisher's own pin must already be held at that instant.
 */
static void rtc_e2e_publish_pinned(struct kunit *test)
{
	struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
	struct svc_rtc_send send = { .fn = rtc_pub_send_fn };
	struct svc_rtc_stats a, b;
	unsigned int ref;
	int active;

	rtc_e2e_ref(test, e);
	e->mode = RTC_M_ASYNC;
	rtc_e2e_kick(e, &a, &b);
	KUNIT_ASSERT_NOT_NULL(test, e->pending);
	drain_workqueue(e->serv->sv_rtc_wq);
	memset(&rtc_pub, 0, sizeof(rtc_pub));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, rtc_pub_unhook,
							NULL), 0);
	WRITE_ONCE(svc_rtc_kunit_published, rtc_pub_hook);

	active = atomic_read(&e->serv->sv_rtc_active);
	ref = kref_read(&e->xprt.xpt_ref);
	svc_rtc_complete(e->pending);
	KUNIT_EXPECT_EQ(test, rtc_pub.n, 1);
	KUNIT_EXPECT_EQ(test, rtc_pub.active, active + 1);
	KUNIT_EXPECT_EQ(test, rtc_pub.ref, ref + 1);
	drain_workqueue(e->serv->sv_rtc_wq);

	active = atomic_read(&e->serv->sv_rtc_active);
	ref = kref_read(&e->xprt.xpt_ref);
	svc_rtc_queue_send(&e->xprt, &send);
	KUNIT_EXPECT_EQ(test, rtc_pub.n, 2);
	KUNIT_EXPECT_EQ(test, rtc_pub.active, active + 1);
	KUNIT_EXPECT_EQ(test, rtc_pub.ref, ref + 1);
	drain_workqueue(e->serv->sv_rtc_wq);
	WRITE_ONCE(svc_rtc_kunit_published, NULL);

	rtc_e2e_expect_identical(test, e);
	rtc_e2e_done(test, e);
}

struct rtc_race {
	struct work_struct work;
	struct svc_rqst *rqstp;
};

static void rtc_race_complete(struct work_struct *work)
{
	svc_rtc_complete(container_of(work, struct rtc_race, work)->rqstp);
}

/*
 * M1: svc_rtc_complete() on another CPU races a work pass (a receive
 * kick) and, every other round, connection teardown.  Every pin is
 * balanced and the transport is freed exactly once (rtc_e2e_done()).
 */
static void rtc_e2e_complete_race(struct kunit *test)
{
	unsigned int round, other;

	other = cpumask_next(raw_smp_processor_id(), cpu_online_mask);
	if (other >= nr_cpu_ids)
		other = cpumask_first(cpu_online_mask);
	for (round = 0; round < 64; round++) {
		struct rtc_e2e *e = rtc_e2e_new(test, RTC_PROC_FAST);
		struct svc_rtc_stats a, b;
		struct rtc_race race;

		e->mode = RTC_M_ASYNC;
		rtc_e2e_kick(e, &a, &b);
		KUNIT_ASSERT_NOT_NULL(test, e->pending);
		e->mode = RTC_M_SYNC;
		race.rqstp = e->pending;
		INIT_WORK_ONSTACK(&race.work, rtc_race_complete);
		queue_work_on(other, system_highpri_wq, &race.work);
		set_bit(XPT_DATA, &e->xprt.xpt_flags);
		svc_xprt_enqueue(&e->xprt);
		if (round & 1) {
			set_bit(XPT_DEAD, &e->xprt.xpt_flags);
			svc_rtc_xprt_stop(&e->xprt);
		}
		flush_work(&race.work);
		destroy_work_on_stack(&race.work);
		drain_workqueue(e->serv->sv_rtc_wq);
		/* A thread-queued receive (busy pass) is served there. */
		if (!(round & 1) && test_bit(XPT_BUSY, &e->xprt.xpt_flags) &&
		    lwq_dequeue(&e->serv->sv_pools[0].sp_xprts, struct svc_xprt,
				xpt_ready))
			rtc_e2e_thread_serve(test, e, 0);
		rtc_e2e_done(test, e);
	}
}

static struct kunit_case svc_rtc_test_cases[] = {
	KUNIT_CASE(rtc_no_work_item_no_divert),
	KUNIT_CASE(rtc_no_hook),
	KUNIT_CASE(rtc_hook_declines),
	KUNIT_CASE(rtc_hook_wants_no_peek_hands_back),
	KUNIT_CASE(rtc_xprt_state_goes_to_thread),
	KUNIT_CASE(rtc_dead_xprt_idle),
	KUNIT_CASE(rtc_send_fifo_budget),
	KUNIT_CASE(rtc_queue_send_runs_work),
	KUNIT_CASE(rtc_stop_releases_sends),
	KUNIT_CASE(rtc_send_after_stop_released),
	KUNIT_CASE(rtc_register_once),
	KUNIT_CASE(rtc_e2e_read_reply_identical),
	KUNIT_CASE(rtc_e2e_other_call_to_thread),
	KUNIT_CASE(rtc_e2e_auth_to_thread),
	KUNIT_CASE(rtc_e2e_pool_empty),
	KUNIT_CASE(rtc_e2e_recv_empty),
	KUNIT_CASE(rtc_e2e_miss_handoff),
	KUNIT_CASE(rtc_e2e_miss_steal),
	KUNIT_CASE(rtc_e2e_async_complete),
	KUNIT_CASE(rtc_e2e_early_complete),
	KUNIT_CASE(rtc_e2e_vs_count_once),
	KUNIT_CASE(rtc_e2e_teardown_inflight),
	KUNIT_CASE(rtc_e2e_region_handoff_balanced),
	KUNIT_CASE(rtc_e2e_no_mem),
	KUNIT_CASE(rtc_e2e_no_slot),
	KUNIT_CASE(rtc_e2e_publish_pinned),
	KUNIT_CASE_SLOW(rtc_e2e_complete_race),
	{}
};

static struct kunit_suite svc_rtc_test_suite = {
	.name = "sunrpc_svc_rtc",
	.test_cases = svc_rtc_test_cases,
};

kunit_test_suite(svc_rtc_test_suite);

MODULE_DESCRIPTION("KUnit tests for the SUNRPC server run-to-completion work item");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
