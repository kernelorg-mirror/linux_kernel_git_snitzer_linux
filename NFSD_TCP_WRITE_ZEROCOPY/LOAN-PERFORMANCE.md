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

**The mesh counts two of its events at a time.** With five asked
for, as in every reading below, each was counted about 40% of the
time and scaled up by `perf`. For loads that hold steady over the 10
s that is an estimate and not a count; `memcost-cal.sh` asks for two
(`hnf_mc_reqs` and `hnf_cache_fill`) so that the battery's figures
are counted throughout.

Stores alone give about one request for each line stored, so it
counts writes to memory as well as reads. Loads alone and the copy
come out under one a line because 4 MiB buffers partly stay in cache
(which is also how `rd` shows more than 166 GB/s). The most seen is
2.5e9 a second; 166 GB/s is 2.6e9 lines. Not verified beyond that:
exactly what it takes for one request.

## Calibration: the card and the drives by themselves (13 October, 18:30 to 19:00)

`xeu-rig/memcost-cal.sh <out>` is this, as one script. The two
tables below are from the commands run by hand, with five mesh
events and so scaled from 40% of the time; the script's own first
run, counted throughout, is after them and agrees. The figures are
in `/var/tmp/zc-run-logs/cmn-cal/` and `memcost-cal-464/`.

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

### The same as one script, counted throughout (13 October, 19:50)

`./memcost-cal.sh $L/memcost-cal-464`, its `summary.txt`. This is
the table to lay beside the next kernel's:

| | GB/s | requests to memory a second | for each line | cache filled a second |
|---|---|---|---|---|
| idle | 0 | 0.002e9 | | 0.007e9 |
| plain TCP, port a, receivers discard | 49.6 | 0.019e9 | 0.03 | 0.838e9 |
| plain TCP, port b, receivers discard | 49.7 | 0.019e9 | 0.02 | 0.831e9 |
| plain TCP, both ports, receivers discard | 99.6 | 0.391e9 | 0.25 | 1.677e9 |
| plain TCP, both ports, receivers copy | 82.7 | 1.512e9 | 1.17 | 2.857e9 |
| drives writing, queue depth 32 (1 GiB of buffers) | 58.0 | 0.524e9 | 0.58 | 0.157e9 |
| drives reading, queue depth 32 | 105.4 | 1.717e9 | 1.04 | 1.673e9 |
| drives writing, queue depth 1 (32 MiB of buffers) | 94.9 | 0.131e9 | 0.09 | 0.200e9 |
| drives reading, queue depth 1 | 105.4 | 0.358e9 | 0.22 | 1.653e9 |

Every figure is within 0.03 a line of the hand run's but the copy's
(1.17 for 1.36, at 82.7 GB/s for 86.2), which is the one load here
that does not hold steady.

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
- **A loan is not bounded by memory as a copy is** (62 to 68 GB/s
  for a bare copy here, measured). That
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

## The loan on the stand-in (13 October, 19:10 to 19:20)

`plans/16k-36-loan8192-sgl.plan` through `sweep.sh`: a loan cell
with a budget of 16 WRITEs and its copy twin, one port (a), sixteen
connections, 1 MiB held buffer, rings of 256, the linger at 50 us,
NVMe/TCP on this host in front of a 16 GiB file on `/hs/nvme0`. The
counter every 5 s through both cells (`perf stat -I 5000`), with the
port's bytes and the drive's beside it; a cell is 120 s.

    SGL_BACKING=/hs/nvme0/zcbudget-nvmet.img NULLB_MIB=16384 HW_ANY_MTU=1 RX_LINGER_US=50 PLACE=1 \
        PRP_DIR=/hs/nvme0/zcbudget STEP_JOBS=16 STEP_BS=8M ./sweep.sh $L/xeu-16k-464-loanmem-1 plans/16k-36-loan8192-sgl.plan

| | the loan | its copy |
|---|---|---|
| GB/s over the cell (fio) | 11.42 | 7.13 |
| how the WRITEs were issued | direct, all 1,466,728 | direct, all 976,208 |
| WRITEs that held more than they were charged | 0 | |
| region pages in use at most | 4878 (76 MiB) | 3261 (51 MiB) |
| client retransmits, timeouts | 11,035, 0 | 268,691, 5 |
| the stretch the counter is read over | 110 s at 10.3 to 12.0 GB/s | 30 s at 10.3 to 12.0 GB/s |
| requests to memory a second | 0.70e9 | 1.06e9 |
| **for each line of payload** | **3.99** (3.92 to 4.15) | **5.72** (5.67 to 5.96) |
| lines filled into the cache, a line of payload | 3.05 | 4.66 |
| lines the cores count fetched from beyond the L3 | 3.00 | 4.09 |

