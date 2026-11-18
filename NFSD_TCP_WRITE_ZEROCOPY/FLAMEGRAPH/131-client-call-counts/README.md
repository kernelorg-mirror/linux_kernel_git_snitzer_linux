# Round 131: the client's call_start counters, measured first

Mike, 18 November: "fix the client's call_start counters next", then
(the fix being wider than the server's) "yes, do step 1": measure what
the two counters cost before designing their per-CPU form.

Round 130 put 0.31 us of the client's 12.08 us a READ in `call_start`,
57% of it on `clnt->cl_stats->rpccnt++` and 35% on the version's
`counts[idx]++`: two counters every CPU of the client writes.  Both live
outside sunrpc (`nfs_rpcstat` and the counts array are nfs's statics,
reached through `struct rpc_stat` and `struct rpc_version`), so their
per-CPU form changes the layout sunrpc, nfs, nfs_acl, lockd and nfsd
share and needs an allocation at registration.

`09e0762eac9aa` "DROP: SUNRPC: A/B switch for FLAMEGRAPH round 131":
`sunrpc.rpc_call_counts_ab` N skips both increments (the statistics
stop counting calls in that arm).  The client's build 541 predates
rounds 123 and 124's `struct svc_stat`, so it runs this tree's whole NFS
stack for the round: `cbuild131.sh` builds net/sunrpc (GSS and RDMA
with it), fs/nfs_common, fs/lockd, fs/nfs and fs/nfsd from the tip on
the client against its kernel-devel with the tree's sunrpc headers, as
round 91's `cbuild.sh` did, into a module root that round 91's
`cswap.sh` loads with `modprobe -d` (nothing installed: a reboot brings
build 541's back; `cswap.sh stock` at the end).  Build 541's commit
`4c8aa21d29987` is an ancestor of the tip, and only
`include/linux/sunrpc` has changed in `include/` since.

The quick A/B: `ab-counts.sh 1054:Y 1055:N 1056:Y 1057:N` under
`settled-iops.sh`.

Built on the client from `09e0762eac9aa`: 26 modules, no warning but
the 10 compiler and pahole version notes; sunrpc srcversion
`DF01CCB1C31213C2F060D0C`, with the switch.  The KUnit modules it built
are taken out of the module root.

**The swap.**  `cswap.sh` as it was loads nfsd too, and the tree's
nfsd links in the export-borrow KUnit suite, which wants the kunit
module the client does not load: it fell back to build 541's stack
(sunrpc `225361815EB170B866C5033`, 16 exports, nothing lost).
`cswap131.sh` takes the modules to load from `NFSM`; with
`NFSM="sunrpc nfsv3 nfsv4"` the client runs the tree's sunrpc
`DF01CCB1C31213C2F060D0C` (switch Y), nfs, nfsv3, nfsv4, lockd,
nfs_acl, grace, nfs_localio, nfs_dio and nfsidmap.  `auth_rpcgss` is
not loaded (the stock one, loaded on demand when gssproxy started,
refuses the new sunrpc); the runs mount NFSv3 with AUTH_SYS.  The
client's NFS server stays stopped, as it was.
