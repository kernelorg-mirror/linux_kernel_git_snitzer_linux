# Tracing NFSD and xeu: where the time and the memory go on receive

Started 2026-10-13 on tardis1 (aarch64 Neoverse-N2, 64 CPUs, one
NUMA node, 32 MiB shared L3, 16 KiB pages, build 464), during the
day's deep dive on the two-port rate. Two ports of NFS in copy mode
carry 60 to 61 GB/s and nothing that adds parallelism around the
copy has moved that (`XEU-4-VS-8-RING.md`: eight rings in four
layouts, copy threads, the wakeup mark). This document is the trace
of the receive path at that rate: what the rings' CPUs and nfsd's
CPUs each spend in cycles and in fetches from memory, what the copy
itself costs against a bare copy, and what that leaves to gain. Work
on what is left to gain is added below as it is done.

**In short.** The two ring layouts are the same machine at work.
The copy is 39% of the cycles on nfsd's CPUs and 61% of their
fetches from memory, and it fetches 1.20 lines for each line of
payload, as a bare copy in frame-sized pieces does. Nothing else on
the path is large: the next item is the scheduler at 0.16 lines a
line. Two ports carry what the bare piecewise copy carries (60.1
GB/s), so the copy is the whole of the limit, and trimming the rest
would save CPU and not raise the rate. What is left inside copy mode
is the 4% between copying in pieces and copying whole.

Related: `TARDIS1_COPY_PERFORMANCE.md` (the copy benchmark of 12
October, and that a copy which does not fetch its destination gains
nothing here), `XEU-4-VS-8-RING.md` (the layouts and every run
named here), `XEU_CONVERGENCE.md` (the dated accounts).

## How the trace is taken

`PROFILE=1` to `xeu-rig/twoport.sh`, with `RUNTIME=55` at least. Over
the middle of a run:

1. `perf stat` for 8 s on the rings' CPUs (0-7, 56-63) and on
   nfsd's (8-55) apart: cycles, instructions, `l3d_cache_lmiss_rd`
   (a line fetched from beyond the L3), `bus_access_rd`,
   `bus_access_wr`, `l2d_cache_refill`.
2. 8 s of cycles sampled at 499 Hz on every CPU, with call graphs.
3. 6 s sampled on `l3d_cache_lmiss_rd`, one sample in 20000, with
   call graphs.

Each recording is reported by symbol for all CPUs and for the two
groups (`perf-sym*.txt`, `perf-lmiss-*.txt`), by task
(`perf-comm.txt`) and with callers (`perf-callers.txt`,
`perf-lmiss-callers.txt`), in the run's directory under
`/var/tmp/zc-run-logs/`. `KEEP_PERF=1` keeps `perf.data` and
`perf-lmiss.data`. `$L/prof-driver.sh` ran the four runs below:

    TAG=<tag> QDISC=fq LINGER=0 LOWAT=0 RCVBUF=393216 WARM=15 RUNTIME=55 \
        PROFILE=1 KEEP_PERF=1 ENTRIES=128 \
        ./twoport.sh $L/xeu-16k-464-<tag> ab

The setting is the pinned two-port one (384 KiB held buffer, no
linger, mark off, `fq` on the client (it was unpinned later that
day; the default `fq_codel` carries the same), 16 connections a port placed
evenly on the rings), with four rings of 128 a port or eight of 64
(`RINGS=8 ENTRIES=64 TXIRQ_A=8 TXIRQ_B=54`).

## The trace (13 October, 13:15 to 13:30)

Runs of 55 s counted, each layout twice: `prof-4x128-1` (51.69
GB/s; port a went silent from second 56, which is in
`XEU-4-VS-8-RING.md`; its counters and most of its cycles were taken
before that), `prof-8x64-1` (59.58), `prof-4x128-2` (59.89),
`prof-8x64-2` (60.23). The profiler cost no rate. Each drive wrote
628 to 654 GB in these, just past the fast region (the placement
load before the warm-up was not counted in sizing them); none
stepped down. With `WARM=10` they would fit.

