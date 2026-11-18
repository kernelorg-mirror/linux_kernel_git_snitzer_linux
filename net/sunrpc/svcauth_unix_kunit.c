// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for svcauth_unix's per-rqst borrow of ip_maps and gids.
 *
 * Each case runs svcauth_unix_set_client() and the flavor's release on a
 * hand-built svc_rqst whose transport is on a test net: a struct net
 * with just enough net_generic() for sunrpc_net_id, and its own ip_map
 * and unix_gid caches made from svcauth_unix's templates.  No mountd is
 * needed: entries are set as its downcalls set them, and the caches'
 * cache_upcall counts upcalls and queues nothing.  Nothing touches
 * init_net's caches.  The borrow's counters are global, so the cases
 * check them as deltas and expect no other AUTH_SYS traffic meanwhile.
 */
#include <kunit/test.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/in.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sunrpc/cache.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/svcauth.h>
#include <net/ipv6.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

#include "netns.h"

#define SAB_TTL		3600
#define SAB_ADDRS	5
#define SAB_UID		1000

struct sab {
	struct kunit		*test;
	struct net		*net;
	struct sunrpc_net	*sn;
	struct svc_xprt		xprt;
	struct svc_program	prog;
	struct svc_serv		serv;
	struct auth_ops		gss_ops;
	struct svc_rqst		*rqstp;
	struct auth_domain	*dom[2];
	struct group_info	*gi;
	struct cache_head	*ipm[SAB_ADDRS];
	struct cache_head	*ug;
	unsigned int		upcalls;
};

static struct sab *sab_current;

static int sab_upcall(struct cache_detail *cd, struct cache_head *h)
{
	if (sab_current)
		sab_current->upcalls++;
	return 0;	/* "queued": the entry stays as it is */
}

static struct cache_detail *sab_cache(struct kunit *test, struct net *net,
				      bool gid)
{
	struct cache_detail *cd = svcauth_unix_kunit_cache_create(net, gid);

	KUNIT_ASSERT_FALSE(test, IS_ERR_OR_NULL(cd));
	sunrpc_init_cache_detail(cd);
	cd->cache_upcall = sab_upcall;
	/* below every last_refresh, so that a case can age an entry */
	cd->flush_time = -100000;
	return cd;
}

static void sab_addr(int i, struct in6_addr *a6)
{
	ipv6_addr_set_v4mapped(htonl(0xc0000201 + i), a6);	/* 192.0.2.1+ */
}

static struct cache_head *sab_set_ipm(struct sab *t, int i,
				      struct auth_domain *dom)
{
	struct in6_addr a6;
	struct cache_head *h;

	sab_addr(i, &a6);
	h = svcauth_unix_kunit_ip_map_set(t->sn->ip_map_cache, "nfsd", &a6,
					  dom, seconds_since_boot() + SAB_TTL);
	KUNIT_ASSERT_NOT_NULL(t->test, h);
	return h;
}

static int sab_init(struct kunit *test)
{
	size_t ngsz = max(sizeof(struct net_generic),
			  (sunrpc_net_id + 1) * sizeof(void *));
	struct net_generic *ng;
	struct svc_rqst *r;
	struct sab *t;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->test = test;
	test->priv = t;
	sab_current = t;

	t->net = kunit_kzalloc(test, sizeof(*t->net), GFP_KERNEL);
	t->sn = kunit_kzalloc(test, sizeof(*t->sn), GFP_KERNEL);
	ng = kunit_kzalloc(test, ngsz, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->net);
	KUNIT_ASSERT_NOT_NULL(test, t->sn);
	KUNIT_ASSERT_NOT_NULL(test, ng);
	ng->ptr[sunrpc_net_id] = t->sn;
	RCU_INIT_POINTER(t->net->gen, ng);
	t->sn->ip_map_cache = sab_cache(test, t->net, false);
	t->sn->unix_gid_cache = sab_cache(test, t->net, true);

	t->dom[0] = unix_domain_find("svcauth-borrow-kunit-0");
	t->dom[1] = unix_domain_find("svcauth-borrow-kunit-1");
	KUNIT_ASSERT_NOT_NULL(test, t->dom[0]);
	KUNIT_ASSERT_NOT_NULL(test, t->dom[1]);
	t->gi = groups_alloc(2);
	KUNIT_ASSERT_NOT_NULL(test, t->gi);
	t->gi->gid[0] = KGIDT_INIT(100);
	t->gi->gid[1] = KGIDT_INIT(200);
	groups_sort(t->gi);

	t->xprt.xpt_net = t->net;
	t->prog.pg_class = "nfsd";
	t->serv.sv_programs = &t->prog;
	t->gss_ops = *svcauth_unix_kunit_ops(RPC_AUTH_UNIX);
	t->gss_ops.flavour = RPC_AUTH_GSS;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r);
	t->rqstp = r;
	r->rq_xprt = &t->xprt;
	r->rq_server = &t->serv;
	r->rq_authop = svcauth_unix_kunit_ops(RPC_AUTH_UNIX);
	r->rq_proc = 1;
	r->rq_cred.cr_uid = KUIDT_INIT(SAB_UID);
	svcauth_unix_kunit_borrow_enable(true);
	return 0;
}

