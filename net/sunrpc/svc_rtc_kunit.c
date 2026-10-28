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
	KUNIT_EXPECT_EQ(test, svc_rtc_recv_step(&f->xprt, &reason),
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
	KUNIT_EXPECT_EQ(test, svc_rtc_recv_step(&f->xprt, &reason),
			SVC_RTC_THREAD);
	KUNIT_EXPECT_EQ(test, reason, SVC_RTC_FB_DECLINED);
	KUNIT_EXPECT_EQ(test, rtc_want_calls, 1);
	/* XPT_BUSY is still held: it passes to the thread with the xprt. */
	KUNIT_EXPECT_TRUE(test, test_bit(XPT_BUSY, &f->xprt.xpt_flags));
	rtc_fixture_done(test, f);
}

/* The work item has no receive yet: even a wanting hook hands back. */
static void rtc_hook_wants_no_receive_hands_back(struct kunit *test)
{
	struct rtc_fixture *f = rtc_fixture_new(test, true);
	enum svc_rtc_reason reason = SVC_RTC_NR_REASONS;

	KUNIT_ASSERT_EQ(test, svc_rtc_register(&f->serv, &rtc_ops), 0);
	rtc_want_result = true;
	KUNIT_EXPECT_EQ(test, svc_rtc_recv_step(&f->xprt, &reason),
			SVC_RTC_THREAD);
	KUNIT_EXPECT_EQ(test, reason, SVC_RTC_FB_NO_RECEIVE);
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
		KUNIT_EXPECT_EQ(test, svc_rtc_recv_step(&f->xprt, &reason),
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
	KUNIT_EXPECT_EQ(test, svc_rtc_recv_step(&f->xprt, &reason),
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
	KUNIT_EXPECT_GE(test, f->xprt.xpt_rtc->cpu, 0);
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

static struct kunit_case svc_rtc_test_cases[] = {
	KUNIT_CASE(rtc_no_work_item_no_divert),
	KUNIT_CASE(rtc_no_hook),
	KUNIT_CASE(rtc_hook_declines),
	KUNIT_CASE(rtc_hook_wants_no_receive_hands_back),
	KUNIT_CASE(rtc_xprt_state_goes_to_thread),
	KUNIT_CASE(rtc_dead_xprt_idle),
	KUNIT_CASE(rtc_send_fifo_budget),
	KUNIT_CASE(rtc_queue_send_runs_work),
	KUNIT_CASE(rtc_stop_releases_sends),
	KUNIT_CASE(rtc_send_after_stop_released),
	KUNIT_CASE(rtc_register_once),
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
