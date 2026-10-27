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
  - That is about 4.5k cycles a WRITE, 3% of the rings' cycles, about 2.3 µs at the cores' 2.0 GHz (round 35).
- **The next one is the same line.** Most of `xeu_rx_refill`'s stall is its first read of the new page's `struct page` (`dma_addr`, [annotate](annotate-stall_backend_mem-xeu_rx_refill.isra.0-r67.txt)).
- **Round 33's caveat is settled.** The completion entry is still about a third of `xeu_rx_poll`'s L1 refills ([annotate](annotate-l1d_cache_refill_rd-xeu_rx_poll-r67.txt)), but the whole poll is 2.9% of the stall, about 0.05 µs a WRITE for the entry. Round 33 stays a look.

## The change

**xeu cuts `rx_buf_size`'s buffers from whole pages it counts once** ([the patch](xeu-cut-rx-buffers-from-whole-pages-counted-once.patch), on round 28's source; xeu `DC23D10E104D370F14FB06E`):
- **What it builds on:** `rx_buf_size` below a page cuts each page into pieces (two of 8 KiB at 16 KiB pages). Until now the pieces came from `page_pool_alloc_frag()`, which charges a new page `BIAS_MAX` references, because it does not know how many pieces it will cut, and drains the rest with the atomic above.
- **The ring knows:** a page gives `PAGE_SIZE / rx_buf_size` pieces. So the refill takes a whole page with `page_pool_alloc_pages()`, sets its count to that number with `page_pool_fragment_page()`, a plain store, and keeps the pieces not yet posted in the ring (`split_page`, `split_offset`, `split_left`) for the next refill. No drain, no atomic when buffers are taken.
- **Freeing:** each piece is still freed through page_pool as before, and the last one recycles the page. A ring being released hands back a piece it kept, one reference each.
- **The footprint:** the same. A ring holds at most one page with a piece not posted, as page_pool's frag page does now.
- **No parameter:** it is what `rx_buf_size` below a page does. Whole-page buffers are as before.

**As measured,** the A/B's module (`30470F8F8281F32E66A5F35`, [its patch](xeu-cut-rx-buffers-from-whole-pages-counted-once-ab.patch)) had the same code behind a parameter, `rx_page_split`, read at each refill, so one loaded module gave both sides; with it N the refill was round 28's. The kept module drops the parameter; nothing else differs.

**Installed** in `updates/xeu.ko` (round 28's is beside it as `xeu.ko.r28-2C05DF674C9D013185FDB34`); xeu was reloaded with no client connected, for the A/B's module and again for the kept one, and each time port a was set up again with `port-setup.sh enp161s0f0 0` and settled.

**The initrd carries it** from 27 October, 23:52: rebuilt as round 28's was (`dracut -f /boot/ofp-rocky/$K/initrd.new $K`) and swapped in, with the one before beside it as `initrd.pre-xeu-r34-2026-10-27`. It also carries the tip's sunrpc (`A06FE478658242A10E100FF`) from `updates/`, uncompressed, where the one before carried the kernel's; both are what runs. Rebuild it again whenever a module in `updates/` changes.

## The A/B

**Run 2026-10-27 on tardis2** (driver [run68.sh](run68.sh), run `/var/tmp/zc-run-logs/t2-r68/`): the A/B's xeu, A `rx_page_split` N (round 28's refill), B Y (the change), four untraced loan64 runs a side, A B A B A B A B, at the current state. Every run had no drive stepping down, dmesg had no WARNING, and the end was the settled setting.

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
- **So the change saves the rings nothing that shows on its own.** The atomic's stall share suggested about 2.3 µs a WRITE. Where its wait went is not known from these runs: round 35 traces it (and finds these µs, tick-sampled, too coarse to show a change of that size).
- **nfsd** moves the way it does between runs, with no direction.
- **The region:** the peak is the same on both sides. Free pages at rest held at 31,220 to 31,235 from run 1's end on (the drop from 32,192 before it is the pools filling their caches after the reload), so the split path gives back every piece it keeps.

## Decision

- **Kept (Mike, 27 October).** It is the step `rx_buf_size` below a page should take: the ring counts what it cuts instead of charging a bias and draining it. That it shows no saving on its own says the rings' time it should free is still taken by something after it.
- **Next, round 35:** keep chasing the rings' wins in xeu that build on it, first by tracing where the drain's wait went and what in the rings is left ([35-after-split](../35-after-split/README.md)).
- `settled.sh` has no parameter to hold; its xeu note names the module.
- **What the look leaves:** past the frag atomic the stall is spread thin. No other single place holds more than 8.5% of it, and that one (`_raw_spin_unlock_irqrestore`) is mostly where interrupts are unmasked.
