# Handoff: the 7.3 canary with David's performance series, on tardis2

Written 2026-11-04.  Background and every port decision:
`CANARY-SPLIT.md` beside this file.

## What to test

`kernel-7.3/canary` as rebuilt after this commit, made by `hs/merge703_canary.sh v7.3-11`
(kernel-rpmtree 6d97ef3):

| order | topic | tip |
|---|---|---|
| 1 | nfsd-testing-canary-nfs_common-XFS-IOLOCK-and-async-DIO-middle | 2649cfbf2d43 |
| 2 | nfs-localio-performance-canary (David's 13 LOCALIO patches) | 6c4afbe39222 |
| 3 | NFSD_TCP_WRITE_ZEROCOPY-canary (hs-gitlab ZEROCOPY up to David's base) | 96438024c362 |
| 4 | nfs-and-nfsd-performance-canary (David's other 69, plus the localio KUnit real-file fix and the KUnit configs) | cfcdd9807468 |
| 5 | NFSD_TCP_WRITE_ZEROCOPY-canary-part2 (ZEROCOPY after David's base, ported) | this branch |
| 6 | changelog-canary | cfeb1c621b45 |

State: every commit of topics 2-5 builds W=1 clean with all KUnit suites
enabled.  Build 512 (`05df800d5668`) booted on tardis2 on 2026-11-05 and
every KUnit suite it has passes once nine stale tests and setup steps are
fixed: see "KUnit on build 512" below.  The runtime baselines ran the same
day: see "Runtime on build 512" below.  In short, loan mode on two ports runs
at build 507's rate, but at 45% more nfsd cycles a WRITE until the per-CPU
pools and the run-to-completion hop on WRITEs are turned off; copy mode runs
at 62.38 GB/s; the region did not grow.

## KUnit on build 512 (tardis2, 16 KiB, 2026-11-05)

Each suite was loaded alone, the riskiest last, with kdump armed (it was
not at boot: the SELinux symlink of TESTING.md, fixed by `bringup.sh`'s
`kdump_armed`) and netconsole to the client.  Nothing crashed and no
suite warned.  Logs and notes: `/var/tmp/zc-run-logs/t2-512-canary/`.

| suite | first run | after the fixes |
|---|---|---|
| `xdr_kunit` | 10/10 | 10/10 |
| `cache_kunit` | 3/3 | 3/3 |
| `rx_loan_kunit` | 32/34 | 34/34 |
| `nfsd_rtc` (in `nfsd.ko`) | 26/26 | 26/26 |
| `nfsd_bvec_kunit` | 16/18 | 18/18 |
| `localio_cpu_kunit` | 7/7, 20/20 | 7/7, 20/20 |
| `nfsd4_bvec_kunit` | 8/10 | 10/10 |
| `svc_pool_kunit` | 28/28 | 28/28 |
| `svc_rtc_kunit` | 40/43 | 43/43 |
| `svcsock_kunit` | 187/188 | 187 and 1 skip; 188/188 at `skb_defer_max` 128 |

Not run: the NVMe target TCP suite, as `NVME_TARGET_TCP_KUNIT_TEST`
needs `KUNIT=y` and the build has `KUNIT=m`.

Every failure was a test or the test setup, not the code under test:

- **`rx_loan` tile tests** (`rx_loan_tile_region_test`,
  `rx_loan_tile_arena_test`) assumed 4 KiB pages: a 5000-byte payload
  at region offset 100 fits one 16 KiB page.  Sizes now follow
  `PAGE_SIZE` and are as before at 4 KiB.  Fixes `2a68b3a50d7c`.
- **`nfsd_bvec_dio_segments_test`** did not expect the `IOCB_NOSERIAL`
  that `293b0ad2399b` puts on a split write's buffered ends.  Fixes
  `6a653f95202f`.
- **`nfsd_dio_geometry_iomap_test`** expected `IOCB_DIO_GEOMETRY` at bit 25
  and an async kiocb's EDIOALIGN as -EINVAL.  Port decisions 0008 and 0009
  (`CANARY-SPLIT.md`) moved the flag to bit 26 (25 is `IOCB_NOSERIAL`) and
  let the flag alone report the refusal.  Fixes `a7105d5e4eb6`.
- **`nfsd4_q35_decode_matrix_test`, `nfsd4_q35_malformed_post_write_test`**
  send NFSv4.0 COMPOUNDs.  With the Hammerspace extensions
  (`1bb07fd5eb81`), writing `+4.2` to nfsd's versions file clears 4.0, so
  `run-kunit.sh`'s `-2 -3 +4 +4.1 +4.2` left it off and every COMPOUND was
  refused whole (NFS4ERR_MINOR_VERS_MISMATCH).  `run-kunit.sh` now writes
  `+4.0` last, and the tests say so when 4.0 is off.  Also: writing 0 to
  `threads` drops the listener, so write `tcp 2049` to `portlist` again
  before restarting threads.
- **`svcsock_rx_region_pool_defer_test`** needs deferred skb frees, and
  the settled setting has `net.core.skb_defer_max` 0
  (`/etc/sysctl.d/90-zc-settled.conf`): the skb is freed at once and the
  page is back before the pool goes.  It now skips with its reason there,
  and `run-kunit.sh` takes that skip as expected.  Fixes `cb2438048e4c`.
- **`rtc_e2e_pool_grow_teardown`, `rtc_e2e_async_complete`,
  `rtc_e2e_publish_pinned`** read `serv->sv_rtc_active`, which
  `6879939e7074` replaced with a per-CPU count: it reads 0 always, so the
  checks for 0 in the suite passed whatever was held.  An interface commit
  exports the sum as `svc_rtc_kunit_active()`, and the suite reads it
  against the count at each test's start.  `sv_rtc_active` in
  `include/linux/sunrpc/svc.h` is now unused (code; left for David).

The nine commits (seven KUnit, two `run-kunit.sh`) are at the tip of
local `kernel-7.3/canary` on `05df800d5668`, build 512's own base, not
pushed.  The tree at the tip is the one tested: the fixes ran as modules
built from it.  Their `Fixes:` name commits of this history.

Notes for the next run:

- **No kernel has been built from the tip.** The fixes ran only as
  modules built out of tree for 512 (below).  Build a kernel from the
  tip, boot it with `updates/` empty and run every suite again; the
  results above are expected to hold.
- A rebase of the nine onto `v7.3-11.canary` (`5dd960d0097a`) was made
  and undone on 2026-11-05.  It applied unchanged (`git range-diff` all
  `=`); that base's 32 changed files touch neither the `IOCB_*` bits nor
  the server side of sunrpc (only `clnt.c`, `clnt.h`).
- **Empty `updates/` first.** It holds modules built out of tree for 512
  from these fixes: `sunrpc.ko` (with `svc_rtc_kunit_active`; its 438 old
  CRCs equal 512's) and `rx_loan_kunit`, `nfsd_bvec_kunit`,
  `nfsd4_bvec_kunit`, `svcsock_kunit`, `svc_rtc_kunit`.
- `modprobe -r` of a suite also unloads its unused dependencies, `kunit`
  among them; nfsd then autoloads `kunit` without `enable=1` and the next
  suite reports `kunit: disabled`.  Use `rmmod`.
- `pool_mode` reads `auto` at boot and `percpu` once a pooled service has
  started (`svc_pool_map_choose_mode()` always picks it): not a leak of
  the suites.
- Build 512 came from `05df800d5668`: its sources were checked against the
  debuginfo in `/usr/src/debug/`.  On tardis2 the local tag
  `v7.3-11.canary` was `4b828d2219f8` that morning, then `5dd960d0097a`;
  neither is build 512's base.

## First: David's KUnit suites, with the code as it is

Build with these on (the config used here is
`kernel-rpmtree/rpmbuild/configs/kernel-aarch64-7.3-localmod.config` plus):
RX_LOAN_KUNIT_TEST, NFSD_BVEC_KUNIT_TEST, NFSD_RTC_KUNIT_TEST,
NFSD4_BVEC_KUNIT_TEST, SUNRPC_XDR_KUNIT_TEST, SUNRPC_SVCSOCK_KUNIT_TEST,
SUNRPC_SVC_POOL_KUNIT_TEST, SUNRPC_SVC_RTC_KUNIT_TEST,
SUNRPC_CACHE_KUNIT_TEST (all =m), NVME_TARGET_TCP_KUNIT_TEST=y,
NFS_LOCALIO_KUNIT_TEST.  The page_pool region suite lives in
net/core and follows PAGE_POOL with KUNIT.

Every suite must pass before anything below.  Most at risk, because they
were ported rather than applied: the page_pool region (device slots,
63f918f53588), rx_loan (run drop, prefetch, takeover, checked refs), and
svcsock_kunit's tests of the post-cut code (736454e343a7, 18c12d621f75,
cc56e328ef6b).

### The tardis2 KUnit crashes of 2-3 November, checked against this tree

TESTING.md "KUnit crashes on build 506" (in-tree docs, hs-gitlab
e999ccf776a9, carried by topic 3) traces them to fake or broken fixtures:

| crash | cause | here |
|---|---|---|
| boot3, boot4 | `nfsd_bvec_kunit` fake files, NULL `f_mapping` read by `nfs_dio_split()` | fixed: real files; David's added cases use `nfsd_bvec_plain_file()` too |
| boot5 | the real-service svcsock fixture's teardown | not here: that fixture is the TODO below, so the old fixture runs |
| boot6 | nfsd4 fixture without `rq_authop` | fixed: `nfsd_kunit_svc.h` sets it |
| (new) | David's `localio_cpu_kunit` fake file, same NULL `f_mapping` (this tree's write cut reads it; his v7.3-7 base did not) | fixed: c0400502bcec, a real shmem file; expected splits unchanged |

Still fake, never run on tardis2, so suspect first if one takes the host
down: `svc_rtc_kunit` and `svc_pool_kunit` build zeroed `svc_rqst` and
`svc_serv` (the pattern of the `xpt_net` crash on tardis1), and
`svcsock_kunit`'s old fixture builds a zeroed `svc_sock`, `svc_rqst` and
`svc_serv` (the TODO below replaces it).  Load them one at a time, the
riskiest last, with kdump armed.

## Then: the runtime tests

The two xeu NICs must share the one region, each keeping its own
mappings: that is what 63f918f53588 is for.  Check
`/sys/kernel/debug/page_pool_rx_region`: one `mapped <dev>` line per
NIC, `free` and `cached` per slot.  (Build 512: both NICs `mapped`, on
`0000:a1:00.0` and `0000:a4:00.0`, after `bringup.sh 32768 256`.)

## Runtime on build 512 (tardis2, 2026-11-05)

Build 512's own sunrpc and nfsd (`updates/` emptied first; sunrpc
`E00DDF4219EE8F0CD7A25BA`, nfsd `90122931101ECCD1B292201`, xeu
`CA0931345E46C929B11D01F`), no pseudo-NMI, kdump armed, `bringup.sh 32768
256`, then `settled.sh apply` ending in the settled setting (after the
script changes below).  Every run is round 61's: `fstrim` of the eight
drives, 120 s of rest, `env WARM=10 RUNTIME=30 WRITES=<n> ./twoport.sh
$L/<run> ab` and `./cost.sh`, two ports, 1 MiB wsize from the client
10.20.12.106; `WRITES=128` is a loan run (budget 16262 pages), `WRITES=0`
copy.  No run had a drive step down or stall, and dmesg stayed clean.
Logs in `/var/tmp/zc-run-logs/<run>/`, notes in
`/var/tmp/zc-run-logs/t2-512-canary/NOTES`.

| run | mode | pools, `rtc_read` | GB/s | nfsd cycles (instr) a WRITE | ring cycles | woken a WRITE |
|---|---|---|---|---|---|---|
| build 507 blk-1..4 | loan | one, (none) | 65.63-70.54 | 269.5k-279.6k (272.7k-278.7k) | 72.8k-74.0k | 3.42 (r178) |
| t2-r179 | loan | per-CPU, Y | 67.23 | 397.2k (353.6k) | 79.2k | 6.22 |
| t2-r180 | loan | one, Y | 66.47 | 350.3k (328.0k) | 77.5k | 3.89 |
| t2-r181 | loan | one, N | 65.83 | 291.7k (277.9k) | 71.5k | 3.99 |
| t2-r182 | loan | one, N | 64.56 | 291.2k (278.8k) | 75.8k | — |
| t2-r185 | loan | one, N | 66.59 | 296.9k (278.1k) | 75.0k | — |
| t2-r186 | loan | per-CPU, N | 68.71 | 347.8k (305.5k) | 77.4k | 6.43 |
| t2-r184 | copy | one, N | 62.38 | 706.1k (477.0k) | 83.2k | 4.12 |

The loan runs are held by memory bandwidth, as on every run of this rig
(the eight drives take about 80 GB/s, and nfsd's CPUs are about 16% busy),
so their rate says little; the cost a WRITE is the measure.  Two of David's
changes account for nearly all of the 45% more nfsd cycles, and their costs
add up:

| nfsd cycles a WRITE | one pool | 64 per-CPU pools |
|---|---|---|
| `rtc_read=Y` (the default: the hop) | 350.3k (t2-r180) | 397.2k (t2-r179) |
| `rtc_read=N` (no hop) | 291.2k-296.9k (t2-r181, r182, r185) | 347.8k (t2-r186) |


- **Per-CPU pools** (`3fe6000275d8`): `auto`, the default `pool_mode`,
  always picks one pool for each CPU, 64 pools of 8 threads on tardis2, with
  sibling wake and steal.  About 50k cycles a WRITE with the hop or
  without it (t2-r179 against t2-r180; t2-r186 against t2-r181, r182 and
  r185, which change only `pool_mode`), 1.6 times the wakes and half again
  the context switches (1.07M a second against 703k), with more cycles
  than instructions added.  This brings back the several pools a node that
  the series dropped on 15 October (`pools_per_node`), and undoes Jeff
  Layton's `a1a7071195b9` (in `v7.3-11`), which removed every mode but
  per-node: on tardis2's one node that is one pool, the same code path as
  `pool_mode=global` (no thread affinity, `svc_pool_for_cpu()` returns the
  one pool), so `global` here stands for upstream and build 507.
- **The run-to-completion hop on WRITEs** (`cd57a466c835`, on
  `9874eba2dca0`'s fast path): a receive goes
  to a work item while either direction is on and its mode is direct
  (`fs/nfsd/nfs3proc.c:1248`).  `rtc_read` defaults to Y and the rig runs
  `io_cache_read=2`, so every receive, WRITEs included, went to a
  `kworker` and back to a thread: in `rtc_stats` 63.2M diverted, 51.2M
  `fallback_no_peek`, 11.9M `fallback_not_fast`, none run to completion.
  FLAMEGRAPH profile fl62 (one pool, `rtc_read=Y`; `$L/t2-fl62/`,
  compared with round 61's fl-blk in `vs-fl-blk/`): the `kworker`s cost
  26.4k cycles a WRITE on nfsd's CPUs, absent on 507.  With `echo N >
  /sys/kernel/debug/nfsd/rtc_read` the hop is gone (`diverted` did not
  move), nfsd is at 291.7k cycles a WRITE and its instructions within
  507's range, and context switches halve.

What is left, about 18k cycles a WRITE (+6.5%) at the same instructions,
is not explained; 507's own runs spread by about 10k.  Whether the rig
holds `pool_mode=global` and `rtc_read=N`, or the defaults change in the
series, is Mike's.  Neither is in `settled.sh`: both go back to their
defaults when nfsd is reloaded or the host reboots.  After t2-r186 the host
was left at `pool_mode=percpu` and `rtc_read=N`.

**Copy mode** (t2-r184): 62.38 GB/s, nothing loaned (the budget read
`0 (manual)` throughout and `admitted` did not move), drives 71 to 76%
busy, nfsd 2.4 times a loaned WRITE's cycles.  There is no copy run on
tardis2 at the two-port setting of 3 November to set it against.

**The region did not grow.** The rig's "region pages in use at most"
(`pages - min_free`) read 18880 to 26080 on these loan runs against 507's
10118 to 12124, but the figure changed meaning: on 507 the region's `free`
and `min_free` counted the pages free on each device's mapped slot
(`free = free_unmapped + mapped free`), and here (`63f918f53588`) they
leave the slots out, so up to about 20,000 free pages parked on the slots
count as held.  Sampled every 20 ms with the slots' free and cached
counted free, t2-r185 held 2867 pages at the median, 5616 at the 99th
percentile and 8420 at most (copy, t2-r184: 922 at the median).  A 20 ms
sample can miss a short dip, so 8420 is a lower bound of the peak, which
507's figure gives exactly.  Ways to make the figure exact again: in
`twoport.sh`, add the slots (samples only, as the kernel keeps no
low-water mark for them), or in the kernel, a low-water mark over the
shards, the CPU caches and the slots together in the region's debugfs file.
Earlier on this day the figure was read as real growth, and samples
without the slots seemed to confirm it; they were wrong in the same way.

**What the rig scripts needed** (committed as "the rig scripts on David's
region sizing and per-CPU pools": `xeu-rig/common.sh`, `settled.sh`,
`twoport.sh`, `run-budget.sh`):
on this canary a budget reads back `<n> (auto)` or `<n> (manual)`, and a
write of 0 is auto, the computed size (the whole region here), so the 0
that the scripts wrote to mean copy turned loaning on (`cb2438048e4c`:
"kNFSd lends with no parameter set"; `99197ab25abb`: "off" is a budget
of 0).  That reverses the series' decision of 22 October that a budget of
0, the default, loans nothing.  `rx_w` writes 0 as `off` on such a kernel,
`rx_r` reads the value in effect, and `settled.sh` expected 64 pools of 8
with `pool_mode=percpu` (until 6 November: it now holds `pernode`, see
"Runtime on build 514").  A new network namespace starts at `32768
(auto)`, so it loans with nothing configured.

## Runtime on build 514 (tardis2, 2026-11-06)

Build 514 (`7.3.0-11.hs.514.canary`) differs from 512 on the server only in
KUnit (the fixes above and `svc_rtc_kunit_active()`); the rest is NFS client
work.  `updates/` empty; sunrpc `1C2C5C459F3413804A2CEE1`, nfsd
`90122931101ECCD1B292201` (512's), xeu `CA0931345E46C929B11D01F`; no
pseudo-NMI.  kdump was not armed at boot; `bringup.sh 32768 256` armed it
and mapped both NICs, and `settled.sh apply` ended in the settled setting,
`rtc_read` and `rtc_write` at N for every run.  The runs are 512's: fstrim,
120 s of rest, `WARM=10 RUNTIME=30 twoport.sh ab`, `cost.sh`.  No drive
stepped down or stalled and dmesg stayed clean.  Driver and notes in
`/var/tmp/zc-run-logs/t2-514-canary/`.

| run | mode | pools | GB/s | nfsd cycles (instr) a WRITE | ring cycles | woken a WRITE | 512's |
|---|---|---|---|---|---|---|---|
| t2-r187 | loan | per-CPU | 66.34 | 340.1k (305.0k) | 77.2k | 6.39 | t2-r186: 347.8k (305.5k) |
| t2-r189 | loan | one | 66.72 | 297.0k (278.7k) | 73.4k | 3.95 | t2-r181/182/185: 291.2k-296.9k (~278k) |
| t2-r188 | copy | per-CPU | 57.93 | 925.2k (552.1k) | 85.3k | 10.99 | none |
| t2-r190 | copy | one | 62.23 | 693.4k (476.1k) | 80.5k | 4.22 | t2-r184: 62.38, 706.1k (477.0k) |

- **No regression against 512.** With one pool, loan and copy are within
  512's spread, and so within about 6% of 507's loan cost
  (269.5k-279.6k, the unexplained gap noted above).
- **Per-CPU pools cost even more in copy mode.** With the hop off, they add
  about 43k cycles a loaned WRITE (as on 512) and about 232k cycles (+33%) a
  copied WRITE.  Copy runs 7% slower (57.93 against 62.23 GB/s), with 2.6
  times the wakes and twice the context switches (1.50M a second against
  769k).  This is the first copy run with per-CPU pools.
- **t2-r189 denied 515 loans** (2.52M admitted); every 512 run denied 0.
  Not a regression: build 507 denied too (84 in t2-r178, 344 in fl61), and
  a denied record is not copied: its transport stops reading until credit
  returns (`paused` and `overrun` stayed 0).  The consumer's share refused
  nothing (`share_denied` 0; the share is the whole region), so it was the
  budget.  t2-r191 repeated t2-r189 at one pool (`pernode`) and sampled the
  budget every 20 ms: 66.63 GB/s, nfsd 298.0k cycles a WRITE, **denied 0**.
  While admitting, the budget had 12058 of 16262 pages free at the median
  and 5422 at the least, never under one WRITE's 126; 42 records in flight
  at the median, 105 at most.  So a denial comes from a burst shorter than
  20 ms, now and then, as on 507.  Sampler and samples:
  `/var/tmp/zc-run-logs/t2-514-canary/budget-sampler.sh`,
  `/var/tmp/zc-run-logs/t2-r191.budget.tsv`.

**Per-CPU pools are off from here (Mike, 6 November):** they were a
liability on tardis2's Xsight hardware when tried before (`pools_per_node`,
dropped 15 October), and these runs show the same.  `settled.sh` holds
`pool_mode=pernode`, as it holds `rtc_read` and `rtc_write` at N.  That is
upstream's only mode since Jeff Layton's `a1a7071195b9`, and on tardis2's one
NUMA node it is one pool of 512, the same as `global` in the runs above.
`apply` restarts nfs-server to put it in place, because the mode is read only
when a pooled service starts and is refused (-EBUSY) while one runs.  Whether
the series' default changes is still open.

## The nfsd cycles over 507 (tardis2, 7 November)

The profile fl63 (`WHICH=ab flame.sh t2-fl63 fl63 128` at the settled
setting: `pernode`, `rtc_read` and `rtc_write` N, budget 128; 66.33 GB/s,
denied 346) against build 507's fl-blk (round 55's B, 67.31 GB/s).  nfsd's
CPUs were busy 100.85 us a WRITE against 88.79; idle left out, their cycles
are 210.5k a WRITE against 189.5k, +21.0k.  Instructions a WRITE are 507's
(about 278k, `cost.sh`), so the gap is cycles an instruction, spread over
the path.  By stage, in k cycles a WRITE, inclusive (the canary's `rx_loan_*`
paired with 507's `svc_tcp_rx_*` they replace):

| stage | 507 | 514 | delta | where |
|---|---|---|---|---|
| receive (`svc_tcp_recvfrom` and its new caller `svc_xprt_recv_call`) | 62.9 | 70.6 | +7.7 | ACKs (`tcp_send_ack`) +1.8; admission (`rx_loan_admit`) +1.3; the loan walk, prefetch and region pages +1.6; the new caller's own +1.1; `skb_attempt_defer_free` +0.6 |
| thread wake and wait (`svc_recv` outside `svc_handle_xprt`) | 18.0 | 22.7 | +4.7 | `wake_up_process` +0.9, waiting for work +1.8, `svc_thread_should_sleep` +1.1, `svc_pool_idle_add` +0.7, the `lwq` dequeue +0.5 |
| WRITE and COMMIT (`nfsd_dispatch`) | 66.0 | 68.9 | +3.0 | `nfsd_write` +2.0 (XFS itself -1.4), COMMIT +0.6 |
| loan release at the reply (`svc_tcp_release_ctxt`) | 12.1 | 14.9 | +2.9 | pages back to the region one at a time, +2.5; credit return +0.9 |
| reply send (`svc_tcp_sendto` less the release) | 14.5 | 16.6 | +2.1 | David's `svc_tcp_combine_sends` around `svc_tcp_sendmsg` |
| the rest of `svc_process` | | | +0.8 | |

- **The release's +2.5 is a port decision, not David's code:** round 58's
  bulk return to the region (507's `ca7b00ac39c7`, one lock and one atomic
  for the pages a pool's stack cannot take) was dropped at the port
  (`CANARY-SPLIT.md`), as it was written for one region list.  On the
  canary the overflow goes back page by page (`page_pool_return_netmem`, then
  `rx_region_page_put` and `rx_region_slot_put`): 6.6k a WRITE against
  507's 4.1k under the region's lock.  A bulk return to a device's slot
  would take it back.  Rounds 53 and 55's changes are in the canary.
- The rest is in David's receive, admission, thread and send changes, and
  the base (`v7.3-10` to `v7.3-11` and the canary's other topics, XFS among
  them); 1.0k to 2.0k a stage, none alone the gap.  507's runs spread by
  about 10k cycles a WRITE, so what is under 1k a stage is not to be read.

**Against the profiles nearest 70 GB/s** (cycles a WRITE rise with the
rate on this rig, so fl-blk, at fl63's rate, is the like-for-like one):

| reference | GB/s | nfsd busy cycles a WRITE | 514's excess |
|---|---|---|---|
| fl-blk, build 507 (round 55's B) | 67.31 | 189.5k | +21.0k |
| fl-pp, build 507 (round 58's B) | 69.39 | 192.5k | +18.1k |
| round 57's profile, build 506 (`t2-r165/fl56-flame`) | 71.68 | 201.8k | +8.7k |

Build 506 had neither round 55 nor round 58 built in, and ran at the top
of the rig's spread.  Against all three, the loan release
(`svc_tcp_release_ctxt` +2.3k to +3.0k, of it the pages' return +1.7k to
+2.6k) and the reply send (+3.4k to +5.0k) are over, and the receive by
+2.0k to +5.8k.

Files: `/var/tmp/zc-run-logs/t2-fl63/` (the profile) and
`t2-fl63/vs-fl-blk/` (`incl.txt`, `children.txt`, `by-leaf.txt`, and the
scripts `incl.py`, `children.py`, `fdiff.py`).

## Next boot: the A/B of the bulk return to the region's slot (7 November)

The commit at the tip of `kernel-7.3/canary`, "page_pool: return a
bulk's overflow to the receive region's slot at once", brings back for the
slots what round 58's `ca7b00ac39c7` did for one region list. When a pool
with a slot finds its stack full, the pages
`page_pool_recycle_ring_bulk()` leaves over go back to this CPU's cache of
the slot under one hold of its lock, with one `atomic_add()` on the pool's
release count. Pages not still mapped for the slot, or held elsewhere, go
back one at a time as before. Built W=1 clean.

**The A/B kernel** is built on tardis2, not on gitlab: branch
`kernel-7.3/canary.ab-bulk-514` is build 514's own source (`6089265ea2c1`:
514's debug sources are byte for byte that commit, all but pNFS flexfiles,
a client module) with the change cherry-picked (`78efb6bc835c`) and a DROP
switch on top (`1e95d3be2ede`: `page_pool.rx_region_return_bulk_ab`, Y the
bulk return, N one page at a time as 514). It has 514's config and release
string, so it boots with 514's initrd and modules: all 14,075 of vmlinux's
exported CRCs are 514's. The config differs only in the compiler (gcc 14.3.1
here, 14.4.1 on gitlab), a signing key made here for the build, and
`CONFIG_MODULE_ALLOW_BTF_MISMATCH=y`, so that 514's modules, whose BTF was
generated against gitlab's vmlinux, load with a warning and are not
refused. Turning BTF off instead drops `sched_ext` and changes the CRCs.
514's modules load tainted (signed by another key; signatures are not
enforced). The image, its `System.map`, `vmlinux.symvers`, config and key
are in `/var/tmp/zc-run-logs/t2-514-canary/ab-bulk-kernel/`, with
`SHA256`.

**Installed and set to boot once (7 November, at Mike's request):** the
entry `ofp-rocky-ab-bulk-514.conf` and `/boot/ofp-rocky/ab-bulk-514/`
(`linux` checked against `SHA256`), and `bootctl set-oneshot` on it; build
514 stays the default, so the boot after this one is 514's again.  A reboot
is all that is left.  What was run, for the record:

    R=7.3.0-11.hs.514.canary.el10.aarch64+16k K=/var/tmp/zc-run-logs/t2-514-canary/ab-bulk-kernel
    mkdir -p /boot/ofp-rocky/ab-bulk-514
    cp $K/vmlinuz.efi /boot/ofp-rocky/ab-bulk-514/linux
    cp /boot/ofp-rocky/$R/initrd /boot/ofp-rocky/ab-bulk-514/initrd
    printf '%s\n' 'title      A/B bulk return on 514 (one-shot test)' "version    $R" \
      'sort-key   ofp-rocky-zz-ab' 'options    console=ttyAMA0 rodata=noalias crashkernel=1G' \
      'linux      /ofp-rocky/ab-bulk-514/linux' 'initrd     /ofp-rocky/ab-bulk-514/initrd' \
      > /boot/loader/entries/ofp-rocky-ab-bulk-514.conf
    bootctl set-oneshot ofp-rocky-ab-bulk-514.conf

**After it boots:**

0. `git worktree prune` in the tree: the A/B branch's worktree was in
   `/dev/shm/wt514`, gone with the reboot (the branch stays).  If a
   setting differs from what is written here, read NOTES in
   `/var/tmp/zc-run-logs/t2-514-canary/` for the last state.
1. `uname -v` names tardis2's build (`root@tardis2`, gcc 14.3.1), and
   `/sys/module/page_pool/parameters/rx_region_return_bulk_ab` reads Y. If
   it is missing, the boot fell back to 514.
2. `dmesg | grep -iE 'btf|taint|WARNING:|BUG:|Oops'`: BTF-mismatch
   warnings for 514's modules and their taint are expected, nothing else.
   `lsmod | grep -E 'xeu|sunrpc|nfsd'` shows all three loaded.
3. From `xeu-rig/`: `./bringup.sh 32768 256` (it arms kdump), then
   `./settled.sh apply`, ending in "the settled setting".
4. `/var/tmp/zc-run-logs/t2-514-canary/ab-bulk.sh` (about 20 minutes): four
   loan runs A B B A, `t2-r192` to `t2-r195`, each in NOTES.
5. Read A against B by nfsd's cycles a WRITE (`cost.sh`, last line of each
   `.out`) and the region's pages in use, which must not rise (AGENTS.md).
   The profile's `page_pool_recycle_ring_bulk` was 7.75k cycles a WRITE on
   514 (`page_pool_return_netmem` 6.6k of it): what B can take is about 2.5k
   (507's was 4.1k under the region's lock). A `flame.sh` profile of B
   shows where it went.
6. Keep or drop: on a drop, revert that commit at the tip (a code
   commit). The A/B branch and its DROP commit stay off `kernel-7.3/canary`
   either way. Then boot 514 again (the one-shot entry already gives way to
   the default) and remove `/boot/ofp-rocky/ab-bulk-514` and its entry.

### The first boot into it (7 November, 02:00): port a lost, no data

Steps 0 to 3 passed: `/proc/version` names `root@tardis2` and gcc 14.3.1,
the switch read Y, dmesg had only the BTF-mismatch and taint lines, kdump
was armed, `bringup.sh 32768 256` mapped both NICs and `settled.sh apply`
ended in the settled setting.  Step 4 gave nothing: each of `t2-r192` to
`t2-r195` stopped after its rest at `a: no answer from 192.168.1.106 in
20 s`.  Port a (`enp161s0f0`) has a link at both ends and sends, but
receives nothing: `rx_packets` 0, `rx_dropped` 17,
`port_rx_fifo_tc0_packets` 4596, `port_rx_non_prom_drop_packets` 1, its
rings filled, no fault in dmesg; the client's `ens3np0` cannot ping it
either.  Port b answers.  Four reopens by `twoport.sh` did not bring it
back.  That is the card's known failure, seen on tardis1's port b on build
504 ("tardis1 on build 504 (29 October, night)" in `xeu-rig/README.md`),
where only a hard power reset brought the port back.  It is not shown to be
the A/B kernel's doing, and not ruled out: this was its first boot.

**Next:** a hard power reset, with the one-shot entry set again first
(`bootctl set-oneshot ofp-rocky-ab-bulk-514.conf`; a power cycle uses it
up like a reboot), then steps 0 to 4 again, with a ping of both ports
(`ping -I enp161s0f0 192.168.1.106`, `-I enp164s0f0 192.168.0.106`)
after `bringup.sh` and before `ab-bulk.sh`.  The empty `t2-r192` to
`t2-r195` are to be overwritten (`ab-bulk.sh 192`).

### The second boot (7 November, after the power reset): dropped

The power reset brought port a back.  Steps 0 to 3 passed again (the
switch read Y, dmesg only BTF and taint, kdump armed, the settled
setting), and both ports answered three pings of three.  The four runs,
all clean (no drive stepped down, no stall, dmesg clean):

| run | | GB/s | nfsd cycles/W | nfsd instr/W | rig's region figure | denied |
|---|---|---|---|---|---|---|
| `t2-r192` | A | 65.65 | 288.5k | 277.6k | 19658 | 0 |
| `t2-r193` | B | 66.32 | 288.4k | 274.4k | 21200 | 501 |
| `t2-r194` | B | 70.60 | 302.9k | 272.1k | 20176 | 0 |
| `t2-r195` | A | 65.33 | 288.3k | 277.7k | 19558 | 0 |

B runs 3k to 5.5k fewer instructions a WRITE in nfsd, but no fewer
cycles: `t2-r193`, at A's rate, costs the same 288k (`t2-r194` ran
faster and costs more).  It is **dropped** for that: the revert of
"page_pool: return a bulk's overflow to the receive region's slot at
once" is at the tip of `kernel-7.3/canary`.  The A/B branch and its DROP
commit stay off it.

**The region did not grow with it.**  B's rig figure ("region pages in
use at most", 518 to 1642 above A's) was first read as a rise, and the
drop given that reason; it is not one.  On this kernel `pages -
min_free` counts as held the pages free on the device slots and in the
CPU caches, and sums the shards' low-water marks, each reached at its
own moment ("The region did not grow" under "Runtime on build 512"):
it is 507's exact peak no more.  The bulk return parks more free pages
on the slot, so it raises that figure and no page held.  One more pair
with the region sampled every 20 ms, a page counted held when it is on
no shard, CPU cache or slot (`t2-514-canary/region-sampler.sh`, the
loaded 59 s):

| run | | GB/s | nfsd cycles/W | held median | 99th pct | max | rig's figure |
|---|---|---|---|---|---|---|---|
| `t2-r196` | A | 69.08 | 303.2k | 3122 | 6394 | 7559 | 20464 |
| `t2-r197` | B | 66.06 | 290.8k | 2925 | 5691 | 7577 | 21936 |

The same held, and A's maximum of 7559 sits under 507's exact 12124
(`t2-r178`); a sample can miss a dip under 20 ms, so a sampled maximum
is a lower bound of the peak.  The cycles of this pair follow its rates
(65582 against 63142 WRITEs a second) and decide nothing.  `twoport.sh`
now samples the region so on such a kernel (`regionsample.py`, see
"What twoport reports of the region" in `xeu-rig/README.md`); an exact
peak there needs a low-water mark over the whole region in the kernel.

**Left for Mike:** boot build 514 again (the one-shot entry is used up,
so any reboot gives 514), then remove `/boot/ofp-rocky/ab-bulk-514` and
`/boot/loader/entries/ofp-rocky-ab-bulk-514.conf`.

## Next boot: the region's held mark (7 November)

Mike, the same night: add a low-water mark to the kernel, behind a static
key, so the region's peak is exact again; it may be dropped after the test.
`bb7fd3100c3b` at the tip, "page_pool: count the receive region's pages
held and their peak": each partition counts the pages it has out (in no
shard, CPU cache or slot) and the most since the debugfs file was last
written, shown there as `held` and `max_held`, while
`page_pool.rx_region_held_mark` is Y (N by default; switched on, it starts
from the free counts, so at rest).  The count moves only where pages leave
the free lists or come back: `rx_region_get()`, `rx_region_slot_take()`,
the pool's steal, and the returns in `page_pool_rx_region_put()`,
`rx_region_slot_put()` and `rx_region_give()`'s bulk push; escaped pages
stay counted.  It moves just after its pages do, so a peak can be off by
the takes and returns in flight on other CPUs at that instant (a refill
each at most), with no drift.  W=1 and checkpatch clean.

**The kernel** is built on tardis2 as the A/B kernel was: branch
`kernel-7.3/canary.held-514` is build 514's source (`6089265ea2c1`) with
the change (`cd7e94513437`), 514's config and release string
(`make EXTRAVERSION=-11.hs.514.canary.el10.aarch64+16k LOCALVERSION=`),
all of vmlinux's exported CRCs 514's.  Image and the rest in
`/var/tmp/zc-run-logs/t2-514-canary/held-kernel/` with `SHA256`; the
worktree was `/dev/shm/wt-held` (gone with the reboot: `git worktree
prune`).  **Installed and set to boot once:** `ofp-rocky-held-514.conf`
and `/boot/ofp-rocky/held-514/`; 514 stays the default.

**After it boots:**

1. `/proc/version` names `root@tardis2`, gcc 14.3.1, and
   `/sys/module/page_pool/parameters/rx_region_held_mark` reads N.  If it
   is missing, the boot fell back to 514: say so and stop.
2. dmesg: only the BTF-mismatch and taint lines, as on the A/B kernel.
3. From `xeu-rig/`: `./bringup.sh 32768 256`, `./settled.sh apply`, a ping
   of both ports (`-I enp161s0f0 192.168.1.106`, `-I enp164s0f0
   192.168.0.106`).
4. At rest: `echo Y > .../rx_region_held_mark`; `held` must equal the
   pages held from the free counts (`pages - free - Σ slots' free and
   cached`, about 1152 at rest on this boot's predecessor); `echo N`.
5. `/var/tmp/zc-run-logs/t2-514-canary/ab-held.sh` (about 20 minutes):
   four loan runs A B B A, `t2-r198` to `t2-r201`, A the mark off, B on,
   `twoport.sh`'s 20 ms sampler in all four (`REGION_SAMPLE=1`).  At rest
   before and after each B, `held` against the free counts (they must
   agree: no drift).
6. Read: B's exact `max_held` against the sampled maximum of the same run
   (the sample is a lower bound, so the mark must be at or above it, by
   the dips under 20 ms), against 507's exact 12124 (`t2-r178`); A against
   B in nfsd and ring cycles a WRITE, the mark's own cost.  Then Mike's:
   keep the mark or drop it (a revert at the tip).
7. Record the runs here and in NOTES, and commit (docs) before anything
   else.  Then boot 514 again (the one-shot entry gives way to the
   default) and, with Mike, remove the two test kernels:
   `/boot/ofp-rocky/held-514`, `/boot/ofp-rocky/ab-bulk-514` and their
   entries `ofp-rocky-held-514.conf`, `ofp-rocky-ab-bulk-514.conf`.

**Before step 5, know:**
- `git worktree prune` in the tree first: `/dev/shm/wt-held` went with
  the reboot (the branch stays).
- `ab-held.sh` is the first run of `twoport.sh`'s new region paths (the
  sampler of `6ad3e2e2daca`, `max_held` and `REGION_SAMPLE` of
  `a9bac4b7a5a8`); only the summary lines were tried, on `t2-r197`'s
  samples.  If a run's `summary.txt` has no "region pages in use" lines,
  or `region-sec.tsv` is missing in a run dir, stop and fix `twoport.sh`
  before reading anything.
- If a port does not answer the ping in step 3 (port a received nothing
  on the A/B kernel's first boot, the card's known failure): only a hard
  power reset brought it back.  Set the one-shot entry again first
  (`bootctl set-oneshot ofp-rocky-held-514.conf`), since a power cycle
  uses it up.  What is known of it, the cases and what to record are in
  `PORT-DEAD-AFTER-BOOT.md`; add each boot there, good or not.
- Nothing of tonight is pushed: on `kernel-7.3/canary` from `2a4ce5220019`
  (the bulk return's revert, reworded; the tip before is kept as
  `kernel-7.3/canary.pre-reword-0407`) to the tip, and
  `kernel-7.3/canary.held-514`.


### The first boot into it (7 November, 04:25): port a lost again

Steps 1 and 2 passed: `/proc/version` names `root@tardis2` and gcc
14.3.1, `rx_region_held_mark` reads N, dmesg has only the BTF-mismatch and
taint lines, kdump is armed (1024 MiB).  In step 3 `bringup.sh 32768 256`
mapped both NICs and `settled.sh apply` ended in the settled setting, but
port a (`enp161s0f0`) did not answer the ping (0 of 3, neighbour FAILED);
port b answered 3 of 3.  Port a has a link and sends (8 packets), and
receives nothing: `rx_packets` 0 on every ring, `rx_dropped` 15, no fault
in dmesg.  Unlike the A/B kernel's first boot, `port_rx_fifo_tc0_packets`
and `port_rx_non_prom_drop_packets` are 0 too: the port saw no frame at
all, not only its rings.  The card's known failure again, on the first
boot of a test kernel for the second time; nothing was run, step 4 not
reached.

**Next:** a hard power reset.  The one-shot entry is set again
(`bootctl set-oneshot ofp-rocky-held-514.conf`, `OneShot Entry` reads
it).  Then steps 1 to 3 again, with both pings, before step 4.

### The second boot into it (7 November, 04:35, after the power reset): port b lost

The reset brought port a back and lost port b.  Steps 1 and 2 passed as
before (`root@tardis2`, gcc 14.3.1, the mark N, dmesg only BTF and taint,
kdump armed at 1024 MiB), `bringup.sh 32768 256` mapped both NICs and
`settled.sh apply` ended in the settled setting.  Port a answered 3 of 3;
port b (`enp164s0f0`) 0 of 3, three times over, neighbour FAILED: link up,
18 packets sent, `rx_packets` 0, `rx_dropped` 11, no fault in dmesg.  The
client (up 25 days, both carriers up) pings 192.168.1.213 and not
192.168.0.213, so it is tardis2's port.  `port_rx_fifo_tc0_packets` is 0
here, but it reads 0 on the working port a too, so that counter does not
say a port saw no frame, and the first boot's reading of it above does not
stand.  Nothing was run, step 4 not reached.

That is the card's failure on three of the four boots into a test kernel
tonight (the A/B kernel's first, the held mark's first and second), on
either port.  Whether boots of build 514 itself lose a port as often has
not been counted.

**Next:** a hard power reset again; the one-shot entry is set again
(`OneShot Entry` reads `ofp-rocky-held-514.conf`).  Then steps 1 to 3 with
both pings, before step 4.

### The third boot into it (7 November, 04:55, after the power reset): both ports lost

Steps 1 and 2 passed as before (`root@tardis2`, gcc 14.3.1, the mark N,
dmesg only the firmware, BTF and taint lines, kdump armed).
`bringup.sh 32768 256` mapped both NICs and `settled.sh apply` ended in
the settled setting, and then **neither** port answered: port a 0 of 3
(9 packets sent, `rx_dropped` 21), port b 0 of 3 (11 sent, `rx_dropped`
12), `rx_packets` 0 on both, both neighbours FAILED, no fault in dmesg.
The client (up 25 days) has both links up at 400 Gb/s and answers on its
management address, and cannot ping 192.168.1.213 or 192.168.0.213.
`port_rx_fifo_tc0_packets` 0 on both.  No reset tried (none helps:
`PORT-DEAD-AFTER-BOOT.md`).  Counters and dmesg in
`/var/tmp/zc-run-logs/t2-514-canary/port-dead/held-boot3/`.  Nothing run.

That is four of five boots into a test kernel tonight with a port lost,
this one both.  The pings were only after `bringup.sh`, which reopens both
ports; on the next boot ping before it too (step 0 of "On every boot" in
`PORT-DEAD-AFTER-BOOT.md`).

**Next:** a hard power reset; the one-shot entry is set again
(`OneShot Entry` reads `ofp-rocky-held-514.conf`).  Then steps 1 and 2,
the pings before `bringup.sh`, then step 3 with both pings, before step 4.


### The fourth boot into it (7 November, 05:05, after the power reset): port b dead before `bringup.sh`

Steps 1 and 2 passed (`root@tardis2`, gcc 14.3.1, the mark N, dmesg only
the firmware, BTF and taint lines, kdump armed).  Pinged before
`bringup.sh` (step 0): port a 3 of 3, port b (`enp164s0f0`) 0 of 3,
neighbour FAILED.  So b was dead as the boot left it, before any reopen.
b was not silent this time: 38 frames received (31 on ring 3), 65 dropped,
21 sent.  `bringup.sh` was not run and nothing else was.  Counters and
dmesg are in `/var/tmp/zc-run-logs/t2-514-canary/port-dead/held-boot4/`.
That is five of six boots into a test kernel tonight with a port lost.

**The power cut at the third boot's commit (05:00) broke the tree.**  The
commit's five objects, `.git/index` and `refs/heads/kernel-7.3/canary`
were left empty, and the reflog's last line was lost.  Repaired by setting
the branch back to the reflog's last entry, `139cc9976c4c` (04:48: "a bus
reset does not bring a dead port back either"), and rebuilding the index
with `git reset`.  The working tree was intact, and the third boot's text
went into this boot's commit.  The empty objects are kept outside the tree.

**Integrity of the kernel, checked on this boot** (Mike doubted it after
the cut): `held-kernel/SHA256` verifies (`vmlinuz.efi`, `System.map`,
`vmlinux.symvers`, `config`), `/boot/ofp-rocky/held-514/linux` is
byte-for-byte that `vmlinuz.efi`, and `rpm -V` is clean (beyond
timestamps) on every build 514 package, `kmod-16k-xsight` (`xeu.ko`)
included.  Nothing under `/lib/modules` or `/boot` was written after 04:50,
and `updates/` is empty.  The kernel was installed at 04:18 and first
booted at 04:25, both before the cut.  The initrd has no recorded sum,
but it is dated 04:18 and three boots used it before the cut.  No
reinstall.

**Next (Mike, 05:20): boot stock build 514, to see whether it loses a port
too.**  The one-shot entry is cleared, so the power reset boots the default,
`ofp-rocky-7.3.0-11.hs.514.canary.el10.aarch64+16k.conf`.  On that boot:
check `uname -r` and that `rx_region_held_mark` is absent; arm kdump; ping
both ports before `bringup.sh` and record the result in
`PORT-DEAD-AFTER-BOOT.md`'s table.  Then `bringup.sh` and `settled.sh apply`
and ping again.  Count a few boots that way before going back to the held
mark (`bootctl set-oneshot ofp-rocky-held-514.conf`).  Before `bringup.sh`,
`page_pool.rx_region_dev` is empty, so the receive region has no device and
`xeu`'s pools are plain page pools.  A port dead at that point died without
the region's override.


### Stock build 514, first boot of the count (7 November, 05:19, after the power reset): port a dead before `bringup.sh`

`uname -r` 7.3.0-11.hs.514.canary.el10.aarch64+16k (the default entry, no
one-shot), `rx_region_held_mark` absent, kdump armed (`kexec_crash_loaded`
1, `crashkernel=1G`, the kernel's copy in `/var/lib/kdump/boot/`).  Pinged
before `bringup.sh` (step 0): port a (`enp161s0f0`) 0 of 3, neighbour
FAILED; port b 3 of 3.  The client (up 25 days) has both ports up at 400
Gb/s and pings 192.168.0.213 and not 192.168.1.213, so it is tardis2's
port a.  As with the held mark's fourth boot, a was not silent: 86 frames
received (31 on ring 0, 36 on ring 3), 147 dropped, 21 sent,
`port_rx_fifo_tc0_packets` 0, and no ARP reply reached it.  dmesg has
nothing beyond the usual firmware, taint and `rx_ip_align` lines.  Then
`bringup.sh 32768 256` (both NICs mapped) and `settled.sh apply` (the
settled setting), and a was still dead: 0 of 3, 0 received, 23 dropped, 8
sent; b 3 of 3.  Counters, dmesg and both scripts' output are in
`/var/tmp/zc-run-logs/t2-514-canary/port-dead/stock514-boot1/`.

**So stock build 514 loses a port too, from boot, before the region has a
device.**  The held mark's kernel is not shown to cause it; one boot gives
no rate to set beside its five of six.

**Next:** a hard power reset into the default entry again (no one-shot is
set), and the same steps, to go on counting stock 514's boots.

### Stock build 514, second boot of the count (7 November, 05:27, after the power reset): both ports good

`uname -r` 7.3.0-11.hs.514.canary.el10.aarch64+16k (the default entry, no
one-shot), kdump armed (`kexec_crash_loaded` 1, `crashkernel=1G`).  Pinged
before `bringup.sh` (step 0): port a 3 of 3, port b 3 of 3, both neighbours
REACHABLE.  Then `bringup.sh 32768 256` (both NICs mapped, 4 pools each) and
`settled.sh apply` (the settled setting, no BAD item), and both still 3 of
3.  dmesg has nothing beyond the usual lines.  Counters, dmesg and both
scripts' output are in
`/var/tmp/zc-run-logs/t2-514-canary/port-dead/stock514-boot2/`.

**So far stock 514 is one lost of two boots**, against five of six for the
two test kernels: too few boots for either rate.

**This good boot ran build 514's runtime baselines again (Mike), below.**

#### Baselines on it (05:31 to 05:52)

As in "Runtime on build 514": `run.sh` (fstrim, 120 s of rest, `WARM=10
RUNTIME=30 twoport.sh ab`, `cost.sh`), at one pool (`pernode`, the settled
mode since 6 November), loan and copy in turns.  Driver
`/var/tmp/zc-run-logs/t2-514-canary/base-boot2.sh`, its log `base-boot2.log`
beside it.  `t2-r198..r201` are left to `ab-held.sh`.  No drive stepped down
or stalled, dmesg stayed clean, no loan was denied, and both ports still
answered after the last run.

| run | mode | GB/s | nfsd cycles (instr) a WRITE | ring cycles | region sampled: median / p99 / max | 6 November, one pool |
|---|---|---|---|---|---|---|
| t2-r202 | loan | 65.64 | 292.4k (277.4k) | 75.0k | 2981 / 6074 / 9175 | t2-r189: 66.72, 297.0k (278.7k) |
| t2-r203 | copy | 64.15 | 738.6k (475.2k) | 80.3k | 930 / 950 / 985 | t2-r190: 62.23, 693.4k (476.1k) |
| t2-r204 | loan | 66.36 | 294.5k (277.7k) | 74.4k | 3023 / 6227 / 8160 | t2-r191: 66.63, 298.0k |
| t2-r205 | copy | 62.29 | 694.1k (475.7k) | 81.7k | 921 / 942 / 958 | |

- **Build 514 runs as it did on 6 November.** Loan is within 1.6% in rate
  and 2% in nfsd cycles of t2-r189 and t2-r191; t2-r205 is t2-r190 again
  (62.29 against 62.23 GB/s, 694.1k against 693.4k cycles).
- **t2-r203 is the outlier of the copy runs:** 3% faster and 6.5% more nfsd
  cycles a WRITE on the same instructions (475.2k), so cycles a WRITE in
  copy mode vary by about that much from run to run.
- The region figures are sampled with the slots and caches free (a lower
  bound of the peak); the 6 November runs' "in use at most" is not set
  beside them (see "What twoport reports of the region" in
  `xeu-rig/README.md`).

**Next (Mike): back to the held mark.**  The one-shot is set again
(`bootctl set-oneshot ofp-rocky-held-514.conf`; `bootctl status` shows it as
`OneShot Entry`), so the next boot is the held-mark kernel, its fifth.  Start
at "Next boot: the region's held mark" above: ping both ports before
`bringup.sh` (step 0 of `PORT-DEAD-AFTER-BOOT.md`), `bringup.sh 32768 256`
(it copies the kernel into `/var/lib/kdump/boot/`), `settled.sh apply`, ping
again, then `ab-held.sh` (`t2-r198..r201`).  If `rx_region_held_mark` is
missing, the boot fell back to stock 514: say so and stop.

### The fifth boot into it (7 November, 05:59): port a dead before `bringup.sh`

Steps 1 and 2 passed: `/proc/version` names `root@tardis2` and gcc
14.3.1 (built 04:16), `rx_region_held_mark` reads N, dmesg has only the
firmware, BTF-mismatch, taint and `rx_ip_align` lines, kdump is armed
(`kexec_crash_loaded` 1, `crashkernel=1G`).  Pinged before `bringup.sh`
(step 0): port a (`enp161s0f0`) 0 of 3, neighbour FAILED; port b 3 of 3,
REACHABLE.  As on the fourth boot and stock 514's first, a has a link and
is not silent: 79 frames received (31 on ring 0, 39 on ring 3), 137
dropped, 21 sent, `port_rx_fifo_tc0_packets` 0.  `bringup.sh` was not run
and nothing else was.  Counters, neighbours and dmesg are in
`/var/tmp/zc-run-logs/t2-514-canary/port-dead/held-boot5/`.

One counter seen on reading the boots side by side, not yet a finding:
`ring_0_rx_hw_pp_drop_packets` is not 0 on both ports on each of the
three boots with a port dead before `bringup.sh` (held-boot4: a 107,
b 53; stock514-boot1: a 124, b 52; this boot: a 114), on the port that
works as well, and 0 on both on stock 514's good second boot.

**That is six of seven boots into a test kernel with a port lost, and one
of two for stock 514.**  `ab-held.sh` has still not run.

**Next:** the one-shot is set again (`bootctl set-oneshot
ofp-rocky-held-514.conf`; `bootctl status` shows it), so a hard power
reset boots the held-mark kernel, its sixth.  The same steps as on this
boot: ping before `bringup.sh`, and on two good ports go on to step 3 and
`ab-held.sh`.

### The sixth boot into it (7 November, 06:08): both ports answer

The boot before it left git broken: the commit recording the fifth boot
(06:04) had its ref, index and three objects written empty when the power
was cut.  The working tree had kept its text, so the ref was set back to
`01c99985f0a6`, the empty objects removed (copies kept in the session's
scratchpad), the index rebuilt with `git reset` and the fifth boot
committed again.

Steps 1 and 2 passed (`root@tardis2`, gcc 14.3.1, built 04:16, the mark
N, dmesg only the firmware, BTF-mismatch, taint and `rx_ip_align` lines,
kdump armed at `crashkernel=1G`).  Pinged before `bringup.sh`: port a 3 of
3 and port b 3 of 3, both neighbours REACHABLE.  `bringup.sh 32768 256`
mapped both NICs, `settled.sh apply` ended in the settled setting, and both
ports answered again after it.  Step 4 at rest: `held` 1072, `max_held`
1072, and 1072 from the free counts.  Counters and logs are in
`/var/tmp/zc-run-logs/t2-514-canary/port-dead/held-boot6/`.

The fifth boot's counter does not stand: `ring_0_rx_hw_pp_drop_packets`
reads a 101 and b 51 on this boot, with both ports good, so it is not a
mark of a dead port.

`ab-held.sh` ran there (06:18 to 06:38), four loan runs at the settled
setting, A B B A, A with the mark off and B on, all four with the 20 ms
sampler.  Every run was clean: no drive stepped down, no stall, no loan
denied, paused or overrun, dmesg clean.

| run | mark | GB/s | nfsd cycles a WRITE | ring cycles a WRITE | sampled at most | exact `max_held` |
|---|---|---|---|---|---|---|
| `t2-r198` | A, off | 70.25 | 310.6k | 74.7k | 8639 | |
| `t2-r199` | B, on | 65.28 | 294.2k | 73.5k | 6939 | 8949 |
| `t2-r200` | B, on | 66.17 | 295.7k | 76.1k | 7892 | 9134 |
| `t2-r201` | A, off | 65.77 | 294.9k | 74.4k | 8926 | |

- **No drift:** at rest before and after each B, `held` was 1152 and the
  free counts gave 1152.
- **The mark is at or above the sample, as it must be:** 8949 against
  6939 and 9134 against 7892, the sample short by 1200 to 2000 pages of
  dips under 20 ms.  In the A runs the sample alone reached 8639 and 8926,
  so a single run's sample can come within 200 pages of the peak or fall
  2000 short of it.
- **Against 507's exact 12124 (`t2-r178`):** 514's exact peak is about
  9000 to 9100 pages, a quarter fewer.
- **The mark's cost is not seen:** `t2-r198`, the first run, is the
  outlier (70.25 GB/s, 310.6k nfsd cycles); `t2-r201`, the A beside the
  two Bs, matches them in rate (65.77 against 65.28 and 66.17) and in nfsd
  cycles (294.9k against 294.2k and 295.7k), and ring cycles are within
  the runs' spread (73.5k to 76.1k).  Whatever the shared count costs is
  below what four runs can tell.

The "before" line of a B read the last run's peak (`max_held` 8949 before
`t2-r200`): `ab-held.sh` did not restart it at rest.  The run's own figure
was not affected, since `twoport.sh` writes the debugfs file just before
the load (`echo 1 > $RG`), which restarts the peak.  `ab-held.sh` now
writes it before the "before" line too.

**Kept (Mike, 7 November):** the mark stays, `bb7fd3100c3b` at the tip of
`kernel-7.3/canary`, off by default.  From here on a region peak is
`max_held` with `rx_region_held_mark=Y`, not the sample.

**The held-mark kernel is tardis2's default (Mike, 7 November), in place
of step 7's return to 514:** stock 514 has no mark, so its region figures
could not be exact.  `ofp-rocky-held-514.conf` is retitled "Held mark on
514 (default)" and set with `bootctl set-default` (the EFI variable; there
is no `default` in `loader.conf`), with the same options as 514's entry.
Stock 514 stays in the menu.  The A/B kernel of the bulk return is gone
from `/boot` (`ofp-rocky-ab-bulk-514.conf`, `/boot/ofp-rocky/ab-bulk-514/`);
its image, the same by SHA256, and its entry are kept in
`/var/tmp/zc-run-logs/t2-514-canary/ab-bulk-kernel/`.  The two kernels
share 514's release string, so kdump's capture kernel in
`/var/lib/kdump/boot/` is stock 514's image; that does for a capture, and
`bringup.sh`'s copy stays as it is.

**514 against 507: the region's peak (7 November).**  514 holds fewer of
the region's pages than 507 did, at the same setting:

| run | kernel | GB/s | peak pages | pages a GB/s | loans denied |
|---|---|---|---|---|---|
| `t2-r178` | 507 | 71.21 | 12124 (exact) | 170 | 84 |
| `t2-r199` | 514, the mark | 65.28 | 8949 (exact) | 137 | 0 |
| `t2-r200` | 514, the mark | 66.17 | 9134 (exact) | 138 | 0 |

507's `pages - min_free` is exact (its region is one free list with the
slots), as is 514's `max_held`.  A slower run has fewer WRITEs in flight,
so part of the drop is the rate, but per GB/s 514 still holds about 20%
fewer pages, and it never reached the budget where 507 was denied 84
loans.

The two runs were checked to be at the same setting (`geometry.txt` and
`audit.txt` of each): `settled.sh audit two` passed on both, off the
settled setting only in the budget the run sets
(`svc_tcp_rx_budget_pages=16262`, `svc_tcp_rx_budget_reserve=130`); the
"this run" lines are the same (rings 4 x 128, rcvbuf 524288, 12
connections a port, 16 jobs of 8M at iodepth 16, `/hs/nvme0` to
`/hs/nvme7`, both region devices), and the region is 32768 pages on both.
The audits differ only in what the kernels have: 507 has no `rtc_read` or
`rtc_write` (no run-to-completion, which is where `settled.sh` holds 514,
both N), and `pool_mode` was audited only from 6 November, but 507 is a
main-series kernel, which always reads pernode: one pool of 512, as 514
runs now.  The `256` of `bringup.sh 32768 256` is not the ring, which is
128 entries on both.  What differs is the region's design: on 507 one
free list and each device's slot, on 514 16 shards, per-CPU caches and
the slots' caches (about 2300 pages cached on each slot at rest).

So the region's peak is not 514's problem.  Its rate may be: 65 to 66
GB/s in three of the four runs (70.25 in the first) against 507's 71.21.
That rests on one 507 run, and whether 514's runs were limited by the
drives, as 507's was (each drive 90 to 92% busy), is not yet read.

**The drives, read (7 November):** 514's three slower runs were not
limited by the drives or the server.

| run | kernel | GB/s | drives of a | drives of b | server busy (rings a, b, nfsd) | client |
|---|---|---|---|---|---|---|
| `t2-r178` | 507 | 71.21 | 8.9 to 9.0, 92%, DRIVE-LIMITED | 8.8 to 8.9, 90%, DRIVE-LIMITED | 16, 14, 13% | 75% |
| `t2-r198` A | 514 | 70.25 | 8.5 to 8.6, 88% | 9.0, 92%, DRIVE-LIMITED | 16, 17, 15% | 71% |
| `t2-r199` B | 514 | 65.28 | 8.0, 84% | 8.3 to 8.4, 87% | 15, 12, 12% | 75% |
| `t2-r200` B | 514 | 66.17 | 8.5 to 8.6, 88% | 7.9 to 8.0, 83% | 16, 14, 13% | 74% |
| `t2-r201` A | 514 | 65.77 | 8.1 to 8.2, 85% | 8.3, 88% | 15, 14, 13% | 75% |

A drive is about 10% busy for each GB/s it writes in every run, so the
drives are not slower: they are sent less, each port carrying 32 to 34
GB/s on the wire against about 36 in the fast runs.  The server's CPUs
are 12 to 17% busy throughout.  Run order does not explain the gap, though it was
first read so: `t2-r178` was a run on its own, after an idle, as
`t2-r198` was, and the two are within 1.3%.  But `run.sh` trims every
drive and rests 120 s before each run, and 514's other loan runs at the
settled setting all carry about 66: `t2-r189` 66.72, `t2-r191` 66.63,
`t2-r197` 66.06, `t2-r202` 65.64 (the first loan run after stock 514's
second boot, the drives long idle), `t2-r204` 66.36, and `t2-r199` to
`t2-r201` 65.28 to 66.17.  `t2-r198` is the outlier, not the others.  So
507's 71.21 against 514's 66 is a gap of about 7%, on one 507 run, with
neither the drives nor the server the limit on 514 (less arrives on the
wire): open.

**nfsd's wakeups, read from the runs (7 November).**  Loan runs at the
settled setting only (not copy, and not `t2-r179`, `t2-r180`, `t2-r186`
to `t2-r188`, on per-CPU pools or run-to-completion, at 6 to 11 wakeups
a WRITE), from "nfsd in the window" of each `summary.txt`:

| kernel | runs | GB/s | threads woken a second | woken a WRITE | context switches a WRITE |
|---|---|---|---|---|---|
| 507 | `t2-r178` | 71.21 | 231.7k | 3.42 | 9.96 |
| 514, fast | `t2-r194`, `t2-r196`, `t2-r198` | 69.1 to 70.6 | 249.8k to 251.4k | 3.73 to 3.83 | 10.6 to 10.8 |
| 514, usual | ten, `t2-r189` to `t2-r204` | 65.3 to 66.7 | 250.4k to 253.9k | 3.95 to 4.07 | 11.1 to 11.35 |
| 512 | `t2-r181` to `t2-r185` | 64.6 to 66.6 | 249.8k to 250.1k | 3.93 to 4.06 | 11.1 to 11.4 |

512 and 514 wake about 250k threads a second against 507's 232k, 9% more
a WRITE even in 514's fast runs.  But within 514 the count is the same a
second in fast runs and usual ones, so the wakeups do not decide a run's
rate: they are a difference that came with 512, open on its own.

**514's fast runs against its usual ones.**  Three of 13 loan runs carry
69 to 70.6 GB/s, ten 65 to 67.

| | fast (`t2-r194`, `t2-r196`, `t2-r198`) | usual (ten) | 507 (`t2-r178`) |
|---|---|---|---|
| port a, b (GB/s) | 34.3 to 35.5, 34.5 to 36.0 | 31.7 to 34.3, 31.9 to 34.3 | 35.7, 35.5 |
| ring drops in the window, a port | 543 to 1001 | 97 to 309 | 266, 270 |
| client retransmits | 1187 to 2073 | 209 to 592 | 562 |
| client CPUs: usr, sys, idle | 8.2 to 8.5, 53.5 to 55.9, 28 to 31% | 10.2 to 10.4, 56.2 to 58.9, 24 to 27% | 8.9, 58.5, 25% |
| server busy: rings a, b; nfsd | 16 to 19, 16 to 20; 14 to 15% | 13 to 16, 11 to 17; 12 to 13% (`t2-r191` 30, 30) | 16, 14; 13% |

- **Both ports are faster together** in a fast run, not one port: what
  differs is not a port, a drive set or the flows' spread over the rings
  (25% each in every run).
- **More drops and retransmits go with the higher rate:** the client
  offers more, and the rings drop more of it.
- **The client does less user work while it sends more:** usr 8.2 to
  8.5% in the fast runs, 10.2 to 10.4% in every usual one, sys and idle
  following.  507's run reads 8.9.  It is not a rule: 512's `t2-r185`
  read 8.8 at 66.59.  No CPU of the client is saturated in any run
  (busiest 76 to 83%).
- **Not the runs' order or an idle:** `t2-r194` was the second of two B
  runs, `t2-r196` the first of a pair after 25 minutes, `t2-r198` the
  first of four.

So what sets a 514 run at 66 or 70 shows on the client (its user time
and how much it offers) more than on the server, whose figures follow
the rate.  That is a correlation, not a cause: `cli-mpstat.txt` has no
per-process figures, so what the client's extra user time is (fio, or
something else on the client, up 25 days) is not known.

**The client's fio pinned (7 November).**  The client (Xeon Gold 6542Y,
two sockets, `idle=poll`, all CPUs at 3600 MHz throughout) has port a's
NIC (`ens3np0`) on node 0 and port b's (`ens6np0`) on node 1, and
`twoport.sh` left fio unpinned: the scheduler placed it.  `twoport.sh`
now records the client by process, where fio runs each second, its
clocks, and its NICs' counters (`b88e391a8e94`, `d7082977b0a1`).  With
the client's `irqbalance` stopped (its IRQs, all on their NIC's own node,
saved in `t2-514-canary/client-irq-affinity-0730.txt`; started again
after the runs), `ab-pin.sh` ran P U P U P U, P being `PIN=1` (fio on
CPUs 0-23 for a, 24-47 for b):

| run | fio | GB/s | fio on its NIC's node (a, b) | fio CPUs | kworker CPUs |
|---|---|---|---|---|---|
| `t2-r209` | pinned | 67.92 | 100%, 100% | 8.4 | 23.1 |
| `t2-r210` | not | 63.38 | 85%, 80% | 9.9 | 23.8 |
| `t2-r211` | pinned | 67.90 | 100%, 100% | 8.6 | 23.6 |
| `t2-r212` | not | 64.60 | 89%, 93% | 9.7 | 23.5 |
| `t2-r213` | pinned | 68.35 | 100%, 100% | 8.3 | 23.0 |
| `t2-r214` | not | 63.43 | 81%, 76% | 9.4 | 23.7 |

Pinned, 67.90 to 68.35 GB/s; not, 63.38 to 64.60: about 6% more, and
steadier.  Unpinned, 7 to 24% of fio's time is on the far node and fio
takes about 1.3 CPUs more.  The sending is mostly the sunrpc kworkers'
(23 CPUs of system time), which start where their work is queued: only
about 5% of each NIC's packets leave from the far node's CPUs, pinned or
not (from the NIC's per-queue counters and the client's XPS map; port
b's NIC numbers its queues from CPU 24).  The pinned runs do not reach
the earlier fast runs' 69 to 70.6.  `t2-r206`, unpinned with
`irqbalance` running, carried 68.59.

The kworkers cannot be pinned strictly on the client's kernel:
`rpciod` and `xprtiod` are not `WQ_SYSFS` (no `affinity_strict` or
cpumask of their own), kworkers refuse `taskset`, and the global unbound
cpumask cannot give each port its node.  Next, `ab-scope.sh`
(`t2-r215` to `t2-r220`): the client's
`workqueue.default_affinity_scope` `numa` against `cache_shard`, fio
pinned.

**The client's workqueue scope (7 November, 07:50 to 08:19).**
`ab-scope.sh` ran N C N C N C, fio pinned in all six and `irqbalance`
stopped, N the client's `workqueue.default_affinity_scope` at `numa`
(each node one pod), C at `cache_shard` (pods of 8 CPUs, the default);
`cache_shard` was set back after it.

| run | scope | GB/s | far node's share of each NIC's packets (a, b) | retransmits | kworker CPUs |
|---|---|---|---|---|---|
| `t2-r215` | numa | 69.80 | 2.4%, 2.7% | 1623 | 22.9 |
| `t2-r216` | cache_shard | 69.76 | 3.1%, 3.5% | 1229 | 22.7 |
| `t2-r217` | numa | 69.55 | 1.3%, 1.5% | 3268 | 21.7 |
| `t2-r218` | cache_shard | 68.30 | 4.6%, 4.0% | 705 | 23.1 |
| `t2-r219` | numa | 69.13 | 2.8%, 2.8% | 873 | 22.8 |
| `t2-r220` | cache_shard | 68.05 | 5.5%, 4.3% | 596 | 23.2 |

- **These are the fast runs:** 68.05 to 69.80, every run clean (no drive
  stepped down, no stall, dmesg clean).  Each has more drops and
  retransmits than the slower runs, as the earlier fast runs had.
- **The far node's share of the sends orders the rates:** from 1.3 to
  1.5% (`t2-r217`, 69.55) up to 5.5 and 4.3% (`t2-r220`, 68.05), the
  slowest runs those that sent most from the far node, `t2-r216` the
  exception at 3.3% and 69.76.
- **`numa` halves the far share:** 1.3 to 2.8% against 3.1 to 5.5%.  In
  rate it is 69.13 to 69.80 against 68.05 to 69.76, about 1% on three
  runs each, the two overlapping by one run.
- **Pinned with `cache_shard` came out higher than in `ab-pin.sh`'s
  runs** (68.05 to 69.76 against 67.90 to 68.35): the same setting half
  an hour apart, so between series a drift of about 1 GB/s that pinning
  does not remove.

So a 514 run reaches 69 to 70 GB/s when the client sends from its NICs'
own nodes: fio on its port's node (`PIN=1`), and its sunrpc kworkers
there as much as the non-strict scope lets them.  The earlier fast runs,
unpinned, are the ones where the scheduler happened to place it so; that
was not recorded for them, so it is the likely reading, not a finding.

**`twoport.sh` pins the client's fio by default from here on (Mike, 7
November: "the cpu pinning on client is a really good catch and sorely
missing"):** `PIN` defaults to 1, `PIN=0` leaves fio unpinned, and
`geometry.txt`'s "this run" line ends in `client pin`.  Runs from before
it, all unpinned, are not to be set beside pinned ones without saying
so: pinned carries about 6% more on 514.

**And the client's kworkers and interrupts (Mike, 7 November):** each
run sets the client's `workqueue.default_affinity_scope` to `numa`
(`CLI_SCOPE`) and stops its `irqbalance` (`CLI_IRQB=keep` leaves it),
records both on a "client:" line of `geometry.txt`, and puts both back
as they were when it ends.  `irqbalance` was started again on the
client at 16:03, after the series.

**At those defaults, 514 reaches 70 (7 November, 16:04 to 16:34).**
`more-numa.sh` ran six loan runs at `twoport.sh`'s new defaults (fio
pinned, the client's scope `numa`, its `irqbalance` stopped for each
run), the held mark off:

| run | GB/s | port a, b | far node's share of each NIC's packets (a, b) | busiest drive (a, b) | retransmits |
|---|---|---|---|---|---|
| `t2-r221` | 70.88 | 35.64, 35.24 | 1.6%, 0.9% | 91%, 91%, both DRIVE-LIMITED | 2222 |
| `t2-r222` | 70.07 | 35.81, 34.25 | 0.5%, 1.3% | 93% DRIVE-LIMITED, 88% | 1913 |
| `t2-r223` | 70.62 | 35.96, 34.66 | 1.7%, 1.3% | 92% DRIVE-LIMITED, 90% | 2042 |
| `t2-r224` | 71.03 | 35.86, 35.17 | 1.0%, 1.1% | 91% DRIVE-LIMITED, 90% | 2688 |
| `t2-r225` | 70.21 | 35.66, 34.54 | 1.4%, 2.1% | 91% DRIVE-LIMITED, 90% | 1494 |
| `t2-r226` | 69.18 | 35.73, 33.45 | 2.7%, 2.0% | 91% DRIVE-LIMITED, 87% | 1049 |

- **Five of six at 70.07 to 71.03,** `t2-r224` within 0.3% of 507's
  71.21 (`t2-r178`, unpinned); port a is limited by its drives in every
  run, as 507's run was.  The client's CPUs are 68 to 71% busy, against
  73 to 76% unpinned.  Every run clean; the client was put back
  (`cache_shard`, `irqbalance` active) and the audit at the end found
  nothing BAD.
- **The far share orders the rate again:** the one run under 70,
  `t2-r226`, sent the most from the far node (2.7%, 2.0%).
- **Faster than the morning's `numa` runs at the same setting** (69.13
  to 69.80, far 1.3 to 2.8%).  Between the two `irqbalance` ran again
  from 16:03 until the first run stopped it: 48 of port a's 49 NIC IRQs
  and 47 of port b's moved to another CPU, every one still on its NIC's
  node (`t2-514-canary/client-irq-affinity-0730.txt` against `-1635.txt`,
  the NICs' IRQs in `client-nic-irqs.txt`).  Which CPU of the node takes
  a queue's interrupt may be what tells 69 from 70; these runs cannot say,
  and the drives had also been idle about eight hours.

So the gap to 507 was the client: with its writers and kworkers on their
NICs' nodes, 514 carries what 507 did, up to the drives.  What 507 does
pinned is not measured (a reboot).

**Stay on this boot (Mike, 7 November):** the held-mark kernel's sixth
boot has both ports, and a reboot risks losing one (six of seven boots
into a test kernel did).  Use this kernel as long as possible: no reboot,
nothing that could take the host down (no KUnit suite, which crashed
tardis2 three times on build 506), and no reload of `xeu`.  A comparison
with 507 waits for a later boot; what can be learned on 514 alone is done
here.

**Port the svcsock KUnit fixture rework onto David's suites.**  Mike
updated the fixture so that the tests run on a real service and real
transports rather than fake interfaces; those changes are needed.  Not
carried yet: 2bac64e249d3, 85f70c3e342d, cf877205f8d5, 0fb04a5caaff,
14b16444a03d, edae0572b8bd (hs-gitlab kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY
e999ccf776a9).  David's 0023-0033 suites extend the old fixture (per-test
entries and pages, wait init, per-namespace budgets), so his tests move
onto the real-service fixture with them.  Land it on
NFSD_TCP_WRITE_ZEROCOPY-canary-part2, then run every suite again.

Other open items:
- changelog-canary: done 2026-11-04 (cfeb1c621b45): the ZEROCOPY entry
  reworded, entries for the performance and part2 topics added.
- Not pushed: topics 2-5, changelog-canary, canary (a force push: it
  was rebuilt), kernel-rpmtree 7914dfc and 6d97ef3.
