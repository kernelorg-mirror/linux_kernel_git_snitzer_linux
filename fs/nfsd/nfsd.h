/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hodge-podge collection of knfsd-related stuff.
 * I will sort this out later.
 *
 * Copyright (C) 1995-1997 Olaf Kirch <okir@monad.swb.de>
 */

#ifndef LINUX_NFSD_NFSD_H
#define LINUX_NFSD_NFSD_H

#include <linux/types.h>
#include <linux/mount.h>

#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>

#include <uapi/linux/nfsd/debug.h>
#include <uapi/linux/nfsd_netlink.h>

#undef ifdebug
#ifdef CONFIG_SUNRPC_DEBUG
# define ifdebug(flag)		if (nfsd_debug & NFSDDBG_##flag)
#else
# define ifdebug(flag)		if (0)
#endif

/*
 * nfsd version
 */
#define NFSD_MINVERS			2
#define	NFSD_MAXVERS			4
#define NFSD_SUPPORTED_MINOR_VERSION	2
bool nfsd_support_version(int vers);

/*
 * Default and maximum payload size (NFS READ or WRITE), in bytes.
 * The maximum is an implementation limit.
 */
enum {
	NFSSVC_DEFBLKSIZE       = 4 * 1024 * 1024,
	NFSSVC_MAXBLKSIZE       = RPCSVC_MAXPAYLOAD,
};

/* Maximum number of operations per session compound */
#define NFSD_MAX_OPS_PER_COMPOUND	200

extern struct svc_program	nfsd_programs[];
extern const struct svc_version	nfsd_version2, nfsd_version3, nfsd_version4;
#if IS_ENABLED(CONFIG_KUNIT)
const struct svc_procedure *nfsd3_procedure(unsigned int procedure);
#if IS_ENABLED(CONFIG_NFSD_V4)
const struct svc_procedure *nfsd4_procedure(unsigned int procedure);
#endif
#endif
extern struct mutex		nfsd_global_mutex;
extern atomic_t			nfsd_th_cnt;		/* number of available threads */

extern const struct seq_operations nfs_exports_op;

/*
 * One cached export: a key and the slot's own reference on the
 * svc_expkey it found and on the svc_export that resolves to.
 */
struct nfsd_exp_slot_ent {
	u64			used;		/* the slot's clock at last use */
	struct net		*net;
	struct auth_domain	*client;	/* compared only; ek pins it */
	struct cache_detail	*ekcd;
	struct svc_expkey	*ek;
	struct svc_export	*exp;
	u32			fsid[6];
	u8			fsid_type;
};

/* The most words of a file id a slot keeps; a longer one is not lent. */
#define NFSD_FH_SLOT_WORDS	6

/*
 * One decoded file handle: the dentry that file id @fid of type
 * @fileid_type resolved to under @exp, one of the slot's exports, and
 * the slot's own reference on it.  Only regular files are kept.
 */
struct nfsd_fh_slot_ent {
	u64			used;		/* the slot's clock at last use */
	time64_t		last;		/* seconds_since_boot() then */
	struct svc_export	*exp;		/* compared only; an entry pins it */
	struct dentry		*dentry;
	u32			fid[NFSD_FH_SLOT_WORDS];
	u8			fileid_type;
	u8			len;		/* words of @fid */
};

/*
 * The exports an nfsd thread has cached, for NFSv3 requests from an
 * AUTH_SYS or AUTH_NULL client: up to @nr entries, the least recently
 * used replaced on a miss.  A request whose key matches one borrows that
 * export for the life of its svc_fh instead of looking it up and taking
 * and dropping shared references.
 *
 * @lock guards @borrowed and @dead.  While @borrowed is set only its
 * holder (the owner thread, or a drain that claimed an idle slot) reads
 * or changes the entries.  See export.c.
 */
struct nfsd_exp_slot {
	spinlock_t		lock;		/* guards borrowed, dead */
	bool			borrowed;	/* the entries are claimed */
	bool			dead;		/* drained while borrowed */
	bool			enabled;	/* a thread's or rtc rqst's slot */
	bool			registered;	/* on the drain list (owner) */
	unsigned int		nr;		/* entries in @ent */
	unsigned int		mru;		/* the entry hit or filled last */
	u64			clock;		/* bumped on every hit and fill */
	struct nfsd_exp_slot_ent *ent;
	unsigned int		nfh;		/* entries in @fh */
	unsigned int		fh_mru;		/* the file handle hit or filled last */
	struct nfsd_fh_slot_ent	*fh;		/* decoded handles under @ent's exports */
	struct list_head	link;
};

