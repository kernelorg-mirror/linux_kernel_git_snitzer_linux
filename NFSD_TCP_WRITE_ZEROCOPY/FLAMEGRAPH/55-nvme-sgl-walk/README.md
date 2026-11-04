# Round 55: the walk per piece in the NVMe SGL write path (the plan)

**Nothing here has been run.** This is the plan written on 2 November before tardis2 went down for four more SGL drives and its second port (Mike).

**The target:** about 5 us a WRITE, 5.9% of nfsd, stepping one piece at a time through a table of about 128 pieces to split the bio and to map it for DMA. On these drives (`max_segments` 65, `max_sectors_kb` 256, `dma_alignment` 3) a 1 MiB WRITE is about four 256 KiB requests.

## Why this one

From fl54 (`/var/tmp/zc-run-logs/t2-r142/fl54-flame`), with IPC and refills from round 54's `perf record` (`54-fh-verify/pmu.txt` is that run, `t2-r139/pmu.script` has every symbol):

| frame | us a WRITE | % of nfsd | IPC | L2 refills a thousand instructions |
|---|---|---|---|---|
| `blk_rq_dma_map_iter_next`, inclusive | 1.64 | 1.87 | 5.24 | 1.2 |
| `blk_map_iter_next` | 1.62 | 1.85 | 4.23 | 1.2 |
| `bio_split_io_at` | 0.84 | 0.96 | 3.18 | 2.6 |
| `dma_map_phys`, under SGL setup | 0.25 | 0.29 | 3.95 | 1.7 |
| `nvme_pci_setup_data_sgl`, self | 0.31 | 0.36 | 4.83 | 1.3 |
| the rest of `nvme_map_data` | 0.87 | 1.0 | about 1 | about 21 |

(The last row's PMU figures are rough: `nvme_map_data` itself reads 1.12 and 21.0, and `blk_rq_dma_map_iter_start`, most of the row, was not counted.)

- **It is work, not misses.** IPC 3 to 5 with almost no refills. Round 52's walks were like that and saved 2.1% of nfsd. Round 54's handle path ran at 0.2 to 0.6 and its fix saved a third of the estimate, because the misses stayed.
- **It is the largest group left.** `nfsd_direct_write` is 18.7 us, 21% of nfsd. Sleep and wake (rounds 17, 18, 40) and the transmit lock (rounds 7, 31, 32) have been worked through, and `page_pool_put_netmem_bulk` by rounds 23, 24, 38 and 45.
- **It is what SGL drives run on,** and any O_DIRECT user with fragmented buffers takes this path, so a fix would matter beyond this series.

This corrects the ranking after round 53, which set "the block layer's 128 pieces" aside as following from the 8 KiB buffers and the 256 KiB limit. The number of pieces does follow from them. What it costs each piece was not measured then.

## How it is to go

1. **Bring-up.** `bringup.sh` (kdump first), then `settled.sh`, which will need the new drives and the second port. Any change to the setting is made there.
2. **A new baseline.** Four loan runs, latency and a profile. Figures before the new hardware are not comparable. Note which CPUs nfsd and the rings run on if the port's layout changes.
3. **Profile again.** Does this path grow or shrink with more drives? If the ranking changes, this plan changes with it.
4. **Trace before code,** as round 54 did. Pieces per request and per bio. Whether neighbouring pieces are ever contiguous: they probably are not, since headers sit between payloads in the 8 KiB buffers, but it is unmeasured. Which walks repeat what an earlier one worked out, starting with `bio_split_io_at` and the DMA iterators, and whether `nr_phys_segments` or a segment boundary could carry that over. Also where `dma_direct_map_phys` spends its time.
5. **A change only if the trace shows a repeat to remove.** The table must still be walked once to write the SGL, so only the repeats and the overhead of each step can go.

**Estimate: 1 to 2 us, 1 to 2% of nfsd, and only a guess.** Round 54's was three times too high.

**What it costs to test:** `block/` is built in, so a change there needs a new kernel and only `nvme.ko` can be swapped. Its reviewers are block and NVMe, not Chuck Lever. Do not grow the region's footprint, and A/B latency in both orders, as rounds 52 to 54 showed it has to be.

**The fallback:** `svc_tcp_rx_append_ref` and `svc_tcp_rx_borrow_segment`, about 4.1 us together at IPC 3 to 4, and a `sunrpc` swap to test. Round 27's prefetch did not help, and about a third of the second one's cost is on `struct page`. Nobody has tried trimming the per-fragment checks, which may repeat what the skb already guarantees.

## For a new kernel (the plan of 3 November, after round 57)

Round 57's baseline at the two-port setting (12 connections a port, 512 KiB,
rings of 128) is the new reference. Its profile (fl56,
`/var/tmp/zc-run-logs/t2-r165/fl56-flame`), nfsd, µs a WRITE: `nfsd_direct_write`
20.4, `submit_bio` 10.7, `nvme_queue_rqs` 6.2, `nvme_prep_rq` 5.8,
`blk_map_iter_next` 1.7 self, `bio_split_io_at` 0.9, `dma_direct_map_phys`
1.1 and `dma_map_phys` 0.7 self, `nvme_pci_setup_data_sgl`'s producer-lock
leaf 0.5. The path has not shrunk with eight drives: it is the same cost a
WRITE.

