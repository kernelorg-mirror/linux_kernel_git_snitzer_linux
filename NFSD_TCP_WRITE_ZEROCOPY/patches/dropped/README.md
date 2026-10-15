# Dropped patches

Code commits taken out of the series, kept here as `git format-patch`
output so that they can be read or applied again. Each entry says what
the patch did, why it was dropped, and what has to change with it.

## 0001-SUNRPC-Allow-more-than-one-thread-pool-per-NUMA-node.patch

From `99c4413a0644` on `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`, the
second commit of the code group. It adds `sunrpc.pools_per_node` (1 to
64, default 1): a NUMA node gets that many thread pools, and
`svc_xprt_enqueue()` puts a transport on one of its node's pools by a
hash of the transport, to spread the contention on one pool's queue
lock that 64 connections and 512 threads showed on tardis1 (T6 in
`../../XEU_CONVERGENCE.md`).

**Dropped (Mike, 15 October).** It ran at the settled setting
(`pools_per_node=4`) from build 462 on, pinned in
`/etc/modprobe.d/sunrpc.conf` on 11 October, but it was never shown to
earn its place: "written, not shown to work" in `XEU_CONVERGENCE.md`'s
account of T6, and step 6 of `../../CODE-SERIES.md` lists it among the
settings in use whose effect was never measured on its own. The test
written for it, 64 connections with 1 pool and with 4 ("Does
`pools_per_node` earn its place" in `../../xeu-rig/README.md`), was
never run; the rig runs sixteen connections, where the lock it spreads
is not near the top of the list (S6). It is also one of the three
commits put at the front of the code group on 14 October so that they
could go without touching the rest.

What dropping it takes (to do once the runs on tardis1 that use
`pools_per_node=4` are finished):

- The branch: drop the commit. The only conflict is context in
  `net/sunrpc/svc.c`'s includes: the patch adds `<linux/hash.h>` and
  the KUnit interface commit ("SUNRPC/nfsd: expose the receive
  internals for the KUnit suites") adds `<kunit/visibility.h>` after
  it; keep the second.
- The rig: `xeu-rig/settled.sh` holds `pools_per_node=4` in `REST` and
  audits it; on a kernel without the parameter it has to be reported,
  not failed (its `GONE_LATER`), as for `svc_tcp_rx_copy_unlocked`.
  `xeu-rig/swap-sunrpc.sh` (`POOLS_PER_NODE=`) and
  `xeu-rig/build-sunrpc.sh` (its `modinfo` line) name it too.
- tardis1: `pools_per_node=4` comes out of
  `/etc/modprobe.d/sunrpc.conf`.
- The figures: every run from build 462 to build 474 was measured with
  four pools. A kernel without the patch has one; the first runs on it
  are also the comparison with four.
