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
enabled.  Nothing has run: no KUnit, no boot.

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
