/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The "<value> (auto|manual)" syntax of the receive region's and the
 * receive loans' settings, shared by net/core/page_pool.c and
 * net/sunrpc/svcsock.c (and their KUnit suites).  Not for drivers.
 */
#ifndef _NET_PAGE_POOL_CFG_H
#define _NET_PAGE_POOL_CFG_H

#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <linux/mm.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/types.h>

/*
 * The receive region's and the receive loans' sizes (page_pool.rx_region_*,
 * sunrpc.svc_tcp_rx_*, net.sunrpc.region_budget_* and their debugfs files)
 * read back as "<value> (auto)" or "<value> (manual)": the value in effect,
 * and whether it is computed or was set by hand.  A write takes a number,
 * "auto", or a line read back, so a saved value restores as it was: 0,
 * "auto" and "<n> (auto)" return the setting to auto; "<n>" (not 0) and
 * "<n> (manual)" set it by hand.  "0 (manual)" and "off" are a manual 0,
 * which only a receive budget tells apart from auto (no loans at all).
 */
static inline int page_pool_cfg_parse(const char *val, unsigned int *out,
				      bool *manual)
{
	char buf[32], *s, *mode;
	int ret;

	if (strscpy(buf, val, sizeof(buf)) < 0)
		return -EINVAL;
	s = strim(buf);
	if (!strcmp(s, "auto") || !strcmp(s, "off")) {
		*out = 0;
		*manual = s[0] == 'o';
		return 0;
	}
	mode = strchr(s, ' ');
	if (mode) {
		*mode++ = '\0';
		mode = skip_spaces(mode);
	}
	ret = kstrtouint(s, 0, out);
	if (ret)
		return ret;
	if (!mode || !*mode) {
		*manual = *out != 0;
	} else if (!strcmp(mode, "(manual)")) {
		*manual = true;
	} else if (!strcmp(mode, "(auto)")) {
		*out = 0;
		*manual = false;
	} else {
		return -EINVAL;
	}
	return 0;
}

/* @buf holds PAGE_SIZE, as a kernel_param_ops get's does. */
static inline int page_pool_cfg_format(char *buf, unsigned long val,
				       bool manual)
{
	return scnprintf(buf, PAGE_SIZE, "%lu (%s)\n", val,
			 manual ? "manual" : "auto");
}

#endif /* _NET_PAGE_POOL_CFG_H */
