# The silent port

Started 2026-10-13 on tardis1 (build 464, 16 KiB pages; the client is
plsm121c-06 on 7.3.0-0.rc7.2.hs.469). In about one two-port run in
ten, one of the two ports stops carrying data for about 25 s while
the other carries on, and the run's total falls by 7 to 9 GB/s. Mike
set it aside on 13 October until it showed again; it showed three
more times that afternoon and is the largest thing between a
two-port run and its 60 GB/s. This is what is known, in the order it
was found.

## The events

| run | when | port | from | what was on record |
|---|---|---|---|---|
| `stall-two-3` | 12 October, evening | a | 25 s | per-second rates; client's `TCPTimeouts` rising, nothing dropped at the server |
| `r13b-8x64-tx8own-2` | 13 October, 12:18 | a | second 30 of the load | the harness's socket snapshots, inside it |
| `ct0-4x128-2` | 12:47 | a | second 48 | the client watcher's dump, 3 s in |
| `prof-4x128-1` | 13:17 | a | second 56 | the server's cycles being recorded as it began |
| `silent-1` | 15:47 | **b** | second 35 | both ends' counters five times a second, before and after |

All are two ports at the two-port setting of the time, with `fq` on
the client's ports (unpinned since; see below). 13 October: five events in about 50 two-port runs since
11:40 and none in the 20 before. In each the other port takes up
load (35 to 38 GB/s) while the silent one is out.

## What it is not

- **The drives.** The silent port's drives are 26 to 62% busy or
  idle and none stepped down.
- **Loss at the server's card.** No FIFO drops, a handful of ring
  drops, nothing in the window.
- **The server's software.** `prof-4x128-1` had every CPU sampled as
  it began: the silent port's receive CPUs go idle (cycles fall to a
  twenty-fifth), its transmit CPUs go idle, nfsd's are the copy with
  more idle. Nothing spins, holds a lock or runs out of CPU.
- **nfsd short of CPU.** The first event of 13 October was with nfsd
  squeezed onto 32 CPUs; the next three were with all 48.
- **Port a's link.** Four events on port a suggested it, and the two
  ports are not the same kind of link (below), but the fifth was on
  port b, which is clean.
- **ARP, link flaps, pause frames, a stopped transmit queue.** None
  moved across `silent-1`'s start.

## The two ports are not the same kind of link

Found while looking for why it was always port a. It did not turn
out to be the cause, and is worth knowing anyway:

| | port a: `enp161s0f0`, client `ens3np0` | port b: `enp164s0f0`, client `ens6np0` |
|---|---|---|
| the client's module | optical, Proficium MMA4Z00-NS400-P, multimode | copper, NVIDIA MCP7Y00-N002, 2 m |
| bits corrected by FEC at the client's receiver since its boot (29.5 h) | 164 billion, 141 billion of them on lane 0 | 10,474 |
| blocks FEC could not correct | 0 | 0 |
| frames the client dropped as matching no receive rule | 580 thousand | 74 thousand |
| frames the server dropped as not addressed to it | 22,964 | 10,686 |
| other hosts on the subnet | 192.168.1.105, .172, .195 | 192.168.0.105 |

Port a's client receiver corrects about 1.5 million bits a second,
loaded or idle, a bit error rate near 1e-5 on its worst lane. FEC
corrects all of it so far. `xeu` exposes no such counter, so the
server's receivers are not known. Port a alone drops 33 to 1,087
frames at the card's FIFO in a one-port run and port b none, which
may or may not be related.

