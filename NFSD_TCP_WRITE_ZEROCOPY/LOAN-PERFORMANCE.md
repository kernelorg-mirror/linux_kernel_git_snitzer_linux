# Loan performance: what a loaned byte costs in memory

Started 2026-10-13, evening, on tardis1 (aarch64 Neoverse-N2, 64
CPUs, one 32 MiB L3, four DIMMs at 5200 MT/s: 166 GB/s in theory;
16 KiB pages, build 464). Copy mode on two ports is at the memory's
limit (`TRACING-NFSD-AND-XEU-MEMCPY-TIME.md`): a copy sends every
byte of payload through memory twice. The loan does not copy, and
is what NVMe drives with SGLs are for when they come. The question
here (Mike, 13 October) is what a loaned byte costs in memory, and
so what bounds a loan on this machine.

**This is to be run again, all of it, on the 7.3-rc7 kernel the
series is about to be rebased onto (Mike, 13 October), with one-port
and two-port copy-mode runs beside it to show no regression.** So
each step here is a script and a table that can be laid beside the
next kernel's. "The battery" at the end is the list.

## The counter

`arm_cmn/hnf_mc_reqs/`, from the mesh's PMU: the requests the
cache's home nodes make of the memory controller. The cores' own
counters (`l3d_cache_lmiss_rd`, `bus_access_*`) cannot tell a line
that came from memory from one that came from elsewhere in the
cache; this one can. Read with the cache's own traffic beside it:

    perf stat -a -e arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_miss/,arm_cmn/hnf_slc_sf_cache_access/,arm_cmn/hnf_cache_fill/,arm_cmn/hnf_slc_eviction/,l3d_cache_lmiss_rd,bus_access_wr -- sleep 10

What it reads against things whose memory traffic is known
(`xeu-rig/copybench`, 16 threads from CPU 8, 4 MiB buffers, a line
being 64 bytes):

| | GB/s | lines a second | requests to memory a second |
|---|---|---|---|
| idle | 0 | | 0.002e9 |
| loads alone (`rd`) | 177.6 | 2.77e9 | 2.03e9 |
| stores alone (`wr`) | 152.4 | 2.38e9 | 2.49e9 |
| a copy (`stp`) | 73.7 | 1.15e9 | 1.91e9 |

Stores alone give about one request for each line stored, so it
counts writes to memory as well as reads. Loads alone and the copy
come out under one a line because 4 MiB buffers partly stay in cache
(which is also how `rd` shows more than 166 GB/s). The most seen is
2.5e9 a second; 166 GB/s is 2.6e9 lines. Not verified beyond that:
exactly what it takes for one request.

## Calibration: the card and the drives by themselves (13 October, 18:30 to 19:00)

`xeu-rig/memcost-cal.sh <out>` is this, as one script. (It was
written from the commands run by hand that evening and has not yet
been run as a script.) The figures are in
`/var/tmp/zc-run-logs/cmn-cal/`.

### The card writing into memory: plain TCP, no NFS, no drives

`MODE=trunc|copy ./iperf2port.sh <a|b|ab> 16 30`, sixteen streams a
port, the counter over 10 s of it:

| | on the wire | payload lines a second | requests to memory a second | for each line | lines filled into the cache a second | evicted from it |
|---|---|---|---|---|---|---|
| one port (a), receivers discard without reading | 49.83 GB/s | 0.78e9 | 0.017e9 | **0.02** | 0.83e9 | 0.010e9 |
| one port (b), the same | 49.63 | 0.78e9 | 0.020e9 | **0.03** | 0.83e9 | 0.014e9 |
| both ports, the same | 99.75 | 1.56e9 | 0.386e9 | **0.25** | 1.65e9 | 0.368e9 |
| both ports, receivers copy into a small buffer they reuse | 86.17 | 1.35e9 | 1.826e9 | **1.36** | 3.09e9 | 1.358e9 |

- **What the card receives is written into the cache, not into
  memory.** One port brings 0.78e9 lines a second and the memory
  controller is asked for 0.017e9. The cache's fill counter reads
  0.83e9, the payload and its headers. With nothing holding the
  data its pages go round inside the cache and are overwritten
  there.
- **With both ports some of it spills**: a quarter of a request a
  line at 99.75 GB/s, and the evictions match (0.368e9).
- **Reading it is what costs.** With a copy, 1.36 requests a line,
  though the copy's own destination is a small buffer that stays in
  cache: the received lines are evicted one for one (1.358e9) before
  or as they are read.

### The drives reading from memory and writing into it: local, no NFS

`fio`, direct, four jobs a drive on all eight, 1 MiB requests, the
counter over 10 s, 25 s of idle before each:

