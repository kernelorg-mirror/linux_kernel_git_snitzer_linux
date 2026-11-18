# NFS WRITE throughput on tardis2: commits, configuration, TODO

Started 17 November 2026 (Mike: "create a WRITE.md project file to
track the important commits and configuration and TODO items").  It is
the WRITE counterpart of `IOPS.md`: where WRITEs from the client
stand, what carries them on each end, and what is owed.  The history
behind it is in `README.md`, `XEU_CONVERGENCE.md` and the FLAMEGRAPH
rounds; the hard rule on WRITEs is AGENTS.md's "Every READ round keeps
WRITEs at 78 GB/s".

**Priority (Mike, 17 November): IOPS first.**  "I would like to recover
the client's WRITE path.. but I don't want to take the time to do that
at the expense of IOPS right now."  The TODO below waits on `IOPS.md`'s
rounds.

## Where it stands

`xeu-rig/write-check.sh`: one two-port WRITE run on all eight drives at
`settled.sh`'s setting, fio 8 MiB writes on libaio, 16 jobs a port at
iodepth 16, nconnect 16 a port, `WARM=3 RUNTIME=15`.  It passes at
**74.6 GB/s since 18 November** (75.0 from 17 November, 77.6 before;
TODO 2) with no drive
stepped down and no new kernel warning.

| Client kernel | Runs | GB/s | Drives each (busiest) | Client busy |
|---|---|---|---|---|
| build 469 (`7.3.0-0.rc7.2.hs.469`) | t2-r858 (round 106), t2-r868 to r871 (round 107) | 77.73 to 77.91 | 9.7 to 9.8 GB/s (98%) | 48 to 50% |
| build 541 (`7.3.3-16.hs.541.canary`, the canary-WIP tip) | t2-r905, t2-r907 (round 109) | **74.85, 75.16** (RERUN twice) | 9.2 to 9.5 GB/s (95 to 96%) | 39% |

On build 541 every one of the 32 server sockets carries a few percent
less (41 to 51 GB a run against 43 to 53), and the client is less busy,
not more: the client sends less, evenly, and nothing on either end is
full.  The server's only change between the two is that nfsd is NFSv3
only (`settled.sh`, round 109), which WRITEs over NFSv3 do not reach.
Mike: "has to be client side difference.. I upgraded that client
kernel".  **The mark is lowered to 75.0 for build 541's client (Mike,
17 November: "for now we must tolerate that WRITE has dropped down to
75 GB/s, we will hunt down the client regression after we get IOPS
performance up")**, until its WRITE path is recovered (TODO 2).  The
tip's server stack on build 541 read 74.91 (t2-r915, RERUN under
both marks).  **Lowered again to 74.6 on 18 November (Mike: "just lower to 74.6,
this is a known issue we need to get back to later.. IOPS is what
matters for now")**, after the export borrow's build (FLAMEGRAPH round
110) read 74.55 and 74.98 (t2-r931, r934), about 0.3 under the stacks
before it (74.85 to 75.25) and inside their spread; whether the
borrow costs WRITEs anything is not known (TODO 5).

For scale: the drives take 9.8 GB/s each at 98% busy (78.4 for eight),
and a WRITE of 1 MiB costs the client about 2.17 bytes of memory
traffic a byte sent (round 107, `FLAMEGRAPH/107-client-rx/README.md`),
which is not explained yet.

## The client's WRITE path: what build 541 changed

None of these is on build 469; all are on the canary-WIP tip and in
build 541 (checked at `4c8aa21d29987`, `IOPS.md`).  Any of them can
move a 1 MiB O_DIRECT WRITE's rate, and none was measured on WRITEs
alone:

