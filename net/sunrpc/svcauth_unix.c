// SPDX-License-Identifier: GPL-2.0-only
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/sunrpc/types.h>
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/svcauth.h>
#include <kunit/visibility.h>
#include <linux/sunrpc/gss_api.h>
#include <linux/sunrpc/addr.h>
#include <linux/err.h>
#include <linux/seq_file.h>
#include <linux/hash.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <net/sock.h>
#include <net/ipv6.h>
#include <linux/kernel.h>
#include <linux/user_namespace.h>
#include <net/genetlink.h>
#include <uapi/linux/sunrpc_netlink.h>
#include <trace/events/sunrpc.h>

#define RPCDBG_FACILITY	RPCDBG_AUTH

#include "netns.h"
#include "netlink.h"

/*
 * AUTHUNIX and AUTHNULL credentials are both handled here.
 * AUTHNULL is treated just like AUTHUNIX except that the uid/gid
 * are always nobody (-2).  i.e. we do the same IP address checks for
 * AUTHNULL as for AUTHUNIX, and that is done here.
 */


struct unix_domain {
	struct auth_domain	h;
	/* other stuff later */
};

extern struct auth_ops svcauth_null;
extern struct auth_ops svcauth_unix;
extern struct auth_ops svcauth_tls;

static void svcauth_unix_domain_release_rcu(struct rcu_head *head)
{
	struct auth_domain *dom = container_of(head, struct auth_domain, rcu_head);
	struct unix_domain *ud = container_of(dom, struct unix_domain, h);

	kfree(dom->name);
	kfree(ud);
}

static void svcauth_unix_domain_release(struct auth_domain *dom)
{
	call_rcu(&dom->rcu_head, svcauth_unix_domain_release_rcu);
}

struct auth_domain *unix_domain_find(char *name)
{
	struct auth_domain *rv;
	struct unix_domain *new = NULL;

	rv = auth_domain_find(name);
	while(1) {
		if (rv) {
			if (new && rv != &new->h)
				svcauth_unix_domain_release(&new->h);

			if (rv->flavour != &svcauth_unix) {
				auth_domain_put(rv);
				return NULL;
			}
			return rv;
		}

		new = kmalloc_obj(*new);
		if (new == NULL)
			return NULL;
		kref_init(&new->h.ref);
		new->h.name = kstrdup(name, GFP_KERNEL);
		if (new->h.name == NULL) {
			kfree(new);
			return NULL;
		}
		new->h.flavour = &svcauth_unix;
		rv = auth_domain_lookup(name, &new->h);
	}
}
EXPORT_SYMBOL_GPL(unix_domain_find);


/**************************************************
 * cache for IP address to unix_domain
 * as needed by AUTH_UNIX
 */
#define	IP_HASHBITS	8
#define	IP_HASHMAX	(1<<IP_HASHBITS)

struct ip_map {
	struct cache_head	h;
	char			m_class[8]; /* e.g. "nfsd" */
	struct in6_addr		m_addr;
	struct unix_domain	*m_client;
	struct rcu_head		m_rcu;
};

static void ip_map_put(struct kref *kref)
{
	struct cache_head *item = container_of(kref, struct cache_head, ref);
	struct ip_map *im = container_of(item, struct ip_map,h);

	if (test_bit(CACHE_VALID, &item->flags) &&
	    !test_bit(CACHE_NEGATIVE, &item->flags))
		auth_domain_put(&im->m_client->h);
	kfree_rcu(im, m_rcu);
}

static inline int hash_ip6(const struct in6_addr *ip)
{
	return hash_32(ipv6_addr_hash(ip), IP_HASHBITS);
}
static int ip_map_match(struct cache_head *corig, struct cache_head *cnew)
{
	struct ip_map *orig = container_of(corig, struct ip_map, h);
	struct ip_map *new = container_of(cnew, struct ip_map, h);
	return strcmp(orig->m_class, new->m_class) == 0 &&
	       ipv6_addr_equal(&orig->m_addr, &new->m_addr);
}
static void ip_map_init(struct cache_head *cnew, struct cache_head *citem)
{
	struct ip_map *new = container_of(cnew, struct ip_map, h);
	struct ip_map *item = container_of(citem, struct ip_map, h);

	strscpy(new->m_class, item->m_class);
	new->m_addr = item->m_addr;
}
static void update(struct cache_head *cnew, struct cache_head *citem)
{
	struct ip_map *new = container_of(cnew, struct ip_map, h);
	struct ip_map *item = container_of(citem, struct ip_map, h);

	kref_get(&item->m_client->h.ref);
	new->m_client = item->m_client;
}
static struct cache_head *ip_map_alloc(void)
{
	struct ip_map *i = kmalloc_obj(*i);
	if (i)
		return &i->h;
	else
		return NULL;
}

static int ip_map_upcall(struct cache_detail *cd, struct cache_head *h)
{
	return sunrpc_cache_upcall(cd, h);
}

static void ip_map_request(struct cache_detail *cd,
				  struct cache_head *h,
				  char **bpp, int *blen)
{
	char text_addr[40];
	struct ip_map *im = container_of(h, struct ip_map, h);

	if (ipv6_addr_v4mapped(&(im->m_addr))) {
		snprintf(text_addr, 20, "%pI4", &im->m_addr.s6_addr32[3]);
	} else {
		snprintf(text_addr, 40, "%pI6", &im->m_addr);
	}
	qword_add(bpp, blen, im->m_class);
	qword_add(bpp, blen, text_addr);
	(*bpp)[-1] = '\n';
}

static struct ip_map *__ip_map_lookup(struct cache_detail *cd, char *class, struct in6_addr *addr);
static int __ip_map_update(struct cache_detail *cd, struct ip_map *ipm, struct unix_domain *udom, time64_t expiry);

static int ip_map_parse(struct cache_detail *cd,
			  char *mesg, int mlen)
{
	/* class ipaddress [domainname] */
	/* should be safe just to use the start of the input buffer
	 * for scratch: */
	char *buf = mesg;
	int len;
	char class[8];
	union {
		struct sockaddr		sa;
		struct sockaddr_in	s4;
		struct sockaddr_in6	s6;
	} address;
	struct sockaddr_in6 sin6;
	int err;

	struct ip_map *ipmp;
	struct auth_domain *dom;
	time64_t expiry;

	if (mesg[mlen-1] != '\n')
		return -EINVAL;
	mesg[mlen-1] = 0;

	/* class */
	len = qword_get(&mesg, class, sizeof(class));
	if (len <= 0) return -EINVAL;

	/* ip address */
	len = qword_get(&mesg, buf, mlen);
	if (len <= 0) return -EINVAL;

	if (rpc_pton(cd->net, buf, len, &address.sa, sizeof(address)) == 0)
		return -EINVAL;
	switch (address.sa.sa_family) {
	case AF_INET:
		/* Form a mapped IPv4 address in sin6 */
		sin6.sin6_family = AF_INET6;
		ipv6_addr_set_v4mapped(address.s4.sin_addr.s_addr,
				&sin6.sin6_addr);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		memcpy(&sin6, &address.s6, sizeof(sin6));
		break;
#endif
	default:
		return -EINVAL;
	}

	err = get_expiry(&mesg, &expiry);
	if (err)
		return err;

	/* domainname, or empty for NEGATIVE */
	len = qword_get(&mesg, buf, mlen);
	if (len < 0) return -EINVAL;

	if (len) {
		dom = unix_domain_find(buf);
		if (dom == NULL)
			return -ENOENT;
	} else
		dom = NULL;

	/* IPv6 scope IDs are ignored for now */
	ipmp = __ip_map_lookup(cd, class, &sin6.sin6_addr);
	if (ipmp) {
		err = __ip_map_update(cd, ipmp,
			     container_of(dom, struct unix_domain, h),
			     expiry);
	} else
		err = -ENOMEM;

	if (dom)
		auth_domain_put(dom);

	cache_flush();
	return err;
}

static int ip_map_show(struct seq_file *m,
		       struct cache_detail *cd,
		       struct cache_head *h)
{
	struct ip_map *im;
	struct in6_addr addr;
	char *dom = "-no-domain-";

	if (h == NULL) {
		seq_puts(m, "#class IP domain\n");
		return 0;
	}
	im = container_of(h, struct ip_map, h);
	/* class addr domain */
	addr = im->m_addr;

	if (test_bit(CACHE_VALID, &h->flags) &&
	    !test_bit(CACHE_NEGATIVE, &h->flags))
		dom = im->m_client->h.name;

	if (ipv6_addr_v4mapped(&addr)) {
		seq_printf(m, "%s %pI4 %s\n",
			im->m_class, &addr.s6_addr32[3], dom);
	} else {
		seq_printf(m, "%s %pI6 %s\n", im->m_class, &addr, dom);
	}
	return 0;
}


static struct ip_map *__ip_map_lookup(struct cache_detail *cd, char *class,
		struct in6_addr *addr)
{
	struct ip_map ip;
	struct cache_head *ch;

