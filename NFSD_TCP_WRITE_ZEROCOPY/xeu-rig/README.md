# 15 October: the boot into the reworked tree — read this first

Written late on 14 October, after Mike pushed
`kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY` at `32cf5556bd2e` (459
commits on `v7.3-3`) and started a pipeline build of it, for the
session after he reboots tardis1 into that build. Where it differs
from the sections after it, it is right.

## What the kernel is

The series reworked for upstream that night: the code group reviewed,
folded, split and given upstream messages, 38 code commits, then 44
KUnit interface and test commits, then the docs. The account is steps
3 to 5 of `../CODE-SERIES.md`; `AGENTS.md`'s branch entry has the
summary. Build 473 was built from the tree before it. The code differs
from 473's in six places only (`git diff
kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-code.pre-fixes
kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-code.73rebase8`, 6 files):

- **TCP receive page loans are off by default**
  (`svc_tcp_rx_loan_pages`, Y on 473). `settled.sh` sets Y and its
  audit holds the rig to Y, so the rig loans as before; a kernel booted
  without it applied copies every record.
- An NFSv4 COMPOUND with a tag keeps its loan until the reply is
  encoded (the tag is copied from the received pages then).
- The reply cache checksums a call's first bytes from a buffer on the
  stack, not from the reply buffer.
- `pc_xdr_bvec` is a bool, `svc_proc_lookup()` is exported only for
  KUnit, an unused include is gone, and the KUnit-only
  `svc_tcp_rx_fixed_factor()` lives in the KUnit interface.
- `svc_tcp_rx_copy_unlocked` was already gone (see "Copy mode on 7.3"):
  records are copied under the socket lock unless
  `svc_tcp_rx_copy_threads` is 1 or more. On one port the two carried
  the same on 473 (45.81 GB/s locked, 45.40 unlocked, six runs
  alternating on seven drives, `$L/b473/ab4.log`); two ports were not
  settled.

**The branch has moved on since that build** (later on 14 October,
`.73rebase9`): two calls into TCP now go through TCP's own interfaces,
and that build has neither.

- The region budget clamps a held transport's window with a new
  `tcp_sock_set_window_clamp()` (added in the tcp commit beside
  `tcp_sock_set_scaling_ratio_max()`) instead of the `TCP_WINDOW_CLAMP`
  socket option. The same `tcp_set_window_clamp()` under the same lock,
  so no change in behaviour; but it is built into vmlinux, so it needs
  the next full kernel build to run.
- `svc_tcp_rx_lowat` sets the mark through the socket's
  `->set_rcvlowat` (TCP's `tcp_set_rcvlowat()`, as `SO_RCVLOWAT` does)
  instead of writing `sk_rcvlowat`, and leaves TLS transports alone by
  `XPT_TLS_SESSION`. Only with `svc_tcp_rx_lowat` set does it change
  anything: TCP caps the mark at half the receive buffer and wakes the
  transport at once if enough is queued.

Recognise it: `uname -r` names a build after 473, and
`modinfo -p sunrpc | grep loan_pages` says "(default: N)".

## First, in this order

1. `getconf PAGESIZE` is 16384. Mount the data drives (not in
   `/etc/fstab`): `for n in 0 1 2 3 4 5 6 7; do mount -o noatime
   /dev/nvme${n}n1 /hs/nvme$n; done`; `./drive-cache.sh` shows `write
   through` on all eight.
2. `./settled.sh apply` ends in "the settled setting"; never get past a
   BAD item by hand. Then `cat
   /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages` is `Y`.
3. **The four KUnit suites, never yet run on the reworked tree**:
   `../run-kunit.sh` with nfs-server stopped (it puts the server and
   the settled setting back and fails if the host is not at it). It
   needs the suites installed for this kernel (the RPM's
   modules-internal package). On 473 they ran 7, 46, 6 and 10 cases
   with no failure and no skip; the tests did not change in the rework,
   so the same is expected. Both the tip with KUnit and without it were
   built clean with `CONFIG_WERROR=y` on 14 October
   (`/var/tmp/zc-run-logs/walk-73rebase/kbuild-tip-result.txt`), but
   nothing was run.
4. **No regression against build 473**, at the setting and with the
   commands under "Where it stands" and "The loan on 7.3":
   two ports (`RCVBUF=393216 ENTRIES=128 LINGER=0 LOWAT=0 ./twoport.sh
   <out> ab`; 473: 59.27 and 59.36), one port on eight drives (473:
   45.98 to 47.76), and the three loan plans through `sweep.sh` (473's
   table below). Same code as 473 but for the six changes, so the same
   figures are expected.
5. **The `->set_rcvlowat` change, without a reboot**: confined to
   sunrpc, so `./build-sunrpc.sh` builds `sunrpc.ko` from the branch
   against this kernel's `kernel-devel` and `./swap-sunrpc.sh` puts it
   in (nfsd stopped, mounts down: Mike's to do, as for the locked and
   unlocked comparison on 14 October). Build it with the window clamp
   call put back to the socket option (a local edit, not committed),
   or the module will not load: this kernel has no
   `tcp_sock_set_window_clamp()`. Compare with this kernel's own sunrpc
   with `svc_tcp_rx_lowat=131072 svc_tcp_rx_keep=1`, the setting that
   halved thread wakeups on 13 October.

**The drives first.** On 14 October they were tired: after 13 minutes
idle nvme2 stepped down at 341 GB (a fresh drive takes about 625) and
nvme3 had been stepping down first in every run; the one-port
comparison that night ran on seven drives without nvme3 (`FS_A` of
the other seven). After a reboot they have rested; still read
`drives.txt` of every run, and keep runs short (the comparisons that
night used `WARM=10 RUNTIME=30 REST=120`, and nvme3 still stepped down
once at 34 s on two ports).

## Still Mike's to decide

Dropping the first three code commits (the stacking device's DMA
alignment, several thread pools per NUMA node, veth's `rx_ip_align`)
and the last ("SUNRPC: Optionally copy TCP record data outside the
socket lock"; the case is in `../CODE-SERIES.md`, step 4); the
page_pool region's future upstream; Ronaldo's author address on his
twelve commits. Do not act on these unasked.

## Left over from 14 October

- The walk and the builds ran in worktrees on nvme3, removed again
  that night: nvme3 holds only the harness's `zcbudget` and was
  trimmed. What is kept of them is in
  `/var/tmp/zc-run-logs/walk-73rebase/`: `walk.sh` (to walk again, give
  it a tree and its paths), its configuration, and every walk's
  summary (`run15-73rebase9.summary.txt` the last, 38 of 38 clean,
  and `kbuild-tip-result.txt` for the tip with and without KUnit).
- The rework's working worktree was in a session scratch directory
  under `/tmp`; if a reboot removed it, `git worktree prune`. Its
  branches stay: `…-code.73rebase1` to `.73rebase9` are the stages
  worth keeping; `…-code` and the `…-code.pre-*`, `.restructure*`,
  `.splits*`, `.msgs`, `.comments`, `.tcpif`, `.bvecmove-1`,
  `-reorder-1*`, `-reorder-2` and `-tcpif` branches are working copies
  Mike may delete.
- `sparse` v0.6.5-rc1 from git master is in `/usr/local/bin` (EPEL's
  0.6.4 fails the kernel's checker probe).

# 13 October, evening

Written at the end of 13 October on tardis1 (build 464, 16 KiB
pages) as the handoff, brought up to date late that night after the
loan measurements and the loan page, and on 14 October for the
rebase onto `v7.3-3`. Everything below this section is dated
history and says so; where it differs from this section, this
section is right.

## Where it stands

Copy mode, NFS WRITEs from the one client, durable, every run inside
the drives' fast region:

| | best run | runs of the setting | setting |
|---|---|---|---|
| two ports | **60.41 GB/s** | 59.96 to 60.41 in seven | `RCVBUF=393216 ENTRIES=128 LINGER=0 LOWAT=0 ./twoport.sh <out> ab` |
| one port, on all eight drives | **47.39 GB/s** | 46.5 to 47.4 in four | `FS_A="/hs/nvme0 ... /hs/nvme7" RUNTIME=60 LOWAT=0 ./twoport.sh <out> a` (`FS_B=`, `b` for the other) |

- Two ports: 384 KiB held buffer, four rings of 128 a port, no
  linger. One port: the values in `/etc/modprobe.d/sunrpc.conf` (1 MiB,
  linger 50) and four rings of 256. Both: `svc_tcp_rx_keep=1`,
  `svc_tcp_rx_lowat=0`, 8192-byte receive buffers at an MTU of 7792,
  512 nfsd threads on CPUs 8-55, the client untouched.
- **The setting, the CPU layout included, is `settled.sh`'s, and
  every run is audited against it (Mike, 14 October: "lock it down
  hard").** nfsd on CPUs 8-55 and nothing of the ports' there; port
  a's Rx-i on CPU 2i and Tx-i on 2i+1, port b's on 56+2i and 57+2i,
  each port's async interrupt with its Tx-0 (1 and 57); transmit
  steering (XPS) port a's tx-i to CPU 8+i and port b's to 32+i, the
  map every figure here was measured with; no RPS, NAPI not threaded,
  irqbalance kept off nfsd's CPUs and the card. `./settled.sh audit
  rest` checks it all, and the module settings, live; `./settled.sh
  apply` puts it back. `twoport.sh`, `sweep.sh` (each cell, under
  load), `iperf2port.sh` and `memcost-cal.sh` refuse to run off it,
  and `run-kunit.sh` restores it. Its header says what undoes each
  part and why XPS is held where it is.
- **!!! `irq_tuning.service` and `rps_xps_tuning.service` are MASKED
  (Mike, 14 October) and are TO BE RECONCILED with this setting. !!!**
  Hammerspace's E1 profile puts the rings on nfsd's CPUs (port a on
  8-31, port b on 32-55) with its own XPS map, and re-ran on every
  reload of `xeu`. Kept, with how to put them back, in
  `/root/hs-tuning-units-masked-2026-10-14/`; the card's MaxReadReq
  of 512, which `irq_tuning` set, `settled.sh` sets now. See
  `AGENTS.md`.
- 61.02 GB/s was carried once on two ports with `fq` on the client,
  which is no longer part of the setting and is worth nothing
  measurable.
- **The limit on two ports is memory.** The DIMMs are fully
  populated (Mike: four at 5200 MT/s, 166 GB/s in theory). A copy
  sends each byte through memory twice: the measured limit is a bare
  benchmark's 62 to 68 GB/s, 60.1 in frame-sized pieces. NFS on two
  ports is at that: 2.17 bytes through memory for each byte of
  payload, about 131 GB/s of the DIMMs' 166 at 60.4.
  Nothing that adds parallelism around the copy moves it.
- One port is at 95% of what its wire carries with no NFS (49.8).

## Copy mode on 7.3: build 473 (14 October)

The battery's regression check (step 3 under "Next"), on build 473
(`7.3.0-0.rc7.3.hs.473.loanpages_pool.el9.aarch64+16k`, the series on
`v7.3-3`), 12:41 to 13:04 by tardis1's clock. Each run at the setting
under "Where it stands", audited by `settled.sh` before it changed
anything and again just before its load, the client on `fq_codel` as
for build 464's comparison runs, and `memcost-log.sh` beside each. No
drive stepped down in any of them. The runs are `$L/b473-<tag>`, their
lines in `$L/b473/results-twoport-473.tsv`, the memory samples
`$L/b473/copymem-<tag>`.

| | build 473 | GB/s | bytes through memory for each byte of payload | build 464 |
|---|---|---|---|---|
| two ports | `ab-1`, `ab-2` | **59.27, 59.36** (ports 28.2 + 31.1, 28.7 + 30.7) | 2.25, 2.23 | 59.96 to 60.41 in eight runs on `fq_codel`; 2.17 (`cmn-two-1`, 60.38) |
| two ports | `ab-3` | 47.45, port b silent from second 25 (the eighth event, `../SILENT-PORT.md`) | | |
| one port a, eight drives | `a8-1` to `-3` | 46.77, 45.98, 47.59 | 1.44, 1.42, 1.46 | 46.5 to 47.4 (both ports' runs) |
| one port b, eight drives | `b8-1` to `-3` | **47.76**, 47.54, 47.06 | 1.48, 1.47, 1.46 | the same |

- **One port shows no regression on 7.3.** Its six runs (45.98 to
  47.76) cover 464's range (46.5 to 47.4), and `b8-1`'s 47.76 GB/s,
  at 1.48 bytes through memory for each byte, is the best one-port
  run on either build. It is the newest entry on the region budget
  page.
- **Two ports are about 1.5% under 464**, 59.27 and 59.36 against a
  mean of 60.24, with about 3% more memory traffic for each byte (2.25
  and 2.23 against 2.17). Two runs; not yet looked into. The figures
  divide the mesh's requests by the run's whole payload, as 464's 2.17
  does: `memcost-read.py` reads one port's payload, so its figure for
  a two-port run is that port's, and halving it is right only when the
  ports are even.
- What differs from 464's runs besides the kernel: the async
  interrupts on CPUs 1 and 57 (on 464 they sat on nfsd's 8 and 9),
  which fired no times, and nothing else `settled.sh` holds.
- **These runs copied with the socket unlocked**
  (`svc_tcp_rx_copy_unlocked=Y`, the default on 464 and 473). The knob
  is gone from the code after build 473 (Mike, 14 October; "SUNRPC:
  Copy with the socket unlocked only when records are claimed"): a
  kernel built from the tip copies under the lock unless
  `svc_tcp_rx_copy_threads` is 1 or more. On 464 the two carried the
  same (43.6 and 43.5 against 43.6, one port), so the next build's
  figures should match these; `settled.sh` reports the knob as not in
  that kernel rather than failing on it.

## The loan on 7.3: build 473 (14 October)

The battery's step 4 on build 473: `plans/16k-42-loan-memcost.plan`,
`16k-43-loan-memcost-small.plan` and `16k-44-loan-memcost-floor.plan`
through `sweep.sh` on the stand-in, as written out in
`../LOAN-PERFORMANCE.md`, 13:04 to 14:03 by tardis1's clock, with
`CELL_BOUNCE=1` for all three (on build 464 for the third only, so the
first two plans' footprints are not compared). Every cell was audited
once its load was running, against its plan line, and passed. The runs
are `$L/xeu-16k-473-loanmem-1` to `-3`, the memory samples
`$L/loanmem-473-1` to `-3` and their readings `$L/loanmem-473-<n>.read.txt`.

| rings, held buffer | WRITEs let in | build 473: GB/s | bytes through memory for each byte of payload | ring drops | build 464: GB/s, bytes, drops |
|---|---|---|---|---|---|
| 256, 1 MiB | 16 | 11.74 | 4.01 | 0 | 11.74, 4.07, 359 |
| 256, 1 MiB | 8 | 11.72 | 2.99 | 0 | 11.74, 2.99, 269 |
| 256, 1 MiB | 4 | 10.65 | 2.37 | 0 | 10.59, 2.28, 297 |
| 256, 1 MiB | copy | 10.44 | 5.97 | 0 | 10.35, 5.75, 6,203 |
| 128, 384 KiB | 8 | 11.73 | 2.14 | 1 | 11.73, 2.00, 646 |
| 128, 384 KiB | 4 | 8.45 | 1.11 | 0 | 8.40, 0.93, 207 |
| 128, 384 KiB | copy | 10.49 | 5.11 | 5 | 10.41, 5.02, 17,966 |
| 128, 256 KiB | 8 | 11.66, 11.70 | 1.86, 1.78 | 0, 0 | 11.72, 1.86, 203; 11.71, 1.89, 221 |
| 128, 256 KiB | 6 | 9.90 | 1.42 | 0 | 10.28, 1.39, 162 |
| 128, 256 KiB | 4 | 7.44 | 1.01 | 0 | 7.45, 0.80, 120 |
| 128, 256 KiB | copy | 10.41 | 4.96 | 0 | 10.41, 4.90, 171 |
| 128, 192 KiB | 8 | 11.50 | 1.72 | 0 | 11.55, 1.70, 154 |
| 128, 128 KiB | 8 | 10.74 | 1.68 | 0 | 10.81, 1.62, 121 |
| 64, 256 KiB | 8 | 11.63 | 1.42 | 13,509 | 11.60, 1.36, 64,803 |
| 64, 192 KiB | 8 | 11.46 | 1.38 | 3,216 | 11.43, 1.34, 19,809 |
| 64, 128 KiB | 8 | 10.48 | 1.24 | 1 | 10.74, 1.32, 7,463 |
| 64, 128 KiB | 6 | 8.24 | 0.79 | 0 | 8.12, 0.77, 865 |
| 64, 128 KiB | copy | 10.48 | 4.72 | 0 | 10.47, 4.71, 7,898 |

- **The loan on 7.3 is build 464's, cell for cell**: the same rates,
  its cost in memory falling with what it holds to a floor of 1.24
  (1.32 on 464), and its copy at 4.7 to 6.0 whatever it holds. The
  edges are where they were: fewer than 8 WRITEs let in costs rate, a
  128 KiB held buffer about 8%, and rings of 64 with a larger held
  buffer drop frames by the thousand. The best setting that loses
  nothing is still rings of 128, a 192 to 256 KiB held buffer and 8
  WRITEs let in, at 1.7 to 1.9.
- **Every loaned WRITE was written direct**, 9.70 million in the 15
  loan cells (`wr_direct`, none buffered or `wr_dontcache`), **and none
  held more than it was charged** (`overrun` 0).
- **Fewer frames dropped at the rings** in most cells than on 464,
  none over 5 at rings of 128. One run of each cell on each build; not
  looked into.
- On the loan page, "NFSD Loan Performance", from 14 October.

## Reads and mixes: the 7.1.13 baseline (build 464, 14 October)

Taken before the boot into 7.3 so that it can be laid beside it:
`./rwbase.sh <build>` runs the same loads (about 25 minutes a round,
two rounds), each through `twoport.sh`, whose `RW=`, `RWMIX=` and
`RWTSV=` are new for it. A run that reads first writes its files
whole (16 GiB a port) and rests 20 s. nfsd serves READs direct
(`io_cache_read` 2), so they come from the drives, not the server's
page cache. 16 writers a port at a queue of 16, 512 MiB files.
Both rounds' lines are in `results-rwbase-464.tsv`; each run is
`$L/rwbase-464-saved/rwbase-<tag>-<round>`, except round 1's 8 MiB
READ, 1 MiB random READ and 4 KiB random READ, which build 473's
first round overwrote (their lines in the table and the `.tsv` are
all that is left of them). `rwbase.sh` now names each run
`$L/rwbase-<build>-<tag>-<round>`.

**A restart of nfs-server puts nfsd on all 64 CPUs.**
`hammerspace-nfsd-tuning` puts nfsd's threads on CPUs 8-55 at boot,
and a stop and start of nfs-server (`run-kunit.sh` needs one) brings
up 512 threads on every CPU, among the ports' ring CPUs. `systemctl
restart hammerspace-nfsd-tuning` puts them back, starting nfs-server
if it is stopped; `run-kunit.sh` now does that itself, and
`settled.sh` refuses a run until it is so.

Two ports at the two-port setting, four drives a port:

| load | round 1 | round 2 | of which READ / WRITE | completion, mean |
|---|---|---|---|---|
| READ, 8 MiB sequential | 52.72 GB/s | 53.71 | | 81 ms |
| READ, 1 MiB random | 49.08 | 38.61, port a silent | | 13 ms |
| READ, 4 KiB random | 1.16 M IOPS (4.76 GB/s) | 1.16 M (4.75) | | 0.44 ms |
| 70/30, 8 MiB sequential | 55.68 | 56.03 | 38.9 / 16.8 and 39.2 / 16.9 | 80 / 89 ms |
| 70/30, 1 MiB random | 54.72 | 54.69 | 38.3 / 16.4 both | 9.5 / 10.6 ms |
| 70/30, 4 KiB random | 944 k IOPS (3.87 GB/s) | 951 k (3.89) | 661 k / 283 k and 666 k / 285 k | 0.49 / 0.67 ms |
| WRITE, 8 MiB sequential | 54.90 | 57.99 | | 83 ms |

One port over all eight drives, at the one-port setting:

| load | round 1 | round 2 | of which READ / WRITE |
|---|---|---|---|
| READ, 8 MiB sequential | 37.51 GB/s | 36.90 | |
| 70/30, 8 MiB sequential | 31.59 | 36.38 | 22.1 / 9.5 and 25.4 / 10.9 |

- **Large reads are not limited by the drives.** On two ports each
  drive reads 6.5 to 6.8 GB/s at 61 to 67% busy. On one port it is
  4.6 at 50%, and the port sends 36.8 GB/s, well short of the 47.4
  it takes in WRITEs. Not looked into.
- **4 KiB is limited by nfsd's CPUs**: CPUs 8-55 are 97 to 98% busy
  in every 4 KiB run. The drives read 0.6 GB/s each and show 99%
  busy, but for an NVMe drive that only means something was always
  outstanding; the script's "DRIVE-LIMITED" mark there is not the
  limit.
- **The two WRITE runs** (54.90 and 57.99) are below the 60.0 to
  60.4 of 13 October's seven runs at the same setting. They ran
  after the mixes, with the usual 30 s of rest; every drive stayed
  inside its fast region and none stepped down. Not looked into.
- **The one-port 70/30 mix varies** by 15% between its two rounds,
  for no reason seen in the drives (46 to 50% busy).
- **Port a went silent** in round 2's 1 MiB random READs, from 22 s
  into what fio counted to the end: the 1 MiB figure to lay beside
  is round 1's. This is the sixth event, the first on a read load
  and the first with the client on `fq_codel`; see
  `../SILENT-PORT.md`.
- The mixes drop a few frames at the card (up to 84 at the FIFO and
  128 at the rings in a window) and the client retransmits 100 to
  360 segments in one; the READ-only loads drop none.

## Next: the same battery on the v7.3-3 rebase (Mike, 13 October)

**The series is rebased onto `v7.3-3`** (`kernel-7.3/main`, after
7.3-rc7), on the branch `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`,
done on 14 October, and tardis1 booted build 473 from it that
morning (`7.3.0-0.rc7.3.hs.473.loanpages_pool.el9.aarch64+16k`).
Its KUnit suites pass: 7, 46, 6 and 10, the same as on 7.1.13.

**Every run on build 473 so far is discarded and is to be done again
(Mike, 14 October)**, kept in `$L/b473-discarded/` with a README.
None of it was at the settled setting:
- the KUnit run restarted nfs-server, which put nfsd's 512 threads
  on all 64 CPUs, among the ports' ring CPUs, until 10:28 (the ring
  CPUs spent 13 to 18% in nfsd, and the 1 MiB random READs carried
  44.1 GB/s against 464's 49.1);
- the svcsock suite left `svc_tcp_rx_rcvbuf` at 0 (fixed in the
  suite, which now puts back what it changes; the installed one does
  not, and `run-kunit.sh` writes the setting back after it), so
  every one-port run and every loan cell after it held no buffer;
- each port's async interrupt sat on one of nfsd's CPUs, and the
  transmit steering was not checked by anything.
`settled.sh` (under "Where it stands") was written for it: every
script that runs a load now audits the host against the settled
setting and refuses to run off it.

Run the KUnit suites with `run-kunit.sh` (it leaves the host at the
settled setting and says so), then "The battery" at the end of
`../LOAN-PERFORMANCE.md` from its first step, and lay each figure
beside build 464's, which are in that table:

1. `./memcost-cal.sh $L/memcost-cal-<build>`: the card and the drives
   by themselves (about 6 min, no NFS).
2. `./rwbase.sh <build>`: the READs and mixes above, beside build
   464's (`results-rwbase-464.tsv`).
3. Copy mode on two ports, three runs, and on one port over all
   eight drives, three for each port: the command lines under
   "Where it stands" above. **This is the check for a regression**:
   60.0 to 60.4 GB/s on two ports and 46.5 to 47.4 on one on build
   464. `memcost-log.sh` beside a run and `memcost-read.py` after it
   give its bytes through memory for each byte of payload (2.17 on
   two ports). **Done on build 473**: "Copy mode on 7.3" above. One
   port shows no regression; two ports are about 1.5% under.
4. The loan and its copy on the stand-in:
   `plans/16k-42-loan-memcost.plan`, `16k-43-loan-memcost-small.plan`
   and `16k-44-loan-memcost-floor.plan` through `sweep.sh` with
   `CELL_BOUNCE=1` and `memcost-log.sh` beside each, as written out
   in that document. Take port b down and up once first (`ip link
   set enp164s0f0 down; ip link set enp164s0f0 up`, then
   `./settled.sh place b`). A sweep now ends with port a's rings back
   at 256 and its layout in place, and writes `audit-end.txt`.
   **Done on build 473**: "The loan on 7.3" above. The loan is
   build 464's, cell for cell.
5. **The silent port** (Mike, 14 October): chase it on this kernel,
   after the four above. See "Set aside" below and
   `../SILENT-PORT.md`. **7.3 shows it**: the seventh event, on port
   b in round 2's 70/30 8 MiB mix on build 473 (12:17:50, 21 to 23
   s, the same signature as the sixth); see "The seventh" there.

Before any of it on a new boot: the "After a boot" line under the
rules, `getconf PAGESIZE` (16384), and `./settled.sh apply` ending in
"the settled setting" (it checks `xeu`'s settings, the MTU, the
module settings and the CPU layout; a BAD item names what is off). The
mesh's counter is `arm_cmn/hnf_mc_reqs/`: check that the new kernel
still has it (`ls
/sys/bus/event_source/devices/arm_cmn_0/events/hnf_mc_reqs`, and
`perf stat -a -e arm_cmn/hnf_mc_reqs/ -- sleep 1` reading a few
million idle), since every memory figure rests on it.

## What a loaned byte costs in memory: done on build 464 (13 October, evening)

The account is `../LOAN-PERFORMANCE.md`, and the page is "NFSD Loan
Performance", https://claude.ai/artifact/1hAkQK1xDiqPr1efdv4ZDb
(source `../artifacts/loan-performance.html`).

- **The measure is bytes through memory for each byte of payload**
  (the mesh's requests to the memory controller, 64 bytes each,
  over the payload carried). Copy mode on two ports is 2.17.
- **The card and the drives move data through the cache.** One port
  receiving 49.6 GB/s uses 0.03; the drives writing data still in
  the cache use 0.09, and 0.58 for data that has left it. So a
  loaned byte uses about 0.1 if it is still in the cache when a
  drive takes it and about 1.6 if not.
- **A loan is not bounded by memory as a copy is.** A copy's limit
  is quoted only as measured, the bare benchmark's 62 to 68 GB/s;
  no figure derived from the DIMMs' 166 (Mike).
- **On the stand-in the loan's use falls with its footprint**: 4.07
  at about 37 MiB, 2.99 at about 24, 1.89 at 23 with rings of 128,
  and a floor near 1.3 at 17 to 18 MiB with rings of 64, all at
  11.4 to 11.7 GB/s, where its copy uses 5.75 to 4.71. The floor is
  inside the cache and is the stand-in's own. Every loaned WRITE
  was written direct and none was over its charge. Fewer than 8
  WRITEs let in costs rate (a tenth at 6, a third at 4), a 128 KiB
  held buffer costs 7%, and rings of 64 drop tens of thousands of
  frames; the best setting that loses nothing is rings of 128, a
  192 to 256 KiB held buffer and 8 WRITEs let in.
- **A footprint** is what a load takes from the region beyond what
  was there idle plus its own port's posted rings. `CELL_BOUNCE=1`
  to `sweep.sh` empties the port's pools before each cell so that
  it reads true; the region's pages less its free ones is not it.
- "Dialing in loan mode" in that document is what to do when the
  SGL drives are here.
- **The stand-in** is NVMe/TCP on this host in front of a file on
  `/hs/nvme0`. It passes the data through TCP to itself, so its
  figures are not an SGL drive's; the difference between a loan and
  its copy is what to read. It leaves `nvmet`, `nvmet_tcp` and
  `nvme_tcp` loaded with nothing configured.
- The mesh counts two of its events at a time. Ask for two
  (`hnf_mc_reqs`, `hnf_cache_fill`), as the scripts do; with more,
  each is counted part of the time and scaled.

Not done, and not asked for: the loan on two ports, which `sweep.sh`
does not do. What a loan uses and carries in front of a drive that
takes the pages itself waits for the SGL drives.

**Writing about it:** the measure is "bytes through memory for each
byte of payload", and a footprint is as above. Mike asked for the
pages to be rewritten when a figure or a term changed, without
dwelling on what they said before; the loan page and
`../LOAN-PERFORMANCE.md` are written that way.

## The rules

- **A run stays inside the drives' fast region**: about 625 GB a
  drive from its last idle of 20 s. `twoport.sh` defaults to
  `WARM=15 RUNTIME=45` after `REST=30`; a traced run
  (`PROFILE=1`, `RUNTIME=55`) wants `WARM=10`. Read `drives.txt` of
  every run (what each drive wrote, and `no drive stepped down`)
  before its figure.
- **The drives are `write through`** (Mike: "never forget this").
  `./drive-cache.sh` prints it; `twoport.sh` applies it.
- **Nothing connected before a port is reconfigured or `xeu`
  reloaded**: `ss -tn state established '( sport = :2049 )'`.
- **The client is reached at 10.20.12.106** (`CLIMGMT`), not over a
  port under test.
- **Copy mode is not optimized at the loan's expense** (Mike): one
  frame to a buffer, pages returned last in first out, all inside
  the region.
- After a boot: `mount -o noatime /dev/nvmeNn1 /hs/nvmeN` for 0 to
  7, `./bringup.sh 32768 256` (the region and port a), `./port-setup.sh
  enp164s0f0 56` (port b), then `./settled.sh apply`, which has to
  end in "the settled setting".
- **Every run is audited against `settled.sh` and refuses to run off
  it** (Mike, 14 October). Do not get past a BAD item by hand or by
  a variable: put the host back with `./settled.sh apply` (or what
  the item names), and change the setting itself only in
  `settled.sh`, after a run that shows why.

## Decided on 13 October: do not undo from older text

| what | now | why |
|---|---|---|
| `svc_tcp_rx_lowat` | 0, in `modprobe.d` | 131072 halves wakeups on two ports and costs 2 to 3 GB/s; about 1 on one port |
| `fq` on the client | not set; the client keeps `fq_codel` | level with it over 19 runs; the 2 GB/s of 11 October was on flushing drives and runs past the fast region |
| ssh to the client | the management address | the harness's traffic was on port a |
| rings a port | four of 128 on two ports | eight of 64 are level in four layouts, never better |
| `svc_tcp_rx_copy_threads` | 0 | level on four rings, 3 GB/s down on eight, thousands of ring drops |
| the copy's last 4% | left alone | every way to it costs the loan |
| MD raid0 | not used, not compared | Mike |

## Set aside, with what would reopen it

**The silent port**: a port carrying nothing for about 25 s while
the other carries on, five times by 13 October, on either port, all
with `fq`. Not the drives, the card or the server's software: the
client's transmit side for the port stops for about 1.4 s and `fq`'s
pacing then holds it at a trickle. **It showed again on 14 October**
(port a, in the READ baseline above), with the client on `fq_codel`
and lasting the 24 s to the end of the run, so pacing is not what
holds it. **It is to be chased on the 7.3 kernel (Mike, 14
October)**, after the battery under "Next": `../SILENT-PORT.md` has
the account and what to do, and `$L/silent-driver.sh` runs with both
ends sampled.

Noted and not looked into: port a alone drops 33 to 1,087 frames at
the card's FIFO in a one-port run and port b none; port a's link is
optical with 1.5 million bits a second corrected by FEC at the
client, port b's is 2 m of copper with none.

## Where each account is

| document | what |
|---|---|
| `../XEU_CONVERGENCE.md` | the dated accounts; 13 October's are "What a drive takes", "The 128 KiB mark off and on", "Eight receive rings again" and "The client's queueing again" |
| `../XEU-4-VS-8-RING.md` | where interrupts and CPUs sit with four rings and eight, the eight-ring layouts, copy threads |
| `../TRACING-NFSD-AND-XEU-MEMCPY-TIME.md` | the receive path traced at 60 GB/s, the memory's limit, where the copy's 4% goes and why it is left |
| `../SILENT-PORT.md` | the silent port |
| `../TARDIS1_COPY_PERFORMANCE.md` | the copy benchmark of 12 October |
| `../LOAN-PERFORMANCE.md` | what a loaned byte costs in memory: the counter, the card and the drives alone, the loan on the stand-in, and the battery for the next kernel |
| `../LOST-TIME-CHASING-SPENT-DRIVES.md` | what 12 October's slow drives skewed; closed |
| `results-twoport-464.tsv` | a line for every two-port and one-port run |
| the artifacts | "NFSD Receive Region Budget", https://claude.ai/artifact/3UQkMVoWW8p9A5ZASPnm3g, source `../artifacts/region-budget.html` (its `droids.gif` is published beside it and is not in the tree): copy mode. "NFSD Loan Performance", https://claude.ai/artifact/1hAkQK1xDiqPr1efdv4ZDb, source `../artifacts/loan-performance.html`: the loan. Both are open to anyone with the link (Mike). To change one from another session, read it first and publish to its link |

Tools new on 13 October, all in this directory: `drvchar.py` (a
drive's fast region), `stall-sample.py` and `stall-read.py` (both
ends of both ports five times a second), `copy-adjacency.bt` and
`ring-flow-runs.bt` (where the copy's pieces come from, and a
connection's runs of frames on a ring), `copybench.c` with `wr`,
`rd` and `RUN=`, `memcost-cal.sh`, `memcost-log.sh` and
`memcost-read.py` (bytes through memory for each byte of payload);
`twoport.sh` gained `LOWAT=`, `TXN=`, `NFSD_CPUS=`, `QDISC=fqnp` and
a `PROFILE=1` that reports by group of CPUs; `../run-budget.sh`
gained `STEP_SECS=` and `sweep.sh` `CELL_REST=`.

The host as left: no NFS connections, nothing exported, both client
ports on `fq_codel`, the drives `write through`, the live
`svc_tcp_rx_lowat` 0, budget 0, four rings of 256 on each port (both
taken down and up during the floor run, their interrupts placed
again), the stand-in's modules loaded and idle.

Build 464 was built from
`origin/kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY-7.1.13-21.hs.464.loanpages_pool.el9.aarch64+16k`;
at the end of 13 October that held all of that day's work. On 14
October the series was regrouped (code, KUnit interface, KUnit tests,
docs) and `origin/kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY` force-pushed
at `ea18d80f8bda`; the same series was then rebased onto `v7.3-3` as
`kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`, where work goes on.

# 12 October, 22:30, with 13 October's notes as they were added (history: the section above supersedes it)

Its figures for two ports (58.5), for one port on four drives (42.2)
and "two `sunrpc` settings pinned" are of 12 October; the command
lines below that pass `QDISC=fq` are of then.


**The drives are characterised (12 October, 21:07 to 22:30 by
tardis1's clock), and a run has to stay inside their fast region.**
A drive takes about 625 GB at full rate from its last idle of 20 s.
Past that it keeps up only while the writes do not stop: after any
pause, 0.3 s is enough, it is at 4 GB/s for 25 to 55 s. Rest was
never the matter (20 s is all a drive needs); the length of the run
was: 120 s on two ports is 1.28 TB a drive. "What a drive takes" in
`../XEU_CONVERGENCE.md`; `./drvchar.py` is the local probe.

- `twoport.sh` now defaults to `WARM=15 RUNTIME=45` and waits
  `REST=30` first: two ports stay inside the region. **One port on
  its own four drives needs `WARM=10 RUNTIME=25`; better, give it
  all eight** (`FS_A="/hs/nvme0 ... /hs/nvme7"`, `RUNTIME=60`).
- **Read `drives.txt` of every run**: what each drive wrote (marked
  past 600 GB) and `no drive stepped down`. `drives-sec.tsv` is
  every drive each second; `./drvsum.sh` summarises it.
- **Rerun that way, 60 s apart, no drive stepping down in fifteen
  runs: two ports 57.0, 58.0, 58.2 GB/s; one port on eight drives
  45.2 to 46.8 (93 to 94% of plain TCP); one port on its four 44.4
  to 45.2, the drives at 100%.** `$L/rested-driver.sh` ran them.
- **The 128 KiB mark is unpinned (13 October):
  `svc_tcp_rx_lowat=0` in `/etc/modprobe.d/sunrpc.conf`.** Off, two
  ports carry 60.0 and 60.7 GB/s and one port 46.5 to 47.4; at
  131072, 57.9 and 57.3, and 45.6 to 46.3. `LOWAT=` in `twoport.sh`
  sets it for a run. The account is "The 128 KiB mark off and on" in
  `../XEU_CONVERGENCE.md`. The artifact was brought up to date the
  same morning.
- **Eight receive rings were run again (13 October) and are level
  with four**: 8 x 64 carries 59.5 to 60.5 GB/s against 59.6 to 60.7
  for 4 x 128, with the Rx CPUs a third less busy. `xeu` takes only
  powers of two for a ring's depth. Four of 128 stays. "Eight
  receive rings again" in `../XEU_CONVERGENCE.md`.
- **`fq` on the client is unpinned (Mike, 13 October): the two-port
  setting leaves the client on its default `fq_codel`,** so nothing
  on the client is changed. By turns inside the drives' fast region:
  `fq` 59.4 to 60.7 (seven runs), `fq nopacing` 59.9 to 60.3,
  `fq_codel` 60.0 to 60.4. The 2 GB/s of 11 October was with the
  drives flushing and runs past the fast region. Do not pass
  `QDISC=fq`. "The client's queueing again" in
  `../XEU_CONVERGENCE.md`.
- **The scripts reach the client over the management network (13
  October): `CLIMGMT`, 10.20.12.106, in `common.sh`.** Every ssh and
  scp to the client goes there and not over a port under test.
  `CLIIP` and `twoport.sh`'s `CLI[]` stay the client's addresses on
  the ports, for what is exported to, mounted and pinged. The first
  two-port run that way, on the client's default queueing:
  `mgmt-two-1`, 60.32 GB/s.
- **The silent port** (a port carrying nothing for 25 s in about one
  two-port run in ten, five times by 13 October) is set aside again
  (Mike): chase it if it shows again. All five were with `fq`, whose
  pacing is what holds the port at a trickle once it has begun; none
  showed in 19 runs of the comparison. `../SILENT-PORT.md`, with
  `stall-sample.py` and `stall-read.py` to catch one at both ends.
- Nothing is left from `../LOST-TIME-CHASING-SPENT-DRIVES.md`
  (Mike, 13 October): raid0 is not compared again, and the silent
  port only if it shows again. The work now is two ports with more
  parallelism: `../XEU-4-VS-8-RING.md`. Each is now a comparison on a level
  floor. Where this section says a figure was "limited by the
  drives" or gives a range of two-port totals, that was this.

**There is no MD any more (Mike, 12 October evening).** Each data
drive has its own XFS on `/hs/nvme0` to `/hs/nvme7` (not in
`/etc/fstab`: `mount -o noatime /dev/nvmeNn1 /hs/nvmeN` after a
boot). `twoport.sh` exports and mounts a port's four and divides its
writers among them. All eight drives were formatted (`nvme format`)
that evening and are 4 KiB. Where this section says stripe, read the
port's four drives. Two ports that way: 52.6, 50.9, 50.4 GB/s, no
gain over the stripes; single drives go slow for a run (4.4 GB/s at
98% busy), a different one each time. Also tried that evening and
not gains: eight receive rings on CPUs of their own, nfsd's threads
split by port.

**The drives are run as enterprise storage (Mike: "never forget
this").** Linux sees `write through` on the eight data drives and
both stripes, so no cache flush is sent; the drives' caches stay on
and the export stays `sync`. Check with `./drive-cache.sh` before
any run (`twoport.sh` and `sweep.sh` run it; a udev rule sets it at
boot, with the drives' autonomous power states off). Before this,
every COMMIT (one per 8 MiB write of the job) flushed all four
drives of a stripe and held one port at 36 GB/s.

**Two ports: 58.5 GB/s at best** (seven runs with no flush sent,
45.0 to 58.5), same setting as before (61.0 on 13 October with the
mark off; `QDISC=fq` was part of it until that day and is not now):

    RCVBUF=393216 ENTRIES=128 LINGER=0 LOWAT=0 ./twoport.sh <out> ab

**One port on four drives: 42.2 at best, and limited by the drives
in every run** (six runs, 18.1 to 42.2), each of them past the
drives' fast region (above). Read `drives.txt` in a run's
directory, or `drvbusy_a`/`drvbusy_b` in the TSV, before believing a
one-port figure. `twoport.sh` warms up for 15 s (`WARM`; 40 when
these were run) before fio counts. The best one port has carried is
now 46.8, on all eight drives (above).

**The copy is at about 90% of what the machine can copy** (62 to
68 GB/s memory to memory, reached with 8 CPUs). A copy that does
not fetch its destination gains nothing: `copybench.c`, and
`../TARDIS1_COPY_PERFORMANCE.md`. Next: what sets that ceiling
(memory channels populated?), and the loan. The 12% of cycles in
wakeups and context switches has not been traced.

**Two `sunrpc` settings pinned on 12 October, both in
`/etc/modprobe.d/sunrpc.conf`:** `svc_tcp_rx_keep=1` (a thread that
finds a WRITE short does not wake another for data already there)
and `svc_tcp_rx_lowat=131072` (a thread is woken when 128 KiB of a
short WRITE has arrived, not for each piece). Together they take
thread wakeups on two ports from 1.15 million a second to 0.40
million. **The second was taken off on 13 October: it costs 2 to 3
GB/s on two ports and about 1 on one.** Splitting nfsd's threads by port was tried
and is not a gain. All in `../XEU_CONVERGENCE.md` from "nfsd's
threads split by port".

The client is on 7.3.0-0.rc7.2.hs.469, which changed little. The
account is "No cache flush to the drives, and two ports at 58.5
GB/s" in `../XEU_CONVERGENCE.md`.

# On build 464: start here (2026-10-11, 10:00)

## 1. The baseline is done: 43.0 and 42.5 GB/s

Build 464 carries the tip and is level with 463 (42.5 and 42.7) at the
pinned copy setting: `rx_buf_size=8192`, MTU 7792, rings of 4 x 256,
`rx_refill_thresh=8`, a 1 MiB held buffer, 16 connections, `PLACE=1`,
plan `plans/16k-35-rxbuf8192.plan`, run by
`$L/xeu-16k-464-baseline-driver.sh` (`RUNS="5 6"` for two more). The
account is "Build 464: the baseline" in `../XEU_CONVERGENCE.md`.

**The svcsock KUnit suite used to leave the factor fixed at 150** in
the loaded `sunrpc`, and every window after it was 348 KiB where it
should be 487 (41.6 and 40.4 GB/s, the first two runs of this boot).
It is fixed at the tip, and the fixed `svcsock_kunit.ko` is installed
in 464's `updates/`, so `../run-kunit.sh` on this build is safe to
measure after. It was not seen before 10 October because the harness
wrote `svc_tcp_rx_credit_factor` for every cell, which put back
whatever the suite had left; the parameter's removal took that away.
The check, if in doubt: `rcv_ssthresh` in a run's `ss-server.txt` is
487424 or 483497 here.

**The state of tardis1 this boot:** `sunrpc` built from the tip in
464's `updates/` (it has the hold tracepoint; the packaged one is
beside it), `pools_per_node` 4, nfsd at 512 threads, a
32768-page region bound, rings of 4 x 256, `/dev/md0` on `/hs/stripe`,
`kernel.panic` 10, nfsd's threads on CPUs 8-55. About 52 TB written
to the stripe. The hand-built
modules in `/lib/modules/7.1.13-21.hs.463…/updates/` are still there
and belong to 463 only.

## 2. TODO, next: what limits the drain (Mike, 2026-10-11)

The saved traces of the 42.7 GB/s run
(`$L/xeu-16k-463-tipcheck-2`) say the limit is how fast the server
drains each connection: the client's sixteen connections were
`rwnd_limited` for 94% to 98% of their busy time. The copy is 25% of
server cycles. What else is there, before it and after it:

**Step A is done (2026-10-11): "What limits the drain, step A" in
`../XEU_CONVERGENCE.md`.** Use `perf report --no-inline` on that file;
it is quick.

- `arch_counter_get_cntpct` is the linger's wait loop, not the
  tracing. The cycles are a thread waiting, and a cheaper clock would
  free nothing.
- A connection copies about two fifths of the time and waits in the
  linger on an empty socket about one fifth. The copy's own speed is
  not all that holds the drain: the window and the round trip to
  refill it are in it.
- The copy is done with the socket locked (trace and code). Arrivals
  go to the backlog and a reply waits for the lock under the send
  mutex.

**Step B is done (2026-10-11): "What limits the drain, step B" in
`../XEU_CONVERGENCE.md`.**

- Eight rings of 128 on 8192-byte buffers: 42.2 and 41.9, no better
  than four (43.0 and 42.5), with the rings' CPUs 58% idle, fifty
  times the ring drops and a third more system time. The rings' CPUs
  are not the limit.
- **Do not set `napi_defer_hard_irqs` or `gro_flush_timeout` on the
  port: it stops receiving** and only a down and up brings it back.
  For Xsight.
- `svcsock_tcp_rx_hold` (in the hand-built `sunrpc` now loaded) shows
  what each transport is held to.

**The copy is made outside the socket lock (2026-10-11)**, at the tip
and in the loaded module: 43.6 and 43.5 GB/s against 43.6 with
`svc_tcp_rx_copy_unlocked=N` on the same module, so no gain by itself.
It is the first step towards two threads on one connection. "The copy
outside the socket lock" in `../XEU_CONVERGENCE.md`. **Pin every nfsd
thread to 8-55 after `echo 512`:** with that the baseline is 43.5,
not 42.5 to 43.0.

**Two threads on one connection (2026-10-11)**:
`svc_tcp_rx_copy_threads` 1 or 2, at the tip and in the loaded module,
default 0. 43.6 to 44.0 GB/s against 44.2: no gain, though the client
is held by the window less (85% of the time against 96%). Retransmits
rise four to ten times. "Two threads on one connection" in
`../XEU_CONVERGENCE.md`. **Everything tried on the server's drain
today lands between 43.5 and 44.6**; the client has not been looked at
since. Two runs of ten had about 360 RTOs and 34 to 35 GB/s, not
explained.

**A larger held buffer with claiming (2026-10-11)**:
`plans/16k-40-claim-1536k.plan`, `16k-41-claim-2m.plan`. Claiming
gains nothing at 1.5 MiB or 2 MiB (43.9 to 44.1 against 44.3 to 44.6
without). The client is held by the window 30% to 59% of the time
instead of 96% and the rate does not move; ring drops and retransmits
rise a hundred times. **The window is not the limit; look at the
client next.** "A larger held buffer, with and without claiming" in
`../XEU_CONVERGENCE.md`. The pinned setting stays at 1 MiB.

**The client looked at (2026-10-11)**: `client-look.sh` (run on the
client during a cell: counters 20 s apart and a 10 s profile). **The
client puts 355 to 363 Gbit/s on the wire, 92% to 94% of the 48.4 GB/s
iperf3 put on it; about 3.5 GB/s is all that is left.** It is not the
limit: every socket has megabytes unsent, a WRITE waits 11.6 ms for
its connection, a third of its CPUs are idle and no thread is near a
whole CPU. It copies what it sends (16% of its cycles; the RPC
client's send does not splice pages). "The client" in
`../XEU_CONVERGENCE.md`.

**fq on the client (2026-10-11)**: tried at 1 MiB, 1.5 MiB and 2 MiB
and put back. Ring drops and retransmits fall to a tenth to two
fifths; the rate is 42.0 to 42.2 at 1 MiB, 43.2 to 43.6 at 1.5 MiB,
44.2 to 44.4 at 2 MiB, so no gain. A paced sender waits on the window
more. "fq on the client" in `../XEU_CONVERGENCE.md`. **44.2 to 44.6
GB/s is the ceiling by every route tried: window, loss, or pacing.**
The client's `mq` has no handle: `tc qdisc replace dev ens3np0 root
handle 1: mq` first, and `tc qdisc del dev ens3np0 root` puts
`fq_codel` back.

**More than two threads (2026-10-11)**: `svc_tcp_rx_copy_threads` 3, 4
and 8 give 43.7 to 44.3, as 2 did. **The runs with hundreds of RTOs
(34 to 40 GB/s) have all been with 0: three of nine at 1 MiB, none of
twelve with claiming.** Not settled, and what happens in such a run
is the open question on the server. "More than two threads" in
`../XEU_CONVERGENCE.md`.

**0 and 2 by turns, six each (2026-10-11, evening)**: 43.9 against
44.2 GB/s on average, inside the spread; no RTO episode in either, or
in fourteen watched runs before. **Keep `svc_tcp_rx_copy_threads` at
0 on one port.** Its retransmits are explained: **the card loses
frames ahead of its rings and counts them nowhere.** One cell of each
accounted for packet by packet: the client's PHY sent 234,702 more
than the rings forwarded or dropped with 2 threads, 5,153 with 0, and
the client retransmitted 240,733 and 10,492. Read a setting's loss
from the client's retransmits, not from the ring drops. For Xsight:
where is a frame dropped ahead of `fwd`, `tail_drop` and `pp_drop`?
The note to Xsight is drafted, not sent:
`../xsight-uncounted-loss-email.txt`, this first and the smaller
questions after it (deferral, moderation, the Toeplitz hash, a filter
to a ring). The SDK has `xsl_rm_eu_rx_port_counters_get()` with
per-priority `tc_drop_packets`, which `xeu` never calls. The older
`../xsight-steering-email.txt` predates the RSS table patch and is
superseded by it.

**The uncounted loss is the port's RX FIFO (2026-10-11, 20:30).**
kmod-xsight 2.0.1-7 (kernel-rpmtree `7a06a06`) is installed and
loaded: with `echo Y > /sys/module/xeu/parameters/rx_port_stats`
(default N), `ethtool -S` lists `port_rx_fifo_tc<N>_drop_packets`
and the FIFO's fill. Accounted packet by packet, the FIFO dropped
13,038 frames in a cell with 0 threads and 234,332 with 2, which is
what was missing (11,445 and 233,507). By the SDK's sizing it is
2 MiB for this port, 268 frames, 46 microseconds at 44 GB/s, and
that is all there is: there is nothing to resize it to. The link is
clean (`xsllink get --fec-hist port=0`: `NCCW: 0`).
**The ceiling near 44 GB/s is that FIFO overflowing in bursts, with
flow control off.** Next: `ethtool -A` on both ends so that it
pauses the sender, and whether it can be made larger (for Xsight).
"The uncounted loss is the port's RX FIFO" in
`../XEU_CONVERGENCE.md`. **Flow control was tried and does not
reach the wire**: with `ethtool -A enp161s0f0 tx on` (and `rx on`
on the client) the client received no pause frame in four cells and
the FIFO went on dropping. **`fq` on the client with the FIFO
counted (22:00)**: its drops fall to an eightieth at 1 MiB (167) and
the rate is 42.2; with every flow capped (`fq maxrate 23gbit`) the
FIFO is never used and the rate is 38.7 with the rings dropping, so
the bursts are not what holds the rate ("fq on the client again" in
`../XEU_CONVERGENCE.md`). `rx on tx on` with the port remade stops
the rings dropping and the FIFO drops it all. For Xsight. The FIFO
is 2 MiB, measured (16327 lines of 16384 at its fullest). After a reload of `xeu`, `bringup.sh`
refuses because the region still shows: run its steps by hand (that
section says which). `rpm -ivh --force` and `rpm -e --justdb` of the
old release install a kmod without removing other kernels' kmods.
"0 and 2 threads by turns" in `../XEU_CONVERGENCE.md`. Do not kprobe
every received segment (`gro-order.bt`): it makes the rings drop.

**The tuning service sets 512 threads and pins them all** (a drop-in,
`hammerspace-nfsd-tuning.service.d/threads.conf`). No more `echo 512`
and `taskset` by hand; a restart of the service does both. It still
leaves `io_cache_write` at 4, which no cell has measured.

**The pinned module settings are in `/etc/modprobe.d` (2026-10-11,
19:00)**: `xeu.conf` gained `rx_refill_thresh=8` (no more setting it
by hand after a load), and a new `sunrpc.conf` has `pools_per_node=4
svc_tcp_rx_rcvbuf=1048576 svc_tcp_rx_linger_us=50`. `sunrpc` was
reloaded from it (`SUNRPC_KO=none ./swap-sunrpc.sh`, no clients
connected) and came up with all three, 512 threads in pools of 128,
all on 8-55. They hold for every port and every NFS connection.
`run-budget.sh` now puts the linger back as it found it after a run,
where it wrote 0. Not in a file yet: the port's MTU of 7792, its
rings and region.

**Two stripes of four drives (2026-10-11, 22:40), for runs on both
ports**: `/dev/md0` (`nvme0n1` to `nvme3n1`) on `/hs/stripe` and
`/dev/md1` (`nvme4n1` to `nvme7n1`) on `/hs/stripe1`, raid0 with a
1 MiB chunk, XFS, `zcbudget` in each. `/hs/nvme0` and `/hs/nvme1`
were wiped for it (Mike) and are out of `/etc/fstab` and
`/etc/exports`. After a boot: `mdadm --assemble --scan`, then `mount
-o noatime` each. Locally each takes 41 to 44 GB/s, alone or
together, **which is about what one port carries: run the pinned
setting again on four drives before comparing two ports with one.**
"Two stripes of four drives" in `../XEU_CONVERGENCE.md`.

**Two ports, the best setting found (2026-10-12, 01:35): 54 GB/s**
(seven runs, 48.1 to 54.2, middle 54.0), with the two ports' receive
memory in 21 MiB of the 32 MiB L3:

    RCVBUF=393216 ENTRIES=128 QDISC=fq LINGER=0 ./twoport.sh <out> ab

384 KiB held buffer, rings of 4 x 128, linger 0, `fq` on the client
(`fq_codel`: 52). **`fq` is no longer part of it (13 October): the
client keeps its default `fq_codel`, which carries the same.** The one-port setting on two ports is 43 (48 MiB).
**It is not the one-port setting: one port alone with it carries 31,
against 36 with 1 MiB and 4 x 256.** Why two ports did not add up:
with receivers that discard without copying, plain TCP carries 99.4
GB/s on the two ports (`MODE=trunc ./iperf2port.sh ab`); with a copy,
86.5; and a memory-to-memory copy on this machine tops out near 62
GB/s. The copy is what the ports share. Every run is in
`results-twoport-464.tsv`; the account is "Two ports: why they did
not add up" in `../XEU_CONVERGENCE.md`. Open: port b runs 2 to 4 GB/s
under port a when both are loaded, and what the smaller memory saves
is not explained by the cache counters.

**Two ports (2026-10-11, 23:40): 36 GB/s each alone on four drives,
and 43.3 together (21.6 each), with each port's FIFO dropping 870
thousand frames in 70 s.** No gain over one port on six drives, and
what the shared limit is is not known: iperf3 to both ports at once
and a profile are next. `./port-setup.sh enp164s0f0 56` gives the
second port the pinned settings and puts it in the region (this boot
only), `./ring-irqs.sh <port> <cpu>` puts its rings' interrupts on
56-63 (the driver puts every port's rings on 0-7, so two ports would
share them; redo after a down and up or an `ethtool -G`), `./twoport.sh <out> a|b|ab` runs either port or both.
"Two ports" in `../XEU_CONVERGENCE.md`.

**`io_cache_write` 2 is pinned (2026-10-11, 19:20)**: one cell with 4,
which the tuning service's profile set, carried 28.6 GB/s against 44:
nfsd syncs every 1 MiB WRITE where the client's COMMIT syncs 8 MiB.
The service's drop-in now passes `--io-cache-write 2`, so a restart
or a boot leaves 2. "nfsd's write mode" in `../XEU_CONVERGENCE.md`.
The bundle's own `e1` profile (`HS_IO_CACHE_WRITE=4` in
`/usr/local/sbin/hs-tune-common.sh`) is unchanged: the fix is this
host's drop-in only. Changing the default there is for later (Mike,
2026-10-11); do not edit the profile until he asks.

**Noted.**

- 12% of lingers time out (287,671 of 2.37 million), 50 us of a
  thread each and a hand-off of the connection.
- A quarter of direct writes take over 1 ms (147 thousand of 619
  thousand; most take 130 to 500 us) and 52 thousand take 4 to 8 ms.
  XFS's inode lock and log are in the lock table. With 512 threads and
  the client waiting on the window, this is not the limit now; it
  would matter once the drain is faster.

# The xeu rig at 16 KiB: where it stands (2026-10-10, 12:50, on build 463)

**Build 463 is running, and both new `xeu` features work on the
card.** The account is "Build 463: the RSS table and `rx_buf_size` on
the card" in `../XEU_CONVERGENCE.md`. In short:

- **The RSS table** (`rss_indir_size=128`, now in
  `/etc/modprobe.d/xeu.conf`): `ethtool -X` moves flows between
  rings, and bits 13 to 19 of a flow's hash pick its table entry, so a
  flow is placed by setting that entry (`ynl` `rss-set`, with
  `--no-schema`; `rss-balance.py` builds the request
  from `xeu-rx.bt`'s output).
- **Two bugs in the RSS patch were fixed** and the kmod rebuilt and
  loaded (`xeu` srcversion `9B0770879836A992C43D41D`, still Release
  6, from `$L/kmod-xsight-16k-463-r6fix`): setting the key had
  stopped the hashing, and the firmware's own table could not be
  read. **A kmod built before kernel-rpmtree `b32b6c9` has both
  bugs: do not use `ethtool -X hkey` on it.**
- **`rx_buf_size=5440`** receives a frame in two buffers and three
  fifths of the region pages (1360 to 1376 against 2168 to 2376), and
  dropped no frames where the whole page dropped 3557 and 127510. The
  rate does not separate the sizes in two runs each (41.0 and 36.6
  GB/s against 41.4 and 38.3).
- The `ethtool` 6.15 command cannot set the hash function on this
  port; `ynl` can. A flow's hash cannot yet be computed from its
  ports.

**The state of tardis1** (it does not outlive a reboot, except the
modprobe file and the installed kmod): `xeu` loaded with
`rx_buf_size=5440`, a 32768-page region bound, rings of 4 x 512,
`rx_refill_thresh` 8, `pools_per_node` 4, nfsd at 512 threads,
`/dev/md0` on `/hs/stripe`, `kernel.panic` 10. This boot wrote
about 18 TB to the stripe.

**`rx_buf_size=5440` is settled for now (Mike, 2026-10-10): no more
runs against the whole page.** What it gives is enough. The next work
is to settle the other settings around it: ring depth, the held
buffer, `rx_refill_thresh`, the linger, the pools. It is pinned:
`/etc/modprobe.d/xeu.conf` has `rx_ip_align=2 rss_indir_size=128
rx_buf_size=5440`, and `xeu` was reloaded with it (rings of 4 x 512,
the region bound again, `rx_refill_thresh` 8). The artifact
was republished on 2026-10-10 with a first section on build 463 and
the call with Xsight (`id="xeu463"`, `id="call"`).

**`PLACE=1` places the flows (2026-10-10, 14:25):** the harness sets
the RSS table so each ring carries a quarter, in place of
`BALANCE=35`. Proved under NFS in one cell: 23/21/36/20% as the flows
fell, 25.0/25.0/25.4/24.6% in the cell, 39.9 GB/s. Use it for every
cell from here ("Flows placed on the rings under NFS" in
`../XEU_CONVERGENCE.md`).

**Ring depth and the held buffer (2026-10-10, 16:05):** on 5440-byte
buffers, placed, two runs each: **4 x 512 with a 1 MiB buffer carries
41.8 and 40.7 GB/s with no drops, in 28.5 to 30 MiB**; 685 KiB on the
same rings 39.4 and 39.6; rings of 4 x 256 drop 105 to 880 thousand
frames. The rings were left at 4 x 256 by the last cell: `ethtool -G
enp161s0f0 rx 512` before anything else. The account is "Ring depth
and the held buffer on 5440-byte buffers: the results" in
`../XEU_CONVERGENCE.md`.

**Eight rings did not help (2026-10-10, 17:10):** 39.6 and 39.3 GB/s
at 8 x 256 with the 1 MiB buffer against 4 x 512's 41.8 and 40.7; the
rings' CPUs went from 9% idle to 60% to 70% idle and the region in use
went past the L3 (32 to 33 MiB). With the 685 KiB buffer eight rings carry 39.8 and 39.8, four
rings' rate. The rings' CPUs are not the limit. **The port is back at
4 rings of 512 entries and nfsd's threads on CPUs 8-55.** A 1 MiB
WRITE takes about 360 us to receive on its connection, one thread at
a time, so sixteen connections top out near 46 GB/s: 32 connections
(`STEP_MOUNTS=2`) were tried and the client fell over: 12 GB/s with
the client's CPUs 80% in system time and the server 88% idle. Mike
stopped the queue. **Do not run two mounts from this client again
without looking at the client first** ("Thirty-two connections" in
`../XEU_CONVERGENCE.md`; it also records a page pool that has not
finished shutting down since the 8-to-4 ring change).

**The client's kernel changes (Mike, 2026-10-10, about 18:40):** he
is rebooting 192.168.1.106 from `6.12.76-2.hs.212.el8` into
`v7.1.13-21.hs.460.el9`, which is built on the same `v7.1.13-21` as
tardis1's kernel. Every figure before that was taken with the 6.12
client. On the new client kernel, first run `plans/16k-32-best.plan`
once as its baseline (41.8, 40.7 and 41.1 GB/s on the old one), then
`./conns-watch.sh 2 <outdir>` to see whether two mounts still
collapse. `kernel-rpmtree` was copied to the client at
`/root/snitm/git/kernel-rpmtree`.

**The best setting is now 8192-byte buffers with the MTU at 7792
(2026-10-10, 21:15): 43.2 and 43.3 GB/s** in 24 to 26 MiB, against
5440's 41 to 42 in 27 to 30. One buffer to a frame, so it is also the
setting a loan should try first. `plans/16k-35-rxbuf8192.plan`, run
with `HW_ANY_MTU=1`; **Both are pinned (Mike, 2026-10-10, 21:40):**
`/etc/modprobe.d/xeu.conf` has `rx_ip_align=2 rss_indir_size=128
rx_buf_size=8192`, and NetworkManager's `enp161s0f0` profile has MTU
7792 (`nmcli con mod enp161s0f0 802-3-ethernet.mtu 7792`). The region,
the four rings of 256 and `rx_refill_thresh` 8 are still set by hand
after a boot or a reload. `rx_buf_size` is the driver's, so the other
port, `enp164s0f0`, has 8192-byte buffers too; its MTU is still 9000,
where a frame takes two of them.

**The client keeps MTU 9000, and that is safe.** Each end of a TCP
connection advertises its MSS from its own MTU in the SYN, and a
sender never sends more than the peer's. The server advertises 7752
(7792 less 40), so the client's segments fit a buffer whatever its
own MTU is. Read on the client during a run (`ss -tin dst
192.168.1.211`): `mss:7740` (7752 less 12 of TCP timestamps),
`advmss:8948`, `pmtu:9000`. It is per connection and settled before
data moves, so nothing is fragmented and no path MTU discovery is
involved; the server's own frames are at most 7792, which a
9000-byte port takes; the client's traffic to other hosts is
untouched. **It covers TCP only**: `ping -s 8500` from the client to
the server gets no answer (with or without DF), where `ping -M do -s
7764` does. NFS and its mount here are over TCP. Anything that sends
the server a single non-TCP packet over 7792 bytes will lose it. The account is "One
buffer to a frame" in `../XEU_CONVERGENCE.md`, which also says that
`where.sh`'s per-ring GB/s were a tenth too high before this.

**The loan is written direct on the 8192-byte buffers (2026-10-10,
21:25):** on NVMe/TCP, all 2.5 million loaned WRITEs direct, 20.9
GB/s against its copy's 25.4 at 0.61 CPU-seconds per GB against 1.43,
and no crash. It is held by its budget, charged as if a frame had a
whole page: set the factor for two frames to a page before the next
loan run ("The loan on 8192-byte buffers is written direct" in
`../XEU_CONVERGENCE.md`).

**`svc_tcp_rx_credit_factor` is gone (2026-10-11, 00:20).** A
transport's window is held by what its skbs are measured to occupy
(`hold_factor` in the budget's statistics: 107 here) and opened to
half of what its held buffer holds; a budget learns a loan's charge.
The copy at the pinned setting: 42.5 and 42.7 GB/s, against 42.4 and
42.0 the same night on the code before with its factor at 190. **The
rig is about 1 GB/s under its 43.3 of 21:00 whichever code runs**, not
explained. The sunrpc of the tip is loaded from `updates/`
(`954D242F33480A3301D26E3`). "The factor is gone" in
`../XEU_CONVERGENCE.md` has the six commits and the five batches.

**The kernel learns a loan's charge now (2026-10-10, 22:30).**
"SUNRPC: Learn a loaned record's charge from the records":
`svc_tcp_rx_credit_factor` 0, the default, has each budget keep the
most pages a record held. The sunrpc built from the tip is loaded
from `updates/` (`7ED891B5AFE050EE947DD94`). A loan cell is run with
`KERNEL_CREDIT_FACTOR=0`; the harness still writes its own factor for
copy cells ("The charge for a loan" in `../XEU_CONVERGENCE.md`).

**Where the search for the rest stands (2026-10-10, 19:45).** The
copy carries 41 to 42 GB/s of the wire's 48 at the best setting
(`plans/16k-32-best.plan`), on the 6.12 client and on the 7.1.13-21
one. Tried since and not a gain: eight rings (the rings' CPUs were
not the limit), a load paced at the source and stepped up, and 32
connections from two mounts (37.0 GB/s at 1 MiB a connection, 35.7 at
512 KiB with the footprint held). A 1 MiB WRITE takes about 350 us to
receive on its connection, of which the copy is about 150. A 1.5 MiB
held buffer gave 41.4: less waiting and a dearer copy (38 MiB in use,
`memmove` 38%), which cancel. Not tried: a shorter or longer linger,
`rx_refill_thresh`, one pool against four; none is expected to move
the rate.

**The loan written direct waits for drives, not for the card (Mike,
2026-10-10).** Sample NVMe drives that support SGLs are coming to
tardis1. NVMe/TCP was used as the stand-in so that everything a loan
needs for storage that takes arbitrary pieces is in place when they
arrive. What the card will not do is header-data split or payload
aggregation: Hammerspace raised header-data split with coalescing
with Xsight, who showed no interest in delivering it, and
`../xeu-hds-experiment/` is not being pursued. So on the drives
tardis1 has today, which need whole pages, a loan goes through the
page cache; on the SGL drives it can be written direct if every piece
is on a 4-byte boundary, which is the TODO below. For the copy past
41 to 42 GB/s what is left is how one connection's WRITE is received
(one thread at a time, about 350 us for 1 MiB).

**Not done from the list below:** 5440 at the old
setting of 256 entries and a 1 MiB buffer; and everything in step 3
(the loan cell that crashed tardis1, the linger's values, the pools,
4 x 128, the region resize test). Step 0's crash log cannot be read:
the journal holds only the running boot.

**From build 462, still standing:** the whole stack carried 38.0 GB/s
at the median of three traced runs there (36.7 to 38.3; "Build 462:
the whole stack of changes" in `../XEU_CONVERGENCE.md`). **Step 6 of
"The boot after build 461", what each change is worth, waits (Mike,
2026-10-09): do not start it unasked.** Step 7 has not been run.

## The boot into the next kernel: the new xeu features first (planned 2026-10-09, late)

**Mike is building a kernel from the tip of this tree (`…-WIP3`, to be
pushed to hs-gitlab) with kmod-xsight 2.0.1-6 (kernel-rpmtree
`xsight-WIP3`, 8c58863).** On that boot, test the two new `xeu`
features first: where flows land on the rings (the RSS table) and
receive buffers smaller than a page (`rx_buf_size`). Mike wants to
see both working. Everything else in this file comes after.

What that kernel has, so none of it needs building or swapping:
- the sunrpc fixes, built in: the stack-overflow fix, its KUnit test,
  the `BUILD_BUG_ON`, and the linger tracepoint;
- kmod-xsight 2.0.1-6, with `rss_indir_size` and `rx_buf_size`
  beside `rx_refill_thresh` and `rx_ip_align`.

The `updates/sunrpc.ko` of build 462 is under that kernel's own
`/lib/modules`, so it does not load on the new one.

**Before the reboot (Mike):** add `rss_indir_size=128` to the
`options xeu` line in `/etc/modprobe.d` that has `rx_ip_align=2`. Then
the first boot has the table, and the RSS tests need no reload of
`xeu`.

### 0. First checks (minutes)

- `uname -r` is the new kernel and `getconf PAGESIZE` is 16384.
- `rpm -q kmod-16k-xsight` is 2.0.1-6.
- `modinfo xeu | grep -E 'parm: *(rss_indir_size|rx_buf_size)'`
  shows both.
- `cat /sys/module/xeu/parameters/{rx_ip_align,rss_indir_size,rx_buf_size,rx_refill_thresh}`
  reads 2, 128, 0 and 32.
- `ls /sys/kernel/tracing/events/sunrpc/svcsock_tcp_rx_linger` exists.
- `sysctl kernel.panic=10`, so a panic reboots and does not hang.
- The crash: `journalctl --list-boots`, then `journalctl -k -b <the
  boot that crashed>`. It is a stack-protector panic, or a bad PC,
  under `svc_tcp_rx_release_refs` or `page_pool_put_netmem_bulk`, if
  the crash section below has the cause right.
- `../run-kunit.sh`: svcsock 44/44 with
  `svcsock_rx_page_pool_bulk_release_test`. Before any load, with
  nfs-server stopped; afterwards restore nfsd as "How the module was
  put in" below says.
- `mount -o noatime /dev/md0 /hs/stripe`, `./bringup.sh 32768 256`,
  `echo 512 > /proc/fs/nfsd/threads`. **Run nothing on veth.**

### 1. Ring placement (the RSS table): about an hour, no reload

The tools:
- `./rings.sh 5`: each ring's GB/s, share, frames, and the tail drops.
- `timeout -s INT 5 bpftrace xeu-rx.bt`: each flow's hash (the card's)
  and the NAPI, which is the ring, it landed on; the buffers' truesize
  and frags. It fires on every frame, so the rate while it runs is a
  traced rate.

A load to watch: 16 TCP streams to the port. Run `iperf3 -s -B
192.168.1.211 -D` here, and `iperf3 -c 192.168.1.211 -P 16 -t 60`
on the client, as in 5b of `../XEU_CONVERGENCE.md`. Then the harness's
copy cell, for NFS.

1. `ethtool -x enp161s0f0` before setting anything: the firmware's own
   table. If it fails, `dmesg` names the first entry that is not one of
   the port's rings. Either way, record what it shows. It answers the
   question of how big the hardware's table is.
2. Under load: `rings.sh` and `xeu-rx.bt` give the baseline, the
   shares as they fall and which flow is on which ring.
3. `ethtool -X enp161s0f0 weight 1 0 0 0`. **Everything should move to
   ring 0** (`rings.sh`, under load). That shows the table is used.
   - If only part moves, the hardware indexes fewer entries than
     were set. Reload with a smaller `rss_indir_size` (64, 16, 4).
     The reload is Mike's.
   - Then `weight 0 1 0 0`, and `equal 4` to spread it again.
4. `ethtool -X enp161s0f0 hfunc toeplitz`; `ethtool -x` then shows it
   with the key. In `xeu-rx.bt` the flows' hashes change. Then check
   whether **the hash's low bits pick the entry**: for each flow in
   `@flow`, take hash mod 128, look that entry up in `ethtool -x`, and
   see whether it names the ring the flow is on. If it does for every
   flow:
   - a flow's ring can be chosen;
   - Toeplitz over the 4-tuple with the shown key gives the hash, so
     a flow's entry can be computed from its ports;
   - this is one of the questions to Xsight, answered.
5. If 4 holds, balance by giving each flow's entry a ring of your
   choosing. The `ethtool` CLI (6.11) sets only weights, not single
   entries. 7.1's netlink `rss-set` takes the whole table as an
   `indir` array, and the in-tree `ynl` client sends it. Nothing new
   is needed:

       cd <the kernel tree>
       tools/net/ynl/pyynl/cli.py --spec Documentation/netlink/specs/ethtool.yaml \
           --do rss-set --json '{"header": {"dev-name": "enp161s0f0"}, "indir": [0, 1, 2, 3, ...]}'

   The array is `rss_indir_size` (128) ring numbers, one for each
   entry. To move one flow, read the table with `ethtool -x` (or
   `--do rss-get` with the same header), change that flow's entry,
   and send the whole table back. Then check with `rings.sh` and
   `xeu-rx.bt` that the flow moved and nothing else did. `ynl` needs
   Python 3 with PyYAML on tardis1, which has not been checked. With
   sixteen flows on four rings the aim is four flows to a ring.

Record it all as a new section of `../XEU_CONVERGENCE.md`.

### 2. `rx_buf_size`: one reload, then the A/B

1. Reload `xeu` with pieces of a page. This is Mike's: it drops
   `enp164s0f0` for a few seconds. **Watch this reload:** it may be
   the first time a pool bound to the region is destroyed on a kernel
   with the LIFO stack. Destroying the pool drains the stack in
   `page_pool_empty_ring()` ("page_pool: Recycle a region-bound pool's
   pages last in, first out"). On build 462, `xeu` was reloaded for
   kmod release 5, but the docs do not say whether a region was bound
   then. Have `kernel.panic=10` set (step 0). Afterwards, check `dmesg`
   for warnings from `page_pool` and that `alloc_fail` is still 0.

       echo none > /sys/module/page_pool/parameters/rx_region_dev
       rmmod xeu && modprobe xeu rss_indir_size=128 rx_buf_size=5440
       ./bringup.sh 32768 256

2. `dmesg | grep rx_buf_size` should show: "5376-byte RX buffers, 3 to a
   page, up to 2 to a 9000-byte MTU frame".
3. **See it work.** Under load, `xeu-rx.bt` should show `@truesize`
   10752 with 1 frag for full frames, where a whole page showed 16384
   with 0. Then compare the region's `used_pages` for the same copy
   cell against the control. Expect about 2/3.
4. Then the A/B in "Receive buffers smaller than a page" below:
   - 5376, twice, now;
   - 4096, twice, after another reload;
   - the default, twice, as this boot's control, after a reload
     with `rx_buf_size` left out.

Read every run's ring shares before comparing rates, or run with
`BALANCE=35`.

### 3. Then, in this order

1. The loan cell again, which crashed tardis1, with and without the
   linger. Reading its `rx_pp_recycle_ring_full` and `alloc_slow`
   decides the page-pool items.
2. "Is the linger worth its CPU in copy mode" (50, 25, 20 and 0 us,
   with `linger.bt`).
3. "Does `pools_per_node` earn its place" (64 connections; each pool
   change restarts nfsd, which is Mike's).
4. The 4 x 128 cell ("The footprint against the L3").
5. **The region resize test.** "page_pool: Give the fixed receive
   region back when it is resized" has never run: it was queued after
   the loan cell when tardis1 crashed. It takes the port down, so it
   comes after the loads above. With the port down, write the new size
   (`echo 16384 > /sys/module/page_pool/parameters/rx_region_pages`),
   then bring the port up. `/sys/kernel/debug/page_pool_rx_region`
   should show the new size, and the kernel log should show the old
   region freed. Then `./bringup.sh 32768 256` puts the size back for
   any later run.

## tardis1 crashed in the loan cell (2026-10-09, evening)

(On the next kernel, which has the fix built in, step 1 of "On the
boot after the power cycle" below does not apply. The section above
has that boot's order.)

After the three runs above, the session started the loan cell, traced,
twice: once with no linger and once with `svc_tcp_rx_linger_us` 50. It
meant to run the region resize test next. **tardis1 crashed and did
not reboot itself; it was unreachable until a hard power cycle.** The
console was not read, and it is not yet known which of those runs was
going when it crashed.

**The likely cause is a stack overflow in the batched return** ("SUNRPC:
Return a loaned record's page-pool pages in batches", `bdd837db04f1`).
`svc_tcp_rx_release_refs()` handed `page_pool_put_netmem_bulk()` up to
64 pages at a time. That function collects the pages whose last
reference it drops into an on-stack `bulk[XDP_BULK_QUEUE_SIZE]` (16
entries) and does not check the bound. A loaned 1 MiB WRITE at 16 KiB
is 64 or more pages of one pool, and nfsd usually holds their last
references, so nearly every loaned WRITE wrote up to 48 entries past
the end of that array on an nfsd thread's stack. Only a loaned record
takes this path: every copy-mode run of build 462 had the patch loaded
and never ran it. The loan cell was the first run that did.

Three commits at the tip:

| commit | what |
|---|---|
| "SUNRPC: Keep page-pool return batches within the bulk API's bound" | the batch is `XDP_BULK_QUEUE_SIZE`; `Fixes: bdd837db04f1` |
| "SUNRPC: test releasing more page-pool pages than one bulk batch in the svcsock KUnit suite" | `svcsock_rx_page_pool_bulk_release_test`: 65 pages of one pool, held only by the record, all back in the pool's ring after the release |
| "SUNRPC: Refuse to build a page-pool return batch the bulk API cannot take" | a `BUILD_BUG_ON` in `svc_tcp_rx_release_refs()`: a batch over `XDP_BULK_QUEUE_SIZE` fails the build (checked: 64 does) |

`svcsock_kunit` passed 43/43 on the buggy module because no test
dropped a last reference inside a batch, so nothing was collected. The
new test passes on the fixed code. **Never run it against a sunrpc
from before the fix.** It is expected to overflow the stack there and
can take down whichever host runs it.

**On the boot after the power cycle:**

1. **The sunrpc with the bug is still installed.** It is
   `/lib/modules/$(uname -r)/updates/sunrpc.ko` (srcversion
   `988D31FBE6BF6DC88282CB6`) and loads again on boot. Copy mode does
   not reach the bug; a loan does. Before any loan cell, run
   `./build-sunrpc.sh` from the tip and `./swap-sunrpc.sh`, then
   `../run-kunit.sh`: svcsock should be 44/44.
2. Read the crash from the boot before: `journalctl -k -b -1` if the
   journal is persistent, or else the BMC console. A
   `__stack_chk_fail` panic, or a bad PC, under
   `svc_tcp_rx_release_refs` or `page_pool_put_netmem_bulk` confirms
   the cause. Anything else means it is still open: the linger, and
   the region resize ("page_pool: Give the fixed receive region back
   when it is resized"), are then the suspects.
3. `sysctl kernel.panic=10` before any load, so that a panic reboots
   the box instead of leaving it hung.
4. Then the loan cell's two runs again (no linger, linger 50), and the
   region resize test only after them.

The state below is from before the crash. A reboot resets everything
in it except the installed modules.

## Is the linger worth its CPU in copy mode: 50, 25, 20 and 0 us

What build 462 says so far (the whole stack, sixteen connections):

| linger | runs | GB/s | CPU-seconds per GB | calls per WRITE |
|---|---|---|---|---|
| 50 us | 3 | 38.3, 38.0, 36.7 | 0.52, 0.52, 0.51 | 2.1, 2.1, 2.3 |
| 0 (`xeu-16k-462-minus-linger`) | 1 | 37.2 | 0.43 | 6.9 |

The linger does its job: a WRITE is received in about 2 calls where it
took 7. But in copy mode it has not shown a gain in rate (37.2 is inside
the spread of the three runs at 50), and it costs about a fifth more
server CPU for each GB (0.52 against 0.43). Much of what it was meant
to save between threads, the page swap already saves: a record that
changes hands no longer frees and allocates its pages. And the copy is
held by the L3 and by how the flows fall onto the rings, not by the
server's CPU (38 GB/s at 0.52 is about 20 of the 64 CPUs). So in copy
mode the linger's worth is open. A shorter wait could keep the calls
near 2 at less CPU, or show that the linger is not needed in copy mode.
For the loan it is a separate question: a loaned record that changes
hands carries its loan table with it.

- **The cell:** `plans/16k-26-copybest.plan` (rings of 4 x 256, 1 MiB
  held buffer), the rest of the stack as on build 462, with
  `RX_LINGER_US` at 50, 25, 20 and 0. The linger is set for each run,
  so nothing needs a build or an nfsd restart and no client is dropped.
  50 is run again as this boot's control, and 0 is the cell that
  answers whether the linger is needed at all.
- **The order:** after the fixed sunrpc is in place. 50, 25, 20, 0, and
  then all four again. Eight runs of about 3 TB is 24 TB on the stripe:
  say so to Mike before the queue, and trim before each run.

      RX_LINGER_US=<us> PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/xeu-16k-<build>-linger<us>-<k> plans/16k-26-copybest.plan

- **What decides it:** `@calls_per_write` and `@write_in_calls_us` in
  `handoff-*.txt`, CPU-seconds per GB, `arch_counter_get_cntpct`'s share
  of the profile, and the rate. The best value is the shortest linger
  that keeps calls near 2, if the rate holds. If 0 runs as fast as the
  others at less CPU, the linger is off for copy mode.
- **How long each wait lasts, which picks the value directly.** "SUNRPC:
  Trace how long a thread lingers for the rest of a TCP record" adds
  the tracepoint `sunrpc:svcsock_tcp_rx_linger`. It fires once per
  wait, with the time waited, the limit, how much of the fragment had
  arrived, and why the wait ended: data, timeout, close or resched.
  `linger.bt` makes histograms of it by outcome, and `trace-sweep.sh`
  runs it for 8 s in every cell, into `linger-*.txt`. The cell's
  tracers now end at about 94 s of its 120. Read the waits that ended
  on data: the limit wants to sit just above most of them. Every
  timeout is a wait spent for nothing. The tracepoint is in sunrpc, so
  it comes with the module build of the tip. Neither the tracepoint
  nor `linger.bt` has run yet. With an older sunrpc, `linger.bt`
  fails to attach and leaves only its error in the file.

## TODO: 4-byte placement with `rx_buf_size` (work item; Mike, 2026-10-10)

**The most a loan can hope for on the E1 is 4-byte placement**, so
that a loaned WRITE goes direct to storage that takes arbitrary pieces
(NVMe with SGLs, NVMe/TCP). Header-data split and payload aggregation
are not coming from Xsight (above). **It matters on tardis1: sample
NVMe drives with SGLs are coming, and this has to be settled before
they are used for a loan.** The drives there today need whole pages
whatever the placement.

**`rx_buf_size=5440` breaks the placement `rx_ip_align=2` gave, and
it has to be revisited before any loan is run on these buffers.** The
driver says so at every open: "MTU 9000 spans 2 RX buffers; payload
after the first buffer is not 4-byte aligned".

- Every buffer is posted with the same headroom, 64 + 2 = 66 bytes,
  because any descriptor can be the first of a frame.
- In a frame's first buffer the payload starts at 66 + 54 = 120 (132
  with TCP timestamps): on a 4-byte boundary, which is what
  `rx_ip_align=2` is for.
- The rest of the frame starts at byte 66 of the next buffer: 2 past a
  4-byte boundary. So with two buffers to a 9000-byte frame, about
  half of every loaned WRITE's pieces fail the 4-byte gate. Only the
  offset fails: the lengths are multiples of 4 (5000 and 3960 bytes of
  payload in the two buffers; 4988 and 3960 with TCP timestamps,
  which both ends have on), and the three buffers of a page start at
  0, 5440 and 10880.
- With `rx_ip_align=0` it is the other way about (118 and 64): one of
  the two is off whichever is chosen, as long as both buffers are
  posted alike.

To work out, none of it started:

1. **Read, 2026-10-10: one misplaced piece sends the whole WRITE
   through the page cache.** `nfs_dio_split()`
   (`fs/nfs_common/nfs_dio.c`) walks every fragment of the payload in
   `nfs_dio_iter_aligned_and_splittable()` and returns at the first
   whose offset or length is not a multiple of the file's
   `dio_mem_align` (`(off | len) & (mem_align - 1)`). The split is then
   `NFS_DIO_MEM_MISALIGNED`: the payload is issued as one buffered
   segment, with `IOCB_DONTCACHE` since
   `nfsd_direct_misaligned_dontcache` is on by default, and
   `nfsd_write_dio_split` traces it. There is no partial path: the
   split divides a WRITE by file offset (a prefix, a direct middle, a
   suffix), never by which pieces of memory are aligned. Every loaned
   WRITE larger than one buffer has a continuation piece, so **on
   5440-byte buffers every loaned WRITE would be buffered**, not half
   of them.
2. Whether the driver can post a frame's later buffers at a headroom
   that is a multiple of 4. It does not know at posting time which
   buffer will be first; a short frame (an ACK, the tail of a burst)
   takes one buffer and shifts the order.
3. **The ask of Xsight, already mentioned to them (Mike,
   2026-10-10): that every buffer of a frame allows 4-byte
   placement.** It is the change that would help most, and a far
   smaller one than header-data split. One way to put it: the driver
   posts every buffer at a headroom that is a multiple of 4, and the
   card skips 2 bytes at the start of a frame only, so the IP header
   and the payload are placed in the first buffer and the rest of the
   frame starts on a boundary in the next. Today the 2 bytes are in
   the address the driver posts, so every buffer has them.
4. **The fallback that needs nothing, and is proven: 16 KiB pages
   with a whole page to a frame** (`rx_buf_size=0`), where
   `rx_ip_align=2` places every piece. Loans went direct that way on
   NVMe/TCP, the SGL drives' stand-in. It bloats the L3: 34 to 37 MiB
   of receive memory for the copy against 21 at 5440, and a loan
   holds more. So until 3 or 5 it is the loan on whole pages or the
   copy on 5440-byte buffers, by the storage.

5. **A frame that fits in one buffer** (thought of on reading the
   gate; run in copy mode the same evening: 43.2 and 43.3 GB/s, the
   best yet, and every frame one buffer. Run as a loan on NVMe/TCP the
   same night: every loaned WRITE was written direct). The misplacement is of a frame's second buffer, so
   it goes away if no frame has one. A buffer holds its size less 386
   bytes (66 of headroom, 320 of `skb_shared_info`): 5054 at 5440, and
   7806 at 8192, which is two buffers to a 16 KiB page. With
   `rx_buf_size=8192` and the server's MTU at 7792 (the client's can
   stay 9000: the server's MSS sets the segment size) every frame is
   one buffer placed by `rx_ip_align=2`. And it is denser than today:
   8192 bytes of buffer for 7752 of payload is 1.06 a byte, against
   10880 for 8960 (1.21) at 5440 and 16384 for 8960 (1.83) with whole
   pages. The price is 16% more frames for the same data. Worth a
   copy-mode cell for the footprint alone, before any loan.

Copy mode is not affected: nfsd copies the pieces out and its own
buffer is what is written.

## A known gap in the page swap: pages a socket keeps (work item)

Since the page swap (`47f13c685c5b5`), a connection keeps the pages a
thread gave it in exchange for a parked record. That is up to the
largest record that changed hands on it, about 1 MiB for nfsd, and it
keeps them until it closes. 16 MiB for this rig, about 16 GiB for a
thousand nconnect=16 clients. They are freed at close, so nothing
leaks, but nothing bounds the total. The details and three ways to
bound it are under "sunrpc, the pages a socket keeps after the page
swap" in `../XEU_CONVERGENCE.md`'s "Since then". It does not affect
this rig's measurements. It has to be bounded before the page swap
goes to production or upstream.

## The page-pool work items: what was done (2026-10-09, evening)

T1 and T2 in `../XEU_CONVERGENCE.md` suggested three things for a pool
bound to the region. "Since then", after that document's list of
suggestions, has the account.

| item | state |
|---|---|
| a LIFO in place of the pool's FIFO ring | done: "page_pool: Recycle a region-bound pool's pages last in, first out", in build 462; run only in copy mode, with other changes, never alone |
| a much shorter ring (`xeu`'s `pool_size`) | not done; the LIFO was chosen instead. `rx_refill_thresh` is another knob |
| the DMA mapping kept while a page is in the region | not done; an overflowing page is still unmapped, put on the region's list and mapped again |

**What decides the rest is the loan cell, rerun on the fixed sunrpc**
(step 4 of the crash section). Read `rx_pp_recycle_ring_full` and
`alloc_slow` in `pp-*.txt` against T2's 4.06 million and 254 thousand
in 10 s. Near zero means the LIFO and the rest of the stack have dealt
with it. Still near 9% of returns means keeping the mapping on region
pages is the next change. It is in `net/core` and needs a kernel
build.

## Receive buffers smaller than a page: `rx_buf_size`, and its A/B

The work item, with the driver's code as it is, the change and the
sizes, is "Receive buffers smaller than a page" in
`../XEU_CONVERGENCE.md`'s "Since then". In short:
- `xeu` gets a load-time `rx_buf_size`;
- each receive descriptor takes one piece of a page-pool page, built
  in place as today;
- at 5440 bytes (5376 once rounded down to the page pool's 128-byte
  alignment) a 9018-byte frame takes 2 pieces, and a 16 KiB page holds
  3, so 2/3 of a page per frame where today it is a whole one.

**Written, built, not run** (9 October, evening):
`kernel-rpmtree/xsight/xeu-cut-RX-buffers-smaller-than-a-page-with-rx_buf_size.patch`,
Patch6 of `xsight.spec`, Release 6 (`kernel-rpmtree` 8c58863, on its
`xsight-WIP3` branch). The spec's `%prep` applies Patch0 to Patch6 in
order. Every object of the module that this host's 4 KiB config builds
compiles clean (`xeu_ipsec.c` and `xeu_psp.c` need config this host
lacks), with no new W=1 warnings. Besides the pieces, it counts the
whole frame (MTU plus Ethernet and VLAN headers) when it tells the
firmware how many buffers a frame may take, where the MTU alone could
come out one short. The open logs the size in use. To build: `build-
kmod.sh` (Release 6 has both patches); a size is set with `modprobe
xeu rx_buf_size=5440`.
Lowering the MTU tests nothing: the driver posts a whole page whatever
the frame. 9000 is the driver's maximum MTU, so it cannot be raised
either.

**The A/B.** The cell is `plans/16k-26-copybest.plan`'s copy cell, on
the whole stack with the fixed sunrpc, `BALANCE` on. Each
`rx_buf_size` is a reload of `xeu`. That takes `enp164s0f0` down for
a few seconds, so each reload is Mike's.

| `rx_buf_size` | ring entries | frames the ring holds | held receive buffer | the same data window |
|---|---|---|---|---|
| 0 (a page, the control) | 256 | 256 | 1 MiB | yes |
| 5440 (5376) | 512 | 256 | 672 KiB | yes |
| 4096 | 1024 | about 341 | 768 KiB | yes; more frames posted |

Every row holds the same frames and the same data in the receive
buffer. So what changes is the memory per frame, and what that does
to the rate. 672 and 768 KiB are 1 MiB scaled by each size's truesize
per frame (10752 and 12288 bytes against 16384). The ring size has to
be a power of two, so 4096 cannot be held at 256 frames.

The order: load 5440 and run the cell twice; load 4096, twice; load
the default, twice, as this boot's control. Six runs at about 3 TB
each is 18 TB on the stripe; trim before each. After that, if 5440
wins, run it once at the old setting (256 entries, 1 MiB) as well.
That is half the frames posted and more data in the buffer, to see
what the denser buffers allow and not only what they cost.

**What decides it:**
- the rate, at equal frames and equal data window;
- `used_pages`, expected near 2/3 of the control's at 5440;
- `memmove`'s share of cycles, and calls per WRITE (`handoff.bt`);
- CPU-seconds per GB, which carries the cost of twice the
  descriptors and of smaller GRO skbs;
- ring drops and `rx_pp_*`.

If 5440 is not faster and `memmove` is no smaller, the page per frame
is not what holds the copy, and the default stays. The size of a GRO
skb is not measured by anything here. If the CPU per GB rises, that
is the first thing to instrument.

## How the flows fall on the rings: three work items

The largest source of spread between runs of one cell is which ring
each flow hashes to. Build 462's slow run had one ring at 19.9 GB/s
and 440 thousand drops. **`xeu` supports no steering**: `ethtool -x`
and `-X` and `-n rx-flow-hash` were checked on build 461 and are not
there, and its one ntuple rule can only target a VF. The SDK built
into the module does have an RSS redirection table and key
(`xsl_rm_reta_set()`, `xsl_rm_rss_hash_set()`), which `xeu` does not
expose ("`xeu` gives no way to steer" and "rings, how the flows
fall" in `../XEU_CONVERGENCE.md`). So no `ethtool` rule pins flows
here yet. The
work items, all three also in that document's "Since then" list:

1. **More flows than rings.** At 64 connections the rings came out
   even (28, 27, 20 and 25%), where sixteen gave 25, 6, 31 and 37%.
   The 64-connection test below will show it again.
2. **Mount again until the rings are even: `BALANCE=<percent>`** to
   `run-budget.sh`, and so to `sweep.sh` and `trace-sweep.sh`. It
   applies to the hardware client's step cells with `STEP_JOBS`.
   Before the cell is measured, the harness:
   - mounts and runs the cell's own writers for `BALANCE_SECS`
     (default 4), on files of their own, deleted after;
   - reads each ring's `rx_bytes` from `ethtool -S`;
   - if the busiest ring took more than `<percent>`, unmounts and
     mounts again. Each mount gets new client source ports, so new
     hashes.

   It tries at most `BALANCE_TRIES` mounts (default 6). If none comes
   within the limit, the cell runs on the last mount and says so.
   Each try is a line of `<cell>/balance.txt`: the busiest ring's
   share, every ring's share, and the client's source ports. A try
   whose ports are the same as the one before is marked, since then
   remounting changed nothing. The tracers' 35 s count from the
   cell's start, after the balancing, as before. `geometry.txt` names
   the setting.

   For four rings `BALANCE=35` is a guess to start from: an even fall
   is 25%. Build 462's busiest ring carried 19.9 GB/s in the slow run
   and 14.5 in the fast one; the shares were not read. It does not make the
   flows even. It stops one bad fall from deciding a cell. **Not run
   yet:** that the client's ports change with each mount, and how
   many tries a balanced fall takes, are the first things to read.
3. **Steering in `xeu`**: a kmod change. **The first of it is
   written** (9 October, evening; built, not run):
   `kernel-rpmtree/xsight/xeu-expose-the-RSS-redirection-table-hash-key-and-function-to-ethtool.patch`,
   Patch5 of `xsight.spec`, Release 6 (`kernel-rpmtree` 799acf5,
   on its `xsight-WIP3` branch). It wires `ethtool -x`/`-X`
   to the SDK's existing calls:
   - the redirection table, sized by the new load-time
     `rss_indir_size` (default 0 means no table, as before; at most
     255);
   - the 68-byte key;
   - the hash function, of which only Toeplitz can be set. Spooky,
     the firmware's default, shows as unknown.

   It also adds `get_rx_ring_count`, without which 7.1's `ethtool` core
   refuses any table. What `ethtool -X` set is programmed again after
   every reconfiguration of the port.

   It applies at `-p0` after Patch0 to Patch4, and its four files
   compile with no new warnings (also at W=1) against the 7.1.13 tree.
   The whole module could not be built here: the vendor's
   `xeu_ipsec.c` needs XFRM offload, which this host's 4 KiB config
   lacks.

   **To try it on tardis1** (`build-kmod.sh` builds Release 6;
   loading it reloads `xeu`, so it is Mike's):

       modprobe xeu rss_indir_size=128     # after rmmod xeu
       ethtool -x enp161s0f0               # the firmware's table
       ethtool -X enp161s0f0 hfunc toeplitz
       ethtool -X enp161s0f0 weight 1 0 0 0   # everything to ring 0
       ethtool -X enp161s0f0 equal 4

   Read the first `ethtool -x`. If it fails, the warning in `dmesg`
   names the first entry that is not one of the port's rings: that is
   the end of the firmware's own table, or its handles are not the
   rings'. After `weight 1 0 0 0`, a short load should land on ring 0
   alone (`ethtool -S` ring counters as deltas). If it does not, the
   hardware indexes fewer entries than were set, so try sizes down to
   the ring count. The rest is the questions in
   `../xsight-steering-email.txt`.

   Still to ask for, in order:
   - the RSS indirection table and key (`ethtool -x`/`-X`). It is the
     cheapest, works with standard tools, and with the key readable it
     places a flow by moving its table entry;
   - ntuple filters (`ethtool -N ... action <ring>`), for two hot flows
     in one entry;
   - interrupt coalescing (`ethtool -C`). It is not steering, but it is
     missing too, and `enable_irq` was 6.1% of samples.

   aRFS and RFS would not help: sunrpc reads with `tcp_read_sock()`,
   which does not record a flow's CPU. The reasons, and the questions
   for Xsight (is there a table, how large, Toeplitz and its key,
   filters, moderation), are under "rings, how the flows fall" in
   `../XEU_CONVERGENCE.md`'s "Since then".

Without `BALANCE`, read the busiest and least busy ring of every run
(`where-*.txt`) before comparing rates. Set aside a run whose busiest
ring is far over a quarter, or say so with its figure.

## Does `pools_per_node` earn its place: 64 connections, 1 pool against 4

"SUNRPC: Let a NUMA node have several thread pools" was written for
T6 in `../XEU_CONVERGENCE.md`. With 64 connections and the one pool,
512 threads waited 5.25 s in 10 s on its queue of ready connections
(`__lwq_dequeue`). The build 462 runs had four pools but only sixteen
connections, and at sixteen that lock is not near the top of the list
on either build (S6). So nothing yet says the patch does its job. The
test is the load that showed the problem, run with 1 pool and with 4.

- **The cell:** the copy at its best setting, rings of 4 x 256 and a
  1 MiB held buffer (`plans/16k-26-copybest.plan`), with
  `STEP_MOUNTS=4`. That is four addresses of the server on
  `enp161s0f0`, each with nconnect 16, so 64 connections; the harness
  adds the addresses for the cell and removes them after. The rest of
  the stack is as for build 462: linger 50, `rx_refill_thresh` 8,
  deferred frees at 128. T10's 64-connection copy (21.1 GB/s) was on
  rings of 4 x 128 with a 256 KiB buffer and dropped 190 thousand
  frames, so it is not the figure to compare with. The comparison is
  between the two runs here.
- **The order:** after the sunrpc from the tip is in place (the crash
  section above). Changing the pool count is
  `systemctl stop nfs-server`, `echo <n> >
  /sys/module/sunrpc/parameters/pools_per_node`,
  `systemctl start nfs-server`, `echo 512 > /proc/fs/nfsd/threads`,
  then check `cat /proc/fs/nfsd/pool_threads` (n pools). The value is
  read only when nfsd's service is created. **Stopping nfsd drops
  every client, including the other one on `enp164s0f0`, so each
  change is Mike's.** Run 4 pools, then 1, then 4, then 1. At about
  3 TB a run that is 12 TB on the stripe; trim before each run
  (`TRIM=`).

      STEP_MOUNTS=4 RX_LINGER_US=50 PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/xeu-16k-<build>-m4-pools<n>-<k> plans/16k-26-copybest.plan

- **What answers it:**
  - In `perf lock contention`, `__lwq_dequeue`'s contended count and
    time waited with 1 pool against 4. 4 should cut it by about the
    number of pools. If it does not, the patch is not doing its job.
  - `/proc/fs/nfsd/pool_stats`, saved by hand before and after the
    sweep (the plan is one cell; the harness does not save it). Each pool's
    packets and wakeups show whether the 64 connections spread over
    the four pools. The pool is picked by a hash of the transport, so
    expect an uneven split, not an even one.
  - Then the rate, CPU-seconds per GB, drops and RTOs.
  - Judged as one change of many (see "How to judge a change here"):
    if the lock's wait time goes away and the rate does not move, the
    patch still did its job, and something else is the limit.

The state of tardis1 (it does not outlive a reboot, except where said):

- Kernel `7.1.13-21.hs.462.el9.aarch64+16k`. **sunrpc is the module
  built from the tip, in `/lib/modules/$(uname -r)/updates/sunrpc.ko`**
  (srcversion `988D31FBE6BF6DC88282CB6`; the kernel's own
  `sunrpc.ko.xz` is where it was). `svcsock_kunit.ko` and
  `xdr_kunit.ko` built with it are beside it. They stay across a
  reboot of this kernel; `pools_per_node` does not, being given to
  `modprobe` by `swap-sunrpc.sh` and in no file of `/etc/modprobe.d`.
- `pools_per_node` 4 (four pools of 128 threads; chosen by the
  session, Mike has not said 4 or 1), `svc_tcp_rx_linger_us` 0 until a
  run sets it, nfsd at 512 threads.
- `kmod-16k-xsight` 2.0.1-5 for build 462 is installed and loaded
  (the kmods of builds 459 and 461 went with the upgrade);
  `rx_refill_thresh` is **8**, `rx_ip_align` 2.
- A 32768-page region is bound to `0000:a1:00.0`, rings of 4 x 256,
  `alloc_fail` 0. `/dev/md0` is mounted at `/hs/stripe` by hand.
- This boot wrote about 12 TB to the stripe. No client other than the
  harness's was connected at any check.

How the module was put in: `./build-sunrpc.sh` (it now gives the
module BTF, without which `nfsdlat.bt` and `handoff.bt` do not
attach), then `POOLS_PER_NODE=4 ./swap-sunrpc.sh`, which stops the NFS
services, unloads sunrpc and what depends on it, installs and starts
them again. `../run-kunit.sh` wants nfs-server stopped and leaves it
so: afterwards `systemctl restart hs-nfsd-blksize; systemctl start
nfs-server; systemctl restart hammerspace-nfsd-tuning` and set the
threads. The module build warns that the compiler is not the one the
kernel was built with.

What is left open, by what the runs showed: how the sixteen flows fall
on the four rings (the one slow run of three had a ring at 19.9 GB/s
and 440 thousand drops); the linger's busy wait, 14% to 17% of cycles;
`memmove`, still a quarter of them; a 9000-byte frame with 16 KiB to
itself (now a work item: "Receive buffers smaller than a page"
above). The driver's `rx_refill_thresh` patch is Release 5 in
`kernel-rpmtree` (3bc8f82); Release 6 adds the RSS and `rx_buf_size`
patches (799acf5, 8c58863). Mike pushed them to the `xsight-WIP3`
branch of the kernel-rpmtree remote; `main` there does not have them.

**The footprint against the L3 is the next lever for the copy.** Build
462 used 2052 to 2291 region pages, 32 to 36 MiB, which is the whole
L3. On build 461 the copy's time was set by how much of the region was
in use: a 1 MiB WRITE's calls took 224 us with 24 MiB in use, 269 with
32 and 357 with 48, and `memmove` rose from 24% of cycles to 38%
("The copy with a larger held buffer", F2). With `memmove` still 25%
to 27% of cycles, whatever keeps the pages in use under 32 MiB should
show directly in the copy's time. First, rings of 4 x 128 on the
whole stack, against 4 x 256. On build 461 that ring size used
24 MiB and ran at 35.6 GB/s, but dropped 1.5 million frames. On build
462 the drops at 4 x 256 fell from 250 thousand to about 3 thousand,
so what to read is whether 4 x 128 now holds without dropping. The
cell is `plans/16k-27-copy128.plan`, run the same way as
`16k-26-copybest.plan`, two or three runs of each: how the flows fell
onto the rings moved build 462's runs by 4% on its own.

**The linger's 14% to 17% of cycles in `arch_counter_get_cntpct` is
the busy wait itself, not an extra cost of reading the clock.** The
wait loop in `svc_tcp_rx_linger()` calls `ktime_get_ns()` on every
pass, to check the deadline. On arm64 that reads the counter as
`isb; mrs cntpct_el0` followed by `arch_counter_enforce_ordering()`: a
pipeline flush and a dependency on the value. The rest of a pass is
two bit tests, `need_resched()` and a `yield`, so nearly every sample
taken while a thread lingers lands in the counter read. A session
suggested reading the clock only every N passes to win that back
(2026-10-09, in chat only). That would not work: the samples would
move to the bit test and the `yield`, and the core would stay held
until data came or the time ran out. What costs CPU is how long a
thread lingers, which is why CPU per GB went from 0.49 to 0.52.
Three ways to reduce it, none written or run:

- **A shorter linger.** A piece is about 13 us in coming, and 50 was a
  first guess. Planned: "Is the linger worth its CPU in copy mode"
  below.
- **Sleeping instead of spinning.** The thread sleeps until
  `svc_data_ready()` wakes it, with a timer bounding the wait. One
  thread still receives the record and no core is held, at the cost of
  a wakeup's latency for each piece.
- **`WFE` instead of `yield`** (`smp_cond_load_relaxed()` on the
  flags word). The core idles more cheaply until the flag's cache
  line changes, but it is still held for that thread.

## Before the reboot into build 462 (2026-10-09, 17:30)

**Read this section first; the handoff below it was written before the
16 KiB boot and is kept for the rig's rules and its scripts.** The
results are in `../XEU_CONVERGENCE.md`: "16K" (sizing), "Hero" and what
follows it (the fast runs), **"Correction, 2026-10-09"**, which
changes how every native figure from the stripe is read, "The last
runs on build 459", and **"Build 461"**, the first runs on the kernel
with the stacking-alignment change.

**From 14:30 on 9 October Mike's direction is copy mode. Start at "The
boot after build 461" just below.** Steps 4 and 5 of build 461's list,
further down, were not started and are not the priority.

## The boot after build 461: start here

Mike built a kernel from `origin/…-WIP3` (its tip is "AGENTS.md: a
commit message carries no disclaimer about the work's state") and is
installing it. **It has three of the five code patches written on
9 October; the other two are sunrpc's alone and can be built as a
module** (the table in "The patches" below).

What 9 October found, in the order it matters for copy mode
(`../XEU_CONVERGENCE.md`, from "Build 461" to "The copy with a larger
held buffer"):

1. On build 461 nfsd's copy on the stripe is written direct.
2. **The harness had deferred skb frees off in every run before 15:30
   on 9 October**; they are on now (`SKB_DEFER_MAX`, default 128).
3. At a 256 KiB held buffer a 1 MiB WRITE is received in 17 calls, a
   GRO skb at a time, by a different thread nine times in ten, and
   its connection is copied from for 44% of the time.
4. **With a 1 MiB held buffer the copy carries 35.6 GB/s on rings of
   4 x 128 and 35.9 on 4 x 256** (27.7 at 256 KiB; the loan 30.3).
5. With the waiting gone the copy's own speed is the limit and it
   falls as the region in use passes the 32 MiB L3: rings of 4 x 512
   drop nothing and carry 32.8. Whatever has a page reused sooner or
   fewer pages idle is worth rate.

**A change is one of many** (Mike; the paragraph "How to judge a
change here" below). Keep what does its job and measure them
together.

On the new kernel, in order. **Everything goes in first and the whole
is measured (Mike, 2026-10-09): the two sunrpc patches and the
driver's parameter are loaded before any cell is run, not after the
kernel's three have been measured alone.** All three of those are
off until set (`svc_tcp_rx_linger_us` 0, `pools_per_node` 1,
`rx_refill_thresh` 32), so the kernel's three patches by themselves
are still one run away, with nothing to unload. And straight after a
boot is the cheapest time for both swaps: no region is bound, and the
clients have to be dropped once and not twice.

0. `cat /sys/block/md0/queue/dma_alignment` is 3 and `enp161s0f0` has
   its address. **Run nothing on veth.**
1. **The sunrpc module.** `./build-sunrpc.sh` builds `sunrpc.ko` from
   this tree for the running kernel (it needs that kernel's `-devel`;
   a minute) and says whether nfsd can load against it as it is: no
   export missing and none with another version. It builds
   `svcsock_kunit.ko` beside it. **Putting it in place stops nfsd,
   unmounts every NFS mount and drops every client, the other client
   on `enp164s0f0` too: ask Mike, and keep the installed
   `sunrpc.ko` beside the new one.** If `pools_per_node` is wanted
   for the first runs, set it before nfsd starts again
   (`echo 4 > /sys/module/sunrpc/parameters/pools_per_node`;
   `cat /proc/fs/nfsd/pool_threads` then shows four pools).
2. **The driver.** The refill threshold is Patch4 of
   `kernel-rpmtree/xsight/xsight.spec`, uncommitted ("The driver's
   refill threshold is a module parameter" below). If Mike's build
   did not carry it, `T=$L/kmod-xsight-16k-build-r5 ./build-kmod.sh`
   builds release 5 for the running kernel; installing it and
   reloading `xeu` takes `enp164s0f0` down for a few seconds and is
   Mike's (the commands are in the handoff further down, with 5 for
   4). Then: `grep -c xeu_rx_refill_timer /proc/kallsyms` is 1,
   `rx_ip_align` is 2, `rx_refill_thresh` is 32, both ports have their
   addresses.
3. `mount -o noatime /dev/md0 /hs/stripe` (`mdadm --assemble --scan`
   first if the array is not there), `./bringup.sh 32768 256`,
   `echo 512 > /proc/fs/nfsd/threads`.
4. The KUnit suites, `svcsock_kunit` above all: the page swap changed
   `svc_tcp_save_pages()`, `svc_tcp_restore_pages()` and
   `svc_tcp_clear_pages()` (`../TESTING.md` has how they are run).
   Before any load.
5. **The whole stack**, the copy at its best setting, traced, three
   times (about 3 TB each; the stripe is trimmed before each by
   `TRIM=`):

       echo 8 > /sys/module/xeu/parameters/rx_refill_thresh
       RX_LINGER_US=50 PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/xeu-16k-<build>-stack1 plans/16k-26-copybest.plan

   **Load every tracer at idle first** (`timeout -s INT 3 bpftrace
   nfsdlat.bt`, the same for `handoff.bt`; `handoff.bt` has
   `XPT_QTIME_OFFSET`, which is of the kernel, and says how to read
   it): a cell was thrown away on a tracer that had not loaded.
   Against build 461's one traced run of that cell: 35.9 GB/s, 0.49
   server CPU-seconds per GB, 2054 region pages, 6.4 calls for a
   WRITE, 269 us in them, `memmove` 30% of cycles, 250564 frames
   dropped. What shows each change is doing its job: in `pp-*.txt`,
   `rx_pp_recycle_ring_full` near zero and `rx_pp_alloc_refill`
   taking what `rx_pp_recycle_cached` does not (the stack);
   `__folio_batch_release` and `alloc_pages_bulk` gone from the
   profile (the page swap); in `handoff-*.txt`, calls for a WRITE
   near 1 and the next call by the same thread (the linger, which is
   a busy wait: read the CPU for each GB with it); `__lwq_dequeue`
   down the contended list (the pools). The values 50 and 8 are first
   guesses: a piece of a WRITE is about 13 us in coming.
6. Then what each is worth, by taking one away at a time from the
   whole: `RX_LINGER_US=0`, `rx_refill_thresh` 32, `SKB_DEFER_MAX=0`;
   `pools_per_node` needs nfsd restarted.
7. Not run on any kernel: a load paced at the source (`STEP_RATE`,
   below); 64 connections at a 1 MiB buffer (planned: "Does
   `pools_per_node` earn its place" above); a 2 MiB buffer; rings of
   4 x 128 against 4 x 256 (why it matters now: "The footprint
   against the L3" at the top).

`smartctl` is not installed, so the drives' wear is not read; 9 October
wrote about 90 TB to the stripe, 15 TB a drive.

**Mike rebuilt the kernel from the tip on 9 October and rebooted into
it (build 461). A session has no memory of the days before it:
everything known is in these files and in
`/var/tmp/zc-run-logs/` (which is on the root file system and
survives).** Commits here end with `Assisted-by:` and a `Signed-off-by:`
of the configured git identity (`git commit -s`), never
`Co-Authored-By` (AGENTS.md, "Commit trailers"). Mike corrected that
once already.

## The state of tardis1 now (build 461, 2026-10-09 13:30)

- Kernel `7.1.13-21.hs.461.loanpages_pool.el9.aarch64+16k`;
  `kmod-16k-xsight` 2.0.1-4 for it is installed and loaded (retry
  symbol present, `rx_ip_align` 2). It was checked, not rebuilt.
- A **32768-page** region is bound to `0000:a1:00.0`; the port is at
  4 rings of 256. `alloc_fail` 0.
- `/dev/md0` assembled by itself and is mounted at `/hs/stripe`
  (`mount -o noatime`, by hand: it is still not in `/etc/fstab`).
  Its `dma_alignment` is 3.
- nfsd is at **512** threads (the boot's own count is 128).
- `enp164s0f0` came up with the boot and has its address. It is not
  ours and was not touched. Nothing has run on veth.
- `smartctl` is not installed on this root, so the drives' wear was
  not read. This boot has written about 18 TB to the stripe.

## The state of tardis1 before the reboot (build 459)

A reboot undoes some of this: no region is bound, the port is back at
its 8 rings of 1024, nfsd at its own thread count, `enp164s0f0` is
probably up again (leave it; it is not ours), `/hs/stripe` is not
mounted (it is not in `/etc/fstab`) and the array may come up as
`/dev/md127` and not `/dev/md0`.

- Kernel `7.1.13-21.hs.459.loanpages_pool.el9.aarch64+16k`, `xeu` with
  the refill retry (kmod 2.0.1-4), `rx_ip_align` 2.
- `enp164s0f0` is **down** (Mike, 2026-10-08) and every other mount of
  tardis1 was torn down. Both wait for him.
- A 6144-page region is bound to `0000:a1:00.0`. It cannot be resized
  on this kernel. The port was left at 4 rings of 256.
- **`nvme2` to `nvme7` are an MD raid0, `/dev/md0`, XFS at
  `/hs/stripe`** (1 MiB chunk; not in `/etc/fstab`; made with
  `mdadm --create /dev/md0 --level=0 --chunk=1024 --raid-devices=6`).
  What they held is gone. Their lines in `/etc/fstab` and `/etc/exports`
  are commented out with `#stripe 2026-10-08-stripe#`; the files as they
  were are `/etc/fstab.2026-10-08-stripe` and
  `/etc/exports.2026-10-08-stripe`. `mdadm` was installed. `nvme0` and
  `nvme1` are as they were.
