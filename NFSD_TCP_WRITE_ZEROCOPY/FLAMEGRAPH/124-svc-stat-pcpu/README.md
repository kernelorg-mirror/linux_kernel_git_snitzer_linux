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
