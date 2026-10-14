# Loan performance: how much memory a loaned byte uses

Started 2026-10-13, evening, on tardis1 (aarch64 Neoverse-N2, 64
CPUs, one 32 MiB L3, four DIMMs at 5200 MT/s: 166 GB/s in theory;
16 KiB pages, build 464). Copy mode on two ports is at the memory's
limit (`TRACING-NFSD-AND-XEU-MEMCPY-TIME.md`): a copy sends every
byte of payload through memory twice. The loan does not copy, and
is what NVMe drives with SGLs are for when they come. The question
here (Mike, 13 October) is how much memory a loaned byte uses, and
so what bounds a loan on this machine.

**This is to be run again, all of it, on the 7.3-rc7 kernel the
series is about to be rebased onto (Mike, 13 October), with one-port
and two-port copy-mode runs beside it to show no regression.** So
each step here is a script and a table that can be laid beside the
next kernel's. "The battery" at the end is the list.

The page for it is "NFSD Loan Performance",
https://claude.ai/artifact/1hAkQK1xDiqPr1efdv4ZDb (source
`artifacts/loan-performance.html`).

**In short.** The card writes what it receives into the cache and a
drive takes what it writes from the cache, so memory is used only
for what falls out of the cache in between. A loaned byte still in
the cache when the drive takes it uses about 0.1 bytes of memory
traffic, where a copied byte uses 2.17. On the NVMe/TCP stand-in the
loan's use falls with its footprint, from 4.07 at about 37 MiB to a
floor near 1.3 at 17 to 18 MiB, and that floor is the stand-in's
own; its copy stays at 4.7 to 5.75 whatever it holds.

## The measure: bytes through memory for each byte of payload

`arm_cmn/hnf_mc_reqs/`, from the mesh's PMU, counts the requests
the cache's home nodes make of the memory controller: a 64-byte
line fetched from the DIMMs or written out to them. The cores' own
counters (`l3d_cache_lmiss_rd`, `bus_access_*`) cannot tell a line
that came from memory from one that came from elsewhere in the
cache; this one can.

Every figure below is that count, taken as 64 bytes a request,
divided by the bytes of payload moved in the same time: **bytes
through memory for each byte of payload**. 2 means two bytes went
through the DIMMs for every byte the server took in. The figure
times the rate is the memory traffic, which has to stay under what
the DIMMs can move: copy mode's 2.17 at 60.4 GB/s is about 131 GB/s
of 166.

    perf stat -a -e arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_fill/,l3d_cache_lmiss_rd,bus_access_wr -- sleep 10

**The mesh counts two of its events at a time.** Ask for two
(`hnf_mc_reqs` and `hnf_cache_fill`, the lines filled into the
cache), as the scripts do. With more, each is counted part of the
time and scaled up by `perf`; the first evening's hand readings had
five and were counted 40% of the time, and where a figure below is
from those it says so.

Against loads whose memory traffic is known (`xeu-rig/copybench`, 16
threads from CPU 8, 4 MiB buffers; five events):

| | GB/s | requests to memory a second | bytes through memory for each byte |
|---|---|---|---|
| idle | 0 | 0.002e9 | |
| stores alone (`wr`) | 152.4 | 2.49e9 | 1.05 |
| loads alone (`rd`) | 177.6 | 2.03e9 | 0.73 |
| a copy (`stp`) | 73.7 | 1.91e9 | 1.66 |

Stores alone come out at one for one, so it counts writes to memory
as well as reads. Loads alone and the copy come out under what they
would be (1 and 2) because 4 MiB buffers partly stay in cache, which
is also how `rd` shows more than 166 GB/s. The most seen is 2.5e9
requests a second, 160 GB/s. Not verified beyond that: exactly what
it takes for one request.

## Finding 1: the card and the drives both move data through the cache

`xeu-rig/memcost-cal.sh <out>`: no NFS, the card receiving and the
drives writing and reading by themselves. Its `summary.txt` from
`$L/memcost-cal-464` (13 October, 19:50, two events, counted
throughout), which is the table to lay beside the next kernel's:

| | GB/s | requests to memory a second | bytes through memory for each byte | lines filled into the cache a second |
|---|---|---|---|---|
| idle | 0 | 0.002e9 | | 0.007e9 |
| plain TCP, port a, receivers discard unread | 49.6 | 0.019e9 | **0.03** | 0.838e9 |
| plain TCP, port b, the same | 49.7 | 0.019e9 | **0.02** | 0.831e9 |
| plain TCP, both ports, the same | 99.6 | 0.391e9 | **0.25** | 1.677e9 |
| plain TCP, both ports, receivers copy into a small buffer | 82.7 | 1.512e9 | **1.17** | 2.857e9 |
| drives writing, queue depth 32 (1 GiB of buffers, far more than the cache) | 58.0 | 0.524e9 | **0.58** | 0.157e9 |
| drives writing, queue depth 1 (32 MiB of buffers, about the cache) | 94.9 | 0.131e9 | **0.09** | 0.200e9 |
| drives reading, queue depth 32 | 105.4 | 1.717e9 | **1.04** | 1.673e9 |
| drives reading, queue depth 1 | 105.4 | 0.358e9 | **0.22** | 1.653e9 |

Plain TCP is `MODE=trunc|copy ./iperf2port.sh <a|b|ab> 16 30`,
sixteen streams a port. The drives are `fio`, direct, four jobs a
drive on all eight, 1 MiB requests, 25 s of idle before each turn.
The same by hand an hour earlier (`$L/cmn-cal`, five events) agrees
to within 0.03 everywhere but the copy (1.36 for 1.17), which is
the one load here that does not hold steady.

- **What the card receives is written into the cache, not into
  memory.** One port brings in 49.6 GB/s and under 2 GB/s of it
  touches the DIMMs. The cache's fill counter reads 0.84e9 lines a
  second, the payload and its headers. With nothing holding the
  data its pages go round inside the cache and are overwritten
  there.
- **With both ports some of it spills**: a quarter of a byte for
  each byte at 99.6 GB/s.
- **Reading it is what costs**: 1.17 with a copy, though the copy's
  own destination is a small buffer that stays in cache.
- **A drive's transfers go through the cache as well.** Data a
  drive is to write that is still in the cache is taken from there
  (0.09); data that has fallen out of it is fetched from memory
  (0.58). What a drive reads fills the cache and reaches memory
  only if it is evicted before it is overwritten.

So, before any loan is run:

| a byte of payload | bytes through memory | from |
|---|---|---|
| loaned, and still in the cache when the drive takes it | about 0.1 | 0.03 arriving on one port, 0.09 leaving |
| loaned, and pushed out of the cache before the drive takes it | about 1.6 | written out once, 0.58 to fetch it back |
| copied, two ports of NFS as measured | 2.17 | `cmn-two-1`: 2.05e9 requests a second at 60.4 GB/s (five events) |

- **What decides a loan's use of memory is how much received data
  is waiting for the drives, against the cache.** That is what the
  region, the budget and the held buffer bound, and why they were
  built small.
- **A loan is not bounded by memory as a copy is.** A copy's
  measured limit here is the bare benchmark's 62 to 68 GB/s. Both
  passes of a loaned byte go through the cache, and kept there it
  uses about a twentieth of what a copied one does.
- **A copy's 2.17 is mostly its destination.** nfsd's buffers are
  far larger in all than the cache, so what is copied into them
  goes out to memory and is fetched back for the drive. The card's
  side of a copy is cheap for the same reason a loan's is.

## Finding 2: on the stand-in, the less a loan holds the less memory it uses

### The stand-in, and what a footprint is

**The stand-in** is NVMe/TCP on tardis1 itself in front of a 16 GiB
file on `/hs/nvme0`. It takes data in arbitrary pieces as an SGL
drive would, so a loan can be written direct to it. It then passes
the data through TCP to itself and receives it into buffers of its
own on the way to the file, on the same CPUs: a copy made later by
someone else, and then some. So its figures are higher than a drive
that takes the pages would show. What to read is the loan against
its copy, and how each moves. The one drive behind it holds every
cell to about 11.7 GB/s; nothing here is a measure of rate.

**A footprint**, in these tables, is what the load takes from the
region beyond what was there idle, plus the buffers posted in the
rings of the port carrying it (8 MiB at four rings of 256, 4 at
128, 2 at 64). A ring hands its buffers to the card in turn, so its
whole posted set is memory the card writes through, held or not.
The other port's posted buffers in a one-port run are not touched
and do not count; nor do pages parked in a page pool, while they
stay parked. `CELL_BOUNCE=1` to `sweep.sh` takes the port down and
up before each cell, which empties its pools back into the region,
so that what the cell then takes is its own and no more; port b is
taken down and up once before the run for the same reason. Without
it a cell can take up pages parked in the pool without the region's
free count moving, and its footprint reads low: the cells marked
"about" below were run that way.

