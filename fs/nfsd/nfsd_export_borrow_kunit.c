// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for nfsd's per-thread export borrow: nfsd_exp_borrow(),
 * nfsd_exp_slot_return(), nfsd_exp_slots_drain(), nfsd_exp_slot_exit() and
 * their callers fh_verify() and fh_put().
 *
 * Each test builds two test nets with their own expkey and export caches,
 * made from nfsd's cache templates and filled with svc_expkey_lookup()/
 * svc_expkey_update() and svc_export_lookup()/svc_export_update(), so no
 * mountd is needed.  The caches' cache_upcall is a test callback that
 * counts upcalls per cache and either answers synchronously (as mountd
 * would), leaves the entry pending, or runs a drain first.  Exported paths
 * are the roots of two internal mounts whose fh_to_dentry() fails, which
 * can also run a drain between the borrow and the decode failure.
 *
 * The suite is named under "sunrpc" so that the sunrpc* KUnit boot runs it.
 */
#include <kunit/test.h>

#include <linux/cred.h>
#include <linux/exportfs.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/sunrpc/cache.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svcauth.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <uapi/linux/nfsd_netlink.h>

#include "nfsd.h"
#include "netns.h"
#include "export.h"
#include "nfsfh.h"
#include "vfs.h"
#include "netlink.h"
#include "nfserr.h"

#define EBT_TTL		3600
#define EBT_EX_FLAGS	(NFSEXP_NOSUBTREECHECK | NFSEXP_INSECURE_PORT)
#define EBT_FSID	1

/*
 * WARN_ON_ONCE() on an architecture without a bug table (UML) is spent at
 * its first hit even while suppressed; see net/sunrpc/svcsock_kunit.c.
 * The bad_return counter is the exact check.
 */
#ifdef __WARN_FLAGS
#define EBT_EXPECT_ONCE_WARNING(test) \
	KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1)
#else
#define EBT_EXPECT_ONCE_WARNING(test) \
	KUNIT_EXPECT_LE(test, KUNIT_SUPPRESSED_WARNING_COUNT(), 1)
#endif

enum {
	EBT_ANSWER,		/* validate the entry, as mountd would */
	EBT_PEND,		/* leave it pending: the request defers */
	EBT_DRAIN_ANSWER,	/* drain the slots, then answer */
	EBT_DRAIN_FAIL,		/* drain the slots, then report no listener */
};

struct ebt_net {
	struct net		*net;
	struct nfsd_net		*nn;
	bool			counters;
	bool			stats;
};

struct ebt {
	struct kunit		*test;
	struct ebt_net		n[2];
	struct vfsmount		*mnt[2];
	struct path		path[2];
	struct auth_domain	*dom[2];
	struct svc_rqst		*rqstp;
	struct nfsd_thread_local_info *ntli;
	struct svc_rqst		*rtc;		/* a pool rqst, freed at exit */
	struct svc_xprt		xprt;
	u32			fsid;

	/* the entries ebt_install() made in net 0 */
	struct svc_expkey	*ek;
	struct svc_export	*exp;

	int			upcall_mode;
	const struct path	*answer_path;	/* an answered expkey's path */
	unsigned int		ek_upcalls;
	unsigned int		exp_upcalls;
	unsigned int		defers;
	unsigned int		revisits;
	struct cache_deferred_req dreq;
	unsigned int		decodes;
	bool			decode_drain;
};

static struct ebt *ebt_cur;

#define EBT_SLOT(t)	(&(t)->ntli->ntli_exp_slot)

/* The entry the slot used last; its first when every entry is empty. */
static struct nfsd_exp_slot_ent *ebt_ent(struct nfsd_exp_slot *s)
{
	struct nfsd_exp_slot_ent *e = &s->ent[0];
	unsigned int i;

	for (i = 1; i < s->nr; i++)
		if (s->ent[i].used > e->used)
			e = &s->ent[i];
	return e;
}
#define EBT_ENT(t)	ebt_ent(EBT_SLOT(t))

static unsigned int ebt_ref(struct cache_head *h)
{
	return kref_read(&h->ref);
}

static bool ebt_is_ek_cd(struct ebt *t, struct cache_detail *cd)
{
	return cd == t->n[0].nn->svc_expkey_cache ||
	       cd == t->n[1].nn->svc_expkey_cache;
}

static void ebt_export_fields(struct svc_export *key)
{
	key->ex_flags = EBT_EX_FLAGS;
	key->ex_xprtsec_modes = NFSEXP_XPRTSEC_ALL;
	key->h.expiry_time = seconds_since_boot() + EBT_TTL;
}

/* Answer a lookup's upcall the way mountd's downcall would. */
static void ebt_answer(struct ebt *t, struct cache_detail *cd,
		       struct cache_head *h)
{
	if (ebt_is_ek_cd(t, cd)) {
		struct svc_expkey *ek = container_of(h, struct svc_expkey, h);
		struct svc_expkey key = { }, *res;

		key.ek_client = ek->ek_client;
		key.ek_fsidtype = ek->ek_fsidtype;
		memcpy(key.ek_fsid, ek->ek_fsid, sizeof(key.ek_fsid));
		key.ek_path = *t->answer_path;
		key.h.expiry_time = seconds_since_boot() + EBT_TTL;
		cache_get(h);
		res = svc_expkey_update(cd, &key, ek);
		if (res)
			cache_put(&res->h, cd);
	} else {
		struct svc_export *exp = container_of(h, struct svc_export, h);
		struct svc_export key = { }, *res;

		key.ex_client = exp->ex_client;
		key.ex_path = exp->ex_path;
		key.cd = cd;
		ebt_export_fields(&key);
		cache_get(h);
		res = svc_export_update(&key, exp);
		if (res)
			exp_put(res);
	}
}

static int ebt_upcall(struct cache_detail *cd, struct cache_head *h)
{
	struct ebt *t = ebt_cur;

	if (ebt_is_ek_cd(t, cd))
		t->ek_upcalls++;
	else
		t->exp_upcalls++;
	switch (t->upcall_mode) {
	case EBT_PEND:
		set_bit(CACHE_PENDING, &h->flags);
		return 0;
	case EBT_DRAIN_FAIL:
		nfsd_exp_slots_drain();
		return -EINVAL;
	case EBT_DRAIN_ANSWER:
		nfsd_exp_slots_drain();
		break;
	}
	ebt_answer(t, cd, h);
	return 0;
}

static void ebt_revisit(struct cache_deferred_req *dreq, int too_many)
{
	container_of(dreq, struct ebt, dreq)->revisits++;
}

static struct cache_deferred_req *ebt_defer(struct cache_req *req)
{
	struct ebt *t = ebt_cur;

	t->defers++;
	if (t->upcall_mode != EBT_PEND)
		return NULL;
	t->dreq.revisit = ebt_revisit;
	return &t->dreq;
}

static struct dentry *ebt_fh_to_dentry(struct super_block *sb,
				       struct fid *fid, int fh_len,
				       int fh_type)
{
	struct ebt *t = ebt_cur;

	t->decodes++;
	if (t->decode_drain)
		nfsd_exp_slots_drain();
	return ERR_PTR(-ESTALE);
}

static const struct export_operations ebt_export_ops = {
	.fh_to_dentry	= ebt_fh_to_dentry,
};

static int ebt_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx = init_pseudo(fc, ANON_INODE_FS_MAGIC);

	if (!ctx)
		return -ENOMEM;
	ctx->eops = &ebt_export_ops;
	return 0;
}

static struct file_system_type ebt_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "nfsd_borrow_kunit",
	.init_fs_context = ebt_init_fs_context,
	.kill_sb	= kill_anon_super,
};

static struct cache_detail *ebt_cache(struct kunit *test, struct net *net,
				      const struct cache_detail *tmpl)
{
	struct cache_detail *cd = cache_create_net(tmpl, net);

	KUNIT_ASSERT_FALSE(test, IS_ERR(cd));
	sunrpc_init_cache_detail(cd);
	cd->cache_upcall = ebt_upcall;
	/* below every last_refresh, so that a test can age an entry */
	cd->flush_time = -100000;
	return cd;
}

