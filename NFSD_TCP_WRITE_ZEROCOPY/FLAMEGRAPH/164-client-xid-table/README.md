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