struct nfsd_thread_local_info {
	struct nfs4_client	**ntli_lease_breaker;
	int			ntli_cachetype;
	/* the last cred nfsd_setuser() built, to install again */
	const struct cred	*ntli_user_cred;
	/* the decode's raised copy of ntli_fh_base (nfsd_set_fh_dentry()) */
	const struct cred	*ntli_fh_base;
	const struct cred	*ntli_fh_cred;
	struct nfsd_exp_slot	ntli_exp_slot;
};

/*
 * Common void argument and result helpers
 */
struct nfsd_voidargs { };
struct nfsd_voidres { };
bool		nfssvc_decode_voidarg(struct svc_rqst *rqstp,
				      struct xdr_stream *xdr);
bool		nfssvc_encode_voidres(struct svc_rqst *rqstp,
				      struct xdr_stream *xdr);

/*
 * Function prototypes.
 */
int		nfsd_svc(int n, int *nservers, struct net *net,
			 const struct cred *cred, const char *scope);
int		nfsd_dispatch(struct svc_rqst *rqstp);

int		nfsd_nrthreads(struct net *);
int		nfsd_nrpools(struct net *);
int		nfsd_get_nrthreads(int n, int *, struct net *);
int		nfsd_set_nrthreads(int n, int *, struct net *);
void		nfsd_shutdown_threads(struct net *net);

struct svc_rqst *nfsd_current_rqst(void);

struct nfsdfs_client {
	struct kref cl_ref;
	void (*cl_release)(struct kref *kref);
};

struct nfsd_net;

struct nfsdfs_client *get_nfsdfs_client(struct inode *);
struct dentry *nfsd_client_mkdir(struct nfsd_net *nn,
				 struct nfsdfs_client *ncl, u32 id,
				 const struct tree_descr *,
				 struct dentry **fdentries);
void nfsd_client_rmdir(struct dentry *dentry);
int nfsd_cache_notify(struct cache_detail *cd, struct cache_head *h, u32 cache_type);

#if defined(CONFIG_NFSD_V2_ACL) || defined(CONFIG_NFSD_V3_ACL)
#ifdef CONFIG_NFSD_V2_ACL
extern const struct svc_version nfsd_acl_version2;
#else
#define nfsd_acl_version2 NULL
#endif
#ifdef CONFIG_NFSD_V3_ACL
extern const struct svc_version nfsd_acl_version3;
#else
#define nfsd_acl_version3 NULL
#endif
#endif

#if IS_ENABLED(CONFIG_NFS_LOCALIO)
extern const struct svc_version localio_version1;
#endif

enum vers_op {NFSD_SET, NFSD_CLEAR, NFSD_TEST, NFSD_AVAIL };
int nfsd_vers(struct nfsd_net *nn, int vers, enum vers_op change);
bool nfsd_version_registerable(struct net *net,
			       const struct svc_program *progp, u32 version);
int nfsd_minorversion(struct nfsd_net *nn, u32 minorversion, enum vers_op change);
void nfsd_reset_versions(struct nfsd_net *nn);
int nfsd_create_serv(struct net *net, bool no_rpcbind);
void nfsd_destroy_serv(struct net *net);

#ifdef CONFIG_DEBUG_FS
void nfsd_debugfs_init(void);
void nfsd_debugfs_exit(void);
#else
static inline void nfsd_debugfs_init(void) {}
static inline void nfsd_debugfs_exit(void) {}
#endif

extern bool nfsd_disable_splice_read __read_mostly;
extern bool nfsd_delegts_enabled __read_mostly;

enum {
	/* Values are uAPI: per-export modes are sent over netlink */
	NFSD_IO_BUFFERED	= NFSD_IO_MODE_BUFFERED,
	NFSD_IO_DONTCACHE	= NFSD_IO_MODE_DONTCACHE,
	NFSD_IO_DIRECT		= NFSD_IO_MODE_DIRECT,
	/*
	 * Kernel-internal only: a per-export mode of NFSD_IO_DEFAULT
	 * means "follow the global nfsd_io_cache_read/write knob".
	 * It is never sent across the kernel-userspace interface.
	 * It is -1 so that it stays distinct from all present and
	 * future mode values.
	 */
	NFSD_IO_DEFAULT = -1,
};

extern u64 nfsd_io_cache_read __read_mostly;
extern u64 nfsd_io_cache_write __read_mostly;
extern u32 nfsd_direct_misaligned_min_bytes __read_mostly;
extern bool nfsd_direct_misaligned_dontcache __read_mostly;
extern bool nfsd_rtc_read_on __read_mostly;
extern bool nfsd_rtc_write_on __read_mostly;

bool nfsd_v4client(struct svc_rqst *rqstp);

/*
 * lockd binding
 */
void		nfsd_lockd_init(void);
void		nfsd_lockd_shutdown(void);
void		nfsd3_rx_target_init(void);
void		nfsd3_rx_target_exit(void);


#endif /* LINUX_NFSD_NFSD_H */