/* A test net: just enough struct net for net_generic(net, nfsd_net_id). */
static void ebt_net_init(struct kunit *test, struct ebt_net *en)
{
	size_t ngsz = max(sizeof(struct net_generic),
			  (nfsd_net_id + 1) * sizeof(void *));
	struct net_generic *ng;

	en->net = kunit_kzalloc(test, sizeof(*en->net), GFP_KERNEL);
	en->nn = kunit_kzalloc(test, sizeof(*en->nn), GFP_KERNEL);
	ng = kunit_kzalloc(test, ngsz, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, en->net);
	KUNIT_ASSERT_NOT_NULL(test, en->nn);
	KUNIT_ASSERT_NOT_NULL(test, ng);
	ng->ptr[nfsd_net_id] = en->nn;
	RCU_INIT_POINTER(en->net->gen, ng);
	KUNIT_ASSERT_EQ(test, percpu_counter_init_many(en->nn->counter, 0,
						       GFP_KERNEL,
						       NFSD_STATS_COUNTERS_NUM),
			0);
	en->counters = true;
	/*
	 * Each export the caches make takes a share of its export_stats,
	 * and svc_export_release() puts it from RCU, after the case.
	 */
	export_stats_init(en->nn);
	en->stats = true;
	en->nn->svc_expkey_cache = ebt_cache(test, en->net,
					     &svc_expkey_cache_template);
	en->nn->svc_export_cache = ebt_cache(test, en->net,
					     &svc_export_cache_template);
}

static void ebt_net_exit(struct ebt_net *en)
{
	struct cache_detail *cd;

	if (!en->nn)
		return;
	cd = en->nn->svc_expkey_cache;
	if (cd) {
		sunrpc_destroy_cache_detail(cd);
		cache_destroy_net(cd, en->net);
	}
	cd = en->nn->svc_export_cache;
	if (cd) {
		sunrpc_destroy_cache_detail(cd);
		cache_destroy_net(cd, en->net);
	}
	/* waits for the releases (rcu_barrier()) before the net is freed */
	if (en->stats)
		export_stats_shutdown(en->nn);
	if (en->counters)
		percpu_counter_destroy_many(en->nn->counter,
					    NFSD_STATS_COUNTERS_NUM);
}

static int ebt_init(struct kunit *test)
{
	static char *names[2] = { "nfsd-borrow-kunit-a", "nfsd-borrow-kunit-b" };
	struct ebt *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct svc_rqst *r;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->test = test;
	test->priv = t;
	ebt_cur = t;
	t->fsid = EBT_FSID;
	t->answer_path = &t->path[0];

	for (i = 0; i < 2; i++) {
		ebt_net_init(test, &t->n[i]);
		t->mnt[i] = kern_mount(&ebt_fs_type);
		KUNIT_ASSERT_FALSE(test, IS_ERR(t->mnt[i]));
		t->path[i].mnt = t->mnt[i];
		t->path[i].dentry = t->mnt[i]->mnt_root;
		t->dom[i] = unix_domain_find(names[i]);
		KUNIT_ASSERT_NOT_NULL(test, t->dom[i]);
	}

	/* What nfsd() and svc_process() give a thread's NFSv3 AUTH_SYS call */
	r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	t->ntli = kunit_kzalloc(test, sizeof(*t->ntli), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r);
	KUNIT_ASSERT_NOT_NULL(test, t->ntli);
	t->rqstp = r;
	nfsd_exp_slot_init(&t->ntli->ntli_exp_slot);
	r->rq_private = t->ntli;
	r->rq_prog = NFS_PROGRAM;
	r->rq_vers = 3;
	r->rq_client = t->dom[0];
	r->rq_cred.cr_flavor = RPC_AUTH_UNIX;
	r->rq_cred.cr_group_info = groups_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, r->rq_cred.cr_group_info);
	r->rq_chandle.defer = ebt_defer;
	r->rq_chandle.thread_wait = 0;
	t->xprt.xpt_net = t->n[0].net;
	r->rq_xprt = &t->xprt;
	return 0;
}

static void ebt_exit(struct kunit *test)
{
	struct ebt *t = test->priv;
	int i;

	if (!t)
		return;
	/* fh_verify() installed nfsd creds on this task */
	put_cred(revert_creds(get_cred(current_real_cred())));
	if (t->rtc) {
		/* a test may have freed it already; a second free is a no-op */
		nfsd3_rtc_ops.rqst_free(t->rtc);
		put_group_info(t->rtc->rq_cred.cr_group_info);
	}
	if (t->ntli) {
		nfsd_exp_slot_exit(&t->ntli->ntli_exp_slot);
		put_cred(t->ntli->ntli_user_cred);
		put_cred(t->ntli->ntli_fh_base);
		put_cred(t->ntli->ntli_fh_cred);
	}
	if (t->rqstp && t->rqstp->rq_cred.cr_group_info)
		put_group_info(t->rqstp->rq_cred.cr_group_info);
	for (i = 0; i < 2; i++)
		ebt_net_exit(&t->n[i]);
	rcu_barrier();
	for (i = 0; i < 2; i++) {
		if (t->dom[i])
			auth_domain_put(t->dom[i]);
		if (!IS_ERR_OR_NULL(t->mnt[i]))
			kern_unmount(t->mnt[i]);
	}
	ebt_cur = NULL;
}

/* Hash an expkey (@ni's net): client + fsid -> @path, or negative. */
static struct svc_expkey *ebt_set_ek(struct ebt *t, int ni,
				     struct auth_domain *dom, int type,
				     u32 *fsid, const struct path *path)
{
	struct cache_detail *cd = t->n[ni].nn->svc_expkey_cache;
	struct svc_expkey key = { }, *ek;

	key.ek_client = dom;
	key.ek_fsidtype = type;
	memcpy(key.ek_fsid, fsid, key_len(type));
	ek = svc_expkey_lookup(cd, &key);
	KUNIT_ASSERT_NOT_NULL(t->test, ek);
	key.h.expiry_time = seconds_since_boot() + EBT_TTL;
	if (path)
		key.ek_path = *path;
	else
		__set_bit(CACHE_NEGATIVE, &key.h.flags);
	ek = svc_expkey_update(cd, &key, ek);
	KUNIT_ASSERT_NOT_NULL(t->test, ek);
	cache_put(&ek->h, cd);
	return ek;	/* the hash keeps it */
}

/* Hash an export (@ni's net) of @path to @dom. */
static struct svc_export *ebt_set_exp(struct ebt *t, int ni,
				      struct auth_domain *dom,
				      const struct path *path)
{
	struct cache_detail *cd = t->n[ni].nn->svc_export_cache;
	struct svc_export key = { }, *exp;

	key.ex_client = dom;
	key.ex_path = *path;
	key.cd = cd;
	exp = svc_export_lookup(&key);
	KUNIT_ASSERT_NOT_NULL(t->test, exp);
	ebt_export_fields(&key);
	exp = svc_export_update(&key, exp);
	KUNIT_ASSERT_NOT_NULL(t->test, exp);
	exp_put(exp);
	return exp;	/* the hash keeps it */
}

/* net 0: fsid EBT_FSID of dom[0] -> path[0], exported to dom[0] */
static void ebt_install(struct ebt *t)
{
	t->ek = ebt_set_ek(t, 0, t->dom[0], FSID_NUM, &t->fsid, &t->path[0]);
	t->exp = ebt_set_exp(t, 0, t->dom[0], &t->path[0]);
}

static struct svc_export *ebt_borrow_in(struct ebt *t, int ni, int type,
					u32 *fsid, struct nfsd_exp_slot **slotp)
{
	return nfsd_exp_borrow(t->rqstp, t->n[ni].net, t->rqstp->rq_client,
			       t->rqstp->rq_gssclient, type, fsid, slotp);
}