**Sampling caveat.** This kernel has no pseudo-NMI, so a sample due
while interrupts are off is taken where they come back on. The
large `_raw_spin_unlock_irqrestore` on the rings' CPUs is mostly
`enable_irq()` at the end of `xeu_rx_poll()`, and
`finish_task_switch` and `default_idle_call` collect the same way.
Read those three as "somewhere with interrupts off before here".

### Counters, for each line of payload

A line is 64 bytes; 60 GB/s is 0.94e9 lines a second.

| | 4 x 128 | 8 x 64 | 8 x 64 | `copybench stp`, 48 threads |
|---|---|---|---|---|
| run | `prof-4x128-2` | `prof-8x64-1` | `prof-8x64-2` | no NFS |
| GB/s | 59.89 | 59.58 | 60.23 | 63.6 |
| lines fetched from memory, rings' CPUs | 0.12 | 0.11 | 0.11 | |
| lines fetched from memory, nfsd's CPUs | 1.97 | 2.01 | 1.95 | 1.01 |
| bus reads a second, all CPUs | 3.73e9 | 3.77e9 | 3.69e9 | 2.02e9 |
| bus writes a second, all CPUs | 2.11e9 | 2.10e9 | 2.13e9 | 2.08e9 |
| cycles a second, rings' 16 CPUs | 17.7e9 | 21.0e9 | 21.4e9 | |
| cycles a second, nfsd's 48 CPUs | 52.9e9 | 55.5e9 | 58.4e9 | |
| instructions a cycle, rings / nfsd | 1.28 / 0.54 | 1.11 / 0.56 | 1.10 / 0.54 | |

- **The two layouts are the same machine at work**: every figure for
  nfsd's CPUs is within 3%. Eight rings spend a fifth more cycles on
  the rings' CPUs for the same data and nothing else changes.
- **The rings' CPUs hardly touch memory**: 5% of the fetches. The
  driver and TCP receive handle headers; the payload is first read
  by the copy.
- **nfsd's CPUs run at 0.54 instructions a cycle**: they wait on
  memory.

### Where the cycles go (`prof-4x128-2`; eight rings within a point or two)

