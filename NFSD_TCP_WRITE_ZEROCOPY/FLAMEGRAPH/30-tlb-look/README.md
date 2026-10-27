# Round 30: where the ring's prefetch waits: the linear map is in 16 KiB pages (a look)

**The plan, from round 28:** in `xeu_rx_poll`, load the next frame's descriptor entry a frame earlier. Round 28 had taken about 0.8 µs of `xeu_rx_poll`'s new cost to be the wait for that entry, which the header prefetch's address needs.

**What round 28's profile shows, read again** (fl55b-loan64, `rx_prefetch` Y):
- The descriptor entry's load (`ldr x0, [x2, x0]`, +0x168) has about 2.5% of `xeu_rx_poll`'s samples on the instruction after it.
- 15.5% are on the instruction after the header prefetch itself (`prfm` at +0x1a8), whose address is ready by then.
- A prefetch that holds the pipeline up points at the TLB.

## The look

**Run 2026-10-27 on tardis2** (driver [run57look.sh](run57look.sh), the run `/var/tmp/zc-run-logs/t2-r54/`):
- **The setup:** build 499's own sunrpc, the settled setting (xeu `rx_prefetch` Y), one loan64 run at 40.53 GB/s.
- **The probes:** from 5 s after the WRITEs passed 10,000 a second, on the ring CPUs (0, 2, 4, 6):
  - `perf stat` of TLB and cache events for 4 s, over 156,404 WRITEs;
  - then 4 s each of samples of `dtlb_walk` (one in 200) and `l1d_cache_refill_rd` (one in 500);
  - all annotated while xeu was loaded.

**The ring CPUs, per loaned WRITE** ([stat-rings.txt](stat-rings.txt)):

| event | per WRITE |
|---|---|
| cycles | 147.7k (49 µs) |
| L1 TLB misses (`dtlb-load-misses`) | 1,171 |
| page-table walks (`dtlb_walk`) | 69 |
| L1D read refills | 555 |
| L2 read refills | 985 |
| last-level cache read misses | 1,036 |
| backend memory stall cycles | 31.2k, 21% of the cycles |

**Where the walks are** ([walk-top.txt](walk-top.txt), [annotate.txt](annotate.txt)):
- `gro_list_prepare` 37%, comparing a new skb with the skbs GRO holds;
- `inet_gro_receive` 12%;
- `eth_type_trans` 7%, 67% of it still on the header's load;
- `xeu_rx_poll` 3%, with its largest share (13%) at the header prefetch.

**Where `xeu_rx_poll` waits for L1 refills** ([refill-top.txt](refill-top.txt), [annotate.txt](annotate.txt)):
- about 31% on the completion entry the card has just written (+0xb0, +0xc0, +0xdc);
- 11% at the header prefetch;
- the descriptor entry a few percent.

## Why there are walks: the linear map

[pagewalk.py](pagewalk.py) walks `swapper_pg_dir` through `/proc/kcore` ([pagewalk.txt](pagewalk.txt)):
- **What it finds:** a region buffer at `0xffff0080e0000000` (phys `0x8160000000`), and the one 100 MiB further on, are each mapped by a level-3 entry, **a 16 KiB page**, not a 32 MiB block.
- **What that costs:** the region's 512 MiB takes 32,768 TLB entries, before the slab memory the skbs live in. No TLB holds that working set, so the rings walk page tables about 69 times a WRITE.

**The cause, from `arch/arm64/mm/mmu.c`:** `force_pte_mapping()` maps the linear map with pages when `rodata_full` is true, unless the CPU supports BBML3 or `debug_pagealloc` is on.
- `rodata_full` defaults to true (`arch/arm64/mm/pageattr.c`).
- `cpu_supports_bbml3()` lists the Neoverse V3 and N3 among others, but not the N2, which is tardis2's CPU (part `0xd49`).
- KFENCE does not force pages here: with `CONFIG_KFENCE_SAMPLE_INTERVAL` 100 its pool is mapped early (`kfence_early_init`).
- tardis2's command line says nothing about rodata.

**The setting that gives blocks:** on arm64 `rodata=on` sets `rodata_full` as well. The value that leaves the linear map in blocks is **`rodata=noalias`**:
- the kernel's own text and read-only data stay read-only;
- their aliases in the linear map do not.

