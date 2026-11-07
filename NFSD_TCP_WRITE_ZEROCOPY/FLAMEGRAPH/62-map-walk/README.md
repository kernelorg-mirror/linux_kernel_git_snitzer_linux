# Round 62: the map walk after round 55's inline, and where nfsd's cycles went

**The question (Mike, 7 November):** fl64, a profile of build 514 with the
client's new defaults (writers pinned, its workqueue scope `numa`; see
`../../HANDOFF-tardis2-canary.md`, "fl64"), ranked the WRITE into the block
layer second on nfsd, with the NVMe SGL map walk in it.  Round 55 inlined
`blk_map_iter_next()` (`46a42a774aed`, kept, in this kernel): 5.7k fewer
instructions a WRITE, about 0.6% fewer cycles, and where the rest went was not
found.  And on 514 nfsd does 507's instructions in about 11% more cycles
(`t2-r233`: 309.6k cycles, 277.8k instructions a WRITE).  So: does the walk do
more work now, and once its calls and returns are gone, where did the cycles go?

fl64 first read the walk as twice 507's; that set an inclusive figure against a
self one.  Self against self it is 2.33 us a WRITE against fl-blk's 1.62 (2.12%
of nfsd against 1.83%).

## The runs ([run234.sh](run234.sh), [run234.out](run234.out))

Build 514's held-mark kernel, both ports, budget 128, the client at
`twoport.sh`'s defaults; each run after a trim of the eight and 120 s of rest.

- **t2-r234, the trace** (70.88 GB/s): [sglwalk-kp.bt](sglwalk-kp.bt) for
  10 s.  It is round 55's `xeu-rig/sglwalk.bt` with kprobes for nfsd and nvme,
  whose BTF does not match this boot's vmlinux (fentry finds neither), so it
  leaves out the SGL entries written and the times of nvme's calls.  Output in
  `$L/t2-r234/sglwalk.txt`.
- **t2-r235, the PMU** (70.44 GB/s, 67159 WRITEs a second): cycles,
  instructions and L1 and L2 refills sampled on nfsd's CPUs for 8 s, as round
  55's run170.  Read with [pmu62.py](pmu62.py) and [areas62.py](areas62.py)
  against round 55's `t2-r170` (build 506, the walk out of line, 67.27 GB/s,
  64583 WRITEs a second): [pmu-r170-r235.txt](pmu-r170-r235.txt),
  [areas-r170-r235.txt](areas-r170-r235.txt),
  [areas-detail-r170-r235.txt](areas-detail-r170-r235.txt).  Sampled cycles
  read lower than counted ones (201k against round 55's counted 273k): sample
  is set against sample only.

## The walk does the same work

| | build 506 (round 55) | build 514 (t2-r234) |
|---|---|---|
| a WRITE | one bio of 1 MiB | one bio of 1 MiB (1048545 bytes on average) |
| requests | 256 KiB | 256 KiB, four a WRITE, three splits |
| `nr_phys_segments` a request | 34.9 | 34.9 |
| steps of the map walk | one a piece, no merge | 67.9M for 70.9M segments: one a piece |

`bi_vcnt` reads 0 on these bios: they use nfsd's bvec table
(`bio_iov_bvec_set`), so the pieces are counted from the requests.

## The inline moved the walk's instructions, it did not remove them

k a WRITE, sampled, build 506 against 514:

| | cycles | instructions | IPC |
|---|---|---|---|
| `blk_map_iter_next` (out of line on 506) | 3.48 to 0 | 14.59 to 0 | 4.19 |
| `blk_rq_dma_map_iter_next` (the walk inlined into it on 514) | 1.24 to 5.14 | 6.59 to 21.29 | 5.30 to 4.15 |
| `dma_map_phys` | 1.48 to 1.19 | 5.86 to 2.09 | |
| the whole walk (the eight functions, `bio_split_io_at` included) | **10.70 to 11.45** | **41.34 to 38.49** | 3.86 to 3.36 |

The inlined body's instructions reappear one for one in its caller (+14.7k
against -14.6k).  The walk lost 2.8k instructions a WRITE in all and gained
0.7k cycles, its IPC 3.86 to 3.36, its L2 refills the same (71 a WRITE).  So the
calls and returns were not what the walk spent its time on: the same pieces take
the same cycles, inlined or not.  The walk is not where nfsd's cycles went.

