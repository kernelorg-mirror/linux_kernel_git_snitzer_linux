/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_SUNRPC_SVC_REPLY_PAGES_H
#define _NET_SUNRPC_SVC_REPLY_PAGES_H

#include <linux/skbuff.h>

struct svc_rqst;

/*
 * A TCP reply's pages, sent zero copy and kept for the sending CPU's next
 * fills (svc_reply_pages.c).
 */
struct svc_reply_tx {
	struct ubuf_info	ubuf;	/* held by TCP's skbs and the sender */
	union {
		struct llist_node	node;	/* on a cache's ready list */
		struct svc_reply_tx	*next;	/* on its spare stack */
	};
	unsigned int		cpu;	/* the sending CPU, whose cache it is */
	unsigned int		size;	/* room in pages[] */
	unsigned int		n;	/* pages held */
	bool			copied;	/* TCP copied them (not zero copy) */
	/*
	 * Replies that share skbs (svc_reply_tx_link_skb()): each one linked
	 * behind the chain's head holds a reference on the head, and the head
	 * releases them all.  Changed only under the socket lock.
	 */
	struct svc_reply_tx	*chain_head;	/* NULL: this one is a head */
	struct svc_reply_tx	*chain_next;	/* the head's members */
	unsigned int		chain_nr;	/* a head's members */
	struct page		*pages[];
};

void svc_reply_pages_fill(struct page **pages, unsigned long npages);
struct svc_reply_tx *svc_reply_tx_take(struct svc_rqst *rqstp,
				       const struct bio_vec *bvec,
				       unsigned int nvecs);
int svc_reply_pages_init(void);
void svc_reply_pages_exit(void);

#if IS_ENABLED(CONFIG_KUNIT)
struct svc_reply_kunit_stats {
	unsigned long	sends;
	unsigned long	reused;
	unsigned long	busy;
	unsigned long	over;
	long		live;
};

unsigned int svc_reply_kunit_set(unsigned int pages);
unsigned int svc_reply_kunit_cached(int cpu);
void svc_reply_kunit_stats(struct svc_reply_kunit_stats *k);
#endif

#endif /* _NET_SUNRPC_SVC_REPLY_PAGES_H */
