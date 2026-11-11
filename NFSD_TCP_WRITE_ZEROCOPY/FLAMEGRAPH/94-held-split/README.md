# Round 94: where the region's held pages sit on tardis2

**The ask (Mike, 11 November):** measure what round 93 left open: of
the 56 MiB held at the median (1.76 times the L3), how much is in
records being received on their connections, in the sockets' queues,
and waiting on the drives.  Which lever to pull depends on that split.

## What is known before it

- Round 93 (`t2-r479..r484`, the settled setting, 71.66 GB/s): 3,595
  pages held at the median, a page held 0.82 ms, 1.69 bytes through
  memory a byte of payload.
- Round 56 (build 50x, 16 connections a port, `pagelife.bt`, one page
  in eight followed through its cycle) put about a third of the pages
  in a record's assembly, a fifth to a quarter on the ring and in the
  socket, a tenth at the drive, and the rest on the way back.  Its
  trace slowed the server 15 to 20%, and its probes
  (`svc_tcp_rx_put_run`, `page_pool_alloc_pages`' hand-out to the
  driver) are gone from or changed in this kernel.

## The method

Per record, not per page, so the trace is light:
[heldsplit.bt](heldsplit.bt) follows each loaned record by its
transport and sequence on the `svcsock_tcp_rx_lifetime` tracepoint
(publish, payload-release, release), the bytes nfsd reads off the
socket into it (`svc_tcp_rx_walk_skb` on a read's root skb), and its
`nfsd_direct_write()` (a kprobe: the function has no BTF entry), and
sums each stage's page-microseconds: assembly (each byte from its read
to the publish), publish to the WRITE, the drive, the WRITE's return to
the release, and records with no direct write.  Over the time traced
each sum is the stage's mean pages held.  `rtc_write` is off, so the
WRITE runs on the nfsd thread and its time in `nfsd_direct_write()` is
the drive's.

Beside it [sockq.py](sockq.py) samples every 50 ms the bytes charged to
nfsd's connections' receive queues (`ss`'s `skmem` r), and the rings'
posted buffers are taken as full (512 pages).
[heldsplit-read.py](heldsplit-read.py) sets them all against the mean
held over the trace's span (`regionsample.py`); what is left is a
page's way back after its release (an skb still holding its other
buffer, a pool's stack or alloc cache) and anything unseen.

[run485.sh](run485.sh): three two-port runs on the settled setting from
`t2-r485`, `WARM=10 RUNTIME=40`, the held mark on, the trace and the
sampler from the window's start.  The trace's cost is read from the
rate against round 93's 71.66 GB/s.
