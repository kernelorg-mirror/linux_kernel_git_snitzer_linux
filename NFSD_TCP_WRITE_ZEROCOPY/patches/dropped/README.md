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

## 0007 to 0009: the expkey reference in `exp_find()` (2 November, Mike)

`0007` adds `sunrpc_cache_peek_rcu()`, `0008` uses it for a new `exp_find_rcu()`, and `0009` is its KUnit test. They apply in that order on `c86e11886f2b`; `0007` and `0008` were `b91623167b04` and `1ffe1b8da005`, and the branch reverts them at the tip. They are in the history under the reverts until a regroup folds them away.

They worked: every way out of the fast path ran on tardis2 (a flush, a key due for refresh, an unexport on a live mount), and `exp_find_key` left the hot path. But they saved about 0.11 us a WRITE against 0.35 estimated, too little for an A/B to see, so they were not kept. The account is `FLAMEGRAPH/54-fh-verify/README.md`.

They would be worth another look on a load where a lookup costs more, for instance many more CPUs on one export. `svc_export`'s own reference is still written on every request. `build-nfsd.sh` and a KUnit module built outside the tree need `KBUILD_EXTRA_SYMBOLS` for the new export.

## 0010 to 0012: rounds 72 and 73 on the pool's wakeup (8 November, Mike)

`0010` adds `svc_thread_chosen_to_stop()`, `0011` uses it in nfsd()'s
loop, and `0012` has `svc_pool_hand_idle_thread()` take the reference a
handed transport's thread holds.  They apply in that order on
`6887c0124e6c`, whose code is the branch's since; on the branch before
it was consolidated they were `fc5465db0a78`, `8f395d70e9c6` and
`51cbfc0196e3`, each reverted at the tip, and the commits and their
reverts are kept on `kernel-7.3/canary.pre-drop-reverted`.

They did what they said, but each only moved a cache miss: `0010` and
`0011` took nfsd's loop off the pool's flags, and the miss went to
`svc_pool_can_shrink()` in `svc_recv()` (round 72); `0012` covers only
the transports handed to a thread, while most are queued and pay the
same miss in `svc_xprt_dequeue()`'s reference (round 73).  Neither
saved cycles a WRITE at matched rates.  The accounts are
`FLAMEGRAPH/72-nfsd-loop/README.md` and `FLAMEGRAPH/73-handoff-ref/README.md`.

`0010` and `0011` were written for round 71's layout, with `sp_flags`
on the queue's line, which is dropped too (folded away in the
consolidation, see `FLAMEGRAPH/74-b-vs-pf/README.md`); with `sp_flags`
off that line they have less to save.  `0012` would matter if more
transports were handed than queued.

`0013-SUNRPC-Count-a-loaned-skb-s-region-pages-in-the-walk.patch`
(FLAMEGRAPH round 78, 9 November, dropped by Mike) counts the receive
region's pages of a loaned skb in the walk that prefetches their struct
pages, instead of in a second walk.  It applies on `kernel-7.3/canary`
at `d25f3bc3a935` and is kept as `f9eb67570da8` on the unpushed
`kernel-7.3/canary.PC`.  For the A/B it is sunrpc's own copy of the
walk; kept, it would have gone into `net/core/rx_loan.c`.  It took the
second loop away, but the count then read each page one fragment after
asking for it and waited for it: the two passes went from 5.9k cycles a
WRITE to 8.3k (`FLAMEGRAPH/78-prefetch-count/README.md`).

`0014-SUNRPC-Let-a-thread-stay-with-a-loaned-record-for-it.patch`
(FLAMEGRAPH rounds 81 to 85, 9 November, dropped by Mike) lets the
thread that read a piece of an incomplete loaned record keep it and
sleep until `svc_data_ready()` wakes it for the rest, behind
`sunrpc.svc_tcp_rx_stay_us` (0, off, by default).  It is `02a9b93793f7`
on the unpushed `kernel-7.3/canary.ST`, on `3bdeea5a21cc`.  It adds a
field to `struct svc_sock`, which changes the versions of 273 sunrpc
exports, so it was tried as a whole server stack
(`xeu-rig/build-svcstack.sh`).  It did what it said: a piece is read in
3.4 us against 7.8, and threads woken fall from about 291k to 84k a
second.  But it was lower in all six pairs, by 0.1 to 3.2 GB/s, at the
same cycles a WRITE, and no cause was found in the wait for a piece, the
drives, the block layer or the SGLs (`FLAMEGRAPH/82-thread-stay/` to
`85-write-tail/`).

