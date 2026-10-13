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
  linger, `svc_tcp_rx_keep=1`, `svc_tcp_rx_lowat=0`, `fq` on the
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
  side plus nfsd. Whether that costs the eight-ring runs anything
  has not been tested.
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

## Layouts not yet run

- Eight rings with the transmit interrupts spread over four or eight
  CPUs taken out of nfsd's set (nfsd on 12 to 51, say), so that no
  transmit CPU is shared with nfsd.
- Eight rings with each ring's receive and transmit on one CPU,
  inside the drives' fast region.
- Four rings with the receive interrupts on all eight of a port's
  CPUs is not possible without RPS: a ring has one interrupt. RPS, or
  threaded NAPI, would be the way to spread one ring's receive work
  over two CPUs.

## How to run one

    cd NFSD_TCP_WRITE_ZEROCOPY/xeu-rig
    L=/var/tmp/zc-run-logs
    TSV=$PWD/results-twoport-464.tsv TAG=<tag> QDISC=fq LINGER=0 LOWAT=0 \
        RCVBUF=393216 WARM=15 RUNTIME=45 \
        RINGS=8 ENTRIES=64 TXIRQ_A=8 TXIRQ_B=54 \
        ./twoport.sh $L/xeu-16k-464-<tag> ab

`geometry.txt` of a run has where every interrupt was put,
`window.txt` each ring's share, `srv-mpstat.txt` every CPU, and
`drives.txt` what each drive wrote: read its last line (`no drive
stepped down`) before the figure. `PROFILE=1` adds a 10 s `perf
record` and `perf stat` mid-run. `$L/rings13-driver.sh` ran the
table above.
