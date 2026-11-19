# Round 143: the requests awaiting a reply, hashed by xid

Mike, 19 November: "yes, write the xid table and run the IOPS A/B",
from round 142's profile: the receive queue's rb-tree took 3.5% of the
client's cycles, 0.42 us a READ, each insert walking about eight
`rpc_rqst` nodes last written by other CPUs.

`58b51d007a76e` "SUNRPC: Find a request awaiting its reply by an xid
hash" (W=1 clean on the client): 1024 `hlist` buckets a transport
(8 KiB, `kvzalloc`ed in `xprt_alloc()`), indexed by `hash_32()` of the
xid; `rq_recv` an `hlist_node` in the union it shared with `rq_list`;
`recv_count` beside the table for the autodisconnect timer's emptiness
checks.  It runs for every request, so no gate to check.

`ab-xid.sh`: B (the client's `updates/` stack) against P, 4 KiB random
READs at iodepth 96, B P B P B P (t2-r1155 to r1160), then the WRITE
and READ passes on P (r1161, r1162).

## The runs (07:09 to 07:38): no gain

| Run | Arm | 4 KiB IOPS | Port a / b (M) | Drives a / b, each (GB/s) | p99 a / b (ms) | Client us a READ |
|---|---|---|---|---|---|---|
| t2-r1155 | B | 3.56 M | 1.78 / 1.79 | 1.7-1.9 / 1.8-1.9 | 3.33 / 3.26 | 11.73 |
| t2-r1156 | P | 3.49 M | 1.74 / 1.75 | 1.6-1.9 / 1.8 | 3.39 / 3.62 | 11.69 |
| t2-r1157 | B | 3.46 M | 1.77 / 1.69 | 1.8-1.9 / 1.7-1.8 | 3.33 / 3.39 | 11.79 |
| t2-r1158 | P | 3.36 M | 1.56 / 1.80 | 1.4-1.9 / 1.5-2.0 | 8.85 / 3.98 | 11.71 |
| t2-r1159 | B | 3.55 M | 1.81 / 1.73 | 1.7-2.1 / 1.7-1.9 | 3.29 / 3.49 | 11.76 |
| t2-r1160 | P | 3.38 M | 1.63 / 1.75 | 1.5-1.9 / 1.6-1.9 | 5.60 / 3.78 | 11.79 |
| mean | B | 3.52 M | | | | 11.76 |
| mean | P | 3.41 M | | | | 11.73 |

- **The client's CPU a READ is the same** (11.73 against 11.76 us),
  not the 0.42 us less the profile's share of the tree promised.
- P's lower IOPS is its two runs with port a's drives uneven (1.4 and
  1.5 GB/s at the slowest), whose tails are that port's; the CPU a READ
  of those runs is B's.
- Why, by inference and not measured: the tree's samples at the loads of
  a node's child and its `rq_xid` were mostly the miss on the
  `rpc_rqst`'s line, last written by another CPU.  The hash still reads
  the request it finds to compare its xid, so the miss moves and stays,
  as round 138 found for `rpc_count_iostats_metrics`.  A profile's
  share at a load that misses is the miss's, wherever the load is.
- Passes on P: WRITE t2-r1161 **77.92 GB/s PASS**, READ t2-r1162
  **81.88 GB/s PASS**, no new kernel warning.  The client back on its
  `updates/` stack and the settled setting after.

## Dropped

Mike, 19 November: "revert, seems like dead end".  `58b51d007a76e` is
dropped from the history as rounds 140 and 141's were, not reverted:
the round's two docs commits replayed on `6f488c495542f`, the code
round 139's again, the tip before kept as
`kernel-7.3/canary.pre-drop-xid-hash`, the patch
`patches/dropped/0027`.

The lesson, the third time on this path (rounds 138, 141, 143): **a
function's share of a profile on the client is not what removing it
saves when its samples sit on loads of an `rpc_rqst`**, a line last
written by another CPU.  Moving the load (iostats per CPU, a hash in
place of a tree) moves the miss and saves nothing.  The cost to go
after is the number of cross-CPU touches of a request, which a
profile by function does not show; a profile of the misses by cache
line (`perf c2c`) does.
