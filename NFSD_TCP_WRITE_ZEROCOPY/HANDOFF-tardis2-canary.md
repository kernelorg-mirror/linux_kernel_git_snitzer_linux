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

| run | | GB/s | nfsd cycles/W | nfsd instr/W | region peak | denied |
|---|---|---|---|---|---|---|
| `t2-r192` | A | 65.65 | 288.5k | 277.6k | 19658 | 0 |
| `t2-r193` | B | 66.32 | 288.4k | 274.4k | 21200 | 501 |
| `t2-r194` | B | 70.60 | 302.9k | 272.1k | 20176 | 0 |
| `t2-r195` | A | 65.33 | 288.3k | 277.7k | 19558 | 0 |

B runs 3k to 5.5k fewer instructions a WRITE in nfsd, but no fewer
cycles: `t2-r193`, at A's rate, costs the same 288k (`t2-r194` ran
faster and costs more).  And both B runs hold more of the region at peak
than either A, by 518 to 1642 pages.  A change that raises the region's
peak is not kept (AGENTS.md), so it is **dropped**: the revert of
"page_pool: return a bulk's overflow to the receive region's slot at
once" is at the tip of `kernel-7.3/canary`.  The A/B branch and its DROP
commit stay off it.

**Left for Mike:** boot build 514 again (the one-shot entry is used up,
so any reboot gives 514), then remove `/boot/ofp-rocky/ab-bulk-514` and
`/boot/loader/entries/ofp-rocky-ab-bulk-514.conf`.

## TODO (formal, once the above passes on tardis2)

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
