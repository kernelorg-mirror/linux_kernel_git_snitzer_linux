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

**Dropped (Mike, 15 October).** Chuck Lever has called several thread
pools per NUMA node a dead end, and it was never shown to earn its
place here: it ran at the settled setting (`pools_per_node=4`) from
build 462 on, pinned in `/etc/modprobe.d/sunrpc.conf` on 11 October,
but the test written for it, 64 connections with 1 pool and with 4
("Does `pools_per_node` earn its place" in `../../xeu-rig/README.md`),
was never run, and the rig runs sixteen connections, where the lock it
spreads is not near the top of the list (S6).

What dropping it took, on 15 October:

- The branch: the commit is out of
  `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`; the tip before is kept as
  `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY.pre-drop-pools`. The only
  conflict was context in `net/sunrpc/svc.c`'s includes, in the KUnit
  interface commit ("SUNRPC/nfsd: expose the receive internals for the
  KUnit suites"): the patch had added `<linux/hash.h>`, which nothing
  else in the file uses, and that commit adds `<kunit/visibility.h>`
  after it; only the second remains. The tree after the drop differs
  from the tree before by exactly the patch's lines.
- The rig: `xeu-rig/settled.sh` holds 512 nfsd threads in one pool and
  no longer holds `pools_per_node`; `xeu-rig/swap-sunrpc.sh` lost its
  `POOLS_PER_NODE=` and `xeu-rig/build-sunrpc.sh` no longer lists it.
- tardis1: `pools_per_node=4` is out of `/etc/modprobe.d/sunrpc.conf`
  (the file before is `/root/sunrpc.conf.pre-drop-pools`), and sunrpc
  was reloaded from it at 15:45: one pool of 512 threads, at the
  settled setting.
- The figures: every run from build 462 to build 474 was measured with
  four pools; from 15:45 on 15 October tardis1 runs one.
