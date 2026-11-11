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

## The WRITE's time, XFS or the drive (t2-r488, t2-r489)

Mike: "split the WRITE's time between XFS and the drive".
[writesplit.bt](writesplit.bt) on each `nfsd_direct_write()` call, on
its nfsd thread: entry to its first bio queued (XFS and iomap before the
I/O), the first bio to its last request completed, that completion to
the return; and each request on the eight data drives issued from such
a call, its bytes times issue to complete (the pages at the drive).
`run485.sh` with `BT=writesplit`, two runs, [run488.out](run488.out);
71.43 and 71.29 GB/s, 0 ring drops, no drive stepped down, no new
kernel warning.

| | t2-r488 pages | t2-r489 pages | MiB | share of the call | mean time |
|---|---|---|---|---|---|
| a Before the I/O (XFS, iomap) | 39 | 37 | 0.6 | 2.5% | 10 us |
| b First bio .. last request done | 1,397 | 1,349 | 21.5 | 91% | 378 us |
| of it at the drive (issue .. complete) | **1,239** | **1,218** | **19.2** | **82%** | 335 to 341 us |
| of it before issue or after its own completion | 158 | 131 | 2.3 | 9% | |
| c Last request done .. the return | 21 | 20 | 0.3 | 1.3% | 6 us |
| Calls with no I/O seen | 59 | 84 | 1.1 | 4 to 6% | 16 to 23 us |
| The whole call | 1,516 | 1,490 | 23.5 | | 410 to 417 us |

- **The drive's own time is 82% of the call; XFS is under 4%.**  XFS
  and iomap take 10 us before the first bio and 6 us after the last
  completion.  Every WRITE is four requests of 256 KiB
  (`max_sectors_kb` 256), and a request is at the drive 335 to 341 us
  on average (median 256 to 512 us, 1% over 1 ms).
- **The rest of the I/O window, 9%,** is a request's bytes before its
  issue (blk-mq, no scheduler) and after its own completion while the
  call's other three finish.
- **The drive's time is its queue.**  A drive writes about 9 GB/s, so a
  256 KiB request's own transfer is about 29 us; at 340 us a request,
  Little's law puts about 12 requests (three WRITEs) at each drive at
  once.  The pages at the drive are the rate times that queue's wait,
  and only a shorter queue at the drive makes them fewer at this rate.
  Whether a drive keeps 9 GB/s with fewer requests outstanding is not
  measured here: local fio at a lower queue depth would say.
- The whole call is 410 to 417 us under this trace against 360 under
  heldsplit.bt; the block tracepoints (about a million events a second)
  lengthen it.  The 25,269 and 34,136 calls with no I/O seen are short
  (16 to 23 us) and hold 1 MiB; 75,042 and 71,826 requests were issued
  outside a WRITE's call, 68,714 of them 4 to 8 KiB (the log's, most
  likely).  Not looked into.

## One drive at lower queue depths (qdprobe.sh)

Mike: "run local fio on one drive at lower queue depths".
[qdprobe.sh](qdprobe.sh): `/hs/nvme0`, locally with no NFS (no client
connected), JOBS threads of fio's psync engine, each one 1 MiB O_DIRECT
write at a time, as nfsd issues them (four 256 KiB requests each); 20 s
a point after 3 s of ramp, an fstrim and 30 s of rest.  All eight
drives are the same model (DapuStor DPRP5), each with 64 hardware
queues of 1,023 requests, so the depth is set by what is issued, not
by the drive.  [qdprobe.out](qdprobe.out):

| 1 MiB writes outstanding | GB/s | a write, mean | p99 | p99.9 | drive busy |
|---|---|---|---|---|---|
| 1 | 8.74 | 108 us | 110 us | 138 us | 79% |
| **2** | **10.20** | **194 us** | 204 us | 281 us | 100% |
| 3 (first, last) | 10.20, 10.20 | 296, 295 us | 309 us | 420, 416 us | 100% |
| 4 | 10.20 | 399 us | 412 us | 553 us | 100% |
| 6 | 10.20 | 601 us | 627 us | 840 us | 100% |
| 8 | 10.20 | 810 us | 856 us | 1,122 us | 100% |
| 12 | 10.20 | 1,222 us | 1,286 us | 1,729 us | 100% |

