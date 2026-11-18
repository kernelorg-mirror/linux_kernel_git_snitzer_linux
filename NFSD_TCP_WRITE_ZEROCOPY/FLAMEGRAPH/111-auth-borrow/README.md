# Round 111: the AUTH_SYS ip_map and gid borrow, and the client's profile

Mike, 18 November: "do the auth cache borrow and profile the client
too".  Round 110's profiles named it: `svc_set_client` 4.05% of the
server's samples at 2.49 M IOPS (t2-r933), `svcauth_unix_set_client`
1.75% self, the largest nfsd-side self cost after the spin locks.

## Why

In t2-r933's annotate, three atomic read-modify-writes (two `ldadd +1`,
one `ldaddl -1`) are 68% of `svcauth_unix_set_client()`'s own time: the
`kref_get()` on the auth_domain every client of the rig shares, the
reference on the uid's group_info, and the put of the call's old one.
`auth_domain_put()` (as `refcount_dec_not_one`) is 0.94% more, and the
uid's unix_gid lookup about 1.0% (`sunrpc_cache_find_rcu` 0.63%,
`unix_gid_match` 0.37%).  The transport's one-entry ip_map cache is
taken by one call at a time, so concurrent calls on a connection walk
the ip_map hash too.

## The change

`d8161366f11b2` "SUNRPC: Cache AUTH_SYS ip_maps and gids in each
svc_rqst": four ip_maps and eight unix_gids a rqst, each with its own
reference, the entry used last looked at first; a hit lends the
ip_map's domain and the unix_gid's group_info with no reference
(`rq_client_lent`, `rq_gi_lent`), a miss looks up as before and keeps
the lookup's reference.  Freshness as the export borrow checks it.
AUTH_SYS and AUTH_NULL only.  `sunrpc.svcauth_borrow` (default Y, read
on every call) turns it off; `sunrpc.svcauth_borrow_stats` reads its
counters.  No KUnit suite yet.

It adds fields to `struct svc_rqst`, so 275 of sunrpc's 446 exports
take new versions: built with `build-svcstack.sh` into
`/var/tmp/svcstack-541-authborrow/stack` (sunrpc
`E038753F5CE7FE08331B07F`, nfsd `528EBC791C7D3FAEB133919` rebuilt
against it); every module of the stack imports at the right versions,
and nothing outside the stack uses sunrpc on tardis2.  No warning at
W=1.

## The plan

    cd xeu-rig
    STACK=/var/tmp/svcstack-541-authborrow/stack SUNRPC_KO=none \
        ./step.sh /var/tmp/zc-run-logs/541 swap-auth ./swap-sunrpc.sh
    ./settled.sh apply
    ./write-check.sh 935; ./read-check.sh 936
    ../FLAMEGRAPH/111-auth-borrow/ab-auth.sh 937:Y 938:N 939:Y 940:N 941:Y 942:N
    ../FLAMEGRAPH/111-auth-borrow/prof-both.sh 943 Y
    ../FLAMEGRAPH/111-auth-borrow/prof-both.sh 944 N

The client has perf and debuginfo since 18 November (Mike), so its
kernel's symbols resolve.

## The passes (18 November, 00:48 to 00:56)

Server build 541 with `/var/tmp/svcstack-541-authborrow/stack` (sunrpc
`E038753F5CE7FE08331B07F`, nfsd `528EBC791C7D3FAEB133919`,
`svcauth_borrow` Y, `export_borrow_slots` 16), client build 541.

| Run | | GB/s | Result | ip_map hits / fills | gid hits / fills |
|---|---|---|---|---|---|
| t2-r935 | WRITE pass | 75.18 | **PASS** (mark 74.6) | 5.15 M / 180 | 5.15 M / 98 |
| t2-r936 | READ pass (io_uring, rsize 256 KiB) | 79.89 | **PASS** (mark 71.5) | 18.43 M / 521 | 8.55 M / 9.88 M |

No drive stepped down and no new kernel warning in either.

- **WRITE 75.18 GB/s**: port a 37.38, port b 37.80, the drives 9.2 to
  9.5 GB/s each at 96 to 97% busy, ring drops 126 to 131.  The best
  WRITE pass of the night (74.55 to 75.25 on the stacks before it).
- **READ 79.89 GB/s**: 39.93 and 39.97 a port, the drives 9.9 to 10.1
  GB/s each at 100% busy.  The best READ pass of the night (79.22 to
  79.65 before).  Against the frame: 76% of what the drives read with
  local fio (104.58 GB/s), 81% of what the ports send with iperf3 `-Z`
  (98.31); the pass measures no memory or Send-Q.

