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

## 0002-SUNRPC-Optionally-copy-TCP-record-data-outside-the-s.patch

From `778e7e2f1f62` on `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`, the
last commit of the code group. It adds `sunrpc.svc_tcp_rx_copy_threads`
(default 0, no change): at 1 the actor claims each payload skb by
cloning it and the thread copies it after `->read_sock()` returns, with
the socket unlocked; at 2 or more a thread whose claim completes a
record also releases the transport before copying, so that more than
one thread copies for a connection. It is the fold of the claim commit
(63 in `../../CODE-SERIES.md`) with the copy outside the lock (62, 66).

**Dropped (Mike, 15 October).** It was tried and not adopted: more
threads copying a connection did not move two ports off 60 GB/s ("NFSD
Receive Region Budget", 13 October), and the rig ran it at 0
(`xeu-rig/settled.sh`). Dropping it, with 0003, changed nothing else in
the code: no KUnit test or interface refers to either, and the tree
after differs from the tree before (`.pre-drop-claim-lowat`) by exactly
the two patches. `xeu-rig/settled.sh` reports both settings as "not in
this kernel" where they are gone, and `xeu-rig/twoport.sh` refuses
`THREADS=` and `LOWAT=` there.

## 0003-SUNRPC-Optionally-raise-SO_RCVLOWAT-while-a-TCP-reco.patch

From `d3da9e964208` on `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`, the
36th commit of the code group. It adds `sunrpc.svc_tcp_rx_lowat`
(default 0, the mark left alone): while a record is incomplete and the
read drained the socket, the socket's receive low-water mark is raised,
through `->set_rcvlowat`, to the smaller of what the record still lacks
and `svc_tcp_rx_lowat`, so that a WRITE arriving in pieces wakes fewer
threads.

**Dropped (Mike, 15 October), with 0002.** The mark saves wakeups and
costs rate. At 131072 it halves the threads woken a WRITE on two ports,
and costs 2 to 3 GB/s there (13 October, build 464) and about 3 GB/s on
build 474 (15 October); the claim in its message, "at the same
throughput", was read from runs the drives were limiting. The rig ran it
at 0; `svc_tcp_rx_lowat=0` is out of `/etc/modprobe.d/sunrpc.conf` on
tardis1 (the file before is `/root/sunrpc.conf.pre-drop-lowat`).

## 0004-veth-add-rx_ip_align-extra-headroom-for-frames-copie.patch

From `3c5f1798714e` on `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`, the
second commit of the code group. It adds `veth.rx_ip_align` (0 to 3,
default 0): extra headroom for the frames veth copies into page-pool
pages for XDP, so that at 2 a TCP payload starts on a 4-byte boundary,
as xeu's own `rx_ip_align=2` places it on the Xsight card. It let the
VM rigs' NVMe/TCP budget stages (`run-budget.sh STORAGE=sgl`) write
loaned WRITEs direct without the card.

**Dropped (Mike, 15 October).** Its own message calls it a test aid,
not a general interface; nothing in the series depends on it, it never
runs on tardis1 ("Run nothing on veth"), and netdev would not take it.
The tree after the drop has `drivers/net/veth.c` as in `v7.3-3` and
differs from the tree before (`.pre-drop-veth`) in nothing else. On a
kernel without it, `run-budget.sh` runs at 0 and says so, and
`transplant/preflight.sh` notes it; both name this patch. To run the
NVMe/TCP stages with loans written direct, apply it to the kernel
first.

## 0005-SUNRPC-Optionally-wait-for-the-rest-of-a-partially-r.patch

From `f2ec94287c3a` on `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`. It
adds `sunrpc.svc_tcp_rx_linger_us` (default 0): when a read leaves a
record incomplete and the socket empty, the thread busy-waits up to that
long (at most 1000 us) for the rest, instead of saving the record on the
socket and giving the transport up, with the `svcsock_tcp_rx_linger`
tracepoint for each wait. It also rewrote the control-record branch of
`svc_tcp_recvfrom()` into an equivalent form, which goes with it.

**Dropped (Mike, 15 October), with 0006.** At the settled setting it ran
at 50 us on one port and 0 on two. On and off by turns on build 474,
one port on eight drives, three runs each with no drive slowed: without
it the threads woken a WRITE tripled (2.5 to 8) but nfsd's CPU was about
7 points less busy (some 3 of 48 CPUs spent busy-waiting with it), and
its rate at 0 was 46.58, 47.07 and 46.12 GB/s against 46.29, 47.69 and
47.57, about 1% under, which three runs do not establish. The account
is "15 October, evening: keep and the linger, each on and off" in
`xeu-rig/README.md`.

## 0006-SUNRPC-Optionally-avoid-waking-a-thread-after-a-shor.patch

From `20c69dd7613f` on `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`. It
adds `sunrpc.svc_tcp_rx_keep` (default N) and
`svc_xprt_received_keep()`: a thread that reads part of a record and
finds the socket empty releases the transport without queueing it again
for data that was already there when it read.

**Dropped (Mike, 15 October), with 0005.** It was pinned on 12 October
together with the SO_RCVLOWAT mark (0003) and never measured apart from
it. On and off by turns on build 474: on two ports it woke about a
quarter fewer threads a WRITE (11.7 to 14.0 against 16.6 to 16.8), but
the context switches a WRITE were within 3%, nfsd's CPU within a point
or two, and the rate the same (58.22 and 57.35 GB/s against 58.16 in
the runs no drive slowed); on one port, where the linger already waits,
it changed nothing.

Dropping both changed nothing else in the code: no KUnit test refers to
either, and the lines the tree lost and gained are exactly the two
patches' reversed. Two conflicts were context only: the hold tracepoint
of "SUNRPC: Learn the region costs of loans and held receive buffers"
sits where the linger tracepoint was, and the KUnit interface commit's
spare helpers where its parameter was. `xeu-rig/settled.sh` holds
`svc_tcp_rx_linger_us=0` and `svc_tcp_rx_keep=N` on kernels that have
them and reports them gone on those that do not, `/etc/modprobe.d/sunrpc.conf`
on tardis1 sets neither (the file before is
`/root/sunrpc.conf.pre-drop-linger-keep`), and `xeu-rig/twoport.sh`
accepts `LINGER=0` and `KEEP=N` on a kernel without them and refuses
anything else there.

`0006-keep-on-c42e910d1151.diff` is 0006 as rebased onto
`c42e910d1151` (its `svcsock.c` hunk had the linger's context), as an
uncommitted diff against that commit: what the worktree
`/var/tmp/keep-tree` on tardis2 held, for the runs of "`svc_tcp_rx_keep`
again, on top of the gating" in `xeu-rig/README.md`. The worktree was
removed on 28 October to free space, and this is what it kept.
