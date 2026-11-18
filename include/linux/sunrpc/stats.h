/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux/include/linux/sunrpc/stats.h
 *
 * Client statistics collection for SUN RPC
 *
 * Copyright (C) 1996 Olaf Kirch <okir@monad.swb.de>
 */

#ifndef _LINUX_SUNRPC_STATS_H
#define _LINUX_SUNRPC_STATS_H

#include <linux/proc_fs.h>
#include <linux/percpu.h>

struct rpc_stat {
	const struct rpc_program *program;

	unsigned int		netcnt,
				netudpcnt,
				nettcpcnt,
				nettcpconn,
				netreconn;
	unsigned int		rpccnt,
				rpcretrans,
				rpcauthrefresh,
				rpcgarbage;

	/*
	 * When rpc_stat_alloc_counts() has run, every call is counted per
	 * CPU, so that the CPUs do not pass this struct's line and the
	 * versions' counts between them: rpccnt and rpcauthrefresh in
	 * pcpu, and each version of program in vs_count[version],
	 * allocated at the first client of that version.  Read them with
	 * rpc_stat_rpccnt() and rpc_stat_rpcauthrefresh().
	 */
	struct rpc_stat_pcpu __percpu	*pcpu;
	unsigned int __percpu	**vs_count;
};

struct rpc_stat_pcpu {
	unsigned int		rpccnt;
	unsigned int		rpcauthrefresh;
};

struct svc_stat {
	struct svc_program *	program;

	unsigned int		netcnt,
				netudpcnt,
				nettcpcnt,
				nettcpconn;
	unsigned int		rpccnt,
				rpcbadfmt,
				rpcbadauth,
				rpcbadclnt;

	/* Per-version per-procedure call counts (per-cpu, per-netns) */
	unsigned long __percpu	**vs_count;
	/*
	 * The counts every call raises, per CPU when allocated with
	 * vs_count, so that the CPUs do not pass this struct's line between
	 * them: read them with svc_stat_netcnt() and the like.
	 */
	struct svc_stat_pcpu __percpu *pcpu;
};

struct svc_stat_pcpu {
	unsigned int		netcnt;
	unsigned int		nettcpcnt;
	unsigned int		rpccnt;
};

/* Raise one of the counts in struct svc_stat_pcpu. */
#define svc_stat_inc(statp, field) do {					\
	struct svc_stat *__s = (statp);					\
									\
	if (__s->pcpu)							\
		this_cpu_inc(__s->pcpu->field);				\
	else								\
		data_race(__s->field++);				\
} while (0)

int			rpc_stat_alloc_counts(struct rpc_stat *statp);
void			rpc_stat_free_counts(struct rpc_stat *statp);
unsigned int		rpc_stat_rpccnt(const struct rpc_stat *statp);
unsigned int		rpc_stat_rpcauthrefresh(const struct rpc_stat *statp);

unsigned int		svc_stat_netcnt(const struct svc_stat *statp);
unsigned int		svc_stat_nettcpcnt(const struct svc_stat *statp);
unsigned int		svc_stat_rpccnt(const struct svc_stat *statp);

struct net;
int			svc_stat_alloc_counts(struct svc_stat *statp);
void			svc_stat_free_counts(struct svc_stat *statp);

#ifdef CONFIG_PROC_FS
int			rpc_proc_init(struct net *);
void			rpc_proc_exit(struct net *);
struct proc_dir_entry *	rpc_proc_register(struct net *,struct rpc_stat *);
void			rpc_proc_unregister(struct net *,const char *);
void			rpc_proc_zero(const struct rpc_program *);
struct proc_dir_entry *	svc_proc_register(struct net *, struct svc_stat *,
					  const struct proc_ops *);
void			svc_proc_unregister(struct net *, const char *);

void			svc_seq_show(struct seq_file *,
				     const struct svc_stat *);
#else
static inline int rpc_proc_init(struct net *net)
{
	return 0;
}

static inline void rpc_proc_exit(struct net *net)
{
}
static inline struct proc_dir_entry *rpc_proc_register(struct net *net, struct rpc_stat *s) { return NULL; }
static inline void rpc_proc_unregister(struct net *net, const char *p) {}
static inline void rpc_proc_zero(const struct rpc_program *p) {}

static inline struct proc_dir_entry *svc_proc_register(struct net *net, struct svc_stat *s,
						       const struct proc_ops *proc_ops) { return NULL; }
static inline void svc_proc_unregister(struct net *net, const char *p) {}

static inline void svc_seq_show(struct seq_file *seq,
				const struct svc_stat *st) {}
#endif

#endif /* _LINUX_SUNRPC_STATS_H */
