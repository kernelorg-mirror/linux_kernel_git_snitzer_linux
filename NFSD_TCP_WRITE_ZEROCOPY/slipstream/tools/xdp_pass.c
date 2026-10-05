// SPDX-License-Identifier: GPL-2.0
/*
 * Pass every frame.  Attached to the server end of a veth pair it makes veth
 * copy each received skb into page-pool pages marked for recycling, so NFS
 * receive exercises page-pool loans.  xdp.frags admits multi-buffer frames.
 */
#define SEC(name) __attribute__((section(name), used))

SEC("xdp.frags")
int xdp_pass(void *ctx)
{
	return 2;	/* XDP_PASS */
}

char _license[] SEC("license") = "GPL";
