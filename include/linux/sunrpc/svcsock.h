/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux/include/linux/sunrpc/svcsock.h
 *
 * RPC server socket I/O.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#ifndef SUNRPC_SVCSOCK_H
#define SUNRPC_SVCSOCK_H

#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>

enum svc_tcp_rx_mode {
	SVC_TCP_RX_EMPTY,
	SVC_TCP_RX_PREFIX,
	SVC_TCP_RX_MIXED,
	SVC_TCP_RX_COPY,
	SVC_TCP_RX_PUBLISHED,
};

enum svc_tcp_rx_action {
	SVC_TCP_RX_CLASSIFY,
	SVC_TCP_RX_MATERIALIZE,
	SVC_TCP_RX_PUBLISH,
	SVC_TCP_RX_ERROR,
	SVC_TCP_RX_RELEASE,
	SVC_TCP_RX_ABORT,
};

enum svc_tcp_rx_reason {
	SVC_TCP_RX_REASON_NONE,
	SVC_TCP_RX_REASON_SHORT_PREFIX,
	SVC_TCP_RX_REASON_DISABLED,
	SVC_TCP_RX_REASON_TLS,
	SVC_TCP_RX_REASON_DIRECTION,
	SVC_TCP_RX_REASON_RPC_VERSION,
	SVC_TCP_RX_REASON_PROGRAM,
	SVC_TCP_RX_REASON_VERSION,
	SVC_TCP_RX_REASON_PROCEDURE,
	SVC_TCP_RX_REASON_AUTH,
	SVC_TCP_RX_REASON_LOCKED_HEAD,
	SVC_TCP_RX_REASON_MANAGED_FRAGS,
	SVC_TCP_RX_REASON_CAPACITY,
	SVC_TCP_RX_REASON_UNREADABLE,
	SVC_TCP_RX_REASON_NET_IOV,
	SVC_TCP_RX_REASON_NULL_PAGE,
	SVC_TCP_RX_REASON_COPY_FAULT,
	SVC_TCP_RX_REASON_INVARIANT,
};

/*
 * Receive descriptors and their page references move together by exchanging
 * this preallocated object between a request and a connected TCP socket.
 */
struct svc_tcp_rx_state {
	struct bio_vec		*bvec;
	unsigned long		*borrowed;
	unsigned long		*page_pool;
	struct svc_rqst		*rqstp;
	u32			capacity;
	u32			count;
	u32			body_bytes;
	u32			fixed_bytes;
	u32			borrowed_bytes;
	u32			copied_bytes;

	u32			materialized_bytes;
	u32			page_pool_bytes;
	u32			refs_acquired;
	u32			refs_released;
	u32			borrowed_count;
	u32			xid;
	u64			record_seq;
	u8			mode;
	u8			reason;
	bool			terminal;
	/* bytes still to copy into the locked-head merge page */
	u32			merge_fill;
} ____cacheline_aligned;

/*
 * RPC server socket.
 */
struct svc_sock {
	struct svc_xprt		sk_xprt;
	struct socket *		sk_sock;	/* berkeley socket layer */
	struct sock *		sk_sk;		/* INET layer */

	/* We keep the old state_change and data_ready CB's here */
	void			(*sk_ostate)(struct sock *);
	void			(*sk_odata)(struct sock *);
	void			(*sk_owspace)(struct sock *);

	/* For sends (protected by xpt_mutex) */
	struct bio_vec		*sk_bvec;

	/* private TCP part */
	/* On-the-wire fragment header: */
	__be32			sk_marker;
	/* As we receive a record, this includes the length received so
	 * far (including the fragment header): */
	u32			sk_tcplen;
	/* Total length of the data (not including fragment headers)
	 * received so far in the fragments making up this rpc: */
	u32			sk_datalen;
	u64			sk_rx_sequence;
	struct svc_tcp_rx_state	*sk_rx;

	struct page_frag_cache  sk_frag_cache;

	struct completion	sk_handshake_done;

	/* received data */
	unsigned long		sk_maxpages;
	struct page *		sk_pages[] __counted_by(sk_maxpages);
};

static inline u32 svc_sock_reclen(struct svc_sock *svsk)
{
	return be32_to_cpu(svsk->sk_marker) & RPC_FRAGMENT_SIZE_MASK;
}

static inline u32 svc_sock_final_rec(struct svc_sock *svsk)
{
	return be32_to_cpu(svsk->sk_marker) & RPC_LAST_STREAM_FRAGMENT;
}

/*
 * Function prototypes.
 */
int		svc_recv(struct svc_rqst *rqstp, long timeo);
void		svc_send(struct svc_rqst *rqstp);
int		svc_addsock(struct svc_serv *serv, struct net *net,
			    const int fd, char *name_return, const size_t len,
			    const struct cred *cred);
void		svc_init_xprt_sock(void);
void		svc_cleanup_xprt_sock(void);
struct svc_tcp_rx_state *svc_tcp_rx_state_alloc(unsigned long capacity,
						gfp_t gfp, int node);
void svc_tcp_rx_state_free(struct svc_tcp_rx_state *state);

/*
 * svc_makesock socket characteristics
 */
#define SVC_SOCK_DEFAULTS	(0U)
#define SVC_SOCK_ANONYMOUS	(1U << 0)	/* don't register with pmap */
#define SVC_SOCK_TEMPORARY	(1U << 1)	/* flag socket as temporary */

#endif /* SUNRPC_SVCSOCK_H */
