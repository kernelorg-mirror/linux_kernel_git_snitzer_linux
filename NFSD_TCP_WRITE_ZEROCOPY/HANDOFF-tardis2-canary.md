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
fixed: see "KUnit on build 512" below.  The runtime tests have not run.

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
NIC, `free` and `cached` per slot.

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
