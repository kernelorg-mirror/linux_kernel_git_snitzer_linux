// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de> */

#include <linux/sched.h>
#include "nfsd.h"
#include "export.h"
#include "auth.h"

int nfsexp_flags(struct svc_cred *cred, struct svc_export *exp)
{
	struct exp_flavor_info *f;
	struct exp_flavor_info *end = exp->ex_flavors + exp->ex_nflavors;

	for (f = exp->ex_flavors; f < end; f++) {
		if (f->pseudoflavor == cred->cr_flavor)
			return f->flags;
	}
	return exp->ex_flags;

}

/*
 * The fsuid and fsgid nfsd_setuser() gives a request, after squashing.
 */
static void nfsd_setuser_ids(struct svc_cred *cred, struct svc_export *exp,
			     int flags, kuid_t *uid, kgid_t *gid)
{
	*uid = cred->cr_uid;
	*gid = cred->cr_gid;
	if (flags & NFSEXP_ALLSQUASH) {
		*uid = exp->ex_anon_uid;
		*gid = exp->ex_anon_gid;
	} else if (flags & NFSEXP_ROOTSQUASH) {
		if (uid_eq(*uid, GLOBAL_ROOT_UID))
			*uid = exp->ex_anon_uid;
		if (gid_eq(*gid, GLOBAL_ROOT_GID))
			*gid = exp->ex_anon_gid;
	}
	if (uid_eq(*uid, INVALID_UID))
		*uid = exp->ex_anon_uid;
	if (gid_eq(*gid, INVALID_GID))
		*gid = exp->ex_anon_gid;
}

/*
 * Whether @old, which an earlier call built from the same real cred, is
 * what this call would build: the same fsuid and fsgid, and the same
 * groups -- none when all are squashed, else the request's own, taken
 * only when no root group is to be squashed in them.  The capabilities
 * follow from the fsuid.
 */
static bool nfsd_setuser_same(const struct cred *old, struct svc_cred *cred,
			      int flags, kuid_t uid, kgid_t gid)
{
	const struct group_info *gi = old->group_info;
	const struct group_info *rqgi = cred->cr_group_info;
	int i;

	if (!uid_eq(old->fsuid, uid) || !gid_eq(old->fsgid, gid))
		return false;
	if (flags & NFSEXP_ALLSQUASH)
		return gi->ngroups == 0;
	if (gi->ngroups != rqgi->ngroups)
		return false;
	for (i = 0; i < rqgi->ngroups; i++) {
		if (!gid_eq(gi->gid[i], rqgi->gid[i]))
			return false;
		if ((flags & NFSEXP_ROOTSQUASH) &&
		    gid_eq(rqgi->gid[i], GLOBAL_ROOT_GID))
			return false;
	}
	return true;
}

/**
 * nfsd_setuser - Act as the NFS client's user
 * @rqstp: the request, or NULL for a LOCALIO caller
 * @cred: the user's credential from the request
 * @exp: the export, for its squashing
 *
 * An nfsd thread keeps the cred it last built: consecutive requests
 * usually come from one user, and installing that cred again saves
 * building another for each.  A LOCALIO caller runs in its own task and
 * keeps nothing.
 *
 * Returns 0, or -ENOMEM.
 */
int nfsd_setuser(struct svc_rqst *rqstp, struct svc_cred *cred,
		 struct svc_export *exp)
{
	struct nfsd_thread_local_info *ntli = rqstp ? rqstp->rq_private : NULL;
	struct group_info *rqgi;
	struct group_info *gi;
	struct cred *new;
	kuid_t uid;
	kgid_t gid;
	int i;
	int flags = nfsexp_flags(cred, exp);

	nfsd_setuser_ids(cred, exp, flags, &uid, &gid);
	if (ntli && ntli->ntli_user_cred &&
	    nfsd_setuser_same(ntli->ntli_user_cred, cred, flags, uid, gid)) {
		if (current_cred() != ntli->ntli_user_cred)
			put_cred(override_creds(get_cred(ntli->ntli_user_cred)));
		return 0;
	}

	/* discard any old override before preparing the new set */
	put_cred(revert_creds(get_cred(current_real_cred())));
	new = prepare_creds();
	if (!new)
		return -ENOMEM;

	new->fsuid = uid;
	new->fsgid = gid;

	rqgi = cred->cr_group_info;

	if (flags & NFSEXP_ALLSQUASH) {
		gi = groups_alloc(0);
		if (!gi)
			goto oom;
	} else if (flags & NFSEXP_ROOTSQUASH) {
		gi = groups_alloc(rqgi->ngroups);
		if (!gi)
			goto oom;

		for (i = 0; i < rqgi->ngroups; i++) {
			if (gid_eq(GLOBAL_ROOT_GID, rqgi->gid[i]))
				gi->gid[i] = exp->ex_anon_gid;
			else
				gi->gid[i] = rqgi->gid[i];
		}

		/* Each thread allocates its own gi, no race */
		groups_sort(gi);
	} else {
		gi = get_group_info(rqgi);
	}

	set_groups(new, gi);
	put_group_info(gi);

	if (!uid_eq(new->fsuid, GLOBAL_ROOT_UID))
		new->cap_effective = cap_drop_nfsd_set(new->cap_effective);
	else
		new->cap_effective = cap_raise_nfsd_set(new->cap_effective,
							new->cap_permitted);
	if (ntli) {
		put_cred(ntli->ntli_user_cred);
		ntli->ntli_user_cred = get_cred(new);
	}
	put_cred(override_creds(new));
	return 0;

oom:
	abort_creds(new);
	return -ENOMEM;
}

/**
 * nfsd_user_namespace - Get user_namespace in effect for an RPC request
 * @rqstp: RPC execution context
 *
 * xpt_cred is set once at transport creation and never modified. The
 * transport itself is reference-counted during request processing, so
 * no explicit reference on the namespace is necessary.
 *
 * Return: the user_namespace from the transport credential, or
 * init_user_ns if no credential was set. The returned namespace pointer
 * is valid for the duration of the RPC request.
 */
struct user_namespace *nfsd_user_namespace(const struct svc_rqst *rqstp)
{
	const struct cred *cred = rqstp->rq_xprt->xpt_cred;

	return cred ? cred->user_ns : &init_user_ns;
}
