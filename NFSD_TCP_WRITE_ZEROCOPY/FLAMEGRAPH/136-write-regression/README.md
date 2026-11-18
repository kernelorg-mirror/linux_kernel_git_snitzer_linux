# Round 136: the WRITE pass from 77.8 to 75 GB/s

Mike, 18 November: "I think next round should be to revisit the WRITE
drop from 78 to 75 GB/s.  That is a big red flag that cannot go
unanswered.  When the current queued work is completed, I can reboot
the client into the previous kernel that enjoyed 78 GB/s and then we
can have the fun of figuring out why the latest tip regresses WRITE
from 78 to 75 GB/s".  `WRITE.md`, TODO 1, has the client commits
between build 469 (77.73 to 77.91, rounds 106 and 107) and build 541
(74.85 to 75.21) and the order of attack.

The server runs the tip's own svc stack since 17:24 (round 135, "After").

1. **Before the reboot:** `write-check.sh 1076` on build 541's own
   client stack against that server: the figure the reboot is set
   against.
2. **On build 469 (Mike reboots the client):** the boot's checks (both
   ports, `settled.sh audit`, reported as found), two WRITE passes: if
   469 still writes about 77.8 against today's server the regression is
   the client's alone.  Then a WRITE pass with perf on both hosts (data
   in /dev/shm, `-N`), the server sockets' Send-Q and the client's
   per-transport queues.
3. **Back on build 541:** the same profile beside it; the A/Bs with no
   rebuild (`CLI_SCOPE`, `PIN=0`, nconnect 12 and 24); then the client
   commits out one at a time with cbuild/cswap (the splice
   `1b9af79d9f16f` first, then the per-CPU transport map
   `5555163d661a0`, then rpciod per-CPU `ccaaf769083fe`).

## Before the reboot (18 November, 17:25)

| Run | Client | Server | WRITE GB/s | Result |
|---|---|---|---|---|
| t2-r1070 (round 134) | build 541, round 132's P stack | round 129's stack | 75.21 | PASS |
| **t2-r1076** | **build 541's own stack** | **the tip's stack** | **74.60** | PASS, at the mark; no drive stepped down, no new kernel warning |
| t2-r858, r868 to r871 (rounds 106, 107) | build 469 | build 534's stack then | 77.73 to 77.91 | |

The client has no NFS mounted and is ready for Mike's reboot into
build 469.

## The before profile and trace (Mike: "have you collected perf data
from both the client and server while running the tip on the server?
as the comprehensive 'before' profile and trace data")