| Commit | Author | What it does to a WRITE |
|---|---|---|
| `1b9af79d9f16f` SUNRPC: Splice a request's pages into the TCP socket | Mike Snitzer | sends a first transmission's pages with `MSG_SPLICE_PAGES`, no copy into the socket (carried on build 469 as `patches/client/0002`, in `updates/`) |
| `8c2abe4089def` SUNRPC: Make rpciod and xprtiod visible in sysfs | Mike Snitzer | `WQ_SYSFS`, so `twoport.sh` can hold xprtiod strict (`patches/client/0001` on build 469) |
| `ccaaf769083fe` SUNRPC: make rpciod a per-CPU workqueue | David Flynn | each WRITE's async legs run on the CPU that queued them; on build 469 rpciod was unbound and held strict to the node |
| `5555163d661a0` SUNRPC: give each CPU its own transport under nconnect | David Flynn | a WRITE goes on the submitting CPU's transport, not round-robin; a CPU spills to round-robin at 64 tasks |
| `a37330054f3ca`, `994e5b01c627c` home nconnect transports on their NIC's node, on distinct cores | Jonathan Flynn | which CPU each of a port's 16 transports belongs to (`sunrpc.xprt_numa_map`, Y) |
| `50eee8803afc5` SUNRPC: let NFS pgio and commit run the first RPC leg in the caller | David Flynn | an O_DIRECT WRITE and COMMIT run their first leg in fio's context (libaio is not NOWAIT, so it applies) |
| `ba5642ee4cac2` SUNRPC: hand a sent task from sending to pending without a requeue | David Flynn | one rpciod trip less a WRITE |
| `7cc54f28c69fc` SUNRPC: Shard the rpc_clnt task list | Jonathan Flynn | `cl_lock` in 16 shards |
| `d398df1554edd` NFS: admit O_DIRECT I/O without taking inode->i_rwsem | Jonathan Flynn | no `i_rwsem` a WRITE |
| `32faf45611a7d` NFS: finish stable O_DIRECT writes without the nfsiod round trip | Jonathan Flynn | the end of a stable WRITE without nfsiod |
| `bcd79ae606aa8` NFS: io_uring NOWAIT admission for NFSv3 O_DIRECT I/O | David Flynn | NOWAIT WRITEs only with `nfs.nowait_writable` (N at the tip); libaio is not affected |
| `334ea04bd3718` NFS: NOWAIT direct write over RPC defers request build and issue to a workqueue | David Flynn | NOWAIT WRITEs only |