- **The drive is at its full 10.2 GB/s from two writes outstanding.**
  Past two, every write more adds about 100 us to each and nothing to
  the rate: the time is the queue.  One alone leaves the drive idle a
  fifth of the time between writes, at 8.74 GB/s.
- **Under the two-port load a drive writes 8.6 to 9.0 GB/s with about
  three outstanding** (`t2-r487` to `r489`: 335 to 341 us a 256 KiB
  request at the drive, against 296 us for a whole 1 MiB write here at
  three).  At two, a write would take about 194 us here.
- **What it would save:** the pages at the drive are the rate times a
  write's time there.  At 72 GB/s, about 0.2 ms a write in place of 0.34
  to 0.38 is about 9 to 10 MiB fewer held, which, with nothing else
  changed, takes the 50 MiB toward 40.  Not tried.
- **The margin is thin.**  Two outstanding have about 13% over the 9
  GB/s a drive takes in the two-port runs; one has none.  nfsd's WRITEs
  reach a drive as the connections deliver them, not evenly, so a
  limit of two a drive could leave a drive idle when a burst of WRITEs
  for another drive is ahead of it.  Only a run on the load would show
  whether the rate holds.

## The payload's layout at the drive (sglwrite.c, sglprobe.sh)

Mike asked whether the SGL payload makes the I/O finer than 256 KiB.
It does not: under the load every WRITE call was four requests (589,498
of 589,498 in `t2-r489`) and every request on the data drives from a
call 256 KiB (2,497,648; `max_segments` is 65, `virt_boundary_mask` 0,
`dma_alignment` 3).  The payload's grain is inside each request, as its
segments: one a received frame.  fio writes from page-contiguous
buffers, so its requests are the same size and number with fewer
segments, which qdprobe.sh does not reproduce.

[sglwrite.c](sglwrite.c) writes 1 MiB O_DIRECT as the payload is laid
out (136 iovecs: 7,740 bytes at byte 68 of each 8 KiB receive buffer,
two buffers to a 16 KiB page) or flat, one write a thread as nfsd
issues them.  Checked with `blk_mq_start_request`: every 256 KiB
request carries 34 or 35 segments laid out as the payload, 16 flat.
[sglprobe.sh](sglprobe.sh) on `/hs/nvme0`, each point after an fstrim
and 30 s of rest, [sglprobe.out](sglprobe.out):

| writes outstanding | payload layout: GB/s | a write | flat: GB/s | a write |
|---|---|---|---|---|
| 1 | **6.66** | **157 us** | 9.76 | 107 us |
| 2 (twice for the payload layout) | 10.11, 10.11 | 207 us | 10.20 | 205 us |
| 3 | 10.20 | 308 us | 10.20 | 308 us |
| 4 | 10.17 | 412 us | 10.20 | 411 us |

- **The layout costs a write about 50 us of its own** (157 us against
  107 at one outstanding, 12 or so a 256 KiB request), most likely the
  drive fetching 34 descriptors in place of 16 and moving unaligned
  pieces; it is not separated from the writer's own time, which builds
  the same 136 iovecs either way.
- **From two outstanding the queue hides it:** 10.11 GB/s against 10.20
  (1%), and from three the two are the same.  So two writes still fill
  a drive with the payload's layout, about 12% over the 9 GB/s a drive
  takes in the two-port runs.
- **But one is much worse than it looked:** 6.66 GB/s, not fio's 8.74
  (this writer's flat 9.76; fio's own time between writes made its one
  slower).  A drive that falls to one WRITE outstanding loses a third
  of its rate with this payload, so a limit of two a drive has less
  room under bursts than the flat figures said.
