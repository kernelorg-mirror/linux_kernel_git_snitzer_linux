# Round 140: a reply completed in the receive worker that matched it

Mike, 19 November: "yes, complete the READ in the receive worker", from
round 139's trace: a READ waited 183 us between its reply being matched
in the transport's receive worker and its completion, on the same CPU,
in that CPU's rpciod queue behind the receive work: 14% of its 1.35 ms.

`83b22f44df786` "SUNRPC: Complete a reply in the receive worker that
matched it" (W=1 clean on the client): the reply's wakeup puts the task
on the transport's `recv_done` list instead of queueing it on rpciod,
and `xs_stream_data_receive()` runs the list after each record, with
`recv_mutex` let go (a task's next step may close the socket, which
takes `recv_mutex`).  Only async tasks marked `RPC_TASK_INLINE_OK` (NFS
pgio and commit, audited to run outside rpciod by `50eee8803afc5`), not
a swap task and not on TLS; every other task is queued as before.

`ab-complete.sh`: B (the client's `updates/` stack) against P, 4 KiB
random READs at iodepth 96 B P B P (t2-r1138 to r1141), round 139's
handoff trace on P (r1142), then the WRITE and READ passes on P (r1143,
r1144).
