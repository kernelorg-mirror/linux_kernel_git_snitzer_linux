# Round 35: where round 34's wait went, and a lean refill loop

**The plan (Mike, 27 October):** keep chasing the rings' wins in xeu that build on round 34. First find out, by tracing, where the frag drain's wait went once round 34 took it out, and what in the rings comes after it.

**All on tardis2 at the current state:** the tip's sunrpc (`A06FE478658242A10E100FF`), nfsd the kernel's (`3F0C341E86E266D5FDF2ADF`), `rodata=noalias`, the settled setting, xeu round 34's (`DC23D10E104D370F14FB06E`) unless named. Runs in `/var/tmp/zc-run-logs/t2-r69/` to `t2-r74/`.

**The clock:** the cores run at **2.0 GHz** (2.02G cycles in 1.01 s busy). Rounds 34's README and the talk before this round turned cycles into µs at 3 GHz; those µs figures are a third low.

## 1. The sampled look on round 34's xeu (r69)

Driver [run69.sh](run69.sh): run67.sh's look again, with cycles on the ring CPUs and stall cycles on nfsd's CPUs added.

| on the ring CPUs, a WRITE | r67 (round 28's xeu) | r69 (round 34's) |
|---|---|---|
| cycles | 143.4k | 138.3k |
| backend memory stall cycles | 27.6k | 25.8k |
| at the frag drain and the refill's page loads | about 5.7k | about 5.2k, in `xeu_rx_refill` |

([stat-rings-r69.txt](stat-rings-r69.txt), [samples-stall-r69.txt](samples-stall-r69.txt), [samples-cycles-r69.txt](samples-cycles-r69.txt); one traced run each.)

- **The drain's samples moved into `xeu_rx_refill`.** It has 22.2% of the rings' stall now, and 91% of that sits right after `ldr x3, [x3, #32]`, the load of the page's `dma_addr` ([annotate](annotate-stall-xeu_rx_refill-r69.txt)). That load is in the same `struct page` line as `pp_ref_count` (+40), which the drain's atomic used to wait on.
- **What sampling cannot say:** without SPE the PMU's samples name where the core was stopped, not what stopped it. SPE's driver loads, but the firmware does not describe the unit (no SPE interrupt in the MADT), so no device appears.

## 2. Timed: the refill's own accesses (r70, r71)

Diagnostic xeus, the kept one with a per-CPU `refill_lat` histogram, timed by the 1 GHz counter (`cntvct_el0`, checked against the wall clock: 1,000,060,776 ticks in 1.000061 s; two reads 16 ns apart at least, so about 31 ns is the floor of a timed access with its barriers). Neither is for keeping.

**r70** ([run70.sh](run70.sh), [patch](xeu-diag-refill-dma-addr.patch)): the `dma_addr` load alone (`DSB LD` before and after).

| per buffer | n | mean | under 64 ns |
|---|---|---|---|
| page just taken from the pool | 10.33M | 31.2 ns | 99.99% |
| kept piece of the page cut last | 10.33M | 31.2 ns | 99.99% |

([refill-lat-r70.txt](refill-lat-r70.txt).) The load is near: a load from another core's cache would be at 64 ns and above. In the same run a bpftrace count ([frees-r70.txt](frees-r70.txt)) found the pages freed on nfsd's CPUs: 2.38M `napi_pp_put_page` and 10.5M pages bulk-freed against 1,564 on the ring CPUs.

**r71** ([run71.sh](run71.sh), [patch](xeu-diag-refill-loads-stores.patch)): the writes of the RX descriptor the card reads, drained (`DSB ST` before and after).

| per buffer | n | mean | under 64 ns | 64 to 511 ns |
|---|---|---|---|---|
| `dma_addr` load | 20.68M | 31.2 ns | 99.99% | 0.01% |
| RX descriptor writes, drained | 20.68M | 34.5 ns | 97.2% | 2.8% |