static struct svc_export *ebt_borrow(struct ebt *t,
				     struct nfsd_exp_slot **slotp)
{
	return ebt_borrow_in(t, 0, FSID_NUM, &t->fsid, slotp);
}

/*
 * A pool rqst from nfsd3_rtc_ops.rqst_init(), given what svc_rtc's call
 * set-up gives an NFSv3 AUTH_SYS call; ebt_exit() frees it.
 */
static struct svc_rqst *ebt_rtc_new(struct ebt *t)
{
	struct kunit *test = t->test;
	struct svc_rqst *r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, r);
	KUNIT_ASSERT_EQ(test, nfsd3_rtc_ops.rqst_init(r), 0);
	t->rtc = r;
	r->rq_prog = NFS_PROGRAM;
	r->rq_vers = 3;
	r->rq_client = t->dom[0];
	r->rq_cred.cr_flavor = RPC_AUTH_UNIX;
	r->rq_cred.cr_group_info =
		get_group_info(t->rqstp->rq_cred.cr_group_info);
	r->rq_chandle.defer = ebt_defer;
	r->rq_chandle.thread_wait = 0;
	r->rq_xprt = &t->xprt;
	return r;
}

static struct nfsd_exp_slot *ebt_rtc_slot(struct svc_rqst *r)
{
	return &((struct nfsd_thread_local_info *)r->rq_private)->ntli_exp_slot;
}

static struct svc_export *ebt_rtc_borrow(struct ebt *t, struct svc_rqst *r,
					 struct nfsd_exp_slot **slotp)
{
	return nfsd_exp_borrow(r, t->n[0].net, r->rq_client, r->rq_gssclient,
			       FSID_NUM, &t->fsid, slotp);
}

struct ebt_delta {
	struct nfsd_exp_borrow_counts	c0;
};

static void ebt_mark(struct ebt_delta *d)
{
	nfsd_exp_borrow_read_counts(&d->c0);
}

#define EBT_EXPECT_DELTA(test, d, field, n) do {			\
	struct nfsd_exp_borrow_counts __c;				\
									\
	nfsd_exp_borrow_read_counts(&__c);				\
	KUNIT_EXPECT_EQ(test, __c.field - (d)->c0.field, (unsigned long)(n)); \
} while (0)

/* A borrow that misses and refills; returns the (borrowed) export. */
static struct svc_export *ebt_miss_in(struct ebt *t, int ni, int type,
				      u32 *fsid)
{
	struct kunit *test = t->test;
	struct nfsd_exp_slot *slot;
	struct svc_export *exp;
	struct ebt_delta d;

	ebt_mark(&d);
	exp = ebt_borrow_in(t, ni, type, fsid, &slot);
	KUNIT_ASSERT_FALSE(test, IS_ERR_OR_NULL(exp));
	KUNIT_ASSERT_PTR_EQ(test, slot, EBT_SLOT(t));
	EBT_EXPECT_DELTA(test, &d, hit, 0);
	EBT_EXPECT_DELTA(test, &d, fill, 1);
	KUNIT_EXPECT_PTR_EQ(test, EBT_ENT(t)->exp, exp);
	nfsd_exp_slot_return(slot);
	return exp;
}

static struct svc_export *ebt_miss(struct ebt *t)
{
	return ebt_miss_in(t, 0, FSID_NUM, &t->fsid);
}

/* A borrow that hits; returns the export. */
static struct svc_export *ebt_hit_once(struct ebt *t)
{
	struct kunit *test = t->test;
	struct nfsd_exp_slot *slot;
	struct svc_export *exp;
	struct ebt_delta d;

	ebt_mark(&d);
	exp = ebt_borrow(t, &slot);
	KUNIT_ASSERT_FALSE(test, IS_ERR_OR_NULL(exp));
	KUNIT_ASSERT_PTR_EQ(test, slot, EBT_SLOT(t));
	EBT_EXPECT_DELTA(test, &d, hit, 1);
	EBT_EXPECT_DELTA(test, &d, fill, 0);
	nfsd_exp_slot_return(slot);
	return exp;
}

static void ebt_expect_idle_empty(struct kunit *test, struct ebt *t)
{
	struct nfsd_exp_slot *s = EBT_SLOT(t);

	unsigned int i;

	KUNIT_EXPECT_FALSE(test, s->borrowed);
	KUNIT_EXPECT_FALSE(test, s->dead);
	for (i = 0; i < s->nr; i++) {
		KUNIT_EXPECT_NULL(test, s->ent[i].exp);
		KUNIT_EXPECT_NULL(test, s->ent[i].ek);
		KUNIT_EXPECT_NULL(test, s->ent[i].client);
		KUNIT_EXPECT_NULL(test, s->ent[i].net);
	}
}

/* An NFSv3-sized handle for net 0's export: fsid type FSID_NUM. */
static void ebt_fh(struct ebt *t, struct svc_fh *fhp, int fileid_type)
{
	struct knfsd_fh *fh = &fhp->fh_handle;

	fh_init(fhp, NFSD_FHSIZE_UNSPEC);
	fh->fh_version = 1;
	fh->fh_auth_type = 0;
	fh->fh_fsid_type = FSID_NUM;
	fh->fh_fileid_type = fileid_type;
	*fh_fsid(fh) = t->fsid;
	fh->fh_size = 4 + key_len(FSID_NUM);
	if (fileid_type != FILEID_ROOT) {
		/* an ino32/generation fid for the failing fh_to_dentry() */
		memset(&fh->fh_raw[fh->fh_size], 0x5a, 8);
		fh->fh_size += 8;
	}
}

/* 1: a fill, then a hit; no refcount moves across borrow and return. */
static void ebt_hit_no_refcount(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *slot;
	struct svc_export *exp;
	unsigned int rek, rexp;
	struct ebt_delta d;

	ebt_install(t);
	KUNIT_EXPECT_PTR_EQ(test, ebt_miss(t), t->exp);
	rek = ebt_ref(&t->ek->h);
	rexp = ebt_ref(&t->exp->h);

	ebt_mark(&d);
	exp = ebt_borrow(t, &slot);
	KUNIT_ASSERT_PTR_EQ(test, exp, t->exp);
	KUNIT_ASSERT_PTR_EQ(test, slot, EBT_SLOT(t));
	KUNIT_EXPECT_TRUE(test, slot->borrowed);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), rek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), rexp);
	nfsd_exp_slot_return(slot);
	KUNIT_EXPECT_FALSE(test, slot->borrowed);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), rek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), rexp);
	EBT_EXPECT_DELTA(test, &d, hit, 1);
	EBT_EXPECT_DELTA(test, &d, fill, 0);
	EBT_EXPECT_DELTA(test, &d, skip, 0);
}

/* 2: sunrpc_cache_update() replaces the export: a miss. */
static void ebt_export_replaced(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct svc_export *newexp;

	ebt_install(t);
	ebt_miss(t);
	newexp = ebt_set_exp(t, 0, t->dom[0], &t->path[0]);
	KUNIT_ASSERT_PTR_NE(test, newexp, t->exp);
	KUNIT_EXPECT_EQ(test, READ_ONCE(t->exp->h.expiry_time), 0);
	KUNIT_EXPECT_PTR_EQ(test, ebt_miss(t), newexp);
	KUNIT_EXPECT_PTR_EQ(test, ebt_hit_once(t), newexp);
}

/* 3: the expkey is replaced (fsid -> another path): a miss, new export. */
static void ebt_expkey_replaced(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct svc_export *expb;
	struct svc_expkey *ekb;

	ebt_install(t);
	expb = ebt_set_exp(t, 0, t->dom[0], &t->path[1]);
	ebt_miss(t);
	ekb = ebt_set_ek(t, 0, t->dom[0], FSID_NUM, &t->fsid, &t->path[1]);
	KUNIT_ASSERT_PTR_NE(test, ekb, t->ek);
	KUNIT_EXPECT_PTR_EQ(test, ebt_miss(t), expb);
	KUNIT_EXPECT_PTR_EQ(test, EBT_ENT(t)->ek, ekb);
}