### The path as it runs on these drives

Read from the source of build 506 (`block/blk-merge.c`, `block/blk-mq-dma.c`,
`drivers/nvme/host/pci.c`), not yet traced:
- **A 1 MiB WRITE is about 135 pieces,** one a received frame (about 7.75 KB
  of payload in each 8 KiB buffer). `max_sectors_kb` 256 binds before
  `max_segments` 65, so it goes out as about four requests of about 33 pieces
  (4.12 NVMe interrupts a WRITE agree).
- **SGL is forced, not chosen:** `nvme_pci_use_sgls()` returns `SGL_FORCED`
  because the pieces leave gaps inside the controller's page
  (`req_phys_gap_mask()`), so `sgl_threshold` (32768, above the average piece)
  is not consulted.
- **No IOMMU:** every piece is mapped with `dma_map_phys()` →
  `dma_direct_map_phys()` one at a time. The IOVA path is not taken.
- **Each piece is walked twice:**
  - `bio_split_io_at()` walks the bvecs to find the 256 KiB split. For each
    it does the alignment checks, the gap test and accumulation, and the
    segment and byte counts. It never merges neighbours.
  - `blk_map_iter_next()` walks them again to map them. For each piece it
    works out `get_max_segment_size()` and, at every bvec boundary, tries
    `biovec_phys_mergeable()` against the next piece.
- **The merge test likely never succeeds:** each buffer's payload begins
  after its frame's headers, so neighbouring pieces should never be
  physically contiguous. If so, the test is paid on about 135 pieces a WRITE
  for nothing. `bio_split_io_at()` already knows the segment count without
  merging; if the request's `nr_phys_segments` equals its bvec count, nothing
  can merge.

### Steps