([refill-lat-r71.txt](refill-lat-r71.txt).) Near too: the descriptor ring is cacheable (only six non-cacheable DMA remaps on the host, too few to be xeu's rings), and the card's reads do not take the lines away. Even at its full width the 2.8% tail would be about 0.4 µs a WRITE.

- **Both diagnostics' third slot was wrong** (a control: a load of the ring's mask in r70, the same writes to a line of the CPU's own in r71): its count and its histogram disagreed on CPU 0. It was discarded; r72's counters were rebuilt, each slot on its own line, and every slot's count matched its histogram on every CPU.

## 3. Timed: what is outstanding when the refill starts (r72)

[run72.sh](run72.sh), [patch](xeu-diag-refill-drain.patch): every other refill first drains what is outstanding (`DSB SY`) and times the drain; every refill's loop is timed from its start to before the doorbell, with no barrier inside.

| r72, 383k refills each | mean |
|---|---|
| `DSB SY` at the refill's start | 36.8 ns (95.7% under 64 ns: the floor) |
| loop after that drain | 49.0 ns a buffer (27.4 a refill) |
| loop as the driver runs it | 49.0 ns a buffer (27.5 a refill) |

([refill-lat-r72.txt](refill-lat-r72.txt).) **Nothing is inherited**: no memory work is left from before the refill, and a loop after a drain costs what the others do. The refill's cost is its own loop: 49 ns, about 98 cycles a buffer, 6.6 µs of 135 buffers a WRITE.

## 4. Why the loop is long

The kept loop's disassembly ([refill-kept.dis](refill-kept.dis)), a kept piece, `0x106c0` to `0x107e0`:

- 73 instructions and three calls of `cache_line_size()` a buffer: `xeu_rx_data_size()` and `xeu_rx_headroom()` through `dma_get_cache_alignment()`, which on arm64 is `cache_line_size()`, out of line.
- About 19 loads and 10 stores a buffer.
  - `ring->rx_sr` is loaded again before each of five stores into the descriptor: its packed fields are written one at a time through `WRITE_ONCE` (a clear, `strb`, `strh`, `str`, `str`), and the compiler cannot prove they leave the ring alone.
  - The split state and `sr_prod_idx` go through the ring struct each buffer, so each buffer's index waits on the store of the one before.
  - `rx_buf_size`, `rx_ip_align` and `dma_profile` are read again each buffer.
- So the stall sampled after the `dma_addr` load is the serial chain of L1 accesses around it, not one far miss. The ring CPUs run 2.2 instructions a cycle (r73, r74): they are not stall-bound overall.

## 5. The change: `xeu_rx_refill_lean()`

([The A/B's patch](xeu-refill-lean-ab.patch), on round 34's source; xeu `F713CD92558EF73C0D04443`, behind an A/B-only `rx_refill_lean`, read at each refill.)

- **Once a refill:** the buffer's size, the pieces a page gives, the headroom, the descriptor's first eight bytes (`ctrl | ring_profile << 16 | len_rid_sid << 32`, `pasid` 0) and the six calls of `cache_line_size()`.
- **In registers for the loop:** `rx_sr`, `rx_sw`, the mask, the producer index and the split state, written back once before the doorbell.
- **Per buffer:** two stores into `rx_sw[idx]`, the `dma_addr` load, and the descriptor as two aligned eight-byte stores. A kept piece is 25 instructions with no calls ([refill-lean.dis](refill-lean.dis), `0x10788` to `0x107e8`).
- **Unchanged:** a page just taken still comes from `page_pool_alloc_pages()` with its count set by a store (round 34); the doorbell is still ordered after the descriptors (`dmb oshst`); no new memory is touched and no buffer is held longer.

| a kept piece | before | after |
|---|---|---|
| instructions | 73, and 3 calls of `cache_line_size()` | 25 |
| loads | about 19 | 1 (`dma_addr`) |
| stores | 10 | 4 |
| carried to the next buffer through memory | producer index, split state | nothing |

## 6. How the cost was measured, and what was wrong with it

- **mpstat's `%soft` comes from the tick.** This kernel has no `CONFIG_IRQ_TIME_ACCOUNTING` and no `nohz_full=` CPUs, so `/proc/stat`'s softirq time is the 100 Hz tick's samples. Over 5 s under load ([accounting.txt](accounting.txt)) the jiffies add up to wall time, but the busy time is booked low: CPU 0 63% softirq where its counted cycles say 74% busy, CPUs 2, 4 and 6 40 to 49% where they say 67 to 74%.
- **The rings were never uneven.** Each ring got four connections and 24.4 to 25.4% of the bytes, and NET_RX softirqs, ring interrupts and counted cycles are even across CPUs 0, 2, 4 and 6. Only the tick sampling of CPU 0 differs.
- **So every rings and nfsd figure in FLAMEGRAPH so far, `cost.sh`'s µs a WRITE, is tick-sampled.** It reads low (about 43 µs a WRITE on the rings where the cycles say about 70; nfsd 87 to 92 where they say about 147) and moves by 2 to 4 µs between runs. A/B means on both sides were sampled alike, so their direction holds, but differences of a few µs were hidden.
- **From this round on,** `twoport.sh` counts each CPU's cycles and instructions over its window (`perf-stat-window.txt`, `perf stat -a -A`), and `cost.sh` gives the ring and nfsd CPUs' cycles and instructions a WRITE beside the tick-sampled figures. Rounds are judged on the counted cycles.

## 7. The A/B (r73, r74)

[run73.sh](run73.sh) and [run74.sh](run74.sh): one xeu, A `rx_refill_lean` N (round 34's loop), B Y, untraced loan64 runs, alternating. r73 had four pairs, counted only on its pair 4 (a side count, `cycles-r73-4-*.csv`); r74 counted every run and stopped before 3-B, when the client had another server's share mounted ([run74b.sh](run74b.sh) found it mounted again; Mike: decide on what there is). Every run had full drive rate and dmesg no WARNING. All lines: [cost-r73-r74.txt](cost-r73-r74.txt).

| run | rx_refill_lean | GB/s | rings µs (tick) | ring cycles / instructions a WRITE | nfsd µs (tick) | nfsd cycles / instructions | peak |
|---|---|---|---|---|---|---|---|
| r73-4-A | N | 40.48 | 41.6 | 141.0k / 313.5k | 89.7 | | 6357 |
| r73-4-B | Y | 40.51 | 37.8 | 137.6k / 306.2k | 88.9 | | 6389 |
| r74-1-A | N | 40.52 | 43.4 | 140.0k / 312.7k | 87.1 | 294.8k / 305.1k | 6388 |
| r74-1-B | Y | 40.58 | 44.2 | 137.2k / 305.3k | 87.4 | 294.8k / 305.8k | 6425 |
| r74-2-A | N | 40.45 | 45.2 | 140.6k / 314.0k | 91.3 | 295.4k / 305.2k | 6404 |
| r74-2-B | Y | 40.52 | 43.3 | 138.6k / 307.9k | 92.1 | 294.1k / 304.6k | 6439 |
| r74-3-A | N | 40.49 | 45.1 | 141.2k / 314.7k | 89.2 | 294.6k / 305.7k | 6303 |

| the three counted pairs | B − A, by pair | mean |
|---|---|---|
| ring cycles a WRITE | −3.4k, −2.8k, −2.0k | **−2.7k (−1.9%, about 1.35 µs at 2 GHz)** |
| ring instructions a WRITE | −7.3k, −7.4k, −6.1k | −6.9k |
| nfsd cycles a WRITE (r74) | 0.0k, −1.3k | −0.65k |
| GB/s | +0.03, +0.06, +0.07 | +0.05 |

- **The rings' saving is real.** No A run's cycles overlap a B run's, and the instructions fall by what the shorter loop saves (about 50 a buffer, 135 buffers a WRITE). The cycles fall by about 40% of the instructions, at 2.2 instructions a cycle.
- **The tick-sampled figure could not see it:** r73's four pairs gave −1.08 ± 1.38 µs, r74's two +0.8 and −1.9.
- **The region's peak runs higher on B:** +32 and +37, +35 in the counted pairs, and +72, +96, −31 in r73's other pairs; about +40 pages (640 KiB, 0.6% of the peak) in five pairs of six. The lean loop posts the same buffers and holds no page longer, and no mechanism for it is known; B also carried a little more each time (+0.05 GB/s), which may keep a few more buffers in flight. Against the footprint rule it is reported here, not explained away.

## Decision

- **To be decided by Mike:** the rings 2.7k cycles a WRITE cheaper (−1.9%) with nfsd and the rate the same, against a region peak about 40 pages higher in five pairs of six. Kept, it would lose its parameter and be staged in kernel-rpmtree as release 12, as round 34 was.
- **Not done:** the A/B's last three runs (two pairs and r73's counted pair stand in).
- The A/B's xeu was loaded from the scratchpad for its runs only; `updates/xeu.ko` is still round 34's.
