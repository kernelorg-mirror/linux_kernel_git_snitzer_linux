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

## Result, first four runs (18 November, 15:55 to 16:09)

| Run | Client's stack | M IOPS (ports) | p99 a; b (ms) | Server busy, us a READ | Client busy (busiest) | Client us a READ | Client CPUs min / median / max |
|---|---|---|---|---|---|---|---|
| t2-r1058 | B | 3.10 (1.59 / 1.51) | 2.41; 2.61 | 76%, 15.69 | 77% (88%) | 11.92 | 66 / 76 / 89 |
| t2-r1059 | P | 3.08 (1.55 / 1.54) | 2.47; 2.54 | 77%, 16.00 | 76% (82%) | 11.84 | 66 / 76 / 82 |
| t2-r1060 | B | 3.03 (1.40 / 1.63) | 2.47; 2.44 | 75%, 15.84 | 76% (88%) | 12.04 | 66 / 76 / 88 |
| t2-r1061 | P | 3.06 (1.55 / 1.52) | 2.41; 2.51 | 76%, 15.90 | 77% (85%) | 12.08 | 69 / 76 / 85 |

B: 3.065 M at 11.98 us of the client's CPU a READ; P: 3.07 M at 11.96.
Per-CPU `rpccnt` and version counts alone save nothing.  The reason,
read from the client's `/proc/net/rpc/nfs` after each run: its `rpc`
line's third count, `rpcauthrefresh`, is raised for every call as well
(`call_reserveresult()` sends every call through `call_refresh()`), by
a plain increment on the rpc_stat's same line, so P still passed that
line between the client's CPUs on every call (round 131's switch left
it too).

**The shared counts undercount.**  Under B, `rpc` counted 66.5 and 66.0
million calls where its own `proc3` counts summed to about 81 million
(18% lost); under P the two agree to the call (80,986,894 against
80,986,844 in t2-r1059: the 50 more are NFSACL calls, counted in NFS's
rpccnt).  `rpcauthrefresh` lost as much under both (66 to 68 million).

Mike: "make rpcauthrefresh per-CPU in the same commit".  Folded into
the SUNRPC commit, `6a8bd94c11924`, whose message now leads with the
undercount (Mike: "point out in the commit header ... that these stats
undercount due to increments colliding"); the tips before are kept as
`kernel-7.3/canary.pre-fold-authrefresh`.  Built at W=1 with no
warning.

Then (Mike: "please just do a single 1062:P, I need us to move onto the
next ... have 1063 be a quick run P and iodepth of 96"): P rebuilt on
the client from the tip, `ab-pcpu.sh 1062:P 1063:P:96`, set beside
B's two runs above and round 130's iodepth 96 run (t2-r1053, 3.36 M).

## Result with rpcauthrefresh per CPU (18 November, 16:10 to 16:23)

P rebuilt on the client from `e476bfc6bb64e` (sunrpc
`C4A23C35035DD3A1E1C967B`).  t2-r1062 measured nothing: it started while
a git worktree, added on tardis2 to reword the SUNRPC commit, filled the
root disk; nvme8 logged three WRITE timeouts (aborted, status 0) at
16:13 to 16:14, port b gave no answer for 20 s at the run's start, and
the run stopped (the host back at the settled setting after it).  Mike:
no worktrees or bulk IO on tardis2's root disk while runs go ("nvme8 is
fragile and I think it has been the cause of crashes").  The commits
were then rewritten with git plumbing, and 1062 was run again as 1064.

| Run | Client's stack | iodepth | M IOPS (ports) | p99 a; b (ms) | Server busy, us a READ | Client busy (busiest) | Client us a READ | Client CPUs min / median / max |
|---|---|---|---|---|---|---|---|---|
| t2-r1058, r1060 | B | 64 | 3.065 (mean) | | 15.77 (mean) | | 11.98 (mean) | busiest 88, 89 |
| t2-r1064 | P | 64 | 3.00 (1.45 / 1.55) | 2.54; 2.41 | 73%, 15.57 | 74% (83%) | 11.84 | 65 / 74 / 83 |
| t2-r1053 (round 130) | build 541's own | 96 | 3.36 (1.71 / 1.65) | 3.65; 4.69 | 83%, 15.81 | 85% (90%) | 12.14 | |
| t2-r1063 | P | 96 | 3.50 (1.85 / 1.65) | 3.75; 3.78 | 85%, 15.54 | 83% (88%) | 11.38 | 74 / 84 / 88 |

At iodepth 64, P's one run: -0.14 us of the client's CPU a READ
against B's two, at 3.00 M against 3.065 (within the spread of these
runs, 3.03 to 3.10).  At iodepth 96, against round 130's run on build
541's own client stack (which costs about what B does at iodepth 64,
12.08 against 11.98 us): **3.50 M against 3.36 (+4.2%), the client's CPU
a READ 11.38 against 12.14 us (-0.76 us, -6.3%)**, the server's 15.54
against 15.81.  At that rate the client's ceiling (48 / 11.38 us,
4.22 M) is now above the server's (64 / 15.54 us, 4.12 M).  No new
kernel warning in the runs that ran.

**Every count exact under P:** `rpc` and `rpcauthrefresh` both read
91,559,431 after t2-r1063 and 79,270,502 after t2-r1064, the `proc3`
counts 50 below (NFSACL).

The client is back on build 541's stack (`cswap131.sh stock` with the
client-side NFSM: sunrpc `225361815EB170B866C5033`, 16 exports); the
server at the settled setting.