**The gid borrow misses half of every two minutes on this host.**
mountd runs without manage-gids (`/etc/nfs.conf`: `manage-gids=n`, the
default) and never answers an auth.unix.gid upcall, so each uid's entry
is one the kernel negates itself, for `CACHE_NEW_EXPIRY` (120 s).  Past
half that age the borrow's freshness rule misses so that cache_check()
starts a refresh, and the refresh stays pending, unanswered, until the
entry expires: every call of those 60 s misses and refills.  The READ
pass ran through such a window (9.88 M gid fills); the WRITE pass did
not (98).  The fix to make after the A/B: an entry whose refresh upcall
is already pending (`CACHE_PENDING`) is fresh until it expires, since a
miss then does nothing cache_check() does not.  The export borrow has
the same rule (mountd answers its upcalls, so it matters less there).
The A/B below runs with the gap in place; its fill counts show its
cost.

## The first A/B, stopped (18 November, 00:56 to 01:03)

| Run | svcauth_borrow | M IOPS (ports) | ms mean / p99 | Server busy (busiest) | Client busy (busiest) | ip_map hits / fills | gid hits / fills |
|---|---|---|---|---|---|---|---|
| t2-r937 | Y | 2.53 (1.26 / 1.27) | 1.21 / 4.42 | 71% (98%) | 71% (77%) | 67.42 M / 2,651 | 44.63 M / 22.79 M |
| t2-r938 | N | 2.49 (1.25 / 1.24) | 1.23 / 4.49 | 72% (94%) | 70% (75%) | | |