| | GB/s, all eight | buffers in play | requests to memory a second | for each line | cache filled | evicted |
|---|---|---|---|---|---|---|
| writing, queue depth 32 (held to 7.25 GB/s a drive) | 58.0 | 1 GiB, far more than the cache | 0.533e9 | **0.59** | 0.167e9 | 0.088e9 |
| writing, queue depth 1 | 94.9 | 32 MiB, about the cache | 0.149e9 | **0.10** | 0.230e9 | 0.012e9 |
| reading, queue depth 32 | 105.4 | 1 GiB | 1.714e9 | **1.04** | 1.668e9 | 1.665e9 |
| reading, queue depth 1 | 105.4 | 32 MiB | 0.392e9 | **0.24** | 1.649e9 | 0.392e9 |

- **A drive's transfers go through the cache as well.** Data a
  drive is to write that is still in the cache is taken from there
  (0.10 a line); data that has fallen out of it is fetched from
  memory (0.59).
- **And on the way in**: what a drive reads fills the cache
  (1.65e9 lines a second either way) and reaches memory only if it
  is evicted before it is overwritten, one request a line into 1
  GiB of buffers and a quarter of one into 32 MiB.

## What follows for a loan

Both ends of a loaned byte's journey go through the cache: the card
writes it there and the drive takes it from there. Memory is asked
only for what falls out of the cache in between.

| a byte of payload | requests to memory for each line | from |
|---|---|---|
| loaned, and still in the cache when the drive takes it | about 0.1 | 0.02 arriving on one port, 0.10 leaving |
| loaned, and evicted before the drive takes it | about 1.6 | one eviction, 0.59 to fetch it back |
| copied, two ports of NFS as measured | 2.17 | `cmn-two-1`: 2.05e9 a second at 60.4 GB/s |

- **What decides a loan's cost in memory is how much received data
  is waiting for the drives, against the cache.** That is what the
  region and its budget bound, and why they were built small. A loan
  has held 45 to 50 MiB on the NVMe/TCP stand-in, more than the 32
  MiB L3; what it holds in front of drives that finish a write
  quickly is not known.
- **A loan is not bounded by memory at 83 GB/s as a copy is.** That
  was the inference before this calibration
  (`TRACING-NFSD-AND-XEU-MEMCPY-TIME.md`, "The memory itself") and
  it was wrong: it took the card's writes and the drives' reads for
  memory traffic, and they are that only for what the cache does
  not hold. Kept in cache, a loaned byte asks about a twentieth of
  what a copied one does.
- **A copy's 2.17 is mostly its destination.** nfsd's buffers are
  far larger in all than the cache, so what is copied into them
  goes out to memory and is fetched back for the drive. The card's
  side of a copy is cheap for the same reason a loan's is.

Not shown by any of this: a loan itself. These are the two ends
measured apart, and the stand-in below is the only loan this host
can write direct.

## The loan on the stand-in

(In hand, 13 October, 19:10: `plans/16k-36-loan8192-sgl.plan`
through `sweep.sh`, a loan cell with a budget of 16 WRITEs and its
copy twin, one port, the backing file on `/hs/nvme0`, the counter
every 5 s. To be added here.)

Read it knowing what it is: NVMe/TCP on the same host takes the
loaned pages as they are and then copies them once into its own
buffers on the way to the file, on the same CPUs. So its requests
to memory are a copy's, made later and by someone else. It shows
that a loaned WRITE is written direct and what the loan path itself
holds, not what an SGL drive will see.

## The battery, for this kernel and the next

To be run on build 464 (in hand) and again on the 7.3-rc7 rebase,
each run inside the drives' fast region with `drives.txt` read
first:

| step | how | what to lay side by side |
|---|---|---|
| the counter against known traffic | `copybench rd|wr|stp 16 8 5 4` under the `perf stat` line above | requests a line for each |
| the card and the drives alone | `xeu-rig/memcost-cal.sh <out>` | `summary.txt` |
| copy mode, two ports | `RCVBUF=393216 ENTRIES=128 LINGER=0 LOWAT=0 ./twoport.sh <out> ab`, three runs | GB/s (60.0 to 60.4 on build 464), requests a line (2.17) |
| copy mode, one port on eight drives | `FS_A="/hs/nvme0 ... /hs/nvme7" RUNTIME=60 LOWAT=0 ./twoport.sh <out> a`, and `b`, three each | GB/s (46.5 to 47.4 on build 464) |
| the loan and its copy on the stand-in | `plans/16k-36-loan8192-sgl.plan` through `sweep.sh` | GB/s, written direct or not, region held, requests a line |

`twoport.sh` does not read the counter itself yet; `cmn-two-1` had
it read by hand 30 s into the load.