static void sab_exit(struct kunit *test)
{
	struct sab *t = test->priv;
	struct cache_detail *cd;
	int i;

	svcauth_unix_kunit_borrow_enable(true);
	if (t->rqstp) {
		svcauth_unix_borrow_free(t->rqstp);
		kfree(t->rqstp);
	}
	for (i = 0; i < SAB_ADDRS; i++)
		if (t->ipm[i])
			cache_put(t->ipm[i], t->sn->ip_map_cache);
	if (t->ug)
		cache_put(t->ug, t->sn->unix_gid_cache);
	cd = t->sn ? t->sn->ip_map_cache : NULL;
	if (cd) {
		sunrpc_destroy_cache_detail(cd);
		cache_destroy_net(cd, t->net);
	}
	cd = t->sn ? t->sn->unix_gid_cache : NULL;
	if (cd) {
		sunrpc_destroy_cache_detail(cd);
		cache_destroy_net(cd, t->net);
	}
	for (i = 0; i < 2; i++)
		if (t->dom[i])
			auth_domain_put(t->dom[i]);
	if (t->gi)
		put_group_info(t->gi);
	sab_current = NULL;
}

/* One call from address @i: the wire's groups as accept sets them. */
static enum svc_auth_status sab_call(struct sab *t, int i)
{
	struct svc_rqst *r = t->rqstp;
	struct sockaddr_in *sin = (struct sockaddr_in *)&r->rq_addr;

	memset(&r->rq_addr, 0, sizeof(r->rq_addr));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(0xc0000201 + i);
	r->rq_gi_lent = false;
	r->rq_cred.cr_group_info = groups_alloc(1);
	KUNIT_ASSERT_NOT_NULL(t->test, r->rq_cred.cr_group_info);
	r->rq_cred.cr_group_info->gid[0] = KGIDT_INIT(1);
	return svcauth_unix_set_client(r);
}

static void sab_release(struct sab *t)
{
	t->rqstp->rq_authop->release(t->rqstp);
}

struct sab_delta {
	unsigned long	c0[4];
};

static void sab_mark(struct sab_delta *d)
{
	svcauth_unix_kunit_borrow_counts(d->c0);
}

/* ipm_hit, ipm_fill, ug_hit, ug_fill since sab_mark() */
#define SAB_EXPECT(test, d, ih, ifl, uh, uf) do {			\
	unsigned long __c[4];						\
									\
	svcauth_unix_kunit_borrow_counts(__c);				\
	KUNIT_EXPECT_EQ(test, __c[0] - (d)->c0[0], (unsigned long)(ih)); \
	KUNIT_EXPECT_EQ(test, __c[1] - (d)->c0[1], (unsigned long)(ifl)); \
	KUNIT_EXPECT_EQ(test, __c[2] - (d)->c0[2], (unsigned long)(uh)); \
	KUNIT_EXPECT_EQ(test, __c[3] - (d)->c0[3], (unsigned long)(uf)); \
} while (0)

static unsigned int sab_ref(struct cache_head *h)
{
	return kref_read(&h->ref);
}

static void sab_install(struct sab *t)
{
	t->ipm[0] = sab_set_ipm(t, 0, t->dom[0]);
	t->ug = svcauth_unix_kunit_gid_set(t->sn->unix_gid_cache,
					   KUIDT_INIT(SAB_UID), t->gi,
					   seconds_since_boot() + SAB_TTL);
	KUNIT_ASSERT_NOT_NULL(t->test, t->ug);
}

