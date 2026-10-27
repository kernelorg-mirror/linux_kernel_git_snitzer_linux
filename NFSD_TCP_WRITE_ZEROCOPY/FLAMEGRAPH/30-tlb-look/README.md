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

## Decision

- **A look; the descriptor-entry prefetch is not built.**
- **Next (Mike):** decide on `rodata=noalias` for tardis2. It is a reboot and a security trade-off, so it is for Hammerspace's product too.
- **Its measure:** an A/B across a reboot, copy and loan at the settled setting on the current kernel, against the same with `rodata=noalias`.
