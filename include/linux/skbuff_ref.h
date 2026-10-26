/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *	Skb ref helpers.
 *
 */

#ifndef _LINUX_SKBUFF_REF_H
#define _LINUX_SKBUFF_REF_H

#include <linux/skbuff.h>

/**
 * __skb_frag_ref - take an addition reference on a paged fragment.
 * @frag: the paged fragment
 *
 * Takes an additional reference on the paged fragment @frag.
 */
static __always_inline void __skb_frag_ref(skb_frag_t *frag)
{
	get_netmem(skb_frag_netmem(frag));
}

/**
 * skb_frag_ref - take an addition reference on a paged fragment of an skb.
 * @skb: the buffer
 * @f: the fragment offset.
 *
 * Takes an additional reference on the @f'th paged fragment of @skb.
 */
static __always_inline void skb_frag_ref(struct sk_buff *skb, int f)
{
	__skb_frag_ref(&skb_shinfo(skb)->frags[f]);
}

bool napi_pp_put_page(netmem_ref netmem);

static __always_inline void skb_page_unref(netmem_ref netmem, bool recycle)
{
#ifdef CONFIG_PAGE_POOL
	if (recycle && napi_pp_put_page(netmem))
		return;
#endif
	put_netmem(netmem);
}

/**
 * __skb_frag_unref - release a reference on a paged fragment.
 * @frag: the paged fragment
 * @recycle: recycle the page if allocated via page_pool
 *
 * Releases a reference on the paged fragment @frag
 * or recycles the page via the page_pool API.
 */
static __always_inline void __skb_frag_unref(skb_frag_t *frag, bool recycle)
{
	skb_page_unref(skb_frag_netmem(frag), recycle);
}

/**
 * skb_frag_unref - release a reference on a paged fragment of an skb.
 * @skb: the buffer
 * @f: the fragment offset
 *
 * Releases a reference on the @f'th paged fragment of @skb.
 */
static __always_inline void skb_frag_unref(struct sk_buff *skb, int f)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);

	if (!skb_zcopy_managed(skb))
		__skb_frag_unref(&shinfo->frags[f], skb->pp_recycle);
}

/**
 * skb_frags_hand_over - hand the last paged fragments of an skb to the caller
 * @skb: the buffer, which its owner frees next
 * @first: the offset of the first fragment handed over
 *
 * Fragments @first to the last of @skb leave it with their references: the
 * caller now holds each as @skb held it (a page_pool reference if
 * @skb->pp_recycle is set) and releases it itself, and freeing @skb no
 * longer does.  Nothing else in @skb changes, so @skb->len still counts the
 * fragments' bytes: it may only be freed, as ->read_sock frees an skb once
 * its actor has read it to its end.  @skb must hold its fragments' references
 * alone: not cloned, and its fragments not managed.
 */
static inline void skb_frags_hand_over(struct sk_buff *skb, unsigned int first)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);

	DEBUG_NET_WARN_ON_ONCE(skb_cloned(skb) || skb_zcopy_managed(skb) ||
			       first > shinfo->nr_frags);
	shinfo->nr_frags = first;
}

#endif	/* _LINUX_SKBUFF_REF_H */