	strscpy(ip.m_class, class);
	ip.m_addr = *addr;
	ch = sunrpc_cache_lookup_rcu(cd, &ip.h,
				     hash_str(class, IP_HASHBITS) ^
				     hash_ip6(addr));

	if (ch)
		return container_of(ch, struct ip_map, h);
	else
		return NULL;
}

static int __ip_map_update(struct cache_detail *cd, struct ip_map *ipm,
		struct unix_domain *udom, time64_t expiry)
{
	struct ip_map ip;
	struct cache_head *ch;

	ip.m_client = udom;
	ip.h.flags = 0;
	if (!udom)
		set_bit(CACHE_NEGATIVE, &ip.h.flags);
	ip.h.expiry_time = expiry;
	ch = sunrpc_cache_update(cd, &ip.h, &ipm->h,
				 hash_str(ipm->m_class, IP_HASHBITS) ^
				 hash_ip6(&ipm->m_addr));
	if (!ch)
		return -ENOMEM;
	cache_put(ch, cd);
	return 0;
}

void svcauth_unix_purge(struct net *net)
{
	struct sunrpc_net *sn;

	sn = net_generic(net, sunrpc_net_id);
	cache_purge(sn->ip_map_cache);
}
EXPORT_SYMBOL_GPL(svcauth_unix_purge);

static inline struct ip_map *
ip_map_cached_get(struct svc_xprt *xprt)
{
	struct ip_map *ipm = NULL;
	struct sunrpc_net *sn;

	if (test_bit(XPT_CACHE_AUTH, &xprt->xpt_flags)) {
		spin_lock(&xprt->xpt_lock);
		ipm = xprt->xpt_auth_cache;
		if (ipm != NULL) {
			sn = net_generic(xprt->xpt_net, sunrpc_net_id);
			if (cache_is_expired(sn->ip_map_cache, &ipm->h)) {
				/*
				 * The entry has been invalidated since it was
				 * remembered, e.g. by a second mount from the
				 * same IP address.
				 */
				xprt->xpt_auth_cache = NULL;
				spin_unlock(&xprt->xpt_lock);
				cache_put(&ipm->h, sn->ip_map_cache);
				return NULL;
			}
			cache_get(&ipm->h);
		}
		spin_unlock(&xprt->xpt_lock);
	}
	return ipm;
}

static inline void
ip_map_cached_put(struct svc_xprt *xprt, struct ip_map *ipm)
{
	if (test_bit(XPT_CACHE_AUTH, &xprt->xpt_flags)) {
		spin_lock(&xprt->xpt_lock);
		if (xprt->xpt_auth_cache == NULL) {
			/* newly cached, keep the reference */
			xprt->xpt_auth_cache = ipm;
			ipm = NULL;
		}
		spin_unlock(&xprt->xpt_lock);
	}
	if (ipm) {
		struct sunrpc_net *sn;

		sn = net_generic(xprt->xpt_net, sunrpc_net_id);
		cache_put(&ipm->h, sn->ip_map_cache);
	}
}

void
svcauth_unix_info_release(struct svc_xprt *xpt)
{
	struct ip_map *ipm;

	ipm = xpt->xpt_auth_cache;
	if (ipm != NULL) {
		struct sunrpc_net *sn;

		sn = net_generic(xpt->xpt_net, sunrpc_net_id);
		cache_put(&ipm->h, sn->ip_map_cache);
	}
}

/****************************************************************************
 * auth.unix.gid cache
 * simple cache to map a UID to a list of GIDs
 * because AUTH_UNIX aka AUTH_SYS has a max of UNX_NGROUPS
 */
#define	GID_HASHBITS	8
#define	GID_HASHMAX	(1<<GID_HASHBITS)

struct unix_gid {
	struct cache_head	h;
	kuid_t			uid;
	struct group_info	*gi;
	struct rcu_head		rcu;
};

static int unix_gid_hash(kuid_t uid)
{
	return hash_long(from_kuid(&init_user_ns, uid), GID_HASHBITS);
}

static void unix_gid_free(struct rcu_head *rcu)
{
	struct unix_gid *ug = container_of(rcu, struct unix_gid, rcu);
	struct cache_head *item = &ug->h;

	if (test_bit(CACHE_VALID, &item->flags) &&
	    !test_bit(CACHE_NEGATIVE, &item->flags))
		put_group_info(ug->gi);
	kfree(ug);
}

static void unix_gid_put(struct kref *kref)
{
	struct cache_head *item = container_of(kref, struct cache_head, ref);
	struct unix_gid *ug = container_of(item, struct unix_gid, h);

	call_rcu(&ug->rcu, unix_gid_free);
}

static int unix_gid_match(struct cache_head *corig, struct cache_head *cnew)
{
	struct unix_gid *orig = container_of(corig, struct unix_gid, h);
	struct unix_gid *new = container_of(cnew, struct unix_gid, h);
	return uid_eq(orig->uid, new->uid);
}
static void unix_gid_init(struct cache_head *cnew, struct cache_head *citem)
{
	struct unix_gid *new = container_of(cnew, struct unix_gid, h);
	struct unix_gid *item = container_of(citem, struct unix_gid, h);
	new->uid = item->uid;
}
static void unix_gid_update(struct cache_head *cnew, struct cache_head *citem)
{
	struct unix_gid *new = container_of(cnew, struct unix_gid, h);
	struct unix_gid *item = container_of(citem, struct unix_gid, h);

	get_group_info(item->gi);
	new->gi = item->gi;
}
static struct cache_head *unix_gid_alloc(void)
{
	struct unix_gid *g = kmalloc_obj(*g);
	if (g)
		return &g->h;
	else
		return NULL;
}

static int unix_gid_upcall(struct cache_detail *cd, struct cache_head *h)
{
	return sunrpc_cache_upcall_warn(cd, h);
}

static void unix_gid_request(struct cache_detail *cd,
			     struct cache_head *h,
			     char **bpp, int *blen)
{
	char tuid[20];
	struct unix_gid *ug = container_of(h, struct unix_gid, h);

	snprintf(tuid, 20, "%u", from_kuid(&init_user_ns, ug->uid));
	qword_add(bpp, blen, tuid);
	(*bpp)[-1] = '\n';
}

static struct unix_gid *unix_gid_lookup(struct cache_detail *cd, kuid_t uid);

static int unix_gid_parse(struct cache_detail *cd,
			char *mesg, int mlen)
{
	/* uid expiry Ngid gid0 gid1 ... gidN-1 */
	int id;
	kuid_t uid;
	int gids;
	int rv;
	int i;
	int err;
	time64_t expiry;
	struct unix_gid ug, *ugp;

	if (mesg[mlen - 1] != '\n')
		return -EINVAL;
	mesg[mlen-1] = 0;

	rv = get_int(&mesg, &id);
	if (rv)
		return -EINVAL;
	uid = make_kuid(current_user_ns(), id);
	ug.uid = uid;

	err = get_expiry(&mesg, &expiry);
	if (err)
		return err;

	rv = get_int(&mesg, &gids);
	if (rv || gids < 0 || gids > 8192)
		return -EINVAL;

	ug.gi = groups_alloc(gids);
	if (!ug.gi)
		return -ENOMEM;

	for (i = 0 ; i < gids ; i++) {
		int gid;
		kgid_t kgid;
		rv = get_int(&mesg, &gid);
		err = -EINVAL;
		if (rv)
			goto out;
		kgid = make_kgid(current_user_ns(), gid);
		if (!gid_valid(kgid))
			goto out;
		ug.gi->gid[i] = kgid;
	}

	groups_sort(ug.gi);
	ugp = unix_gid_lookup(cd, uid);
	if (ugp) {
		struct cache_head *ch;
		ug.h.flags = 0;
		/*
		 * mountd sends at least the user's primary group on
		 * success, so an empty list can only mean the lookup
		 * failed. Keep the credential's own groups instead.
		 */
		if (gids == 0)
			set_bit(CACHE_NEGATIVE, &ug.h.flags);
		ug.h.expiry_time = expiry;
		ch = sunrpc_cache_update(cd,
					 &ug.h, &ugp->h,
					 unix_gid_hash(uid));
		if (!ch)
			err = -ENOMEM;
		else {
			err = 0;
			cache_put(ch, cd);
		}
	} else
		err = -ENOMEM;
 out:
	if (ug.gi)
		put_group_info(ug.gi);
	return err;
}

static int unix_gid_show(struct seq_file *m,
			 struct cache_detail *cd,
			 struct cache_head *h)
{
	struct user_namespace *user_ns = m->file->f_cred->user_ns;
	struct unix_gid *ug;
	int i;
	int glen;

	if (h == NULL) {
		seq_puts(m, "#uid cnt: gids...\n");
		return 0;
	}
	ug = container_of(h, struct unix_gid, h);
	if (test_bit(CACHE_VALID, &h->flags) &&
	    !test_bit(CACHE_NEGATIVE, &h->flags))
		glen = ug->gi->ngroups;
	else
		glen = 0;