Every cell is one port (a), sixteen connections, `STEP_JOBS=16
STEP_BS=8M`, the linger at 50 us, connections placed on the rings,
50 s long (`STEP_SECS=50`) with 30 s of idle before it
(`CELL_REST=30`) so that the drive is at full rate throughout.
`memcost-log.sh` runs beside the sweep and `memcost-read.py` reads
it:

    ./memcost-log.sh $L/loanmem-4 2400 enp161s0f0 nvme0n1 &
    SGL_BACKING=/hs/nvme0/zcbudget-nvmet.img NULLB_MIB=16384 HW_ANY_MTU=1 RX_LINGER_US=50 PLACE=1 \
        PRP_DIR=/hs/nvme0/zcbudget STEP_JOBS=16 STEP_BS=8M STEP_SECS=50 CELL_REST=30 CELL_BOUNCE=1 \
        ./sweep.sh $L/xeu-16k-464-loanmem-4 plans/16k-44-loan-memcost-floor.plan
    ./memcost-read.py $L/loanmem-4 enp161s0f0

### Every cell

| rings | held buffer | WRITEs let in | GB/s | footprint | bytes through memory for each byte of payload | ring drops | client retransmits | run |
|---|---|---|---|---|---|---|---|---|
| 256 | 1 MiB | 16 | 11.74 | about 37 MiB | 4.07 (4.03 to 4.09) | 359 | 1,576 | 2 |
| 256 | 1 MiB | 8 | 11.74 | about 24 MiB | 2.99 (2.94 to 3.35) | 269 | 1,382 | 2 |
| 256 | 1 MiB | 4 | 10.59 | about 19 MiB | 2.28 (2.11 to 2.62) | 297 | 471 | 2 |
| 256 | 1 MiB | copy | 10.35 | about 10 MiB | 5.75 (5.65 to 6.78) | 6,203 | 248,392 | 2 |
| 128 | 384 KiB | 8 | 11.73 | about 24 MiB | 2.00 (1.87 to 2.25) | 646 | 1,974 | 3 |
| 128 | 384 KiB | 4 | 8.40 | about 13 MiB | 0.93 (0.88 to 1.22) | 207 | 163 | 3 |
| 128 | 384 KiB | copy | 10.41 | about 4 MiB | 5.02 (4.94 to 5.64) | 17,966 | 21,660 | 3 |
| 128 | 256 KiB | 8 | 11.72 | (23 MiB, as below) | 1.86 (1.79 to 2.02) | 203 | 1,371 | 3 |
| 128 | 256 KiB | 4 | 7.45 | about 11 MiB | 0.80 (0.71 to 1.07) | 120 | 72 | 3 |
| 128 | 256 KiB | copy | 10.41 | about 4 MiB | 4.90 (4.58 to 5.31) | 171 | 3,952 | 3 |
| 128 | 256 KiB | 8 | 11.71 | 23 MiB | 1.89 (1.83 to 2.06) | 221 | 1,441 | 4 |
| 128 | 256 KiB | 6 | 10.28 | 20 MiB | 1.39 (1.34 to 1.68) | 162 | 101 | 4 |
| 128 | 192 KiB | 8 | 11.55 | 22 MiB | 1.70 (1.63 to 1.98) | 154 | 1,471 | 4 |
| 128 | 128 KiB | 8 | 10.81 | 21 MiB | 1.62 (1.58 to 1.89) | 121 | 456 | 4 |
| 64 | 256 KiB | 8 | 11.60 | 18.5 MiB | 1.36 (1.27 to 1.47) | 64,803 | 65,708 | 4 |
| 64 | 192 KiB | 8 | 11.43 | 17.6 MiB | 1.34 (1.29 to 1.48) | 19,809 | 20,668 | 4 |
| 64 | 128 KiB | 8 | 10.74 | 16.8 MiB | 1.32 (1.23 to 1.46) | 7,463 | 7,439 | 4 |
| 64 | 128 KiB | 6 | 8.12 | 13.8 MiB | 0.77 (0.74 to 0.92) | 865 | 825 | 4 |
| 64 | 128 KiB | copy | 10.47 | 4.2 MiB | 4.71 (4.65 to 4.76) | 7,898 | 11,666 | 4 |