Of all cycles, 62% are in nfsd's threads and 37% in the idle task
(which is where softirq work on the rings' CPUs is counted).

| nfsd's CPUs | | rings' CPUs | |
|---|---|---|---|
| `__memmove` | 39.1% | `_raw_spin_unlock_irqrestore` (see the caveat) | 22.3% |
| `default_idle_call` | 11.0% | `default_idle_call` | 10.0% |
| `finish_task_switch` | 5.5% | `handle_softirqs` | 5.9% |
| `_raw_spin_unlock_irqrestore` | 1.9% | `eth_type_trans` | 4.3% |
| `skb_release_data` | 1.6% | `xeu_rx_poll` | 4.1% |
| `_raw_spin_lock` | 1.6% | `napi_pp_put_page` | 3.5% |
| `check_heap_object` | 1.3% | `tcp_gro_receive` | 2.8% |
| `flush_dcache_page` | 1.0% | `skb_defer_free_flush` | 2.7% |
| nothing else over 0.75% | | `xeu_tx_poll`, `svc_data_ready` | 2.5%, 2.4% |

### Where the fetches from memory are made, nfsd's CPUs (`prof-4x128-2`)

1.97 lines fetched for each line of payload, over 1010 symbols:

| what the code is doing | share | lines fetched per line of payload |
|---|---|---|
| the copy, `__memmove` under `svc_tcp_rx_copy_skb` | 61.0% | 1.20 |
| scheduler: switch, idle, wake | 8.4% | 0.16 |
| TCP and IP, receive and send, in nfsd's context | 6.4% | 0.13 |
| sunrpc and nfsd | 6.2% | 0.12 |
| locks, and samples deferred to an interrupt restore | 3.8% | 0.08 |
| the write: VFS, XFS, iomap, block, NVMe | 2.9% | 0.06 |
| usercopy checks and the iterator's copy glue | 2.7% | 0.05 |
| freeing and recycling skbs and pages | 2.2% | 0.04 |
| everything else, no symbol over 0.5% | 6.4% | 0.13 |

`prof-8x64-1` has the copy at 60.0% the same way. (`prof-8x64-2`'s
fetch sampling started after its load had ended and is of an idle
machine: the cycles recording took too long to write out. Not used.)

- **The copy fetches 1.20 lines for each line it copies**, which is
  what `copybench` does copying in 7,740-byte pieces 68 bytes into
  a buffer (1.12). The payload is read from memory once: the card
  writes it there and it is not in the cache when the copy comes for
  it, whatever the size of the receive region.
- **The other 0.77 (and the rings' 0.12) have no large item.** The
  biggest group is the scheduler at 0.16, from 780 thousand wakeups
  a second. There is no single function to fix.

### What the limit is: the benchmark with stores alone and loads alone

`copybench` gained two methods for this, `wr` (lines stored and
nothing loaded) and `rd` (lines loaded and nothing stored), 4 MiB
buffers, CPUs from 8:

| method | threads | GB/s | lines fetched a second | bus reads a second | bus writes a second |
|---|---|---|---|---|---|
| `stp` (copy) | 8 | 66.0 | 1.04e9 | 2.08e9 | 2.22e9 |
| `stp` | 16 | 68.2 | 1.07e9 | 2.14e9 | 2.18e9 |
| `stp` | 48 | 63.6 | 1.01e9 | 2.02e9 | 2.08e9 |
| `wr` | 8 | 105.0 | | | 3.28e9 |
| `wr` | 16 | 138.6 | | | 4.34e9 |
| `wr` | 48 | 126.3 | | | 4.00e9 |
| `rd` | 8 | 105.4 | 1.66e9 | 3.31e9 | 0.31e9 |
| `rd` | 16 | 164.1 | 2.57e9 | 5.14e9 | 0.26e9 |
| `rd` | 48 | 161.9 | 2.54e9 | 5.07e9 | 0.23e9 |

- **Writes to memory are not capped at the 2.1e9 a second that
  every NFS run and the saturated copy show**: stores alone reach
  4.0 to 4.3e9. Loads alone reach 5.1e9. So neither direction is at
  its own limit in the copy; it is the two together. A copy moves
  128 to 136 GB/s of memory traffic, which is what stores alone
  manage (126 to 139).
- **NFS at 60 GB/s moves more than that**: 1.95e9 lines read and
  about 1.05e9 written a second is 192 GB/s, above any figure in
  this table. Either some of what the counter calls a fetch from
  beyond the L3 is a line brought from another CPU's cache and not
  from memory (nfsd's threads share a good deal across 48 CPUs), or
  the traffic limit is not one number. Not resolved, and it decides
  what follows.
- **What can be said:** two ports of NFS carry what the bare
  piecewise copy carries (60.1 GB/s), while fetching 0.89 lines
  more for each line of payload than that copy does. If those extra
  fetches were costing rate, NFS would be under the benchmark, and
  it is not. So on this evidence removing them (fewer wakeups, less
  shared state) would save CPU and not raise the rate, and the copy
  itself is the whole of the limit: one read and one write of every
  payload byte through memory. Within copy mode what is left is the
  4% between copying in pieces and copying whole (62.5 against
  60.1). Past that it is not copying; there is no more memory
  bandwidth to be had (the DIMMs are fully populated, next section).

## The memory itself: four DIMMs, 166 GB/s, and NFS at what a saturated copy asks of it (13 October, evening)

**The DIMMs are fully populated (Mike): four at 5200 MT/s, a
theoretical 166 GB/s.** That is 2.6e9 lines a second. No channel is
empty, so there is no memory to add: the question left open above
and in `TARDIS1_COPY_PERFORMANCE.md` is closed.

The mesh has a counter for it. `arm_cmn/hnf_mc_reqs/` counts the
requests the cache's home nodes make of the memory controller,
which the cores' own counters cannot tell from a line brought from
elsewhere in the cache. `copybench`, 16 threads from CPU 8, 4 MiB
buffers, and a two-port run at the pinned setting (`cmn-two-1`,
60.38 GB/s, 10 s mid-run, no drive stepped down):

| | GB/s | requests to memory a second | L3 misses at the home nodes | lines the cores count fetched from beyond the L3 |
|---|---|---|---|---|
| `rd`, loads alone | 177.6 | 2.03e9 | 2.57e9 | 2.62e9 |
| `wr`, stores alone | 152.4 | 2.49e9 | 2.07e9 | 0 |
| `stp`, a copy | 73.7 | 1.91e9 | 2.28e9 | 1.14e9 |
| NFS, two ports | 60.4 | 2.05e9 | 2.02e9 | 1.77e9 |

- **NFS at 60 GB/s asks of memory what the saturated copy does**:
  2.05e9 requests a second against 1.91e9, and 1.9 to 2.5e9 is all
  any method here reaches. If a request is a line, that is 131
  GB/s, four fifths of what the DIMMs can do in theory.
- **That settles the 192 GB/s that did not add up.** A third of
  what the cores count as fetched from beyond the L3, and as
  written to the bus, never reaches memory: it is served within the
  cache. NFS makes twice the L3 accesses of the bare copy (5.9e9 a
  second against 2.8e9).
- So the limit on two ports is memory bandwidth, the DIMMs are all
  there, and a copy spends it twice over for each byte of payload.
  A copy cannot pass 83 GB/s on this machine in theory and reaches
  62 to 68 in practice.

Not checked: exactly what the counter takes for one request, and
whether the card's writes into memory and the drives' reads from it
are among them. The benchmark rows are with 4 MiB buffers, part of
which stay in cache (which is how `rd` shows more than 166 GB/s), so
they are a little under what larger buffers would ask.

**What it means for the loan** was put here as an inference: with
no copy a payload byte still enters memory once from the card and
leaves once to a drive, and if both are requests to memory a loan is
bounded near 83 GB/s as well. **Measured the same evening, that is
wrong.** The card writes into the cache and a drive takes from it;
memory is asked only for what falls out in between, about 0.1
requests a line where a copy asks 2.17. `LOAN-PERFORMANCE.md`.

## The 4% between copying in pieces and copying whole (13 October, 14:00 to 14:30)

Where it goes was found with the benchmark, then checked against
the real path with a tracer on the copy. No change to the server has
been made or measured for it yet.

### The benchmark: it is the order the buffers are copied in, and the unused end of each

`copybench` gained `RUN=<n>` for this: with `PIECE`, the source
buffers are taken in runs of `<n>` neighbours and the runs in random
order, as buffers cut from one page are neighbours in memory and
pages are not. `libc` copy, 48 threads from CPU 8, 5 s, 64 MiB of
8,192-byte buffers a thread, 1 MiB copied at a time into a
destination in order:

| the source | GB/s | lines fetched per line copied | bus writes per line copied |
|---|---|---|---|
| whole 1 MiB, no pieces | 62.5 | 1.025 | 2.09 |
| 8192 of each buffer, neighbours in order | 62.2 | 1.028 | 2.10 |
| 8124 of 8192, 68 in, neighbours in order | 61.4 | 1.039 | 2.12 |
| 7740 of 8192, 68 in (our frames), neighbours in order | 60.1 | 1.119 | 2.31 |
| the same, runs of 32 | 60.1 | 1.124 | 2.32 |
| the same, runs of 8 | 59.9 | 1.134 | 2.40 |
| the same, runs of 4 | 59.7 | 1.149 | 2.47 |
| the same, runs of 2 (the halves of a 16 KiB page) | 59.4 | 1.177 | 2.58 |
| the same, every buffer at random | 58.8 | 1.218 | 2.69 |
| 8124 of 8192, 68 in, at random | 58.6 | 1.221 | 2.55 |
| 8192 of each buffer, at random | 58.5 | 1.222 | 2.53 |

- **The loss is the processor's prefetcher running on past the end
  of a piece.** When the piece copied next is not what follows in
  memory, about 25 lines are fetched for nothing at each frame (0.2
  of 121 or 128). It is the same whether the buffer was full or
  not: all three random rows are at 1.22.
- **Two parts, and the second only pays after the first.** The
  order is worth 2% (58.8 to 60.1 for our frames). The 452 bytes of
  each buffer that a frame does not fill, its headers and its
  unused end, are worth another 2 to 3.5% (60.1 to 61.4 or 62.2),
  but only when the buffers are copied in order: filled buffers at
  random are no better than ours.
- Writes to memory rise with the disorder as well, 2.09 to 2.69 a
  line. Not looked into.

### The real path: pairs

`xeu-rig/copy-adjacency.bt` (bpftrace, a kprobe on
`simple_copy_to_iter` in nfsd's threads) notes, for each piece
copied, where its 8,192-byte buffer is against the one the same
thread copied before. 5 s of a two-port run at the pinned setting
(`adj-4x128`, 60.19 GB/s with the probe on, no drive stepped down):

| the buffer copied next is | pieces | share |
|---|---|---|
| the next one up in memory (+8192) | 15,230,708 | 41.4% |
| elsewhere, more than 64 KiB away | 21,195,621 | 57.7% |
| the same buffer (a piece split at a record's end) | 300,370 | 0.8% |
| within 64 KiB up or down otherwise | 9,523 | 0.03% |

Pieces come from the first and the second half of a page in equal
number (18,368,169 and 18,368,183), and 98% of them are 4 to 8 KiB.

- **Real traffic is the "runs of 2" row.** A frame in the first
  half of a page is nearly always followed, in the copy, by the
  frame in its second half, and then the copy goes somewhere
  unrelated. Runs of 2 would give 50% "next up"; 41% says about a
  fifth of the pairs are broken, by another connection's frame on
  the same ring. The benchmark's 1.177 for runs of 2 and 1.218 at
  random bracket what NFS's copy fetches (1.20).
- So a page's two buffers go to the ring together and are filled by
  consecutive frames of one connection more often than not, and
  consecutive pages have nothing to do with each other: the page
  pool hands back the page freed last.

### What could be had, and what it would take

From where the real path is (between 58.8 and 59.4 in the
benchmark's terms):

| change | benchmark row it would reach | gain |
|---|---|---|
| a connection's frames in longer runs of neighbouring buffers: 8 | 59.9 | about 1% |
| the same, long runs | 60.1 | 1 to 2% |
| long runs, and each buffer filled with payload | 61.4 to 62.2 | 3.5 to 5% |

- **Longer runs** need the driver to cut its receive buffers from
  larger blocks (order-2 pages give 8 to a block), or the pool to
  hand out neighbouring pages in order, and a connection's frames to
  stay consecutive on its ring. Four connections share a ring at
  the pinned setting and two with eight rings, and a fifth of the
  pairs are already broken by that. The receive region and its page
  pool are built on single pages.
- **Filled buffers** means a frame's headers somewhere other than
  in front of its payload, so that one frame's payload ends where
  the next begins: header-data split with the payloads packed. The
  card does not offer it (`ethtool -g` reports `TCP data split:
  n/a`); `xeu-hds-experiment/` in this tree is the earlier look at
  it.
- Neither is a setting. The first is a driver and region change for
  about 1%; the two together are what would be worth doing, and
  they depend on the card.

Not tried: the same tracer on eight rings (two connections a ring
should break fewer pairs), and whether anything in software can
stop the prefetcher at a piece's end (its controls are not
reachable from the kernel on this machine as far as is known).

### Scoping the buffer order: what there is to gain, and where the change would be (13 October, 14:45 to 15:10)

Mike asked whether the order of the buffers is worth going after
with `xeu` changes of our own. Two things were looked at, and
nothing was changed.

**How long a connection's runs of frames are on a ring.**
`xeu-rig/ring-flow-runs.bt` (a kprobe on `dev_gro_receive` on the
rings' CPUs) counts frames in a row with one RSS hash. 5 s of a
two-port run at the pinned setting (`runs-4x128`, 59.68 GB/s with
the probe on, no drive stepped down), 35.8 million frames:

| frames in a row from one connection | share of frames |
|---|---|
| 8 or more | 74% |
| 4 to 7 | 19% |
| 2 or 3 | 4% |
| 1 | 3% |

The commonest run is exactly 8 (2.56 million runs), then 7, then 1,
with smaller peaks at 15 and 16: the client sends 62 KiB at a time,
which is eight frames. So the runs are there on the wire. A
connection's frames reach a ring eight at a time, and what breaks
them up in memory is only that the pool hands the ring unrelated
pages.

**Where the order is decided.** The driver takes its buffers with
`page_pool_alloc_frag()`, two to a 16 KiB page. The region
(`net/core/page_pool.c`) is one physically contiguous range, so its
pages are neighbours of one another, but it keeps its free pages on
a last-in, first-out stack, binds only order-0 pools, and each
ring's pool recycles the page freed last. Three ways to longer runs:

| way | where the change is | what it would reach |
|---|---|---|
| The ring's pool hands out its pages in address order, not last freed first | `net/core/page_pool.c`, built into the kernel: a rebuild and a reboot | runs of 8, as the bursts allow: 1.134 for 1.18 to 1.20, about 0.8% |
| The region binds order-2 pools and the driver asks for 64 KiB blocks, 8 buffers to a block | the same file, and a parameter in `xeu` | a burst is not lined up with a block, so runs of about 4: about 0.5%. In loan mode a block is held while any of its eight frames is |
| The driver asks for 64 KiB blocks and its pools are not in the region | `xeu` alone, a module reload | the same 0.5% at best, with the region's bound on receive memory gone and its pools recycling first in, first out |

- **The gain is 0.5 to 0.8%, 0.3 to 0.5 GB/s**, against a spread of
  1.1 to 1.5 GB/s between two-port runs. It could be shown in the
  copy's fetches for each line (1.20 now, 1.13 to 1.15 after) and
  not in the rate.
- **The first way undoes the last-in, first-out reuse** that was
  put in on 9 October, when it took the pages turned away from a
  full pool from 17,278 to under 1,500 in 10 s. What it was worth
  alone was never measured, and the payload is fetched from memory
  either way, so it may cost nothing; it is not known.
- **The 2% of the benchmark is from buffers at random.** NFS is
  already at pairs, and the bursts cap a run at 8, so the part of
  it that can be had is under 1%.

**It is not to be done: the loan comes first (Mike, 13 October).**
The loan path is to be kept as fast as it can be, for NVMe drives
with SGLs when they come, and these changes are paid for there. The
loan does not copy, so it gains nothing from a better-ordered copy
and only bears the cost:

- Buffers cut from 64 KiB blocks: a block stays held while any of
  its eight frames is on loan. It is the 64 KiB page result again,
  where frames sharing a page made loans hold more than they were
  charged and WRITEs stopped part-way.
- The same blocks from pools outside the region: a loan's budget is
  a share of the region, so those pools cannot loan under a budget.
- Payloads packed with header-data split: frames of different
  WRITEs share a page.
- Pages handed out in address order: undoes the last-in, first-out
  reuse. What that does to a loan's footprint is not measured; this
  one is not known to hurt.

So copy mode is not to be optimized past the point where the
receive path stops suiting a loan: one frame to a buffer, pages
returned last in, first out, all inside the region.

**Before that was said, the recommendation was already not to implement it by itself.** It is
approachable (the driver's part is a parameter) but the larger part
is in the kernel's page pool and needs a reboot, for a gain smaller
than the rate can show. It becomes worth doing together with packed
payloads, if the card ever gives header-data split: then the order
is the first half of 3.5 to 5%.

### To reproduce

    gcc -O2 -pthread -o /tmp/copybench xeu-rig/copybench.c
    perf stat -a -e l3d_cache_lmiss_rd,bus_access_wr \
        env PIECE=7740 STRIDE=8192 SOFF=68 RUN=2 /tmp/copybench libc 48 8 5 64
    # during a two-port run, 25 s or more into the load:
    bpftrace xeu-rig/copy-adjacency.bt
    bpftrace xeu-rig/ring-flow-runs.bt