- The drives took about 20 TB each on the night of 8 to 9 October
  (3% to 4% of their endurance used; `xeu-16k-smart-*.txt` in the run
  logs). **Trim the stripe before a run** (`TRIM=/hs/stripe` to
  `sweep.sh`): untrimmed it gives up a fifth.
- nfsd is back at 128 threads; the drives' and `md0`'s `write_cache`
  are back at `write back`; `null_blk` is loaded and unused.

## What was found, in one place

1. Sizing (8 October): rings of 4 x 128, a 256 KiB buffer and a budget
   of four WRITEs are the smallest setting that kept up, at 73% of the
   L3, under a load that the client's verifying fio thread held to
   2.2 GB/s. Use `STEP_JOBS` for anything about rate.
2. On the stripe, durable, three runs each: **the loan 30.1 GB/s, the
   copy 25.4** (sixteen connections, 8 MiB writes, 512 nfsd threads;
   the loan at 16 WRITEs, 4 x 256, 1 MiB; the copy at 4 x 128,
   256 KiB).
3. **Neither was written direct.** `md0` asks for 512-byte memory
   alignment where its drives ask for 4, so nfsd's copy goes through
   the page cache there as a loan does: two copies against one.
4. The wire carries 49 GB/s to sixteen plain streams with a 1 MiB
   buffer held. Nothing on the network side holds the loan at 30: not
   the budget past 16, the rings' depth or number, dropped frames,
   connections or files.