/* 1: a fill keeps the lookup's references; a hit takes none. */
static void sab_hit_no_refcount(struct kunit *test)
{
	struct sab *t = test->priv;
	unsigned int rdom, ripm, rug, rgi;
	struct sab_delta d;

	sab_install(t);
	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	SAB_EXPECT(test, &d, 0, 1, 0, 1);
	KUNIT_EXPECT_PTR_EQ(test, t->rqstp->rq_client, t->dom[0]);
	KUNIT_EXPECT_TRUE(test, t->rqstp->rq_client_lent);
	KUNIT_EXPECT_PTR_EQ(test, t->rqstp->rq_cred.cr_group_info, t->gi);
	KUNIT_EXPECT_TRUE(test, t->rqstp->rq_gi_lent);
	sab_release(t);

	rdom = kref_read(&t->dom[0]->ref);
	ripm = sab_ref(t->ipm[0]);
	rug = sab_ref(t->ug);
	rgi = refcount_read(&t->gi->usage);
	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	SAB_EXPECT(test, &d, 1, 0, 1, 0);
	KUNIT_EXPECT_PTR_EQ(test, t->rqstp->rq_client, t->dom[0]);
	KUNIT_EXPECT_PTR_EQ(test, t->rqstp->rq_cred.cr_group_info, t->gi);
	/* during the call: nothing taken */
	KUNIT_EXPECT_EQ(test, kref_read(&t->dom[0]->ref), rdom);
	KUNIT_EXPECT_EQ(test, sab_ref(t->ipm[0]), ripm);
	KUNIT_EXPECT_EQ(test, sab_ref(t->ug), rug);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->gi->usage), rgi);
	sab_release(t);
	/* and nothing dropped */
	KUNIT_EXPECT_NULL(test, t->rqstp->rq_client);
	KUNIT_EXPECT_FALSE(test, t->rqstp->rq_client_lent);
	KUNIT_EXPECT_FALSE(test, t->rqstp->rq_gi_lent);
	KUNIT_EXPECT_EQ(test, kref_read(&t->dom[0]->ref), rdom);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->gi->usage), rgi);
}

/* 2: switched off, a call takes and drops its references as before. */
static void sab_off(struct kunit *test)
{
	struct sab *t = test->priv;
	unsigned int rdom, rgi;
	struct sab_delta d;

	sab_install(t);
	svcauth_unix_kunit_borrow_enable(false);
	rdom = kref_read(&t->dom[0]->ref);
	rgi = refcount_read(&t->gi->usage);
	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	SAB_EXPECT(test, &d, 0, 0, 0, 0);
	KUNIT_EXPECT_PTR_EQ(test, t->rqstp->rq_client, t->dom[0]);
	KUNIT_EXPECT_FALSE(test, t->rqstp->rq_client_lent);
	KUNIT_EXPECT_FALSE(test, t->rqstp->rq_gi_lent);
	KUNIT_EXPECT_EQ(test, kref_read(&t->dom[0]->ref), rdom + 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->gi->usage), rgi + 1);
	KUNIT_EXPECT_NULL(test, t->rqstp->rq_auth_borrow);
	sab_release(t);
	KUNIT_EXPECT_EQ(test, kref_read(&t->dom[0]->ref), rdom);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->gi->usage), rgi);
}

/* 3: an ip_map replaced by a downcall is seen on the next call. */
static void sab_replaced(struct kunit *test)
{
	struct sab *t = test->priv;
	struct sab_delta d;

	sab_install(t);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	sab_release(t);
	cache_put(t->ipm[0], t->sn->ip_map_cache);
	t->ipm[0] = sab_set_ipm(t, 0, t->dom[1]);
	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	SAB_EXPECT(test, &d, 0, 1, 1, 0);
	KUNIT_EXPECT_PTR_EQ(test, t->rqstp->rq_client, t->dom[1]);
	sab_release(t);
	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	SAB_EXPECT(test, &d, 1, 0, 1, 0);
	KUNIT_EXPECT_PTR_EQ(test, t->rqstp->rq_client, t->dom[1]);
	sab_release(t);
}

