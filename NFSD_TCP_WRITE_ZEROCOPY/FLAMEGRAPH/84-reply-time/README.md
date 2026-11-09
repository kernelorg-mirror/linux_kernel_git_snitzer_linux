# Round 84: a WRITE's time from its last piece to its reply

9 November, tardis2, canary `7.3.3-13.hs.519.canary`, round 82's stack
(`$L/svcstack-ST`, `kernel-7.3/canary.ST`).

Round 83 found that staying reads each piece sooner, so round 82's loss of
2.3 GB/s is not in the wait for a piece.  With 128 WRITEs in flight the
rate is the inverse of a WRITE's latency, so the 3% is somewhere else in a
WRITE's life.  This round times the rest of it on the server and takes the
client's view of the same window.

`reply-time.bt` keys each WRITE by its `svc_rqst` and times:

| stage | from | to |
|---|---|---|
| `@recv_us` | the `svc_tcp_recvfrom()` that starts the record (`sk_tcplen` 0 at entry) | the one that returns it whole |
| `@to_write_us` | the record returned | `nfsd_vfs_write()` entry |
| `@write_us` | `nfsd_vfs_write()` entry | its exit |
| `@to_send_us` | `nfsd_vfs_write()` exit | `svc_tcp_send_record()` for it, by whichever thread combines the send |
| `@send_us` | that | its `svc_tcp_sendto()` returns |
| `@last_to_reply_us` | the record returned | `svc_tcp_sendto()` returns |
| `@whole_us` | the record's first read | `svc_tcp_sendto()` returns |

Only records that reach `nfsd_vfs_write()` are counted.  `ms.py` takes the
client's WRITE ops and their mean queue, RTT and execute times from two
copies of its `/proc/self/mountstats`, taken just before and after the
trace.

`run347.sh` swaps the stack in once, runs A (`svc_tcp_rx_stay_us=0`,
`t2-r347`) and B (1000, `t2-r348`) with the trace for five seconds in each
window, and puts the stock stack back.

## Result: the extra time is in the write, and in its slow tail

`run347.out`; the histograms are in `reply-time-A.txt` and
`reply-time-B.txt`.  Neither run raised a new kernel warning, no drive
stepped down, and the stock stack is back in the settled setting.

| mean, us a WRITE | A, off (`t2-r347`) | B, on (`t2-r348`) | B - A |
|---|---|---|---|
| rate, with the trace on | 69.18 GB/s | 68.39 GB/s | -0.79 |
| WRITEs timed in 5 s | 329,649 | 327,100 | |
| record, first read to last | 355.7 | 358.9 | +3.2 |
| last read to `nfsd_vfs_write()` | 10.9 | 11.5 | +0.6 |
| `nfsd_vfs_write()` | 348.6 | 360.8 | **+12.2** |
| write done to the send | 12.0 | 12.4 | +0.4 |
| the send | 9.1 | 9.4 | +0.3 |
| last read to reply | 380.5 | 394.2 | +13.7 |
| first read to reply | 736.2 | 753.0 | **+16.8 (2.3%)** |
| client WRITE RTT (mountstats) | 1,320 | 1,347 | +27 (2.0%) |

- **The server's extra time matches the client's.** A WRITE spends
  16.8 us (2.3%) longer between its first read and its reply.  The
  client's RTT grows by 2.0%, so the loss is on the server, not on the
  wire or the client.
- **Most of it is in `nfsd_vfs_write()`: 12.2 of the 16.8 us.**  The
  stages that staying touches directly cost little extra:
  - the record's receive is 3.2 us longer;
  - the handoffs to the write and to the send add about 1 us together.
- **The write's extra time is in its tail, not its body.**
  - The bins from 128 us to 1 ms hold the same counts in A and B.
  - Writes over 2 ms: A 1,515, B 2,572.  At the bins' middles those
    extra writes come to about 14 us a WRITE, all of the difference.
  - A write over 2 ms is the drive or the block layer, not sunrpc.
    Whether staying causes them (by when it submits, or how it
    competes for the CPUs that complete the I/O) or this pair just
    drew a worse tail is not settled by one run each.
- **The trace narrowed the gap.**  With the trace on, B is 0.79 GB/s
  (1.1%) lower; untraced in round 82 it was 2.3 GB/s.  So these figures
  explain some of round 82's loss, not necessarily all of it.
- The receive tails differ in shape: B has 2,259 records at 2 to 8 ms
  where A has few; A has 65 over 16 ms where B has 10.  The means come
  out nearly equal.
