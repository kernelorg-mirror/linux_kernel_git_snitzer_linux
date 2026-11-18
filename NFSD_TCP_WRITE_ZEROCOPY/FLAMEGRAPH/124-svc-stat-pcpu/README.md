# Round 124: the server's per-call statistics per CPU

Mike, 18 November: "fix request setup next, quick A/B".

Round 119 put the request's set-up (`svc_generic_init_request` from
`nfsd_init_request`) at about 0.3 us a READ.  `perf annotate` on
t2-r1028: 80% of its samples on the load of `sv_stats->program`, 13% on
`sv_stats->vs_count`: cache misses on `struct svc_stat`'s first line,
which also holds `netcnt`, `nettcpcnt` and `rpccnt`, each raised with a
plain increment by every call on every CPU (`svc_xprt_recv_call()`,
`svc_tcp_recvfrom()`, `svc_process_common()`).  The memsets are not the
cost.

`867b384feee73` "SUNRPC: Count the server's per-call statistics per
CPU": the three counted per CPU where `svc_stat_alloc_counts()` runs
(nfsd), summed by `svc_seq_show()` and nfsd's netlink statistics.
Built into `/var/tmp/svcstack-541-svcstat` (sunrpc
`9F8293B3F9A7600316512FE`, nfsd `78F201D2B9A190D3A538FA9`), W=1 clean.

The quick A/B: one `prof-srv.sh` run (t2-r1029) against t2-r1028.

## Result: t2-r1029 (18 November, 08:19 to 08:23)

**2.88 M IOPS** (1.37 / 1.50), p99 3.59 / 3.19 ms, server 77% busy, no
new kernel warning: the most yet at this shape (t2-r1028 2.83).

| self | t2-r1029 | t2-r1028 |
|---|---|---|
| `svc_generic_init_request` | 0.21% | 1.26% |
| `svc_xprt_recv_call` | 0.34% | 1.03% |
| `svc_process_common` | 0.64% | 0.16% |

Net -1.26%, about 0.28 us a READ.  `svc_process_common`'s rise is on
the per-CPU `rpccnt` increment (`ldadd`, 57% of its samples on the
instruction after it): a miss on the CPU's own copy, not a line shared
with other CPUs.  `/proc/net/rpc/nfsd` after the run: `net 76104798 0
76104798 104`, `rpc 76104807 0 0 0 0`, the per-CPU counts summed.