	seq_printf(m, "%u %d:", from_kuid_munged(user_ns, ug->uid), glen);
	for (i = 0; i < glen; i++)
		seq_printf(m, " %d", from_kgid_munged(user_ns, ug->gi->gid[i]));
	seq_printf(m, "\n");
	return 0;
}

static int unix_gid_notify(struct cache_detail *cd, struct cache_head *h)
{
	return sunrpc_cache_notify(cd, h, SUNRPC_CACHE_TYPE_UNIX_GID);
}

/**
 * sunrpc_nl_unix_gid_get_reqs_dumpit - dump pending unix_gid requests
 * @skb: reply buffer
 * @cb: netlink metadata and command arguments
 *
 * Walk the unix_gid cache's pending request list and create a netlink
 * message with a nested entry for each cache_request, containing the
 * seqno and uid.
 *
 * Uses cb->args[0] as a seqno cursor for dump continuation across
 * multiple netlink messages.
 *
 * Returns the size of the reply or a negative errno.
 */
int sunrpc_nl_unix_gid_get_reqs_dumpit(struct sk_buff *skb,
					struct netlink_callback *cb)
{
	struct sunrpc_net *sn;
	struct cache_detail *cd;
	struct cache_head **items;
	u64 *seqnos;
	int cnt, i, emitted;
	void *hdr;
	int ret;

	sn = net_generic(sock_net(skb->sk), sunrpc_net_id);

	cd = sn->unix_gid_cache;
	if (!cd)
		return -ENODEV;

	cnt = sunrpc_cache_requests_count(cd);
	if (!cnt)
		return 0;

	items = kzalloc_objs(*items, cnt);
	seqnos = kcalloc(cnt, sizeof(*seqnos), GFP_KERNEL);
	if (!items || !seqnos) {
		ret = -ENOMEM;
		goto out_alloc;
	}

	cnt = sunrpc_cache_requests_snapshot(cd, items, seqnos, cnt,
					     cb->args[0]);
	if (!cnt) {
		ret = 0;
		goto out_alloc;
	}

	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
			  cb->nlh->nlmsg_seq, &sunrpc_nl_family,
			  NLM_F_MULTI, SUNRPC_CMD_UNIX_GID_GET_REQS);
	if (!hdr) {
		ret = -ENOBUFS;
		goto out_put;
	}

	emitted = 0;
	for (i = 0; i < cnt; i++) {
		struct unix_gid *ug;
		struct nlattr *nest;

		ug = container_of(items[i], struct unix_gid, h);

		nest = nla_nest_start(skb,
				      SUNRPC_A_UNIX_GID_REQS_REQUESTS);
		if (!nest)
			break;

		if (nla_put_u64_64bit(skb, SUNRPC_A_UNIX_GID_SEQNO,
				      seqnos[i], 0) ||
		    nla_put_u32(skb, SUNRPC_A_UNIX_GID_UID,
				from_kuid(&init_user_ns, ug->uid))) {
			nla_nest_cancel(skb, nest);
			break;
		}

		nla_nest_end(skb, nest);
		cb->args[0] = seqnos[i];
		emitted++;
	}

	if (!emitted) {
		genlmsg_cancel(skb, hdr);
		ret = -EMSGSIZE;
		goto out_put;
	}

	genlmsg_end(skb, hdr);
	ret = skb->len;
out_put:
	for (i = 0; i < cnt; i++)
		cache_put(items[i], cd);
out_alloc:
	kfree(seqnos);
	kfree(items);
	return ret;
}

/**
 * sunrpc_nl_parse_one_unix_gid - parse one unix_gid entry from netlink
 * @cd: cache_detail for the unix_gid cache
 * @attr: nested attribute containing unix_gid fields
 *
 * Parses one unix_gid entry from a netlink message and updates the
 * cache. Mirrors the logic in unix_gid_parse().
 *
 * Returns 0 on success or a negative errno.
 */
static int sunrpc_nl_parse_one_unix_gid(struct cache_detail *cd,
					 struct nlattr *attr)
{
	struct nlattr *tb[SUNRPC_A_UNIX_GID_EXPIRY + 1];
	struct unix_gid ug, *ugp;
	struct timespec64 boot;
	struct nlattr *gid_attr;
	int err, rem, gids = 0;
	kuid_t uid;

	err = nla_parse_nested(tb, SUNRPC_A_UNIX_GID_EXPIRY, attr,
			       sunrpc_unix_gid_nl_policy, NULL);
	if (err)
		return err;

	/* uid (required) */
	if (!tb[SUNRPC_A_UNIX_GID_UID])
		return -EINVAL;
	uid = make_kuid(current_user_ns(),
			nla_get_u32(tb[SUNRPC_A_UNIX_GID_UID]));
	ug.uid = uid;

	/* expiry (required, wallclock seconds) */
	if (!tb[SUNRPC_A_UNIX_GID_EXPIRY])
		return -EINVAL;
	getboottime64(&boot);
	ug.h.flags = 0;
	ug.h.expiry_time = nla_get_u64(tb[SUNRPC_A_UNIX_GID_EXPIRY]) -
			   boot.tv_sec;

	if (tb[SUNRPC_A_UNIX_GID_NEGATIVE]) {
		/* failed lookup: keep the credential's own groups */
		set_bit(CACHE_NEGATIVE, &ug.h.flags);
		ug.gi = groups_alloc(0);
		if (!ug.gi)
			return -ENOMEM;
	} else {
		/* Count gids */
		nla_for_each_nested_type(gid_attr, SUNRPC_A_UNIX_GID_GIDS,
					 attr, rem)
			gids++;

		if (gids > 8192)
			return -EINVAL;

		ug.gi = groups_alloc(gids);
		if (!ug.gi)
			return -ENOMEM;

		gids = 0;
		nla_for_each_nested_type(gid_attr, SUNRPC_A_UNIX_GID_GIDS,
					 attr, rem) {
			kgid_t kgid;

			kgid = make_kgid(current_user_ns(),
					 nla_get_u32(gid_attr));
			if (!gid_valid(kgid)) {
				err = -EINVAL;
				goto out;
			}
			ug.gi->gid[gids++] = kgid;
		}
		groups_sort(ug.gi);
	}

	ugp = unix_gid_lookup(cd, uid);
	if (ugp) {
		struct cache_head *ch;

		ch = sunrpc_cache_update(cd, &ug.h, &ugp->h,
					 unix_gid_hash(uid));
		if (!ch) {
			err = -ENOMEM;
		} else {
			err = 0;
			cache_put(ch, cd);
		}
	} else {
		err = -ENOMEM;
	}
out:
	if (ug.gi)
		put_group_info(ug.gi);
	return err;
}

/**
 * sunrpc_nl_unix_gid_set_reqs_doit - respond to unix_gid requests
 * @skb: reply buffer
 * @info: netlink metadata and command arguments
 *
 * Parse one or more unix_gid cache responses from userspace and
 * update the unix_gid cache accordingly.
 *
 * Returns 0 on success or a negative errno.
 */
int sunrpc_nl_unix_gid_set_reqs_doit(struct sk_buff *skb,
				     struct genl_info *info)
{
	struct sunrpc_net *sn;
	struct cache_detail *cd;
	const struct nlattr *attr;
	int rem, ret = 0;

	sn = net_generic(genl_info_net(info), sunrpc_net_id);

	cd = sn->unix_gid_cache;
	if (!cd)
		return -ENODEV;

	nlmsg_for_each_attr_type(attr, SUNRPC_A_UNIX_GID_REQS_REQUESTS,
				 info->nlhdr, GENL_HDRLEN, rem) {
		ret = sunrpc_nl_parse_one_unix_gid(cd,
						   (struct nlattr *)attr);
		if (ret)
			break;
	}

	return ret;
}

/**
 * sunrpc_nl_cache_flush_doit - flush sunrpc caches via netlink
 * @skb: reply buffer
 * @info: netlink metadata and command arguments
 *
 * Flush the ip_map and/or unix_gid caches. If SUNRPC_A_CACHE_FLUSH_MASK
 * is provided, only flush the caches indicated by the bitmask (bit 1 =
 * ip_map, bit 2 = unix_gid). If omitted, flush both.
 *
 * Return 0 on success or a negative errno.
 */
int sunrpc_nl_cache_flush_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct sunrpc_net *sn;
	u32 mask = ~0U;

	sn = net_generic(genl_info_net(info), sunrpc_net_id);

	if (info->attrs[SUNRPC_A_CACHE_FLUSH_MASK])
		mask = nla_get_u32(info->attrs[SUNRPC_A_CACHE_FLUSH_MASK]);

	if ((mask & SUNRPC_CACHE_TYPE_IP_MAP) &&
	    sn->ip_map_cache)
		cache_purge(sn->ip_map_cache);

	if ((mask & SUNRPC_CACHE_TYPE_UNIX_GID) &&
	    sn->unix_gid_cache)
		cache_purge(sn->unix_gid_cache);

	return 0;
}

