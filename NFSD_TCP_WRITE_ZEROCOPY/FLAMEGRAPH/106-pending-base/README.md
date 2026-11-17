# Round 106: the pending base, profiled against the settled setting

**The question (Mike, 17 November):** round 105's base and the send
bound, as if settled, read 64 GB/s where the settled setting reads 73.
"do a profile run to understand why we've lost so much ground going
from 73GB/s to 64GB/s and what that means the next round is."  The
hope is that the base's placement (one connection to a work CPU, each
flow's frames on that CPU) wins once its bottlenecks are gone.

**The two settings**, on build 534 with `sunrpc.ko`
`AF720978447140137AFC2FE` (the placement fix, the send bound, the
hold-back fixes and the write-space fix):

- **settled**: `settled.sh`, the hash placement (`svc_rtc_balance` N,
  no RFS), `tcp_limit_output_bytes` 4 MiB, the bound 0, the client's
  two-port nconnect (`TWO_NCONN`).  READ passes 72.48 to 73.10 (t2-r830,
  t2-r835).
- **pending**: [`xeu-rig/pending-settled.sh`](../../xeu-rig/pending-settled.sh),
  settled.sh's setting with round 105's base (`svc_rtc_balance` Y, RFS
  32768, `tcp_limit_output_bytes` 256 KiB, nconnect 16 a port) and the
  bound at 512 KiB.  `pending-settled.sh apply` puts the host at it;
  `pending-settled.sh run <command>` runs a pass or a round's script at
  it and settles after.  Round 105's t2-r838 read 64.47 on it; t2-r840,
  the READ pass at the bound alone, 67.75.