The rig's own client settings that meet them: `twoport.sh` pins each
port's writers to the CPUs of its NIC's node (`PIN=1`), sets the
client's workqueue `default_affinity_scope` to numa for the run
(`CLI_SCOPE`; build 541's default is cache_shard) and holds an unbound
xprtiod strict (`CLI_STRICT`; rpciod, per-CPU on build 541, is left as
it is since `fb04a6fde366b`).

## The server's WRITE path: what is settled

`xeu-rig/settled.sh` is the one place it is held, and every run audits
against it.  For WRITEs on tardis2:

- **Loans in the two-port runs**: at rest the receive budget is 0
  (`svc_tcp_rx_budget_pages`, nothing loaned), but `write-check.sh`
  and `read-check.sh` run `twoport.sh` with `WRITES=$TWO_WRITES
  BUDGET_PAGES=8198`: a budget of 8198 pages, 64 loaned 1 MiB WRITEs
  (`common.sh`'s `TWO_WRITES`, 64 on tardis2 since 12 November, round
  94), reserve 130, a window of 10 loaned WRITEs an export
  (`TWO_WINDOW`), the held receive buffer 512 KiB (`TWO_RCVBUF`; 750
  KiB at rest), wsize 512 KiB (`TWO_WSIZE`).  Also
  `svc_tcp_rx_want_eighths` 6, spare 64 MiB, the region 32768 pages,
  `page_pool.rx_region_stack` 96.  AGENTS.md: the loan path is kept as
  fast as it can be, and the region's footprint is not to grow.
- **Where work runs**: nfsd's threads and rtc work on CPUs 8-55, the
  rings on the rest; `svc_rtc_balance` Y (each connection on the
  least-loaded work CPU); RFS 32768 on tardis2; `net.core.skb_defer_max`
  0.  nfsd's `rtc_read` is Y and `rtc_write` N: WRITEs run on nfsd's
  threads, not in rtc work items.
- **nfsd**: 512 threads, `pool_mode` pernode (per-CPU pools cost
  WRITEs on builds 512 and 514), `io_cache_write` 2 (direct),
  `max_block_size` 1 MiB, the reply cache at 4096 entries, NFSv3 only
  with nfsdcld masked (round 109).
- **Drives**: each its own XFS, `/hs/nvme0` to `/hs/nvme7`, mounted
  `noatime,lazytime`, the export `sync` with the drives' caches
  write-through (the mode that acts like enterprise storage).
- **TCP**: `tcp_limit_output_bytes` 256 KiB on tardis2 (round 105, for
  READs, kept by the WRITE pass of t2-r858), `tcp_wmem` max 4 MiB,
  `tcp_rmem` max 32 MiB, cubic, fq_codel.
- **The connections**: nconnect 16 a port (`common.sh`'s `TWO_NCONN`).

## TODO

1. **Recover the client's WRITE path on build 541** (Mike, 17 November;
   after IOPS).  From 77.8 on build 469 to 75.0 on build 541 at the
   same server.  In order:
   - profile the client in a WRITE pass on build 541 (perf is on it
     again, `perf-7.1.13-21.hs.460`) beside round 107's on build 469
     (`FLAMEGRAPH/107-client-rx/ab-write.sh`), with the server sockets'
     Send-Q and the client's per-transport queue lengths;
   - the A/Bs the client allows without a rebuild: `CLI_SCOPE`
     cache_shard against numa, `PIN=0`, nconnect 12 and 24 (the
     per-CPU transport map gives 24 writer CPUs a port 16 transports);
   - then, if those do not, the commits above one at a time on a
     client module built without each (the splice first: its pages
     held until TCP sends them, and the per-CPU transport map second).
2. **Done 18 November, late (Mike: "yes, put the WRITE mark back to
   77.6, and raise the READ mark to the same").**  TODO 1's cause is
   `5555163d661a0`, the per-CPU transport choice for nconnect
   (FLAMEGRAPH round 136): reverted at the tip with the commits on it,
   the client writes 78.00 (t2-r1118), and both passes' marks are 77.6
   in `xeu-rig/write-check.sh`, `xeu-rig/read-check.sh` and AGENTS.md.
   A pass runs with the client on a stack built from the tip until a
   client kernel carries the reverts.  The TODO was: reset the WRITE
   pass's mark to 77.6 GB/s once whatever
   holds back build 541's client is fixed (Mike, 17 November).  It is
   74.6 in `xeu-rig/write-check.sh` (the `g >= 74.6` test and its
   header) and in AGENTS.md's hard rule only for that client; when
   TODO 1 brings the pass back to 77.6 or more, put 77.6 back in both
   places and say so here.  A READ or IOPS round's WRITE pass is
   recorded with the client's build beside it.
3. **The client's 2.17 bytes of memory a byte on WRITEs** (round 107),
   on build 469: what they are, and what build 541 makes of them.
4. **The client's RPCs in flight**: the pass's fio writes 8 MiB at a
   time, and the two-port runs mount with wsize 512 KiB on tardis2
   (`TWO_WSIZE`), so each is 16 WRITE RPCs: 16 jobs at iodepth 16 make
   up to 4,096 RPCs a port on 16 transports, far past the 64 a
   transport at which build 541's per-CPU map spills to round-robin.
   Which transport each WRITE takes on build 541 is worth reading
   before any A/B (the per-transport queue lengths in the client's
   sysfs, `/sys/kernel/sunrpc/xprt-switches/`).
5. **Whether the export borrow costs WRITEs** (round 110): WRITE passes
   on one build at `nfsd.export_borrow_slots` 16 and 0 in turns, three
   a side.  Not run (Mike, 18 November: IOPS first).