**Until the evening of 13 October the harness and the old watchers
reached the client by its port a address** (192.168.1.106), so their
ssh traffic rode the port under test, in every run of every event
above. The client has a management address, 10.20.12.106 on its
`ens8f0` (the server's is 10.20.12.211 on `xemi0`). `stall-sample.py`
was run over that from the start, and since then so is every script
in `xeu-rig/` (`CLIMGMT` in `common.sh`). There is no evidence that
the harness's traffic had any part in a silence.

## What the client's side looks like once it has begun (`ct0-4x128-2`)

The client watcher's dump 3 s in, for port a's sixteen connections:

- Round-trip estimates of 17 to 529 ms on eleven of them (0.05 to
  0.13 ms on the healthy ones and on port b; the least ever seen on
  the same connections is 0.012 ms).
- TCP's pacing rate follows: 4 to 142 Mbit/s on those eleven, 23 to
  59 Gbit/s on the rest. `fq` holds their packets as throttled
  flows, the next due up to 185 ms away.
- Retransmission timers of 0.4 to 3.8 s. Congestion windows normal
  (21 to 43 segments), 10 to 16 segments unacknowledged, 1.2 to 1.8
  MB unsent.
- In the next 12 s: `TCPTimeouts` 93, `TCPSpuriousRTOs` 47,
  `TCPSpuriousRtxHostQueues` 197, and 18 segments retransmitted.

So the 25 s is made on the client: with a round-trip estimate of
hundreds of milliseconds `fq` lets a connection's packets out at
megabits, and the estimate stays high while it does.

## The start, both ends, five times a second (`silent-1`, port b)

`xeu-rig/stall-sample.py` on the server and on the client, read
with `stall-read.py`. Thousands of frames a second:

| from the start | port b: client sent | server took | server sent | client took | port a: client sent |
|---|---|---|---|---|---|
| -0.4 s | 3,789 | 3,855 | 363 | 360 | 3,808 |
| -0.2 s | 2,302 | 1,162 | 118 | 222 | 4,178 |
| 0 to +1.2 s | 0 | 0 | 0 | 0 | 4,474 to 5,010 |
| +1.4 to +5 s | 0 to 346, in bursts | the same | 0 to 40 | the same | 4,632 to 5,005 |

- **Nothing is lost on the wire in either direction**: what one end
  sent the other took, sample for sample.
- **Both directions stop within a fifth of a second**, all sixteen
  connections at once, and stay stopped for 1.4 s.
- **The server's replies reached the client and the client's
  acknowledgements did not come back.** From -0.4 to +1.0 s the
  server counted `RetransSegs` 27, `TCPTimeouts` 18,
  `TCPLossProbes` 9; the client counted `TCPDSACKOldSent` 27, so
  each of the 27 arrived as a duplicate of something it already had.
- **The client's own packets were still in the client.** In the same
  1.4 s: `TCPTimeouts` 22 and `TCPSpuriousRtxHostQueues` 54 on the
  client.
- Nothing else moved on either end: no FIFO, ring or socket drops,
  no discards, pause frames or stopped queues, no ARP failure, the
  neighbours REACHABLE throughout, the client's FEC rate unchanged.

So for 1.4 s the client's transmit side for one port lets nothing
out, acknowledgements included, and then `fq` keeps it at a trickle.
At five samples a second it cannot be said whether the client
stopped sending first or stopped hearing acknowledgements first, so
what starts it is not shown.

## The client's queueing compared, and `fq` unpinned (13 October, 15:55 to 17:10)

`$L/qdisc-driver.sh`: the client's queueing by turns, `fq` (pacing),
`fq nopacing` (`QDISC=fqnp`, new in `twoport.sh`) and the client's
default `fq_codel`, the samplers on every run. Stopped after 19 of
24 runs, on Mike's word:

| client's queueing | GB/s, two ports | middle | a port silent |
|---|---|---|---|
| `fq` | 60.74, 60.68, 60.72, 59.67, 59.94, 59.38, 60.47 | 60.47 | none |
| `fq nopacing` | 60.29, 60.29, 59.90, 60.10, 59.98, 60.28 | 60.19 | none |
| `fq_codel`, the default | 60.16, 60.20, 59.96, 60.17, 60.41, 60.34 | 60.19 | none |

- **`fq` is worth nothing that can be measured**: the means are
  60.23, 60.14 and 60.21. It was made part of the two-port setting
  on 11 October for 2 to 4 GB/s, measured with the drives flushing
  on every COMMIT and runs of twice the drives' fast region.
- **No port went silent in the 19 runs,** nor in the one before
  them, so the comparison says nothing about whether a silence
  happens without pacing. At one run in ten, 20 clean runs come up
  about one time in eight.

**`fq` is unpinned (Mike, 13 October): the two-port setting leaves
the client on its default `fq_codel`, and nothing on the client is
changed. The silent port is set aside again: chase it if it shows
again.** All five events were with `fq`. What is known of it says
pacing is what holds a port at a trickle once it has begun, so
without `fq` an event should be shorter if it happens at all; that
is expected and not shown.

## If it shows again

1. `stall-sample.py` on both ends for the run (below), or
   `$L/silent-driver.sh`, which keeps the samples of a run with a
   silence. Note which queueing the client had.
2. `stall-read.py` gives the start in fifths of a second. What is
   not known is which stops first, the client's sending or the
   acknowledgements reaching it; a finer clock on the port's
   connections (a capture of headers on one connection, or
   `tcp:tcp_probe` on the client) would say.
3. The harness's own ssh is off the ports under test now (the
   client's management address, 10.20.12.106); if `CLIMGMT` has
   been pointed back at a port, note it.

## To reproduce

    # both ends, over the management network, for the length of a run
    xeu-rig/stall-sample.py $L/<tag>.srv.txt 170 enp161s0f0 enp164s0f0 &
    ssh root@10.20.12.106 /root/stall-sample.py /tmp/stall-cl.txt 170 ens3np0 ens6np0 &
    # ... the run ...
    xeu-rig/stall-read.py $L/<tag>.srv.txt $L/<tag>.cl.txt 2 5

`$L/silent-driver.sh` does that around pinned two-port runs and
keeps the samples of any run in which a port's drives wrote under a
fifth of the other port's for 2 s.