/* 4: the export cache's flush_time passes last_refresh: a miss. */
static void ebt_export_flush_time(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct cache_detail *cd = t->n[0].nn->svc_export_cache;

	ebt_install(t);
	ebt_miss(t);
	WRITE_ONCE(cd->flush_time, READ_ONCE(t->exp->h.last_refresh));
	KUNIT_EXPECT_PTR_NE(test, ebt_miss(t), t->exp);
	KUNIT_EXPECT_EQ(test, t->exp_upcalls, 1);
	KUNIT_EXPECT_EQ(test, t->ek_upcalls, 0);
}

/* 5: the expkey cache's flush_time passes last_refresh: a miss. */
static void ebt_expkey_flush_time(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct cache_detail *cd = t->n[0].nn->svc_expkey_cache;

	ebt_install(t);
	ebt_miss(t);
	WRITE_ONCE(cd->flush_time, READ_ONCE(t->ek->h.last_refresh));
	KUNIT_EXPECT_PTR_EQ(test, ebt_miss(t), t->exp);
	KUNIT_EXPECT_PTR_NE(test, EBT_ENT(t)->ek, t->ek);
	KUNIT_EXPECT_EQ(test, t->ek_upcalls, 1);
	KUNIT_EXPECT_EQ(test, t->exp_upcalls, 0);
}

/* 6: expiry_time in the past: a miss. */
static void ebt_expired(struct kunit *test)
{
	struct ebt *t = test->priv;

	ebt_install(t);
	ebt_miss(t);
	WRITE_ONCE(t->exp->h.expiry_time, seconds_since_boot() - 1);
	KUNIT_EXPECT_PTR_NE(test, ebt_miss(t), t->exp);
	KUNIT_EXPECT_EQ(test, t->exp_upcalls, 1);
}

/*
 * 7: past half its refresh age, the next request upcalls exactly once on
 * that cache and is served; the replacement entry is then lent.
 */
static void ebt_half_age(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct svc_export *newexp;
	time64_t now;

	ebt_install(t);
	ebt_miss(t);
	now = seconds_since_boot();
	/* refresh age 15 s, age 10 s: past half, not expired */
	WRITE_ONCE(t->exp->h.last_refresh, now - 10);
	WRITE_ONCE(t->exp->h.expiry_time, now + 5);
	KUNIT_EXPECT_PTR_EQ(test, ebt_miss(t), t->exp);
	KUNIT_EXPECT_EQ(test, t->exp_upcalls, 1);
	KUNIT_EXPECT_EQ(test, t->ek_upcalls, 0);
	/* the upcall's answer replaced it */
	KUNIT_EXPECT_EQ(test, READ_ONCE(t->exp->h.expiry_time), 0);
	newexp = ebt_miss(t);
	KUNIT_EXPECT_PTR_NE(test, newexp, t->exp);
	KUNIT_EXPECT_PTR_EQ(test, ebt_hit_once(t), newexp);
	KUNIT_EXPECT_EQ(test, t->exp_upcalls, 1);
}

/* 8: a negative export entry: a miss (the refill reports -ENOENT). */
static void ebt_negative_export(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *slot;
	struct svc_export *exp;
	struct ebt_delta d;

	ebt_install(t);
	ebt_miss(t);
	set_bit(CACHE_NEGATIVE, &t->exp->h.flags);
	ebt_mark(&d);
	exp = ebt_borrow(t, &slot);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(exp), -ENOENT);
	KUNIT_EXPECT_NULL(test, slot);
	EBT_EXPECT_DELTA(test, &d, hit, 0);
	ebt_expect_idle_empty(test, t);
	clear_bit(CACHE_NEGATIVE, &t->exp->h.flags);
}

/* 9: another auth_domain misses and keeps the first; gss never borrows. */
static void ebt_other_client(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *slot;
	struct svc_export *exp;
	struct ebt_delta d;

	ebt_install(t);
	ebt_miss(t);
	t->rqstp->rq_client = t->dom[1];
	exp = ebt_miss(t);
	KUNIT_EXPECT_PTR_EQ(test, exp->ex_client, t->dom[1]);
	KUNIT_EXPECT_PTR_EQ(test, EBT_ENT(t)->client, t->dom[1]);

	/* rq_gssclient set (a gss request's fallback domain) */
	t->rqstp->rq_client = t->dom[0];
	t->rqstp->rq_gssclient = t->dom[1];
	ebt_mark(&d);
	KUNIT_EXPECT_NULL(test, ebt_borrow(t, &slot));
	KUNIT_EXPECT_NULL(test, slot);
	EBT_EXPECT_DELTA(test, &d, skip, 1);
	KUNIT_EXPECT_FALSE(test, EBT_SLOT(t)->borrowed);
	t->rqstp->rq_gssclient = NULL;

	/* a gss domain passed in while rq_gssclient is NULL */
	ebt_mark(&d);
	KUNIT_EXPECT_NULL(test, nfsd_exp_borrow(t->rqstp, t->n[0].net,
						t->dom[0], t->dom[1], FSID_NUM,
						&t->fsid, &slot));
	KUNIT_EXPECT_NULL(test, slot);
	EBT_EXPECT_DELTA(test, &d, skip, 1);
	EBT_EXPECT_DELTA(test, &d, fill, 0);
	KUNIT_EXPECT_FALSE(test, EBT_SLOT(t)->borrowed);

	/* a client passed in that is not rq_client */
	ebt_mark(&d);
	KUNIT_EXPECT_NULL(test, nfsd_exp_borrow(t->rqstp, t->n[0].net,
						t->dom[1], NULL, FSID_NUM,
						&t->fsid, &slot));
	KUNIT_EXPECT_NULL(test, slot);
	EBT_EXPECT_DELTA(test, &d, skip, 1);
	EBT_EXPECT_DELTA(test, &d, fill, 0);
	KUNIT_EXPECT_FALSE(test, EBT_SLOT(t)->borrowed);
	KUNIT_EXPECT_PTR_EQ(test, EBT_ENT(t)->client, t->dom[1]);

	/* an RPCSEC_GSS credential with no gss domain */
	t->rqstp->rq_cred.cr_flavor = RPC_AUTH_GSS_KRB5;
	ebt_mark(&d);
	KUNIT_EXPECT_NULL(test, ebt_borrow(t, &slot));
	EBT_EXPECT_DELTA(test, &d, skip, 1);
	t->rqstp->rq_cred.cr_flavor = RPC_AUTH_NULL;
	KUNIT_EXPECT_PTR_EQ(test, ebt_hit_once(t), t->exp);
	t->rqstp->rq_cred.cr_flavor = RPC_AUTH_UNIX;
}

/* 10: another net misses and fills from that net's caches; net 0 stays. */
static void ebt_other_net(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct svc_export *exp;

	ebt_install(t);
	ebt_miss(t);
	exp = ebt_miss_in(t, 1, FSID_NUM, &t->fsid);
	KUNIT_EXPECT_PTR_EQ(test, exp->cd, t->n[1].nn->svc_export_cache);
	KUNIT_EXPECT_PTR_EQ(test, EBT_ENT(t)->net, t->n[1].net);
	KUNIT_EXPECT_PTR_EQ(test, ebt_hit_once(t), t->exp);
}