A third of t2-r937's gid lookups missed, the gap above.  Mike stopped the
A/B there ("please stop, build CACHE_PENDING fixed code and retest
A/B"); t2-r938's last seconds overlapped the build of the fix, so it is
not a clean figure.  No new kernel warning in either.

## The CACHE_PENDING fix and the suite

- `08924f8cb667b` "SUNRPC: Borrow an auth cache entry while its refresh
  is pending" and `33a76739cdd15` "NFSD: Borrow an export while its
  refresh is pending": an entry whose refresh upcall is already pending
  is fresh until it expires (cache_check() returns it as it stands
  then, and starts no other upcall).
- `01ad6b07da122` (the suite's interface) and `2c014fffa6c3f`, the
  `svcauth_unix_borrow` suite, eleven cases; compiled at W=1 against
  541, not run yet.

Built into `/var/tmp/svcstack-541-pending/stack` (sunrpc
`E834384870D73293B573D8C`, nfsd `DC178C9D921DD1CDFF947D7`), no warning.
The plan:

    cd xeu-rig
    STACK=/var/tmp/svcstack-541-pending/stack SUNRPC_KO=none \
        ./step.sh /var/tmp/zc-run-logs/541 swap-pending ./swap-sunrpc.sh
    ./settled.sh apply
    ./write-check.sh 945; ./read-check.sh 946
    ../FLAMEGRAPH/111-auth-borrow/ab-auth.sh 947:Y 948:N 949:Y 950:N 951:Y 952:N
    ../FLAMEGRAPH/111-auth-borrow/prof-both.sh 953 Y
    ../FLAMEGRAPH/111-auth-borrow/prof-both.sh 954 N

## The CACHE_PENDING fix did not close the gap (01:05 to 01:17)

On `/var/tmp/svcstack-541-pending/stack`:

| Run | | Result | ip_map hits / fills | gid hits / fills |
|---|---|---|---|---|
| t2-r945 | WRITE pass | 74.52 GB/s, RERUN | 5.07 M / 181 | 5.07 M / 98 |
| t2-r946 | READ pass | **79.83 GB/s, PASS** | 18.46 M / 504 | 8.48 M / 9.98 M |
| t2-r947 | IOPS, svcauth_borrow Y | 2.54 M (1.27 / 1.27), 1.21 / 4.42 ms, server 72% (93%), client 71% (79%) | 67.69 M / 2,620 | 44.98 M / 22.72 M |

The gid gap was the same: with manage-gids off mountd does not listen on
the auth.unix.gid channel at all, so cache_check() finds no reader,
clears `CACHE_PENDING` at once, and the entry ages to its expiry with no
refresh ever pending.  The A/B was stopped after t2-r947, whose run
finished on its own.

`9fbe6e9f4ef99` "SUNRPC: Lend an aged auth cache entry with no refresh
listener": each entry keeps the second a lookup last checked it; past
half its age an entry checked this second is lent too, so one call a
second goes through to cache_check() (which starts a refresh whenever
a listener can take it) and the rest are lent the entry.  Its suite
case is `4cd2db18dfc8a` (`sab_half_age_no_listener`).  Built into
`/var/tmp/svcstack-541-checked/stack` (sunrpc `E221A34ED597178925B8BD3`,
nfsd `DC178C9D921DD1CDFF947D7`), no warning at W=1.  The plan:

    cd xeu-rig
    STACK=/var/tmp/svcstack-541-checked/stack SUNRPC_KO=none \
        ./step.sh /var/tmp/zc-run-logs/541 swap-checked ./swap-sunrpc.sh
    ./settled.sh apply
    ./write-check.sh 955; ./read-check.sh 956
    ../FLAMEGRAPH/111-auth-borrow/ab-auth.sh 957:Y 958:N 959:Y 960:N 961:Y 962:N
    ../FLAMEGRAPH/111-auth-borrow/prof-both.sh 963 Y
    ../FLAMEGRAPH/111-auth-borrow/prof-both.sh 964 N

## Results on the fixed stack (18 November, 01:22 to 02:00)

`/var/tmp/svcstack-541-checked/stack` (sunrpc `E221A34ED597178925B8BD3`,
nfsd `DC178C9D921DD1CDFF947D7`), client build 541.  No new kernel
warning in any run (the wider filter of `c30fb8f5fdaab`).

### Passes

| Run | | Result | ip_map hits / fills | gid hits / fills |
|---|---|---|---|---|
| t2-r955 | WRITE pass | **74.90 GB/s, PASS** (mark 74.6) | 5.10 M / 178 | 5.10 M / 97 |
| t2-r956 | READ pass | **78.91 GB/s, PASS** (mark 71.5) | 18.32 M / 552 | **18.32 M / 7,403** |

The READ pass's gid fills are 7,403 where the stack before had 9.98 M:
the gap is closed.  Against the frame: 75% of what the drives read with
local fio (104.58 GB/s), 80% of what the ports send with iperf3 `-Z`
(98.31); the pass measures no memory or Send-Q.  (79.83 to 79.89 on the
two stacks before it, within the night's spread.)

### The A/B: the borrow is worth 2.4%

| Run | svcauth_borrow | M IOPS (ports) | ms mean / p99 | Server busy (busiest) | Client busy (busiest) | gid hits / fills |
|---|---|---|---|---|---|---|
| t2-r957 | Y | 2.55 (1.27 / 1.28) | 1.21 / 4.42 | 71% (92%) | 71% (78%) | 67.80 M / 10,665 |
| t2-r958 | N | 2.47 (1.26 / 1.21) | 1.21 / 4.42 | 71% (93%) | 69% (74%) | |
| t2-r959 | Y | 2.54 (1.26 / 1.28) | 1.21 / 4.55 | 71% (92%) | 70% (78%) | 67.71 M / 1,339 |
| t2-r960 | N | 2.49 (1.24 / 1.25) | 1.23 / 4.69 | 73% (95%) | 70% (77%) | |
| t2-r961 | Y | 2.54 (1.26 / 1.28) | 1.21 / 4.62 | 72% (94%) | 71% (76%) | 67.46 M / 17,335 |
| t2-r962 | N | 2.49 (1.27 / 1.22) | 1.21 / 4.49 | 72% (95%) | 70% (78%) | |

ms mean / p99 is port a's.  ip_maps hit 99.996% in every Y run, the
export borrow 99.98% on both sides.

- **Y 2.54 to 2.55 M, N 2.47 to 2.49 M: every Y run above every N run,
  2.4% on the mean** (2.543 against 2.483).  The first change of rounds
  110 and 111 that moves IOPS and not only CPU.
- Port b carries it: 1.28 M in all three Y runs, 1.21 to 1.25 off.

### Both ends profiled (t2-r963 Y, t2-r964 N)

`prof-both.sh`; reports in `/var/tmp/zc-run-logs/t2-r963/` and
`t2-r964/` (`prof-srv-*`, `prof-cli-*`), the server's data in
`/dev/shm/zc-p111-963.data` and `-964.data`.

| Server, share of samples | t2-r963 (Y, 2.53 M) | t2-r964 (N, 2.48 M) |
|---|---|---|
| `svc_set_client`, with children | **1.26%** | 4.36% |
| `svcauth_unix_set_client`, self | 0.38% | 1.98% |
| `auth_domain_put` (`refcount_dec_not_one`) | **0.08%** | 1.10% |
| `svcauth_unix_release`, with children | 0.76% | 1.53% |
| the borrow's lookups (`svcauth_borrow_ipm_find`, `svcauth_borrow_gid`) | 0.59% | |
| `svcauth_unix_accept` (`groups_alloc` in it) | 1.59% (0.58%) | 1.55% (0.57%) |
| swapper (idle) | 18.67% | 18.19% |
| busiest CPU (mpstat) | **92%** | 100% |

The borrow takes about 3.9 points of the server's samples off the auth
path, most of them the shared refcounts' writes, and the busiest work
CPU drops from 100% to 92%: that CPU sets the pace, so IOPS rise.
What is left of the auth path is mostly `svcauth_unix_accept`'s
`groups_alloc()` of the wire's groups on every call, thrown away once
the cached groups replace them (0.58%).

| Client, share of samples | t2-r963 (Y) | t2-r964 (N) |
|---|---|---|
| `_raw_spin_lock` + `native_queued_spin_lock_slowpath`, self | 5.75 + 3.89% | 5.69 + 3.94% |
| `queue_work_on`, with children | 6.16% | 6.17% |
| `rpc_async_schedule` (rpciod's work), with children | 29.04% | 28.38% |
| `cpu_idle_poll` | 24.88% | 25.45% |

The client is the same on both sides: about 10% of its samples in spin
locks, most of them under `queue_work_on`, a workqueue pool's lock as it
queues work for rpciod and xprtiod (round 108's limit on build 469 at
63.5%, cut by build 541's commits, back at 2.5 M), and a quarter of its
time idle-polling.  `xs_tcp_force_close` at 23% under rpciod is on
inlined frames, likely a symbol misattributed, not connections closed.

Next, not started: the workqueue lock the client queues under at
2.5 M; the wire's groups allocated on every call
(`svcauth_unix_accept`); the borrow suites' KUnit runs (Mike, 18
November: "we do also need to actually test the Kunit tests").

## The borrow suites run (18 November, plan)

Mike: "we do also need to actually test the Kunit tests".  Neither
borrow suite has run.  On build 541 at the tip's stack
(`/var/tmp/svcstack-541-checked`), no client connected, kdump armed:

1. `svcauth_unix_borrow` (`2c014fffa6c3f`, `4cd2db18dfc8a`), twelve
   cases: built as `svcauth_unix_kunit.ko` against the running sunrpc
   (`E221A34ED597178925B8BD3`, which exports the interface of
   `01ad6b07da122`), loaded with insmod through `step.sh`; it runs on
   its own test net and caches.  Results from
   `/sys/kernel/debug/kunit/svcauth_unix_borrow/results`, then rmmod.
   The first insmod (02:05) printed `kunit: disabled`: `kunit` came in
   with nfsd at its default `enable=0`, not writable at runtime
   (`TESTING.md`).  So `kunit-auth.sh` runs it: nfs units stopped, nfsd
   unloaded, `kunit` reloaded `enable=1 filter_glob=svcauth_unix_borrow`
   (the stack's nfsd links in `nfsd_rtc_kunit`, which must not run),
   the suite loaded and unloaded, `kunit` unloaded before nfsd loads
   again, the stack put back with `swap-sunrpc.sh` and `settled.sh
   apply`; each step through `step.sh`, logs in
   `/var/tmp/zc-run-logs/kunit-auth-541/`.
2. `nfsd_export_borrow` (the cases of `5420d7f0a7dc7` and
   `c3493f011b68e`): nfsd built from the same tree with
   `CONFIG_NFSD_EXPORT_BORROW_KUNIT_TEST=y`, swapped in with
   `swap-sunrpc.sh` (`NFSD_KO=`, `SUNRPC_KO=none`); the suite runs as
   nfsd loads.  Results from
   `/sys/kernel/debug/kunit/nfsd_export_borrow/results`, then the
   stack's own nfsd swapped back and `settled.sh apply`.

### The auth suite's first run (02:13): 8 of 12, the four failures the suite's own

`kunit-auth.sh /var/tmp/svcstack-541-checked`: host up, the stack back
at the settled setting after it.  `ok` 1 to 3, 6 to 8, 10, 12; `not
ok` 4 `sab_half_age`, 5 `sab_half_age_no_listener`, 9 `sab_lru`, 11
`sab_free`, with three refcount warnings (`refcount_warn_saturate`:
addition on 0 in `svcauth_unix_kunit_ip_map_set()`, then underflow).

- The warnings, 9 and 11: the suites' ip_map setter put the lookup's
  reference after `__ip_map_update()` had already handed it to
  `sunrpc_cache_update()`, freeing an entry still hashed; the counts
  after it read saturated (0xc0000000).  `9691e5492a90a`.
- 4 and 5: they aged the entry in the second they filled it, and an
  entry checked that second is lent (`9fbe6e9f4ef99`, as meant).
  `91b350c7a3e5f` waits for the next second first.

Neither is in the borrow.  Rebuilt into `/var/tmp/svcstack-541-kunit`
(sunrpc `85537945BAC1207EDA22E45`, nfsd unchanged
`DC178C9D921DD1CDFF947D7`); the sunrpc differs from
`E221A34ED597178925B8BD3` only in that setter, built under
`CONFIG_KUNIT` and reached by no WRITE or READ, so it owes no pass.
Next: `swap-sunrpc.sh` to it, `settled.sh apply`, `kunit-auth.sh
/var/tmp/svcstack-541-kunit`.

### The auth suite passes (02:20)

`kunit-auth.sh /var/tmp/svcstack-541-kunit` after the swap to it:
**`svcauth_unix_borrow` 12 of 12, no skip, no `WARNING:`, `BUG:` or
`Oops` in its run** (logs in `/var/tmp/zc-run-logs/kunit-auth-541/`);
the stack back at the settled setting, kdump armed.

### The export suite: the plan

`nfsd_export_borrow` is linked into nfsd.  A test nfsd from the same
stack's sources, `/var/tmp/svcstack-541-kunit/nfsd-ebt/nfsd.ko`, built
with `CONFIG_NFSD_EXPORT_BORROW_KUNIT_TEST=y` and
`CONFIG_NFSD_RTC_KUNIT_TEST` off (W=1, no warning; its only suite is
`nfsd_export_borrow_suite`).  `kunit-export.sh
/var/tmp/svcstack-541-kunit` loads it with `kunit` at `enable=1
filter_glob=nfsd_export_borrow`, no service started, then puts the
stack's own nfsd back as `kunit-auth.sh` does.

The first try (02:12) ran nothing: the suite is named
`sunrpc-nfsd-export-borrow`, and the filter named
`nfsd_export_borrow`; the test nfsd loaded and unloaded cleanly, no
KTAP, the stack back after it.  The filter and the results path fixed.

### The export suite's first run (02:26): 31 of 32, and writes into freed memory

`kunit-export.sh /var/tmp/svcstack-541-kunit` (logs
`/var/tmp/zc-run-logs/kunit-export-541/`): `ok` 1 to 27 and 29 to 32,
`not ok` 28 `ebt_key_change`, and two workqueue warnings
(`workqueue.c:2678`, `:2680`, `__queue_delayed_work()` from
`svc_export_release()` in an RCU callback).  Both the suite's:

- **The warnings.**  The suite's test nets are a zeroed `nfsd_net`, with
  none of the per-export statistics (`e487e730759a8`, 29 October) set up.
  Every export the caches made took a share of them, and its release,
  from RCU after the case had freed the net, took the net's lock and
  queued its reaper, an uninitialised delayed work.  This tree's
  `__mod_timer()` does not arm a timer with no function, so no timer
  is pending; but the lock, the work's pending bit, `wq`, `cpu` and the
  timer's `expires` were written, most likely in freed memory, perhaps
  once for each of the 64 test nets (`WARN_ON_ONCE` hides all but the
  first).  Nothing has faulted since.  `e0e5100a74155` (interface:
  `export_stats_init()`, `export_stats_shutdown()` for the suites) and
  `0b5cd6f5bb361` (each test net sets them up, and shuts them down after
  its caches, which waits for the releases).
- **`ebt_key_change`** was written for the one-entry slot; with several
  entries the first pair stays held beside the second (2 references,
  not 1).  Fixed in `0b5cd6f5bb361`.

The test nfsd rebuilt from the tree (W=1, no warning; its only suite
`nfsd_export_borrow_suite`); the running stack's nfsd is not changed,
the interface commit touching nothing nfsd runs.  Next: `kunit-export.sh
/var/tmp/svcstack-541-kunit` again.
