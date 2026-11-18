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
