# Round 34: the rings' memory stall, and cutting RX buffers from whole pages

**The plan, from the queue:** round 30 left the rings' memory stall at about 20% of their cycles on `rodata=noalias`, cache misses and not TLB walks. Locate it first, by sampled refills on the ring CPUs, then choose a change.

## The look

**Run 2026-10-27 on tardis2** (driver [run67.sh](run67.sh), run `/var/tmp/zc-run-logs/t2-r67/`):
- **The state:** the current state (the tip's sunrpc, nfsd the kernel's, xeu `2C05DF674C9D013185FDB34` with `rx_prefetch` Y, `rodata=noalias`, the settled setting), one loan64 run, 40.55 GB/s.
- **The probe:** on the ring CPUs (0, 2, 4, 6), from 5 s after the WRITEs passed 10,000 a second: `perf stat` for 4 s, then 4 s of samples each of `stall_backend_mem`, `l1d_cache_refill_rd`, `l2d_cache_refill_rd` and `ll_cache_miss_rd`, ranked by symbol ([top-*-r67.txt](.)) and annotated.
- **Traced:** its cost (rings 43.5, nfsd 88.8 µs a WRITE) is not set against untraced runs.

**The count** ([stat-rings-r67.txt](stat-rings-r67.txt), 155,966 WRITEs in 4 s), a loaned WRITE on the ring CPUs:

| | a WRITE |
|---|---|
| cycles | 143.4k |
| backend memory stall cycles | 27.6k (19%) |
| L1D read refills | 541 |
| L2 read refills | 900 |
| last-level read misses | 915 |
| page-table walks | 0.3 |

**Where the stall is** (`stall_backend_mem` samples):

| | share of the stall |
|---|---|
| `page_pool_alloc_frag_netmem` | 20.4% |
| `_raw_spin_unlock_irqrestore` (mostly interrupts coming back on) | 8.5% |
| `xeu_rx_refill` | 7.4% |
| `xeu_rx_poll`, `svc_data_ready` | 2.9% each |
| GRO, TCP and the rest | each 2.2% or less |

- **The largest single stall is one atomic.** 81% of `page_pool_alloc_frag_netmem`'s samples are on one `ldaddal` ([annotate](annotate-stall_backend_mem-page_pool_alloc_frag_netmem-r67.txt)), the page's `pp_ref_count` in `page_pool_drain_frag()`.
  - xeu cuts each 16 KiB page into two 8 KiB buffers through page_pool's frag API (`rx_buf_size=8192`).
  - The API charges a new page `BIAS_MAX` references and takes back the part not handed out with that atomic when it moves on to the next page. Its result has to be checked, so the poll waits for it.
  - That is about 4% of the rings' cycles, roughly 1.5 µs a WRITE at about 3 GHz.
- **The next one is the same line.** Most of `xeu_rx_refill`'s stall is its first read of the new page's `struct page` (`dma_addr`, [annotate](annotate-stall_backend_mem-xeu_rx_refill.isra.0-r67.txt)).
- **Round 33's caveat is settled.** The completion entry is still about a third of `xeu_rx_poll`'s L1 refills ([annotate](annotate-l1d_cache_refill_rd-xeu_rx_poll-r67.txt)), but the whole poll is 2.9% of the stall, about 0.05 µs a WRITE for the entry. Round 33 stays a look.

## The change

**xeu cuts its buffers from whole pages counted once** ([the patch](xeu-cut-rx-buffers-from-whole-pages-counted-once.patch), on round 28's source; xeu `30470F8F8281F32E66A5F35`):
- A new parameter `rx_page_split` (bool, 0644, read at each refill, default N).
- With Y and buffers smaller than a page, the refill takes a whole page with `page_pool_alloc_pages()`, sets its count to the number of pieces (2) with `page_pool_fragment_page()`, a plain store, and keeps the pieces not yet posted in the ring for the next refill. No drain, no atomic at allocation; each piece is still freed through page_pool as before, and the last one recycles the page.
- A ring being released hands back a piece it kept, one reference each.
- **The footprint:** the same. A ring holds at most one page with a piece not posted, as page_pool's frag page does now.
- With N the refill is round 28's.

**Installed** in `updates/xeu.ko` (round 28's is beside it as `xeu.ko.r28-2C05DF674C9D013185FDB34`), xeu reloaded once with no client connected, port a set up again with `port-setup.sh enp161s0f0 0` and settled. `settled.sh` holds `rx_page_split` at N and writes it on `apply`. The initrd still carries round 28's xeu.

## The A/B

**Run 2026-10-27 on tardis2** (driver [run68.sh](run68.sh), run `/var/tmp/zc-run-logs/t2-r68/`): one xeu, A `rx_page_split` N, B Y, four untraced loan64 runs a side, A B A B A B A B, at the current state. Every run had no drive stepping down, dmesg had no WARNING, and the end was the settled setting.

| run | rx_page_split | GB/s | rings µs/WRITE | nfsd µs/WRITE | region peak, pages |
|---|---|---|---|---|---|
| 1-A | N | 40.42 | 43.6 | 91.4 | 6422 |
| 1-B | Y | 40.49 | 42.1 | 92.1 | 6358 |
| 2-A | N | 40.46 | 42.7 | 88.8 | 6343 |
| 2-B | Y | 40.47 | 44.7 | 87.4 | 6402 |
| 3-A | N | 40.50 | 43.6 | 87.1 | 6519 |
| 3-B | Y | 40.54 | 42.6 | 92.2 | 6463 |
| 4-A | N | 40.41 | 48.3 | 89.1 | 6205 |
| 4-B | Y | 40.52 | 42.4 | 88.4 | 6548 |

| | A (N) | B (Y) | B − A, by pair |
|---|---|---|---|
| rings | 44.55 ± 2.54 | 42.95 ± 1.18 | −1.60 ± 1.63 (−1.5, +2.0, −1.0, −5.9) |
| nfsd | 89.10 ± 1.77 | 90.03 ± 2.49 | +0.93 ± 1.46 |
| GB/s | 40.45 | 40.51 | |
| region peak | 6205–6519 | 6358–6548 | the same |

- **Pair 4's A is off by flow placement, not by the change.** Its ring CPUs ran 61, 25, 59 and 41% soft against the usual one busy ring and three near 36% (3-A: 59, 37, 35, 38): two connections landed on CPU 4's ring. It gives the rings 48.3, 4.7 above every other A run, and alone makes the mean difference. Without pair 4 the rings' difference is −0.17 (−1.5, +2.0, −1.0).
- **So the change saves the rings nothing that shows.** The 1.5 µs the atomic's stall share suggested is not there: with the atomic gone, the cycles it waited on are paid elsewhere, or the wait overlapped work the poll does anyway.
- **nfsd** moves the way it does between runs, with no direction.
- **The region:** the peak is the same on both sides. Free pages at rest held at 31,220 to 31,235 from run 1's end on (the drop from 32,192 before it is the pools filling their caches after the reload), so the split path gives back every piece it keeps.

## Decision

- **Not kept** is the reading of the A/B: no saving outside the spread between runs, for a change to xeu's refill. To be decided by Mike.
- `settled.sh` holds `rx_page_split` at N; with N the module is round 28's in behaviour, so it can stay installed.
- **What the look leaves:** the stall is spread thin past the frag atomic. No other single place holds more than 8.5% of it, and that one (`_raw_spin_unlock_irqrestore`) is mostly where interrupts are unmasked.
