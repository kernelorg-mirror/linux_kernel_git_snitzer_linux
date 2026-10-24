# Round 06: a receive low-water mark while a loaned record is incomplete

**The change:** "SUNRPC: Wait for the rest of a loaned TCP record before waking a thread" (`c8cf27841351`).
- While a record that holds loan credit is incomplete and a read drained the socket, `sk_rcvlowat` is raised through `->set_rcvlowat` to what the record still lacks. TCP caps it at half the receive buffer.
- Records that are copied keep a mark of 1.
- It was based on the dropped `patches/dropped/0003`, which raised the mark for every record and cost copy mode 2-3 GB/s.

**Run 2026-10-23/24 on tardis2**, build 494.
- A is round 05: `r5-sunrpc.ko`, srcversion `91F9753F3DA76DEF91A628D`.
- B is round 06: `r6-sunrpc.ko`, srcversion `8E4688E2FCF9F5FBD999426`.
- The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run23.sh`. Each run's snapshots also hold TCP's `InSegs` and `OutSegs`.

## Why

In round 05's profile, a 1 MiB WRITE was taken up 12 times (22 hand-offs of the record). Each pass is:
- a wake;
- `lock_sock`/`release_sock`;
- the hand-offs;
- often an ACK from `tcp_cleanup_rbuf`, 18 µs a WRITE.

That came to about 50 µs a WRITE of nfsd's time. For a loaned record a pass copies nothing, so it is all overhead.

## Outcome: not kept. nfsd 22% cheaper, the rings 27% dearer

**KUnit on B:** 7, 50, 7 and 10 pass.

| run | GB/s | ring µs a WRITE | nfsd µs a WRITE | wakes a WRITE | refusals | TCP segments out a WRITE | region pages, peak |
|---|---|---|---|---|---|---|---|
| r6-1-loan64-A | 40.49 | 67.5 | 153.4 | 6.41 | 1.03 | 12.55 | 6641 |
| r6-2-loan64-B | 40.17 | 86.3 | 124.0 | 3.53 | 0.37 | 18.26 | 6558 |
| r6-3-loan64-A | 40.46 | 66.3 | 154.8 | 6.45 | 1.02 | 12.53 | 6388 |
| r6-4-loan64-B | 40.32 | 84.9 | 119.1 | 3.58 | 0.45 | 18.24 | 6498 |
| r6-5-loan64-A | 40.44 | 67.2 | 155.2 | 6.53 | 1.00 | 12.96 | 6369 |
| r6-6-loan64-B | 40.36 | 83.9 | 119.9 | 3.61 | 0.68 | 17.68 | 6652 |

Every run was settled and DRIVE-LIMITED, with no drive stepped down.

**It did what it was for, on nfsd.**
- Profiled: `svc_tcp_recvfrom` 12.1 to 3.8 calls a record, exchanges 22.2 to 5.6.
- nfsd 154.5 to 121.0 µs a WRITE (-33.5).
- `tcp_cleanup_rbuf` on nfsd 19.6 to 7.1, `svc_thread_wait_for_work` 5.8 to 3.0, the wake 4.9 to 2.4.
- Fewer refusals and no larger working set: the region's peak pages in use were unchanged.

**It moved the ACKs onto the rings.** In the profile:
- `tcp_send_ack` on the rings rose from 0.91 to 15.28 µs a WRITE, all from `__tcp_ack_snd_check` in the softirq.
- `svc_data_ready` fell only from 5.86 to 2.72.
- TCP sent 5.5 more segments a WRITE.

This is TCP's own rule. Since Eric Dumazet's "tcp: fix delayed acks behavior for SO_RCVLOWAT" (2018), an ACK is not delayed while the receive queue holds fewer bytes than `sk_rcvlowat`, so a sender is not left waiting on a mark it cannot see. Every skb that arrives below the mark is acknowledged at once, on the receiving CPU. Any mechanism that works through `sk_rcvlowat` pays this. The rings are the next limit, so the round is not kept. It is reverted at the tip (`b8c9ee7c6d2a`), and tardis2 is back on round 05's module.

## What it leaves: the same saving without `sk_rcvlowat` (06b, Mike to decide)

The nfsd saving is real and large. It came from waking a thread less often for a loaned record; the ACKs came only from the mechanism. A version that leaves `sk_rcvlowat` at 1 keeps TCP's delayed ACKs:
- **The idea:** `svc_data_ready()` would decline to set `XPT_DATA` for a loaned record still short of its length, until the queued bytes (`rcv_nxt - copied_seq`) cover what it lacks.
- **Stalls:** it must still wake a thread when TCP would report the socket readable for memory or window reasons (the `tcp_rmem_pressure()` and receive-window tests of `tcp_epollin_ready()`), so the window cannot close on a sleeping record.
- **The risk:** fewer reads mean fewer window updates. ACKs would come from the reads that remain and from TCP's delayed-ACK timer. The sender, with 768 KB of window per connection, must not stall. The A/B would watch retransmits, refusals, segments a WRITE and the rate.
- **The tests:** it needs KUnit coverage of the short-record path, and an A/B on both yardsticks, because a record that waits longer for its rest may wait longer in latency too.