static const struct cache_detail unix_gid_cache_template = {
	.owner		= THIS_MODULE,
	.hash_size	= GID_HASHMAX,
	.name		= "auth.unix.gid",
	.cache_put	= unix_gid_put,
	.cache_upcall	= unix_gid_upcall,
	.cache_notify	= unix_gid_notify,
	.cache_request	= unix_gid_request,
	.cache_parse	= unix_gid_parse,
	.cache_show	= unix_gid_show,
	.match		= unix_gid_match,
	.init		= unix_gid_init,
	.update		= unix_gid_update,
	.alloc		= unix_gid_alloc,
};

int unix_gid_cache_create(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	struct cache_detail *cd;
	int err;

	cd = cache_create_net(&unix_gid_cache_template, net);
	if (IS_ERR(cd))
		return PTR_ERR(cd);
	err = cache_register_net(cd, net);
	if (err) {
		cache_destroy_net(cd, net);
		return err;
	}
	sn->unix_gid_cache = cd;
	return 0;
}

void unix_gid_cache_destroy(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	struct cache_detail *cd = sn->unix_gid_cache;

	sn->unix_gid_cache = NULL;
	cache_purge(cd);
	cache_unregister_net(cd, net);
	cache_destroy_net(cd, net);
}

static struct unix_gid *unix_gid_lookup(struct cache_detail *cd, kuid_t uid)
{
	struct unix_gid ug;
	struct cache_head *ch;

	ug.uid = uid;
	ch = sunrpc_cache_lookup_rcu(cd, &ug.h, unix_gid_hash(uid));
	if (ch)
		return container_of(ch, struct unix_gid, h);
	else
		return NULL;
}

static struct group_info *unix_gid_find(kuid_t uid, struct svc_rqst *rqstp)
{
	struct unix_gid *ug;
	struct group_info *gi;
	int ret;
	struct sunrpc_net *sn = net_generic(rqstp->rq_xprt->xpt_net,
					    sunrpc_net_id);

	ug = unix_gid_lookup(sn->unix_gid_cache, uid);
	if (!ug)
		return ERR_PTR(-EAGAIN);
	ret = cache_check(sn->unix_gid_cache, &ug->h, &rqstp->rq_chandle);
	switch (ret) {
	case -ENOENT:
		return ERR_PTR(-ENOENT);
	case -ETIMEDOUT:
		return ERR_PTR(-ESHUTDOWN);
	case 0:
		gi = get_group_info(ug->gi);
		cache_put(&ug->h, sn->unix_gid_cache);
		return gi;
	default:
		return ERR_PTR(-EAGAIN);
	}
}

/*
 * Per-rqst auth borrow.
 *
 * Every AUTH_SYS or AUTH_NULL call otherwise looks up its ip_map (from
 * the transport's one-entry cache, which concurrent calls on a
 * connection miss, or from the hash), takes a reference on the
 * auth_domain it names and later drops it, and looks up its uid's
 * unix_gid and takes and drops a reference on its group_info.  Every
 * client of one domain shares that domain's refcount, and every call
 * from one uid that group_info's, so each call writes cache lines that
 * every CPU writes.  Instead each svc_rqst keeps a few ip_maps and a few
 * unix_gids, with its own reference on each, and a call whose key
 * matches a fresh entry is given that entry's domain and group_info
 * without taking a reference; rq_client_lent and rq_gi_lent tell the
 * release that the references are the cache's.
 *
 * An svc_rqst serves one call at a time and the cache is only its own,
 * so it takes no lock.  Freshness, checked on every hit: CACHE_VALID
 * set, CACHE_CLEANED clear (set on every removal from the hash), not
 * expired, last_refresh after the cache's flush_time, and not past half
 * the refresh age, where cache_check() starts a refresh upcall; a
 * negative ip_map is never kept, a negative unix_gid is (it means "the
 * wire's groups").  So a flush or an update is seen on the next call,
 * and an entry no longer fresh is looked up and checked exactly as
 * before.  The entries are dropped when the rqst is freed, before its
 * net's caches go away.  Only AUTH_SYS and AUTH_NULL borrow: gss calls
 * this function for its fallback domain and releases with its own code.
 */
#define SVCAUTH_BORROW_IPM	4
#define SVCAUTH_BORROW_UG	8

struct svcauth_borrow_ipm {
	u64			used;
	time64_t		checked;	/* the second it was filled */
	struct ip_map		*ipm;
	struct cache_detail	*cd;
	const char		*class;		/* compared only */
};

struct svcauth_borrow_ug {
	u64			used;
	time64_t		checked;	/* the second it was filled */
	struct unix_gid		*ug;
	struct cache_detail	*cd;
};

struct svcauth_unix_borrow {
	u64				clock;
	unsigned int			ipm_mru;
	unsigned int			ug_mru;
	struct svcauth_borrow_ipm	ipm[SVCAUTH_BORROW_IPM];
	struct svcauth_borrow_ug	ug[SVCAUTH_BORROW_UG];
};

struct svcauth_borrow_counts {
	unsigned long	ipm_hit;
	unsigned long	ipm_fill;
	unsigned long	ug_hit;
	unsigned long	ug_fill;
};

static DEFINE_PER_CPU(struct svcauth_borrow_counts, svcauth_borrow_pcpu);

static bool svcauth_borrow = true;
module_param(svcauth_borrow, bool, 0644);
MODULE_PARM_DESC(svcauth_borrow,
		 "Cache AUTH_SYS ip_maps and gids in each svc_rqst (read on every call)");

static int svcauth_borrow_stats_get(char *buf, const struct kernel_param *kp)
{
	struct svcauth_borrow_counts c = { };
	int cpu;

	for_each_possible_cpu(cpu) {
		const struct svcauth_borrow_counts *p =
			per_cpu_ptr(&svcauth_borrow_pcpu, cpu);

		c.ipm_hit += READ_ONCE(p->ipm_hit);
		c.ipm_fill += READ_ONCE(p->ipm_fill);
		c.ug_hit += READ_ONCE(p->ug_hit);
		c.ug_fill += READ_ONCE(p->ug_fill);
	}
	return sysfs_emit(buf, "ipm_hit %lu ipm_fill %lu ug_hit %lu ug_fill %lu\n",
			  c.ipm_hit, c.ipm_fill, c.ug_hit, c.ug_fill);
}

static const struct kernel_param_ops svcauth_borrow_stats_ops = {
	.get	= svcauth_borrow_stats_get,
};
module_param_cb(svcauth_borrow_stats, &svcauth_borrow_stats_ops, NULL, 0444);
MODULE_PARM_DESC(svcauth_borrow_stats, "The svcauth borrow's hits and fills");

/* One read of each field, so the decision is internally consistent. */
static bool svcauth_borrow_fresh(const struct cache_detail *cd,
				 const struct cache_head *h, time64_t now,
				 time64_t checked)
{
	unsigned long flags = READ_ONCE(h->flags);
	time64_t expiry = READ_ONCE(h->expiry_time);
	time64_t refreshed = READ_ONCE(h->last_refresh);

	if (!(flags & BIT(CACHE_VALID)) || (flags & BIT(CACHE_CLEANED)))
		return false;
	/* an entry sunrpc_cache_update() replaced has expiry_time 0 */
	if (expiry <= now || READ_ONCE(cd->flush_time) >= refreshed)
		return false;
	/*
	 * cache_check() starts a refresh upcall past half the refresh age;
	 * once one is pending it returns the entry as it stands until the
	 * entry expires, so a borrow may too.  When no one listens for the
	 * upcall (a unix_gid when mountd does not manage gids) none is ever
	 * pending, and the entry would miss on every call of its second
	 * half: let one call a second through to the lookup's cache_check()
	 * (@checked is the second the rqst's cache last did), and lend the
	 * entry to the others.
	 */
	return now - refreshed <= (expiry - refreshed) / 2 ||
	       (flags & BIT(CACHE_PENDING)) || checked == now;
}

/* The rqst's cache, made on its first eligible call; NULL to not borrow. */
static struct svcauth_unix_borrow *svcauth_borrow_get(struct svc_rqst *rqstp)
{
	u32 flavour = rqstp->rq_authop->flavour;

	if (!READ_ONCE(svcauth_borrow) ||
	    (flavour != RPC_AUTH_UNIX && flavour != RPC_AUTH_NULL))
		return NULL;
	if (unlikely(!rqstp->rq_auth_borrow))
		rqstp->rq_auth_borrow = kzalloc(sizeof(*rqstp->rq_auth_borrow),
						GFP_NOWAIT | __GFP_NOWARN);
	return rqstp->rq_auth_borrow;
}

