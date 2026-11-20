# Round 162: the client's preallocated RPC slots, then the idle timer

Mike, 19 November: "yes, run the slot-table A/B, then do 2".  Round
161's client profile: `xprt_alloc_slot` takes the transport's
`reserve_lock` at 0.22 us a READ (barely contended: the line moves
between fio's CPU and the completing kworker's), and with the client's
`sunrpc.tcp_slot_table_entries` at 2, the default, and some 128 READs
outstanding a transport, nearly every slot is kzalloc'd and kfree'd
(0.07 us).  `ab-slots.sh`: B (2) against S (128), B S B S, t2-r1233 to
r1236, one nfs4k.sh run each at iodepth 96 under settled-iops.sh, the
client back at 2 after each.  A client setting for the IOPS runs only,
so the passes are not run for it.

Then 2: `xprt_request_enqueue_receive()` calls `timer_delete_sync()` on
the transport's idle timer for every request, which takes the timer
base's lock whether the timer is armed or not (0.07 us a READ, and the
top `_irqsave` caller in round 161).

## The slot-table A/B (20 November, 00:15 to 00:30)

| run | arm | slots | IOPS | a / b M | p99 ms a / b | server us a READ | client us a READ | client busiest |
|---|---|---|---|---|---|---|---|---|
| `t2-r1233` | B | 2 | 3.49 M | 1.80 / 1.69 | 3.10 / 3.36 | 14.85 | 11.42 | 100% |
| `t2-r1234` | S | 128 | 3.36 M | 1.66 / 1.70 | 3.36 / 3.36 | 15.43 | 11.43 | 93% |
| `t2-r1235` | B | 2 | 3.57 M | 1.77 / 1.79 | 3.06 / 2.90 | 14.70 | 11.29 | 93% |
| `t2-r1236` | S | 128 | 3.52 M | 1.75 / 1.77 | 3.26 / 3.10 | 14.73 | 11.45 | 93% |
| mean | B | | 3.53 M | | | 14.78 | 11.36 | |
| mean | S | | 3.44 M | | | 15.08 | 11.44 | |

**Preallocated slots gain nothing:** the client's CPU a READ is 0.08 us
higher on S, not the 0.07 lower its kmalloc and kfree pointed to, and
both of S's runs are below both of B's.  A preallocated slot is
`memset` whole by `xprt_free_slot()` under `reserve_lock` before it goes
back on the free list, which costs about what kfree and kzalloc did,
and the lock's line moves the same either way.  Not kept: the client
stays at the default 2 (it is back at 2 after the A/B).