- **A loaned WRITE is still written direct, every one**, on build
  464 with the charge learned by the kernel, the scripts reaching
  the client by its management address and the backing file on one
  drive. Nothing held more than it was charged.
- **The loan asks 1.7 fewer requests of memory for each line than
  its copy** (3.99 for 5.72), which is about what a copy was found
  to cost by itself (2.17 a line on two ports to the drives).
- **The stand-in is most of both figures.** About 4 requests a line
  are there with no copy by nfsd at all: NVMe/TCP on the same host
  sends the loaned pages through TCP to itself and receives them
  into its own buffers before the file is written. It is a copy
  made later by someone else, and then some. So the 3.99 is not
  what an SGL drive would see; the calibration above (about 0.1 to
  1.6) is the better guide to that, and this run's use is the
  difference between the two cells.
- **One drive holds both cells to about 11 GB/s**, where the stripe
  let the stand-in reach 20 to 22. The loan held to that for the
  whole cell. The copy did not: its cell averaged 7.13 with 268
  thousand retransmits, and only 30 s of it were steady enough to
  read the counter over. The drive was past its fast region by then
  (the loan's cell had just written 1.3 TB to it), so the copy's
  rate here is not a finding; its requests a line, read over the
  steady stretch at the loan's rate, are.
- The loan held 76 MiB of the region, more than twice the cache,
  so much of what it received was evicted before the stand-in took
  it. How much a loan holds in front of drives that finish a write
  quickly is the open figure.

Read with the five mesh events, so each counted 40% of the time.
The samples are `$L/loanmem-1.cmn.txt` (the counter),
`loanmem-1.srv.txt` (the port) and `loanmem-1.drv.txt` (the drive
and the region).

## The loan against its budget, each cell inside the drive's fast region (13 October, 19:30 to 19:45)

`plans/16k-42-loan-memcost.plan`: the loan with 16, 8 and 4 WRITEs
let in at once and its copy, cells of 50 s (`STEP_SECS=50`, new in
`run-budget.sh`) with 30 s of idle before each (`CELL_REST=30`, new
in `sweep.sh`), so that the one drive is at full rate throughout.
`memcost-log.sh` beside it and `memcost-read.py` to read it, the
mesh's two events counted the whole time:

    ./memcost-log.sh $L/loanmem-2 1500 enp161s0f0 nvme0n1 &
    SGL_BACKING=/hs/nvme0/zcbudget-nvmet.img NULLB_MIB=16384 HW_ANY_MTU=1 RX_LINGER_US=50 PLACE=1 \
        PRP_DIR=/hs/nvme0/zcbudget STEP_JOBS=16 STEP_BS=8M STEP_SECS=50 CELL_REST=30 \
        ./sweep.sh $L/xeu-16k-464-loanmem-2 plans/16k-42-loan-memcost.plan
    ./memcost-read.py $L/loanmem-2 enp161s0f0

| | loan, 16 WRITEs | loan, 8 | loan, 4 | the copy |
|---|---|---|---|---|
| budget, pages | 2150 | 1142 | 638 | 0 |
| GB/s over the cell (fio) | 11.74 | 11.74 | 10.59 | 10.35 |
| WRITEs written direct | all 719,736 | all 719,656 | all 645,456 | all 657,096 |
| WRITEs over their charge | 0 | 0 | 0 | |
| WRITEs refused admission and retried | 1,535,632 | 1,532,890 | 1,380,139 | |
| region in use, steady / at most | 76 / 78 MiB | 62 / 65 MiB | 59 / 60 MiB | 49 / 51 MiB |
| client retransmits, timeouts | 1,576, 0 | 1,382, 2 | 471, 0 | 248,392, 7 |
| requests to memory a second | 0.75e9 | 0.55e9 | 0.38e9 | 1.06e9 |
| **for each line of payload** | **4.07** (4.03 to 4.09) | **2.99** (2.94 to 3.35) | **2.28** (2.11 to 2.62) | **5.75** (5.65 to 6.78) |
| lines filled into the cache, a line of payload | 3.09 | 2.97 | 2.95 | 4.70 |

- **The pair, at a steady rate: 4.07 against 5.75.** The first
  run's 3.99 and 5.72 repeat, now with both cells inside the drive's
  fast region and the counter not scaled. The loan also carries 13%
  more than its copy here (11.74 for 10.35) with a hundredth of the
  retransmits.
- **A smaller budget costs the loan less in memory, as the
  calibration said it would.** Halving the budget from 16 WRITEs to
  8 takes the region in use from 76 MiB to 62 and the requests a
  line from 4.07 to 2.99, at the same rate. At 4 WRITEs it is 2.28,
  for a tenth of the rate (10.59). What a loan holds waiting for the
  storage is what it pays for in memory.
- **The stand-in's own share can now be bounded.** The cache fills
  hardly move across the three loans (3.09, 2.97, 2.95 a line): that
  is the card's writes and the stand-in's passage of the data
  through TCP to itself, and it is there whatever the budget. The
  requests to memory are what fall as the footprint does.
- At the smallest budget the loan on this stand-in asks 2.28
  requests a line, about what a copy straight to the drives asks
  (2.17 on two ports), while carrying the stand-in's extra passage.
  How far it would fall in front of drives that take the pages
  themselves is the calibration's 0.1 to 1.6, and where in that it
  lands depends on the same footprint.
- Even the smallest footprint here, 59 MiB, is nearly twice the
  cache. The region in use is not only what loans hold: sixteen
  connections' held receive buffers and the rings are in it too (a
  copy holds 49 MiB at this setting). A loan cell with a smaller
  held buffer and shallower rings has not been run with the
  counter.

