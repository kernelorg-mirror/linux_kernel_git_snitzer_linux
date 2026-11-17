# NFS 4 KiB random-READ IOPS, and what the client needs to scale

Started 17 November 2026 (Mike: "start a new project document that is
focused on IOPS and client side advances to scale").  The goal is the
most 4 KiB random READs a second from one Linux NFS client to one
kNFSd, on tardis2 and its client.  The rate work on 1 MiB READs and
WRITEs is in `FLAMEGRAPH/104-rx-cpus` to `107-client-rx`, and goes on
apart from this.  Here the cost that matters is each RPC's, not each
byte's, and most of that cost is on the client.

The reference is David Flynn's
`nfsv3-max-iops-agent-instructions-20261117.md` (in this directory):
**3.59 M IOPS** on one NFSv3 mount between two AWS aarch64 hosts (192
vCPU each), 3.17 M typical without one NFSv4.1 patch, and about 575 K
with a stock client.

## Where it stands

All on tardis2 (build 534, 64 Neoverse-N2 cores, eight DapuStor drives
on two E1 ports) and its client (build 469, two Xeon Gold 6542Y, 48
CPUs, two ports: 192.168.1.106 on node 0, 192.168.0.106 on node 1).
The account is `FLAMEGRAPH/108-4k-randread/README.md`.

| What | M IOPS | Limit |
|---|---|---|
| The drives, local fio on the server, 8 jobs a drive at iodepth 64 | 18.78 | the server's CPUs, 93% busy |
| NFS, settled setting, 32 connections (t2-r872 to r874) | 0.95 to 0.98 | the client, 85 to 89% busy |
| NFS, `settled-iops.sh`, iodepth 32 (t2-r875 to r877) | **1.05** median, 1.05 min | the client, 94 to 95% busy |
| NFS, `settled-iops.sh`, iodepth 16 / 64 (t2-r878, r879) | 1.00 / 0.94 | the client, 84% / 100% |

NFS reaches about 5.6% of what the drives do, and the server is about
half idle throughout.  The wire is not close: 1 M READs of 4 KiB are
about 35 Gbit/s.

## What limits the client now: the workqueue's pool lock

t2-r880 profiled both ends at iodepth 32 (`prof4k.sh`).  **The client
spends 63.5% of its cycles in `native_queued_spin_lock_slowpath`**, and
nearly all of it is the workqueue pool lock:

- Every async step of every RPC is queued on **rpciod**
  (`rpc_wake_up_queued_task` 15.5%, `rpc_async_schedule` 14.0%), and
  every socket receive on **xprtiod** (`xs_stream_data_receive_workfn`
  18.9%).
- On build 469 both are unbound workqueues with `affinity_scope` numa,
  so the 24 CPUs of each socket share one worker pool (`kworker/u217`,
  `u218`).  `__queue_work` is 32.9% of cycles with its children, and
  `process_one_work`, where the worker takes the next item, most of the
  rest.
- The rpc_clnt's `cl_lock` does not show (`call_reserveresult` and
  `rpc_task_release_client`).  That is why mounting the eight
  filesystems from eight addresses (eight rpc_clnts) gained nothing:
  every rpc_clnt queues on the same two pools.

## The client commits that remove the contention

All are on `kernel-7.3/canary-WIP` (checked at `4c8aa21d29987` on 17
November: every line each one adds is at the tip, and nothing reverts
any of them) and none is on the client's build 469.  The four of 30 and
31 October are in the base David's instructions apply to
(`2e3a1b18333ef`, under other hashes); patch 27 is in his tarball.