## What it shows

- **The descriptor entry's prefetch, the planned round 30, would gain little.** The descriptor load is a few percent of `xeu_rx_poll`'s refills.
- **The receive path's memory stalls are largely TLB walks,** made by a linear map in 16 KiB pages. That cost is on every CPU that touches skbs and buffers, nfsd's included; only the rings were measured.
- **The completion entry the card writes** is the poll's largest refill. Prefetching it further ahead is a possible xeu change, with an uncertain gain, since the card writes it just before the poll reads it.

## The A/B: `rodata=noalias`

**Run 2026-10-27 on tardis2** after Mike rebooted it with `rodata=noalias` (driver [run58.sh](run58.sh), the runs `/var/tmp/zc-run-logs/t2-r58/`):
- **A** is `rodata=full`, before the reboot: round 28's four B runs (r55-2, -4, -6, -8) and this round's look above.
- **B** is `rodata=noalias`. Everything else is the same as A: build 499's own sunrpc (`EFD53C53F61C1F0CDF37D89`) and nfsd (`3F0C341E86E266D5FDF2ADF`), xeu `2C05DF674C9D013185FDB34` with `rx_prefetch` Y, and the settled setting.
- **The runs:** `lat.sh` twice, four untraced loan64 runs, then one loan64 run with this round's look, plus `perf stat` on nfsd's CPUs (8 to 55) beside the rings. Each run came after `settled.sh apply`, an fstrim and 120 s of rest.
- **No copy runs** (see "No copy runs" in [the FLAMEGRAPH README](../README.md)).
- **The boot's xeu:** it loaded the initrd's xeu, older than release 9, so xeu was reloaded once (no NFS client connected) before `bringup.sh 32768 256`. The initrd was rebuilt afterwards (round 28's README).
- **The linear map** ([pagewalk-noalias.txt](pagewalk-noalias.txt)): this boot's region (phys `0x8200000000`) and the address found in 16 KiB pages above are each mapped by a block at level 2, 32 MiB.
- **KUnit:** 7, 52, 7 and 10, no failure and no skip.
- No run raised a WARNING, and none had FIFO drops, ring drops or overruns. Every run was DRIVE-LIMITED, with no drive stepped down ([cost-noalias.txt](cost-noalias.txt)).

| run | GB/s | ring µs a WRITE | nfsd µs a WRITE | segments sent a WRITE | region peak in use, pages | slowest WRITE, ms |
|---|---|---|---|---|---|---|
| r58-1-loan64 | 40.47 | 49.1 | 105.8 | 6.62 | 6228 | 118.7 |
| r58-2-loan64 | 40.51 | 47.4 | 107.9 | 6.49 | 6478 | 117.7 |
| r58-3-loan64 | 40.45 | 49.9 | 106.3 | 6.60 | 6373 | 130.7 |
| r58-4-loan64 | 40.48 | 48.3 | 107.2 | 6.63 | 6341 | 123.2 |
| r58-look-loan64 (perf beside) | 40.50 | 49.5 | 109.3 | 6.57 | 6226 | 123.0 |

| per loaned WRITE | A, `rodata=full` | B, `rodata=noalias` | B − A |
|---|---|---|---|
| rings, µs | 48.48 ± 0.59 | 48.68 ± 1.07 | +0.20 ± 0.61 |
| nfsd, µs | 112.03 ± 1.36 | 106.80 ± 0.94 | **−5.23 ± 0.83 (−4.7%)** |
| segments sent | 6.61 | 6.59 | −0.02 |
| enqueues · wakes · switches | 5.48 · 4.36 · 5.86 | 5.48 · 4.42 · 5.90 | about 0 |

- **nfsd:** about 6.3 standard errors. Every B run (105.8 to 107.9) is below every A run (110.7 to 113.6).
- **The rings:** unchanged.
- **Unchanged:** the rate, segments, enqueues, wakes and switches, and the region's peak (B 6228 to 6478, against A's 6289 to 6476 without r55-8's stall).
- **Across a reboot:** A and B differ by the boot as well as by `rodata`. The pages the region and slab got, and the drives' state, are not the same, so the A/B cannot exclude a part of the 5.2 µs that is the boot's. The counters below say where the change is.

**The look with `rodata=noalias`**, run as above, from 06:39:57 at 36,359 WRITEs a second, over 158,777 WRITEs ([stat-rings-noalias.txt](stat-rings-noalias.txt), [stat-nfsd-noalias.txt](stat-nfsd-noalias.txt)). Per loaned WRITE:

| event | rings, A | rings, B | nfsd's CPUs, B |
|---|---|---|---|
| cycles | 147.7k | 146.0k | 327.6k |
| page-table walks (`dtlb_walk`) | 69 | **0.50** | 4.96 |
| L1 TLB misses (`dtlb-load-misses`) | 1,171 | **191** | 333 |
| L1D read refills | 555 | 544 | 1,794 |
| L2 read refills | 985 | 939 | 4,332 |
| last-level cache read misses | 1,036 | 956 | 2,500 |
| backend memory stall cycles | 31.2k (21%) | 29.6k (20%) | 112.6k (34%) |

- **The walks are gone from the rings,** 69 to 0.5 a WRITE, and the L1 TLB misses fall to a sixth. What walks are left ([walk-top-noalias.txt](walk-top-noalias.txt), 344 samples) are still mostly in `gro_list_prepare` (22%), then `page_pool_alloc_frag_netmem` (12%) and `xeu_rx_refill` (8%).
- **They were not the rings' stall.** The rings' cycles fall 1%, and the untraced runs' rings are unchanged. Round 30 took the walks to be most of the receive path's memory stall. Most of the stall is cache misses, which `rodata` does not change: the backend memory stall is still 20% of the rings' cycles.
- **nfsd's CPUs were not measured on A,** so the B column cannot be set against a before. The 5.2 µs a WRITE that nfsd saves is about 15.6k cycles at 3 GHz. Walks saved there, at round 30's rate on the rings, would account for it, but this A/B does not show that they do.

**Latency** ([latency-noalias.txt](latency-noalias.txt), at iodepth 1): untraced, p50 0.432 and 0.461 ms, mean 0.442 and 0.463, against round 28's B at p50 0.444 and 0.416, mean 0.446 and 0.429. The same. The first latency run overlapped a 12 s rebuild of the initrd, on CPUs 56 to 63 at idle priority, and does not differ from the second.

## The A/B again, with the look on nfsd's CPUs on both sides

**Run 2026-10-27 on tardis2** (driver [run64.sh](run64.sh), the runs `/var/tmp/zc-run-logs/t2-r64-full/` and `t2-r64-noalias/`):
- **A** booted with `rodata=full` (16:19 to 16:53), **B** with `rodata=noalias` (17:22 to 17:56), with a reboot between them.
- **Both sides:** the tip's sunrpc (`A06FE478658242A10E100FF`), the kernel's nfsd (`3F0C341E86E266D5FDF2ADF`), xeu `2C05DF674C9D013185FDB34` with `rx_prefetch` Y, and the settled setting (A's audit differs only in `rodata`).
- **The runs:** the same as run58.sh's, with one addition. The look runs `perf stat` and samples `dtlb_walk` on nfsd's CPUs (8 to 55) as well as on the rings, on both sides.
- No run raised a WARNING or had an overrun. Every run was DRIVE-LIMITED, with no drive stepped down.
- **A's r64-4:** its total for the whole run is 36.08 GB/s, with a third fewer loans admitted. Its measured window is like the others' (38,598 WRITEs a second, 40.74 GB/s on the wire), so it is kept.

**Cost a loaned WRITE** ([cost-r64.txt](cost-r64.txt), the four untraced runs a side):

| per loaned WRITE | A, `rodata=full` | B, `rodata=noalias` | B − A |
|---|---|---|---|
| rings, µs | 47.55 ± 0.56 | 44.60 ± 0.99 | **−2.95 ± 0.57 (−6.2%)** |
| nfsd, µs | 100.50 ± 2.56 | 91.67 ± 1.43 | **−8.83 ± 1.47 (−8.8%)** |
| region peak in use, pages | 6292 to 6877 | 6318 to 6597 | about the same |

- **nfsd:** about 6.0 standard errors. Every B run (90.3 to 93.5) is below every A run (97.6 to 102.7).
- **The rings:** about 5.2 standard errors. Every B run (43.5 to 45.7) is below every A run (47.0 to 49.0). Round 30's first A/B found no change on the rings; this one finds 3 µs.
- **The base is different from the first A/B.** That one used build 499's own sunrpc. This one uses the tip's, which is why nfsd is at 100 µs here and was at 112 there.

**The look, per loaned WRITE** ([stat-nfsd-r64-full.txt](stat-nfsd-r64-full.txt), [stat-nfsd-r64-noalias.txt](stat-nfsd-r64-noalias.txt), [stat-rings-r64-full.txt](stat-rings-r64-full.txt), [stat-rings-r64-noalias.txt](stat-rings-r64-noalias.txt); 157,396 and 157,877 WRITEs):

| event | nfsd's CPUs, A | nfsd's CPUs, B | rings, A | rings, B |
|---|---|---|---|---|
| cycles | 312.7k | 293.0k | 143.6k | 141.7k |
| page-table walks (`dtlb_walk`) | **108** | **2.1** | 60.6 | 0.29 |
| L1 TLB misses (`dtlb-load-misses`) | 1,306 | 311 | 1,044 | 181 |
| L1D read refills | 1,575 | 1,535 | 515 | 548 |
| L2 read refills | 4,773 | 4,030 | 946 | 908 |
| last-level cache read misses | 2,731 | 2,373 | 992 | 903 |
| backend memory stall cycles | 101.0k (32%) | 92.8k (32%) | 29.2k (20%) | 28.0k (20%) |

- **This is the before that the first A/B lacked.** On `rodata=full`, nfsd's CPUs walk page tables 108 times a WRITE. With the linear map in blocks that falls to 2.
- **The cycles agree with the untraced runs.** nfsd's CPUs spend 19.7k fewer cycles a WRITE, about 6.6 µs at 3 GHz, against −8.8 µs untraced. The rings spend 1.9k fewer cycles, about 0.6 µs, against −3.0 µs.
- **The memory stall falls with the cycles** (−8.2k cycles on nfsd's CPUs), as does the count of L2 refills. That fits walks that miss the cache: a walk's table loads are refills of their own.
- **Where nfsd's CPUs walk on A** ([walk-top-nfsd-r64-full.txt](walk-top-nfsd-r64-full.txt)): about a twelfth of the samples are in `_raw_spin_unlock_irqrestore`, where interrupts and softirqs land. After that come `svc_tcp_rx_prefetch_pages` (4.4%), `__memcg_slab_free_hook` (4.1%), `__pi_memset_generic` and `kmem_cache_free`. These are the receive path's page and slab work.
- **On B too few walks are left to place.** The B profile ([walk-top-nfsd-r64-noalias.txt](walk-top-nfsd-r64-noalias.txt)) includes process setup and teardown (`copy_present_ptes`, `zap_present_ptes`, `el0_da`). Those are most likely the look's own shell loop.

**Latency** ([latency-r64.txt](latency-r64.txt), at iodepth 1): untraced, p50 0.420 and 0.424 ms on A against 0.420 and 0.420 on B; the mean 0.431 and 0.434 ms against 0.425 and 0.426. B's p90 is 0.02 ms lower and its p99 0.02 to 0.03 ms lower, a slight gain at most.

## Decision

- **A look; the descriptor-entry prefetch is not built.**
- **`rodata=noalias`:** nfsd 5.23 ± 0.83 µs a WRITE cheaper (−4.7%), with the rings, rate, region and latency the same. The rings' 69 page-table walks a WRITE are gone, but they were not the rings' cost.
- **`rodata=noalias` kept (Mike, 27 October):** `settled.sh` holds the kernel's command line at it (`f20aad763e78`), and a boot without it audits BAD. It costs the linear map's read-only aliases of the kernel's text and read-only data, a security trade-off still to be weighed for Hammerspace's product.
- **The nfsd figure is settled** (run64.sh, both sides measured on the tip's sunrpc):
  - nfsd is 8.83 ± 1.47 µs a WRITE cheaper (−8.8%), and the rings 2.95 ± 0.57 (−6.2%).
  - On nfsd's CPUs, the page-table walks fall from 108 to 2 a WRITE, with 19.7k fewer cycles.
  - The region and latency are the same. The saving is the TLB's.
