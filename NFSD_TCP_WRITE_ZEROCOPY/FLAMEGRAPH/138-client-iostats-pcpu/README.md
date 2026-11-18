# Round 138: the client's RPC iostats per CPU

Mike, 18 November: "yes, make the RPC iostats per-CPU", from round
137's profile: `rpc_count_iostats_metrics()` 2.17% of the client's
samples and the spin lock it takes 0.52%, about 0.36 us of the
client's CPU a READ at 3.59 M IOPS, with the client the limit.

The change, two code commits (W=1 clean on the client; no sparse
there):

- `7e85409ea83f6` "NFS: Name a flexfiles op's RPC iostats by offset,
  not by element": `cl_metrics + op` for `&cl_metrics[op]`, no change
  in behaviour.
- `772a7f50696af` "SUNRPC: Count a client's per-procedure iostats per
  CPU": `cl_metrics` is a per-CPU array; a task adds to its CPU's copy
  with preemption off in a `u64_stats_sync` section, no lock;
  `rpc_clnt_show_stats()` sums the CPUs.  The cost is memory: about 72
  bytes a procedure a CPU, so an NFSv3 client (22 procedures) holds
  about 76 KiB on 48 CPUs where it held about 2.8 KiB, and NFSv4's
  procedure table is about three times longer.

`ab-iostats.sh`: B (the `updates/` stack) against P, 4 KiB random READs
at iodepth 96 B P B P (t2-r1129 to r1132), then the WRITE and READ
passes on P (r1133, r1134).
