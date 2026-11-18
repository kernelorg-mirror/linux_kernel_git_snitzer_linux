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

## The client on the canary-WIP tip: 1.81 M IOPS (17 November, evening)

The client booted build 541 (`7.3.3-16.hs.541.canary.el9.x86_64`, the
canary-WIP tip, every commit of the tables above in it, and both of
`patches/client/`).  Same server, same load (`settled-iops.sh`, iodepth
32, 1,536 READs in flight):

| | build 469 (t2-r875 to r877) | build 541 (t2-r883 to r887) |
|---|---|---|
| M IOPS, median / min | 1.05 / 1.05 | **1.82 / 1.81** (1.81 to 1.86) |
| completion mean / p99 | 1.46 / 4.2 ms | 0.82 to 0.87 / 2.9 to 3.0 ms |
| client busy (busiest CPU) | 94 to 95% (96 to 97%) | 61 to 63% (68 to 75%) |
| client cycles in spin-lock slowpath | 63.5% | 13.4% |
| server busy (busiest CPU) | 48 to 49% (63 to 66%) | 54 to 56% (87 to 92%) |

**Neither end is saturated now.**  The rate is 1,536 in flight over
0.84 ms: what decides it is how long a READ takes, round trip, not
any one CPU being full.  Where the time goes (t2-r886 server, t2-r887
client, 8 s at 999 Hz on both):

- **Server, 56% busy, nfsd's work CPUs (8-55) 66% on average, two of
  them over 85%.**  READs run in rtc work items (`svc_rtc_work` 70% of
  all samples, idle included), not in nfsd threads (`nfsd` 2.5%).  Of
  it: `nfsd3_proc_read` 18.9% (XFS and the drive 9.8%), the TCP send
  11.4%, the receive 8.0%, and **the per-request checks 10.4%**:
  `fh_verify` 6.9% (`nfsd_set_fh_dentry` 5.9%, `rqst_exp_find` 2.6%)
  and `svc_set_client` 3.5% (`svcauth_unix_set_client` 3.3%), with
  `try_module_get`/`module_put` 1.4% each and `sunrpc_cache_find_rcu`
  1.6% in their self costs.  That is the cost David's 18 and 19
  (borrowing the export) take away, and it is in neither this tree nor
  build 534.
- **Client, 63% busy.**  Spin-lock slowpath 13.4%: `queue_work_on`
  10.1% and the worker's own dequeue, mostly xprtiod's two unbound
  pools (`kworker/u213`, `u214`), which `twoport.sh` holds at
  `affinity_scope` numa for the run (`CLI_SCOPE`; build 541's own
  default is cache_shard).  rpciod (per-CPU) is 23.7% with its
  children and contends no more.

## Round 109: what to chase next

In order, each an A/B under `settled-iops.sh` at iodepth 32, three
runs a side, with t2-r883 to r887 the base:

1. **The server's `pool_mode`, `auto` against `pernode`: done**
   (`FLAMEGRAPH/109-iops/README.md`, t2-r888 to r893).  percpu, what
   auto picks, read 1.93 M IOPS median against pernode's 1.89, 2% more
   and every percpu run above every pernode run.  Mike: pernode stays
   settled for the passes, and **`settled-iops.sh` holds percpu** ("for
   settled-iops.sh settle percpu").
2. **Depth: iodepth 16 and 64** on the new client, to see whether the
   rate now follows the number in flight (latency-bound) or holds (a
   limit somewhere).
3. **The server's spread: 128 connections on 48 work CPUs** (2.67 a
   CPU, two CPUs over 85%).  nconnect 12 (96, two a CPU) and 18 (144,
   three) against 16.
4. **The client's xprtiod**: `CLI_SCOPE=cache_shard` (build 541's
   default) against numa, then xprtiod per-CPU if the lock stays.
   **Done (t2-r909 to r914): cache_shard 2.39 M IOPS three times
   against numa's 2.31 to 2.32, the client 68% busy against 77%, p99
   0.8 ms longer; `settled-iops.sh` holds cache_shard since (Mike).**
5. **Port David's 18 and 19** (an nfsd thread borrows the export, no
   shared reference a request) if the server becomes the limit: 10% of
   the server's samples are the checks it shortens.

## The server side: `xeu-rig/settled-iops.sh`