static __always_inline bool
svcauth_borrow_ipm_match(const struct svcauth_borrow_ipm *e,
			 const struct cache_detail *cd, const char *class,
			 const struct in6_addr *addr)
{
	return e->ipm && ipv6_addr_equal(&e->ipm->m_addr, addr) &&
	       e->class == class && e->cd == cd;
}

/* A fresh ip_map for (@cd, @class, @addr), the entry used last first. */
static struct ip_map *
svcauth_borrow_ipm_find(struct svcauth_unix_borrow *b,
			const struct cache_detail *cd, const char *class,
			const struct in6_addr *addr, time64_t now)
{
	struct svcauth_borrow_ipm *e = &b->ipm[b->ipm_mru];
	unsigned int i;

	if (!svcauth_borrow_ipm_match(e, cd, class, addr)) {
		for (i = 0; i < SVCAUTH_BORROW_IPM; i++) {
			e = &b->ipm[i];
			if (i != b->ipm_mru &&
			    svcauth_borrow_ipm_match(e, cd, class, addr))
				break;
		}
		if (i == SVCAUTH_BORROW_IPM)
			return NULL;
		b->ipm_mru = i;
	}
	if (!svcauth_borrow_fresh(e->cd, &e->ipm->h, now, e->checked) ||
	    test_bit(CACHE_NEGATIVE, &e->ipm->h.flags))
		return NULL;
	e->used = ++b->clock;
	return e->ipm;
}

/*
 * Keep @ipm, with the reference the caller's lookup took, in place of
 * the entry with the same key, else an empty one, else the least
 * recently used.  The entry dropped is put; that never sleeps.
 */
static void svcauth_borrow_ipm_fill(struct svcauth_unix_borrow *b,
				    struct cache_detail *cd, const char *class,
				    struct ip_map *ipm)
{
	struct svcauth_borrow_ipm *e, *victim = NULL;
	unsigned int i;

	for (i = 0; i < SVCAUTH_BORROW_IPM; i++) {
		e = &b->ipm[i];
		if (svcauth_borrow_ipm_match(e, cd, class, &ipm->m_addr)) {
			victim = e;
			break;
		}
		if (!victim || (victim->ipm &&
				(!e->ipm || e->used < victim->used)))
			victim = e;
	}
	if (victim->ipm)
		cache_put(&victim->ipm->h, victim->cd);
	victim->ipm = ipm;
	victim->cd = cd;
	victim->class = class;
	victim->used = ++b->clock;
	victim->checked = seconds_since_boot();
	b->ipm_mru = victim - b->ipm;
	this_cpu_inc(svcauth_borrow_pcpu.ipm_fill);
}

static __always_inline bool
svcauth_borrow_ug_match(const struct svcauth_borrow_ug *e,
			const struct cache_detail *cd, kuid_t uid)
{
	return e->ug && uid_eq(e->ug->uid, uid) && e->cd == cd;
}

/* A fresh unix_gid for @uid, positive or negative, the last used first. */
static struct unix_gid *
svcauth_borrow_ug_find(struct svcauth_unix_borrow *b,
		       const struct cache_detail *cd, kuid_t uid, time64_t now)
{
	struct svcauth_borrow_ug *e = &b->ug[b->ug_mru];
	unsigned int i;

	if (!svcauth_borrow_ug_match(e, cd, uid)) {
		for (i = 0; i < SVCAUTH_BORROW_UG; i++) {
			e = &b->ug[i];
			if (i != b->ug_mru && svcauth_borrow_ug_match(e, cd, uid))
				break;
		}
		if (i == SVCAUTH_BORROW_UG)
			return NULL;
		b->ug_mru = i;
	}
	if (!svcauth_borrow_fresh(e->cd, &e->ug->h, now, e->checked))
		return NULL;
	e->used = ++b->clock;
	return e->ug;
}

/* As svcauth_borrow_ipm_fill(), for a unix_gid. */
static void svcauth_borrow_ug_fill(struct svcauth_unix_borrow *b,
				   struct cache_detail *cd, struct unix_gid *ug)
{
	struct svcauth_borrow_ug *e, *victim = NULL;
	unsigned int i;

	for (i = 0; i < SVCAUTH_BORROW_UG; i++) {
		e = &b->ug[i];
		if (svcauth_borrow_ug_match(e, cd, ug->uid)) {
			victim = e;
			break;
		}
		if (!victim || (victim->ug &&
				(!e->ug || e->used < victim->used)))
			victim = e;
	}
	if (victim->ug)
		cache_put(&victim->ug->h, victim->cd);
	victim->ug = ug;
	victim->cd = cd;
	victim->used = ++b->clock;
	victim->checked = seconds_since_boot();
	b->ug_mru = victim - b->ug;
	this_cpu_inc(svcauth_borrow_pcpu.ug_fill);
}

/**
 * svcauth_unix_borrow_free - drop an svc_rqst's cached ip_maps and gids
 * @rqstp: the rqst, serving no call
 *
 * Called as the rqst is freed: a thread's at its exit, an rtc pool's as
 * the pool goes, either before the service's net and its caches go.
 */
void svcauth_unix_borrow_free(struct svc_rqst *rqstp)
{
	struct svcauth_unix_borrow *b = rqstp->rq_auth_borrow;
	unsigned int i;

	if (!b)
		return;
	for (i = 0; i < SVCAUTH_BORROW_IPM; i++)
		if (b->ipm[i].ipm)
			cache_put(&b->ipm[i].ipm->h, b->ipm[i].cd);
	for (i = 0; i < SVCAUTH_BORROW_UG; i++)
		if (b->ug[i].ug)
			cache_put(&b->ug[i].ug->h, b->ug[i].cd);
	kfree(b);
	rqstp->rq_auth_borrow = NULL;
}
EXPORT_SYMBOL_IF_KUNIT(svcauth_unix_borrow_free);

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * For the KUnit suites: the AUTH_SYS ops, caches made from this file's
 * templates on a test net, entries set as mountd's downcalls set them,
 * and the borrow's switch and counters.
 */
VISIBLE_IF_KUNIT struct auth_ops *svcauth_unix_kunit_ops(u32 flavour)
{
	return flavour == RPC_AUTH_NULL ? &svcauth_null : &svcauth_unix;
}
EXPORT_SYMBOL_IF_KUNIT(svcauth_unix_kunit_ops);

static const struct cache_detail ip_map_cache_template;

VISIBLE_IF_KUNIT struct cache_detail *
svcauth_unix_kunit_cache_create(struct net *net, bool gid)
{
	return cache_create_net(gid ? &unix_gid_cache_template :
				      &ip_map_cache_template, net);
}
EXPORT_SYMBOL_IF_KUNIT(svcauth_unix_kunit_cache_create);

/* Set (@class, @addr) to @dom, or negative for NULL; returns it, held. */
VISIBLE_IF_KUNIT struct cache_head *
svcauth_unix_kunit_ip_map_set(struct cache_detail *cd, char *class,
			      struct in6_addr *addr, struct auth_domain *dom,
			      time64_t expiry)
{
	struct ip_map *ipm = __ip_map_lookup(cd, class, addr);

	if (!ipm)
		return NULL;
	/* the update consumes the lookup's reference, as in ip_map_parse() */
	if (__ip_map_update(cd, ipm,
			    dom ? container_of(dom, struct unix_domain, h) : NULL,
			    expiry))
		return NULL;
	ipm = __ip_map_lookup(cd, class, addr);
	return ipm ? &ipm->h : NULL;
}
EXPORT_SYMBOL_IF_KUNIT(svcauth_unix_kunit_ip_map_set);

/* Set @uid's groups to @gi, or negative for NULL; returns it, held. */
VISIBLE_IF_KUNIT struct cache_head *
svcauth_unix_kunit_gid_set(struct cache_detail *cd, kuid_t uid,
			   struct group_info *gi, time64_t expiry)
{
	struct unix_gid ug = { .uid = uid }, *ugp;
	struct cache_head *ch;

	ug.gi = gi ? get_group_info(gi) : groups_alloc(0);
	if (!ug.gi)
		return NULL;
	ugp = unix_gid_lookup(cd, uid);
	if (!ugp)
		goto out;
	ug.h.flags = 0;
	if (!gi)
		set_bit(CACHE_NEGATIVE, &ug.h.flags);
	ug.h.expiry_time = expiry;
	ch = sunrpc_cache_update(cd, &ug.h, &ugp->h, unix_gid_hash(uid));
	if (ch)
		cache_put(ch, cd);
out:
	put_group_info(ug.gi);
	ugp = unix_gid_lookup(cd, uid);
	return ugp ? &ugp->h : NULL;
}
EXPORT_SYMBOL_IF_KUNIT(svcauth_unix_kunit_gid_set);

VISIBLE_IF_KUNIT struct auth_domain *
svcauth_unix_kunit_ip_map_domain(struct cache_head *h)
{
	return &container_of(h, struct ip_map, h)->m_client->h;
}
EXPORT_SYMBOL_IF_KUNIT(svcauth_unix_kunit_ip_map_domain);