- The 4% between the load's rate while busy (9.7 to 9.8 GB/s) and 10.2
  is not the layout: at three outstanding, as under the load, the
  layout costs nothing.  Where it comes from is not shown.

### At 128 KiB requests (`MAXKB=128`)

Mike: "lower max_sectors_kb to 128 and rerun the sgl probe".
`sglprobe.sh` with `MAXKB=128` (nvme0n1 only, put back to 256 at the
end), [sglprobe-128.out](sglprobe-128.out).  A trace of
`blk_mq_start_request` over the first points saw the 128 KiB requests
with 16 to 19 segments in the payload's layout and 8 to 9 flat.

| writes outstanding | payload layout, 256 KiB | payload layout, 128 KiB | flat, 256 | flat, 128 |
|---|---|---|---|---|
| 1 | 6.66 GB/s, 157 us | 6.90 GB/s, 151 us | 9.76, 107 us | 9.84, 106 us |
| 2 | 10.11, 207 us (twice) | 10.04, 208 us (twice) | 10.20, 205 us | 10.20, 205 us |
| 3 | 10.20, 308 us | 10.17, 309 us | 10.20, 308 us | 10.20, 308 us |
| 4 | 10.17, 412 us | 10.14, 413 us | 10.20, 411 us | 10.20, 411 us |

- **The request size does not change the layout's cost.**  At one write
  outstanding, eight requests of 128 KiB take 151 us against four of
  256 KiB at 157 (6.90 GB/s against 6.66, 4% better); at two to four
  they are 0.3 to 0.7% slower.  Flat, the two sizes are the same.
- So the 50 us a write goes with the payload's 136 pieces, however
  they are cut into requests, not with the number of requests: the
  cost is per segment, not per command.  A smaller `max_sectors_kb` is
  no lever for the pages held at the drive either: at two outstanding
  and more a write's time is the same.

## The loan budget sweep (t2-r490 to r493)

**A full budget pauses, it does not copy.**  In the receive path
(`net/sunrpc/svcsock.c`, at `svc_tcp_rx_credit_admit_to()`), a record
the budget cannot admit at its first skb with region pages is left
unread there: its transport waits, oldest first, and is woken when
credit returns (`rx_loan_admit()`, `net/core/rx_loan.c`; the oldest
record in flight always starts).  Only a budget of 0 copies
(`svc_tcp_rx_budget_copy_from()`, reason `NO_BUDGET`).  The budget's
`denied` counts these pauses; its `paused` stayed 0.  So round 93's
"loans refused" were pauses too.

[run490.sh](run490.sh), one two-port run a budget, heldsplit.bt, the
socket sampler and the mesh beside each, [run490.out](run490.out).
Every run at 0 ring drops, no drive stepped down, no new kernel
warning.  A WRITE's charge is 127 pages, about twice its 64-odd pages.

| budget, WRITEs (pages) | 128 (16,262) | 80 (10,214) | 64 (8,198) | 48 (6,182) |
|---|---|---|---|---|
| Rate, GB/s | 71.79 | 71.13 | 71.41 | 70.30 |
| Admissions paused | 254 | 9,439 | 84,632 | 154,330 |
| Held, median (MiB, L3) | 3,574 (56, 1.75) | 3,535 (55, 1.73) | 3,543 (55, 1.73) | 3,521 (55, 1.72) |
| 99th percentile | 6,909 | 6,925 | 5,926 | 5,250 |
| Kernel peak | 11,511 | 8,674 | 7,362 | 5,923 |
| Bytes through memory a byte | 1.72 | 1.69 | 1.71 | 1.66 |
| At the drive (heldsplit) | 1,499 | 1,454 | 1,482 | 1,358 |
| Assembly | 671 | 679 | 686 | 691 |
| Sockets' queues | 90 | 95 | 97 | 97 |

