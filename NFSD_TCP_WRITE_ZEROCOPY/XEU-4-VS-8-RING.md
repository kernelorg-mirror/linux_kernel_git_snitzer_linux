# Four receive rings a port against eight, on two ports

Started 2026-10-13 on tardis1 (16 KiB pages, build 464), for a day's
deep dive on the two-port rate. Four rings of 128 a port stays the
pinned two-port setting (Mike, 13 October). Eight of 64 is level with
it on rate, and the question here is whether more parallelism in the
receive path opens anything up: the aim is traces of where time goes
on receive into sunrpc and its copy, with the rings' CPUs no longer
near full. This first part is where the interrupts and the receive
CPUs sit in each layout, as measured that morning. Later parts are to
be added below it as they are run.

The accounts of the runs themselves are in `XEU_CONVERGENCE.md`:
"Eight receive rings on CPUs of their own" (12 October, on drives
past their fast region) and "Eight receive rings again, inside the
drives' fast region" (13 October).

## The machine and what is fixed

- 64 CPUs (Neoverse-N2), one NUMA node, one shared 32 MiB L3.
- nfsd: 512 threads in four pools of 128 (`pools_per_node=4`), all
  allowed on CPUs 8 to 55, which is 48 CPUs.
- CPUs 0 to 7 are kept for the rings of `enp161s0f0` (port a) and 56
  to 63 for those of `enp164s0f0` (port b).
- No RPS (`rps_cpus` is 0 on both ports) and NAPI is not threaded, so
  a frame goes through the driver and TCP receive on the CPU its
  ring interrupts. The copy into nfsd's buffer is made later by an
  nfsd thread on one of 8 to 55.
- `xeu` allows at most eight rings a port (`num_rings` 8). A ring is
  a receive ring and a transmit ring, each with its own interrupt
  (`<port>-Rx-<n>`, `<port>-Tx-<n>`), and `ethtool -L combined <n>`
  sets both.
