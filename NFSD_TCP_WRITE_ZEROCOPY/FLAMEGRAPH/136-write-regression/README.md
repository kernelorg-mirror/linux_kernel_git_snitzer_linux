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
