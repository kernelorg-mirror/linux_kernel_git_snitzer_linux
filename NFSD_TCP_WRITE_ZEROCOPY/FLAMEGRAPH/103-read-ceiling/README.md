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
of the window, by queue ([txq-sk-r638.txt](txq-sk-r638.txt)):

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