| Commit | Author | What it removes | Switch |
|---|---|---|---|
| `ccaaf769083fe` SUNRPC: make rpciod a per-CPU workqueue | David Flynn | rpciod's one pool per node: each CPU gets its own pool, lock and `max_active`; a task woken by a reply runs on that CPU | none |
| `50eee8803afc5` SUNRPC: let NFS pgio and commit run the first RPC leg in the caller | David Flynn | the first rpciod trip of every O_DIRECT READ, WRITE and COMMIT (`RPC_TASK_INLINE_OK`) | none; see the io_uring caveat below |
| `ba5642ee4cac2` SUNRPC: hand a sent task from sending to pending without a requeue | David Flynn | the rpciod trip a sent task made only to put itself to sleep on `xprt->pending` | none |
| `5555163d661a0` SUNRPC: give each CPU its own transport under nconnect | David Flynn | the round-robin cursor and `xps_queuelen` all CPUs wrote, and any CPU sending on any transport; the receive worker is queued on the transport's home CPU | none; a CPU spills to round-robin at 64 tasks on its transport |
| `7cc54f28c69fc` SUNRPC: Shard the rpc_clnt task list to remove cl_lock contention | Jonathan Flynn | `cl_lock` taken twice by every RPC (David's patch 27: 575 K to 2.9 M on one mount) | none |
| `a37330054f3ca` SUNRPC: home nconnect transports on their NIC's NUMA node | Jonathan Flynn | transports homed away from the NIC their route leaves by | `sunrpc.xprt_numa_map`, default Y |
| `994e5b01c627c` SUNRPC: home each switch's transports on distinct cores | Jonathan Flynn | several switches' transports homed on the same few cores | the same switch, now 0444 (set at load) |

On the O_DIRECT path, also on the tip and not on build 469:

| Commit | Author | What it does |
|---|---|---|
| `bcd79ae606aa8` NFS: io_uring NOWAIT admission for NFSv3 O_DIRECT I/O | David Flynn | io_uring issues an NFSv3 O_DIRECT READ inline with `IOCB_NOWAIT` instead of handing it to an io-wq worker (a file opened read-only gets `FMODE_NOWAIT`; a writable one only with `nfs.nowait_writable`, default N at the tip) |
| `d398df1554edd` NFS: admit O_DIRECT I/O without taking inode->i_rwsem | Jonathan Flynn | the inode's `i_rwsem` on every O_DIRECT request |
| `32faf45611a7d` NFS: finish stable O_DIRECT writes without the nfsiod round trip | Jonathan Flynn | an nfsiod trip at the end of each stable O_DIRECT write |

### What to know before reading a result on the new kernel

- **io_uring and `50eee8803afc5`.**  fio opens a randread job's file
  read-only, so it gets `FMODE_NOWAIT`, and io_uring issues each READ
  with `IOCB_NOWAIT`.  A NOWAIT pgio sets `NFS_IOHDR_NOWAIT`, and
  `nfs_initiate_pgio()` then does not set `RPC_TASK_INLINE_OK`: the
  first leg still goes to rpciod, which is now per-CPU.  With libaio
  the READ is not NOWAIT and runs its first leg inline.  David's runs
  were on io_uring too.  An A/B of the two engines shows what each path
  is worth.
- **xprtiod stays unbound** on the tip (`ccaaf769083fe`: "xprtiod and
  nfsiod keep their allocation").  `5555163d661a0` queues the receive
  worker on the transport's home CPU, but with `affinity_scope` numa
  that is still the node's one pool.  If the next profile shows
  `xs_stream_data_receive_workfn` under the pool lock, xprtiod's
  `affinity_scope` (sysfs, since `8c2abe4089def`) set to `cpu` is the
  first thing to try, as a client setting and with Mike's word.
- **The client's two ports are on different nodes**, port a's address
  on node 0 (CPUs 0-23) and port b's on node 1 (24-47), so
  `xprt_numa_map` homes port a's mounts on node 0 and port b's on
  node 1.

## The server side: `xeu-rig/settled-iops.sh`

`settled.sh`'s setting with David's server values held over it:
768 nfsd threads (512 settled), `rmem_max` and `wmem_max` 256 MiB, and
`tcp_rmem` / `tcp_wmem` "4096 1048576 268435456".  Its `run` also sets
his load:

- each filesystem mounted from its own server address (`SRV_ALIAS=1`),
  at nconnect 16, so 128 connections;
- one fio job for each client CPU (24 a port) at iodepth 32, on
  io_uring with registered buffers.

Already settled and as he has it: `max_block_size` 1 MiB, direct I/O
for nfsd's READs and WRITEs, `svc_tcp_rx_region_reads` N, per-CPU rtc
pools (`89a3792597d7e`, his 16 and 17), each connection on the
least-loaded CPU apart from the rings (`svc_rtc_balance` Y,
`fe25e474fa3de`, his 22 and 23).

Not taken from his instructions:

- **The async export:** the export stays sync, the mode that acts like
  enterprise storage (AGENTS.md).  A READ does not wait on it.
- **MTU 9001:** the ports run 7792.
- **His default IRQ placement with no RPS or XPS:** this would be an
  A/B of its own against `settled.sh`'s layout.

On the server the 4 KiB load is cheap: t2-r880 was 41% busy, nfsd 16%
of samples, with the scheduler's switch (7%) and softirqs (5%) its
largest self costs.

## Next

1. **Boot the client into the canary-WIP tip** (Mike is building it).
   After the boot, read only:
   - `uname -r`;
   - `cat /sys/devices/virtual/workqueue/rpciod/per_cpu`, which should
     read 1;
   - `cat /sys/module/sunrpc/parameters/xprt_numa_map`, which should
     read Y;
   - `ss -tni` during a run, to see 16 connections a mount.
2. **The same three runs at iodepth 32** under `settled-iops.sh`
   (`FLAMEGRAPH/108-4k-randread/nfs4k.sh`), set against t2-r875 to
   r877, then `prof4k.sh` again to see what the client waits on next.
3. **A/Bs, one at a time:**
   - one rpc_clnt at nconnect 128 (`SRV_ALIAS=0 NCONN=128`) against
     eight at 16, which is what patch 27 is for;
   - libaio against io_uring, for the inline first leg;
   - xprtiod `affinity_scope` cpu, if the profile points there;
   - his IRQ placement.
4. Each round's README records its WRITE pass and READ pass, as the hard
   rules in AGENTS.md ask of any change to a path they take.  A
   client-only change takes no server path, but say so in the README.