/* 11: another fsid type or fsid misses; the first fsid stays cached. */
static void ebt_other_fsid(struct kunit *test)
{
	struct ebt *t = test->priv;
	u32 fsid2 = EBT_FSID + 1;
	u32 devfsid[2] = { EBT_FSID, 0 };

	ebt_install(t);
	ebt_miss(t);
	ebt_miss_in(t, 0, FSID_NUM, &fsid2);
	KUNIT_EXPECT_EQ(test, EBT_ENT(t)->fsid[0], fsid2);
	ebt_hit_once(t);
	/* the same leading bytes under another type */
	ebt_miss_in(t, 0, FSID_DEV, devfsid);
	KUNIT_EXPECT_EQ(test, EBT_ENT(t)->fsid_type, FSID_DEV);
	ebt_hit_once(t);
}

/* 11b: the entry used last is looked at first; another key is scanned. */
static void ebt_mru_first(struct kunit *test)
{
	struct ebt *t = test->priv;
	u32 fsid2 = EBT_FSID + 1;
	struct ebt_delta d;

	ebt_install(t);
	ebt_miss(t);
	ebt_miss_in(t, 0, FSID_NUM, &fsid2);	/* fsid2 is now the last */
	ebt_mark(&d);
	ebt_hit_once(t);			/* EBT_FSID: found by the scan */
	EBT_EXPECT_DELTA(test, &d, mru, 0);
	ebt_mark(&d);
	ebt_hit_once(t);			/* EBT_FSID again: the last */
	EBT_EXPECT_DELTA(test, &d, mru, 1);
	EBT_EXPECT_DELTA(test, &d, hit, 1);
	KUNIT_EXPECT_EQ(test, EBT_ENT(t)->fsid[0], (u32)EBT_FSID);
}

/* 11a: more keys than entries: the least recently used one is replaced. */
static void ebt_lru_evict(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s = EBT_SLOT(t);
	u32 fsid2 = EBT_FSID + 1, fsid3 = EBT_FSID + 2;
	unsigned int nr = s->nr;
	struct ebt_delta d;

	KUNIT_ASSERT_GE(test, nr, 2U);
	ebt_install(t);
	s->nr = 2;		/* two of the slot's entries */
	ebt_mark(&d);
	ebt_miss(t);
	ebt_miss_in(t, 0, FSID_NUM, &fsid2);
	EBT_EXPECT_DELTA(test, &d, evict, 0);
	ebt_hit_once(t);	/* EBT_FSID is now the more recent */
	ebt_mark(&d);
	ebt_miss_in(t, 0, FSID_NUM, &fsid3);
	EBT_EXPECT_DELTA(test, &d, evict, 1);
	ebt_hit_once(t);	/* EBT_FSID kept, fsid2 replaced */
	ebt_mark(&d);
	ebt_miss_in(t, 0, FSID_NUM, &fsid2);
	EBT_EXPECT_DELTA(test, &d, evict, 1);
	s->nr = nr;
}

/* 12: a drain of an idle slot drops both references. */
static void ebt_drain_idle(struct kunit *test)
{
	struct ebt *t = test->priv;
	unsigned int bek, bexp;
	struct ebt_delta d;

	ebt_install(t);
	bek = ebt_ref(&t->ek->h);
	bexp = ebt_ref(&t->exp->h);
	ebt_miss(t);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek + 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp + 1);
	ebt_mark(&d);
	nfsd_exp_slots_drain();
	EBT_EXPECT_DELTA(test, &d, drain, 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
	ebt_expect_idle_empty(test, t);
}

/*
 * 13: a drain during a borrow marks the slot dead and leaves the pair; the
 * return drops it and clears both flags; a later borrow refills and hits.
 */
static void ebt_drain_borrowed(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s = EBT_SLOT(t), *slot;
	unsigned int bek, bexp;
	struct svc_export *exp;

	ebt_install(t);
	bek = ebt_ref(&t->ek->h);
	bexp = ebt_ref(&t->exp->h);
	ebt_miss(t);
	exp = ebt_borrow(t, &slot);
	KUNIT_ASSERT_PTR_EQ(test, exp, t->exp);
	nfsd_exp_slots_drain();
	KUNIT_EXPECT_TRUE(test, s->borrowed);
	KUNIT_EXPECT_TRUE(test, s->dead);
	KUNIT_EXPECT_PTR_EQ(test, ebt_ent(s)->exp, t->exp);
	KUNIT_EXPECT_PTR_EQ(test, ebt_ent(s)->ek, t->ek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek + 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp + 1);
	/* a second drain changes nothing */
	nfsd_exp_slots_drain();
	KUNIT_EXPECT_TRUE(test, s->dead);
	KUNIT_EXPECT_PTR_EQ(test, ebt_ent(s)->exp, t->exp);
	nfsd_exp_slot_return(slot);
	ebt_expect_idle_empty(test, t);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
	KUNIT_EXPECT_PTR_EQ(test, ebt_miss(t), t->exp);
	KUNIT_EXPECT_PTR_EQ(test, ebt_hit_once(t), t->exp);
}

/* 14: a double return warns and changes nothing, also on an empty slot. */
static void ebt_double_return(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s = EBT_SLOT(t), *slot;
	unsigned int bek, bexp;
	struct ebt_delta d;

	ebt_install(t);
	bek = ebt_ref(&t->ek->h);
	bexp = ebt_ref(&t->exp->h);
	KUNIT_ASSERT_PTR_EQ(test, ebt_borrow(t, &slot), t->exp);
	nfsd_exp_slot_return(slot);

	ebt_mark(&d);
	kunit_warning_suppress(test) {
		nfsd_exp_slot_return(slot);
		EBT_EXPECT_ONCE_WARNING(test);
	}
	EBT_EXPECT_DELTA(test, &d, bad_return, 1);
	KUNIT_EXPECT_FALSE(test, s->borrowed);
	KUNIT_EXPECT_FALSE(test, s->dead);
	KUNIT_EXPECT_PTR_EQ(test, ebt_ent(s)->exp, t->exp);
	KUNIT_EXPECT_PTR_EQ(test, ebt_ent(s)->ek, t->ek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek + 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp + 1);

	nfsd_exp_slots_drain();
	ebt_expect_idle_empty(test, t);
	ebt_mark(&d);
	kunit_warning_suppress(test) {
		nfsd_exp_slot_return(slot);
		EBT_EXPECT_ONCE_WARNING(test);
	}
	EBT_EXPECT_DELTA(test, &d, bad_return, 1);
	ebt_expect_idle_empty(test, t);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
}

/* 15: thread exit drops both references and leaves the drain list. */
static void ebt_thread_exit(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s = EBT_SLOT(t);
	unsigned int bek, bexp;

	ebt_install(t);
	bek = ebt_ref(&t->ek->h);
	bexp = ebt_ref(&t->exp->h);
	ebt_miss(t);
	KUNIT_ASSERT_TRUE(test, s->registered);
	nfsd_exp_slot_exit(s);
	KUNIT_EXPECT_FALSE(test, s->registered);
	KUNIT_EXPECT_TRUE(test, list_empty(&s->link));
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
	ebt_expect_idle_empty(test, t);

	/* a drain no longer visits it: it would mark a borrowed slot dead */
	s->borrowed = true;
	nfsd_exp_slots_drain();
	KUNIT_EXPECT_FALSE(test, s->dead);
	s->borrowed = false;
}

/* 16: cache_purge() of the export cache, flush_time unchanged: a miss. */
static void ebt_export_purged(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct cache_detail *cd = t->n[0].nn->svc_export_cache;
	time64_t flush;

	ebt_install(t);
	ebt_miss(t);
	flush = READ_ONCE(cd->flush_time);
	cache_purge(cd);
	KUNIT_EXPECT_EQ(test, READ_ONCE(cd->flush_time), flush);
	ebt_miss(t);
	KUNIT_EXPECT_EQ(test, t->exp_upcalls, 1);
	KUNIT_EXPECT_EQ(test, t->ek_upcalls, 0);
}