/* Wait for seconds_since_boot() to move on (at most a second). */
static void sab_next_second(void)
{
	time64_t s = seconds_since_boot();

	while (seconds_since_boot() == s)
		msleep(20);
}

/*
 * 4: past half its refresh age, in a second after the one that last
 * checked it, an entry misses, and the refresh starts.
 */
static void sab_half_age(struct kunit *test)
{
	struct sab *t = test->priv;
	time64_t now = seconds_since_boot();
	struct sab_delta d;

	sab_install(t);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	sab_release(t);
	WRITE_ONCE(t->ipm[0]->expiry_time, now + 10);
	WRITE_ONCE(t->ipm[0]->last_refresh, now - 100);
	/* an entry checked this second is lent: wait for the next */
	sab_next_second();
	t->upcalls = 0;
	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	SAB_EXPECT(test, &d, 0, 1, 1, 0);
	KUNIT_EXPECT_EQ(test, t->upcalls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, t->rqstp->rq_client, t->dom[0]);
	sab_release(t);
}

/* 5: a negative ip_map denies the call and is not kept. */
static void sab_negative_ip_map(struct kunit *test)
{
	struct sab *t = test->priv;
	struct sab_delta d;
	int i;

	t->ipm[0] = sab_set_ipm(t, 0, NULL);
	for (i = 0; i < 2; i++) {
		sab_mark(&d);
		KUNIT_EXPECT_EQ(test, sab_call(t, 0), SVC_DENIED);
		SAB_EXPECT(test, &d, 0, 0, 0, 0);
		KUNIT_EXPECT_NULL(test, t->rqstp->rq_client);
		sab_release(t);
	}
}

/* 6: a negative gid keeps the wire's groups, and is kept. */
static void sab_negative_gid(struct kunit *test)
{
	struct sab *t = test->priv;
	struct group_info *wire;
	struct sab_delta d;

	t->ipm[0] = sab_set_ipm(t, 0, t->dom[0]);
	t->ug = svcauth_unix_kunit_gid_set(t->sn->unix_gid_cache,
					   KUIDT_INIT(SAB_UID), NULL,
					   seconds_since_boot() + SAB_TTL);
	KUNIT_ASSERT_NOT_NULL(test, t->ug);
	KUNIT_ASSERT_TRUE(test, test_bit(CACHE_NEGATIVE, &t->ug->flags));

	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	SAB_EXPECT(test, &d, 0, 1, 0, 1);
	wire = t->rqstp->rq_cred.cr_group_info;
	KUNIT_EXPECT_PTR_NE(test, wire, t->gi);
	KUNIT_EXPECT_EQ(test, wire->ngroups, 1);
	KUNIT_EXPECT_FALSE(test, t->rqstp->rq_gi_lent);
	sab_release(t);

	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	SAB_EXPECT(test, &d, 1, 0, 1, 0);
	KUNIT_EXPECT_EQ(test, t->rqstp->rq_cred.cr_group_info->ngroups, 1);
	KUNIT_EXPECT_FALSE(test, t->rqstp->rq_gi_lent);
	sab_release(t);
}

/* 7: two addresses in turns are both kept. */
static void sab_two_addrs(struct kunit *test)
{
	struct sab *t = test->priv;
	struct sab_delta d;
	int i;

	sab_install(t);
	t->ipm[1] = sab_set_ipm(t, 1, t->dom[1]);
	sab_mark(&d);
	for (i = 0; i < 6; i++) {
		KUNIT_ASSERT_EQ(test, sab_call(t, i & 1), SVC_OK);
		KUNIT_EXPECT_PTR_EQ(test, t->rqstp->rq_client, t->dom[i & 1]);
		sab_release(t);
	}
	SAB_EXPECT(test, &d, 4, 2, 5, 1);
}

/* 8: a fifth address replaces the least recently used. */
static void sab_lru(struct kunit *test)
{
	struct sab *t = test->priv;
	unsigned int base0;
	struct sab_delta d;
	int i;

	sab_install(t);
	for (i = 1; i < SAB_ADDRS; i++)
		t->ipm[i] = sab_set_ipm(t, i, t->dom[0]);
	base0 = sab_ref(t->ipm[0]);
	for (i = 0; i < SAB_ADDRS - 1; i++) {
		KUNIT_ASSERT_EQ(test, sab_call(t, i), SVC_OK);
		sab_release(t);
	}
	KUNIT_EXPECT_EQ(test, sab_ref(t->ipm[0]), base0 + 1);
	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, SAB_ADDRS - 1), SVC_OK);
	sab_release(t);
	SAB_EXPECT(test, &d, 0, 1, 1, 0);
	/* address 0 was the least recently used: its reference is gone */
	KUNIT_EXPECT_EQ(test, sab_ref(t->ipm[0]), base0);
	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	sab_release(t);
	SAB_EXPECT(test, &d, 0, 1, 1, 0);
}