## The loan with a smaller footprint: rings of 128 and a 384 or 256 KiB held buffer (13 October, 20:00 to 20:20)

`plans/16k-43-loan-memcost-small.plan`, run and read as the one
before it (`loanmem-3`), the counter counted throughout:

| held buffer | | loan, 8 WRITEs | loan, 4 | the copy |
|---|---|---|---|---|
| 384 KiB | GB/s over the cell (fio) | 11.73 | 8.40 | 10.41 |
| | region in use, steady / at most | 48 / 50 MiB | 43 / 46 MiB | 37 / 38 MiB |
| | client retransmits, ring drops | 1,974, 646 | 163, 207 | 21,660, 17,966 |
| | **requests to memory a line of payload** | **2.00** (1.87 to 2.25) | **0.93** (0.88 to 1.22) | **5.02** (4.94 to 5.64) |
| | lines filled into the cache, a line | 2.71 | 2.54 | 4.48 |
| 256 KiB | GB/s over the cell (fio) | 11.72 | 7.45 | 10.41 |
| | region in use, steady / at most | 46 / 49 MiB | 41 / 45 MiB | 37 / 37 MiB |
| | client retransmits, ring drops | 1,371, 203 | 72, 120 | 3,952, 171 |
| | **requests to memory a line of payload** | **1.86** (1.79 to 2.02) | **0.80** (0.71 to 1.07) | **4.90** (4.58 to 5.31) |
| | lines filled into the cache, a line | 2.67 | 2.51 | 4.47 |

Every loaned WRITE was written direct in all four loan cells
(718,808, 508,528, 717,472 and 453,680) and none held more than it
was charged.

Across the three runs, the loan at the stand-in's full rate (11.7
GB/s) with what it held and what it asked of memory:

| rings, held buffer, budget | region in use | requests to memory a line | its copy at the same rings and buffer |
|---|---|---|---|
| 256, 1 MiB, 16 WRITEs | 76 MiB | 4.07 | 5.75 |
| 256, 1 MiB, 8 | 62 MiB | 2.99 | 5.75 |
| 128, 384 KiB, 8 | 48 MiB | 2.00 | 5.02 |
| 128, 256 KiB, 8 | 46 MiB | 1.86 | 4.90 |

- **The loan's cost in memory follows what it holds, all the way
  down**: from 4.07 requests a line at 76 MiB to 1.86 at 46 MiB,
  with no loss of rate. At the smallest setting it asks under two
  fifths of what its copy does (1.86 for 4.90), and less than a
  copy straight to the drives (2.17) though it still carries the
  stand-in's own passage of the data.
- **The copy does not move**: 5.75, 5.02, 4.90. Its footprint
  falls as well (49 MiB to 37) and it gains little, because what a
  copy pays for is its destination, which is outside the region and
  as large as ever.
- **With 4 WRITEs let in it is under one request a line** (0.93
  and 0.80), and the rate goes with it: 8.40 and 7.45 GB/s for
  11.7. Sixteen connections share four WRITEs' worth of budget and
  most of them wait. That is the budget holding the load back, not
  memory. A budget of 8 is the smallest here that carries the
  stand-in's full rate.
- Even 41 MiB is more than the 32 MiB cache, and the stand-in adds
  its own data in flight to what the region shows. So the figures
  are still falling at the smallest setting tried: the floor on
  this stand-in has not been found.
- The copy at 384 KiB dropped 17,966 frames at rings of 128 and
  retransmitted 21,660 segments; at 256 KiB, 171 and 3,952. The
  loans dropped a few hundred at most.

What it says for drives that take the pages themselves: nothing
directly, as before. But the one thing the calibration predicted
that could be tested here has held three times over. A loaned
byte's cost in memory is set by how much is waiting, and the budget
and the held buffer are the controls for that.

## Dialing in loan mode

Copy mode was dialed in by its rings and its buffers. Those dials
all still apply to a loan, and one is added that matters most: the
budget. What changes is what they are tuned against. In copy mode
the aim was that no data should wait; in loan mode it is how much
is allowed to wait.