GB/s is fio's over the cell; the memory figure is the middle of the
2 s readings over the cell's steady part, with their range. Run 2
is `plans/16k-42-loan-memcost.plan` (`loanmem-2`, 19:30), run 3
`16k-43-loan-memcost-small.plan` (`loanmem-3`, 20:00), run 4
`16k-44-loan-memcost-floor.plan` with `CELL_BOUNCE=1` (`loanmem-4`,
21:35). All three with two mesh events, counted throughout.

Every loaned WRITE in every loan cell was written direct
(`wr_direct`, none `wr_dontcache`), 7.3 million in runs 2 to 4, and
none held more than it was charged.

Run 1 (`loanmem-1`, 19:10) was `plans/16k-36-loan8192-sgl.plan`,
the first loan and copy pair, in cells of 120 s with five mesh
events: the loan 11.42 GB/s at 3.99, its copy 5.72 over the 30 s it
ran steadily (the drive was past its fast region by then, and the
copy's cell averaged 7.13 GB/s). Run 2's first and fourth rows are
the same pair done properly, and agree.

### What it shows

- **The loan's use of memory follows its footprint, with no loss of
  rate.** From about 37 MiB held to 18, it falls from 4.07 to 1.34
  while the rate stays at 11.4 to 11.7 GB/s. At 18 MiB the loan
  uses under a third of what its copy does, and less than a copy
  straight to the drives (2.17), though it still carries the
  stand-in's extra passage.
- **It levels off near 1.3, and what is left is the stand-in's.**
  With rings of 64 it is 1.36, 1.34 and 1.32 as the footprint goes
  from 18.5 MiB to 16.8: no longer falling, and all of it well
  inside the 32 MiB cache. What remains is the stand-in passing the
  data through TCP to itself and into buffers of its own, which are
  ordinary memory outside the region and which a drive would not
  have. The lines filled into the cache hardly move across all the
  loans (2.5 to 3.1 for each line of payload): the card's writes
  and the stand-in's passage are there whatever is held.
- **It goes lower only with the rate.** 6 WRITEs let in give 1.39
  at 10.3 GB/s and 0.77 at 8.1; 4 give 2.28, 0.93 and 0.80 at 10.6,
  8.4 and 7.5. At a lower rate the stand-in has less of its own in
  flight. That is not a setting to want.
- **The copy does not move**: 5.75, 5.02, 4.90, 4.71. It holds
  next to nothing in the region beyond its rings, 10 MiB falling to
  4, as a copy always has here, and gains little from the same
  changes, because what a copy pays for is its destination, which
  is outside the region and as large as ever. The difference
  between a loan and its copy at one setting, 1.7 to 3.4, is of the
  size a copy was found to cost by itself.
- **Too small a budget costs rate, not memory.** Sixteen
  connections sharing 4 or 6 WRITEs' worth of budget mostly wait:
  6 gives up a tenth of the rate and 4 a third. Eight is the
  smallest here that carries the stand-in's rate.
- **A held buffer of 128 KiB starts to cost rate** (10.7 to 10.8
  GB/s for 11.4 to 11.7 on either ring depth); 192 KiB does not.
- **Rings of 64 reach the floor and pay for it in loss**: 7 to 65
  thousand frames dropped at the rings in a cell and as many
  retransmits, most with the larger held buffer. Rings of 128 drop
  a couple of hundred.
- **The loan carries about 13% more than its copy** on the stand-in
  (11.74 for 10.35 at rings of 256), with far fewer retransmits.

**On this stand-in, then:** the best setting that loses nothing is
rings of 128, a 192 to 256 KiB held buffer and 8 WRITEs let in, at
1.7 to 1.9 and 22 to 23 MiB; the floor is about 1.3 at 17 to 18
MiB; and below that the stand-in itself is what is being measured.
For drives that take the pages themselves the stand-in's share
goes, and finding 1's 0.1 for data still in the cache is what a
footprint of 17 to 23 MiB points at. That is an inference until
there is such a drive.

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
| the budget, as WRITEs let in (`svc_tcp_rx_budget_pages`: 2150 pages for 16, 1142 for 8, 638 for 4) | connections wait to be let in and the rate falls | loans hold more than the cache and each byte uses more memory |
| the held receive buffer (`svc_tcp_rx_rcvbuf`) | a connection waits for data | unread data sits in the region on top of the loans |
| ring depth | frames dropped in bursts | more memory written through before a buffer comes round again |
| rings a port | one ring's CPU runs out | more posted memory for the same traffic |

The budget and the held buffer limit different things, and they
add. The held buffer caps what each connection has received and
nfsd has not yet read: sixteen connections at 1 MiB can have 16
MiB of the region taken up that way. The budget caps the pages out
on loan and counts nothing else.

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

**The target** is the footprint, for both ports together, inside the
32 MiB cache.

**The order, when the drives are here:**

1. That every loaned WRITE is written direct (`wr_direct` against
   `wr_dontcache`), at copy mode's rings and buffer and a generous
   budget.
2. The smallest budget that carries the rate: stepped down, reading
   the rate and the WRITEs refused admission.
3. Then the held buffer, then the ring depth, down until the rate
   falls or frames are lost, and back one step.
4. At each step the footprint and the bytes through memory for
   each byte of payload (`memcost-log.sh`, `memcost-read.py`, with
   `CELL_BOUNCE=1`).
5. Again on two ports, where the budget and the region are shared.

Not known: whether a loan wants a smaller held buffer than copy
mode's 384 KiB to 1 MiB in front of real drives (192 KiB cost
nothing on the stand-in, which is not fast enough to show a
connection starved of data), and what the two-port setting is at
all, since `sweep.sh` is one port.