VISIBLE_IF_KUNIT struct group_info *
svcauth_unix_kunit_gid_groups(struct cache_head *h)
{
	return container_of(h, struct unix_gid, h)->gi;
}
EXPORT_SYMBOL_IF_KUNIT(svcauth_unix_kunit_gid_groups);

VISIBLE_IF_KUNIT void svcauth_unix_kunit_borrow_enable(bool on)
{
	WRITE_ONCE(svcauth_borrow, on);
}
EXPORT_SYMBOL_IF_KUNIT(svcauth_unix_kunit_borrow_enable);

/* The counters' sums: ipm_hit, ipm_fill, ug_hit, ug_fill. */
VISIBLE_IF_KUNIT void svcauth_unix_kunit_borrow_counts(unsigned long c[4])
{
	int cpu;

	c[0] = c[1] = c[2] = c[3] = 0;
	for_each_possible_cpu(cpu) {
		const struct svcauth_borrow_counts *p =
			per_cpu_ptr(&svcauth_borrow_pcpu, cpu);

		c[0] += READ_ONCE(p->ipm_hit);
		c[1] += READ_ONCE(p->ipm_fill);
		c[2] += READ_ONCE(p->ug_hit);
		c[3] += READ_ONCE(p->ug_fill);
	}
}
EXPORT_SYMBOL_IF_KUNIT(svcauth_unix_kunit_borrow_counts);
#endif

/* Drop the call's domain and group_info unless the rqst's cache lent them. */
static void svcauth_unix_put_client(struct svc_rqst *rqstp)
{
	if (rqstp->rq_client && !rqstp->rq_client_lent)
		auth_domain_put(rqstp->rq_client);
	rqstp->rq_client = NULL;
	rqstp->rq_client_lent = false;
	if (rqstp->rq_cred.cr_group_info && !rqstp->rq_gi_lent)
		put_group_info(rqstp->rq_cred.cr_group_info);
	rqstp->rq_cred.cr_group_info = NULL;
	rqstp->rq_gi_lent = false;
}

/*
 * The uid's groups through the rqst's cache: a hit installs them lent; a
 * miss looks the unix_gid up and checks it as unix_gid_find() does and
 * keeps it, positive or negative.  Returns what svcauth_unix_set_client()
 * returns for the gid step, SVC_OK to go on.
 */
static enum svc_auth_status
svcauth_borrow_gid(struct svc_rqst *rqstp, struct svcauth_unix_borrow *b,
		   struct sunrpc_net *sn, time64_t now)
{
	struct svc_cred *cred = &rqstp->rq_cred;
	struct cache_detail *cd = sn->unix_gid_cache;
	struct unix_gid *ug;

	ug = svcauth_borrow_ug_find(b, cd, cred->cr_uid, now);
	if (ug) {
		this_cpu_inc(svcauth_borrow_pcpu.ug_hit);
	} else {
		ug = unix_gid_lookup(cd, cred->cr_uid);
		if (!ug)
			return SVC_DROP;
		/* cache_check() drops a reference on every result but 0 */
		cache_get(&ug->h);
		switch (cache_check(cd, &ug->h, &rqstp->rq_chandle)) {
		case 0:
			cache_put(&ug->h, cd);
			break;
		case -ENOENT:
			break;
		case -ETIMEDOUT:
			cache_put(&ug->h, cd);
			return SVC_CLOSE;
		default:
			cache_put(&ug->h, cd);
			return SVC_DROP;
		}
		svcauth_borrow_ug_fill(b, cd, ug);
	}
	if (test_bit(CACHE_NEGATIVE, &ug->h.flags))
		return SVC_OK;		/* the wire's groups stand */
	put_group_info(cred->cr_group_info);
	cred->cr_group_info = ug->gi;
	rqstp->rq_gi_lent = true;
	return SVC_OK;
}

enum svc_auth_status
svcauth_unix_set_client(struct svc_rqst *rqstp)
{
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6, sin6_storage;
	struct ip_map *ipm;
	struct group_info *gi;
	struct svc_cred *cred = &rqstp->rq_cred;
	struct svc_xprt *xprt = rqstp->rq_xprt;
	struct net *net = xprt->xpt_net;
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	char *class = rqstp->rq_server->sv_programs->pg_class;
	struct svcauth_unix_borrow *b;
	time64_t now = 0;

	switch (rqstp->rq_addr.ss_family) {
	case AF_INET:
		sin = svc_addr_in(rqstp);
		sin6 = &sin6_storage;
		ipv6_addr_set_v4mapped(sin->sin_addr.s_addr, &sin6->sin6_addr);
		break;
	case AF_INET6:
		sin6 = svc_addr_in6(rqstp);
		break;
	default:
		BUG();
	}

	rqstp->rq_client = NULL;
	rqstp->rq_client_lent = false;
	if (rqstp->rq_proc == 0)
		goto out;

	rqstp->rq_auth_stat = rpc_autherr_badcred;
	b = svcauth_borrow_get(rqstp);
	if (b) {
		now = seconds_since_boot();
		ipm = svcauth_borrow_ipm_find(b, sn->ip_map_cache, class,
					      &sin6->sin6_addr, now);
		if (ipm) {
			this_cpu_inc(svcauth_borrow_pcpu.ipm_hit);
			rqstp->rq_client = &ipm->m_client->h;
			rqstp->rq_client_lent = true;
			goto gid;
		}
		ipm = __ip_map_lookup(sn->ip_map_cache, class,
				      &sin6->sin6_addr);
	} else {
		ipm = ip_map_cached_get(xprt);
		if (ipm == NULL)
			ipm = __ip_map_lookup(sn->ip_map_cache, class,
					      &sin6->sin6_addr);
	}

	if (ipm == NULL)
		return SVC_DENIED;

	switch (cache_check(sn->ip_map_cache, &ipm->h, &rqstp->rq_chandle)) {
		default:
			BUG();
		case -ETIMEDOUT:
			return SVC_CLOSE;
		case -EAGAIN:
			return SVC_DROP;
		case -ENOENT:
			return SVC_DENIED;
		case 0:
			rqstp->rq_client = &ipm->m_client->h;
			if (b) {
				/* the lookup's reference becomes the cache's */
				svcauth_borrow_ipm_fill(b, sn->ip_map_cache,
							class, ipm);
				rqstp->rq_client_lent = true;
			} else {
				kref_get(&rqstp->rq_client->ref);
				ip_map_cached_put(xprt, ipm);
			}
			break;
	}

gid:
	if (b) {
		enum svc_auth_status stat = svcauth_borrow_gid(rqstp, b, sn, now);

		if (stat != SVC_OK)
			return stat;
		goto out;
	}
	gi = unix_gid_find(cred->cr_uid, rqstp);
	switch (PTR_ERR(gi)) {
	case -EAGAIN:
		return SVC_DROP;
	case -ESHUTDOWN:
		return SVC_CLOSE;
	case -ENOENT:
		break;
	default:
		put_group_info(cred->cr_group_info);
		cred->cr_group_info = gi;
	}

out:
	rqstp->rq_auth_stat = rpc_auth_ok;
	return SVC_OK;
}
EXPORT_SYMBOL_GPL(svcauth_unix_set_client);

/**
 * svcauth_null_accept - Decode and validate incoming RPC_AUTH_NULL credential
 * @rqstp: RPC transaction
 *
 * Return values:
 *   %SVC_OK: Both credential and verifier are valid
 *   %SVC_DENIED: Credential or verifier is not valid
 *   %SVC_GARBAGE: Failed to decode credential or verifier
 *   %SVC_CLOSE: Temporary failure
 *
 * rqstp->rq_auth_stat is set as mandated by RFC 5531.
 */
static enum svc_auth_status
svcauth_null_accept(struct svc_rqst *rqstp)
{
	struct xdr_stream *xdr = &rqstp->rq_arg_stream;
	struct svc_cred	*cred = &rqstp->rq_cred;
	u32 flavor, len;
	void *body;

	/* Length of Call's credential body field: */
	if (xdr_stream_decode_u32(xdr, &len) < 0)
		return SVC_GARBAGE;
	if (len != 0) {
		rqstp->rq_auth_stat = rpc_autherr_badcred;
		return SVC_DENIED;
	}

	/* Call's verf field: */
	if (xdr_stream_decode_opaque_auth(xdr, &flavor, &body, &len) < 0)
		return SVC_GARBAGE;
	if (flavor != RPC_AUTH_NULL || len != 0) {
		rqstp->rq_auth_stat = rpc_autherr_badverf;
		return SVC_DENIED;
	}

	/* Signal that mapping to nobody uid/gid is required */
	cred->cr_uid = INVALID_UID;
	cred->cr_gid = INVALID_GID;
	rqstp->rq_gi_lent = false;
	cred->cr_group_info = groups_alloc(0);
	if (cred->cr_group_info == NULL)
		return SVC_CLOSE; /* kmalloc failure - client must retry */

	if (xdr_stream_encode_opaque_auth(&rqstp->rq_res_stream,
					  RPC_AUTH_NULL, NULL, 0) < 0)
		return SVC_CLOSE;
	if (!svcxdr_set_accept_stat(rqstp))
		return SVC_CLOSE;

	rqstp->rq_cred.cr_flavor = RPC_AUTH_NULL;
	return SVC_OK;
}