- `xeu` takes only a power of two for a ring's depth: `ethtool -G rx
  96`, `80` and `112` all give 64.
- At every open of a port (down and up, `ethtool -G`, `ethtool -L`)
  the driver puts that port's interrupts on CPUs 0 to 7, whichever
  port it is. `xeu-rig/ring-irqs.sh <port> <first cpu> [<first tx
  cpu>]` places them again and `twoport.sh` runs it after each
  change.
- The two-port setting otherwise: 384 KiB held receive buffer, no
  linger, `svc_tcp_rx_keep=1`, `svc_tcp_rx_lowat=0`, `fq` (in every
  run of this document; unpinned later that day, the default
  `fq_codel` carrying the same) on the
  client, 16 connections a port placed evenly on the rings by the
  RSS table, 8192-byte receive buffers at an MTU of 7792.

## Four rings of 128 a port (pinned)

`ENTRIES=128`, no `RINGS`. A port's receive and transmit interrupts
alternate over its eight CPUs:

| port | Rx-0 to Rx-3 | Tx-0 to Tx-3 |
|---|---|---|
| a, `enp161s0f0` | CPUs 0, 2, 4, 6 | CPUs 1, 3, 5, 7 |
| b, `enp164s0f0` | CPUs 56, 58, 60, 62 | CPUs 57, 59, 61, 63 |

From `r13-4x128-2` (59.56 GB/s), over its 10 s window:

| CPUs | what runs there | softirq | kernel (sys) | idle |
|---|---|---|---|---|
| 0, 2, 4, 6 and 56, 58, 60, 62 | receive: driver and TCP | 80 to 88% | 0 to 1% | 12 to 19% |
| 1, 3, 5, 7 and 57, 59, 61, 63 | transmit: ACKs and replies | 6 to 23% | 0% | 77 to 94% |
| 8 to 55 | nfsd | 0 to 1% | 45 to 47% | 34 to 36% |

- Four connections to a ring; each ring carried 24 to 27% of its
  port.
- All of the driver's receive work and all of TCP receive for 60
  GB/s is on eight CPUs, each near full, while the eight beside them
  are about 85% idle.

## Eight rings of 64 a port, as run on 12 and 13 October

`RINGS=8 ENTRIES=64 TXIRQ_A=8 TXIRQ_B=54`. The eight receive rings
take all eight of the port's CPUs, one each, so the transmit
interrupts have to go elsewhere. They were put on two CPUs at each
edge of nfsd's set, four transmit rings to a CPU:

| port | Rx-0 to Rx-7 | Tx-0 to Tx-7 |
|---|---|---|
| a, `enp161s0f0` | CPUs 0 to 7, one each | CPUs 8, 9, 8, 9, 8, 9, 8, 9 |
| b, `enp164s0f0` | CPUs 56 to 63, one each | CPUs 54, 55, 54, 55, 54, 55, 54, 55 |

From `r13-8x64-2` (59.87 GB/s):

| CPUs | what runs there | softirq | kernel (sys) | idle |
|---|---|---|---|---|
| 0 to 7 and 56 to 63 | receive | 50 to 62% | 0% | 37 to 50% |
| 8, 9, 54, 55 | transmit for a whole port, and nfsd | 26 to 42% | 34 to 42% | 14 to 20% |
| 10 to 53 | nfsd | 1% | 49% | 32% |

- Two connections to a ring; each ring carried 11 to 13% of its
  port.
- The four transmit CPUs are the busiest on the machine in this
  layout, and nfsd's threads are scheduled on them as well.

Without `TXIRQ_A`/`TXIRQ_B`, `ring-irqs.sh` puts a ring's receive
and transmit interrupts on the same CPU (Rx-i and Tx-i on first +
i), which keeps everything on the port's eight. That was run once on
12 October (`r8share-e64-1`, 53.7 GB/s, Rx CPUs 55 to 76% in softirq,
on slow drives) and not on 13 October.

## The rates so far

Two ports, mark off, every run inside the drives' fast region, 13
October. Each cell is round 1, round 2.

| rings a port | GB/s | ring drops, both ports | client retransmits | region pages | Rx CPUs in softirq |
|---|---|---|---|---|---|
| 4 x 128 | 60.70, 59.56 | 17, 4 | 18, 4 | 1216, 1200 | 80 to 89% |
| 8 x 64 | 59.92, 59.87 | 104, 59 | 107, 61 | 1387, 1400 | 49 to 63% |
| 8 x 64, `rx_refill_thresh` 2 | 59.53, 60.45 | 12, 37 | 24, 38 | 1422, 1397 | 49 to 63% |
| 8 x 128, 256 KiB held buffer | 51.89, 52.09 | 0, 0 | 0, 0 | 1808, 1808 | 52 to 58% |

Four runs of 4 x 128 that morning gave 59.6, 60.0, 60.7 and 60.7, so
its own spread is 1.1 GB/s and eight of 64 is inside it. A region
page is 16 KiB: 1216 pages is 19 MiB and 1400 is 22 MiB, both inside
the 32 MiB L3.

## What bears on it

- **The copy ceiling.** `copybench` carries 62 to 68 GB/s memory to
  memory on this machine with 8 CPUs or with 48, and 60.1 in
  7,740-byte pieces as frames arrive here
  (`TARDIS1_COPY_PERFORMANCE.md`). Two ports of NFS at 60 to 60.7 are
  at that figure, with nfsd's CPUs a third idle. If the ceiling is
  memory bandwidth, more parallelism cannot pass it; what it can do
  is show where the path spends memory fetches beyond the copy's own
  (NFS fetches 2.2 lines from memory for each line of payload, the
  bare copy 1.1).
- **Halving the receive CPUs' load did not raise the rate**, on
  either day. They are not what holds two ports.
- **The eight-ring layout that was run is lopsided.** It relieves
  the receive CPUs and loads four CPUs with a port's whole transmit
  side plus nfsd. Tested since, below: it costs nothing.
- **The four-ring layout wastes the other way**: the transmit CPUs
  of each port sit about 85% idle beside receive CPUs near full.
- **Transmit queue selection (XPS)** is as the driver leaves it and
  has not been looked at: with four rings, port a's four transmit
  queues are mapped to CPUs 8, 9, 10 and 11 and port b's to CPUs 32,
  33, 34 and 35, one each. A sender on any other CPU has no queue
  mapped to it and falls back to the flow hash.
- **Where a connection's work lands.** A connection's frames are
  received on its ring's CPU; the nfsd thread that copies them runs
  on any of 8 to 55; the reply and the ACKs go out through a transmit
  ring whose interrupt is on a third CPU. Nothing ties the three
  together today.

## The eight-ring layouts, run (13 October, 11:40 to 12:22 by tardis1's clock)

Six layouts by turns in two rounds, two ports, mark off, 60 s of
idle between runs, every run inside the drives' fast region. Two
knobs were added to `twoport.sh` for it: `TXN=<n>` puts a port's
transmit interrupts on `<n>` CPUs from `TXIRQ_A`/`TXIRQ_B` and not
two, and `NFSD_CPUS=<list>` moves nfsd's threads for the run and
puts them back on 8 to 55 after it.

| layout | Rx interrupts | Tx interrupts | nfsd's CPUs |
|---|---|---|---|
| 4 x 128 (pinned) | every other ring CPU | the ring CPUs between | 8-55 |
| 8 x 64, Tx on 2 | all eight ring CPUs | 8-9 and 54-55, shared with nfsd | 8-55 |
| 8 x 64, sharing | all eight ring CPUs | the same CPU as the ring's Rx | 8-55 |
| 8 x 64, Tx on 4 of their own | all eight ring CPUs | 8-11 and 52-55 | 12-51 |
| 8 x 64, Tx on 8 of their own | all eight ring CPUs | 8-15 and 48-55 | 16-47 |
| 4 x 128, nfsd on 12-51 (control) | as pinned | as pinned | 12-51 |

The control is there because the two "of their own" layouts take 8
and 16 CPUs from nfsd: it shows what that costs by itself. Each cell
is round 1, round 2:

| layout | GB/s | ring drops, both ports | client retransmits | region pages | ring CPUs: softirq, idle | Tx-only CPUs: softirq, idle | nfsd: sys, idle | threads woken a WRITE |
|---|---|---|---|---|---|---|---|---|
| 4 x 128 | 60.06, 61.02 | 5, 25 | 5, 25 | 1200, 1168 | 48, 50 / 51, 50 | | 48, 51 / 33, 30 | 13.7, 13.5 |
| 8 x 64, Tx on 2 | 59.45, 60.27 | 64, 102 | 64, 115 | 1391, 1402 | 58, 60 / 42, 40 | | 47, 51 / 32, 29 | 13.8, 13.1 |
| 8 x 64, sharing | 59.29, 59.55 | 213, 226 | 236, 242 | 1371, 1372 | 70, 71 / 29, 29 | | 50, 52 / 32, 30 | 13.9, 13.7 |
| 8 x 64, Tx on 4 of their own | 59.25, 58.62 | 78, 27 | 86, 28 | 1404, 1406 | 59, 57 / 41, 43 | 13, 14 / 87, 86 | 65, 62 / 19, 22 | 12.8, 13.1 |
| 8 x 64, Tx on 8 of their own | 52.92, 44.70 | 10, 29 | 13, 57 | 1407, 1457 | 49, 39 / 51, 61 | 3, 3 / 96, 97 | 89, 65 / 5, 20 | 5.8, 8.4 |
| 4 x 128, nfsd on 12-51 | 59.62, 59.13 | 22, 12 | 22, 12 | 1248, 1216 | 49, 48 / 50, 52 | 0, 0 / 99, 100 | 67, 63 / 18, 21 | 12.0, 12.8 |

The ring CPUs' figure for four rings averages a port's receive CPUs
(80 to 89% in softirq) with its transmit CPUs (6 to 23%). In the
"Tx on 2" layout the nfsd figure includes CPUs 8, 9, 54 and 55,
which carry transmit as well.

- **No eight-ring layout beats four rings.** The best, transmit on
  two CPUs shared with nfsd, is 59.5 to 60.3 (four runs that day:
  59.5, 59.9, 59.9, 60.3) against 59.6 to 61.0 for four rings (six
  runs: 59.6, 60.0, 60.1, 60.7, 60.7, 61.0). **61.02 is the best
  two-port figure so far**, on the pinned setting.
- **Where the transmit interrupts go makes no difference to the
  rate** while nfsd keeps its CPUs: sharing two of nfsd's, sharing
  the receive CPU, or four of their own are within 0.2 GB/s in round
  1. A port's whole transmit side is 13 to 14% of four CPUs, about
  half a CPU. So the lopsidedness of the layout run earlier was not
  what held eight rings level.
- **Receive and transmit on one CPU drops the most**, 213 and 226
  frames a run with the ring CPUs 70% in softirq, and is still at
  59.3 to 59.6.
- **nfsd's CPUs do matter.** On 40 (12 to 51) it is 63 to 67% in the
  kernel and about 20% idle, and the four-ring rate is 59.1 to 59.6
  where it is 60.1 to 61.0 on 48: about half to one GB/s. On 32 (16
  to 47) it ran out in round 1, 89% in the kernel and 5% idle, at
  52.9 GB/s, with wakeups down to 5.8 a WRITE because a thread
  finds data already waiting. So 60 GB/s needs between 32 and 40 of
  these CPUs for nfsd as it is.
- **Sixteen transmit CPUs are 96 to 97% idle**, and the eight that
  the control leaves empty are 99 to 100% idle. With four rings there
  are CPUs to spare outside nfsd's set; with eight there are none
  unless nfsd gives them up.
- No drive stepped down in the twelve runs.

### The silent port again, in round 2 of "Tx on 8 of their own"

`r13b-8x64-tx8own-2` carried 44.70 GB/s, port a 13.3 and port b
31.4, and that is not a layout result. From second 30 to second 54
of the run port a's drives wrote 0 to 2 GB/s between them (one
second at 23), then 27 again; port b's rose from 26 to 37 while it
was out. It is what `stall-two-3` showed on 12 October, the same
port and the same 25 s:

- Not the drives: port a's four were 26% busy and none stepped down.
- Not loss at the server's card: no FIFO drops, 16 ring drops on
  port a, 57 client retransmits in the 10 s window.
- The harness's socket snapshots fell inside the silence. On the
  server, port a's sockets had empty receive queues (`r0`) and had
  last received 0.9 s before, with one or two packets counted as
  dropped by the socket (`d1`, `d2`). On the client, 15 of port a's
  16 connections had retransmission timers of 1.1 to 2.9 s (0.2 s is
  usual), congestion windows of 26 to 42 segments, 13 to 23 segments
  unacknowledged and 1.0 to 1.5 MB not yet sent.
- The worst write on port a waited 17.5 s (6.5 s on port b).

The stall watchers were not running. nfsd was on 32 CPUs in this
layout and had run out of CPU in round 1, which may be what provokes
it; the next event, below, was with nfsd on all 48 and says not.
Mike had set the silent port aside until it showed again. From the
next batch both watchers (`stall-watch-server.sh`,
`stall-watch-client.sh`) run beside every run.

The runs are `r13b-*` in `xeu-rig/results-twoport-464.tsv`
(`$L/rings13b-driver.sh`).

## More than one thread copying a connection's data (13 October, 12:22 to 13:07)

`svc_tcp_rx_copy_threads` (`THREADS=` in `twoport.sh`): at 0 a
thread copies a WRITE's data as it reads the socket; at 2 or more it
claims the data, which returns the window to the client at once,
gives the connection up and copies afterwards, so that another
thread reads the next WRITE meanwhile, and that many less one may be
copying a connection's data at a time. 0, 2 and 4 on the pinned
four rings of 128 and on eight of 64 with the transmit interrupts on
two CPUs shared with nfsd, by turns in two rounds, mark off, both
stall watchers beside every run. Each cell is round 1, round 2:

| rings a port | copy threads | GB/s | ring drops, both ports | client retransmits | region pages | nfsd (CPUs 10-53): sys, idle | threads woken a WRITE |
|---|---|---|---|---|---|---|---|
| 4 x 128 | 0 | 59.86, (52.83) | 5, 0 | 5, 5 | 1200, 1184 | 47 / 34 | 13.8 |
| 4 x 128 | 2 | 60.19, 60.31 | 2337, 3049 | 2489, 3276 | 1280, 1312 | 60, 60 / 23, 22 | 10.6, 11.1 |
| 4 x 128 | 4 | 60.22, 60.41 | 4361, 1880 | 4642, 1964 | 1296, 1280 | 62, 57 / 21, 25 | 10.6, 11.1 |
| 8 x 64 | 0 | 59.35, 59.24 | 277, 84 | 282, 96 | 1407, 1388 | 49, 48 / 32, 34 | 14.0, 13.5 |
| 8 x 64 | 2 | 56.54, 56.57 | 1629, 1811 | 1752, 1933 | 1491, 1490 | 62, 63 / 22, 21 | 9.9, 10.1 |
| 8 x 64 | 4 | 56.32, 56.51 | 1970, 1257 | 2075, 1387 | 1491, 1491 | 63, 59 / 21, 24 | 9.9, 10.2 |

The 52.83 is a run in which port a went silent (next section), and
its CPU figures are left out.

- **On four rings copy threads do not move the rate**: 60.2 to 60.4
  with 2 or 4 against 59.6 to 61.0 for 0 that day. They cost a third
  more of nfsd's CPU (60% in the kernel for 47%) and the rings drop
  two to four thousand frames in the 10 s window where they dropped
  a handful, with as many retransmits.
- **On eight rings they cost 3 GB/s**, 56.3 to 56.6 against 59.2 to
  59.4, and repeat to within 0.2. Rings of 64 have half the room for
  the closer-spaced sending that a window returned sooner brings.
- **4 is no different from 2** on either.
- Receive memory rises by about 100 pages (1.5 MiB) and stays inside
  the cache; no frame was dropped at the card's FIFO in any run.
- No drive stepped down in the twelve runs.

It is what one port showed on 11 October: the window is shut less,
the loss goes up and the rate stays where it was. With the copy
ceiling where it is, taking the copy off the socket's critical path
gives the copy nothing more to run on. `svc_tcp_rx_copy_threads`
stays 0. The runs are `ct*` in `xeu-rig/results-twoport-464.tsv`
(`$L/copythr-driver.sh`).

## The silent port, caught: the client paces its connections at megabits (13 October, 12:47)

`ct0-4x128-2`, the pinned setting with nothing changed and nfsd on
all 48 CPUs: 52.83 GB/s, port a 21.4 and port b 31.4. Port a's
drives wrote 29 to 30 GB/s between them for 47 s, 25 in second 48, 0
to 4 for three seconds, then 13 to 21 until the run ended at 60;
port b's rose from 31 to 35-38. Port a's drives were 62% busy and
none stepped down; the server dropped nothing (no FIFO or ring drops
and 5 retransmits in the window, which was before it). So the
squeezed nfsd of the earlier event is not what it needs.

The client's watcher dumped port a 3 s into the silence
(`$L/ct0-4x128-2.clwatch/dump-ens3np0-1`; its other five dumps and
all the server's are from the end of the run, when the load stops:
the watchers outlive a 45 s run). In that dump:

- **Round-trip estimates of 17 to 529 ms on eleven of port a's
  sixteen connections** (`rtt:` in `ss`), where the five healthy
  ones and all of port b's are at 0.05 to 0.13 ms and the least ever
  seen on the same connections is 0.011 to 0.012 ms.
- **TCP's pacing rate follows it down**: 4 to 142 Mbit/s on those
  eleven, 23 to 59 Gbit/s on the healthy ones. Nine of the port's
  `fq` qdiscs hold packets for throttled flows, the next due in
  anything from microseconds to 185 ms.
- **Retransmission timers of 0.4 to 3.8 s** (0.2 is the floor), with
  10 to 16 segments unacknowledged, congestion windows of 21 to 43
  segments and 1.2 to 1.8 MB waiting unsent. The windows are not
  what holds them.
- Over the 12 s after it, from the counters of that dump and the
  next: `TCPTimeouts` 93, `TCPSpuriousRTOs` 47, `TCPLossUndo` 47,
  `TCPSpuriousRtxHostQueues` 197 (a retransmit not made because the
  original was still in the client's own queues), and only 18
  segments retransmitted.
- Three of the stalled connections had last received anything from
  the server 1.826 to 1.827 s before, two more 1.0 s and one 2.9 s.
  In the earlier event (`r13b-8x64-tx8own-2`) the server's sockets
  for port a had all last received about 0.9 s before the snapshot.

So the length of the silence is made on the client: once a
connection's round-trip estimate is hundreds of milliseconds, `fq`
releases its packets at megabits, and the estimate stays high while
it does. `fq` on the client's ports is this rig's own two-port
setting (`QDISC=fq`, worth about 2 GB/s on 11 October; measured
again later this day it is worth nothing and was unpinned,
`SILENT-PORT.md`). Not shown:
what starts it. Something leaves port a's connections without an
ACK for about a second, several at the same instant, and whether
that second is the server's (its port, its ring CPUs, nfsd) or the
client's is not in this dump. The server's watcher needs three
seconds under 2 GB/s and the port was back above that sooner.

Three events are known: `stall-two-3` on 12 October and these two,
all on port a (`enp161s0f0`, the client's `ens3np0`), in 12 and 24
two-port runs on 13 October. To do: the server's watcher to dump on
a one-second gap, and `fq` against the client's default `fq_codel`
for whether a gap still grows into a silence.

## The receive path traced at 60 GB/s (13 October, 13:15 to 13:30)

Both layouts were traced with `PROFILE=1`: `prof-4x128-1` (51.69
GB/s, port a silent from second 56, next section), `prof-8x64-1`
(59.58), `prof-4x128-2` (59.89), `prof-8x64-2` (60.23). **The
account is `TRACING-NFSD-AND-XEU-MEMCPY-TIME.md`.** For this
document's question its answer is that the two layouts are the same
machine at work: every counter on nfsd's CPUs is within 3% between
them, eight rings spend a fifth more cycles on the rings' CPUs for
the same data, and the copy, which is the limit, is untouched by
either.

## The silent port, a fourth time, with the server's CPUs recorded as it began

`prof-4x128-1`: port a level at 30 to 31 GB/s for 55 s, then 0 to 3
from second 56 until the run ended. The cycles recording spanned
the start of it. Cycles in each eighth of the 8 s, on the CPUs of
port a's receive rings (0, 2, 4, 6), port b's (56, 58, 60, 62),
port a's transmit rings (1, 3, 5, 7) and nfsd's:

| eighth | a Rx | b Rx | a Tx | nfsd |
|---|---|---|---|---|
| 1 to 5 | 6.3 to 7.0e9 | 6.2 to 6.8e9 | 1.7 to 2.0e9 | 46 to 51e9 |
| 6 | 1.4e9 | 7.0e9 | 0.45e9 | 26e9 |
| 7 | 0.25e9 | 6.5e9 | 0.16e9 | 20e9 |
| 8 | 2.4e9 | 7.0e9 | 0.73e9 | 28e9 |

Port a's receive CPUs go idle; they are not stuck. In the last
eighth their profile is the usual one with more idle
(`default_idle_call` 16% for 3.5%), port a's transmit CPUs are 59%
idle, and nfsd's is the copy at 28% with more idle. Nothing on the
server spins, holds a lock or runs out of CPU as it starts: frames
stop arriving at port a. With the client's dump of the 12:47 event,
that puts the start between the client's sender and the card's
delivery of frames to port a's rings, and not in the server's
software. Four events now, all port a, three of them on 13 October
in 40 two-port runs since 11:40 and none in the 20 before.

## Layouts not run

- Four rings with the receive interrupts on all eight of a port's
  CPUs is not possible without RPS: a ring has one interrupt. RPS, or
  threaded NAPI, would be the way to spread one ring's receive work
  over two CPUs.
- Eight rings of 128 at the 384 KiB held buffer, inside the drives'
  fast region (47.0 GB/s at 34 MiB on 12 October, on slow drives).

## How to run one

    cd NFSD_TCP_WRITE_ZEROCOPY/xeu-rig
    L=/var/tmp/zc-run-logs
    TSV=$PWD/results-twoport-464.tsv TAG=<tag> LINGER=0 LOWAT=0 \
        RCVBUF=393216 WARM=15 RUNTIME=45 \
        RINGS=8 ENTRIES=64 TXIRQ_A=8 TXIRQ_B=54 \
        ./twoport.sh $L/xeu-16k-464-<tag> ab

`geometry.txt` of a run has where every interrupt was put,
`window.txt` each ring's share, `srv-mpstat.txt` every CPU, and
`drives.txt` what each drive wrote: read its last line (`no drive
stepped down`) before the figure. `TXN=4` with `TXIRQ_A=8
TXIRQ_B=52` spreads a port's transmit interrupts over four CPUs, and
`NFSD_CPUS=12-51` keeps nfsd off them for the run. `PROFILE=1` adds a 10 s `perf
record` and `perf stat` mid-run. `$L/rings13-driver.sh` ran the
table above.
