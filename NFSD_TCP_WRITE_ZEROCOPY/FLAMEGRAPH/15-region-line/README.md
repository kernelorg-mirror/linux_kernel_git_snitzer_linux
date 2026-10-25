# Round 15: `rx_region` off the line the region stack writes

**The change:** "page_pool: Keep rx_region off the line the region stack writes" (`085734569d9d`).
- `svc_tcp_rx_append()` checks that each page of a loaned skb is the region's by reading its pool's `rx_region`.
- At offset 864 that field shared a line with `rx_stack` and `rx_stack_n`, which the ring writes for every region page it takes or returns. From round 14's profile, about 0.6 µs a WRITE of false sharing on nfsd.
- The change moves `rx_region` to the struct's last line, written only at registration (offset 968 in build 499).
- It has no switch, so there is no A/B: the round is the load itself, before and after.

**Profile 2026-10-25 on tardis2**, fl15-loan64: build 499 (the tip, `bca1a2f143eb`), on the kernel's own sunrpc (`EFD53C53F61C1F0CDF37D89`) and nfsd (`3F0C341E86E266D5FDF2ADF`), the settled setting, `rx_region_stack` 0, the reply cache at its default. 40.56 GB/s, no drive stepped down. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run33.sh`. Against it, fl17-loan64: build 494 with round 17's sunrpc.

## Outcome: kept in the kernel; about 0.1 µs a WRITE, not 0.6

[annotate.txt](annotate.txt): the load of `pool->rx_region` and the branch on it, which takes the load's skid, as shares of `svc_tcp_rx_append`'s self time:

| | offset | the load and its branch | `svc_tcp_rx_append` self | the load, µs a WRITE |
|---|---|---|---|---|
| fl17 (build 494) | 864 | 11.0% | 2.84 µs a WRITE | 0.31 |
| fl15 (build 499) | 968 | 8.6% | 2.72 | 0.23 |

- **The line is still missed:** the load is about as hot as before, so most of what it waits on was not the line it shared with the stack. What it does wait on was not looked for.
- **The saving is about 0.1 µs a WRITE**, across two kernels, within one profile's scatter. The estimate of 0.6 counted the whole load as false sharing.
- It stays in the kernel: it costs nothing, and the false sharing it removes grows with the ring's rate.

## What else the profile shows

From [top-nfsd.txt](top-nfsd.txt) and [top-rings.txt](top-rings.txt), against fl17, µs a WRITE:

| | fl17 (build 494) | fl15 (build 499) |
|---|---|---|
| the rings, busy | 61.4 | 59.2 |
| nfsd, busy | 117.9 | 111.8 |
| `nfsd_cache_lookup` | 3.68 | 3.18 |
| `page_pool_put_netmem_bulk` | 8.45 | 8.34 |
| `svc_tcp_rx_region_span` | 0.99 | 0.85 |

- nfsd is 6 µs a WRITE cheaper on build 499, from one profile a side and across a change of kernel, sunrpc and nfsd modules from `updates/` to the kernel's own. Not attributed to anything.
- fl15 is the stack-0, default-cache profile for rounds 04 and 12.