`0015-SUNRPC-Optionally-run-a-connection-s-completions-and.patch` to
`0018-SUNRPC-test-a-split-connection-s-handoff-to-its-send.patch`
(FLAMEGRAPH round 104, 16 November, dropped by Mike) are
`sunrpc.svc_rtc_tx_split`: a second work item a connection, on a CPU
of its own, for its completions and its send FIFO, so that the first
only receives and submits (`0015`, `008493ad5d9a6`); the receive pass's
ownership of the rqst it is still in (`RQ_RTC_RXOWN`), without which
the send work item released a call under the pass and hung tardis2
(`0016`, `254cead34a672`); its KUnit interface (`0017`) and cases
(`0018`).  They applied on `kernel-7.3/canary` after `fe25e474fa3de`.
Two teardown fixes made for it stay in the tree, as they hold without
it: a live send batch releases the rest of its replies once the
connection stops, and `svc_rtc_unregister()` reports a wait that does
not end.  With the fixes the split was safe (twelve connections on one
port, nfsd stopped under load, two ports), but on the two-port READ
load it read 61.94 and 57.68 GB/s against 71.52 and 70.51 unsplit: every
stage of a READ cost more on two CPUs than on one, 103 us a READ on
the work items' CPUs against 57, and by Little's law the READ's
latency rose 1.7 ms (`FLAMEGRAPH/104-rx-cpus/README.md`, t2-r766 to
r769).

`0019-SUNRPC-Let-a-run-to-completion-pass-receive-more-tha.patch`
(FLAMEGRAPH round 104, 16 November, dropped by Mike) lets a
connection's rtc work item take up to `sunrpc.svc_rtc_recv_budget` (8)
calls a pass instead of one.  It was `dac0fc832ab18` on
`kernel-7.3/canary`, reverted at the tip.  The calls waiting in each
server socket were taken to be waiting for the next pass; they were
not: 35 a socket at 8 against 31 at 1.  The passes went from 2.7 M to
0.3 M and sent their replies eight at a time, which quadrupled the
round trip on the wire (0.17 to 0.67 ms) and tripled the replies
waiting to be sent: the two-port READ strobe read 61.82 and 62.03 GB/s
at 8 against 71.05 at 1 (t2-r775 to r777).

`0020-SUNRPC-Recycle-a-TCP-reply-s-pages-on-the-CPU-that-s.patch` to
`0024-SUNRPC-Let-consecutive-recycled-replies-share-skbs.patch`
(FLAMEGRAPH round 104, 16 and 17 November, dropped by Mike) are
`sunrpc.svc_tcp_reply_recycle`: a large TCP reply sent zero copy under
a descriptor holding the rqst's reply pages, its completion giving them
back to the sending CPU's cache for `svc_alloc_arg()` (`0020`,
`80e7a8cafad0a`), its KUnit interface and suite and the suite's config
(`0021` to `0023`), and a `link_skb` so consecutive replies share skbs
(`0024`, `c7fefbfe46f2a`).  They applied on `kernel-7.3/canary` after
`a7e5a1b76d046`'s tree (with David Flynn's READ-confinement set); the
build-534 port is `FLAMEGRAPH/104-rx-cpus/reply-recycle-534.diff`.  It
removed the zone-lock contention it was written for (the rtc CPUs'
spinning from 9% of their samples to 1.3%) and reused 96 to 97% of the
pages, but with recycled pages every NIC transmit queue sat at its byte
limit, the sockets' data backed up below TCP, TSQ stopped them and the
work items slept in `sendmsg()`: the two-port READ read 62 to 65 GB/s at
512 pages a CPU against 70 to 74 without it (t2-r781 to r810).  At 1
page a CPU (the zero-copy send with every page freed as before) nothing
backed up (t2-r809), so the recycled pages, not the send, were the
cause; why the NIC drains recycled pages more slowly was not found.
`0024`'s message names the small tail skbs as the cause: the chain
removed those (t2-r793) and the backup stayed, so that message is wrong.
