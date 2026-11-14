# Round 103: why two ports read only 54 GB/s

14 November, tardis2, build 531, the DRC commits' `nfsd.ko`, xeu
Release 15 (`C3071685F0211EF8EBA2A7F`).  Mike: "look into why
dual-port reads stop at 54 GB/s", then "run the xps_rxqs A/B on
dual-port reads".

## What round 102 left

Two ports on all eight drives read 54.3 and 54.6 GB/s (`t2-r633`,
`r637`), barely more than four drives' 52.4 (`r625`, `r629`, the
drives' limit there), with every drive 69 to 83% busy and each port
carrying 22 to 32 GB/s against the 43 one port reads alone.  From the
runs' own records:

- **One transmit ring's CPU on each port is nearly saturated**: CPU 61
  or 59 (port b) at 94%, CPU 7 or 3 (port a) at 74 to 78%, every other
  ring CPU under 45% (`srv-mpstat.txt`).  In the 4-ring layout Tx-i is
  on CPU 2i+1 of the port's eight.  One port alone (`r632`) has none:
  its busiest are the receive rings' CPUs at 59 to 75%.
- **The server's connections see 1 to 3 ms round trips, retransmit up
  to 22 MB each (many spurious: `dsack_dups`), and are limited by
  their send buffer 9 to 39% of the time**, against 0.25 to 1.0 ms,
  under 1 MB and 1 to 4% on one port (`ss-server.txt`).
- **The client lost nothing**: no out-of-buffer or discard counts on
  either mlx5 port (`cli-ethtool-*`; port a's corrected lane errors
  are the same in write runs), and its CPUs are 47% busy, none above
  60%.

## The suspect: transmit queue steering