5. **Build 461: the copy on the stripe is written direct, and carries
   27.7 GB/s (27.3 to 28.1) against the loan's 30.3 (30.2 to 30.4),
   three runs each.** The loan still goes through the page cache. The
   direct copy costs the server less CPU per GB than the loan (0.48
   against 0.54 CPU-seconds) and holds 770 region pages against 4100.
6. **Traced on build 461** (after the call with Xsight; "Build 461,
   traced" in `../XEU_CONVERGENCE.md`): the pools reuse pages through a
   FIFO and not last in, first out; the rings take 6% to 42% each of
   sixteen flows; a loan through the page cache queues on each file's
   inode lock; the outliers in nfsd are COMMITs; 64 connections on
   rings of 128 made the copy slower (21.1 GB/s). Nothing was changed
   in the kernel for it. `trace-sweep.sh` and `nfsdlat.bt` are the
   tools.

## The patches (2026-10-09)

Five code patches at the tip, by subject. The first three are in the
kernel Mike built from `…-WIP3`; the last two are not:

| commit | what | which mode it is for |
|---|---|---|
| "SUNRPC: Trade pages with the socket when a partial record changes hands" | nfsd no longer frees and allocates a WRITE's pages each time a part-received record passes between threads (T3) | copy |
| "page_pool: Recycle a region-bound pool's pages last in, first out" | a stack in place of the FIFO ring for pools bound to the region (T1) | both |
| "SUNRPC: Return a loaned record's page-pool pages in batches" | the pool's lock is taken for each batch and not each page (T4) | loan |
| "SUNRPC: Let a NUMA node have several thread pools" | `pools_per_node`: several queues of ready connections where 512 threads shared one (T6) | both |

| "SUNRPC: Let a thread wait for the rest of a partly received TCP record" | `svc_tcp_rx_linger_us`: a WRITE is received by one thread, which waits for the next piece and does not park the record (W3) | both |

The last two were written after Mike's build of `…-WIP3` began and are
not in it. The linger is off at 0; `RX_LINGER_US=50` to `sweep.sh`
sets it for a run (a piece is about 13 us in coming), and
`handoff.bt` shows whether it took: calls for each WRITE near 1 and
the next call by the same thread. It is a busy wait, so read the
server's CPU with it. `pools_per_node` does nothing until it is set, and that is read
when nfsd starts: `options sunrpc pools_per_node=4` in
`/etc/modprobe.d` before a boot, or
`echo 4 > /sys/module/sunrpc/parameters/pools_per_node` and a restart
of nfsd, which drops every client's connections and so is Mike's to
do. `cat /proc/fs/nfsd/pool_threads` then shows the pools.

`net/core/page_pool.c` is built in, so they need a kernel build and a
reboot. `svcsock_kunit` covers the save and restore of pages; run it
on the new kernel.

**The testing is of copy mode (Mike, 2026-10-09).** The aim is the
shortest time from a frame landing to its buffer being back in the
pool, by making the copy and the direct write as efficient as they
will go; that path is also what a loan to a drive that takes
arbitrary pieces uses. Each iteration is the copy cell alone,
`plans/16k-23-copy.plan`, which halves what an iteration writes to
the stripe (about 3 TB):

    PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/<outdir> plans/16k-23-copy.plan

The figures to beat, build 461 with sixteen connections and 512 nfsd
threads: **35.9 GB/s with a 1 MiB held buffer on rings of 4 x 256**
(`plans/16k-25-copybuf-rings.plan`, its first line; 0.49 server
CPU-seconds per GB, 2054 region pages, 250 thousand frames dropped),
and at the old setting of 256 KiB and 4 x 128, 27.7 GB/s (27.3 to
28.1 untraced, 27.7 traced), 0.48 server
CPU-seconds per GB, the client `rwnd_limited` 98.5%, `memmove` 16% of
cycles, page frees and allocations about 5%. What to read on the new
kernel: `rx_pp_alloc_refill` against `rx_pp_alloc_empty` in
`pp-*.txt` (the stack in use), `__folio_batch_release` and
`alloc_pages_bulk` gone from the profile, and the rate.

**The driver's refill threshold is a module parameter in
`kernel-rpmtree`, uncommitted (2026-10-09).** `xsight/xeu-make-the-RX-refill-threshold-a-module-parameter.patch`
is Patch4 of `xsight.spec`, whose Release is 5: `rx_refill_thresh`,
default 32 (as the driver is), the empty slots at which a poll refills
the ring, writable while the port is up. It is not committed there
because that repository has no git identity configured for this user
and its AGENTS.md forbids taking one from the history; the spec has no
changelog entry for the same reason. `./build-kmod.sh` built it
against build 461 into `$L/kmod-xsight-16k-build-r5` (the parameter is
in `modinfo`); nothing was installed or loaded. A new kernel needs the
kmod built against it, and loading it reloads `xeu`, which is Mike's
to do. After that
`echo 8 > /sys/module/xeu/parameters/rx_refill_thresh` needs no
reload.

**How to judge a change here (Mike, 2026-10-09): as one of many, not
by whether it alone moves the rate.** A combination of changes can be
what unlocks the rate, and one stage's limit hides the gain of a fix
ahead of it. So a change that does what it is meant to (the counters,
the lock contention, the order of reuse, the cycles in the profile)
is kept and added to the others, and the whole is measured; "no gain
alone" is a result to record and not a reason to drop it or to leave
the next one untried.

**The harness turns deferred skb frees off** (`net.core.skb_defer_max=0`
in `run-budget.sh`, since 4 October, undocumented until 9 October), so
every figure from this rig before 15:30 on 9 October was taken that
way. **The harness's default is now 128, on (Mike);** `SKB_DEFER_MAX=0`
repeats the old behaviour, and a run whose `geometry.txt` does not name
the setting was made with 0. On, half of the copy cell's pages come back
by the pool's lockless cache and the rate is the same (27.3 and
28.0 GB/s): "The harness had deferred skb frees off" in
`../XEU_CONVERGENCE.md`. That is one change alone. The page-pool patch
above makes the other half of the pages, those that overflow the
32-page cache, last in, first out as well.