- **The budget cuts the bursts, not the working set.**  The peak falls
  from 11,511 to 5,923 pages and the 99th percentile from 6,909 to
  5,250, but the median stays at 3,520 to 3,575 pages (1.72 to 1.75
  L3), and so do the bytes through memory (1.66 to 1.72).
- **The drive's share does not move** until 48, and then only by 9%
  (1,358 pages), with the rate 2% down (70.30 GB/s).  Assembly and the
  sockets do not grow: a paused connection's bytes wait on the client.
- **Why:** the budget charges a record from its first payload skb, so
  the records in assembly (about 24, one a connection) draw on it as
  much as those at the drives.  About 52 records are in flight in
  steady state, so 48 is the first budget that binds outside bursts,
  and it binds on both kinds alike.  To bring the drives to two
  outstanding (16 WRITEs), a global budget would have to go to about
  40 and pause assembly too; at 48 the rate is already falling.  A
  bound that counts only the WRITEs at a drive (or that admits a
  record only when its drive has room) is what would shorten the
  drive's queue without starving assembly.

## The design of a per-drive bound

[DESIGN-per-export-window.md](DESIGN-per-export-window.md): a window of
loaned WRITEs in flight for each export, keyed by the file handle's
fsid at admission (an nfsd procedure hook names the target from the
record's first bytes; sunrpc counts and pauses as the budget does),
what it would buy (the drive's queue from three writes to two, 5 to 6
MiB of the 50 held) and what it would not (the assembly's 11 MiB).

## The window built and run (t2-r494 to r496, then r497 and r498)

Mike: "go ahead, build it and run W 0, 5, 4".  The code is two commits
on `kernel-7.3/canary`: `05844df27a1f` (SUNRPC: the targets' table in
the budget, the pause at admission, the wake at the credit's return,
`svc_tcp_rx_target_window`, four fields on the budget's stats line) and
`db48abad7ef5` (NFSD: `nfs3svc_rx_target()`, the NFSv3 WRITE hook that
hashes the handle's fsid, registered as nfsd loads).  The hook is
registered by program (`svc_rx_target_register()`) rather than a field
on `struct svc_procedure`, so no structure lockd or the client share is
relaid out; the two fields added to `struct svc_sock` and the record's
state still give 273 of sunrpc's 440 exports new versions, so the whole
server stack was rebuilt with it (`xeu-rig/build-svcstack.sh`) and
swapped in (`swap-sunrpc.sh STACK=`; the tip's own sunrpc, srcversion
`F99F549CC7AF44FA39DAE34`, is in `$L/sunrpc-522-tip/sunrpc/` if it is
wanted back).  [run494.sh](run494.sh), [run494.out](run494.out).

| window | GB/s | held, median (L3) | at the drive (heldsplit) | assembly | sockets | bytes a byte | pauses |
|---|---|---|---|---|---|---|---|
| 0 (t2-r494) | 70.79 | 3,500 (1.71) | 1,382 | 698 | 100 | 1.68 | 0 |
| 5 (t2-r495) | **51.51** | 2,422 (1.18) | 525 | 411 | 309 | 1.01 | 4,679,224 |
| 4 (t2-r496) | **43.81** | 2,249 (1.10) | 401 | 352 | 355 | 0.86 | 4,421,891 |

The window did what it is for to the region (38 MiB held at 5, the
drive's row down to 8 MiB, one byte through memory a byte) and took
27% and 38% of the rate with it: the drives ran at about one write
outstanding (525 pages at 51.5 GB/s is 0.17 ms a write at the drive),
which is 6.66 GB/s a drive with this payload, 53 for eight.  Two faults
in the first build, both fixed in `9e4eb880bb91` before the rerun:

- **Two drives shared a slot.**  The eight fsids (77010 to 77013,
  77020 to 77023) hashed into 64 slots put nvme2 and nvme6, one on
  each port, in slot 57 (computed with the kernel's jhash in Python),
  so those two shared one window of 5, and every connection on either
  port paused for them held back its records for the other drives.
  The design's "unlikely and harmless" was wrong on both counts: with
  eight keys in 64 slots about one table in three has a pair.  Now a
  key probes from its hash to the slot that holds it or is free and
  claims it; keys share only once the table is full.
- **A paused transport was woken for every segment.**  The budget's
  pause is left alone by `svc_data_ready()` (the credit's return runs
  the transport); the target's pause was not, so each arriving segment
  woke a thread to read 300 bytes and pause again, 4.7 M pauses for
  2.5 M records.  Now a transport paused for a place is left to the
  return that runs it, as a transport waiting for credit is.

### The rerun with both fixed (t2-r497, r498)

[run497.out](run497.out); the stack's sunrpc is
`FC78BAE8A6C2B0A92A1366A`.
Beside them, the budget sweep's nearest point (`t2-r493`, 48 WRITEs)
and the window off (`t2-r494`):

| | off (r494) | window 5 (r497) | window 4 (r498) | budget 48 (r493) |
|---|---|---|---|---|
| Rate, GB/s | 70.79 | **64.79 (−8.5%)** | 60.28 (−15%) | 70.30 (−2%) |
| Held, median (MiB, L3) | 3,500 (55, 1.71) | **2,748 (43, 1.34)** | 2,511 (39, 1.23) | 3,521 (55, 1.72) |
| 99th percentile, peak | 6,998, 11,845 | 2,996, 3,308 | 2,706, 2,989 | 5,250, 5,923 |
| Bytes through memory a byte | 1.68 | **1.24** | 1.10 | 1.66 |
| At the drive (pages) | 1,382 | 760 | 615 | 1,358 |
| Assembly | 698 | 547 | 475 | 691 |
| Sockets' queues | 100 | 196 | 236 | 97 |
| Records paused (of admitted) | 0 | 2.03 M (of 3.09 M) | 2.38 M (of 2.87 M) | 154 k (of 3.35 M) |
| Client retransmits in the window | 28 | 56 | 31 | 41 |

- **The window moves the region as nothing else has:** 55 to 43 MiB
  held at the median at 5, the peak from 11,845 pages to 3,308, the
  bytes through memory from 1.68 to 1.24 a byte (80 GB/s to the DIMMs
  in place of 118).  The drive's row falls from 21.6 to 11.9 MiB, the
  assembly's from 10.9 to 8.6, while the sockets' queues double (a
  paused connection's bytes wait in its socket, about 3 MiB).
- **And it costs rate: 8.5% at 5, 15% at 4.**  The drives ran under
  two outstanding: 760 pages at 64.8 GB/s is 0.19 ms a write at the
  drive, about 1.5 writes, where two fill a drive.  The design's sizing
  (5 in flight a drive leaves 2.4 at the drive) assumed the steady mean,
  4 to 6 in flight a drive; the client's 16 jobs at a depth of 16 send
  each drive's WRITEs in bursts, so a drive's count swings well past
  the mean and back, and at the window the burst waits on the client
  while the drive, 0.3 ms of assembly away from the next record, runs
  dry.  Two of every three records paused at 5.
- **Against the global budget:** 48 WRITEs (six a drive) cost 2% and
  saved nothing of the median; the window at 5 saves 12 MiB and costs
  8.5%.  The window is the stronger lever on the footprint and the
  dearer one in rate; neither setting is free.
- **Not run:** windows of 6 and 7, where the drive would stay above two
  under the bursts and the saving would be smaller; one run each would
  place the knee.  Nor the client's side of the same lever: fewer,
  larger bursts (`iodepth`) or a connection set a drive, which would
  end the head-of-line pauses.  No ring drops, no drive stepped down,
  no kernel warning in any run; the host is left at the settled setting
  with the window at 0 and the stack from `9e4eb880bb91` loaded.