`settled.sh`'s setting with David's server values held over it:
768 nfsd threads (512 settled), `rmem_max` and `wmem_max` 256 MiB,
`tcp_rmem` / `tcp_wmem` "4096 1048576 268435456", and since round 109
`pool_mode` percpu (pernode settled), 64 pools of 12 threads.  Its `run` also sets
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
   - **the server's `sunrpc.pool_mode`, `auto` against `pernode`**
     (Mike, 17 November).  David's instructions leave it at its default,
     `auto`, and in his tree `auto` always means per-CPU pools
     (`2464b0d42380a`, "SUNRPC: per-cpu svc pools, sibling wake and
     steal, adaptive idle spin"): his 192-CPU server ran 192 pools.
     `settled.sh` holds `pernode` since 6 November (Mike: per-CPU pools
     cost WRITEs on builds 512 and 514, about 43k nfsd cycles a loaned
     WRITE, copy mode 7% slower), which on tardis2's one NUMA node is
     one pool, 768 threads under `settled-iops.sh`.  The A/B is
     `EXP_POOL_MODE=auto` under `settled-iops.sh` (64 pools of 12),
     which `settled.sh` applies by restarting nfs-server.  It is for
     IOPS: the settled `pernode` changes only with a WRITE pass and
     Mike's word.  The client's `pool_mode` plays no part (its RPCs use
     no svc pool);
   - his IRQ placement.
4. Each round's README records its WRITE pass and READ pass, as the hard
   rules in AGENTS.md ask of any change to a path they take.  A
   client-only change takes no server path, but say so in the README.

## Handoff (17 November, 19:20): where IOPS stand

**Since round 109 the IOPS runs are at iodepth 64 and nconnect 12, now
`settled-iops.sh`'s defaults (Mike, 17 November: "that should be
reflected in settled-iops.sh settings!"); the commands below that name
`:32` are from before it.  Give `nfs4k.sh` the run number alone.**

**Read this first on the next session.**  tardis2 is on the boot that
ran round 106 (build 534 with the promise stack in `updates/`), kdump
armed, at the settled setting (`settled.sh audit rest`: "the settled
setting"), nothing running.  The client is still build 469
(`7.3.0-0.rc7.2.hs.469.el9.x86_64`, up 36 days); **Mike is building the
canary-WIP tip for it**, and that boot is the next step.  The server
needs no change for it (the new code is all the client's).

What was done on 17 November, after round 107:

- Round 108 (`FLAMEGRAPH/108-4k-randread/README.md`): the drives read
  18.78 M IOPS locally; NFS 0.98 M on the settled setting (t2-r872 to
  r874), **1.05 M on David Flynn's setting** (`xeu-rig/settled-iops.sh`,
  t2-r875 to r879), the client 94 to 100% busy and the server about
  half idle.
- t2-r880's profile: the client spends 63.5% of its cycles on the
  workqueue pool lock of rpciod and xprtiod ("What limits the client
  now", above).  The commits that remove it are checked present at the
  canary-WIP tip ("The client commits that remove the contention").
- Each drive has two more 512 MiB files, `f-<port>.4` and `.5`, for
  `settled-iops.sh`'s 24 jobs a port; the fill marker is unchanged.
- `settled.sh` took `EXP_NFSD_THREADS` and `EXP_SYSCTL`; unset they
  change nothing.
- **The x86_64 build of canary-WIP failed** in
  `net/core/rx_loan_kunit.c` (a `__percpu` pointer, `__seg_gs` on x86,
  given to `KUNIT_ASSERT_NOT_NULL()`).  The fix is `bf421b073b621`, at
  this branch's tip and not on canary-WIP: the client's build needs it,
  or `CONFIG_RX_LOAN_KUNIT_TEST=n`.  It is compiled on arm64 only; a
  later x86-only error in the same build is possible.
- David's instructions are committed beside this file
  (`nfsv3-max-iops-agent-instructions-20261117.md`).

When the client is up on the canary-WIP tip, in this order:

1. The checks in "Next", step 1 (read only), and ping both ports.
2. Three runs at iodepth 32 under `settled-iops.sh`, numbered from
   t2-r881:

       cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r108 iops-wip ./settled-iops.sh run \
           ../FLAMEGRAPH/108-4k-randread/nfs4k.sh 881:32 882:32 883:32

   then `prof4k.sh 884` for what the client waits on next.
3. The A/Bs of "Next", step 3, one at a time.  The round's README
   records each, and the client's kernel beside every result.

Still open: the client's 2.17 bytes of memory a byte on WRITEs (round
107) is unexplained; the EFI partition's FAT dirty bit (nvme8n1p1)
waits for a `fsck.fat` at the next maintenance.  The claude.ai artifact
"NFSD Read Promised Count" is round 106's READ story and is not used
for IOPS (Mike, 17 November): IOPS progress is not offered for it.

## Boot of 17 November, 22:43: both hosts on build 541

The session on build 534 (boot of 20:04) hung at 22:15 on the root
drive, not the rig: `nvme8` (the 64 GB Micron that holds `/` and the
EFI partition) timed out two 256 KiB WRITEs (22:14:50, 22:15:51)
while `boot.mount` was unmounting, and writeback waited behind it
(kworker 187 s, umount 149 s).  No oops, no dump (none saved).  A
boot into 534 at 22:30 installed 541 and was reset at 22:40.

tardis2 now runs `7.3.3-16.hs.541.canary.el10.aarch64+16k`, the
client `7.3.3-16.hs.541.canary.el9.x86_64`:

- kdump failed at boot: `/var/lib/kdump/boot/` held a symlink for
  541, which SELinux denies `kdump_t`.  Replaced by a copy;
  `kexec_crash_loaded` reads 1.
- 541 carries the promised count: `5562f5e440e7f`, `6e2ea298a2dc3`
  and `49f022fca9378` are ancestors of `4c8aa21d29987`, and the
  running sunrpc has `svc_rtc_has_wspace`, `svc_rtc_sk_full` and
  `svc_rtc_unpark`.  `updates/` is empty, as it should be.  The code
  at the tip not in 541 is the export borrow (`dcffec4ac50c1`,
  `e5df1b4f4106b`, `d7f9f04a9ded7`) and its KUnit commits.
- Both ports ping both ways; `bringup.sh 32768 256`, then
  `settled.sh apply`: "the settled setting", no BAD item.
- The client's checks of "Next", step 1: rpciod `per_cpu` 1,
  `xprt_numa_map` Y, no NFS mounts.

Still open: `nvme8`'s timeouts (the FAT dirty bit on `nvme8n1p1`
again, at 22:30 and 22:33).

The tip's stack for 541 (Mike, 17 November, 23:00): `build-svcstack.sh`
from `9a2433060ea3a` into `/var/tmp/svcstack-541-9a2433060ea3a/stack`,
against `4c8aa21d29987` (541's source; only
`include/linux/sunrpc/svcauth.h` differs in `include/`).  Every
module's imports match 541's exports and the stack's, none at another
version; sunrpc keeps all 446 of 541's exports at their versions.
srcversions: sunrpc `7A2D473DDE10AB1E01D4BB3`, nfsd
`10AE6D2903DB88D99FB4A23`, lockd `E80CADD7B3A4CF927760A78`.  Next, with
no NFS connection up: `swap-sunrpc.sh` with `STACK=` that set and
`SUNRPC_KO=none` through `step.sh`, then `settled.sh apply`.

The root drive's hang left `refs/remotes/origin/kernel-7.3/canary-WIP`
empty (written 22:14), with `FETCH_HEAD` and `MERGE_RR`; the ref is
removed, and the pack and every loose object written since 20:00
check out.

Swapped at 23:05 (`/var/tmp/zc-run-logs/541/steps.log`): sunrpc
`7A2D473DDE10AB1E01D4BB3` and nfsd `10AE6D2903DB88D99FB4A23` run from
541's `updates/` (all eight modules of the set).  nfsd came back on 64
per-CPU pools; `settled.sh apply` put it back on `pernode`, 512
threads on 8-55, and ends in "the settled setting", no BAD item, no new
kernel warning, kdump armed.  No WRITE or READ pass has run on it yet.

The passes on it (client build 541): WRITE t2-r915 74.91 GB/s (RERUN),
t2-r917 **75.25, PASS** at the mark of 75.0 (WRITE.md, TODO 2), no
drive stepped down, no new kernel warning; READ t2-r916 **79.65 GB/s,
PASS** (io_uring with registered buffers, rsize 256 KiB), no new
kernel warning, the drives 9.7 to 10.1 GB/s each at 99 to 100% busy.
Against the frame: the drives read 104.58 GB/s with local fio and the
ports send 98.31 with iperf3 `-Z`, so NFS reads at 76% of the drives
and 81% of the wire; the pass measures no memory or Send-Q.

IOPS on the tip's stack at the handoff's iodepth 32 and nconnect 16,
`settled-iops.sh` run: t2-r918 2.02 M, t2-r919 2.01 M (client 59%
busy, server 56 to 57%, busiest 94 to 95%).  Those were the script's
defaults, not round 109's 64 and 12, which it had only on its command
lines; the defaults are 64 and 12 since then, and `nfs4k.sh` takes them
when a run gives only its number.  Export borrow over t2-r918: 28.0 M
hits, 26.6 M fills (51% hits): one slot a thread against eight exports.

## Rounds 110 to 112 (18 November): 2.54 M IOPS, the server the limit

Both hosts on build 541, the server on the tip's stack, `settled-iops.sh`
(iodepth 64, nconnect 12, 48 fio jobs, 96 connections):

| Round | Change | M IOPS | What it showed |
|---|---|---|---|
| 110 | the export borrow, several entries a thread (`export_borrow_slots` 16) | 2.47 to 2.48 | hits from 24 to 61% to 99.98%; 2% server CPU, no rate |
| 111 | the AUTH_SYS borrow (`svcauth_borrow` Y) | **2.54 to 2.55**, against 2.47 to 2.49 off | +2.4%, the server's busiest CPU from 100% to 92%; both borrows held by `settled-iops.sh` since |
| 112 | the client's scope at `cpu` (`CLI_SCOPE`) | 2.54 to 2.55, against cache_shard's 2.52 to 2.54 | the same rate, 3 points less client CPU, p99 0.1 to 0.3 ms longer; `settled-iops.sh` holds `cpu` since (Mike, 18 November) |

Round 112's profile (t2-r965) found the client's spin locks (about 10%
of its samples) on six unbound workqueue pools: nfsiod runs the release
of every finished READ (`rpc_async_release`, one item a READ), and
shares its pools with xprtiod.  At `cpu` that lock is gone, and in every
run of the A/B the server's busiest work CPU reads 95 to 98%: the server
is the limit now.

### TODO

1. **The server's per-READ cost**, the limit now: profile the server at
   2.54 M with both borrows (round 111's t2-r963 is the last: softirq,
   the reply's send, `nfsd_file_lookup_cached` 7.8%, `fh_verify` 6.7%).
2. **nfsiod on a per-CPU workqueue** (`WQ_PERCPU`, as build 541 made
   rpciod) on the client (Mike, 18 November: "add IOPS.md todo to
   implement nfsiod with percpu wq"): each READ's release queued on the
   CPU that ran its task, on that CPU's own pool, strictly, where scope
   `cpu` only prefers it.  Expected, while the server is the limit: the
   same rate for less client CPU, as round 112's A/B.  `nfsiod` is made
   in `fs/nfs/inode.c` (`alloc_workqueue("nfsiod", WQ_MEM_RECLAIM |
   WQ_UNBOUND, 0)`); with it per-CPU, `CLI_SCOPE` would only steer
   xprtiod.  A client-only change: it takes no server path, so it owes
   no WRITE or READ pass, but its README says so.
3. **The WRITE pass back to 77.6** once the client's WRITE path on
   build 541 is fixed (`WRITE.md`, TODO 2).

## MAYBE TODO

Ideas Mike may take up later; none is planned.

1. **Cached attributes in the file handle borrow** (Mike, 18 November:
   "save 'implement the cached attributes in the fh borrow' as a MAYBE
   TODO").  Every NFSv3 READ reply encodes post-op attributes, about
   0.64 us of the server's 18.3 a READ (FLAMEGRAPH round 120,
   t2-r1015): `vfs_getattr_nosec` and `xfs_vn_getattr` 30%,
   `security_inode_getattr` (SELinux) 22%, the ID mapping 11%.  Round
   121 took only what changes nothing a client sees (no direct I/O
   alignment for a reply's attributes, no ID map lookup in the initial
   user namespace), about 0.15 to 0.2 us.  The rest: keep a `kstat` in
   each fh borrow entry, filled by the full `vfs_getattr()` when the
   entry is filled (the LSM check and XFS's own fields included), and
   on a hit reuse it while the inode's ctime and size are as they were,
   taking atime fresh from the inode.  It would skip most of the 0.64 us
   on the 99.8% of READs that hit.  What it costs: the LSM check runs at
   the entry's fill, not on every reply, so a policy change takes effect
   within `fh_borrow_ttl` (2 s); and each entry grows by a `kstat`
   (about 150 bytes, 48 entries a thread).
