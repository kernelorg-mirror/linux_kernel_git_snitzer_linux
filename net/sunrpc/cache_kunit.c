// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the cache_check_rcu() refresh path when the upcall has
 * no listener (for example auth.unix.gid with mountd not managing gids).
 *
 * try_to_negate_entry() is the only place on this path that takes
 * detail->hash_lock, and it always ends in cache_fresh_unlocked(), which
 * clears CACHE_PENDING.  A CACHE_PENDING bit planted on a valid entry is
 * therefore a lock-free witness: if it survives cache_check_rcu(), the
 * lookup did not take hash_lock.
 */

#include <kunit/test.h>

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sunrpc/cache.h>
#include <net/net_namespace.h>

#define CT_HASH_SIZE	4

struct ct_ent {
	struct cache_head h;
	int key;
	struct rcu_head rcu;
};

static atomic_t ct_upcalls;

static void ct_put(struct kref *ref)
{
	struct ct_ent *e = container_of(ref, struct ct_ent, h.ref);

	kfree_rcu(e, rcu);
}

static int ct_upcall(struct cache_detail *cd, struct cache_head *h)
{
	atomic_inc(&ct_upcalls);
	return sunrpc_cache_upcall_warn(cd, h);
}

static struct cache_head *ct_alloc(void)
{
	struct ct_ent *e = kzalloc_obj(*e);

	return e ? &e->h : NULL;
}

static int ct_match(struct cache_head *a, struct cache_head *b)
{
	return container_of(a, struct ct_ent, h)->key ==
	       container_of(b, struct ct_ent, h)->key;
}

static void ct_init(struct cache_head *cnew, struct cache_head *citem)
{
	container_of(cnew, struct ct_ent, h)->key =
		container_of(citem, struct ct_ent, h)->key;
}

static void ct_update(struct cache_head *cnew, struct cache_head *citem)
{
}

static const struct cache_detail ct_template = {
	.owner		= THIS_MODULE,
	.hash_size	= CT_HASH_SIZE,
	.name		= "kunit.cache",
	.cache_put	= ct_put,
	.cache_upcall	= ct_upcall,
	.alloc		= ct_alloc,
	.match		= ct_match,
	.init		= ct_init,
	.update		= ct_update,
};

static int ct_test_init(struct kunit *test)
{
	struct cache_detail *cd = cache_create_net(&ct_template, &init_net);

	if (IS_ERR(cd))
		return PTR_ERR(cd);
	sunrpc_init_cache_detail(cd);
	atomic_set(&ct_upcalls, 0);
	test->priv = cd;
	return 0;
}

static void ct_test_exit(struct kunit *test)
{
	struct cache_detail *cd = test->priv;

	atomic_set(&cd->writers, 0);
	sunrpc_destroy_cache_detail(cd);
	cache_destroy_net(cd, &init_net);
}

/* A new entry, not yet valid, with one reference held by the caller. */
static struct cache_head *ct_lookup(struct kunit *test, int key)
{
	struct ct_ent k = { .key = key };
	struct cache_head *h;

	h = sunrpc_cache_lookup_rcu(test->priv, &k.h, key % CT_HASH_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, h);
	KUNIT_ASSERT_FALSE(test, test_bit(CACHE_VALID, &h->flags));
	return h;
}

/*
 * A valid positive entry 100 s into a 120 s lifetime: past refresh_age/2,
 * not expired.  flush_time is moved below last_refresh so the cleaner
 * does not treat the back-dated entry as flushed.
 */
static struct cache_head *ct_valid_aged(struct kunit *test, int key)
{
	struct cache_detail *cd = test->priv;
	struct ct_ent n = { .key = key };
	struct cache_head *h = ct_lookup(test, key);
	time64_t now = seconds_since_boot();

	n.h.expiry_time = now + CACHE_NEW_EXPIRY;
	h = sunrpc_cache_update(cd, &n.h, h, key % CT_HASH_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, h);
	KUNIT_ASSERT_TRUE(test, test_bit(CACHE_VALID, &h->flags));
	KUNIT_ASSERT_FALSE(test, test_bit(CACHE_NEGATIVE, &h->flags));

	cd->flush_time = now - 200;
	h->last_refresh = now - 100;
	h->expiry_time = now + 20;
	return h;
}

static int ct_queued(struct cache_detail *cd)
{
	int n;

	spin_lock(&cd->queue_lock);
	n = list_count_nodes(&cd->requests);
	spin_unlock(&cd->queue_lock);
	return n;
}

