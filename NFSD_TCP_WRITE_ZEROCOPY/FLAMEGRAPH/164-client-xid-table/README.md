# Round 164: the client's requests awaiting a reply, by XID

Mike, 20 November: "yes, write it with the rb-tree fallback".  Round
161's client profile put the transport's receive rb-tree at about 0.6
us of the client's CPU a READ: the insert's walk in
`xprt_request_enqueue_receive()` (most of its 0.37 us of its own),
`rb_insert_color` 0.07, `xprt_lookup_rqst` 0.14, `rb_erase` 0.06; with
some 128 requests outstanding a transport, each walk reads 7 to 8
`rpc_rqst`s, most last written on another CPU.

`0e9aa447e911e`, "SUNRPC: Find a request awaiting its reply by its
XID's low bits": a transport hands out XIDs in sequence, so each
request goes in a 256-slot table by its XID's low bits (one store to
queue, one load and compare to find, one store to remove); a request
whose slot is taken goes in the rb-tree as before, and a lookup the slot
does not answer walks the tree.  2 KiB more a transport.  W=1 clean on
tardis2 (aarch64) and built on the client from round 163's tree,
`/root/zc-ckern/XT` (`cbuild136.sh`, 17 modules, no warnings but
compiler notes; sunrpc `F55891CC4033B4C039DED19`).

`ab-xid.sh`: B (the client's `updates/`, round 163's stack, sunrpc
`72B6522F5FD84CCD583F45D`) against X, B X B X B X (t2-r1245 to
r1250), then X's WRITE pass (r1251) and READ pass (r1252), the client
back on B.

## The A/B (20 November, 01:11 to 01:33)

| run | arm | IOPS | a / b M | p99 ms a / b | server us a READ | client us a READ | client busy (busiest) |
|---|---|---|---|---|---|---|---|
| `t2-r1245` | B | 3.49 M | 1.75 / 1.74 | 3.26 / 3.16 | 14.67 | 11.42 | 83% (100%) |
| `t2-r1246` | X | 3.51 M | 1.76 / 1.75 | 3.19 / 3.29 | 14.77 | 11.08 | 81% (93%) |
| `t2-r1247` | B | 3.58 M | 1.79 / 1.80 | 2.97 / 3.00 | 14.66 | 11.26 | 84% (92%) |
| `t2-r1248` | X | 3.42 M | 1.67 / 1.75 | 4.49 / 4.11 | 15.16 | 11.09 | 79% (92%) |
| `t2-r1249` | B | 3.56 M | 1.81 / 1.75 | 3.00 / 3.26 | 14.74 | 11.33 | 84% (99%) |
| `t2-r1250` | X | 3.53 M | 1.77 / 1.76 | 3.00 / 3.00 | 14.87 | 10.88 | 80% (96%) |
| mean | B | 3.54 M | | | 14.69 | 11.34 | |
| mean | X | 3.49 M | | | 14.93 | 11.02 | |

- **The client's CPU a READ is 0.32 us lower on X (2.8%)**, every X run
  below every B run (10.88 to 11.09 against 11.26 to 11.42): about half
  of what round 161 put on the tree, the rest being the request's own
  line, which any lookup reads.
- **IOPS do not follow:** the server is the limit (its nfsd CPUs at
  100%), and X's t2-r1248 is one of the server's slow runs (15.16 us a
  READ, port a 1.67 M), as t2-r1234 and t2-r1241 were.  Its p99 of 4.5
  ms did not come back in t2-r1250 (3.00 on both ports).
- **Kept (Mike, 20 November: "keep this change")**, `0e9aa447e911e`;
  its WRITE and READ passes follow.