**How a WRITE is received in copy mode** (`handoff.bt`; "How a WRITE
is received" in `../XEU_CONVERGENCE.md`): 17 calls of
`svc_tcp_recvfrom()` for each 1 MiB WRITE, 89% of them by another
thread than the last, and the connection copied from for 44% of the
time; 38% is waiting for data behind a 256 KiB buffer and 18% is
waiting for a thread.

**The copy's best setting so far, build 461: a 1 MiB held buffer on
rings of 4 x 128 or 4 x 256, 35.6 and 35.9 GB/s** (one traced run
each; "The copy with a larger held buffer" in `../XEU_CONVERGENCE.md`).
Rings of 4 x 512 drop nothing and run at 32.8: once the waiting is
gone the copy's own speed is the limit, and it falls as the region in
use passes the 32 MiB L3 (a 1 MiB WRITE's copy takes 224 us with
24 MiB in use and 357 us with 48). Whatever has pages reused sooner
or fewer of them idle is worth rate from here.

Copy-mode things that need no build and are not run: fewer nfsd
threads (idle threads are woken last in, first out already, so this
may change nothing); 64 connections on rings deeper than 128; a load
paced at the source, for which the harness now has `STEP_RATE`:

    STEP_RATE="24000 27000 30000 33000" PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/<outdir> plans/16k-23-copy.plan

One figure (MB/s, all writers together) paces the whole cell; several
are steps of equal length within it, the same writers on the same
files, each step starting as the one before ends. The log and
`<cell>/ramp.tsv` have each step's rate against what it was offered,
and the cell's MBps is their mean. `where.sh` and the tracers sample
from 35 s in, so with four steps they see the second and third.

## This boot (build 461): steps 0 to 3 are done, 4 and 5 wait for Mike

Step 0 passed with no rebuild; step 1's `dma_alignment` is 3; step 2's
region is bound and the stripe trimmed; step 3 ran three times
(`$L/xeu-16k-461-best1` to `-best3`, driver
`$L/xeu-16k-461-best-driver.sh`) and is written up as "Build 461" in
`../XEU_CONVERGENCE.md`. **Steps 4 and 5 have not been started.** For
step 5, the region has been set once this boot and never resized.
`setsid` forks when started from a shell that leads its process group,
so the PID `$!` gives is not the sweep's: find it with `pgrep -xf` of
the exact command line, then `tail --pid`.

Three things were waiting for this kernel build:

| commit, by its subject | what |
|---|---|
| "page_pool: Give the fixed receive region back when it is resized" | the region is given back when its size changes with no pool bound |
| "block: Let a stacking device take its members' memory alignment" | a stacking device (every MD level, device-mapper) takes its members' memory alignment |
| (no commit) | a larger region: `./bringup.sh 32768 256` |

The steps, in order:

0. **The driver: check it, do not rebuild it.** Mike folded the Xsight
   changes he knows of (the refill retry among them) into
   `kernel-rpmtree`, so the kmod built with the new kernel should carry
   them (Mike, 2026-10-09). Check and go on:
   `grep -c xeu_rx_refill_timer /proc/kallsyms` is 1,
   `cat /sys/module/xeu/parameters/rx_ip_align` is 2, `rpm -q
   kmod-16k-xsight` names the running kernel, and `enp161s0f0` has its
   address. Only if one of those fails is the handoff's
   `./build-kmod.sh` and reload wanted, and then say so to Mike first
   (the reload takes `enp164s0f0` down for a few seconds).
   **Run nothing on veth.**
1. `cat /sys/block/md0/queue/dma_alignment` is 3 (it is 511 without
   the stacking change). If the array does not assemble by itself:
   `mdadm --assemble --scan`, then `mount -o noatime /dev/md0 /hs/stripe`.
2. `./bringup.sh 32768 256`, then `fstrim /hs/stripe`.
3. The copy on the stripe, and that it is written direct now
   (`wr_direct` in `summary.tsv`):
   `PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./sweep.sh $L/<outdir> plans/16k-17-best.plan`
   with nfsd at 512 threads (`echo 512 > /proc/fs/nfsd/threads`).
   That is the comparison the night was thought to be making: a direct
   copy against a loan through the page cache.
   Every cell of 120 s writes about 3 TB to the stripe, and the drives
   are consumer ones at 4% of their endurance: say so to Mike before a
   long queue. Start a queue from a driver script that waits on the
   one before it (`tail --pid`), and wait on the driver.
4. With the room the larger region gives: rings of 4 x 1024 or more
   with a 1 MiB or 2 MiB buffer and budgets of 16 to 64
   (`plans/16k-15-budget.plan` and `16k-20-deeprings.plan` are the
   shapes; none of their larger lines fitted in 6144 pages).
5. To test the region patch: with the port down,
   `echo 16384 > /sys/module/page_pool/parameters/rx_region_pages`, port
   up, and `/sys/kernel/debug/page_pool_rx_region` shows the new size
   and the kernel log the old region freed.

A loan written direct with no hop in front of it has no storage on this
host: these drives need whole pages, a memory-backed null_blk copies
under one lock (6 GB/s) and brd asks for 512-byte alignment. NVMe/TCP
on the same host is the only one, at about 20 GB/s.

## Scripts added on 8 and 9 October

`sweep.sh` takes `TRIM=`, `PRP_DIR=`, `SGL_BACKING=`, `PRP_STORAGE=`;
`run-budget.sh` takes `STEP_JOBS=`, `STEP_BS=`, `STEP_SIZE=`,
`STEP_IODEPTH=`, `STEP_MOUNTS=`, `EXPORT_SYNC=`. `capture-sweep.sh` runs
`where.sh` (both ends' CPUs, the rings, a profile, the client's view of
each connection) and `anat.bt` (what a loaned WRITE waits for) inside
every cell. `nfsd-stacks.sh` says where nfsd's threads are;
`wirebuf.sh` what the wire carries at each held buffer;
`nvme-baseline.sh` what the storage takes; `napi.bt` whether the
receive poll is held off (written, not yet used). **A sweep started
detached is not waited on by anything: start the next one from a driver
script, or it will sit idle** (it did, for two and a half hours).

---

# The xeu rig: 16 KiB handoff (written 2026-10-08 on the 64 KiB boot)

**A session on tardis1 after the reboot into the 16 KiB kernel starts
here.** Mike built the 16 KiB kernel himself and is installing it; this
file and the scripts beside it are everything the last session knew
that the tree did not.

## The job, and its priority (Mike, 2026-10-08)

Find the best sizing of the receive region against the L3 at 16 KiB, on
the real hardware: the `xeu` port `enp161s0f0`, the client
192.168.1.106, the real NVMe as it is (PRP, needs whole pages) and
behind an NVMe/TCP target (takes arbitrary pieces). **The full battery
of sizing runs is the top priority, and solid 16 KiB results are wanted
the same day.** Everything else (docs still owed, repeats of 64 KiB)
waits.

"Sizing" is four numbers and what they cost: the ring size, the held
receive buffer, the budget (in WRITEs of 1 MiB admitted at once), and
so the region pages in use at peak. The answer wanted is the smallest
footprint, against half of the L3 and the whole of it, at which sixteen
connections' loans run level with their copies with no stall and no
drop, on each storage.

## Rules that do not change (XEU_CONVERGENCE.md section 0)

- Load goes only to the harness's own mount of tardis1 on the client.
  Never to the client's other mounts, never to tardis1's other exports.
- (2026-10-11: `/hs/nvme0` and `/hs/nvme1` are gone, see "Two
  stripes" at the top.) Until then: only `/hs/nvme0` and `/hs/nvme1`,
  as files; no drive is reformatted.
- (2026-10-11, Mike: this rule is lifted. Both ports are ours, for
  runs on two ports at once. Check that no NFS client is connected
  before reconfiguring one.) Until then:
  only `enp161s0f0`. **`enp164s0f0` carries another client's
  (192.168.0.105) connections and is never touched, so `xeu` is never
  reloaded.** A reboot is Mike's to do.
- Commit at the tip of `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`; no
  regrouping (AGENTS.md).

## First ten minutes on the 16 KiB boot

1. `getconf PAGESIZE` is 16384 and `uname -r` ends `+16k`.
2. `cat /sys/module/xeu/parameters/rx_ip_align` is 2
   (`/etc/modprobe.d` has `options xeu rx_ip_align=2`).
3. **Is the driver's retry patch in?** `grep -c xeu_rx_refill_timer
   /proc/kallsyms` (1 is yes) and `rpm -q kmod-16k-xsight` (release 4
   carries it). The patch is `kernel-rpmtree` commit `f61ea4c` (not
   pushed); it was tested at 64 KiB on 2026-10-08 and works
   (`XEU_CONVERGENCE.md` 5c). **Mike expects it to be missing from the
   16 KiB kmod and wants release 4 rebuilt against the 16 KiB kernel by
   the same method (2026-10-08). Do this before `bringup.sh`:** with no
   region bound yet, the reload needs no care about the rings.

   1. `./build-kmod.sh` builds `kmod-16k-xsight` 2.0.1-4 for the
      running kernel from the `kernel-rpmtree` spec and patches, into
      `/var/tmp/zc-run-logs/kmod-xsight-16k-build`, and checks the
      result: the vermagic must be the running kernel's and "retry
      patch in xeu.ko" must be 1. It installs and loads nothing. About
      two minutes. It needs `kernel-16k-devel` and
      `kernel-16k-modules-core` of the running kernel; if either is
      missing, say so to Mike.
   2. Installing it and reloading `xeu` takes `enp164s0f0` down for a
      few seconds and drops the other client's connections, so **it
      needs Mike's leave, and the session's permissions may refuse it
      even then** (they did at 64 KiB until he gave the commands
      himself). Give him these to approve or to run, each as its own
      prompt starting with `!`:

          ! rpm -Uvh /var/tmp/zc-run-logs/kmod-xsight-16k-build/RPMS/aarch64/kmod-16k-xsight-2.0.1_*-4.el9.aarch64.rpm
          ! rmmod xeu && modprobe xeu

      `rpm -U` removes every other installed `kmod-16k-xsight`, since
      they share a name (at 64 KiB it took the kmods of builds 454 and
      455 with it). If a region is already bound, first
      `echo none > /sys/module/page_pool/parameters/rx_region_dev`: a
      fresh load gives the port 8 rings of 1024.
   3. Afterwards: `grep -c xeu_rx_refill_timer /proc/kallsyms` is 1,
      `rx_ip_align` is 2, both ports have their addresses, and
      192.168.0.105's connections are back
      (`ss -tn state established '( sport = :2049 )'`).

   Without the patch a ring that runs empty stays empty until the port
   is taken down and up; the scripts here bounce the port when the
   region runs dry and the patch is absent, so the sweep can run
   either way, but say which it was.
4. Nothing has made a region: `/sys/kernel/debug/page_pool_rx_region`
   does not exist. **Run nothing on veth this boot** (the veth harness
   would make the region for `rxsrv`).
5. `./bringup.sh 6144 128`: a 6144-page (96 MiB) region bound to the
   port's PCI device and rings of 4 x 128. One boot has one region
   size; 6144 has room for the widest line of the plans (rings of
   4 x 256, sixteen WRITEs, 512 KiB buffers). The region is only where
   the pages come from: what is measured against the L3 is the pages
   in use at peak (`used_pages`), which follows the rings, the buffers
   and the budget and not the region's size.

## Then, in this order

Everything long is started detached and waited on by PID:

    setsid nohup ./sweep.sh <outdir> <plan> > <log> 2>&1 < /dev/null &
    tail --pid=<pid> -f /dev/null      # as a background command

1. **`./ringsize.sh 64 128 256`** (about 6 minutes): a plain network
   load at each ring size, buffers free and held. It says the smallest
   ring that does not tail-drop. At 64 KiB that was 256 for one stream
   left to tune itself and 128 for sixteen.
2. **`./sweep.sh $L/xeu-16k-smoke plans/smoke.plan`** (3 minutes), and
   read its `geometry.txt`. **The harness's one-frame-per-page geometry
   has never run at 16 KiB.** Expected by arithmetic: 1 MiB is 64
   pages of payload and about 118 frames, so a factor near 185 and a
   charge near 120 pages a WRITE; a 128 KiB buffer is 9 pages. If the
   factor is far from that, stop and look (`FRAME_PAGE` in
   `run-budget.sh`). Then check that a budget "of n WRITEs" admits n:
   during a sixteen-connection loan cell,
   `sysctl net.sunrpc.region_budget_stats` should show `inflight`
   reaching n and no more. `sweep.sh` works the budget out as
   n x CHARGE + reserve + 4; if `inflight` tops out below n, CHARGE is
   low (trace one cell with `credit.bt`: `ADMIT ... credit=` is what
   the kernel charges).
3. **`plans/16k-1-core.plan`** (1 h 50 min; the lines are in order of
   priority and a stopped sweep resumes). Rings of 4 x 128, both
   storages, buffers of 256, 128 and 512 KiB, budgets of 1, 2, 4 and 8
   WRITEs, with each buffer's copies.
4. **`./summarize.py <outdir>`** prints the sizing table: copy and loan
   at one and sixteen connections, the pages in use and their share of
   the L3, CPU per GB, and a note where a cell stalled, dropped, paused
   or ran the region dry.
5. **`plans/16k-2-rings.plan`** (70 min) at the buffer the core showed
   best (edit the plan's buffer first), then **`plans/16k-3-wide.plan`**
   (30 min).
6. **Repeats of the shortlist.** A sweep skips lines listed in its
   `done` file, so a repeat is the same plan into a new outdir;
   `summarize.py` takes several outdirs and shows the median and range.
   `PERF=1` adds cycles and instructions per cell; `PROFILE=1` a 30 s
   flat profile by function per cell (not both).
7. Record as it comes in a "16K" section of `XEU_CONVERGENCE.md` shaped
   like its "64K" one, and tell Mike the table.

## What 64 KiB showed, to set 16 KiB against

The account is `XEU_CONVERGENCE.md` section "64K" (18 numbered
findings). In short, at 64 KiB on this card:

- a 1 MiB WRITE was about 117 pages, 7.3 MiB; rings that did not drop
  were twice the L3; no working setting fitted in half of the L3 (the
  best cells peaked at 1500 pages, 94 MiB);
- sixteen connections on a 128 KiB buffer (two frames) and a budget of
  one WRITE stalled, 9 untraced runs of 9, through TCP backoff on the
  client; a 256 KiB buffer or a budget of four WRITEs ended it;
- with a 512 KiB buffer and a budget of six WRITEs the loan and its
  copy were level, about 1.4 to 1.5 GB/s at one connection and 2.0 to
  2.3 at sixteen; one cell did not repeat (1979, then 569);
- no saving in server CPU showed for the loan (0.84 against 0.84
  kernel CPU-seconds per GB on NVMe/TCP, 0.35 against 0.45 on the local
  NVMe);
- baselines: the drive 10.4 GiB/s, the wire 7.0 GB/s to one connection
  left to tune itself, 3.9 GB/s to sixteen at 128 KiB, 0.9 to 1.2 GB/s
  to one at 256 KiB.

At 16 KiB a frame still has a page to itself, but the page is a
quarter the size: a WRITE is about 1.8 MiB of region, the L3 is 2048
pages, and a 128 KiB buffer is eight frames. Whether the stall, the
level rates and the misses change with that is what the plans show.

## What went wrong before, so it does not again

- **`pgrep -f` and `pkill -f` match the shell that runs them.** A queue
  of jobs each waiting for "no `run-budget.sh` process" waited on
  itself all night, and a `pkill -f` killed its own shell. Wait on a
  PID (`tail --pid`), kill by PID.
- **Ring counters are zeroed by a ring change and by down/up.** Read
  `rx_hw_tail_drop_packets` before, not after. `sweep.sh` reads it
  inside each cell.
- **The harness's derived budget is wrong on this card.** It reads the
  parked pages too early; the pools then keep several hundred more. A
  derived budget ran the region dry and stopped a ring. Always
  `FORCE_BUDGET` (which `sweep.sh` does) and let the guard skip what
  does not fit.
- **`cells.tsv`'s `drops` column is the interface's, not the ring's**,
  and reads about 125 for every cell whatever the load.
- **The `seq-sized` cells are too short to compare rates** (512 MiB,
  under a second). Use the 120 s `step` cells.
- **Attaching `credit.bt` hides the sixteen-connection stall.** Trace
  to read the accounting, never to decide whether a cell stalls.
- **One run is not a result.** The same cell ran at 1979 and at 569.
- **The region is named by the PCI device** (`0000:a1:00.0`), and
  `ethtool -G` works only with the port up, `-L` only with it down.
- **`fio` returns 0 from a stalled cell.** Read the rate and
  `admitted`.

## Where things are

- `common.sh`, `build-kmod.sh`, `bringup.sh`, `ringsize.sh`, `sweep.sh`,
  `summarize.py`, `plans/`: this directory. `rcv.py` and `snd.py` are `ringsize.sh`'s
  sender and receiver (only the server's receive buffer is set).
  `credit.bt` is the admission trace.
- `../run-budget.sh`: the harness (`NETDEV=`, `CLIENT=`, `STORAGE=`,
  `EXPORT_DIR=`, `BACKING=`, `FORCE_BUDGET=`, `FORCE_RCVBUF=`,
  `CELLS=`).
- Run output: `/var/tmp/zc-run-logs/` (`xeu-*`; it is on the root file
  system and survives the reboot). The 64 KiB runs are `xeu-q-*`,
  `xeu-step-*`, `xeu-cpu-*`, `xeu-llc-*`, `xeu-copy-*`.
- The client: `ssh -i /root/.ssh/id_ed25519_snitzer2 root@192.168.1.106`
  (x86_64, 4 KiB pages, 48 CPUs; it has other work, so its idle CPU is
  not zero).
- The artifact: "NFSD Receive Region Budget",
  <https://claude.ai/artifact/3UQkMVoWW8p9A5ZASPnm3g>. Its source is
  `../artifacts/region-budget.html` (also
  `/var/tmp/zc-run-logs/artifact/region-budget.html`); publish to the
  same URL. Its 64 KiB section opens with the summary of the xeu runs
  (`id="xeu64"`); a 16 KiB one belongs at the top of the 16 KiB section
  when there are results, and the page's lede names the rigs.
- Still owed, after the 16 KiB results: `transplant/tardis1-build457-handoff.md`,
  `transplant/README.md` and `RESULTS.md` say nothing of the xeu runs.
