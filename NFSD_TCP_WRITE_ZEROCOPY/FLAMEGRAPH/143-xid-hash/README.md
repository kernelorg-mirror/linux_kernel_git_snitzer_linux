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
