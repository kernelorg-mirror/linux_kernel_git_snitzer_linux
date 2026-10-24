# L0: the latency baseline at low depth

**Run 2026-10-24 on tardis2**, build 494, with round 05's sunrpc (`91F9753F3DA76DEF91A628D`) and the settled setting of rounds 01 and 03.
- The run is `xeu-rig/lat.sh /var/tmp/zc-run-logs/t2-tipkernel L0`, driven by `run24.sh`.
- 4 writers, one to each of the four drives, iodepth 1, 1 MiB writes, so one NFS WRITE an I/O and four in flight.
- The rate was 7-8 GB/s, with the drives far from saturation.
- The loan budget is 64 WRITEs, so every WRITE is loaned.

## fio's completion latency at the client ([fio.txt](fio.txt))

| run | GB/s | p50 | p90 | p99 | p99.9 | mean | max |
|---|---|---|---|---|---|---|---|
| L0-u, untraced | 6.94 | 0.522 ms | 0.586 | 0.676 | 0.741 | 0.520 | 1.014 |
| L0-t, with `lat.bt` | 7.83 | 0.457 ms | 0.561 | 0.643 | 0.717 | 0.472 | 0.820 |

The traced run came out faster than the untraced one, so two runs on the same setting differ by about 10%. A latency comparison needs several runs a side, like the CPU A/B.

## The server's split ([lat-bt.txt](lat-bt.txt), log2 buckets, µs)

| what | where most fall | notes |
|---|---|---|
| `nfsd_vfs_write`, the write into XFS and the drive | 128-256 | all 412,921 WRITEs; the drive's own time |
| `svc_process`, the whole call | 128-256 | the write, plus a few µs |
| `svc_tcp_sendto`: the loan given back, then the reply sent | 8-16 and 16-32, about half each | the release comes before the send (round 05's notes); left so on purpose, to return pages to the LIFO soonest (Mike) |
| `svc_xprt_dequeue` qtime: queued to taken | 1-4 | |
| `svc_xprt_dequeue` wakeup: asked to wake to running | 2-4 mostly; 8-32 a second mode; **128-512 for about 2.5%** | 140,000 wakes of 5.8 million took over 128 µs |

## What it says

- **About 0.2 ms of a WRITE's 0.5 ms is the drive.**
  - About 0.02 ms is the reply.
  - The rest is the record's arrival and the client's own time.
  - The record arrives over about 12 passes (round 02), so its receive is spread over the transfer and is not one probe here. A next probe for the split: from a record's first byte to its last, on the server.
- **The wake has a tail.** 2.5% of wakes take 128-512 µs.
  - Some of those wakes are for the passes over a record, not for a whole WRITE. So how many WRITEs it delays is not known from this.
  - It is the first thing to look at for latency: what a thread is doing when a wake takes that long (an idle CPU's exit from a deep state, or the CPU busy with something else).