## Where they went: by area

nfsd's CPUs, sampled, k a WRITE, 506 to 514: **201.1 to 250.7 cycles (+49.6k),
243.8 to 255.7 instructions (+11.8k)**.  Not 507 (round 55's run170 is build
506), and 514's run is 5% faster; the shares are what to read.

| area | cycles | instructions |
|---|---|---|
| the loan (sunrpc's receive and the region) | **+10.6** | +6.6 |
| sunrpc and nfsd, the rest | +9.0 | +3.3 |
| locks | **+6.9** | +0.1 |
| everything else | +6.3 | -1.0 |
| transmit (TCP, IP, qdisc, xeu) | +5.2 | +3.5 |
| sleep and wake | +4.3 | +0.7 |
| page_pool and skb frees | +2.4 | +0.9 |
| block and iomap | +2.0 | +0.9 |
| the map walk and NVMe | +1.1 | -3.0 |

- **The loan's code costs about 7k cycles a WRITE more for the same jobs.**
  The append, 4.46k on 506 (`svc_tcp_rx_append_ref`), is 6.72k
  (`svc_tcp_rx_append_ref` 2.80 and `__rx_loan_table_append_ref` 3.92); the
  release 2.81k to 3.41k (`rx_loan_table_release`); the prefetch 2.35k to
  3.51k (`rx_loan_prefetch_pages`); and `rx_region_page_put`, 2.99k, is new: in
  fl64 it is 3.09 us a WRITE, nearly all under the release's
  `page_pool_recycle_ring_bulk`.  `rx_loan_table_release` and
  `rx_loan_prefetch_pages` came with David's `net: rx_loan: move the receive
  page-loan machinery out of sunrpc` (`49e2111a6420`, 27 October, the canary),
  `__rx_loan_table_append_ref` with `d3b462b4da65`, `rx_region_page_put` with
  `4522295b0c55` (the region kept mapped a device).  Code that sunrpc could
  inline is now called across a module boundary, each return checked by
  pointer authentication (round 55: 29% of `dma_map_phys`'s samples were its
  `autiasp` and `ret`).  That is the likely reading; it is not measured apart.
- **Lock contention rose:** `_raw_spin_lock` 5.62 to 9.69k cycles at the same
  instructions, `queued_spin_lock_slowpath` 1.03 to 2.91k.  In fl64 the slow
  path is mostly `svc_recv`'s `__lwq_dequeue` (0.86 of 1.31 us a WRITE): the
  512 threads of the one pool taking transports off one queue; then
  `lock_sock_nested` (0.26).
- **The transmit path's rise is partly the samples' doing:** round 55's run
  did not resolve xeu's symbols (`[unknown]`, 3.3k cycles a WRITE), so xeu's
  transmit frames (about 2k on 514) appear new here.

## For the next round

The walk is closed: it does the same work at the same cost, and its calls were
not its cost.  What these samples point at, by size:

1. **The loan's code since it moved out of sunrpc** (about 7k cycles a WRITE
   against 506, the largest change).  A look first: an A/B of the canary
   before and after `49e2111a6420` would measure the move itself; failing a
   reboot, `perf annotate` of the four functions for call and return
   overhead.  Inline helpers in a header, or one entry point per job, would be
   the change.
2. **The thread pool's transport queue** (`__lwq_dequeue`'s contended lock,
   0.86 us a WRITE in fl64).  Rounds 17, 18 and 40 worked the wakes, not this
   lock.
3. **`rx_region_page_put` under the release** (3.09 us in fl64): round 58's
   ground; the bulk return to the slot was dropped on 7 November on its cycles.