1. **Trace on build 506 now, no rebuild** (bpftrace, one loan run on both
   ports at the settled setting):
   - requests a WRITE and pieces a request (`nr_phys_segments` at
     `nvme_prep_rq`);
   - the SGL entries written (`nvme_pci_setup_data_sgl`'s count) against
     `nr_phys_segments`: any difference is merges, and none means the merge
     test is wasted;
   - calls of `bio_split_io_at()` a WRITE, and bvecs walked a call, to check
     that the split does not walk the same pieces again;
   - the time a call of `bio_split_io_at()`, `nvme_map_data()` and
     `blk_rq_dma_map_iter_next()` (fentry/fexit, the per-call figure only:
     the probes cost more than the work).
2. **Candidate changes, only as far as the trace bears them out:**
   - (a) **Skip the merge test when the split found no mergeable
     neighbours.** For example, a request flag set when
     `nr_phys_segments` equals the bvecs it holds, tested in
     `blk_map_iter_next()` before `biovec_phys_mergeable()`.
   - (b) **Hoist `get_max_segment_size()` out of the walk** when the queue
     has no segment boundary and `max_segment_size` is `UINT_MAX`, as here.
   - (c) **Carry what the split computed into the map walk,** if (a) and (b)
     leave the second walk's cost per piece above about 10 ns.
   - The SGL must still be written once a piece, and each piece mapped once.
     Only the repeats and the per-step overhead can go.
3. **The kernel.** `block/` is built in, and `nvme.ko` cannot be swapped
   while the boot drive (`nvme8`) is on it. So:
   - one kernel carries the change behind a temporary switch (a debugfs or
     sysfs knob, default off) for the A/B, four runs a side, A B B A,
     latency in both orders;
   - the switch is dropped before the commit goes into the code group;
   - the same kernel carries rounds 58 and 59's changes, each behind its own
     switch, so one boot serves all three.
4. **Estimate:** (a) and (b) together about 1 µs a WRITE (135 pieces at about
   7 ns), 1% of nfsd. Still a guess: round 54 was three times too high.

Reviewers: block and NVMe (Christoph Hellwig, Keith Busch, Jens Axboe), not
Chuck Lever. Do not grow the region's footprint; this path holds no region
page longer.

## The trace and the PMU on build 506 (3 November)

Both on tardis2 at the two-port setting, nfsd `3F0C341E86E266D5FDF2ADF`,
sunrpc `5DDCC44639FC73AF533513F`.

**[sglwalk.txt](sglwalk.txt)** (run168, `xeu-rig/sglwalk.bt` for 10 s, the run
at 68.07 GB/s, about 580,000 WRITEs):
- **`bio_split_io_at()`: four calls a WRITE, three splits.** The calls see
  1 MiB, 768 KiB, 512 KiB and 256 KiB left, and each stops at its split, so
  each piece is walked once by the split. There is no repeated walk inside it.
- **About 3.5 requests a WRITE of 256 KiB,** `nr_phys_segments` 32 to 36
  (34.9 on average), about 125 pieces a WRITE.
- **The map walk never merges:** in all 2,011,208 requests the SGL entries
  written equal `nr_phys_segments`. The merge test in `blk_map_iter_next()`
  is paid on every piece and never succeeds.
- The times per call in the trace include the probes' own cost (nested
  probes add to the outer ones), so they are used only to rank.