static int
svcauth_null_release(struct svc_rqst *rqstp)
{
	svcauth_unix_put_client(rqstp);
	return 0; /* don't drop */
}


struct auth_ops svcauth_null = {
	.name		= "null",
	.owner		= THIS_MODULE,
	.flavour	= RPC_AUTH_NULL,
	.accept		= svcauth_null_accept,
	.release	= svcauth_null_release,
	.set_client	= svcauth_unix_set_client,
};


/**
 * svcauth_tls_accept - Decode and validate incoming RPC_AUTH_TLS credential
 * @rqstp: RPC transaction
 *
 * Return values:
 *   %SVC_OK: Both credential and verifier are valid
 *   %SVC_DENIED: Credential or verifier is not valid
 *   %SVC_GARBAGE: Failed to decode credential or verifier
 *   %SVC_CLOSE: Temporary failure
 *
 * rqstp->rq_auth_stat is set as mandated by RFC 5531.
 */
static enum svc_auth_status
svcauth_tls_accept(struct svc_rqst *rqstp)
{
	struct xdr_stream *xdr = &rqstp->rq_arg_stream;
	struct svc_cred	*cred = &rqstp->rq_cred;
	struct svc_xprt *xprt = rqstp->rq_xprt;
	u32 flavor, len;
	void *body;
	__be32 *p;

	/* Length of Call's credential body field: */
	if (xdr_stream_decode_u32(xdr, &len) < 0)
		return SVC_GARBAGE;
	if (len != 0) {
		rqstp->rq_auth_stat = rpc_autherr_badcred;
		return SVC_DENIED;
	}

	/* Call's verf field: */
	if (xdr_stream_decode_opaque_auth(xdr, &flavor, &body, &len) < 0)
		return SVC_GARBAGE;
	if (flavor != RPC_AUTH_NULL || len != 0) {
		rqstp->rq_auth_stat = rpc_autherr_badverf;
		return SVC_DENIED;
	}

	/* AUTH_TLS is not valid on non-NULL procedures */
	if (rqstp->rq_proc != 0) {
		rqstp->rq_auth_stat = rpc_autherr_badcred;
		return SVC_DENIED;
	}

	/* AUTH_TLS is not valid on backchannel (rq_xprt is NULL) */
	if (!xprt) {
		rqstp->rq_auth_stat = rpc_autherr_badcred;
		return SVC_DENIED;
	}

	/* Signal that mapping to nobody uid/gid is required */
	cred->cr_uid = INVALID_UID;
	cred->cr_gid = INVALID_GID;
	rqstp->rq_gi_lent = false;
	cred->cr_group_info = groups_alloc(0);
	if (cred->cr_group_info == NULL)
		return SVC_CLOSE;

	if (xprt->xpt_ops->xpo_handshake && rqstp->rq_server->sv_tls) {
		p = xdr_reserve_space(&rqstp->rq_res_stream, XDR_UNIT * 2 + 8);
		if (!p)
			return SVC_CLOSE;
		trace_svc_tls_start(xprt);
		*p++ = rpc_auth_null;
		*p++ = cpu_to_be32(8);
		memcpy(p, "STARTTLS", 8);

		set_bit(XPT_HANDSHAKE, &xprt->xpt_flags);
		svc_xprt_enqueue(xprt);
	} else {
		trace_svc_tls_unavailable(xprt);
		if (xdr_stream_encode_opaque_auth(&rqstp->rq_res_stream,
						  RPC_AUTH_NULL, NULL, 0) < 0)
			return SVC_CLOSE;
	}
	if (!svcxdr_set_accept_stat(rqstp))
		return SVC_CLOSE;

	rqstp->rq_cred.cr_flavor = RPC_AUTH_TLS;
	return SVC_OK;
}

struct auth_ops svcauth_tls = {
	.name		= "tls",
	.owner		= THIS_MODULE,
	.flavour	= RPC_AUTH_TLS,
	.accept		= svcauth_tls_accept,
	.release	= svcauth_null_release,
	.set_client	= svcauth_unix_set_client,
};


/**
 * svcauth_unix_accept - Decode and validate incoming RPC_AUTH_SYS credential
 * @rqstp: RPC transaction
 *
 * Return values:
 *   %SVC_OK: Both credential and verifier are valid
 *   %SVC_DENIED: Credential or verifier is not valid
 *   %SVC_GARBAGE: Failed to decode credential or verifier
 *   %SVC_CLOSE: Temporary failure
 *
 * rqstp->rq_auth_stat is set as mandated by RFC 5531.
 */
static enum svc_auth_status
svcauth_unix_accept(struct svc_rqst *rqstp)
{
	struct xdr_stream *xdr = &rqstp->rq_arg_stream;
	struct svc_cred	*cred = &rqstp->rq_cred;
	struct user_namespace *userns;
	u32 flavor, len, i;
	void *body;
	__be32 *p;

	/*
	 * This implementation ignores the length of the Call's
	 * credential body field and the timestamp and machinename
	 * fields.
	 */
	p = xdr_inline_decode(xdr, XDR_UNIT * 3);
	if (!p)
		return SVC_GARBAGE;
	len = be32_to_cpup(p + 2);
	if (len > RPC_MAX_MACHINENAME)
		return SVC_GARBAGE;
	if (!xdr_inline_decode(xdr, len))
		return SVC_GARBAGE;

	/*
	 * Note: we skip uid_valid()/gid_valid() checks here for
	 * backwards compatibility with clients that use -1 id's.
	 * Instead, -1 uid or gid is later mapped to the
	 * (export-specific) anonymous id by nfsd_setuser.
	 * Supplementary gid's will be left alone.
	 */
	userns = (rqstp->rq_xprt && rqstp->rq_xprt->xpt_cred) ?
		rqstp->rq_xprt->xpt_cred->user_ns : &init_user_ns;
	if (xdr_stream_decode_u32(xdr, &i) < 0)
		return SVC_GARBAGE;
	cred->cr_uid = make_kuid(userns, i);
	if (xdr_stream_decode_u32(xdr, &i) < 0)
		return SVC_GARBAGE;
	cred->cr_gid = make_kgid(userns, i);

	if (xdr_stream_decode_u32(xdr, &len) < 0)
		return SVC_GARBAGE;
	if (len > UNX_NGROUPS)
		goto badcred;
	p = xdr_inline_decode(xdr, XDR_UNIT * len);
	if (!p)
		return SVC_GARBAGE;
	rqstp->rq_gi_lent = false;
	cred->cr_group_info = groups_alloc(len);
	if (cred->cr_group_info == NULL)
		return SVC_CLOSE;
	for (i = 0; i < len; i++) {
		kgid_t kgid = make_kgid(userns, be32_to_cpup(p++));
		cred->cr_group_info->gid[i] = kgid;
	}
	groups_sort(cred->cr_group_info);

	/* Call's verf field: */
	if (xdr_stream_decode_opaque_auth(xdr, &flavor, &body, &len) < 0)
		return SVC_GARBAGE;
	if (flavor != RPC_AUTH_NULL || len != 0) {
		rqstp->rq_auth_stat = rpc_autherr_badverf;
		return SVC_DENIED;
	}

	if (xdr_stream_encode_opaque_auth(&rqstp->rq_res_stream,
					  RPC_AUTH_NULL, NULL, 0) < 0)
		return SVC_CLOSE;
	if (!svcxdr_set_accept_stat(rqstp))
		return SVC_CLOSE;

	rqstp->rq_cred.cr_flavor = RPC_AUTH_UNIX;
	return SVC_OK;

badcred:
	rqstp->rq_auth_stat = rpc_autherr_badcred;
	return SVC_DENIED;
}

static int
svcauth_unix_release(struct svc_rqst *rqstp)
{
	/* Verifier (such as it is) is already in place.
	 */
	svcauth_unix_put_client(rqstp);
	return 0;
}


struct auth_ops svcauth_unix = {
	.name		= "unix",
	.owner		= THIS_MODULE,
	.flavour	= RPC_AUTH_UNIX,
	.accept		= svcauth_unix_accept,
	.release	= svcauth_unix_release,
	.domain_release	= svcauth_unix_domain_release,
	.set_client	= svcauth_unix_set_client,
};

static int ip_map_notify(struct cache_detail *cd, struct cache_head *h)
{
	return sunrpc_cache_notify(cd, h, SUNRPC_CACHE_TYPE_IP_MAP);
}

