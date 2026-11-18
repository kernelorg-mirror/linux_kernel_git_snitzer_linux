# Round 132: the client's call counts per CPU

Mike, 18 November: "yes, do step 2".  Round 131 showed that skipping
`call_start()`'s two shared increments (the rpc_stat's `rpccnt` and the
version's count of the procedure) saves the client 0.33 us of CPU a
READ (-2.7%) and gives +1.5% IOPS.  This round keeps the counts and
moves them per CPU:

- `6a8bd94c11924` "SUNRPC: Count client calls per CPU, as plain
  increments lose counts": `rpc_stat_alloc_counts()` gives an rpc_stat a per-CPU
  `rpccnt` and a table of per-CPU counts for its program's versions;
  a version's are allocated at its first client (`rpc_new_client()`),
  because NFS's versions register after its rpc_stat is set up.
  `call_start()` raises the per-CPU counts when they are there, the
  shared ones otherwise (an rpc_stat that does not ask, or NFSACL and
  LOCALIO clients that share NFS's rpc_stat).  The proc file sums both.
- `e476bfc6bb64e` "NFS: Count the client's calls per CPU": nfs's
  per-netns rpc_stat asks for it in `nfs_net_init()` and frees it in
  `nfs_net_exit()`.  `/proc/net/rpc/nfs` now counts the namespace's
  own calls, as the server's file does.

Both built at W=1 with no warning (stats.o, clnt.o, nfs's inode.o).

## The A/B

No switch can change `struct rpc_stat`'s layout, so the client swaps
stacks between runs.  `cbuild132.sh` (round 131's, with the directory as
argument) builds two module roots on the client: `/root/zc-ckern/B`
from `3ce8cc6e90857`, the tip before the change (round 131's counting
arm, without its switch), and `/root/zc-ckern/T` from the tip.
`ab-pcpu.sh 1058:B 1059:P 1060:B 1061:P` puts the client on each arm's
stack with `cswap131.sh` (`NFSM="sunrpc nfsv3 nfsv4"`), checks its
sunrpc srcversion, runs `settled-iops.sh` with `nfs4k.sh`, and prints
IOPS, CPU a READ on both hosts and the client's `/proc/net/rpc/nfs`
(the per-CPU sums at work).  The server stays on round 129's stack at
the settled setting.  `cswap131.sh stock` at the end.
