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

## The runs (01:15 to 01:42): the change did not run for these READs

| Run | Arm | 4 KiB IOPS | Port a / b (M) | Drives a / b, each (GB/s) | p99 a / b (ms) | Client us a READ |
|---|---|---|---|---|---|---|
| t2-r1138 | B | 3.54 M | 1.78 / 1.76 | 1.7-1.9 / 1.7-1.9 | 3.39 / 3.39 | 11.80 |
| t2-r1139 | P | 3.33 M | 1.63 / 1.70 | 1.2-2.0 / 1.1-2.1 | 6.85 / 7.31 | 11.82 |
| t2-r1140 | B | 3.19 M | 1.88 / 1.31 | 1.9-2.1 / 1.1-1.7 | 3.52 / 8.45 | 11.74 |
| t2-r1141 | P | 3.31 M | 1.53 / 1.78 | 1.4-2.0 / 1.7-2.0 | 6.39 / 5.47 | 11.89 |
| t2-r1142 | P, traced | 3.42 M | 1.83 / 1.60 | | 3.29 / 6.78 | |

- t2-r1142's trace: **169 us from the reply's match to the
  completion, 99.9% on the same CPU**, against 183 us on B (t2-r1137);
  the other steps as before.
- **Why: the change never ran for these READs.**  The IOPS run's fio
  is on io_uring, which submits each READ `NOWAIT`, and NFS marks an
  O_DIRECT pgio `RPC_TASK_INLINE_OK` only without `NFS_IOHDR_NOWAIT`
  (`fs/nfs/pagelist.c`), since the flag also lets the first leg run in
  the submitter, which a NOWAIT submitter must not.  The change moves
  `INLINE_OK` tasks only, so every 4 KiB READ took rpciod as before.
  The rows above compare two stacks that did the same for them: what
  differs is the drives (100% busy, uneven, about 8 GB written to each
  in each READ run) and run-to-run spread.
- On P: the WRITE pass t2-r1143 **77.86 GB/s PASS** (libaio, whose
  WRITEs are `INLINE_OK`, so it took the new path; p99 82.31 / 82.31
  ms, as B's 82.31 / 81.26 at t2-r1122), the READ pass t2-r1144
  **83.55 GB/s PASS** (io_uring, the old path), no new kernel warning.
  The client back on its `updates/` stack and the settled setting after.

To test the idea the completion needs its own mark: the completion side
does not care whether the submitter may block, so NFS could mark every
O_DIRECT pgio task (NOWAIT or not) with a flag only the receive side
reads, and `INLINE_OK` keep its meaning for the first leg.