/**
 * sunrpc_nl_ip_map_get_reqs_dumpit - dump pending ip_map requests
 * @skb: reply buffer
 * @cb: netlink metadata and command arguments
 *
 * Walk the ip_map cache's pending request list and create a netlink
 * message with a nested entry for each cache_request, containing the
 * seqno, class and addr.
 *
 * Uses cb->args[0] as a seqno cursor for dump continuation across
 * multiple netlink messages.
 *
 * Returns the size of the reply or a negative errno.
 */
int sunrpc_nl_ip_map_get_reqs_dumpit(struct sk_buff *skb,
				     struct netlink_callback *cb)
{
	struct sunrpc_net *sn;
	struct cache_detail *cd;
	struct cache_head **items;
	u64 *seqnos;
	int cnt, i, emitted;
	void *hdr;
	int ret;

	sn = net_generic(sock_net(skb->sk), sunrpc_net_id);

	cd = sn->ip_map_cache;
	if (!cd)
		return -ENODEV;

	cnt = sunrpc_cache_requests_count(cd);
	if (!cnt)
		return 0;

	items = kzalloc_objs(*items, cnt);
	seqnos = kcalloc(cnt, sizeof(*seqnos), GFP_KERNEL);
	if (!items || !seqnos) {
		ret = -ENOMEM;
		goto out_alloc;
	}

	cnt = sunrpc_cache_requests_snapshot(cd, items, seqnos, cnt,
					     cb->args[0]);
	if (!cnt) {
		ret = 0;
		goto out_alloc;
	}

	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
			  cb->nlh->nlmsg_seq, &sunrpc_nl_family,
			  NLM_F_MULTI, SUNRPC_CMD_IP_MAP_GET_REQS);
	if (!hdr) {
		ret = -ENOBUFS;
		goto out_put;
	}

	emitted = 0;
	for (i = 0; i < cnt; i++) {
		struct ip_map *im;
		struct nlattr *nest;
		char text_addr[40];

		im = container_of(items[i], struct ip_map, h);

		if (ipv6_addr_v4mapped(&im->m_addr))
			snprintf(text_addr, 20, "%pI4",
				 &im->m_addr.s6_addr32[3]);
		else
			snprintf(text_addr, 40, "%pI6", &im->m_addr);

		nest = nla_nest_start(skb, SUNRPC_A_IP_MAP_REQS_REQUESTS);
		if (!nest)
			break;

		if (nla_put_u64_64bit(skb, SUNRPC_A_IP_MAP_SEQNO,
				      seqnos[i], 0) ||
		    nla_put_string(skb, SUNRPC_A_IP_MAP_CLASS,
				   im->m_class) ||
		    nla_put_string(skb, SUNRPC_A_IP_MAP_ADDR, text_addr)) {
			nla_nest_cancel(skb, nest);
			break;
		}

		nla_nest_end(skb, nest);
		cb->args[0] = seqnos[i];
		emitted++;
	}

	if (!emitted) {
		genlmsg_cancel(skb, hdr);
		ret = -EMSGSIZE;
		goto out_put;
	}

	genlmsg_end(skb, hdr);
	ret = skb->len;
out_put:
	for (i = 0; i < cnt; i++)
		cache_put(items[i], cd);
out_alloc:
	kfree(seqnos);
	kfree(items);
	return ret;
}

/**
 * sunrpc_nl_parse_one_ip_map - parse one ip_map entry from netlink
 * @cd: cache_detail for the ip_map cache
 * @attr: nested attribute containing ip_map fields
 *
 * Parses one ip_map entry from a netlink message and updates the
 * cache. Mirrors the logic in ip_map_parse().
 *
 * Returns 0 on success or a negative errno.
 */
static int sunrpc_nl_parse_one_ip_map(struct cache_detail *cd,
				      struct nlattr *attr)
{
	struct nlattr *tb[SUNRPC_A_IP_MAP_EXPIRY + 1];
	union {
		struct sockaddr		sa;
		struct sockaddr_in	s4;
		struct sockaddr_in6	s6;
	} address;
	struct sockaddr_in6 sin6;
	struct ip_map *ipmp;
	struct auth_domain *dom = NULL;
	struct unix_domain *udom = NULL;
	struct timespec64 boot;
	time64_t expiry;
	char class[8];
	int err;
	int len;

	err = nla_parse_nested(tb, SUNRPC_A_IP_MAP_EXPIRY, attr,
			       sunrpc_ip_map_nl_policy, NULL);
	if (err)
		return err;

	/* class (required) */
	if (!tb[SUNRPC_A_IP_MAP_CLASS])
		return -EINVAL;
	len = nla_len(tb[SUNRPC_A_IP_MAP_CLASS]);
	if (len <= 0 || len > sizeof(class))
		return -EINVAL;
	nla_strscpy(class, tb[SUNRPC_A_IP_MAP_CLASS], sizeof(class));

	/* addr (required) */
	if (!tb[SUNRPC_A_IP_MAP_ADDR])
		return -EINVAL;
	if (rpc_pton(cd->net, nla_data(tb[SUNRPC_A_IP_MAP_ADDR]),
		     nla_len(tb[SUNRPC_A_IP_MAP_ADDR]) - 1,
		     &address.sa, sizeof(address)) == 0)
		return -EINVAL;

	switch (address.sa.sa_family) {
	case AF_INET:
		sin6.sin6_family = AF_INET6;
		ipv6_addr_set_v4mapped(address.s4.sin_addr.s_addr,
				       &sin6.sin6_addr);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		memcpy(&sin6, &address.s6, sizeof(sin6));
		break;
#endif
	default:
		return -EINVAL;
	}

	/* expiry (required, wallclock seconds) */
	if (!tb[SUNRPC_A_IP_MAP_EXPIRY])
		return -EINVAL;
	getboottime64(&boot);
	expiry = nla_get_u64(tb[SUNRPC_A_IP_MAP_EXPIRY]) - boot.tv_sec;

	/* domain name or negative */
	if (tb[SUNRPC_A_IP_MAP_NEGATIVE]) {
		udom = NULL;
	} else if (tb[SUNRPC_A_IP_MAP_DOMAIN]) {
		dom = unix_domain_find(nla_data(tb[SUNRPC_A_IP_MAP_DOMAIN]));
		if (!dom)
			return -ENOENT;
		udom = container_of(dom, struct unix_domain, h);
	} else {
		return -EINVAL;
	}

	ipmp = __ip_map_lookup(cd, class, &sin6.sin6_addr);
	if (ipmp)
		err = __ip_map_update(cd, ipmp, udom, expiry);
	else
		err = -ENOMEM;

	if (dom)
		auth_domain_put(dom);

	cache_flush();
	return err;
}

/**
 * sunrpc_nl_ip_map_set_reqs_doit - respond to ip_map requests
 * @skb: reply buffer
 * @info: netlink metadata and command arguments
 *
 * Parse one or more ip_map cache responses from userspace and
 * update the ip_map cache accordingly.
 *
 * Returns 0 on success or a negative errno.
 */
int sunrpc_nl_ip_map_set_reqs_doit(struct sk_buff *skb,
				   struct genl_info *info)
{
	struct sunrpc_net *sn;
	struct cache_detail *cd;
	const struct nlattr *attr;
	int rem, ret = 0;

	sn = net_generic(genl_info_net(info), sunrpc_net_id);

	cd = sn->ip_map_cache;
	if (!cd)
		return -ENODEV;

	nlmsg_for_each_attr_type(attr, SUNRPC_A_IP_MAP_REQS_REQUESTS,
				 info->nlhdr, GENL_HDRLEN, rem) {
		ret = sunrpc_nl_parse_one_ip_map(cd,
						 (struct nlattr *)attr);
		if (ret)
			break;
	}

	return ret;
}

static const struct cache_detail ip_map_cache_template = {
	.owner		= THIS_MODULE,
	.hash_size	= IP_HASHMAX,
	.name		= "auth.unix.ip",
	.cache_put	= ip_map_put,
	.cache_upcall	= ip_map_upcall,
	.cache_notify	= ip_map_notify,
	.cache_request	= ip_map_request,
	.cache_parse	= ip_map_parse,
	.cache_show	= ip_map_show,
	.match		= ip_map_match,
	.init		= ip_map_init,
	.update		= update,
	.alloc		= ip_map_alloc,
};

int ip_map_cache_create(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	struct cache_detail *cd;
	int err;

	cd = cache_create_net(&ip_map_cache_template, net);
	if (IS_ERR(cd))
		return PTR_ERR(cd);
	err = cache_register_net(cd, net);
	if (err) {
		cache_destroy_net(cd, net);
		return err;
	}
	sn->ip_map_cache = cd;
	return 0;
}

void ip_map_cache_destroy(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	struct cache_detail *cd = sn->ip_map_cache;

	sn->ip_map_cache = NULL;
	cache_purge(cd);
	cache_unregister_net(cd, net);
	cache_destroy_net(cd, net);
}