XPS gives each transmit queue one nfsd CPU (port a's tx-i CPU 8+i,
port b's 32+i), so a connection whose nfsd thread runs on any of the
other 44 CPUs takes a queue by its hash, and holds it while it has
data in flight (`settled.sh`, "TRANSMIT STEERING").  Twelve
connections a port hashed onto four queues need not split 3, 3, 3, 3;
on writes the replies are small and it does not show, on reads the
transmit side carries the data.

The fix proposed is XPS by receive queue: tx-i steered by rx-i, so a
connection sends by the queue paired with the ring the rig placed it
on, three a ring.  **xeu does not record the receive queue on its
skbs** (no `skb_record_rx_queue()` in either receive path), so a
socket's `sk_rx_queue_mapping` is never set and `xps_rxqs` has nothing
to steer by.  kernel-rpmtree `b4f1a54` adds it (kmod-xsight 2.0.1-17,
Patch15; built for build 531 into
`$L/kmod-xsight-16k-build-rxq/`, xeu srcversion
`4FE2E7FF217385CDB5D4BB2`).  It is not loaded: loading it is a reload
of xeu, Mike's to allow.

## The runs

[run638.sh](run638.sh): two-port reads on all eight drives as round
102's, plus each port's transmit bytes by ring (`ethtool -S` after the
run) and, from a 3 s trace of `xeu_start_xmit` in the window, which
queue each connection sends by.  `twoport.sh` gains `XPS=rxqs`.

1. `t2-r638`, the settled map, on xeu Release 15: is the split
   uneven, and is the hot CPU the busiest queue's?
2. After the reload into Release 17 (if Mike allows it): the settled
   map against `XPS=rxqs`, interleaved, two of each, then a two-port
   write with `XPS=rxqs` to see that writes do not suffer.

## `t2-r638`: the split is uneven, but not by connection

[run638.out](run638.out): 55.85 GB/s (a 25.58, b 30.28), ring drops 0,
no drive stepped down.  The `ethtool -S` transmit counters read 0: the
end of `twoport.sh` reopens the ports, which zeroes them, so they are
read from the trace instead.  Each port's `xeu_start_xmit` calls in 3 s
by queue ([txq-sk-r638.txt](txq-sk-r638.txt)).  **Taken 24 s before the
load (round 104's correction, below at "The A/B"): these are the sends
of twoport.sh's placement write job, not of the reads, so the table
and the two points after it describe that job only.**

| | q0 | q1 | q2 | q3 |
|---|---|---|---|---|
| a | 31.2% | 25.7% | 25.6% | 17.6% |
| b | 17.2% | 39.8% | 33.4% | 9.7% |

- **Uneven, up to four to one on b.**
- **But every connection sent by all four queues**, not one each: a
  socket picks a queue again whenever it has nothing in flight, and
  which it gets follows the nfsd CPU sending (the four in the XPS map
  get theirs, the other 44 the hash), so the hash-a-connection account
  above was wrong in its mechanism.  `XPS=rxqs` would still hold each
  connection to its ring's queue.
- The hot CPU moved: CPU 5 (a's Tx-2) at 87%, and none on b.

## The reload into kmod-xsight 2.0.1-17 (Mike, 14 November)

Mike: "Reload now".  Release 15's three modules are kept in
`$L/xeu-rxq-reload-1114/rel15/` (no Release 15 RPM is on the host).
`rpm -Uvh` of Release 17 at 17:28; it also erased build 522's
`kmod-16k-xsight` (the same package name), which matters only if 522
is booted again.  No NFS connection, `rmmod xeu; modprobe xeu` at
17:28:59: srcversion `4FE2E7FF217385CDB5D4BB2`, both ports up;
`port-setup.sh` on both (the region mapped, 4 pools a port), then
`settled.sh apply`: the settled setting; both ports 5 of 5, no new
kernel warning.  Files in `$L/xeu-rxq-reload-1114/`.

## The A/B

`RUNS="639:read:cpus 640:read:rxqs 641:read:cpus 642:read:rxqs
643:write:rxqs 644:write:cpus" ./run638.sh`, on Release 17 throughout.

## Result: `xps_rxqs` reads 60.5 GB/s, 11% more, and writes the same

[run639.out](run639.out), all on xeu Release 17.  No drive stepped
down, no new kernel warning; the host at the settled setting after.

| run | | map | GB/s | a / b | p99 ms | ring drops | busiest ring CPUs |
|---|---|---|---|---|---|---|---|
| `t2-r639` | read | cpus | 54.42 | 25.2 / 29.2 | 150 to 165 | 76 | Tx 5 at 93%, Tx 63 at 83% |
| `t2-r640` | read | **rxqs** | **60.46** | 30.2 / 30.3 | 80 to 81 | 295 | every Rx CPU 82 to 93% |
| `t2-r641` | read | cpus | 54.09 | 27.5 / 26.6 | 133 to 139 | 33 | Tx 1 at 93%, Tx 57 at 92% |
| `t2-r642` | read | **rxqs** | **60.56** | 30.3 / 30.3 | 79 | 331 | every Rx CPU 81 to 93% |
| `t2-r643` | write | **rxqs** | 78.06 | 39.1 / 39.0 | 81 | 0 | Rx 0 at 50% |
| `t2-r644` | write | cpus | 78.00 | 39.0 / 39.0 | 80 to 81 | 0 | Rx 0 at 53% |

**Correction (14 November, round 104): the traces below were not taken
in the run.** `run638.sh` started them 75 s after twoport.sh, which
then had not yet started the load: each trace ended 6 to 27 s before
the first second of the load (`wire-sec.tsv`), in twoport.sh's flow
placement, a short write job on the same mounts, or just after it.
So these shares are of the sends of that write job and the idle that
followed, not of the reads, and they say nothing about how the
reads' sends spread over the queues. The rates, latencies, drops and
CPU figures above and below come from twoport.sh's own window and
stand.

Each port's sends by transmit queue (`xeu_start_xmit` calls in 3 s
before the load, `txq-sk-r6*.txt`; see the correction above):

| run | map | a: q0 q1 q2 q3 | b: q0 q1 q2 q3 |
|---|---|---|---|
| `t2-r639` | cpus | (every connection on all four) | |
| `t2-r640` | rxqs | 27 27 25 20% | 25 25 25 25% |
| `t2-r642` | rxqs | 28 25 26 21% | 29 21 28 21% |
| `t2-r644` | cpus (write) | 10 10 40 40% | 18 26 32 24% |

- **The read ceiling rises from 54.1 to 54.4 GB/s to 60.5 to 60.6**
  with the two ports even (30.2 to 30.3 each) and the p99 halved.
  How the reads' sends spread over the queues was not measured: the
  queue traces were taken before the load (the correction above).
- **Reads are now at the receive rings' CPUs**: all eight of them 81
  to 93% busy (CPU 0, port a's Rx-0, 93% in both runs), the transmit
  rings' CPUs 11 to 22%, against 27 to 55% and one Tx CPU at 83 to 93%
  on the CPU map.  The drives are 85 to 87% busy, 7.6 GB/s each.  Why
  the receive CPUs carry this much more on a read (the client's ACKs
  and READ calls, and with them the sends TCP makes from its ACK
  processing) was not looked into.
- **Ring drops rise**, 295 and 331 against 33 and 76, on CPUs that
  are now nearly full.
- **Writes are unchanged**: 78.06 against 78.00 GB/s, nfsd and the
  rings as busy (22% and 18 to 19%), loans admitted with none denied.

`settled.sh` held the CPU map until Mike's word.

## Settled: xps_rxqs on tardis2 (Mike, 14 November)

`xeu-rig/settled.sh` now holds `xps_rxqs` on tardis2: `apply` writes
tx-i's `xps_rxqs` as `1 << i` and empties its `xps_cpus`, and the
audit holds both (`XPS CPU none`, `XPS by receive queue i`). The map
needs an xeu that records each frame's receive queue, so on tardis2
the audit also holds the loaded xeu to kmod-xsight release 17 or
later (`xeu records the receive queue`): it checks that the loaded
module is the installed file, by srcversion, and reads that file's
package release. Other hosts keep the recorded CPU map (tardis1 runs
Release 13, which does not record the queue). Applied at rest on
tardis2 the same evening; the audit ends in the settled setting.
`twoport.sh`'s `XPS=rxqs` is now the setting itself on tardis2, and
running it there only skips the interrupt and XPS checks.