/* 17: the same for the expkey cache. */
static void ebt_expkey_purged(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct cache_detail *cd = t->n[0].nn->svc_expkey_cache;
	time64_t flush;

	ebt_install(t);
	ebt_miss(t);
	flush = READ_ONCE(cd->flush_time);
	cache_purge(cd);
	KUNIT_EXPECT_EQ(test, READ_ONCE(cd->flush_time), flush);
	KUNIT_EXPECT_PTR_EQ(test, ebt_miss(t), t->exp);
	KUNIT_EXPECT_EQ(test, t->ek_upcalls, 1);
	KUNIT_EXPECT_EQ(test, t->exp_upcalls, 0);
}

/* 18: nfsd_nl_cache_flush_doit() (purge both, no flush_time move) drains. */
static void ebt_netlink_flush(struct kunit *test)
{
	struct nlattr *attrs[NFSD_A_CACHE_FLUSH_MAX + 1] = { };
	struct ebt *t = test->priv;
	struct nfsd_net *nn = t->n[0].nn;
	time64_t ekflush, expflush;
	struct genl_info info = { };

	info.attrs = attrs;
	genl_info_net_set(&info, t->n[0].net);
	if (genl_info_net(&info) != t->n[0].net)
		kunit_skip(test, "needs CONFIG_NET_NS");

	ebt_install(t);
	ebt_miss(t);
	/* keep both entries readable once the flush unhashes them */
	cache_get(&t->ek->h);
	cache_get(&t->exp->h);
	ekflush = nn->svc_expkey_cache->flush_time;
	expflush = nn->svc_export_cache->flush_time;
	KUNIT_EXPECT_EQ(test, nfsd_nl_cache_flush_doit(NULL, &info), 0);
	KUNIT_EXPECT_EQ(test, nn->svc_expkey_cache->flush_time, ekflush);
	KUNIT_EXPECT_EQ(test, nn->svc_export_cache->flush_time, expflush);
	KUNIT_EXPECT_TRUE(test, test_bit(CACHE_CLEANED, &t->ek->h.flags));
	/* only the test's own reference is left: hash and slot are gone */
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), 1);
	ebt_expect_idle_empty(test, t);
	cache_put(&t->ek->h, nn->svc_expkey_cache);
	exp_put(t->exp);
}

/*
 * 19: a VALID, expired entry is replaced through the normal lookup with
 * the request's cache_req: answered, the request is served after one
 * upcall; pending, fh_verify() defers exactly once and the error comes
 * back without a second lookup.
 */
static void ebt_expired_refill(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s = EBT_SLOT(t);
	struct svc_expkey *old;
	struct ebt_delta d;
	struct svc_fh fh;

	ebt_install(t);
	ebt_miss(t);
	old = t->ek;
	WRITE_ONCE(old->h.expiry_time, seconds_since_boot() - 1);
	KUNIT_EXPECT_PTR_EQ(test, ebt_miss(t), t->exp);
	KUNIT_EXPECT_EQ(test, t->ek_upcalls, 1);
	KUNIT_EXPECT_EQ(test, t->defers, 0);
	KUNIT_EXPECT_PTR_NE(test, ebt_ent(s)->ek, old);

	t->upcall_mode = EBT_PEND;
	WRITE_ONCE(ebt_ent(s)->ek->h.expiry_time, seconds_since_boot() - 1);
	ebt_fh(t, &fh, FILEID_ROOT);
	ebt_mark(&d);
	KUNIT_EXPECT_EQ(test, fh_verify(t->rqstp, &fh, 0, NFSD_MAY_NOP),
			nfserrno(-EAGAIN));
	KUNIT_EXPECT_EQ(test, t->ek_upcalls, 2);
	KUNIT_EXPECT_EQ(test, t->defers, 1);
	KUNIT_EXPECT_NULL(test, fh.fh_export);
	KUNIT_EXPECT_NULL(test, fh.fh_exp_slot);
	EBT_EXPECT_DELTA(test, &d, hit, 0);
	EBT_EXPECT_DELTA(test, &d, fill, 0);
	EBT_EXPECT_DELTA(test, &d, skip, 0);
	ebt_expect_idle_empty(test, t);
	fh_put(&fh);
	EBT_EXPECT_DELTA(test, &d, bad_return, 0);
}

/* 20: a nested fh_verify() finds the slot lent and looks up instead. */
static void ebt_nested_fh_verify(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s = EBT_SLOT(t);
	unsigned int bek, bexp;
	struct svc_fh fh1, fh2;
	struct ebt_delta d;

	ebt_install(t);
	ebt_miss(t);
	bek = ebt_ref(&t->ek->h);
	bexp = ebt_ref(&t->exp->h);
	ebt_fh(t, &fh1, FILEID_ROOT);
	ebt_fh(t, &fh2, FILEID_ROOT);
	ebt_mark(&d);
	KUNIT_ASSERT_EQ(test, fh_verify(t->rqstp, &fh1, 0, NFSD_MAY_NOP),
			nfs_ok);
	KUNIT_EXPECT_PTR_EQ(test, fh1.fh_exp_slot, s);
	KUNIT_EXPECT_PTR_EQ(test, fh1.fh_export, t->exp);
	KUNIT_ASSERT_EQ(test, fh_verify(t->rqstp, &fh2, 0, NFSD_MAY_NOP),
			nfs_ok);
	KUNIT_EXPECT_NULL(test, fh2.fh_exp_slot);
	KUNIT_EXPECT_PTR_EQ(test, fh2.fh_export, t->exp);
	EBT_EXPECT_DELTA(test, &d, hit, 1);
	EBT_EXPECT_DELTA(test, &d, skip, 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp + 1);
	fh_put(&fh2);
	KUNIT_EXPECT_TRUE(test, s->borrowed);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
	fh_put(&fh1);
	KUNIT_EXPECT_FALSE(test, s->borrowed);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
	EBT_EXPECT_DELTA(test, &d, bad_return, 0);
}

/*
 * 21: a run-to-completion pool rqst has an enabled slot, registered with
 * no borrow out; a fill, then a hit, and no refcount moves across borrow
 * and return.
 */
static void ebt_rtc_rqst(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s, *slot;
	struct svc_export *exp;
	unsigned int rek, rexp;
	struct svc_rqst *r;
	struct ebt_delta d;

	ebt_install(t);
	r = ebt_rtc_new(t);
	s = ebt_rtc_slot(r);
	KUNIT_EXPECT_TRUE(test, s->enabled);
	KUNIT_EXPECT_TRUE(test, s->registered);
	KUNIT_EXPECT_FALSE(test, s->borrowed);

	ebt_mark(&d);
	exp = ebt_rtc_borrow(t, r, &slot);
	KUNIT_ASSERT_PTR_EQ(test, exp, t->exp);
	KUNIT_ASSERT_PTR_EQ(test, slot, s);
	EBT_EXPECT_DELTA(test, &d, fill, 1);
	nfsd_exp_slot_return(slot);
	rek = ebt_ref(&t->ek->h);
	rexp = ebt_ref(&t->exp->h);

	ebt_mark(&d);
	exp = ebt_rtc_borrow(t, r, &slot);
	KUNIT_ASSERT_PTR_EQ(test, exp, t->exp);
	KUNIT_ASSERT_PTR_EQ(test, slot, s);
	KUNIT_EXPECT_TRUE(test, s->borrowed);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), rek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), rexp);
	nfsd_exp_slot_return(slot);
	KUNIT_EXPECT_FALSE(test, s->borrowed);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), rek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), rexp);
	EBT_EXPECT_DELTA(test, &d, hit, 1);
	EBT_EXPECT_DELTA(test, &d, fill, 0);
	EBT_EXPECT_DELTA(test, &d, skip, 0);

	/* a request that is not NFSv3 is not counted as a skip */
	t->rqstp->rq_vers = 4;
	ebt_mark(&d);
	KUNIT_EXPECT_NULL(test, ebt_borrow(t, &slot));
	EBT_EXPECT_DELTA(test, &d, skip, 0);
	t->rqstp->rq_vers = 3;
}

