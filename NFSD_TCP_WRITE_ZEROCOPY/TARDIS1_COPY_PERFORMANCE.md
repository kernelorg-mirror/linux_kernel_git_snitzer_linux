# Memory copy performance on tardis1

**Since answered (Mike, 2026-10-13): the DIMMs are fully populated,
four at 5200 MT/s, a theoretical 166 GB/s.** No channel is empty, so
the first item under "Where that leaves the search for more" is
closed. A counter of requests to memory puts two ports of NFS at what
the saturated copy asks of it: "The memory itself" in
`TRACING-NFSD-AND-XEU-MEMCPY-TIME.md`.

Measured 2026-10-12 on tardis1 (aarch64 Neoverse-N2, 64 CPUs, one NUMA
node, 16 KiB pages, 32 MiB shared L3, 63 GB of RAM), kernel
7.1.13-21.hs.464. The question was whether the receive copy in copy
mode could be made cheaper by not fetching its destination from memory
before overwriting it. The report as given that day follows, then how
to reproduce it.

---

A copy that does not fetch its destination gains nothing on this machine: the processor already skips that fetch on its own, so my hypothesis from the counters was wrong.

## The standalone copy test

A benchmark copying between buffers far larger than the caches, pinned to nfsd's CPUs:

| method | 1 thread | 8 | 16 | 32 | 48 threads |
|---|---|---|---|---|---|
| libc `memcpy` | 19.0 | 65.9 | 65.7 | 64.2 | 62.6 |
| paired load/store (as the kernel copies) | 18.6 | 66.1 | 68.2 | 67.3 | 63.8 |
| the same with `DC ZVA` on the destination | 11.6 | 38.6 | 39.4 | 42.3 | 46.2 |
| non-temporal stores | 19.0 | 66.1 | 68.1 | 67.2 | 63.6 |

All figures are GB/s.

- **The plain copy fetches one line from memory per line copied**, which is the source alone. There is no destination fetch to remove.
- **`DC ZVA` is slower and non-temporal stores change nothing.**
- **The machine's copy ceiling is 62–68 GB/s and is reached with 8 threads.** More CPUs add nothing.
- **Copying in frame-sized pieces costs little:** 7,740-byte pieces fetch 1.12 lines per line and still carry 60 GB/s.

## What that says about the NFS run

A two-port run at 58.2 GB/s, with the memory fetches sampled:

- **The NFS path fetches 2.2 lines from memory per line of payload.** 58% of those are in the copy itself, which is about 1.26 per payload line and close to what the benchmark's piecewise copy does. The rest is spread thinly across many functions with no single large item.
- **The server's memory writes are at the benchmark's ceiling.** The processor's bus-write counter read 2.1 billion a second in the NFS run and 2.1 billion a second in the saturated benchmark.

So at 58.5 GB/s copy mode is running at roughly 90% of what this machine can copy memory-to-memory by any method. There is no cheap fix inside the copy.

## Where that leaves the search for more

1. **What sets the 62–68 GB/s ceiling?** It is reached at 8 threads, so it is a shared resource, most likely memory bandwidth. The machine has 63 GB of RAM and `dmidecode` shows no DIMM detail, so I cannot tell how many memory channels are populated. If some are empty, filling them is the cheapest gain available and would raise every figure here. You may know the DIMM layout.
2. **Not copying.** The loan is the only software route past the ceiling, and needs drives that take arbitrary pieces.
3. **The 12% in wakeups and context switches** is still worth tracing, but it is CPU time, and CPU is not what is short: nfsd's CPUs are half idle.

The benchmark is `xeu-rig/copybench.c`.

---

## The figures behind it

Piecewise copies, libc `memcpy` at 48 threads, each piece from its own
buffer `STRIDE` bytes on and `SOFF` bytes in, as received frames are:

| pieces | GB/s | lines fetched from memory per line copied |
|---|---|---|
| whole 1 MiB | 62.5 | 1.02 |
| 7740 bytes, 68 in | 60.1 | 1.12 |
| 7740 bytes, 0 in | 60.1 | 1.12 |
| 8192 bytes, 0 in | 62.4 | 1.02 |
| 4096 bytes, 68 in | 57.0 | 1.31 |
| 1448 bytes of 2048, 68 in | 54.2 | 1.50 |

Counters over the whole machine, per second:

| | `l3d_cache_lmiss_rd` | `bus_access_rd` | `bus_access_wr` |
|---|---|---|---|
| paired load/store, 48 threads, 63.7 GB/s | 1.0e9 | 2.0e9 | 2.1e9 |
| `DC ZVA`, 48 threads, 46.2 GB/s | 0.8e9 | 1.5e9 | 1.5e9 |
| non-temporal stores, 48 threads, 63.4 GB/s | 1.0e9 | 2.0e9 | 2.1e9 |
| NFS, two ports, 58.2 GB/s | 1.98e9 | 3.77e9 | 2.1e9 |

In the NFS run 94% of the fetches from memory were on nfsd's CPUs
(8 to 55) and 3% on each port's ring CPUs; by task, 87% in nfsd and
12% in idle. A line of payload is 64 bytes, so 58.2 GB/s is 0.91e9
lines a second.

The earlier profile of the same setting (54.5 GB/s, `warm-two-prof`):
`__memmove` 29.2% of cycles, `default_idle_call` 10.5%,
`_raw_spin_unlock_irqrestore` 7.3%, `finish_task_switch` 4.5%, nothing
else above 1.7%.

## To reproduce

    cd NFSD_TCP_WRITE_ZEROCOPY/xeu-rig
    gcc -O2 -pthread -o /tmp/copybench copybench.c
    # copybench <libc|stp|zva|stnp> <threads> <first cpu> <seconds> [MiB per buffer]
    /tmp/copybench stp 48 8 4
    PIECE=7740 SOFF=68 /tmp/copybench libc 48 8 4
    perf stat -a -e l3d_cache_lmiss_rd,bus_access_rd,bus_access_wr /tmp/copybench stp 48 8 4

    # the NFS run, with the fetches sampled from 100 s in
    RCVBUF=393216 ENTRIES=128 QDISC=fq LINGER=0 ./twoport.sh <out> ab &
    sleep 100; perf record -a -e l3d_cache_lmiss_rd -c 50000 -- sleep 6
    perf report --no-children --sort symbol

Each thread copies between two 64 MiB buffers of its own, 1 MiB at a
time, with transparent huge pages asked for. `PIECE` works with the
`libc` method only. The NFS run is `warm-two-lmiss` in
`xeu-rig/results-twoport-464.tsv`; its perf data is in
`/var/tmp/zc-run-logs/copybench/`.