`settled.sh` holds the bound at 0 again (Mike: "its fine to keep
settled send bound back to 0").

**The method** is round 105's: profile the READ strobe on each, rank
the costs a READ on the work CPUs (8-55) and the ring CPUs (0-7, 56-63)
with the memory bytes a byte sent beside them, and name what the
pending base pays that the settled setting does not.  That is the next
round's change.

About to run, through `xeu-rig/step.sh /var/tmp/zc-run-logs/r106 NAME
...`: `prof.sh 841 settled`, then `prof.sh 842 pending`, on the same
kernel and module, the files kept between them.

`settled.sh apply` (bound 0) ended in "the settled setting".  Then
`pending-settled.sh audit rest` on that host found exactly the pending
items off, and nothing else: `svc_rtc_balance` N (want Y),
`svc_rtc_send_max` 0 (want 524288), `tcp_limit_output_bytes` 4194304
(want 262144), and every receive queue's RPS and RFS flows and the RFS
table (want nfsd's CPUs, 4096 and 32768).  The profiles started at
07:54.

## The frame: what works with fio and iperf3

Mike, 17 November: "we need to keep framing our progress in contrast to
what worked with fio and iperf3".  Every READ figure in this round, and
in the rounds after it, is set in this table beside round 104's
contrast runs (`104-rx-cpus/contrast.sh`, "What works, beside NFS"):
the devices alone at their ceilings, the two mixed, and NFS.  DMA is
the drives' writes plus the wire's reads (2 × GB/s for NFS); memory and
cache fills are the mesh's (`arm_cmn` `hnf_mc_reqs` and
`hnf_cache_fill`, × 64 bytes, 5 s in the window).

| run | drives GB/s | wire GB/s | DMA GB/s | memory GB/s | memory a byte sent | cache fills GB/s | below TCP a socket | Send-Q a socket |
|---|---|---|---|---|---|---|---|---|
| `contrast-c1`, local fio alone | 104.58 | 0 | 104.6 | 103.9 | | 106.3 | | |
| `contrast-c2`, iperf3 `-Z` alone | 0 | 98.31 | 98.3 | 97.6 | 0.99 | 7.0 | | |
| `contrast-c6`, both, fio capped at 70 | 70.00 | 67.59 | 137.6 | 140.8 | | | | |
| `t2-r815`, NFS at the settled setting | 73.02 | 73.02 | 146.0 | 116.5 | 1.60 | 85.3 | | |
| `t2-r838`, NFS on the pending base | 64.47 | 64.47 | 128.9 | 129.6 | 2.01 | | 0.26 MB | 1.85 MB |
| `t2-r841`, NFS settled, profiled | 68.76 | 68.76 | 137.5 | 99.1 | 1.44 | 82.1 | 0.09 MB | 0.56 MB |
| `t2-r842`, NFS pending, profiled | 64.55 | 64.55 | 129.1 | 129.7 | 2.01 | 74.4 | 0.26 MB | 1.64 MB |

What it says so far: memory's mixed ceiling is about 140 GB/s; NFS at
the settled setting reaches 73 under it at 1.60 bytes a byte sent,
because about 30 GB/s of the NIC's reads come from the cache where the
drive just wrote them (146 GB/s of DMA for 116.5 of memory).  At 1.60,
140 allows about 87 GB/s.  The pending base runs at 2.01, 129.6 GB/s
for 64.47: near the ceiling, with less of each reply read from the
cache.  The reading (not measured yet): its 32 sockets hold some 59 MB
of replies in TCP (1.85 MB each) against a 32 MiB cache, where at the
settled setting the backlog waits at the client.  t2-r841 and t2-r842
fill in both NFS rows on the same kernel and module.

## t2-r841 and t2-r842: the pending base is held by memory, not by CPU

Both on build 534, sunrpc `AF720978447140137AFC2FE`, the files kept,
new kernel warnings 0, settled after each.  **t2-r841, settled: 68.76
GB/s** (the profiler costs some 4 GB/s: the passes read 72.5 to 73.1).
**t2-r842, pending: 64.55 GB/s**, the 32 connections on 32 CPUs.

| | settled (r841) | pending (r842) |
|---|---|---|
| sockets, GB/s each | 24, 2.87 | 32, 2.02 |
| Send-Q a socket, a byte's wait in it | 0.56 MB, 0.195 ms | 1.64 MB, 0.815 ms |
| below TCP a socket | 0.09 MB | 0.26 MB |
| replies held at the server, all sockets | about 16 MB | about 61 MB |
| memory, a byte sent | 99.1 GB/s, 1.44 | 129.7 GB/s, 2.01 |
| cache fills | 82.1 GB/s | 74.4 GB/s |
| drives | 8.3 to 8.9 GB/s each, 100% busy | 7.9 to 8.3, 94% busy |
| cycles a KiB, work CPUs / rings / all | 356 / 197 / 553 | 456 / 234 / 690 |

**Against the frame:** the settled setting holds its replies inside the
32 MiB cache, and the NIC reads them there: 137.5 GB/s of DMA for 99.1
of memory, 1.44 bytes a byte sent, better than round 104's 1.60.  The
pending base holds about 61 MB, nearly twice the cache: 129.1 of DMA
for 129.7 of memory, 2.01 a byte, within some 10 GB/s of the 140 mixed
ceiling (contrast-c6), and fewer cache fills.  It is where fio and
iperf3 mixed sit, not where NFS at its best sits.

**The work CPUs' extra cycles are mostly idle, not work** (each symbol's
share times its run's cycles a KiB): of the work CPUs' +100 cycles a
KiB, `default_idle_call` is +53 and `finish_task_switch` +15.  The rest
is the receive the base moved onto them, as designed (with RFS the ACKs
are processed there: `handle_softirqs` +13, `skb_release_data` +14,
`__list_del_entry_valid_or_report` +12, which is `CONFIG_DEBUG_LIST`,
`skb_splice_from_iter` +8), and what the base was for came off them:
`_raw_spin_unlock_irqrestore` −35 and `queued_spin_lock_slowpath` −17,
the zone and socket locks.  On the rings: `skb_release_data` −18 and
the unlock −26 off, transmit work (`handle_softirqs` +14, the qdisc
and `tcp_write_xmit`) on.  Neither side is saturated (drives at 94%).

**So the 9 GB/s lost from 73 to 64 is memory: the base holds four times
the replies the settled setting does, past the cache.**  The bound at
512 KiB caps what a connection queues before it takes another call, but
the Send-Q still averages 1.64 MB: READs already admitted (the work
items' async reads in flight) land their replies after the bound is
reached.  What the base needs is a bound on what the server holds in
all, under the cache: about 32 MiB over 32 sockets is 1 MiB each,
Send-Q and below TCP and the replies on their way together.

**The next round, from this:** bound what a connection has promised,
not only what it has queued: count each admitted READ's reply against
the bound from its admission, so that queued plus in flight stays under
it.  Before the code, an A/B on the pending base to show the lever:
the bound at 256 and 128 KiB, and nconnect 12 (24 sockets, as settled),
each set beside t2-r842 in the frame, memory a byte first.

## The A/B: what the server holds

Mike: "yes, run the A/B".  [ab.sh](ab.sh) runs the pending base's strobe
with the bound and nconnect given, and prints each run as a row of the
frame with the replies held at the server in all.  About to run,
through `xeu-rig/step.sh /var/tmp/zc-run-logs/r106 ab ...`: `ab.sh
843:524288:16 844:262144:16 845:131072:16 846:524288:12`: the pending
base again (unprofiled), the bound at 256 and 128 KiB, and nconnect 12
(24 sockets, as settled), the last run last since a different
nconnect may fill the files again.

**The A/B: what the whole server holds decides it, not a connection's
bound.**  New kernel warnings 0 in each.

| run | bound | nconnect | GB/s | DMA | memory | a byte | cache fills | below TCP | Send-Q | sockets | held | cycles a KiB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| t2-r841 (settled, profiled) | 0 | settled | 68.76 | 137.5 | 99.1 | 1.44 | 82.1 | 0.09 MB | 0.56 MB | 24 | ~16 MB | 356 / 197 / 553 |
| t2-r843 | 512 KiB | 16 | 64.70 | 129.4 | 129.7 | 2.00 | 74.0 | 0.25 MB | 1.69 MB | 32 | 62 MB | 451 / 230 / 681 |
| t2-r844 | 256 KiB | 16 | 66.32 | 132.6 | 127.6 | 1.92 | 75.6 | 0.23 MB | 1.36 MB | 32 | 51 MB | 436 / 221 / 657 |
| t2-r845 | 128 KiB | 16 | 66.89 | 133.8 | 126.7 | 1.89 | 76.0 | 0.23 MB | 1.44 MB | 32 | 53 MB | 430 / 216 / 647 |
| t2-r846 | 512 KiB | 12 | 69.08 | 138.2 | 125.4 | 1.82 | 78.4 | 0.22 MB | 1.42 MB | 24 | 39 MB | 409 / 193 / 601 |

- **The bound on what is queued has done what it can.**  From 512 to
  256 KiB the Send-Q fell to 1.36 MB a socket and the rate rose 2.5%;
  at 128 KiB the Send-Q did not fall (1.44 MB, eleven times the
  bound).  What a socket holds past the bound is the replies of READs
  admitted before it was reached.
- **Fewer sockets hold less in all, and that is what reads faster.**
  nconnect 12 at 512 KiB holds 39 MB against 62 at 16, reads 69.08
  (best on the pending base, above the settled profile's 68.76), at
  1.82 bytes a byte, the most cache fills (78.4) and the fewest cycles
  (601).  Each socket holds about the same (1.42 MB): the total is the
  lever.
- Against the frame, the settled setting is still the mark for memory:
  16 MB held, 1.44 bytes a byte.  No pending run has come within the
  32 MiB cache yet.

**So the next change bounds what the service holds in all, under the
cache, counting each READ from its admission:** a reply's bytes are
promised when the READ is admitted and counted until TCP has them
acknowledged, against one budget for the service (the cache's 32 MiB,
less what the receive side holds), with a connection's share of it so
that no connection starves.  The per-connection bound counts only what
is queued and so cannot see the promises.