## What is not measured, and cannot be on this host

A loan written direct to a drive that takes arbitrary pieces, with
nothing in between. These drives need whole pages, so a loan to
them goes through the page cache (a copy, later); the stand-in adds
its own. So neither the memory a loaned byte uses in front of such
a drive nor the rate a loan reaches is here.

What this host could still give: the same pairs on two ports, which
`sweep.sh` does not do; and how much of the stand-in's 1.3 is which
of its passages, which would take tracing the stand-in and says
nothing of a drive.

## The battery, for this kernel and the next

Run on build 464, and to be run again on the 7.3-rc7 rebase, each
run inside the drives' fast region with `drives.txt` read first:

| step | how | what to lay side by side |
|---|---|---|
| the counter against known traffic | `copybench rd|wr|stp 16 8 5 4` under the `perf stat` line above | bytes through memory for each byte, as in "The measure" |
| the card and the drives alone | `xeu-rig/memcost-cal.sh <out>` | `summary.txt`, as in finding 1 |
| copy mode, two ports | `RCVBUF=393216 ENTRIES=128 LINGER=0 LOWAT=0 ./twoport.sh <out> ab`, three runs | GB/s (60.0 to 60.4 on build 464); bytes through memory (2.17) |
| copy mode, one port on eight drives | `FS_A="/hs/nvme0 ... /hs/nvme7" RUNTIME=60 LOWAT=0 ./twoport.sh <out> a`, and `b`, three each | GB/s (46.5 to 47.4 on build 464) |
| the loan and its copy on the stand-in | the three plans, `16k-42`, `16k-43` and `16k-44`, through `sweep.sh` with `CELL_BOUNCE=1` and `memcost-log.sh` beside each, as written out above | the table under "Every cell" |

- Take port b down and up once before the loan plans (`ip link set
  enp164s0f0 down; ip link set enp164s0f0 up`, then `./ring-irqs.sh
  enp164s0f0 56`), and put port a's rings back to 256 after them
  (`ethtool -G enp161s0f0 rx 256`, then `./ring-irqs.sh enp161s0f0
  0`): a sweep leaves them where its last line had them.
- `memcost-log.sh <prefix> <seconds> <port> [drives]` beside a
  `twoport.sh` run and `memcost-read.py <prefix> <port>` after it
  give copy mode's figure the same way. For two ports the reader
  takes one port's bytes: run it for each and add, or log
  `enp161s0f0` and double.
- The stand-in leaves `nvmet`, `nvmet_tcp` and `nvme_tcp` loaded
  with nothing configured.

The samples behind each run are `$L/loanmem-<n>.cmn.txt` (the
counter), `.srv.txt` (the port) and `.drv.txt` (the drive and the
region), and the sweep's own directory `$L/xeu-16k-464-loanmem-<n>`.
