// SPDX-License-Identifier: GPL-2.0-only
/*
 * NFS server support for local clients to bypass network stack
 *
 * Copyright (C) 2014 Weston Andros Adamson <dros@primarydata.com>
 * Copyright (C) 2019 Trond Myklebust <trond.myklebust@hammerspace.com>
 * Copyright (C) 2024 Mike Snitzer <snitzer@hammerspace.com>
 */

#include <linux/exportfs.h>
#include <linux/sunrpc/svcauth_gss.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs.h>
#include <linux/string.h>

#include "nfsd.h"
#include "vfs.h"
#include "netns.h"
#include "filecache.h"

/*
 * We need to translate between nfs status return values and
 * the local errno values which may not be the same.
 * - duplicated from fs/nfs/nfs2xdr.c to avoid needless bloat of
 *   all compiled nfs objects if it were in include/linux/nfs.h
 */
static const struct {
	int stat;
	int errno;
} nfs_common_errtbl[] = {
	{ NFS_OK,		0		},
	{ NFSERR_PERM,		-EPERM		},
	{ NFSERR_NOENT,		-ENOENT		},
	{ NFSERR_IO,		-EIO		},
	{ NFSERR_NXIO,		-ENXIO		},
/*	{ NFSERR_EAGAIN,	-EAGAIN		}, */
	{ NFSERR_ACCES,		-EACCES		},
	{ NFSERR_EXIST,		-EEXIST		},
	{ NFSERR_XDEV,		-EXDEV		},
	{ NFSERR_NODEV,		-ENODEV		},
	{ NFSERR_NOTDIR,	-ENOTDIR	},
	{ NFSERR_ISDIR,		-EISDIR		},
	{ NFSERR_INVAL,		-EINVAL		},
	{ NFSERR_FBIG,		-EFBIG		},
	{ NFSERR_NOSPC,		-ENOSPC		},
	{ NFSERR_ROFS,		-EROFS		},
	{ NFSERR_MLINK,		-EMLINK		},
	{ NFSERR_NAMETOOLONG,	-ENAMETOOLONG	},
	{ NFSERR_NOTEMPTY,	-ENOTEMPTY	},
	{ NFSERR_DQUOT,		-EDQUOT		},
	{ NFSERR_STALE,		-ESTALE		},
	{ NFSERR_REMOTE,	-EREMOTE	},
#ifdef EWFLUSH
	{ NFSERR_WFLUSH,	-EWFLUSH	},
#endif
	{ NFSERR_BADHANDLE,	-EBADHANDLE	},
	{ NFSERR_NOT_SYNC,	-ENOTSYNC	},
	{ NFSERR_BAD_COOKIE,	-EBADCOOKIE	},
	{ NFSERR_NOTSUPP,	-ENOTSUPP	},
	{ NFSERR_TOOSMALL,	-ETOOSMALL	},
	{ NFSERR_SERVERFAULT,	-EREMOTEIO	},
	{ NFSERR_BADTYPE,	-EBADTYPE	},
	{ NFSERR_JUKEBOX,	-EJUKEBOX	},
	{ -1,			-EIO		}
};

/**
 * nfs_stat_to_errno - convert an NFS status code to a local errno
 * @status: NFS status code to convert
 *
 * Returns a local errno value, or -EIO if the NFS status code is
 * not recognized.  nfsd_file_acquire() returns an nfsstat that
 * needs to be translated to an errno before being returned to a
 * local client application.
 */
static int nfs_stat_to_errno(enum nfs_stat status)
{
	int i;

	for (i = 0; nfs_common_errtbl[i].stat != -1; i++) {
		if (nfs_common_errtbl[i].stat == (int)status)
			return nfs_common_errtbl[i].errno;
	}
	return nfs_common_errtbl[i].errno;
}

static void
nfsd_local_fakerqst_destroy(struct svc_rqst *rqstp)
{
	if (rqstp->rq_client)
		auth_domain_put(rqstp->rq_client);
	if (rqstp->rq_cred.cr_group_info)
		put_group_info(rqstp->rq_cred.cr_group_info);
	/* rpcauth_map_to_svc_cred_local() clears cr_principal */
	WARN_ON_ONCE(rqstp->rq_cred.cr_principal != NULL);
	kfree(rqstp->rq_xprt);
	kfree(rqstp);
}