/* 9: a gss flavor's call (its fallback domain) does not borrow. */
static void sab_gss(struct kunit *test)
{
	struct sab *t = test->priv;
	unsigned int rdom;
	struct sab_delta d;

	sab_install(t);
	t->rqstp->rq_authop = &t->gss_ops;
	rdom = kref_read(&t->dom[0]->ref);
	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	SAB_EXPECT(test, &d, 0, 0, 0, 0);
	KUNIT_EXPECT_FALSE(test, t->rqstp->rq_client_lent);
	KUNIT_EXPECT_EQ(test, kref_read(&t->dom[0]->ref), rdom + 1);
	sab_release(t);
	KUNIT_EXPECT_EQ(test, kref_read(&t->dom[0]->ref), rdom);
}

/* 10: freeing the rqst drops every reference its cache held. */
static void sab_free(struct kunit *test)
{
	struct sab *t = test->priv;
	unsigned int ripm[2], rug;
	int i;

	sab_install(t);
	t->ipm[1] = sab_set_ipm(t, 1, t->dom[1]);
	ripm[0] = sab_ref(t->ipm[0]);
	ripm[1] = sab_ref(t->ipm[1]);
	rug = sab_ref(t->ug);
	for (i = 0; i < 2; i++) {
		KUNIT_ASSERT_EQ(test, sab_call(t, i), SVC_OK);
		sab_release(t);
	}
	KUNIT_EXPECT_EQ(test, sab_ref(t->ipm[0]), ripm[0] + 1);
	KUNIT_EXPECT_EQ(test, sab_ref(t->ipm[1]), ripm[1] + 1);
	KUNIT_EXPECT_EQ(test, sab_ref(t->ug), rug + 1);
	svcauth_unix_borrow_free(t->rqstp);
	KUNIT_EXPECT_NULL(test, t->rqstp->rq_auth_borrow);
	KUNIT_EXPECT_EQ(test, sab_ref(t->ipm[0]), ripm[0]);
	KUNIT_EXPECT_EQ(test, sab_ref(t->ipm[1]), ripm[1]);
	KUNIT_EXPECT_EQ(test, sab_ref(t->ug), rug);
}

/* 11: the NULL procedure sets no client and borrows nothing. */
static void sab_null_proc(struct kunit *test)
{
	struct sab *t = test->priv;
	struct sab_delta d;

	sab_install(t);
	t->rqstp->rq_proc = 0;
	sab_mark(&d);
	KUNIT_ASSERT_EQ(test, sab_call(t, 0), SVC_OK);
	SAB_EXPECT(test, &d, 0, 0, 0, 0);
	KUNIT_EXPECT_NULL(test, t->rqstp->rq_client);
	KUNIT_EXPECT_NULL(test, t->rqstp->rq_auth_borrow);
	sab_release(t);
}

static struct kunit_case sab_cases[] = {
	KUNIT_CASE(sab_hit_no_refcount),
	KUNIT_CASE(sab_off),
	KUNIT_CASE(sab_replaced),
	KUNIT_CASE(sab_half_age),
	KUNIT_CASE(sab_negative_ip_map),
	KUNIT_CASE(sab_negative_gid),
	KUNIT_CASE(sab_two_addrs),
	KUNIT_CASE(sab_lru),
	KUNIT_CASE(sab_gss),
	KUNIT_CASE(sab_free),
	KUNIT_CASE(sab_null_proc),
	{}
};

static struct kunit_suite sab_suite = {
	.name		= "svcauth_unix_borrow",
	.init		= sab_init,
	.exit		= sab_exit,
	.test_cases	= sab_cases,
};
kunit_test_suite(sab_suite);

MODULE_DESCRIPTION("KUnit tests for svcauth_unix's per-rqst ip_map and gid borrow");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
