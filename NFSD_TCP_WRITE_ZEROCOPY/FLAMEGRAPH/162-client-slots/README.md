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