`prof-write.sh 1077`: one WRITE pass with perf on both hosts (499 Hz,
5 s, data in each host's /dev/shm, `-N`), mpstat on both, the client's
memory controllers (round 107's shape), `ss -tni` on both hosts' ports
and the client's per-transport RPC queues three times in the load, and
nstat deltas on both.  The same script runs on build 469 after the
reboot.

### t2-r1077: build 541's client, the tip's server (17:51 to 17:54)

PASS, 74.79 GB/s (37.14 / 37.65), no drive stepped down, no new kernel
warning; fio's completion 57 ms mean, 156 ms p99.  Raw perf data:
`/dev/shm/zc-p136-1077.data` (server), `/dev/shm/zc-cli136-1077.data`
(client).

- **CPU** (mpstat, seconds 4 to 9, iowait as idle): client 43% / 42%
  (node 0 / node 1), server 25% (work CPUs 8-55 27%).  Neither host is
  CPU-bound.
- **Client memory**: 158.9 GB/s in the 5 s, 2.13 bytes a byte written
  (round 107 on build 469: 2.17).
- **Client profile**: `cpu_idle_poll` 56.6% (idle=poll); fio 15.7% of
  samples, `get_io_u` alone 7.25% (fio's own); then
  `__list_del_entry_valid_or_report` 3.23%, `nfs_page_create` 1.48%,
  `nfs_scan_commit_list` 1.35%, `nfs_clear_request` 1.14%,
  `try_grab_folio_fast` 1.10%, spin lock 1.0 + slowpath 0.83%,
  `mutex_spin_on_owner` 0.82%.  rpciod and xprtiod workers each under
  0.5%.
- **Server profile**: idle 51%, nfsd 47%; softirq 5.6%, the ring's
  receive (`xeu_rx_poll` 1.9%), the loan's table 0.8%.
- **Sockets** (`ss -tni`, seconds 6, 9, 12): the client's 32 sockets
  hold 50 to 53 MB unsent (1.6 MB each), cwnd 41 to 47 segments of 7792
  bytes (about 350 KB); the server's hold nothing unsent and 4.5 to 5.2
  MB unread.  Client bytes retransmitted from second 6 to 12: 1.19 MB.
  The server's rings dropped 85 and 68 frames in the 10 s window.
- **The client's RPC queues** (`xprt_info`): 32 TCP transports, 16 a
  port; 4,042 to 4,398 RPCs waiting to send (up to 315 on one
  transport), 147 to 161 sent and waiting for a reply, none in the
  backlog: about 5 in flight a transport, 130 queued behind them.
- nstat: this run's files have the counters since boot (`nstat -a`, a
  mistake); `prof-write.sh` now writes the deltas since second 4.

## Build 469's client (Mike rebooted it at about 18:28)

The one-shot entry was set with `grub2-reboot`; this grub saves the
entry it boots, so `saved_entry` is now 469 too, and goes back to 541
with `grub2-set-default` when the round wants it.

- `uname -r` 7.3.0-0.rc7.2.hs.469.el9.x86_64, the same command line as
  541's; no NFS mounted; both ports answer (0.049 and 0.039 ms).
- **The two client patches** (Mike: "469 should have the 2
  patches/client/patches applied via updates/ please verify"): sunrpc
  is loaded from `updates/sunrpc.ko`, srcversion
  `AB674CEE87351D814F68CEE`, the build in the client's
  `/root/zc-csunrpc/C`, whose source has both: 0001's `WQ_SYSFS` on
  rpciod and xprtiod (`sched.c` 1333, 1337; both in
  `/sys/devices/virtual/workqueue/`) and 0002's splice (`socklib.c`'s
  splice flag through `xprt_send_pagedata()`, `xprtsock.c` 1146 setting
  `MSG_SPLICE_PAGES`).
- **The client audit**: two BAD, both what build 469 is: the kernel,
  and `rpciod/per_cpu` 0 (per-CPU rpciod, `ccaaf769083fe`, is one of
  the commits it lacks).  `settled.sh` takes `EXP_CLI_WQ` now, as it
  took `EXP_CLI_KERNEL`, and the runs on 469 are held with
  `EXP_CLI_KERNEL=7.3.0-0.rc7.2.hs.469.el9.x86_64
  EXP_CLI_WQ="rpciod/per_cpu=0"`: with them the audit reads the
  settled setting.

Then `write-check.sh 1078` and `prof-write.sh 1079`.

### Build 469 against build 541, the tip's server unchanged (18:45 to 18:50)

| | t2-r1077, build 541 (before) | t2-r1078, build 469 | t2-r1079, build 469 (profiled) |
|---|---|---|---|
| WRITE pass | 74.79 (37.14 / 37.65) | **77.69** (38.93 / 38.77) | **77.70** |
| fio completion mean / p99 (ms) | 57.0 / 156 | 54.5 / 83 | |
| server ring drops a / b (10 s) | 85 / 68 | 276 / 273 | |
| client busy (node 0 / 1), server busy | 43 / 42%, 25% | | 46 / 46%, 28% |
| client memory, a byte written | 159.0 GB/s, 2.13 | | 167.9 GB/s, 2.16 |
| client Send-Q, 32 sockets (t6/t9/t12) | 50.0 / 52.9 / 50.2 MB | | 53.0 / 52.2 / 55.6 MB |
| client cwnd min / median / max | 32 / 41 / 57 | | 32 / 35-40 / 55-61 |
| **RPCs waiting to send** (t6/t9/t12) | **4,042 / 4,398 / 4,083** | | **2,825 / 2,944 / 2,973** |
| **most on one transport** | **268 / 315 / 279** | | **97 / 102 / 102** |
| RPCs in flight | 147 to 161 | | 158 to 169 |

Build 469 against today's server writes what it wrote in rounds 106
and 107: **the 3 GB/s is the client's.**  The sockets look the same
on both (the same unsent bytes, the same cwnd), the server is not
busier than 28% on either, and the client's profile is the same list
in the same order (fio's `get_io_u`, the list checks, `nfs_page_create`,
`nfs_scan_commit_list`, pinning, locks) within a few tenths of a
percent.  What differs is **how the WRITEs are spread over the 32
transports** (each transport's `sending_q_len` at second 9, sorted):

- build 541: 0 3 10 32 37 49 67 75 78 81 100 103 106 106 129 129 145
  146 159 163 163 169 170 177 178 184 191 191 229 235 236 242 315
- build 469: 0 0 85 85 86 86 87 87 88 88 88 89 89 89 90 90 90 90 90 90
  92 92 94 94 94 98 98 98 98 99 99 99 100 102

(each list has the gssproxy socket's 0 too).  Build 469 deals the
WRITEs round-robin and every transport holds 85 to 102; build 541's
per-CPU map (`5555163d661a0`, "SUNRPC: give each CPU its own transport
under nconnect") puts each on its submitting CPU's transport, and the
transports hold 0 to 315.  A WRITE behind 315 waits three times as long
as one behind 100: the p99 is 156 ms on 541 against 83 on 469, at
nearly the same mean.  The map spills to round-robin only once a
transport holds 64 tasks and the round-robin choice holds fewer than
half as many, which at 100 to 300 queued a transport rarely holds.

The 469 runs' nstat deltas (second 4 to 14): the client retransmitted
955 segments of 115 million and timed out once; the server none to
speak of.

## Back on build 541: the per-CPU choice A/B (plan)

Mike rebooted the client into build 541 at about 19:11 (default set
back with `grub2-set-default`); sunrpc `225361815EB170B866C5033`, both
ports, the audit at the settled setting without overrides.

`a60960b6a6f99` "DROP: SUNRPC: A/B switch for FLAMEGRAPH round 136":
`sunrpc.xprt_percpu_choice_ab` N makes `xprt_iter_next_entry_percpu()`
take the round-robin transport for every task, as build 469 does; the
rest of the per-CPU map (each transport's home CPU, the reply worker
queued there) is unchanged.  W=1 clean.  The client's stack is built
from it with `cbuild132.sh` into `/root/zc-ckern/T` and loaded with
`cswap131.sh` (client side only), then `ab-choice.sh 1080:Y 1081:N
1082:Y 1083:N 1084:Y:pin0`: each a profiled WRITE pass
(`prof-write.sh`), the last with fio's writers unpinned (`PIN=0`).
The client back on build 541's stack after.

### Result (18 November, 19:16 to 19:33): the choice makes the tail, not the rate

Client stack from `a60960b6a6f99` (sunrpc `A2807F183DF8C0F5B900901`),
the tip's server; each a profiled WRITE pass.

| Run | Choice | WRITE GB/s | p99 a / b (ms) | RPCs waiting to send t6 / t9 / t12 | A transport at t9: min / median / max |
|---|---|---|---|---|---|
| t2-r1079 (build 469) | round-robin | 77.70 | 83 / 84 | 2,825 / 2,944 / 2,973 | 85 / ~90 / 102 |
| t2-r1080 | Y, per-CPU | 74.89 | 152 / 154 | 5,031 / 4,461 / 4,316 | 43 / 133 / 293 |
| t2-r1081 | N, round-robin | 72.49 (RERUN) | 84 / 89 | 2,152 / 1,924 / 1,817 | 0 / 31 / 218 |
| t2-r1082 | Y, per-CPU | 74.87 | 154 / 154 | 4,983 / 4,631 / 5,101 | 21 / 137 / 294 |
| t2-r1083 | N, round-robin | 72.61 (RERUN) | 87 / 84 | 2,008 / 1,899 / 1,418 | 0 / 34 / 187 |
| t2-r1084 | Y, `PIN=0` | 74.05 (RERUN) | 160 / 156 | 4,412 / 4,315 / 4,209 | 35 / 116 / 318 |

No new kernel warning in any; no drive stepped down.

- **The per-CPU choice makes build 541's tail**: round-robin brings its
  p99 to build 469's (84 to 89 ms against 152 to 160) and halves what
  waits to send.
- **It is not the 3 GB/s**: round-robin on 541 writes 72.55, 2.3 less
  than the per-CPU choice's 74.88, where 469's round-robin writes 77.7.
  Fewer WRITEs reach the transports' queues at all, and those queues
  are still uneven (0 to about 200).  How many WRITEs reach the sockets
  and where they wait comes from the rest of build 541's path, which
  build 469 has none of: rpciod per-CPU (`ccaaf769083fe`), the first
  RPC leg in fio's own context (`50eee8803afc5`), the hand from sending
  to pending without a requeue (`ba5642ee4cac2`).
- **Unpinned writers** (`PIN=0`) change nothing (74.05).

The client is back on build 541's stack.  The DROP switch stays on the
branch while this round goes on; it is rebased out when the round ends.

## rpciod per-CPU or unbound (plan)

Mike: "yes, start with the rpciod per-CPU switch, I would like to
preserve these behind a modparam in case they are critical to IOPS or
READ perf".  `298c732504785` "SUNRPC: Let rpciod run unbound again
with rpciod_per_cpu=N", a code commit: `sunrpc.rpciod_per_cpu`, load
time, Y (per-CPU, as `ccaaf769083fe` made it) by default; N allocates
rpciod `WQ_UNBOUND`, as build 469 has it.  W=1 clean.  The client's
stack is built from the tip (the choice switch at Y in it), and
`ab-rpciod.sh 1085:Y 1086:N 1087:Y 1088:N` loads it with or without
`options sunrpc rpciod_per_cpu=N` for each run (twoport.sh holds an
unbound rpciod `affinity_strict` to its node, as on build 469), each a
profiled WRITE pass.  The client back on build 541's stack after.

## Build 469's NFS stack on build 541's kernel (plan)

Mike: "yes, build 469's NFS stack against 541 after run 4".  Half the
question in one step: if build 541's kernel with build 469's NFS modules
writes about 77.7, the regression is in the NFS modules (and bisectable
with cbuild and cswap, no reboot); if about 74.9, in the core kernel
between 7.3.0-rc7 and 7.3.3.

The source, made with git plumbing (no checkout): the tree of tag
`v7.3.0-0.rc7.2.hs.469.el9` with `patches/client/0001` and `0002`
applied (`git apply --cached` into a scratch index: tree
`fb325d7547c50701c9c44df92a30e0a1f776b22f`), its `net/sunrpc`,
`fs/nfs_common`, `fs/lockd`, `fs/nfs` and their headers
(`include/linux/sunrpc`, `lockd`, `nfs*`, the sunrpc and nfs trace
headers).  `cbuild136.sh` builds it on the client against build 541's
kernel-devel with those headers laid over it, into a module root
`/root/zc-ckern/H469` for `cswap131.sh`.  Its sunrpc's srcversion
should be build 469's (`AB674CEE87351D814F68CEE`).  Then two WRITE
passes with `prof-write.sh`, held with `EXP_CLI_WQ="rpciod/per_cpu=0"`
(469's rpciod is unbound) and without the per-CPU map's module
parameter (469 has none).

**First build (19:55): fs/nfs did not compile** against 7.3.3's
headers: 36 errors, all one change, the VFS's inode operations taking
`const struct mnt_idmap *` in 7.3.3 where 7.3.0-rc7's `fs/nfs` has
`struct mnt_idmap *` (namespace.c 4, nfs3proc.c 13, nfs4proc.c 17,
symlink.c 2).  sunrpc, lockd and nfs_common built.  Adapted for the
build only, in the client's copy, with no change in behaviour: every
`struct mnt_idmap *` not already const in `fs/nfs/*.[ch]` and
`include/linux/nfs*.h` becomes `const struct mnt_idmap *`.

### Result (18 November, 19:55 to 20:01): the regression is in the NFS client modules

With every `mnt_idmap` const, 469's stack built: 17 modules, sunrpc
srcversion `AB674CEE87351D814F68CEE`, build 469's own; nfs
`FEF25C30FDC0440C289BE11`.  Loaded on build 541's kernel with
`cswap131.sh` (client side), rpciod unbound as on 469; the runs held
with `EXP_CLI_WQ="rpciod/per_cpu=0"`.

| Run | Kernel | NFS client modules | WRITE GB/s | p99 a / b (ms) | RPCs waiting to send t6 / t9 / t12 | A transport at t9: min / median / max |
|---|---|---|---|---|---|---|
| t2-r1079 | build 469 | build 469's | 77.70 | 83 / 84 | 2,825 / 2,944 / 2,973 | 85 / ~90 / 102 |
| t2-r1080, r1082, r1085, r1087 | build 541 | the tip's | 74.50 to 74.89 | 152 to 163 | about 4,000 to 5,200 | about 30 / 130 / 300 |
| **t2-r1089** | **build 541** | **build 469's** | **77.81** | **81 / 81** | **3,005 / 3,086 / 3,203** | **89 / 95 / 107** |
| **t2-r1090** | **build 541** | **build 469's** | **77.79** | **82 / 83** | **3,018 / 3,008 / —** | **87 / 93 / 112** |

Both PASS, no drive stepped down, no new kernel warning.  **Build 541's
kernel writes 77.8 with build 469's NFS client modules**: the core
kernel between 7.3.0-rc7 and 7.3.3 (TCP, mm, block, the scheduler) is
cleared, and the whole 3 GB/s is in what sunrpc and nfs changed
between build 469 and the tip.  The per-CPU transport choice and rpciod
per-CPU, each taken out alone above, are not it; another commit, or
several together, is.  The client is back on build 541's stack.

## Bisect, step 1: the tip's base with the two client patches (plan)

Mike: "yes, go ahead with step 1 but do apply the 2 patches/client/
commits that were needed for scaling".  On the branch those two are
the first client commits of all, `8c2abe4089def` (WQ_SYSFS) and
`1b9af79d9f16f` (the splice), and every canary client commit (David
and Jonathan Flynn's eleven in `WRITE.md`) comes after them: the tree
of `1b9af79d9f16f` is the tip's own base with the two patches and no
other client change.  Its `net/sunrpc`, `fs/nfs_common`, `fs/lockd`,
`fs/nfs` and their headers, built with `cbuild136.sh` into
`/root/zc-ckern/S1`, loaded on build 541's kernel; two profiled WRITE
passes, rpciod unbound there (held with `EXP_CLI_WQ`).  About 77.7: the
regression is in the commits after it; about 74.9: it came with the
base.

## The bisect (plan)

Mike: "yes, bisect the commits after 1b9af79".  The canary client
commits came in through merges, so a first-parent list misses them:
`git bisect start --no-checkout 4c8aa21d29987 1b9af79d9f16f` over the
client's paths (`net/sunrpc`, `fs/nfs`, `fs/nfs_common`, `fs/lockd`,
`include/linux/sunrpc`, `nfs_fs.h`, `nfs_xdr.h`, `nfs_page.h`,
`nfs_fs_sb.h`): 365 candidates, about 8 steps, no checkout.
`bisect136.sh 1093` runs it: each step one profiled WRITE pass on
`BISECT_HEAD`'s client stack (`bstep.sh`), good at 76.5 GB/s or more,
bad at 75.8 or less, a second pass between; a tree that does not
build is skipped.

### The bisect's first steps (20:23 to 20:31), then reversals on 541's tree

| Step | Commit | Run | WRITE GB/s | Verdict |
|---|---|---|---|---|
| 1 | `e8f1a72bb9b25` | t2-r1093 | 77.91 | good |
| 2 | `5e543e08a2506` | t2-r1094 | 73.19 | bad |
| 3 | `4ae3b1029cbde` | | | skipped: does not build |
| 4 | `7661739461aab` | | | skipped: does not build |
| 5 | `ae24cc81bcd3e` | (t2-r1095 aborted when the bisect was stopped, in its trim) | | |

The good step already holds `32faf45611a7d`, `d398df1554edd` and
`bcd79ae606aa8`: they are cleared.  From `4ae3b1029cbde` ("net:
rx_loan: move the receive page-loan machinery out of sunrpc") the
middle trees' `svcsock.c` calls an older `rx_loan_admit()`,
`rx_loan_grant_return()` and `rx_loan_region_put()` than build 541's
kernel has, and does not compile; the bisect was stopped (its state is
kept).  The window, read: 13 commits touch the client's own files, of
which the server's receive and send (`4b593427d660b`, `fadc8f0ee0291`,
`aa4297a0999a7`), LOCALIO (`990e699092959`, `f49804fcf59e7`), the
NOWAIT write (`334ea04bd3718`, io_uring NOWAIT only) and the two
already switched off (`ccaaf769083fe`, `5555163d661a0`) cannot be it;
left are the first RPC leg in the caller (`50eee8803afc5`), the hand
from sending to pending (`ba5642ee4cac2`), nconnect up to 128
(`7661739461aab`), the Hammerspace private extensions
(`f1fc2b1c481cd`) and the KCSAN annotations (`d1ea6a2aeac37`).

So from the other end: build 541's own tree (`4c8aa21d29987`, 74.9)
with one suspect reversed, in a scratch index (`git apply -R
--cached`, no checkout), each built against 541's kernel with
`bstep.sh` and one profiled WRITE pass.  `50eee8803afc5` (tree
`d2d378fde1bb1`), `ba5642ee4cac2` (`1a15470c5ff65`) and
`d1ea6a2aeac37` (`0ff4ac893e92c`) reverse cleanly; `7661739461aab`
and `f1fc2b1c481cd` do not, and wait.  First the two: t2-r1096 without
the first leg in the caller, t2-r1097 without the hand to pending.

### The two reversals (20:32 to 20:40), and every per-CPU placement off at once (plan)

| Run | Client stack (on 541's kernel) | WRITE GB/s | p99 a / b (ms) | A transport at t9: min / median / max |
|---|---|---|---|---|
| t2-r1097 | 541 without `ba5642ee4cac2` (send to pending) | 74.75 | 156 / 152 | 35 / 121 / 313 |
| t2-r1096 | 541 without `50eee8803afc5` (first leg in the caller; the whole commit, its trace header too, tree `74f50237a77b6`: the first try, `d2d378fde1bb1`, left `RPC_TASK_INLINE_OK` in the trace header and did not build) | 74.49 (RERUN) | 158 / 150 | 6 / 103 / 274 |

Neither alone.  Of the suspects left, `7661739461aab` only raises
`NFS_MAX_CONNECTIONS` from 16 to 128 (nothing sizes by it at nconnect
16), and `f1fc2b1c481cd` is NFSv4 (idmapping, `nfs4file.c`,
`auth_name`) and an `rpc_completion` API NFSv3 does not call, and does
not touch the O_DIRECT WRITE path (`direct.c`, `write.c`,
`pagelist.c`).  Left: the per-CPU commits together.  Each keeps a WRITE
on the CPU that submitted it (the per-CPU transport choice, rpciod
per-CPU, the first leg in the caller, the hand to pending), so with any
one taken out the others still hold the work where it was.  The test:
the tip's tree (`298c732504785`, the choice switch and
`rpciod_per_cpu` in it) with `50eee8803afc5` and `ba5642ee4cac2`
reversed (tree `fa0c06559fe5b`), loaded with `rpciod_per_cpu=N` and
run with `xprt_percpu_choice_ab=N` (`bstep.sh`'s new `CLI_PRE`): every
per-CPU placement off.  Two passes, t2-r1098 and t2-r1099.

### Every per-CPU placement off (20:40 to 20:47), then forward from the good step

t2-r1098, the tip's tree with `50eee8803afc5` and `ba5642ee4cac2`
reversed, `rpciod_per_cpu=N`, `xprt_percpu_choice_ab=N` (read back
N/N/0): **73.40 GB/s (RERUN), p99 82 / 85 ms**: build 469's tail, not
its rate.  The rest of the window was read again with every path the
client modules build from: `06e3c456182e7` only adds an skb helper to
a header, `2d908b0e2dc60` is the server's READ confinement, and
`d1ea6a2aeac37` on the client is one `READ_ONCE` in `nfs3proc.c`, a
`WRITE_ONCE` and two `data_race()`.  A reversal on 541's tree also
carries every later commit (the sharded task list, the NUMA homing,
the IOPS rounds), so it cannot isolate the window.

Mike: "yes, do the forward stages from e8f1a72".  From the good step's
tree, in a scratch index, each of the window's commits in topological
order applied for the client's files only (`fs/nfs`, `fs/nfs_common`,
`fs/lockd`, `net/sunrpc` and its headers without `svc*`, the nfs
headers, the sunrpc trace headers, no KUnit):

- **stage B** (tree `9e95d70f92314`): every commit but the four per-CPU
  ones; `4ae3b1029cbde` (the rx_loan move) does not apply and is left
  out, the rest do;
- **stage A** (tree `532c58c675a6f`): B with `5555163d661a0`,
  `ba5642ee4cac2`, `50eee8803afc5` and `ccaaf769083fe`.

One pass each, t2-r1100 (B) and t2-r1101 (A).  B good and A bad: the
four together, in this tree; B bad: one of the others.

t2-r1099, the second pass with every per-CPU placement off: **72.43
GB/s (RERUN), p99 83 / 85 ms**; a transport at t9 held 0 to 195,
median 22, about 1,500 in all (t2-r1098 alike), half the good tree's
2,800 to 3,300: fewer WRITEs reach the transports at all.

The forward stages did not build: `aa4297a0999a7` ("sunrpc:
flat-combine svc TCP reply sends") has `xprtsock.c`'s backchannel send
call `svc_tcp_send()`, which is in `svcsock.c`, left out with the
server's files.  The backchannel is NFSv4.1's; an NFSv3 WRITE does not
take it.  So `aa4297a0999a7` is left out too: stage B is tree
`34843b6eccb7d`, stage A `eda3d37b69d08`, t2-r1100 and t2-r1101.

Stage B without `aa4297a0999a7` (`34843b6eccb7d`) did not build
either: `eaaa6f347dc3a` puts `svc_pool_kunit.o` in `net/sunrpc/Makefile`
without its source, which the filter leaves out.  So the window's KUnit
commits (`eaaa6f347dc3a`, `d51f7b8ba5462`, `9f25eee7b0367`) are left
out whole, and `fbcdf8dd44cbf` (the server's upcall cache) then does
not apply on the Makefile and is left out too.  Stage B is tree
`31ab8c4904ec7`: `e8f1a72bb9b25` with `f49804fcf59e7`, `d1ea6a2aeac37`,
`f1fc2b1c481cd`, `990e699092959`, `334ea04bd3718`, `7661739461aab`,
`fadc8f0ee0291` and `4b593427d660b`; stage A `3a085ef0f1dd1`, B with
the four per-CPU commits.  t2-r1100 and t2-r1101.

### The forward stages (20:49 to 20:56)

| Run | Client stack (on 541's kernel) | WRITE GB/s | p99 a / b (ms) | A transport at t9: min / median / max (sum) |
|---|---|---|---|---|
| t2-r1100 | B, `31ab8c4904ec7` | 77.61 | 83 / 83 | 92 / 98 / 104 (3136) |
| t2-r1101 | A, `3a085ef0f1dd1`: B + the four per-CPU commits | 73.03 (RERUN) | 163 / 169 | 0 / 124 / 260 (3316) |

The four per-CPU commits together are the window's regression, with
541's rate and 541's tail.  The other eight are cleared.  Next, B with
each of the four alone, one pass each: t2-r1102 `5555163d661a0` (tree
`71a3b2fe007ff`), t2-r1103 `ba5642ee4cac2` (`4663a8f8ea627`), t2-r1104
`50eee8803afc5` (`a1d55250943a8`), t2-r1105 `ccaaf769083fe`
(`ada5c03fe2459`).

### Each of the four alone on B (20:56 to 21:11): the per-CPU choice

| Run | Client stack (on 541's kernel) | WRITE GB/s | p99 a / b (ms) | A transport at t9: min / median / max (sum) |
|---|---|---|---|---|
| t2-r1100 | B | 77.61 | 83 / 83 | 92 / 98 / 104 (3136) |
| t2-r1102 | B + `5555163d661a0` (the per-CPU choice) | 72.83 (RERUN) | 165 / 169 | 0 / 90 / 303 (3321) |
| t2-r1103 | B + `ba5642ee4cac2` (sending to pending) | 77.88 | 81 / 81 | 83 / 89 / 95 (2822) |
| t2-r1104 | B + `50eee8803afc5` (first leg in the caller) | 77.59 | 84 / 82 | 82 / 92 / 99 (2898) |
| t2-r1105 | B + `ccaaf769083fe` (rpciod per-CPU) | 77.85 | 81 / 83 | 88 / 94 / 104 (3006) |

**`5555163d661a0`, "SUNRPC: give each CPU its own transport under
nconnect", is the regression, whole.**  The other three cost nothing.

Why its A/B switch (`xprt_percpu_choice_ab=N`, t2-r1081 and r1083:
72.49 and 72.61) never brought the rate back: the commit does three
things, and the switch turns off only the first.

1. The per-CPU choice: a CPU's own transport, spilling to round-robin
   only once it holds 64 tasks and the round-robin pick fewer than half
   as many.
2. It deletes round-robin's queue-length balancing,
   `xprt_switch_find_next_entry_roundrobin()`'s skip of any transport
   holding more than the average (`xprt_queuelen * nactive <=
   xps_queuelen`), with `xps_queuelen`.  That is what held build 469's
   transports even, 82 to 104 at t9.
3. It queues a transport's receive work on its home CPU
   (`xprt_queue_work()` in `xs_data_ready()` and
   `xs_poll_check_readable()`).

With the switch at N, a task takes plain round-robin with no balancing
(2): the tail comes back to 84 to 89 ms, but transports run empty while
others hold 200 (t2-r1098, every per-CPU placement off: 0 / 22 / 200,
about 1,500 in all) and the rate is lower still.  With it at Y (1), the
transports stay full but 0 to 300 apart, and a WRITE waits behind the
deepest: p99 150 to 170 ms.

### A balanced per-CPU choice, tried and dropped (21:18 to 21:30)

Mike: "yes, write the fix and run the passes".  The change tried, built
on the client at W=1 without warnings and run from a commit since
dropped (Mike: "I don't want code churn in the commit history for
false-starts"): a CPU's choice compared its own transport with one other
active transport, taken in turn from a per-CPU cursor over the switch's
active transports (kept in the CPU map), and sent the task to the other
when its own held more than `xprt_percpu_slack` (16) tasks beyond it, in
place of the 64-task spill; and the home CPU's receive work behind a
switch, `xs_recv_on_home_cpu` (default Y).  `r136fix.sh` ran it.

| Run | Client stack (on 541's kernel) | WRITE GB/s | Mean / p99 (ms) | Client CPUs busy, mean | A transport at t9: min / median / max (sum) |
|---|---|---|---|---|---|
| t2-r1100 | B (round-robin, balanced) | 77.61 | 54.5 / 83 | 49% | 92 / 98 / 104 (3136) |
| t2-r1102 | B + the per-CPU choice | 72.83 (RERUN) | 57.9 / 165 | 63% | 0 / 90 / 303 (3321) |
| t2-r1106 | the tip + the balanced choice, receive on the home CPU | 70.38 (RERUN) | 60.4 / 169 | 40% | 67 / 101 / 141 (3268) |
| t2-r1107 | the same, receive where it arrives | 70.48 (RERUN) | / 154 | 41% | 49 / 101 / 131 (3133) |

What was found:

- **Even transports are not enough.**  The transports held within
  about 2x of each other instead of 0 to 300, and the rate fell
  further, with the same tail.
- **Where the receive work runs does not matter to WRITEs** (70.38 Y,
  70.48 N).
- **The client's CPUs are not the limit**: 40 to 63% busy in every
  run, the good one included.
- **Not a task transmitting on a transport homed on another CPU**: the
  per-CPU choice alone (t2-r1102), where a task almost always transmits
  on its own CPU's transport, lost as much.  The balanced choice adds
  such transmits and may cost a little more (70.4 against 72.8).
- **What fits every run: the pooling of round-robin.**  The pass runs
  16 fio jobs a port, each pinned to a CPU (`cpus_allowed` 0-23 for
  port a), each with 256 WRITEs of 512 KiB in flight (8 MiB at
  iodepth 16).  Round-robin deals every job's WRITEs over all 16
  connections, so each carries an equal share of every job.  The
  per-CPU choice puts a job's WRITEs in its CPU's connection, and the
  map gives a NUMA node's CPUs their share of the connections: a
  connection with two jobs doubles their completion time (the p99 from
  83 to 165 ms) while others run short.  Round-robin without its
  balancing (the DROP switch at N, t2-r1081 and r1083: 72.5) loses the
  rate too, with the tail back.

The READ pass with the balanced choice (t2-r1108, receive Y) ran before
the chain was stopped; the IOPS runs (r1109, r1110) and the READ pass
with receive N (`r136fix-readn.sh`, r1111) did not run.

Next (Mike: "yes, revert the fix and write the policy modparam"): the
transport policy of an nconnect switch as a sunrpc parameter, the
per-CPU choice or round-robin with its queue-length balancing restored
as it was before `5555163d661a0`; WRITE, READ and IOPS on each, and the
default set to what measures best.

The READ pass with the balanced choice, t2-r1108: **80.96 GB/s, PASS**.
The chain was stopped as its first IOPS run set up (t2-r1109, kept as
`t2-r1109.aborted`); nothing was left running or mounted.

### The policy parameter (plan)

`e42020cc5e247`, "SUNRPC: Let nconnect use the balanced round-robin
with nconnect_percpu=N" (W=1 clean): sunrpc's `nconnect_percpu`
(default Y, fixed at load).  N gives an nconnect switch the
round-robin policy again with its queue-length balancing and the
switch's `xps_queuelen` (counted only under N), as before
`5555163d661a0`; no transport then has a home CPU, so the receive work
is queued where the data arrives.  `ab-policy.sh`: N then Y, each with
the WRITE pass, the READ pass and 4 KiB IOPS at iodepth 96 (t2-r1112
to r1117).

### The policy parameter (21:32 to 21:57): the per-CPU choice never helps

`2879915a28581` (`e42020cc5e247` before its `Fixes:` tag), the client
on the tip's stack, sunrpc `ADFE789DC77EE680D7DF331`:

| | N: balanced round-robin | Y: per-CPU choice |
|---|---|---|
| WRITE pass | **77.75 GB/s**, p99 83 / 83 ms (t2-r1112) | 74.69, p99 154 / 156 (t2-r1115) |
| a transport at t9 | 91 / 99 / 110 (3182) | 37 / 133 / 310 (4733) |
| READ pass | **82.09 GB/s** (t2-r1113) | 79.27 (t2-r1116) |
| 4 KiB IOPS, iodepth 96 | 3.52 M, p99 3.36 / 3.29 ms (t2-r1114) | 3.53 M, p99 3.59 / 4.08 (t2-r1117) |
| CPU a READ, server / client | 16.00 / 11.73 us | 15.41 / 11.29 us |

All PASS, no new kernel warning; the client on build 541's stack and
the settled setting after.  N writes 3.1 GB/s more with half the tail,
reads 2.8 more, and gives the same IOPS; Y saves about half a
microsecond of CPU a READ on each host and buys nothing with it at
iodepth 96.  At N nothing of `5555163d661a0` or of the two homing
commits on it (`a37330054f3ca`, `994e5b01c627c`) runs on the I/O path:
no CPU map is built, no transport has a home CPU, and the homing only
keeps its books when a transport is created.  Mike's to decide how
they go: dropped from the history, one commit at the tip, or at the
next regroup with N the default until then.

### The per-CPU choice reverted (plan)

Mike: "revert each from the tip in commit order, do so in a way that
lends itself to git rebase dropping the reverts and the original commit
at next regroup's rebase".  Six reverts, newest first, each with git's
own "This reverts commit <full SHA>." to pair it with what it reverts:
`31f6d1abad006` (the policy parameter, `2879915a28581`; its subject cut
with "..." to 72 columns), `1f9c3cb5ed1c4` (the DROP switch,
`a60960b6a6f99`), `f920de8058d5a` (`994e5b01c627c`), `f8c8784ce8381`
(`6ddeaa4d160e6`, "xprtrdma: interrupt a transport's completions on its
home core", which calls the homing's `rpc_xprt_numa_comp_vector()`: the
first build without it failed, and it was put back in its place),
`e47f942327151` (`a37330054f3ca`) and `7313a2551b565`
(`5555163d661a0`).  No conflicts; `xprtmultipath.c`,
`xprtmultipath.h` and `xprt.h` are those before `5555163d661a0` byte
for byte.  The tips before are `kernel-7.3/canary.pre-percpu-revert`
and `.pre-rdma-revert`.  `rev136.sh`: the WRITE pass,
the READ pass and IOPS on it (t2-r1118 to r1120).

### The per-CPU choice reverted (22:05 to 22:23): every pass at its best

The client on the tip's stack with the six reverts (sunrpc
`2355B960B6A9951899AF6B4`; W=1 clean, `xprtrdma/verbs.c` included):

| Run | Test | Result | Beside |
|---|---|---|---|
| t2-r1118 | WRITE pass | **78.00 GB/s PASS**, p99 80 / 83 ms, a transport at t9 89 / 95 / 102 | build 469 77.70; N 77.75; per-CPU 74.5 to 74.9 |
| t2-r1119 | READ pass | 77.84 PASS: port a 40.58, port b 37.25, port b's drives 9.3 GB/s each at 100% busy | N 82.09, drives 10.1 to 10.4 |
| t2-r1120 | 4 KiB IOPS, iodepth 96 | **3.63 M** (1.81 / 1.81), p99 3.26 / 3.16 ms, 15.34 / 11.64 us of CPU a READ | N 3.52 M; per-CPU 3.48 to 3.57 |
| t2-r1121 | READ pass again (Mike: "yes, run a second READ pass after IOPS") | **82.24 GB/s PASS** | |

No new kernel warning; the client on build 541's stack and the settled
setting after.  t2-r1119's shortfall was port b's drives (saturated and
reading less, both ports' code the same); the second pass reads as N
did.  **Round 136 is answered: the WRITE regression from 78 to 75
GB/s was `5555163d661a0`, "SUNRPC: give each CPU its own transport
under nconnect", which replaced the balanced round-robin over the
nconnect transports; reverted, with the two homing commits, xprtrdma's
completions on the home core and round 136's switch and parameter,
WRITE is back at 78.00, READ at 82.24 and IOPS at 3.63 M.**  At the
next regroup each revert and the commit it names are dropped together.
The WRITE mark can go back to 77.6 (`WRITE.md`, TODO 2): Mike's word.

### The reverted stack in the client's updates/

Mike: "yes, put the reverted stack in the client's updates/".  The 17
modules of `/root/zc-ckern/REV` (built from `9d2c9fcede7b6`, sunrpc
`2355B960B6A9951899AF6B4`) are in the client's
`/lib/modules/7.3.3-16.hs.541.canary.el9.x86_64/updates/`, which was
empty, with a `COMMIT` and a `README` saying how to go back (remove
them, `depmod -a`); `depmod -a` run, and `cswap131.sh stock` loads them
(sunrpc read back `2355B960B6A9951899AF6B4`).  The rpm's nfsd does not
load on this sunrpc ("disagrees about version of symbol"): nfsd is not
in the build, and the client's `nfs-server` was not running before.
The WRITE and READ marks are 77.6 since (`6db25b6d56984`).

The passes with the client on `updates/` at the 77.6 marks (Mike: "run
a WRITE and READ pass with the client on updates/"):

| Run | Pass | Result |
|---|---|---|
| t2-r1122 | WRITE | **77.87 GB/s PASS**, no drive stepped down, no new kernel warning |
| t2-r1123 | READ | **82.38 GB/s PASS** (41.31 / 41.06 a port, every drive 10.2 to 10.4 GB/s), no new kernel warning |

The settled setting after.

4 KiB random READs at iodepth 96 with the client on `updates/` (Mike:
"run the 4K IOPS test with the client on updates/"): t2-r1124 **3.54 M
IOPS** (1.75 / 1.79 M a port), p99 4.01 / 3.49 ms, 15.37 us of the
server's CPU and 11.80 us of the client's a READ, no new kernel
warning; t2-r1120 on the same stack read 3.63 M, the tip's per-CPU
stack 3.48 to 3.57 M.

Again (Mike: "run the 4K IOPS test again"): t2-r1125 **3.51 M IOPS**
(1.76 / 1.75 M), p99 3.33 / 3.42 ms, 16.05 / 11.90 us of CPU a READ,
no new kernel warning.  The same modules as t2-r1120 byte for byte, so
3.51 to 3.63 M is the spread from run to run on this stack.