**[pmu.txt](pmu.txt)** (run170, [run170.sh](run170.sh), cycles, instructions
and L1 and L2 refills sampled on nfsd's CPUs for 8 s, 67.27 GB/s):

| function | % of nfsd's cycles | IPC | L1 refills / k instructions | L2 refills / k instructions |
|---|---|---|---|---|
| `blk_map_iter_next` | 1.73 | 4.19 | 1.8 | 1.3 |
| `dma_direct_map_phys` | 1.03 | 2.48 | 3.5 | 3.1 |
| `bio_split_io_at` | 0.82 | 3.37 | 1.0 | 2.4 |
| `dma_map_phys` | 0.74 | 3.95 | 2.6 | 1.7 |
| `blk_rq_dma_map_iter_next` | 0.62 | 5.30 | 1.6 | 1.0 |
| `nvme_pci_setup_data_sgl` | 0.34 | 5.10 | 1.5 | 1.3 |
| all of nfsd | 100 | 1.21 | 8.9 | 14.6 |

- **The walk is instructions, not misses.** The per-piece functions run at
  IPC 2.5 to 5.3 with a fifth of nfsd's refills. In round 57's profile the
  samples of `blk_map_iter_next` and `bio_split_io_at` pile up after the
  loads of the bvec table (37% and 45%), which looked like misses. The PMU
  says they are not, so a prefetch of the table was not written.
- **What a piece costs is the calls:** about 4.5% of nfsd's cycles (5.3 with the split) for about
  125 pieces a WRITE, through four calls a piece (`nvme_pci_setup_data_sgl`
  → `blk_rq_dma_map_iter_next` → `blk_map_iter_next`, `dma_map_phys` →
  `dma_direct_map_phys`), each return checked by pointer authentication: in
  `dma_map_phys`, `autiasp` and `ret` take 29% of its samples.

## The change

`block: inline blk_map_iter_next()` (code group, at the tip): one call a
piece fewer, inside the block layer, with no change to what is mapped. The
merge test stays: removing it would need the split to tell the map walk that
nothing can merge, and the split does not look at physical addresses. At a
few instructions a piece it is not worth a new field.

**Estimate: 0.5 to 1 µs a WRITE (about 125 calls and returns), a guess.**

**To test, on the kernel built from the tip** (with the temporary commit
`DROP: page_pool, block: A/B switches for FLAMEGRAPH rounds 55 and 58`):
`blk_mq_dma.map_iter_inline` N calls an out-of-line copy as before, Y (the
default) the inlined one. From this directory:
`../ab-switch.sh blk t2-r172 > run172.out` (four loan runs A B B A, latency A B
B A then B A A B, a profile of B).

## The A/B on build 507 (tardis2, 3 November), the loan runs

`../ab-switch.sh blk t2-r172` (`run172.out`, logs in `$L/t2-r172`), both
ports, 12 connections a port at 512 KiB, rings of 128, budget 128;
`page_pool.rx_region_bulk_ab` at Y throughout (round 58, kept).

| run | GB/s | nfsd cycles a WRITE | instructions | rings | region in use at the middle | peak |
|---|---|---|---|---|---|---|
| blk-1-A (N) | 70.54 | 279.6k | 278.2k | 145.3k | 3304 (52 MiB) | 3664 |
| blk-2-B (Y) | 70.01 | 275.9k | 272.7k | 143.7k | 3408 (53 MiB) | 3763 |
| blk-3-B (Y) | 67.63 | 270.0k | 272.8k | 144.2k | 3088 (48 MiB) | 4455 |
| blk-4-A (N) | 65.63 | 269.5k | 278.7k | 143.9k | 2935 (46 MiB) | 2956 |

The inline takes 5.7k instructions off a WRITE on nfsd (278.45k to
272.75k, -2.0%), the same in every run. Cycles fall less, 274.6k to
273.0k (-0.6%), inside the drift: on this boot cycles a WRITE follow the
rate (about 276 to 280k at 70 GB/s, 270k at 66 to 68), and the A pair
spans both. Rate level (68.09 against 68.82 GB/s), the region the same.
blk-4-A was disturbed: 813 ring drops on port b and 868 client
retransmits, against 19 to 183 and 66 to 322 in the others.

The rest of `run172.out`: `lat.sh` at iodepth 1, A 8.19, 8.38, 7.89,
8.46 GB/s (mean 8.23, mean completion 0.439 ms), B 8.54, 8.38, 7.66, 8.46
(8.26, 0.439 ms): no difference. The profile of B (`fl-blk`, 67.31 GB/s;
rings busy 31.89 µs a WRITE, nfsd 88.79): `blk_rq_dma_map_iter_next`, with
`blk_map_iter_next` in it, 1.83% of nfsd's busy time, 1.62 µs a WRITE,
against 1.22% and 0.47% (1.66 µs) in round 56's profile on build 506
(`t2-r165/fl56`, another boot). The walk costs the same: the inline takes
the call and 5.7k instructions a WRITE, not the walk's time.

What the B profile has at its top on nfsd, past idle: `finish_task_switch`
7.81% (about 700k context switches in a loan run's window),
`_raw_spin_unlock_irqrestore` 3.71% (the region's lock under
`page_pool_recycle_ring_bulk` 1.94% of nfsd's samples, `wake_up_process`
1.62, `nvme_pci_setup_data_sgl` 0.51), `_raw_spin_lock` 2.19% (the qdisc's
root lock in `__dev_xmit_skb` 1.55, `svc_recv` 0.57), then
`blk_rq_dma_map_iter_next` 1.83, `svc_tcp_rx_append_ref` 1.71 and
`svc_tcp_rx_borrow_segment` 1.45.
