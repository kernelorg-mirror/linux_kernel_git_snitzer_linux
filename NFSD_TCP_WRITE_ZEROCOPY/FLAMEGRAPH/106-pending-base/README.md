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
