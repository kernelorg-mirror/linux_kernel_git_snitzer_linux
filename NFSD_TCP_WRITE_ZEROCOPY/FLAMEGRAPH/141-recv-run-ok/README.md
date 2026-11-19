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

## The runs (06:22 to 06:55): on the new path, and no gain

| Run | Arm | 4 KiB IOPS | Port a / b (M) | p99 a / b (ms) | Client us a READ |
|---|---|---|---|---|---|
| t2-r1145 | B | 3.52 M | 1.81 / 1.71 | 3.49 / 3.36 | 11.86 |
| t2-r1146 | P | 3.54 M | 1.74 / 1.80 | 3.46 / 3.42 | 11.80 |
| t2-r1147 | B | 3.38 M | 1.59 / 1.79 | 7.77 / 3.36 | 11.79 |
| t2-r1148 | P | 3.44 M | 1.75 / 1.68 | 3.46 / 3.59 | 12.00 |
| t2-r1149 | B | 3.49 M | 1.72 / 1.78 | 3.59 / 3.42 | 11.83 |
| t2-r1150 | P | 3.41 M | 1.79 / 1.63 | 3.78 / 6.32 | 11.82 |
| mean | B | 3.46 M | | | 11.83 |
| mean | P | 3.46 M | | | 11.87 |

The two long tails (t2-r1147 port a, t2-r1150 port b) each came with
that port's drives uneven (1.3-1.9 and 1.4-2.0 GB/s each).

The trace on P (t2-r1151, 3.53 M), against round 139's on B (t2-r1137):

| Step, mean | B | P |
|---|---|---|
| submit -> send | 228 us (same CPU, 76%), 334 (other) | 300 (75%), 370 (other) |
| send -> reply matched | 868 us | 831 us |
| reply matched -> completion | **183 us** | **17 us** |
| submit -> completion | 1.35 ms | 1.26 ms |

- **The READs take the new path**: the wait from the match to the
  completion is 17 us, from 183.
- **It saves no CPU and no rate.**  The client is the limit, every CPU
  85 to 88% busy; at a fixed iodepth the time saved after the reply
  comes back as more time waiting for the transport before the send
  (+70 us), and the client's CPU a READ is the same, 11.8 to 11.9 us.
  The 183 us was queueing behind work the CPU had to do anyway, not a
  cost of its own.  Only less CPU a READ raises IOPS here.
- Passes on P: WRITE t2-r1152 **77.87 GB/s PASS**, READ t2-r1153
  **83.13 GB/s PASS** (io_uring, so on the new path), no new kernel
  warning.  The client back on its `updates/` stack and the settled
  setting after.
