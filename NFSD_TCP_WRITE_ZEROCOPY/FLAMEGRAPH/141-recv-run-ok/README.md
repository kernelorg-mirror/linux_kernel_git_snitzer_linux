# Round 141: round 140 with its own mark

Mike, 19 November: "yes, add the flag and rerun the IOPS A/B".  Round
140's change (`83b22f44df786`) never ran for the IOPS run's READs: it
was gated on `RPC_TASK_INLINE_OK`, which NFS leaves off for a NOWAIT
O_DIRECT pgio, and io_uring submits every READ NOWAIT.

`f58af467a2ad5` "SUNRPC: Mark the tasks a receive worker may complete
on its own" (W=1 clean on the client): `rpc_task_setup`'s
`recv_run_ok`, kept in the task as `tk_recv_run_ok` in the spare bits
beside `tk_priority` (`tk_flags`' 16 bits are all taken); NFS sets it
for every O_DIRECT pgio and commit, NOWAIT or not, and the receive
worker completes only tasks with it.

`ab-recvok.sh`: B (the client's `updates/` stack) against P, 4 KiB
random READs at iodepth 96, B P B P B P this time for the drives'
spread (t2-r1145 to r1150), round 139's handoff trace on P (r1151),
then the WRITE and READ passes on P (r1152, r1153).