/* 22: a negative expkey: -ENOENT through the normal path, no fill. */
static void ebt_negative_expkey(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *slot;
	struct svc_export *exp;
	struct ebt_delta d;
	struct svc_fh fh;

	ebt_set_ek(t, 0, t->dom[0], FSID_NUM, &t->fsid, NULL);
	ebt_mark(&d);
	exp = ebt_borrow(t, &slot);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(exp), -ENOENT);
	KUNIT_EXPECT_NULL(test, slot);
	EBT_EXPECT_DELTA(test, &d, fill, 0);
	EBT_EXPECT_DELTA(test, &d, hit, 0);
	ebt_expect_idle_empty(test, t);

	ebt_fh(t, &fh, FILEID_ROOT);
	KUNIT_EXPECT_EQ(test, fh_verify(t->rqstp, &fh, 0, NFSD_MAY_NOP),
			nfserr_stale);
	KUNIT_EXPECT_NULL(test, fh.fh_export);
	KUNIT_EXPECT_NULL(test, fh.fh_exp_slot);
	EBT_EXPECT_DELTA(test, &d, fill, 0);
	ebt_expect_idle_empty(test, t);
	KUNIT_EXPECT_EQ(test, t->ek_upcalls, 0);
	fh_put(&fh);
	EBT_EXPECT_DELTA(test, &d, bad_return, 0);
}

/*
 * 23: fh_exp_slot set implies fh_export set.  fh_init() (which the XDR
 * decoders call) and fh_compose() leave it NULL; copying a put fh and
 * putting the copy changes nothing.
 */
static void ebt_fh_slot_invariant(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct svc_fh fh, copy;
	unsigned int bexp;
	struct ebt_delta d;

	ebt_install(t);
	fh.fh_exp_slot = EBT_SLOT(t);
	fh_init(&fh, NFS3_FHSIZE);
	KUNIT_EXPECT_NULL(test, fh.fh_exp_slot);
	KUNIT_ASSERT_EQ(test, fh_compose(&fh, t->exp, t->path[0].dentry, NULL),
			nfs_ok);
	KUNIT_EXPECT_PTR_EQ(test, fh.fh_export, t->exp);
	KUNIT_EXPECT_NULL(test, fh.fh_exp_slot);
	fh_put(&fh);

	ebt_miss(t);
	bexp = ebt_ref(&t->exp->h);
	ebt_fh(t, &fh, FILEID_ROOT);
	ebt_mark(&d);
	KUNIT_ASSERT_EQ(test, fh_verify(t->rqstp, &fh, 0, NFSD_MAY_NOP),
			nfs_ok);
	KUNIT_EXPECT_PTR_EQ(test, fh.fh_exp_slot, EBT_SLOT(t));
	KUNIT_EXPECT_NOT_NULL(test, fh.fh_export);
	fh_put(&fh);
	KUNIT_EXPECT_NULL(test, fh.fh_exp_slot);
	KUNIT_EXPECT_NULL(test, fh.fh_export);
	copy = fh;
	fh_put(&copy);
	KUNIT_EXPECT_FALSE(test, EBT_SLOT(t)->borrowed);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
	EBT_EXPECT_DELTA(test, &d, bad_return, 0);
}

/* 24: a fill's expkey cache_detail is its net's svc_expkey_cache. */
static void ebt_fill_expkey_cd(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s = EBT_SLOT(t);
	int ni;

	ebt_install(t);
	for (ni = 0; ni < 2; ni++) {
		struct nfsd_net *nn;

		ebt_miss_in(t, ni, FSID_NUM, &t->fsid);
		KUNIT_ASSERT_PTR_EQ(test, ebt_ent(s)->net, t->n[ni].net);
		nn = net_generic(ebt_ent(s)->net, nfsd_net_id);
		KUNIT_EXPECT_PTR_EQ(test, ebt_ent(s)->ekcd, nn->svc_expkey_cache);
	}
}

/* 25: a drain during a refill, from the upcall: success and error. */
static void ebt_drain_during_refill(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s = EBT_SLOT(t), *slot;
	struct svc_expkey key = { }, *ek;
	struct svc_export *exp;
	unsigned int bexp;

	ebt_install(t);
	bexp = ebt_ref(&t->exp->h);
	ebt_miss(t);

	/* refill succeeds: the borrow is lent from a dead slot */
	t->upcall_mode = EBT_DRAIN_ANSWER;
	WRITE_ONCE(t->ek->h.expiry_time, seconds_since_boot() - 1);
	exp = ebt_borrow(t, &slot);
	KUNIT_ASSERT_PTR_EQ(test, exp, t->exp);
	KUNIT_EXPECT_EQ(test, t->ek_upcalls, 1);
	KUNIT_EXPECT_TRUE(test, s->dead);
	KUNIT_EXPECT_TRUE(test, s->borrowed);
	nfsd_exp_slot_return(slot);
	ebt_expect_idle_empty(test, t);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
	/* the refilled expkey is back to its hash reference */
	key.ek_client = t->dom[0];
	key.ek_fsidtype = FSID_NUM;
	key.ek_fsid[0] = t->fsid;
	ek = svc_expkey_lookup(t->n[0].nn->svc_expkey_cache, &key);
	KUNIT_ASSERT_NOT_NULL(test, ek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&ek->h), 2);
	cache_put(&ek->h, t->n[0].nn->svc_expkey_cache);

	/* refill fails: the error return still releases the slot */
	t->upcall_mode = EBT_ANSWER;
	ebt_miss(t);
	t->upcall_mode = EBT_DRAIN_FAIL;
	WRITE_ONCE(ek->h.expiry_time, seconds_since_boot() - 1);
	exp = ebt_borrow(t, &slot);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(exp), -ENOENT);
	KUNIT_EXPECT_NULL(test, slot);
	KUNIT_EXPECT_EQ(test, t->ek_upcalls, 2);
	ebt_expect_idle_empty(test, t);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
}

/*
 * 26: a second key fills another entry of the slot, and the first
 * pair stays held there (ebt_lru_evict has a full slot drop one).
 */
static void ebt_key_change(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct svc_export *expb;
	struct svc_expkey *ekb;
	unsigned int bek, bexp;
	u32 fsid2 = EBT_FSID + 1;

	ebt_install(t);
	ekb = ebt_set_ek(t, 0, t->dom[0], FSID_NUM, &fsid2, &t->path[1]);
	expb = ebt_set_exp(t, 0, t->dom[0], &t->path[1]);
	bek = ebt_ref(&t->ek->h);
	bexp = ebt_ref(&t->exp->h);
	ebt_miss(t);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek + 1);
	KUNIT_EXPECT_PTR_EQ(test, ebt_miss_in(t, 0, FSID_NUM, &fsid2), expb);
	KUNIT_EXPECT_PTR_EQ(test, EBT_ENT(t)->ek, ekb);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek + 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp + 1);
}

/*
 * 27: a borrow, then exportfs_decode_fh() fails in nfsd_set_fh_dentry():
 * nfserr_stale, the borrow returned, the parked pair kept; with a drain in
 * between, back to baseline and an empty slot.
 */
static void ebt_decode_failure(struct kunit *test)
{
	struct ebt *t = test->priv;
	unsigned int bek, bexp;
	struct ebt_delta d;
	struct svc_fh fh;

	ebt_install(t);
	bek = ebt_ref(&t->ek->h);
	bexp = ebt_ref(&t->exp->h);
	ebt_miss(t);

	ebt_fh(t, &fh, FILEID_INO32_GEN);
	ebt_mark(&d);
	KUNIT_EXPECT_EQ(test, fh_verify(t->rqstp, &fh, 0, NFSD_MAY_NOP),
			nfserr_stale);
	KUNIT_EXPECT_EQ(test, t->decodes, 1);
	EBT_EXPECT_DELTA(test, &d, hit, 1);
	KUNIT_EXPECT_NULL(test, fh.fh_export);
	KUNIT_EXPECT_NULL(test, fh.fh_exp_slot);
	KUNIT_EXPECT_FALSE(test, EBT_SLOT(t)->borrowed);
	KUNIT_EXPECT_PTR_EQ(test, EBT_ENT(t)->exp, t->exp);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek + 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp + 1);
	fh_put(&fh);

	t->decode_drain = true;
	ebt_fh(t, &fh, FILEID_INO32_GEN);
	KUNIT_EXPECT_EQ(test, fh_verify(t->rqstp, &fh, 0, NFSD_MAY_NOP),
			nfserr_stale);
	KUNIT_EXPECT_EQ(test, t->decodes, 2);
	ebt_expect_idle_empty(test, t);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
	fh_put(&fh);
	EBT_EXPECT_DELTA(test, &d, bad_return, 0);
}