static struct svc_rqst *
nfsd_local_fakerqst_create(struct net *net, struct rpc_clnt *rpc_clnt,
			const struct cred *cred, struct svc_serv *serv)
{
	struct svc_rqst *rqstp;
	int status;

	rqstp = kzalloc(sizeof(*rqstp), GFP_KERNEL);
	if (!rqstp)
		return ERR_PTR(-ENOMEM);

	rqstp->rq_xprt = kzalloc(sizeof(*rqstp->rq_xprt), GFP_KERNEL);
	if (!rqstp->rq_xprt) {
		status = -ENOMEM;
		goto out_err;
	}
	rqstp->rq_xprt->xpt_net = net;

	__set_bit(RQ_SECURE, &rqstp->rq_flags);
	rqstp->rq_server = serv;
	/*
	 * These constants aren't actively used in this fake svc_rqst,
	 * which bypasses SUNRPC, but they must pass negative checks.
	 */
	rqstp->rq_proc = 1;
	rqstp->rq_vers = 3;
	rqstp->rq_prot = IPPROTO_TCP;

	/* Note: we're connecting to ourself, so source addr == peer addr */
	rqstp->rq_addrlen = rpc_peeraddr(rpc_clnt,
			(struct sockaddr *)&rqstp->rq_addr,
			sizeof(rqstp->rq_addr));

	rpcauth_map_to_svc_cred_local(rpc_clnt->cl_auth, cred, &rqstp->rq_cred);

	/*
	 * set up enough for svcauth_unix_set_client to be able to wait
	 * for the cache downcall. Note that we do _not_ want to allow the
	 * request to be deferred for later revisit since this rqst and xprt
	 * are not set up to run inside of the normal svc_rqst engine.
	 */
	INIT_LIST_HEAD(&rqstp->rq_xprt->xpt_deferred);
	kref_init(&rqstp->rq_xprt->xpt_ref);
	spin_lock_init(&rqstp->rq_xprt->xpt_lock);
	rqstp->rq_chandle.thread_wait = 5 * HZ;

	status = svcauth_unix_set_client(rqstp);
	switch (status) {
	case SVC_OK:
		break;
	case SVC_DENIED:
		status = -ENXIO;
		goto out_err;
	default:
		status = -ETIMEDOUT;
		goto out_err;
	}

	return rqstp;

out_err:
	nfsd_local_fakerqst_destroy(rqstp);
	return ERR_PTR(status);
}

/**
 * nfsd_open_local_fh - lookup a local filehandle @nfs_fh and map to @file
 *
 * @cl_nfssvc_net: the 'struct net' to use to get the proper nfsd_net
 * @rpc_clnt: rpc_clnt that the client established, used for sockaddr and cred
 * @cred: cred that the client established
 * @nfs_fh: filehandle to lookup
 * @fmode: fmode_t to use for open
 * @pfilp: returned file pointer that maps to @nfs_fh
 *
 * This function maps a local fh to a path on a local filesystem.
 * This is useful when the nfs client has the local server mounted - it can
 * avoid all the NFS overhead with reads, writes and commits.
 *
 * On successful return, caller is responsible for calling path_put. Also
 * note that this is called from nfs.ko via find_symbol() to avoid an explicit
 * dependency on knfsd. So, there is no forward declaration in a header file
 * for it that is shared with the client.
 */
int nfsd_open_local_fh(struct net *cl_nfssvc_net,
			 struct rpc_clnt *rpc_clnt,
			 const struct cred *cred,
			 const struct nfs_fh *nfs_fh,
			 const fmode_t fmode,
			 struct file **pfilp)
{
	int mayflags = NFSD_MAY_LOCALIO;
	int status = 0;
	struct nfsd_net *nn;
	const struct cred *save_cred;
	struct svc_rqst *rqstp;
	struct svc_fh fh;
	struct nfsd_file *nf;
	struct svc_serv *serv;
	__be32 beres;

	if (nfs_fh->size > NFS4_FHSIZE)
		return -EINVAL;

	/* Not running in nfsd context, must safely get reference on nfsd_serv */
	cl_nfssvc_net = maybe_get_net(cl_nfssvc_net);
	if (!cl_nfssvc_net)
		return -ENXIO;
	nn = net_generic(cl_nfssvc_net, nfsd_net_id);

	serv = READ_ONCE(nn->nfsd_serv);
	if (unlikely(!serv)) {
		status = -ENXIO;
		goto out_net;
	}

	/* Save creds before calling into nfsd */
	save_cred = get_current_cred();

	rqstp = nfsd_local_fakerqst_create(cl_nfssvc_net, rpc_clnt, cred, serv);
	if (IS_ERR(rqstp)) {
		status = PTR_ERR(rqstp);
		goto out_revertcred;
	}

	/* nfs_fh -> svc_fh */
	fh_init(&fh, NFS4_FHSIZE);
	fh.fh_handle.fh_size = nfs_fh->size;
	memcpy(fh.fh_handle.fh_raw, nfs_fh->data, nfs_fh->size);

	if (fmode & FMODE_READ)
		mayflags |= NFSD_MAY_READ;
	if (fmode & FMODE_WRITE)
		mayflags |= NFSD_MAY_WRITE;

	beres = nfsd_file_acquire(rqstp, &fh, mayflags, &nf);
	if (beres) {
		status = nfs_stat_to_errno(be32_to_cpu(beres));
		goto out_fh_put;
	}
	*pfilp = get_file(nf->nf_file);
	nfsd_file_put(nf);
out_fh_put:
	fh_put(&fh);
	nfsd_local_fakerqst_destroy(rqstp);
out_revertcred:
	revert_creds(save_cred);
out_net:
	put_net(cl_nfssvc_net);
	return status;
}
EXPORT_SYMBOL_GPL(nfsd_open_local_fh);

/* Compile time type checking, not used by anything */
static nfs_to_nfsd_open_t __maybe_unused nfsd_open_local_fh_typecheck = nfsd_open_local_fh;
