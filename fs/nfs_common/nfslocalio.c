// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Mike Snitzer <snitzer@hammerspace.com>
 * Copyright (C) 2024 NeilBrown <neilb@suse.de>
 */

#include <linux/module.h>
#include <linux/list.h>
#include <linux/nfslocalio.h>
#include <linux/nfs3.h>
#include <linux/nfs4.h>
#include <linux/nfs_fs.h>
#include <net/netns/generic.h>

#include "localio_trace.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NFS localio protocol bypass support");

static DEFINE_SPINLOCK(nfs_uuids_lock);

/*
 * Global list of nfs_uuid_t instances
 * that is protected by nfs_uuids_lock.
 */
static LIST_HEAD(nfs_uuids);

/*
 * Lock ordering:
 * 1: nfs_uuid->lock
 * 2: nfs_uuids_lock
 * 3: nfs_uuid->list_lock (aka nn->local_clients_lock)
 *
 * May skip locks in select cases, but never hold multiple
 * locks out of order.
 */

static void nfs_uuid_io_drained(struct percpu_ref *ref)
{
	nfs_uuid_t *nfs_uuid = container_of(ref, nfs_uuid_t, io_ref);

	complete(&nfs_uuid->io_drained);
}

void nfs_uuid_init(nfs_uuid_t *nfs_uuid)
{
	RCU_INIT_POINTER(nfs_uuid->net, NULL);
	nfs_uuid->dom = NULL;
	nfs_uuid->list_lock = NULL;
	INIT_LIST_HEAD(&nfs_uuid->list);
	INIT_LIST_HEAD(&nfs_uuid->files);
	spin_lock_init(&nfs_uuid->lock);
	nfs_uuid->nfs3_localio_probe_count = 0;
	nfs_uuid->draining = false;
	nfs_uuid->probing = false;
	init_completion(&nfs_uuid->io_drained);
	/* Allocation failure leaves this client on ordinary RPC. */
	nfs_uuid->io_ref_initialized =
		!percpu_ref_init(&nfs_uuid->io_ref, nfs_uuid_io_drained,
				 PERCPU_REF_INIT_DEAD, GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(nfs_uuid_init);

bool nfs_uuid_begin(nfs_uuid_t *nfs_uuid)
{
	spin_lock(&nfs_uuid->lock);
	if (!nfs_uuid->io_ref_initialized || nfs_uuid->draining ||
	    nfs_uuid->probing || nfs_uuid->list_lock ||
	    rcu_access_pointer(nfs_uuid->net)) {
		/* This nfs_uuid is already in use */
		spin_unlock(&nfs_uuid->lock);
		return false;
	}

	spin_lock(&nfs_uuids_lock);
	if (!list_empty(&nfs_uuid->list)) {
		/* This nfs_uuid is already in use */
		spin_unlock(&nfs_uuids_lock);
		spin_unlock(&nfs_uuid->lock);
		return false;
	}
	nfs_uuid->probing = true;
	/* The global lookup must never see a partly generated UUID. */
	uuid_gen(&nfs_uuid->uuid);
	list_add_tail(&nfs_uuid->list, &nfs_uuids);
	spin_unlock(&nfs_uuids_lock);

	spin_unlock(&nfs_uuid->lock);

	return true;
}
EXPORT_SYMBOL_GPL(nfs_uuid_begin);

void nfs_uuid_end(nfs_uuid_t *nfs_uuid)
{
	spin_lock(&nfs_uuid->lock);
	BUG_ON(!nfs_uuid->probing);
	nfs_uuid->probing = false;
	if (!nfs_uuid->draining && !nfs_uuid->list_lock &&
	    !rcu_access_pointer(nfs_uuid->net)) {
		/* Only this attempt's global negotiation can be unlinked. */
		spin_lock(&nfs_uuids_lock);
		list_del_init(&nfs_uuid->list);
		spin_unlock(&nfs_uuids_lock);
	}
	spin_unlock(&nfs_uuid->lock);
}
EXPORT_SYMBOL_GPL(nfs_uuid_end);

static nfs_uuid_t * nfs_uuid_lookup_locked(const uuid_t *uuid)
{
	nfs_uuid_t *nfs_uuid;

	list_for_each_entry(nfs_uuid, &nfs_uuids, list)
		if (uuid_equal(&nfs_uuid->uuid, uuid))
			return nfs_uuid;

	return NULL;
}

static struct module *nfsd_mod;

void nfs_uuid_is_local(const uuid_t *uuid, struct list_head *list,
		       spinlock_t *list_lock, struct net *net,
		       struct auth_domain *dom, struct module *mod)
{
	nfs_uuid_t *nfs_uuid;

	rcu_read_lock();
	spin_lock(&nfs_uuids_lock);
	nfs_uuid = nfs_uuid_lookup_locked(uuid);
	spin_unlock(&nfs_uuids_lock);
	if (!nfs_uuid)
		goto out_rcu;

	/* Drop global exclusion before taking the outer UUID lock. */
	spin_lock(&nfs_uuid->lock);
	spin_lock(&nfs_uuids_lock);
	if (nfs_uuid_lookup_locked(uuid) != nfs_uuid ||
	    !nfs_uuid->probing || nfs_uuid->draining || nfs_uuid->list_lock ||
	    rcu_access_pointer(nfs_uuid->net))
		goto out_unlock;

	BUG_ON(!nfs_uuid->io_ref_initialized);
	BUG_ON(!percpu_ref_is_dying(&nfs_uuid->io_ref));
	BUG_ON(!percpu_ref_is_zero(&nfs_uuid->io_ref));
	reinit_completion(&nfs_uuid->io_drained);
	percpu_ref_reinit(&nfs_uuid->io_ref);
	__module_get(mod);
	/* Active UUID pins guarantee the same loaded NFSD module. */
	WRITE_ONCE(nfsd_mod, mod);
	kref_get(&dom->ref);
	nfs_uuid->dom = dom;
	nfs_uuid->list_lock = list_lock;

	/*
	 * Publish initialized session state and server membership together.
	 * Membership, rather than a net reference, protects the idle session.
	 */
	spin_lock(list_lock); /* list_lock is nn->local_clients_lock */
	list_move(&nfs_uuid->list, list);
	rcu_assign_pointer(nfs_uuid->net, net);
	spin_unlock(list_lock);

out_unlock:
	spin_unlock(&nfs_uuids_lock);
	spin_unlock(&nfs_uuid->lock);
out_rcu:
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(nfs_uuid_is_local);

void nfs_localio_enable_client(struct nfs_client *clp)
{
	/* nfs_uuid_is_local() does the actual enablement */
	trace_nfs_localio_enable_client(clp);
}
EXPORT_SYMBOL_GPL(nfs_localio_enable_client);

/*
 * Cleanup the nfs_uuid_t embedded in an nfs_client.
 * This is the long-form of nfs_uuid_init().
 */
static void nfs_uuid_put(nfs_uuid_t *nfs_uuid)
{
	struct nfs_client *clp = container_of(nfs_uuid, struct nfs_client, cl_uuid);
	struct nfs_file_localio *nfl;

	spin_lock(&nfs_uuid->lock);
	wait_var_event_spinlock(nfs_uuid, !nfs_uuid->draining,
				&nfs_uuid->lock);
	if (unlikely(!rcu_access_pointer(nfs_uuid->net))) {
		spin_unlock(&nfs_uuid->lock);
		return;
	}
	nfs_uuid->draining = true;
	percpu_ref_kill(&nfs_uuid->io_ref);
	spin_unlock(&nfs_uuid->lock);

	/* Retain the session and cached files until every borrower finishes. */
	wait_for_completion(&nfs_uuid->io_drained);
	spin_lock(&nfs_uuid->lock);
	RCU_INIT_POINTER(nfs_uuid->net, NULL);

	if (nfs_uuid->dom) {
		auth_domain_put(nfs_uuid->dom);
		nfs_uuid->dom = NULL;
	}

	/* Walk list of files and ensure their last references dropped */

	while ((nfl = list_first_entry_or_null(&nfs_uuid->files,
					       struct nfs_file_localio,
					       list)) != NULL) {
		/* If nfs_uuid is already NULL, nfs_close_local_fh is
		 * closing and we must wait, else we unlink and close.
		 */
		if (rcu_access_pointer(nfl->nfs_uuid) == NULL) {
			/* nfs_close_local_fh() is doing the
			 * close and we must wait. until it unlinks
			 */
			wait_var_event_spinlock(nfs_uuid,
						list_first_entry_or_null(
							&nfs_uuid->files,
							struct nfs_file_localio,
							list) != nfl,
						&nfs_uuid->lock);
			continue;
		}

		/* Remove nfl from nfs_uuid->files list */
		list_del_init(&nfl->list);
		spin_unlock(&nfs_uuid->lock);

		nfs_to_nfsd_file_put_local(&nfl->ro_file);
		nfs_to_nfsd_file_put_local(&nfl->rw_file);
		cond_resched();

		spin_lock(&nfs_uuid->lock);
		/* Now we can allow racing nfs_close_local_fh() to
		 * skip the locking.
		 */
		store_release_wake_up(&nfl->nfs_uuid, RCU_INITIALIZER(NULL));
	}

	spin_unlock(&nfs_uuid->lock);
	/*
	 * Quiesce observers of the removed net before another generation can
	 * reinitialize admission, or the final client destructor can exit it.
	 */
	synchronize_rcu();
	spin_lock(&nfs_uuid->lock);

	/* Remove client from nn->local_clients */
	if (nfs_uuid->list_lock) {
		spin_lock(nfs_uuid->list_lock);
		BUG_ON(list_empty(&nfs_uuid->list));
		list_del_init(&nfs_uuid->list);
		wake_up_var_locked(nfs_uuid->list_lock, nfs_uuid->list_lock);
		spin_unlock(nfs_uuid->list_lock);
		nfs_uuid->list_lock = NULL;
	}

	module_put(READ_ONCE(nfsd_mod));
	trace_nfs_localio_disable_client(clp);
	nfs_uuid->draining = false;
	wake_up_var_locked(nfs_uuid, &nfs_uuid->lock);
	/* Waiters must retake this lock; this is our last client access. */
	spin_unlock(&nfs_uuid->lock);
}

void nfs_localio_disable_client(struct nfs_client *clp)
{
	nfs_uuid_put(&clp->cl_uuid);
}
EXPORT_SYMBOL_GPL(nfs_localio_disable_client);

void nfs_localio_destroy_client(struct nfs_client *clp)
{
	nfs_uuid_t *nfs_uuid = &clp->cl_uuid;

	BUG_ON(nfs_uuid->draining || nfs_uuid->probing || nfs_uuid->list_lock);
	BUG_ON(rcu_access_pointer(nfs_uuid->net));
	BUG_ON(!list_empty(&nfs_uuid->list) || !list_empty(&nfs_uuid->files));
	if (nfs_uuid->io_ref_initialized) {
		BUG_ON(!percpu_ref_is_dying(&nfs_uuid->io_ref));
		BUG_ON(!percpu_ref_is_zero(&nfs_uuid->io_ref));
		percpu_ref_exit(&nfs_uuid->io_ref);
	}
}
EXPORT_SYMBOL_GPL(nfs_localio_destroy_client);

/* Resolved only during invalidation to avoid a common -> NFS module cycle. */
extern void nfs_put_client(struct nfs_client *clp);

void nfs_localio_invalidate_clients(struct list_head *nn_local_clients,
				    spinlock_t *nn_local_clients_lock)
{
	LIST_HEAD(local_clients);
	void (*put_client)(struct nfs_client *clp);
	nfs_uuid_t *nfs_uuid;
	struct nfs_client *clp;

	spin_lock(nn_local_clients_lock);
	if (list_empty(nn_local_clients)) {
		spin_unlock(nn_local_clients_lock);
		return;
	}
	put_client = symbol_get(nfs_put_client);
	BUG_ON(!put_client);
	list_splice_init(nn_local_clients, &local_clients);
	while (!list_empty(&local_clients)) {
		struct list_head *first = local_clients.next;

		nfs_uuid = list_entry(first, nfs_uuid_t, list);
		BUG_ON(nfs_uuid->list_lock != nn_local_clients_lock);
		clp = container_of(nfs_uuid, struct nfs_client, cl_uuid);
		if (!refcount_inc_not_zero(&clp->cl_count)) {
			/* The destructor owns this node; retain only its value. */
			wait_var_event_spinlock(nn_local_clients_lock,
						local_clients.next != first,
						nn_local_clients_lock);
			continue;
		}
		spin_unlock(nn_local_clients_lock);
		nfs_localio_disable_client(clp);
		put_client(clp);
		/* Never retain an unreferenced next node across the drain. */
		spin_lock(nn_local_clients_lock);
	}
	spin_unlock(nn_local_clients_lock);
	symbol_put(nfs_put_client);
}
EXPORT_SYMBOL_GPL(nfs_localio_invalidate_clients);

int nfs_uuid_add_file_locked(nfs_uuid_t *nfs_uuid,
			     struct nfs_file_localio *nfl, struct net *net)
{
	lockdep_assert_held(&nfs_uuid->lock);
	if (nfs_uuid->draining || rcu_access_pointer(nfs_uuid->net) != net)
		return -ENXIO;
	if (list_empty(&nfl->list)) {
		rcu_assign_pointer(nfl->nfs_uuid, nfs_uuid);
		list_add_tail(&nfl->list, &nfs_uuid->files);
	} else {
		BUG_ON(rcu_access_pointer(nfl->nfs_uuid) != nfs_uuid);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(nfs_uuid_add_file_locked);

/*
 * The returned file is borrowed from the open-context/layout cache. Caller
 * keeps that cache owner alive and releases admission with nfs_release_local_fh.
 */
struct nfsd_file *nfs_open_local_fh(nfs_uuid_t *uuid,
		   struct rpc_clnt *rpc_clnt, const struct cred *cred,
		   const struct nfs_fh *nfs_fh, struct nfs_file_localio *nfl,
		   struct nfsd_file __rcu **pnf,
		   const fmode_t fmode)
{
	struct net *net;
	struct nfsd_file *localio;

	/*
	 * RCU covers the net observation/admission interval, including rejected
	 * observers during drain. Acquire observes the separately stored session
	 * fields. Admission keeps those fields and NFSD's module alive until put.
	 */
	rcu_read_lock();
	/* Pairs with session publication in nfs_uuid_is_local(). */
	net = unrcu_pointer(smp_load_acquire(&uuid->net));
	if (!net || !percpu_ref_tryget_live(&uuid->io_ref)) {
		rcu_read_unlock();
		return ERR_PTR(-ENXIO);
	}
	if (!nfs_to->nfsd_net_try_get(net)) {
		percpu_ref_put(&uuid->io_ref);
		rcu_read_unlock();
		return ERR_PTR(-ENXIO);
	}
	rcu_read_unlock();
	/* One per-I/O net reference accompanies the borrowed file. */
	localio = nfs_to->nfsd_open_local_fh(net, uuid->dom, rpc_clnt, cred,
					     nfs_fh, uuid, nfl, pnf, fmode);
	if (IS_ERR(localio))
		nfs_release_local_fh(uuid);

	return localio;
}
EXPORT_SYMBOL_GPL(nfs_open_local_fh);

void nfs_release_local_fh(nfs_uuid_t *nfs_uuid)
{
	/* The admitted operation keeps the published session stable until put. */
	nfs_to_nfsd_net_put(rcu_access_pointer(nfs_uuid->net));
	percpu_ref_put(&nfs_uuid->io_ref);
	/* No session or client access is permitted after the admission put. */
}
EXPORT_SYMBOL_GPL(nfs_release_local_fh);

void nfs_close_local_fh(struct nfs_file_localio *nfl)
{
	nfs_uuid_t *nfs_uuid;
	struct file *nowait_file;

	nowait_file = xchg(&nfl->nowait_file, NULL);
	if (nowait_file)
		fput(nowait_file);

	rcu_read_lock();
	nfs_uuid = rcu_dereference(nfl->nfs_uuid);
	if (!nfs_uuid) {
		/* regular (non-LOCALIO) NFS will hammer this */
		rcu_read_unlock();
		return;
	}

	spin_lock(&nfs_uuid->lock);
	if (!rcu_access_pointer(nfl->nfs_uuid)) {
		/* nfs_uuid_put has finished here */
		spin_unlock(&nfs_uuid->lock);
		rcu_read_unlock();
		return;
	}
	if (list_empty(&nfl->list)) {
		/* nfs_uuid_put() has started closing files, wait for it
		 * to finished
		 */
		spin_unlock(&nfs_uuid->lock);
		rcu_read_unlock();
		wait_var_event(&nfl->nfs_uuid,
			       rcu_access_pointer(nfl->nfs_uuid) == NULL);
		return;
	}
	/* tell nfs_uuid_put() to wait for us */
	RCU_INIT_POINTER(nfl->nfs_uuid, NULL);
	spin_unlock(&nfs_uuid->lock);
	rcu_read_unlock();

	nfs_to_nfsd_file_put_local(&nfl->ro_file);
	nfs_to_nfsd_file_put_local(&nfl->rw_file);

	/* Remove nfl from nfs_uuid->files list and signal nfs_uuid_put()
	 * that we are done.  The moment we drop the spinlock the
	 * nfs_uuid could be freed.
	 */
	spin_lock(&nfs_uuid->lock);
	list_del_init(&nfl->list);
	wake_up_var_locked(nfs_uuid, &nfs_uuid->lock);
	spin_unlock(&nfs_uuid->lock);
}
EXPORT_SYMBOL_GPL(nfs_close_local_fh);

/*
 * The NFS LOCALIO code needs to call into NFSD using various symbols,
 * but cannot be statically linked, because that will make the NFS
 * module always depend on the NFSD module.
 *
 * 'nfs_to' provides NFS access to NFSD functions needed for LOCALIO,
 * its lifetime is tightly coupled to the NFSD module and will always
 * be available to NFS LOCALIO because any successful client<->server
 * LOCALIO handshake results in a reference on the NFSD module (above),
 * so NFS implicitly holds a reference to the NFSD module and its
 * functions in the 'nfs_to' nfsd_localio_operations cannot disappear.
 *
 * If the last NFS client using LOCALIO disconnects (and its reference
 * on NFSD dropped) then NFSD could be unloaded, resulting in 'nfs_to'
 * functions being invalid pointers. But if NFSD isn't loaded then NFS
 * will not be able to handshake with NFSD and will have no cause to
 * try to call 'nfs_to' function pointers. If/when NFSD is reloaded it
 * will reinitialize the 'nfs_to' function pointers and make LOCALIO
 * possible.
 */
const struct nfsd_localio_operations *nfs_to;
EXPORT_SYMBOL_GPL(nfs_to);