/*
 * 28: rqst_free on an idle, filled pool rqst drops both references and
 * unlinks the slot: a later drain touches nothing.
 */
static void ebt_rtc_free_idle(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *slot;
	unsigned int bek, bexp;
	struct svc_rqst *r;
	struct ebt_delta d;

	ebt_install(t);
	bek = ebt_ref(&t->ek->h);
	bexp = ebt_ref(&t->exp->h);
	r = ebt_rtc_new(t);
	KUNIT_ASSERT_PTR_EQ(test, ebt_rtc_borrow(t, r, &slot), t->exp);
	nfsd_exp_slot_return(slot);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek + 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp + 1);

	nfsd3_rtc_ops.rqst_free(r);
	KUNIT_EXPECT_NULL(test, r->rq_private);
	/* a slot left linked would leave the drain list on freed memory */
	KUNIT_ASSERT_EQ(test, ebt_ref(&t->ek->h), bek);
	KUNIT_ASSERT_EQ(test, ebt_ref(&t->exp->h), bexp);
	ebt_mark(&d);
	nfsd_exp_slots_drain();
	EBT_EXPECT_DELTA(test, &d, drain, 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
}

/*
 * 29: a drain while a pool rqst's borrow is out (fh_verify() at WRITE
 * submit, fh_put() from pc_release at completion) marks the slot dead and
 * leaves the pair; the fh_put() drops it and clears both flags.
 */
static void ebt_rtc_drain_borrowed(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s, *slot;
	unsigned int bek, bexp;
	struct svc_rqst *r;
	struct ebt_delta d;
	struct svc_fh fh;

	ebt_install(t);
	bek = ebt_ref(&t->ek->h);
	bexp = ebt_ref(&t->exp->h);
	r = ebt_rtc_new(t);
	s = ebt_rtc_slot(r);
	KUNIT_ASSERT_PTR_EQ(test, ebt_rtc_borrow(t, r, &slot), t->exp);
	nfsd_exp_slot_return(slot);

	ebt_fh(t, &fh, FILEID_ROOT);
	ebt_mark(&d);
	KUNIT_ASSERT_EQ(test, fh_verify(r, &fh, 0, NFSD_MAY_NOP), nfs_ok);
	KUNIT_EXPECT_PTR_EQ(test, fh.fh_exp_slot, s);
	KUNIT_EXPECT_PTR_EQ(test, fh.fh_export, t->exp);
	EBT_EXPECT_DELTA(test, &d, hit, 1);
	nfsd_exp_slots_drain();
	KUNIT_EXPECT_TRUE(test, s->borrowed);
	KUNIT_EXPECT_TRUE(test, s->dead);
	KUNIT_EXPECT_PTR_EQ(test, ebt_ent(s)->exp, t->exp);
	KUNIT_EXPECT_PTR_EQ(test, ebt_ent(s)->ek, t->ek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek + 1);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp + 1);
	fh_put(&fh);
	KUNIT_EXPECT_FALSE(test, s->borrowed);
	KUNIT_EXPECT_FALSE(test, s->dead);
	KUNIT_EXPECT_NULL(test, ebt_ent(s)->exp);
	KUNIT_EXPECT_NULL(test, ebt_ent(s)->ek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->ek->h), bek);
	KUNIT_EXPECT_EQ(test, ebt_ref(&t->exp->h), bexp);
	EBT_EXPECT_DELTA(test, &d, bad_return, 0);
}

/*
 * 30: a pool rqst's slot is on the drain list before its first borrow, so
 * the borrow never registers it (no nfsd_exp_slots_mutex in a pool work
 * item); the fill and the hit count no skip.
 */
static void ebt_rtc_registered_eagerly(struct kunit *test)
{
	struct ebt *t = test->priv;
	struct nfsd_exp_slot *s, *slot;
	struct svc_rqst *r;
	struct ebt_delta d;

	ebt_install(t);
	r = ebt_rtc_new(t);
	s = ebt_rtc_slot(r);
	KUNIT_EXPECT_TRUE(test, s->registered);
	KUNIT_EXPECT_FALSE(test, list_empty(&s->link));
	/* a drain visits it already: it marks a borrowed slot dead */
	s->borrowed = true;
	nfsd_exp_slots_drain();
	KUNIT_EXPECT_TRUE(test, s->dead);
	s->borrowed = false;
	s->dead = false;

	ebt_mark(&d);
	KUNIT_ASSERT_PTR_EQ(test, ebt_rtc_borrow(t, r, &slot), t->exp);
	nfsd_exp_slot_return(slot);
	KUNIT_ASSERT_PTR_EQ(test, ebt_rtc_borrow(t, r, &slot), t->exp);
	nfsd_exp_slot_return(slot);
	EBT_EXPECT_DELTA(test, &d, fill, 1);
	EBT_EXPECT_DELTA(test, &d, hit, 1);
	EBT_EXPECT_DELTA(test, &d, skip, 0);
}

static struct kunit_case nfsd_export_borrow_cases[] = {
	KUNIT_CASE(ebt_hit_no_refcount),
	KUNIT_CASE(ebt_export_replaced),
	KUNIT_CASE(ebt_expkey_replaced),
	KUNIT_CASE(ebt_export_flush_time),
	KUNIT_CASE(ebt_expkey_flush_time),
	KUNIT_CASE(ebt_expired),
	KUNIT_CASE(ebt_half_age),
	KUNIT_CASE(ebt_negative_export),
	KUNIT_CASE(ebt_other_client),
	KUNIT_CASE(ebt_other_net),
	KUNIT_CASE(ebt_other_fsid),
	KUNIT_CASE(ebt_lru_evict),
	KUNIT_CASE(ebt_mru_first),
	KUNIT_CASE(ebt_drain_idle),
	KUNIT_CASE(ebt_drain_borrowed),
	KUNIT_CASE(ebt_double_return),
	KUNIT_CASE(ebt_thread_exit),
	KUNIT_CASE(ebt_export_purged),
	KUNIT_CASE(ebt_expkey_purged),
	KUNIT_CASE(ebt_netlink_flush),
	KUNIT_CASE(ebt_expired_refill),
	KUNIT_CASE(ebt_nested_fh_verify),
	KUNIT_CASE(ebt_rtc_rqst),
	KUNIT_CASE(ebt_negative_expkey),
	KUNIT_CASE(ebt_fh_slot_invariant),
	KUNIT_CASE(ebt_fill_expkey_cd),
	KUNIT_CASE(ebt_drain_during_refill),
	KUNIT_CASE(ebt_key_change),
	KUNIT_CASE(ebt_decode_failure),
	KUNIT_CASE(ebt_rtc_free_idle),
	KUNIT_CASE(ebt_rtc_drain_borrowed),
	KUNIT_CASE(ebt_rtc_registered_eagerly),
	{}
};

static struct kunit_suite nfsd_export_borrow_suite = {
	.name		= "sunrpc-nfsd-export-borrow",
	.init		= ebt_init,
	.exit		= ebt_exit,
	.test_cases	= nfsd_export_borrow_cases,
};

kunit_test_suite(nfsd_export_borrow_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
