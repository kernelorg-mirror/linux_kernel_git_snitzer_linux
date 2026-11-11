# Round 94: where the region's held pages sit on tardis2

**The ask (Mike, 11 November):** measure what round 93 left open: of
the 56 MiB held at the median (1.76 times the L3), how much is in
records being received on their connections, in the sockets' queues,
and waiting on the drives.  Which lever to pull depends on that split.

## What is known before it

- Round 93 (`t2-r479..r484`, the settled setting, 71.66 GB/s): 3,595
  pages held at the median, a page held 0.82 ms, 1.69 bytes through
  memory a byte of payload.
- Round 56 (build 506, 16 connections a port, `pagelife.bt`, one page
  in eight followed through its cycle) put about a third of the pages
  in a record's assembly, a fifth to a quarter on the ring and in the
  socket, a tenth at the drive, and the rest on the way back.  Its
  trace slowed the server 15 to 20%, and its probes
  (`svc_tcp_rx_put_run`, `page_pool_alloc_pages`' hand-out to the
  driver) are gone from or changed in this kernel.

## The method

Per record, not per page, so the trace is light:
[heldsplit.bt](heldsplit.bt) follows each loaned record by its
transport and sequence on the `svcsock_tcp_rx_lifetime` tracepoint
(publish, payload-release, release), the bytes nfsd reads off the
socket into it (`svc_tcp_rx_walk_skb` on a read's root skb), and its
`nfsd_direct_write()` (a kprobe: the function has no BTF entry), and
sums each stage's page-microseconds: assembly (each byte from its read
to the publish), publish to the WRITE, the drive, the WRITE's return to
the release, and records with no direct write.  Over the time traced
each sum is the stage's mean pages held.  `rtc_write` is off, so the
WRITE runs on the nfsd thread and its time in `nfsd_direct_write()` is
the drive's.

Beside it [sockq.py](sockq.py) samples every 50 ms the bytes charged to
nfsd's connections' receive queues (`ss`'s `skmem` r), and the rings'
posted buffers are taken as full (512 pages).
[heldsplit-read.py](heldsplit-read.py) sets them all against the mean
held over the trace's span (`regionsample.py`); what is left is a
page's way back after its release (an skb still holding its other
buffer, a pool's stack or alloc cache) and anything unseen.

[run485.sh](run485.sh): three two-port runs on the settled setting from
`t2-r485`, `WARM=10 RUNTIME=40`, the held mark on, the trace and the
sampler from the window's start.  The trace's cost is read from the
rate against round 93's 71.66 GB/s.

## What it found (t2-r485 to r487)

[run485.out](run485.out).  Every run at 0 ring drops, flows placed
12/12, no drive stepped down, no new kernel warning; the host was left
at the settled setting.  The trace costs about 1%: 70.36, 71.58 and
70.66 GB/s against round 93's 71.66 untraced.

The three runs are not alike as traces.  `t2-r485` filled the maps at
65,536 keys partway (24,623 failed updates), so about 16% of its
records' stages are missing; `t2-r486` (1M keys) took 8 s to attach and
traced 6.2 s; `t2-r487` (256K keys) traced 10.7 s with nothing lost.
They agree all the same.  Pages, as means over the trace:

| | t2-r485 (lossy) | t2-r486 | t2-r487 | MiB (r487) | share (r487) |
|---|---|---|---|---|---|
| The rings' posted buffers (taken as full) | 512 | 512 | 512 | 8.0 | 16% |
| The sockets' receive queues | 94 | 91 | 99 | 1.6 | 3% |
| 1 A record's assembly (read off the socket .. published) | 650 | 593 | 691 | 10.8 | 21% |
| 2 Published .. `nfsd_direct_write()` | 56 | 56 | 63 | 1.0 | 2% |
| **3 In `nfsd_direct_write()` (XFS and the drive)** | **1,270** | **1,273** | **1,391** | **21.7** | **43%** |
| 4 The WRITE's return .. the release | 41 | 41 | 46 | 0.7 | 1% |
| No direct write | 1 | 1 | 0 | 0 | 0% |
| The rest: the way back after the release, and unseen | 950 | 557 | 414 | 6.5 | 13% |
| Held, the mean over the trace (`regionsample.py`) | 3,574 | 3,124 | 3,217 | 50.3 | |

- **The WRITE is the largest holder, about 20 to 22 MiB, 40% of the
  pages held**, more than assembly, the rings and the sockets together.
  A loaned record (1 MiB, about 68 pages) is in
  `nfsd_direct_write()` 358 us on average in `t2-r487` (median 256 to
  512 us; 1% over 1 ms, a few to 16 ms).  By Little's law that is the
  rate times the time: 72 GB/s for 0.36 ms is 26 MB, about 3 WRITEs
  outstanding a drive.  The time is the whole call (XFS's direct write
  and the drive); how much is the drive's own was not split.
- **Assembly is about 10 MiB, a fifth.**  24 connections, each part way
  through a 1 MiB record, at about half a record each would be 816
  pages; 600 to 690 are measured.  It follows the connections and the
  record size, as round 56 found.
- **The rings' posted buffers are 8 MiB, a sixth**, fixed by the rings
  (two ports, four rings of 128).
- **Small:** the sockets' queues (1.5 MiB: nfsd reads a socket's bytes
  soon after they arrive), publish to the WRITE (16 to 18 us, 1 MiB) and
  the WRITE's return to the release (0.7 MiB).
- **The rest, 6.5 to 15 MiB, is a remainder,** the held mean less every
  measured stage: a page's way back after its release (the skb still
  holding its other buffer, a pool's stack or alloc cache before it is
  posted) and anything the trace did not see.  It is the noisiest row
  because it carries every other row's error, and in `t2-r485` the
  missing records' stages.

**Against round 56** (build 506, 16 connections a port at 512 KiB,
rings of 256, traced 65 GB/s; `pagelife.bt` counted some free pages on
the way back as in use, see its run155): the drive held 549 pages at
203 us then, 13%; now 1,270 to 1,390 at 360 to 410 us, about 40%.
The record's assembly was the largest holder then (about 900 pages)
and is second now, with 24 connections in place of 32.  What lengthened
the WRITE is not shown here.

**What the trace leaves out, and why it does not change the split.**
76,741 of `t2-r487`'s 690,799 published records had no page-pool bytes
(nothing loaned: not WRITEs) and reach no release on the tracepoint;
they hold nothing.  31,637 keys of bytes read were never published
under their key, each a whole record's 1 MiB: 5% of the bytes read
(676.9 GB read against 644.0 GB published).  Every released WRITE had
its own reads seen, so these look like a record read twice (a first
pass given up and read again under the next sequence), with no abort
traced; at most they add about 5% to assembly.  Not looked into.

**So, for the L3:** of about 50 MiB held, 22 is WRITEs in flight (the
rate times the WRITE's time), 11 is records being assembled (the
connections times half a record), 8 is the rings and 7 to 15 the way
back.  To bring the region near 32 MiB, the WRITE's time and the
assembly are the two levers that are large enough: a shorter WRITE
(fewer outstanding at a drive, if the rate holds) or fewer or smaller
records in assembly (fewer connections, as round 56 showed at a cost in
rate, or a smaller `wsize`).  Neither has been tried here.