**Fixed, not dialed:**

- 8192-byte receive buffers at an MTU of 7792 (`rx_buf_size=8192`).
  For a copy it was the best setting. For a loan it is required:
  one buffer to a frame is what puts every piece on a 4-byte
  boundary, and without that a loaned WRITE is not written direct.
- Every connection placed on a ring (`PLACE=1`).
- The charge for a WRITE: the kernel learns it.

**Dialed, and what each trades:**

| dial | too small | too large |
|---|---|---|
| the budget, as WRITEs let in (`svc_tcp_rx_budget_pages`) | connections wait to be let in and the rate falls | loans hold more than the cache and each byte asks more of memory |
| the held receive buffer (`svc_tcp_rx_rcvbuf`) | a connection waits for data | unread data sits in the region on top of the loans |
| ring depth | frames dropped in bursts | more memory written through before a buffer comes round again |
| rings a port | one ring's CPU runs out | more posted memory for the same traffic |

On the stand-in, sixteen connections: 16 WRITEs let in carry the
full rate at 4.07 requests to memory a line; 8 carry it at 2.99,
and at 1.86 with rings of 128 and a 256 KiB buffer; 4 are under one
a line with a third of the rate gone.

**The stand-in's answer will not be the drives'.** The right budget
is about how many WRITEs are in flight at once: the rate times how
long the storage takes to finish one. The stand-in is slow to
finish a write and carries 11.7 GB/s. SGL drives will finish one
far sooner and are to carry five times the rate over two ports.
Those pull opposite ways, so 8 is where to start looking and no
more.

**One thing a copy never had to mind:** a 16 KiB page holds two
8192-byte buffers, so two frames share it, and they may be of
different connections. The page is out on loan until both are
written. The learned charge allows for it (164 pages for each 100 of
WRITE on this card, where frames that paired up would be 110), but
a loan's footprint is that much more than the count of WRITEs let
in suggests.

**What a footprint is, for these tables.** What the load takes from
the region beyond what was there idle, plus the buffers posted in
the rings of the port carrying it (8 MiB at four rings of 256, 4 at
128, 2 at 64). A ring hands its buffers to the card in turn, so its
whole posted set is memory the card writes through, held or not.
The other port's posted buffers in a one-port run are not touched
and do not count; nor do pages parked in a pool, while they stay
parked. The target is that footprint, for both ports together,
inside the 32 MiB cache.

**The order, when the drives are here:**

1. That every loaned WRITE is written direct (`wr_direct` against
   `wr_dontcache`), at copy mode's rings and buffer and a generous
   budget.
2. The smallest budget that carries the rate: stepped down, reading
   the rate and the WRITEs refused admission.
3. Then the held buffer, then the ring depth, down until the rate
   falls or frames are lost, and back one step.
4. At each step the footprint and the requests to memory for each
   line (`memcost-log.sh`, `memcost-read.py`).
5. Again on two ports, where the budget and the region are shared.

Not known: whether a loan wants a smaller held buffer than copy
mode's 384 KiB to 1 MiB (256 KiB cost nothing on the stand-in, which
is not fast enough to show a connection starved of data), and what
the two-port setting is at all, since `sweep.sh` is one port.

## What is not measured, and cannot be on this host

A loan written direct to a drive that takes arbitrary pieces, with
nothing in between. These drives need whole pages, so a loan to
them goes through the page cache (a copy, later); the stand-in adds
its own. What this host can still give:

- the floor on the stand-in: rings of 64 or eight connections, to
  bring the region in use under 32 MiB with a budget that still
  carries the rate (6 WRITEs has not been tried);
- the same pair on two ports, which `sweep.sh` does not do.

Done since this was first written, above: the pair with each cell
inside the drive's fast region, the loan against its budget, and
the loan with shallower rings and a smaller held buffer.

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
| the loan at three budgets and its copy on the stand-in | `plans/16k-42-loan-memcost.plan` through `sweep.sh` with `memcost-log.sh` beside it, as above | GB/s, written direct or not, region held, requests a line (4.07, 2.99, 2.28 and 5.75 on build 464) |
| the same with rings of 128 and a 384 and 256 KiB held buffer | `plans/16k-43-loan-memcost-small.plan`, the same way; put the port's rings back to 256 after it | the same (2.00, 0.93, 5.02; 1.86, 0.80, 4.90) |

`memcost-log.sh <prefix> <seconds> <port> [drives]` beside a
`twoport.sh` run and `memcost-read.py <prefix> <port>` after it give
copy mode's requests a line the same way; `cmn-two-1` had the
counter read by hand 30 s into the load (2.17, scaled from 40%).
For two ports the reader takes one port's bytes: run it for each
and add, or log `enp161s0f0` and double.