/* No listener: a valid entry is left alone and hash_lock is not taken. */
static void ct_valid_no_listener_keeps_entry(struct kunit *test)
{
	struct cache_detail *cd = test->priv;
	struct cache_req req = {};
	struct cache_head *h = ct_valid_aged(test, 1);
	time64_t expiry = h->expiry_time, refresh = h->last_refresh;

	set_bit(CACHE_PENDING, &h->flags);	/* witness, see top of file */
	KUNIT_EXPECT_EQ(test, cache_check_rcu(cd, h, &req), 0);
	KUNIT_EXPECT_EQ(test, cache_check_rcu(cd, h, &req), 0);

	/* refresh was attempted on each lookup, so it is not starved */
	KUNIT_EXPECT_EQ(test, atomic_read(&ct_upcalls), 2);
	KUNIT_EXPECT_TRUE(test, test_bit(CACHE_PENDING, &h->flags));
	KUNIT_EXPECT_TRUE(test, test_bit(CACHE_VALID, &h->flags));
	KUNIT_EXPECT_FALSE(test, test_bit(CACHE_NEGATIVE, &h->flags));
	KUNIT_EXPECT_EQ(test, h->expiry_time, expiry);
	KUNIT_EXPECT_EQ(test, h->last_refresh, refresh);
	KUNIT_EXPECT_EQ(test, ct_queued(cd), 0);

	clear_bit(CACHE_PENDING, &h->flags);
	cache_put(h, cd);
}

/* No listener: an entry that is not valid is still negated. */
static void ct_invalid_no_listener_negates(struct kunit *test)
{
	struct cache_detail *cd = test->priv;
	struct cache_req req = {};
	struct cache_head *h = ct_lookup(test, 2);

	KUNIT_EXPECT_EQ(test, cache_check_rcu(cd, h, &req), -ENOENT);
	KUNIT_EXPECT_EQ(test, atomic_read(&ct_upcalls), 1);
	KUNIT_EXPECT_TRUE(test, test_bit(CACHE_VALID, &h->flags));
	KUNIT_EXPECT_TRUE(test, test_bit(CACHE_NEGATIVE, &h->flags));
	KUNIT_EXPECT_GT(test, h->expiry_time, seconds_since_boot());
	cache_put(h, cd);
}

/*
 * A listener that appears after a failed refresh gets the refresh, and
 * CACHE_PENDING keeps it to one queued upcall across lookups.
 */
static void ct_listener_appears_one_upcall(struct kunit *test)
{
	struct cache_detail *cd = test->priv;
	struct cache_req req = {};
	struct cache_head *h = ct_valid_aged(test, 3);

	KUNIT_EXPECT_EQ(test, cache_check_rcu(cd, h, &req), 0);
	KUNIT_EXPECT_EQ(test, ct_queued(cd), 0);
	KUNIT_EXPECT_FALSE(test, test_bit(CACHE_PENDING, &h->flags));

	atomic_inc(&cd->writers);		/* listener opened the channel */
	KUNIT_EXPECT_EQ(test, cache_check_rcu(cd, h, &req), 0);
	KUNIT_EXPECT_EQ(test, cache_check_rcu(cd, h, &req), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&ct_upcalls), 3);
	KUNIT_EXPECT_TRUE(test, test_bit(CACHE_PENDING, &h->flags));
	KUNIT_EXPECT_EQ(test, ct_queued(cd), 1);
	KUNIT_EXPECT_TRUE(test, test_bit(CACHE_VALID, &h->flags));
	KUNIT_EXPECT_FALSE(test, test_bit(CACHE_NEGATIVE, &h->flags));

	/* the purge in ct_test_exit dequeues the request */
	cache_put(h, cd);
}

static struct kunit_case ct_test_cases[] = {
	KUNIT_CASE(ct_valid_no_listener_keeps_entry),
	KUNIT_CASE(ct_invalid_no_listener_negates),
	KUNIT_CASE(ct_listener_appears_one_upcall),
	{}
};

static struct kunit_suite ct_test_suite = {
	.name = "sunrpc_cache",
	.init = ct_test_init,
	.exit = ct_test_exit,
	.test_cases = ct_test_cases,
};

kunit_test_suite(ct_test_suite);

MODULE_DESCRIPTION("KUnit tests for the SUNRPC cache refresh path");
MODULE_LICENSE("GPL");
