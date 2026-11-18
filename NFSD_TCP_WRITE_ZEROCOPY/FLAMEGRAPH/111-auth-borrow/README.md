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
