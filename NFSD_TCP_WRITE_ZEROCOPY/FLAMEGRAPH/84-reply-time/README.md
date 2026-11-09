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
