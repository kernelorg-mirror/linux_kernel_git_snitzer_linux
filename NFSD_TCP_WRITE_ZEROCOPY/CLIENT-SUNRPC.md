# The client's sunrpc: holding its work to the NIC's node

The rig's NFS client (106; two nodes, CPUs 0-23 near port a's `ens3np0`
and 24-47 near port b's `ens6np0`) runs Hammerspace build 469,
`7.3.0-0.rc7.2.hs.469.el9.x86_64`, from the tag
`v7.3.0-0.rc7.2.hs.469.el9`.  FLAMEGRAPH round 91
(`FLAMEGRAPH/91-client-far-share/README.md`) is the account of what is
below.

## 12 November: C, the splice of a WRITE's pages (Mike: "get the client updated")

**The client's `updates/sunrpc.ko` is now C: S with one more commit,
`kernel-7.3/hs-469.sunrpc-splice` (`edf08ea097e5`, "SUNRPC: Splice a
request's pages into the TCP socket", `patches/client/0002`).**  FLAMEGRAPH
round 98 found the two-port rate held at 72 GB/s by the client: its
`xprt_send_pagedata()` sent a WRITE's pages as a bvec with no
`MSG_SPLICE_PAGES`, so `tcp_sendmsg_locked()` copied every payload byte
under the socket lock, in rpciod's transmit of one RPC at a time a
connection (about 3 GB/s a connection; 18% of the client's 48 CPUs in
that memcpy, `t2-r531`).  The client sent its pages with
`kernel_sendpage()` until 0472e4766049 (2019) and copied since.

The change: `xs_tcp_send_request()` sets `MSG_SPLICE_PAGES` when the
task has not been sent before (`RPC_WAS_SENT()`, the test
`xprt_request_transmit()` itself uses for a retransmission), and
`xprt_sock_sendmsg()` applies it to the page array only, clearing it
for the head and tail (kvecs over the request's slab buffer, which
`skb_splice_from_iter()` refuses with a `WARN_ON_ONCE` and -EIO when a
page is not `sendpage_ok()`).  A retransmission is copied, the rule of
a6b31d18b02f ("SUNRPC: Fix a data corruption issue when retransmitting
RPC calls", 2013): the reply to the first transmission may complete the
call and free the pages to be rewritten while the socket still holds
them for the second.  Only the TCP transport asks; UDP and AF_LOCAL copy.
`xdr->bvec` is what `xs_stream_prepare_request()` already allocates for
every stream request.  Compile-checked on tardis2 against its own kernel
headers and clean under checkpatch.

Built on the client as B was, `make -C /usr/src/kernels/<v> M=/root/zc-csunrpc/C/sunrpc
CONFIG_SUNRPC_GSS= CONFIG_SUNRPC_XPRT_RDMA= modules` on `git archive`'s
`net/sunrpc` of the branch: srcversion `AB674CEE87351D814F68CEE`, its
`Module.symvers` identical to B's (no header changes), so the rpm's NFS
modules load on it as on S.  Installed over `updates/sunrpc.ko`,
`depmod -a`, and loaded with `cswap.sh stock` at 05:49 on 12 November with
nothing mounted: `rpciod` and `xprtiod` strict 1 (the udev rule), the
client's NFS server and 16 exports back.  **To go back to S:** copy
`/root/zc-csunrpc/B/sunrpc/sunrpc.ko` over `updates/sunrpc.ko`, `depmod
-a`, `cswap.sh stock`.  A run's `geometry.txt` names the srcversion.

Not yet run against the server: tardis2 crashed at about 05:10 that
morning and lost port a on the boot after it.  The first run is in
`HANDOFF-tardis2-canary.md`, "Next boot: after the crash of 12 November".
What to watch on the client after a run: `dmesg` for a `WARN_ON_ONCE` in
`skb_splice_from_iter()` (a page that was not `sendpage_ok()`; none is
expected, the client's send pages being pinned user pages, page cache
pages or `alloc_page()`'d ones, and `nfs4_buf_to_pages_noslab()` exists
for this), and `cli_retrans` in `results.tsv` (the copied path).

## Settled (Mike, 10 November): S

**The client runs with `rpciod` and `xprtiod` held to the node of the
CPU that queues their work.**  What is in place on 106:

- `/lib/modules/7.3.0-0.rc7.2.hs.469.el9.x86_64/updates/sunrpc.ko`: the
  client's tag with `WQ_SYSFS` on both workqueues
  (`kernel-7.3/hs-469.sunrpc-wq-sysfs`, `4fddf1fecd4b`), srcversion
  `85AFD4D91848A02D6133EEF`, built on the client against its
  `kernel-devel` (its source and build in `/root/zc-csunrpc/B/`).  Only
  `sunrpc.ko`: the rpm's NFS modules load on it unchanged (its exports'
  CRCs are the rpm's).  `depmod -a` was run; the initramfs has no
  sunrpc, so every boot loads this one.  **Since 12 November the file
  is C (above), S plus the splice; S's own build stays in `B/`.**
- `/etc/udev/rules.d/99-zc-sunrpc-wq-strict.rules`: `affinity_strict` 1
  on `rpciod` and `xprtiod` whenever they are made.
- `xeu-rig/twoport.sh`: `CLI_STRICT=1`, the default, writes and checks
  `affinity_strict` 1 on both before a run and refuses to run on a client
  without them; `CLI_STRICT=0` runs without (and writes 0).  Their scope
  stays `default`, which `CLI_SCOPE` sets to `numa` for a run.  The
  run's `geometry.txt` names the client's sunrpc srcversion.

To undo: remove the two files, `depmod -a`, and swap sunrpc
(`FLAMEGRAPH/91-client-far-share/cswap.sh stock`, which loads whatever
`modprobe` resolves) or reboot the client.  tardis1 shares the client and
gets this too.  The arms A and F of `run398.sh`, `run407.sh`,
`run411.sh` and `run423.sh` were run before it: run again, the udev rule
would make A's and F's workqueues strict too.

Why S and not this tree's series (F, FX, GX below): in round 91 all
three end the far sends and save 5 to 7 points of the client's CPU at
rates inside each other's spread, and S is two flags on the client's
own kernel.

## Why

`twoport.sh` reports the share of each client port's packets sent from
the far node's CPUs.  With fio pinned to the near node and the client's
`workqueue.default_affinity_scope` at `numa` it still ran 1% to 3%, and
varied from run to run.  Traced on the client (`net:net_dev_start_xmit`
by sending CPU, with call chains), 97% of those sends are `rpciod`
running `rpc_async_schedule()` and the rest `xprtiod`.  Both are unbound
workqueues whose affinity is not strict, and neither has `WQ_SYSFS`, so
nothing can make them strict.

## The change, and the first result

`kernel-7.3/hs-469.sunrpc-wq-sysfs` is the client's tag and one commit,
`4fddf1fecd4b` "SUNRPC: Make rpciod and xprtiod visible in sysfs"
(`patches/client/0001-...`): `WQ_SYSFS` on both, so that
`/sys/devices/virtual/workqueue/{rpciod,xprtiod}/affinity_strict` can
be set.  Nothing changes until it is.

On tardis2 (build 522, loan mode on two ports, three runs a side,
`t2-r392..r397`), with `affinity_strict` 1 on both against the same build
without it:

| | GB/s | far share | client idle | retransmits |
|---|---|---|---|---|
| not strict | 71.53 | 1.0% to 2.3% | 28.1% | 1,397 |
| strict | 71.66 | **0.0%** | **31.7%** | 2,068 |

The far sends are gone in every strict run and the client spends about
1.7 CPUs less at the same rate.  The server's ring drops, and with them
the client's retransmits, rise by about half: not yet understood.

## This tree's client series, factored out

This tree (`kernel-7.3/canary`) already carries the Flynns' client work
that goes further: `rpciod` per-CPU, the first RPC leg of an O_DIRECT
pgio run in the caller, a transport per CPU under nconnect, and those
transports homed on the cores of their NIC's node
(`sunrpc.xprt_numa_map`, LAB ONLY, default Y, read only).  None of it is
in the client's tag.  `kernel-7.3/hs-469.client-sunrpc` is the
WQ_SYSFS branch with eleven of this tree's commits picked onto it, in
order:

- SUNRPC: Balance switch queue length on transport removal
- SUNRPC: Balance queue accounting when relocating a task
- NFS: allow nconnect up to 128
- SUNRPC: give each CPU its own transport under nconnect
- SUNRPC: hand a sent task from sending to pending without a requeue
- SUNRPC: let NFS pgio and commit run the first RPC leg in the caller
  (`[snitzer: …]`: hs.469 has no NOWAIT pgio, so every O_DIRECT pgio
  opts in)
- SUNRPC: make rpciod a per-CPU workqueue (`[snitzer: …]`: rpciod keeps
  WQ_SYSFS)
- SUNRPC: Shard the rpc_clnt task list to remove cl_lock contention
- NFS: Do not revert the iov_iter of a queued async O_DIRECT request
- SUNRPC: home nconnect transports on their NIC's NUMA node
- SUNRPC: home each switch's transports on distinct cores

The two conflicts were context only (`fs/nfs/pagelist.c`,
`net/sunrpc/sched.c`).  The series changes the sunrpc headers, and with
them `struct rpc_clnt`, `rpc_xprt` and `rpc_xprt_switch`, so every module
on sunrpc is rebuilt with it, not sunrpc alone.

## Building and loading on the client

Without a new client kernel: the client has `kernel-devel` for build
469, Secure Boot off and module signatures not enforced.

- `FLAMEGRAPH/91-client-far-share/cbuild.sh`, in `/root/zc-ckern` on the
  client with `A/` and `F/` each holding `net/sunrpc`,
  `fs/{nfs,nfsd,lockd,nfs_common}` and the sunrpc headers of one branch
  (`git archive`): a copy of `kernel-devel` per branch with its headers
  laid over, the 18 modules of those directories built against it, and
  a module root `<v>/root` where they stand in for the rpm's.  They are
  exactly the 16 modules that depend on sunrpc, with grace and nfs_dio.
- `cswap.sh` (in `/root/zc-csunrpc` on the client) swaps them: with no NFS
  mounted, it stops the client's own NFS server and helpers (it exports
  volumes for other work), unloads every module on sunrpc, loads the
  NFS modules from a module root with `modprobe -d` (or stock), and
  starts what was running again.  Nothing is installed in
  `/lib/modules`: a reboot brings stock back.  tardis1 shares the
  client, so no tardis1 run may be on while it swaps (`client_free`).
