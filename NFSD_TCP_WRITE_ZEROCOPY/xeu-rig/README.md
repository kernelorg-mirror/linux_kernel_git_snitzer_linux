# The xeu rig at 16 KiB: where it stands (2026-10-09, 19:10, on build 462 with the whole stack loaded)

**Build 462 is running with every change of 9 October in it, and the
whole stack carries 38.0 GB/s at the median of three traced runs
(36.7 to 38.3), against build 461's 35.9.** The account is "Build 462:
the whole stack of changes" in `../XEU_CONVERGENCE.md`. Steps 0 to 5
of "The boot after build 461" below are done. **Step 6, what each
change is worth, waits (Mike, 2026-10-09): do not start it unasked.**
Step 7 has not been run.

## tardis1 crashed in the loan cell (2026-10-09, evening): read first

After the three runs above, the session started the loan cell, traced,
twice: once with no linger and once with `svc_tcp_rx_linger_us` 50. It
meant to run the region resize test next. **tardis1 crashed and did
not reboot itself; it was unreachable until a hard power cycle.** The
console was not read, and it is not yet known which of those runs was
going when it crashed.

**The likely cause is a stack overflow in the batched return** ("SUNRPC:
Return a loaned record's page-pool pages in batches", `bdd837db04f1`).
`svc_tcp_rx_release_refs()` handed `page_pool_put_netmem_bulk()` up to
64 pages at a time. That function collects the pages whose last
reference it drops into an on-stack `bulk[XDP_BULK_QUEUE_SIZE]` (16
entries) and does not check the bound. A loaned 1 MiB WRITE at 16 KiB
is 64 or more pages of one pool, and nfsd usually holds their last
references, so nearly every loaned WRITE wrote up to 48 entries past
the end of that array on an nfsd thread's stack. Only a loaned record
takes this path: every copy-mode run of build 462 had the patch loaded
and never ran it. The loan cell was the first run that did.

Three commits at the tip:

| commit | what |
|---|---|
| "SUNRPC: Keep page-pool return batches within the bulk API's bound" | the batch is `XDP_BULK_QUEUE_SIZE`; `Fixes: bdd837db04f1` |
| "SUNRPC: test releasing more page-pool pages than one bulk batch in the svcsock KUnit suite" | `svcsock_rx_page_pool_bulk_release_test`: 65 pages of one pool, held only by the record, all back in the pool's ring after the release |
| "SUNRPC: Refuse to build a page-pool return batch the bulk API cannot take" | a `BUILD_BUG_ON` in `svc_tcp_rx_release_refs()`: a batch over `XDP_BULK_QUEUE_SIZE` fails the build (checked: 64 does) |

`svcsock_kunit` passed 43/43 on the buggy module because no test
dropped a last reference inside a batch, so nothing was collected. The
new test passes on the fixed code. **Never run it against a sunrpc
from before the fix.** It is expected to overflow the stack there and
can take down whichever host runs it.

**On the boot after the power cycle:**

1. **The sunrpc with the bug is still installed.** It is
   `/lib/modules/$(uname -r)/updates/sunrpc.ko` (srcversion
   `988D31FBE6BF6DC88282CB6`) and loads again on boot. Copy mode does
   not reach the bug; a loan does. Before any loan cell, run
   `./build-sunrpc.sh` from the tip and `./swap-sunrpc.sh`, then
   `../run-kunit.sh`: svcsock should be 44/44.
2. Read the crash from the boot before: `journalctl -k -b -1` if the
   journal is persistent, or else the BMC console. A
   `__stack_chk_fail` panic, or a bad PC, under
   `svc_tcp_rx_release_refs` or `page_pool_put_netmem_bulk` confirms
   the cause. Anything else means it is still open: the linger, and
   the region resize ("page_pool: Give the fixed receive region back
   when it is resized"), are then the suspects.
3. `sysctl kernel.panic=10` before any load, so that a panic reboots
   the box instead of leaving it hung.
4. Then the loan cell's two runs again (no linger, linger 50), and the
   region resize test only after them.

The state below is from before the crash. A reboot resets everything
in it except the installed modules.

## Is the linger worth its CPU in copy mode: 50, 25, 20 and 0 us

What build 462 says so far (the whole stack, sixteen connections):

| linger | runs | GB/s | CPU-seconds per GB | calls per WRITE |
|---|---|---|---|---|
| 50 us | 3 | 38.3, 38.0, 36.7 | 0.52, 0.52, 0.51 | 2.1, 2.1, 2.3 |
| 0 (`xeu-16k-462-minus-linger`) | 1 | 37.2 | 0.43 | 6.9 |

The linger does its job: a WRITE is received in about 2 calls where it
took 7. But in copy mode it has not shown a gain in rate (37.2 is inside
the spread of the three runs at 50), and it costs about a fifth more
server CPU for each GB (0.52 against 0.43). Much of what it was meant
to save between threads, the page swap already saves: a record that
changes hands no longer frees and allocates its pages. And the copy is
held by the L3 and by how the flows fall onto the rings, not by the
server's CPU (38 GB/s at 0.52 is about 20 of the 64 CPUs). So in copy
mode the linger's worth is open. A shorter wait could keep the calls
near 2 at less CPU, or show that the linger is not needed in copy mode.
For the loan it is a separate question: a loaned record that changes
hands carries its loan table with it.

- **The cell:** `plans/16k-26-copybest.plan` (rings of 4 x 256, 1 MiB
  held buffer), the rest of the stack as on build 462, with
  `RX_LINGER_US` at 50, 25, 20 and 0. The linger is set for each run,
  so nothing needs a build or an nfsd restart and no client is dropped.
  50 is run again as this boot's control, and 0 is the cell that
  answers whether the linger is needed at all.
- **The order:** after the fixed sunrpc is in place. 50, 25, 20, 0, and
  then all four again. Eight runs of about 3 TB is 24 TB on the stripe:
  say so to Mike before the queue, and trim before each run.

      RX_LINGER_US=<us> PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/xeu-16k-<build>-linger<us>-<k> plans/16k-26-copybest.plan

- **What decides it:** `@calls_per_write` and `@write_in_calls_us` in
  `handoff-*.txt`, CPU-seconds per GB, `arch_counter_get_cntpct`'s share
  of the profile, and the rate. The best value is the shortest linger
  that keeps calls near 2, if the rate holds. If 0 runs as fast as the
  others at less CPU, the linger is off for copy mode.
- **How long each wait lasts, which picks the value directly.** "SUNRPC:
  Trace how long a thread lingers for the rest of a TCP record" adds
  the tracepoint `sunrpc:svcsock_tcp_rx_linger`. It fires once per
  wait, with the time waited, the limit, how much of the fragment had
  arrived, and why the wait ended: data, timeout, close or resched.
  `linger.bt` makes histograms of it by outcome, and `trace-sweep.sh`
  runs it for 8 s in every cell, into `linger-*.txt`. The cell's
  tracers now end at about 94 s of its 120. Read the waits that ended
  on data: the limit wants to sit just above most of them. Every
  timeout is a wait spent for nothing. The tracepoint is in sunrpc, so
  it comes with the module build of the tip. Neither the tracepoint
  nor `linger.bt` has run yet. With an older sunrpc, `linger.bt`
  fails to attach and leaves only its error in the file.

## The page-pool work items: what was done (2026-10-09, evening)

T1 and T2 in `../XEU_CONVERGENCE.md` suggested three things for a pool
bound to the region. "Since then", after that document's list of
suggestions, has the account.

| item | state |
|---|---|
| a LIFO in place of the pool's FIFO ring | done: "page_pool: Recycle a region-bound pool's pages last in, first out", in build 462; run only in copy mode, with other changes, never alone |
| a much shorter ring (`xeu`'s `pool_size`) | not done; the LIFO was chosen instead. `rx_refill_thresh` is another knob |
| the DMA mapping kept while a page is in the region | not done; an overflowing page is still unmapped, put on the region's list and mapped again |

**What decides the rest is the loan cell, rerun on the fixed sunrpc**
(step 4 of the crash section). Read `rx_pp_recycle_ring_full` and
`alloc_slow` in `pp-*.txt` against T2's 4.06 million and 254 thousand
in 10 s. Near zero means the LIFO and the rest of the stack have dealt
with it. Still near 9% of returns means keeping the mapping on region
pages is the next change. It is in `net/core` and needs a kernel
build.

## Receive buffers smaller than a page: `rx_buf_size`, and its A/B

The work item, with the driver's code as it is, the change and the
sizes, is "Receive buffers smaller than a page" in
`../XEU_CONVERGENCE.md`'s "Since then". In short:
- `xeu` gets a load-time `rx_buf_size`;
- each receive descriptor takes one piece of a page-pool page, built
  in place as today;
- at 5440 bytes a 9018-byte frame takes 2 pieces, and a 16 KiB page
  holds 3, so 2/3 of a page per frame where today it is a whole one.

**Not written.** It is kmod release 6, on top of release 5's patches.
Lowering the MTU tests nothing: the driver posts a whole page whatever
the frame. 9000 is the driver's maximum MTU, so it cannot be raised
either.

**The A/B.** The cell is `plans/16k-26-copybest.plan`'s copy cell, on
the whole stack with the fixed sunrpc, `BALANCE` on. Each
`rx_buf_size` is a reload of `xeu`. That takes `enp164s0f0` down for
a few seconds, so each reload is Mike's.

| `rx_buf_size` | ring entries | frames the ring holds | held receive buffer | the same data window |
|---|---|---|---|---|
| 0 (a page, the control) | 256 | 256 | 1 MiB | yes |
| 5440 | 512 | 256 | 680 KiB | yes |
| 4096 | 1024 | about 341 | 768 KiB | yes; more frames posted |

Every row holds the same frames and the same data in the receive
buffer. So what changes is the memory per frame, and what that does
to the rate. 680 and 768 KiB are 1 MiB scaled by each size's truesize
per frame (10880 and 12288 bytes against 16384). The ring size has to
be a power of two, so 4096 cannot be held at 256 frames.

The order: load 5440 and run the cell twice; load 4096, twice; load
the default, twice, as this boot's control. Six runs at about 3 TB
each is 18 TB on the stripe; trim before each. After that, if 5440
wins, run it once at the old setting (256 entries, 1 MiB) as well.
That is half the frames posted and more data in the buffer, to see
what the denser buffers allow and not only what they cost.

**What decides it:**
- the rate, at equal frames and equal data window;
- `used_pages`, expected near 2/3 of the control's at 5440;
- `memmove`'s share of cycles, and calls per WRITE (`handoff.bt`);
- CPU-seconds per GB, which carries the cost of twice the
  descriptors and of smaller GRO skbs;
- ring drops and `rx_pp_*`.

If 5440 is not faster and `memmove` is no smaller, the page per frame
is not what holds the copy, and the default stays. The size of a GRO
skb is not measured by anything here. If the CPU per GB rises, that
is the first thing to instrument.

## How the flows fall on the rings: three work items

The largest source of spread between runs of one cell is which ring
each flow hashes to. Build 462's slow run had one ring at 19.9 GB/s
and 440 thousand drops. **`xeu` supports no steering**: `ethtool -x`
and `-X` and `-n rx-flow-hash` were checked on build 461 and are not
there, and its one ntuple rule can only target a VF. The SDK built
into the module does have an RSS redirection table and key
(`xsl_rm_reta_set()`, `xsl_rm_rss_hash_set()`), which `xeu` does not
expose ("`xeu` gives no way to steer" and "rings, how the flows
fall" in `../XEU_CONVERGENCE.md`). So no `ethtool` rule pins flows
here yet. The
work items, all three also in that document's "Since then" list:

1. **More flows than rings.** At 64 connections the rings came out
   even (28, 27, 20 and 25%), where sixteen gave 25, 6, 31 and 37%.
   The 64-connection test below will show it again.
2. **Mount again until the rings are even: `BALANCE=<percent>`** to
   `run-budget.sh`, and so to `sweep.sh` and `trace-sweep.sh`. It
   applies to the hardware client's step cells with `STEP_JOBS`.
   Before the cell is measured, the harness:
   - mounts and runs the cell's own writers for `BALANCE_SECS`
     (default 4), on files of their own, deleted after;
   - reads each ring's `rx_bytes` from `ethtool -S`;
   - if the busiest ring took more than `<percent>`, unmounts and
     mounts again. Each mount gets new client source ports, so new
     hashes.

   It tries at most `BALANCE_TRIES` mounts (default 6). If none comes
   within the limit, the cell runs on the last mount and says so.
   Each try is a line of `<cell>/balance.txt`: the busiest ring's
   share, every ring's share, and the client's source ports. A try
   whose ports are the same as the one before is marked, since then
   remounting changed nothing. The tracers' 35 s count from the
   cell's start, after the balancing, as before. `geometry.txt` names
   the setting.

   For four rings `BALANCE=35` is a guess to start from: an even fall
   is 25%. Build 462's busiest ring carried 19.9 GB/s in the slow run
   and 14.5 in the fast one; the shares were not read. It does not make the
   flows even. It stops one bad fall from deciding a cell. **Not run
   yet:** that the client's ports change with each mount, and how
   many tries a balanced fall takes, are the first things to read.
3. **Steering in `xeu`**: a kmod change. **The first of it is
   written** (9 October, evening; built, not run):
   `kernel-rpmtree/xsight/xeu-expose-the-RSS-redirection-table-hash-key-and-function-to-ethtool.patch`,
   untracked there and not in `xsight.spec`. It wires `ethtool -x`/`-X`
   to the SDK's existing calls:
   - the redirection table, sized by the new load-time
     `rss_indir_size` (default 0 means no table, as before; at most
     255);
   - the 68-byte key;
   - the hash function, of which only Toeplitz can be set. Spooky,
     the firmware's default, shows as unknown.

   It also adds `get_rx_ring_count`, without which 7.1's `ethtool` core
   refuses any table. What `ethtool -X` set is programmed again after
   every reconfiguration of the port.

   It applies at `-p0` after Patch0 to Patch4, and its four files
   compile with no new warnings (also at W=1) against the 7.1.13 tree.
   The whole module could not be built here: the vendor's
   `xeu_ipsec.c` needs XFRM offload, which this host's 4 KiB config
   lacks.

   **To try it on tardis1** (`build-kmod.sh` with a `Patch5:` line
   added to the spec by hand, Release 6; loading it reloads `xeu`, so
   it is Mike's):

       modprobe xeu rss_indir_size=128     # after rmmod xeu
       ethtool -x enp161s0f0               # the firmware's table
       ethtool -X enp161s0f0 hfunc toeplitz
       ethtool -X enp161s0f0 weight 1 0 0 0   # everything to ring 0
       ethtool -X enp161s0f0 equal 4

   Read the first `ethtool -x`. If it fails, the warning in `dmesg`
   names the first entry that is not one of the port's rings: that is
   the end of the firmware's own table, or its handles are not the
   rings'. After `weight 1 0 0 0`, a short load should land on ring 0
   alone (`ethtool -S` ring counters as deltas). If it does not, the
   hardware indexes fewer entries than were set, so try sizes down to
   the ring count. The rest is the questions in
   `../xsight-steering-email.txt`.

   Still to ask for, in order:
   - the RSS indirection table and key (`ethtool -x`/`-X`). It is the
     cheapest, works with standard tools, and with the key readable it
     places a flow by moving its table entry;
   - ntuple filters (`ethtool -N ... action <ring>`), for two hot flows
     in one entry;
   - interrupt coalescing (`ethtool -C`). It is not steering, but it is
     missing too, and `enable_irq` was 6.1% of samples.

   aRFS and RFS would not help: sunrpc reads with `tcp_read_sock()`,
   which does not record a flow's CPU. The reasons, and the questions
   for Xsight (is there a table, how large, Toeplitz and its key,
   filters, moderation), are under "rings, how the flows fall" in
   `../XEU_CONVERGENCE.md`'s "Since then".

Without `BALANCE`, read the busiest and least busy ring of every run
(`where-*.txt`) before comparing rates. Set aside a run whose busiest
ring is far over a quarter, or say so with its figure.

## Does `pools_per_node` earn its place: 64 connections, 1 pool against 4

"SUNRPC: Let a NUMA node have several thread pools" was written for
T6 in `../XEU_CONVERGENCE.md`. With 64 connections and the one pool,
512 threads waited 5.25 s in 10 s on its queue of ready connections
(`__lwq_dequeue`). The build 462 runs had four pools but only sixteen
connections, and at sixteen that lock is not near the top of the list
on either build (S6). So nothing yet says the patch does its job. The
test is the load that showed the problem, run with 1 pool and with 4.

- **The cell:** the copy at its best setting, rings of 4 x 256 and a
  1 MiB held buffer (`plans/16k-26-copybest.plan`), with
  `STEP_MOUNTS=4`. That is four addresses of the server on
  `enp161s0f0`, each with nconnect 16, so 64 connections; the harness
  adds the addresses for the cell and removes them after. The rest of
  the stack is as for build 462: linger 50, `rx_refill_thresh` 8,
  deferred frees at 128. T10's 64-connection copy (21.1 GB/s) was on
  rings of 4 x 128 with a 256 KiB buffer and dropped 190 thousand
  frames, so it is not the figure to compare with. The comparison is
  between the two runs here.
- **The order:** after the sunrpc from the tip is in place (the crash
  section above). Changing the pool count is
  `systemctl stop nfs-server`, `echo <n> >
  /sys/module/sunrpc/parameters/pools_per_node`,
  `systemctl start nfs-server`, `echo 512 > /proc/fs/nfsd/threads`,
  then check `cat /proc/fs/nfsd/pool_threads` (n pools). The value is
  read only when nfsd's service is created. **Stopping nfsd drops
  every client, including the other one on `enp164s0f0`, so each
  change is Mike's.** Run 4 pools, then 1, then 4, then 1. At about
  3 TB a run that is 12 TB on the stripe; trim before each run
  (`TRIM=`).

      STEP_MOUNTS=4 RX_LINGER_US=50 PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/xeu-16k-<build>-m4-pools<n>-<k> plans/16k-26-copybest.plan

- **What answers it:**
  - In `perf lock contention`, `__lwq_dequeue`'s contended count and
    time waited with 1 pool against 4. 4 should cut it by about the
    number of pools. If it does not, the patch is not doing its job.
  - `/proc/fs/nfsd/pool_stats`, saved by hand before and after the
    sweep (the plan is one cell; the harness does not save it). Each pool's
    packets and wakeups show whether the 64 connections spread over
    the four pools. The pool is picked by a hash of the transport, so
    expect an uneven split, not an even one.
  - Then the rate, CPU-seconds per GB, drops and RTOs.
  - Judged as one change of many (see "How to judge a change here"):
    if the lock's wait time goes away and the rate does not move, the
    patch still did its job, and something else is the limit.

The state of tardis1 (it does not outlive a reboot, except where said):

- Kernel `7.1.13-21.hs.462.el9.aarch64+16k`. **sunrpc is the module
  built from the tip, in `/lib/modules/$(uname -r)/updates/sunrpc.ko`**
  (srcversion `988D31FBE6BF6DC88282CB6`; the kernel's own
  `sunrpc.ko.xz` is where it was). `svcsock_kunit.ko` and
  `xdr_kunit.ko` built with it are beside it. They stay across a
  reboot of this kernel; `pools_per_node` does not, being given to
  `modprobe` by `swap-sunrpc.sh` and in no file of `/etc/modprobe.d`.
- `pools_per_node` 4 (four pools of 128 threads; chosen by the
  session, Mike has not said 4 or 1), `svc_tcp_rx_linger_us` 0 until a
  run sets it, nfsd at 512 threads.
- `kmod-16k-xsight` 2.0.1-5 for build 462 is installed and loaded
  (the kmods of builds 459 and 461 went with the upgrade);
  `rx_refill_thresh` is **8**, `rx_ip_align` 2.
- A 32768-page region is bound to `0000:a1:00.0`, rings of 4 x 256,
  `alloc_fail` 0. `/dev/md0` is mounted at `/hs/stripe` by hand.
- This boot wrote about 12 TB to the stripe. No client other than the
  harness's was connected at any check.

How the module was put in: `./build-sunrpc.sh` (it now gives the
module BTF, without which `nfsdlat.bt` and `handoff.bt` do not
attach), then `POOLS_PER_NODE=4 ./swap-sunrpc.sh`, which stops the NFS
services, unloads sunrpc and what depends on it, installs and starts
them again. `../run-kunit.sh` wants nfs-server stopped and leaves it
so: afterwards `systemctl restart hs-nfsd-blksize; systemctl start
nfs-server; systemctl restart hammerspace-nfsd-tuning` and set the
threads. The module build warns that the compiler is not the one the
kernel was built with.

What is left open, by what the runs showed: how the sixteen flows fall
on the four rings (the one slow run of three had a ring at 19.9 GB/s
and 440 thousand drops); the linger's busy wait, 14% to 17% of cycles;
`memmove`, still a quarter of them; a 9000-byte frame with 16 KiB to
itself (now a work item: "Receive buffers smaller than a page"
above). The driver's `rx_refill_thresh` patch is still uncommitted in
`kernel-rpmtree`.

**The footprint against the L3 is the next lever for the copy.** Build
462 used 2052 to 2291 region pages, 32 to 36 MiB, which is the whole
L3. On build 461 the copy's time was set by how much of the region was
in use: a 1 MiB WRITE's calls took 224 us with 24 MiB in use, 269 with
32 and 357 with 48, and `memmove` rose from 24% of cycles to 38%
("The copy with a larger held buffer", F2). With `memmove` still 25%
to 27% of cycles, whatever keeps the pages in use under 32 MiB should
show directly in the copy's time. First, rings of 4 x 128 on the
whole stack, against 4 x 256. On build 461 that ring size used
24 MiB and ran at 35.6 GB/s, but dropped 1.5 million frames. On build
462 the drops at 4 x 256 fell from 250 thousand to about 3 thousand,
so what to read is whether 4 x 128 now holds without dropping. The
cell is `plans/16k-27-copy128.plan`, run the same way as
`16k-26-copybest.plan`, two or three runs of each: how the flows fell
onto the rings moved build 462's runs by 4% on its own.

**The linger's 14% to 17% of cycles in `arch_counter_get_cntpct` is
the busy wait itself, not an extra cost of reading the clock.** The
wait loop in `svc_tcp_rx_linger()` calls `ktime_get_ns()` on every
pass, to check the deadline. On arm64 that reads the counter as
`isb; mrs cntpct_el0` followed by `arch_counter_enforce_ordering()`: a
pipeline flush and a dependency on the value. The rest of a pass is
two bit tests, `need_resched()` and a `yield`, so nearly every sample
taken while a thread lingers lands in the counter read. A session
suggested reading the clock only every N passes to win that back
(2026-10-09, in chat only). That would not work: the samples would
move to the bit test and the `yield`, and the core would stay held
until data came or the time ran out. What costs CPU is how long a
thread lingers, which is why CPU per GB went from 0.49 to 0.52.
Three ways to reduce it, none written or run:

- **A shorter linger.** A piece is about 13 us in coming, and 50 was a
  first guess. Planned: "Is the linger worth its CPU in copy mode"
  below.
- **Sleeping instead of spinning.** The thread sleeps until
  `svc_data_ready()` wakes it, with a timer bounding the wait. One
  thread still receives the record and no core is held, at the cost of
  a wakeup's latency for each piece.
- **`WFE` instead of `yield`** (`smp_cond_load_relaxed()` on the
  flags word). The core idles more cheaply until the flag's cache
  line changes, but it is still held for that thread.

## Before the reboot into build 462 (2026-10-09, 17:30)

**Read this section first; the handoff below it was written before the
16 KiB boot and is kept for the rig's rules and its scripts.** The
results are in `../XEU_CONVERGENCE.md`: "16K" (sizing), "Hero" and what
follows it (the fast runs), **"Correction, 2026-10-09"**, which
changes how every native figure from the stripe is read, "The last
runs on build 459", and **"Build 461"**, the first runs on the kernel
with the stacking-alignment change.

**From 14:30 on 9 October Mike's direction is copy mode. Start at "The
boot after build 461" just below.** Steps 4 and 5 of build 461's list,
further down, were not started and are not the priority.

## The boot after build 461: start here

Mike built a kernel from `origin/…-WIP3` (its tip is "AGENTS.md: a
commit message carries no disclaimer about the work's state") and is
installing it. **It has three of the five code patches written on
9 October; the other two are sunrpc's alone and can be built as a
module** (the table in "The patches" below).

What 9 October found, in the order it matters for copy mode
(`../XEU_CONVERGENCE.md`, from "Build 461" to "The copy with a larger
held buffer"):

1. On build 461 nfsd's copy on the stripe is written direct.
2. **The harness had deferred skb frees off in every run before 15:30
   on 9 October**; they are on now (`SKB_DEFER_MAX`, default 128).
3. At a 256 KiB held buffer a 1 MiB WRITE is received in 17 calls, a
   GRO skb at a time, by a different thread nine times in ten, and
   its connection is copied from for 44% of the time.
4. **With a 1 MiB held buffer the copy carries 35.6 GB/s on rings of
   4 x 128 and 35.9 on 4 x 256** (27.7 at 256 KiB; the loan 30.3).
5. With the waiting gone the copy's own speed is the limit and it
   falls as the region in use passes the 32 MiB L3: rings of 4 x 512
   drop nothing and carry 32.8. Whatever has a page reused sooner or
   fewer pages idle is worth rate.

**A change is one of many** (Mike; the paragraph "How to judge a
change here" below). Keep what does its job and measure them
together.

On the new kernel, in order. **Everything goes in first and the whole
is measured (Mike, 2026-10-09): the two sunrpc patches and the
driver's parameter are loaded before any cell is run, not after the
kernel's three have been measured alone.** All three of those are
off until set (`svc_tcp_rx_linger_us` 0, `pools_per_node` 1,
`rx_refill_thresh` 32), so the kernel's three patches by themselves
are still one run away, with nothing to unload. And straight after a
boot is the cheapest time for both swaps: no region is bound, and the
clients have to be dropped once and not twice.

0. `cat /sys/block/md0/queue/dma_alignment` is 3 and `enp161s0f0` has
   its address. **Run nothing on veth.**
1. **The sunrpc module.** `./build-sunrpc.sh` builds `sunrpc.ko` from
   this tree for the running kernel (it needs that kernel's `-devel`;
   a minute) and says whether nfsd can load against it as it is: no
   export missing and none with another version. It builds
   `svcsock_kunit.ko` beside it. **Putting it in place stops nfsd,
   unmounts every NFS mount and drops every client, the other client
   on `enp164s0f0` too: ask Mike, and keep the installed
   `sunrpc.ko` beside the new one.** If `pools_per_node` is wanted
   for the first runs, set it before nfsd starts again
   (`echo 4 > /sys/module/sunrpc/parameters/pools_per_node`;
   `cat /proc/fs/nfsd/pool_threads` then shows four pools).
2. **The driver.** The refill threshold is Patch4 of
   `kernel-rpmtree/xsight/xsight.spec`, uncommitted ("The driver's
   refill threshold is a module parameter" below). If Mike's build
   did not carry it, `T=$L/kmod-xsight-16k-build-r5 ./build-kmod.sh`
   builds release 5 for the running kernel; installing it and
   reloading `xeu` takes `enp164s0f0` down for a few seconds and is
   Mike's (the commands are in the handoff further down, with 5 for
   4). Then: `grep -c xeu_rx_refill_timer /proc/kallsyms` is 1,
   `rx_ip_align` is 2, `rx_refill_thresh` is 32, both ports have their
   addresses.
3. `mount -o noatime /dev/md0 /hs/stripe` (`mdadm --assemble --scan`
   first if the array is not there), `./bringup.sh 32768 256`,
   `echo 512 > /proc/fs/nfsd/threads`.
4. The KUnit suites, `svcsock_kunit` above all: the page swap changed
   `svc_tcp_save_pages()`, `svc_tcp_restore_pages()` and
   `svc_tcp_clear_pages()` (`../TESTING.md` has how they are run).
   Before any load.
5. **The whole stack**, the copy at its best setting, traced, three
   times (about 3 TB each; the stripe is trimmed before each by
   `TRIM=`):

       echo 8 > /sys/module/xeu/parameters/rx_refill_thresh
       RX_LINGER_US=50 PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/xeu-16k-<build>-stack1 plans/16k-26-copybest.plan

   **Load every tracer at idle first** (`timeout -s INT 3 bpftrace
   nfsdlat.bt`, the same for `handoff.bt`; `handoff.bt` has
   `XPT_QTIME_OFFSET`, which is of the kernel, and says how to read
   it): a cell was thrown away on a tracer that had not loaded.
   Against build 461's one traced run of that cell: 35.9 GB/s, 0.49
   server CPU-seconds per GB, 2054 region pages, 6.4 calls for a
   WRITE, 269 us in them, `memmove` 30% of cycles, 250564 frames
   dropped. What shows each change is doing its job: in `pp-*.txt`,
   `rx_pp_recycle_ring_full` near zero and `rx_pp_alloc_refill`
   taking what `rx_pp_recycle_cached` does not (the stack);
   `__folio_batch_release` and `alloc_pages_bulk` gone from the
   profile (the page swap); in `handoff-*.txt`, calls for a WRITE
   near 1 and the next call by the same thread (the linger, which is
   a busy wait: read the CPU for each GB with it); `__lwq_dequeue`
   down the contended list (the pools). The values 50 and 8 are first
   guesses: a piece of a WRITE is about 13 us in coming.
6. Then what each is worth, by taking one away at a time from the
   whole: `RX_LINGER_US=0`, `rx_refill_thresh` 32, `SKB_DEFER_MAX=0`;
   `pools_per_node` needs nfsd restarted.
7. Not run on any kernel: a load paced at the source (`STEP_RATE`,
   below); 64 connections at a 1 MiB buffer (planned: "Does
   `pools_per_node` earn its place" above); a 2 MiB buffer; rings of
   4 x 128 against 4 x 256 (why it matters now: "The footprint
   against the L3" at the top).

`smartctl` is not installed, so the drives' wear is not read; 9 October
wrote about 90 TB to the stripe, 15 TB a drive.

**Mike rebuilt the kernel from the tip on 9 October and rebooted into
it (build 461). A session has no memory of the days before it:
everything known is in these files and in
`/var/tmp/zc-run-logs/` (which is on the root file system and
survives).** Commits here end with `Assisted-by:` and a `Signed-off-by:`
of the configured git identity (`git commit -s`), never
`Co-Authored-By` (AGENTS.md, "Commit trailers"). Mike corrected that
once already.

## The state of tardis1 now (build 461, 2026-10-09 13:30)

- Kernel `7.1.13-21.hs.461.loanpages_pool.el9.aarch64+16k`;
  `kmod-16k-xsight` 2.0.1-4 for it is installed and loaded (retry
  symbol present, `rx_ip_align` 2). It was checked, not rebuilt.
- A **32768-page** region is bound to `0000:a1:00.0`; the port is at
  4 rings of 256. `alloc_fail` 0.
- `/dev/md0` assembled by itself and is mounted at `/hs/stripe`
  (`mount -o noatime`, by hand: it is still not in `/etc/fstab`).
  Its `dma_alignment` is 3.
- nfsd is at **512** threads (the boot's own count is 128).
- `enp164s0f0` came up with the boot and has its address. It is not
  ours and was not touched. Nothing has run on veth.
- `smartctl` is not installed on this root, so the drives' wear was
  not read. This boot has written about 18 TB to the stripe.

## The state of tardis1 before the reboot (build 459)

A reboot undoes some of this: no region is bound, the port is back at
its 8 rings of 1024, nfsd at its own thread count, `enp164s0f0` is
probably up again (leave it; it is not ours), `/hs/stripe` is not
mounted (it is not in `/etc/fstab`) and the array may come up as
`/dev/md127` and not `/dev/md0`.

- Kernel `7.1.13-21.hs.459.loanpages_pool.el9.aarch64+16k`, `xeu` with
  the refill retry (kmod 2.0.1-4), `rx_ip_align` 2.
- `enp164s0f0` is **down** (Mike, 2026-10-08) and every other mount of
  tardis1 was torn down. Both wait for him.
- A 6144-page region is bound to `0000:a1:00.0`. It cannot be resized
  on this kernel. The port was left at 4 rings of 256.
- **`nvme2` to `nvme7` are an MD raid0, `/dev/md0`, XFS at
  `/hs/stripe`** (1 MiB chunk; not in `/etc/fstab`; made with
  `mdadm --create /dev/md0 --level=0 --chunk=1024 --raid-devices=6`).
  What they held is gone. Their lines in `/etc/fstab` and `/etc/exports`
  are commented out with `#stripe 2026-10-08-stripe#`; the files as they
  were are `/etc/fstab.2026-10-08-stripe` and
  `/etc/exports.2026-10-08-stripe`. `mdadm` was installed. `nvme0` and
  `nvme1` are as they were.
- The drives took about 20 TB each on the night of 8 to 9 October
  (3% to 4% of their endurance used; `xeu-16k-smart-*.txt` in the run
  logs). **Trim the stripe before a run** (`TRIM=/hs/stripe` to
  `sweep.sh`): untrimmed it gives up a fifth.
- nfsd is back at 128 threads; the drives' and `md0`'s `write_cache`
  are back at `write back`; `null_blk` is loaded and unused.

## What was found, in one place

1. Sizing (8 October): rings of 4 x 128, a 256 KiB buffer and a budget
   of four WRITEs are the smallest setting that kept up, at 73% of the
   L3, under a load that the client's verifying fio thread held to
   2.2 GB/s. Use `STEP_JOBS` for anything about rate.
2. On the stripe, durable, three runs each: **the loan 30.1 GB/s, the
   copy 25.4** (sixteen connections, 8 MiB writes, 512 nfsd threads;
   the loan at 16 WRITEs, 4 x 256, 1 MiB; the copy at 4 x 128,
   256 KiB).
3. **Neither was written direct.** `md0` asks for 512-byte memory
   alignment where its drives ask for 4, so nfsd's copy goes through
   the page cache there as a loan does: two copies against one.
4. The wire carries 49 GB/s to sixteen plain streams with a 1 MiB
   buffer held. Nothing on the network side holds the loan at 30: not
   the budget past 16, the rings' depth or number, dropped frames,
   connections or files.
5. **Build 461: the copy on the stripe is written direct, and carries
   27.7 GB/s (27.3 to 28.1) against the loan's 30.3 (30.2 to 30.4),
   three runs each.** The loan still goes through the page cache. The
   direct copy costs the server less CPU per GB than the loan (0.48
   against 0.54 CPU-seconds) and holds 770 region pages against 4100.
6. **Traced on build 461** (after the call with Xsight; "Build 461,
   traced" in `../XEU_CONVERGENCE.md`): the pools reuse pages through a
   FIFO and not last in, first out; the rings take 6% to 42% each of
   sixteen flows; a loan through the page cache queues on each file's
   inode lock; the outliers in nfsd are COMMITs; 64 connections on
   rings of 128 made the copy slower (21.1 GB/s). Nothing was changed
   in the kernel for it. `trace-sweep.sh` and `nfsdlat.bt` are the
   tools.

## The patches (2026-10-09)

Five code patches at the tip, by subject. The first three are in the
kernel Mike built from `…-WIP3`; the last two are not:

| commit | what | which mode it is for |
|---|---|---|
| "SUNRPC: Trade pages with the socket when a partial record changes hands" | nfsd no longer frees and allocates a WRITE's pages each time a part-received record passes between threads (T3) | copy |
| "page_pool: Recycle a region-bound pool's pages last in, first out" | a stack in place of the FIFO ring for pools bound to the region (T1) | both |
| "SUNRPC: Return a loaned record's page-pool pages in batches" | the pool's lock is taken for each batch and not each page (T4) | loan |
| "SUNRPC: Let a NUMA node have several thread pools" | `pools_per_node`: several queues of ready connections where 512 threads shared one (T6) | both |

| "SUNRPC: Let a thread wait for the rest of a partly received TCP record" | `svc_tcp_rx_linger_us`: a WRITE is received by one thread, which waits for the next piece and does not park the record (W3) | both |

The last two were written after Mike's build of `…-WIP3` began and are
not in it. The linger is off at 0; `RX_LINGER_US=50` to `sweep.sh`
sets it for a run (a piece is about 13 us in coming), and
`handoff.bt` shows whether it took: calls for each WRITE near 1 and
the next call by the same thread. It is a busy wait, so read the
server's CPU with it. `pools_per_node` does nothing until it is set, and that is read
when nfsd starts: `options sunrpc pools_per_node=4` in
`/etc/modprobe.d` before a boot, or
`echo 4 > /sys/module/sunrpc/parameters/pools_per_node` and a restart
of nfsd, which drops every client's connections and so is Mike's to
do. `cat /proc/fs/nfsd/pool_threads` then shows the pools.

`net/core/page_pool.c` is built in, so they need a kernel build and a
reboot. `svcsock_kunit` covers the save and restore of pages; run it
on the new kernel.

**The testing is of copy mode (Mike, 2026-10-09).** The aim is the
shortest time from a frame landing to its buffer being back in the
pool, by making the copy and the direct write as efficient as they
will go; that path is also what a loan to a drive that takes
arbitrary pieces uses. Each iteration is the copy cell alone,
`plans/16k-23-copy.plan`, which halves what an iteration writes to
the stripe (about 3 TB):

    PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/<outdir> plans/16k-23-copy.plan

The figures to beat, build 461 with sixteen connections and 512 nfsd
threads: **35.9 GB/s with a 1 MiB held buffer on rings of 4 x 256**
(`plans/16k-25-copybuf-rings.plan`, its first line; 0.49 server
CPU-seconds per GB, 2054 region pages, 250 thousand frames dropped),
and at the old setting of 256 KiB and 4 x 128, 27.7 GB/s (27.3 to
28.1 untraced, 27.7 traced), 0.48 server
CPU-seconds per GB, the client `rwnd_limited` 98.5%, `memmove` 16% of
cycles, page frees and allocations about 5%. What to read on the new
kernel: `rx_pp_alloc_refill` against `rx_pp_alloc_empty` in
`pp-*.txt` (the stack in use), `__folio_batch_release` and
`alloc_pages_bulk` gone from the profile, and the rate.

**The driver's refill threshold is a module parameter in
`kernel-rpmtree`, uncommitted (2026-10-09).** `xsight/xeu-make-the-RX-refill-threshold-a-module-parameter.patch`
is Patch4 of `xsight.spec`, whose Release is 5: `rx_refill_thresh`,
default 32 (as the driver is), the empty slots at which a poll refills
the ring, writable while the port is up. It is not committed there
because that repository has no git identity configured for this user
and its AGENTS.md forbids taking one from the history; the spec has no
changelog entry for the same reason. `./build-kmod.sh` built it
against build 461 into `$L/kmod-xsight-16k-build-r5` (the parameter is
in `modinfo`); nothing was installed or loaded. A new kernel needs the
kmod built against it, and loading it reloads `xeu`, which is Mike's
to do. After that
`echo 8 > /sys/module/xeu/parameters/rx_refill_thresh` needs no
reload.

**How to judge a change here (Mike, 2026-10-09): as one of many, not
by whether it alone moves the rate.** A combination of changes can be
what unlocks the rate, and one stage's limit hides the gain of a fix
ahead of it. So a change that does what it is meant to (the counters,
the lock contention, the order of reuse, the cycles in the profile)
is kept and added to the others, and the whole is measured; "no gain
alone" is a result to record and not a reason to drop it or to leave
the next one untried.

**The harness turns deferred skb frees off** (`net.core.skb_defer_max=0`
in `run-budget.sh`, since 4 October, undocumented until 9 October), so
every figure from this rig before 15:30 on 9 October was taken that
way. **The harness's default is now 128, on (Mike);** `SKB_DEFER_MAX=0`
repeats the old behaviour, and a run whose `geometry.txt` does not name
the setting was made with 0. On, half of the copy cell's pages come back
by the pool's lockless cache and the rate is the same (27.3 and
28.0 GB/s): "The harness had deferred skb frees off" in
`../XEU_CONVERGENCE.md`. That is one change alone. The page-pool patch
above makes the other half of the pages, those that overflow the
32-page cache, last in, first out as well.

**How a WRITE is received in copy mode** (`handoff.bt`; "How a WRITE
is received" in `../XEU_CONVERGENCE.md`): 17 calls of
`svc_tcp_recvfrom()` for each 1 MiB WRITE, 89% of them by another
thread than the last, and the connection copied from for 44% of the
time; 38% is waiting for data behind a 256 KiB buffer and 18% is
waiting for a thread.

**The copy's best setting so far, build 461: a 1 MiB held buffer on
rings of 4 x 128 or 4 x 256, 35.6 and 35.9 GB/s** (one traced run
each; "The copy with a larger held buffer" in `../XEU_CONVERGENCE.md`).
Rings of 4 x 512 drop nothing and run at 32.8: once the waiting is
gone the copy's own speed is the limit, and it falls as the region in
use passes the 32 MiB L3 (a 1 MiB WRITE's copy takes 224 us with
24 MiB in use and 357 us with 48). Whatever has pages reused sooner
or fewer of them idle is worth rate from here.

Copy-mode things that need no build and are not run: fewer nfsd
threads (idle threads are woken last in, first out already, so this
may change nothing); 64 connections on rings deeper than 128; a load
paced at the source, for which the harness now has `STEP_RATE`:

    STEP_RATE="24000 27000 30000 33000" PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/<outdir> plans/16k-23-copy.plan

One figure (MB/s, all writers together) paces the whole cell; several
are steps of equal length within it, the same writers on the same
files, each step starting as the one before ends. The log and
`<cell>/ramp.tsv` have each step's rate against what it was offered,
and the cell's MBps is their mean. `where.sh` and the tracers sample
from 35 s in, so with four steps they see the second and third.

## This boot (build 461): steps 0 to 3 are done, 4 and 5 wait for Mike

Step 0 passed with no rebuild; step 1's `dma_alignment` is 3; step 2's
region is bound and the stripe trimmed; step 3 ran three times
(`$L/xeu-16k-461-best1` to `-best3`, driver
`$L/xeu-16k-461-best-driver.sh`) and is written up as "Build 461" in
`../XEU_CONVERGENCE.md`. **Steps 4 and 5 have not been started.** For
step 5, the region has been set once this boot and never resized.
`setsid` forks when started from a shell that leads its process group,
so the PID `$!` gives is not the sweep's: find it with `pgrep -xf` of
the exact command line, then `tail --pid`.

Three things were waiting for this kernel build:

| commit, by its subject | what |
|---|---|
| "page_pool: Give the fixed receive region back when it is resized" | the region is given back when its size changes with no pool bound |
| "block: Let a stacking device take its members' memory alignment" | a stacking device (every MD level, device-mapper) takes its members' memory alignment |
| (no commit) | a larger region: `./bringup.sh 32768 256` |

The steps, in order:

0. **The driver: check it, do not rebuild it.** Mike folded the Xsight
   changes he knows of (the refill retry among them) into
   `kernel-rpmtree`, so the kmod built with the new kernel should carry
   them (Mike, 2026-10-09). Check and go on:
   `grep -c xeu_rx_refill_timer /proc/kallsyms` is 1,
   `cat /sys/module/xeu/parameters/rx_ip_align` is 2, `rpm -q
   kmod-16k-xsight` names the running kernel, and `enp161s0f0` has its
   address. Only if one of those fails is the handoff's
   `./build-kmod.sh` and reload wanted, and then say so to Mike first
   (the reload takes `enp164s0f0` down for a few seconds).
   **Run nothing on veth.**
1. `cat /sys/block/md0/queue/dma_alignment` is 3 (it is 511 without
   the stacking change). If the array does not assemble by itself:
   `mdadm --assemble --scan`, then `mount -o noatime /dev/md0 /hs/stripe`.
2. `./bringup.sh 32768 256`, then `fstrim /hs/stripe`.
3. The copy on the stripe, and that it is written direct now
   (`wr_direct` in `summary.tsv`):
   `PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./sweep.sh $L/<outdir> plans/16k-17-best.plan`
   with nfsd at 512 threads (`echo 512 > /proc/fs/nfsd/threads`).
   That is the comparison the night was thought to be making: a direct
   copy against a loan through the page cache.
   Every cell of 120 s writes about 3 TB to the stripe, and the drives
   are consumer ones at 4% of their endurance: say so to Mike before a
   long queue. Start a queue from a driver script that waits on the
   one before it (`tail --pid`), and wait on the driver.
4. With the room the larger region gives: rings of 4 x 1024 or more
   with a 1 MiB or 2 MiB buffer and budgets of 16 to 64
   (`plans/16k-15-budget.plan` and `16k-20-deeprings.plan` are the
   shapes; none of their larger lines fitted in 6144 pages).
5. To test the region patch: with the port down,
   `echo 16384 > /sys/module/page_pool/parameters/rx_region_pages`, port
   up, and `/sys/kernel/debug/page_pool_rx_region` shows the new size
   and the kernel log the old region freed.

A loan written direct with no hop in front of it has no storage on this
host: these drives need whole pages, a memory-backed null_blk copies
under one lock (6 GB/s) and brd asks for 512-byte alignment. NVMe/TCP
on the same host is the only one, at about 20 GB/s.

## Scripts added on 8 and 9 October

`sweep.sh` takes `TRIM=`, `PRP_DIR=`, `SGL_BACKING=`, `PRP_STORAGE=`;
`run-budget.sh` takes `STEP_JOBS=`, `STEP_BS=`, `STEP_SIZE=`,
`STEP_IODEPTH=`, `STEP_MOUNTS=`, `EXPORT_SYNC=`. `capture-sweep.sh` runs
`where.sh` (both ends' CPUs, the rings, a profile, the client's view of
each connection) and `anat.bt` (what a loaned WRITE waits for) inside
every cell. `nfsd-stacks.sh` says where nfsd's threads are;
`wirebuf.sh` what the wire carries at each held buffer;
`nvme-baseline.sh` what the storage takes; `napi.bt` whether the
receive poll is held off (written, not yet used). **A sweep started
detached is not waited on by anything: start the next one from a driver
script, or it will sit idle** (it did, for two and a half hours).

---

# The xeu rig: 16 KiB handoff (written 2026-10-08 on the 64 KiB boot)

**A session on tardis1 after the reboot into the 16 KiB kernel starts
here.** Mike built the 16 KiB kernel himself and is installing it; this
file and the scripts beside it are everything the last session knew
that the tree did not.

## The job, and its priority (Mike, 2026-10-08)

Find the best sizing of the receive region against the L3 at 16 KiB, on
the real hardware: the `xeu` port `enp161s0f0`, the client
192.168.1.106, the real NVMe as it is (PRP, needs whole pages) and
behind an NVMe/TCP target (takes arbitrary pieces). **The full battery
of sizing runs is the top priority, and solid 16 KiB results are wanted
the same day.** Everything else (docs still owed, repeats of 64 KiB)
waits.

"Sizing" is four numbers and what they cost: the ring size, the held
receive buffer, the budget (in WRITEs of 1 MiB admitted at once), and
so the region pages in use at peak. The answer wanted is the smallest
footprint, against half of the L3 and the whole of it, at which sixteen
connections' loans run level with their copies with no stall and no
drop, on each storage.

## Rules that do not change (XEU_CONVERGENCE.md section 0)

- Load goes only to the harness's own mount of tardis1 on the client.
  Never to the client's other mounts, never to tardis1's other exports.
- Only `/hs/nvme0` and `/hs/nvme1`, as files. No drive is reformatted.
- Only `enp161s0f0`. **`enp164s0f0` carries another client's
  (192.168.0.105) connections and is never touched, so `xeu` is never
  reloaded.** A reboot is Mike's to do.
- Commit at the tip of `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`; no
  regrouping (AGENTS.md).

## First ten minutes on the 16 KiB boot

1. `getconf PAGESIZE` is 16384 and `uname -r` ends `+16k`.
2. `cat /sys/module/xeu/parameters/rx_ip_align` is 2
   (`/etc/modprobe.d` has `options xeu rx_ip_align=2`).
3. **Is the driver's retry patch in?** `grep -c xeu_rx_refill_timer
   /proc/kallsyms` (1 is yes) and `rpm -q kmod-16k-xsight` (release 4
   carries it). The patch is `kernel-rpmtree` commit `f61ea4c` (not
   pushed); it was tested at 64 KiB on 2026-10-08 and works
   (`XEU_CONVERGENCE.md` 5c). **Mike expects it to be missing from the
   16 KiB kmod and wants release 4 rebuilt against the 16 KiB kernel by
   the same method (2026-10-08). Do this before `bringup.sh`:** with no
   region bound yet, the reload needs no care about the rings.

   1. `./build-kmod.sh` builds `kmod-16k-xsight` 2.0.1-4 for the
      running kernel from the `kernel-rpmtree` spec and patches, into
      `/var/tmp/zc-run-logs/kmod-xsight-16k-build`, and checks the
      result: the vermagic must be the running kernel's and "retry
      patch in xeu.ko" must be 1. It installs and loads nothing. About
      two minutes. It needs `kernel-16k-devel` and
      `kernel-16k-modules-core` of the running kernel; if either is
      missing, say so to Mike.
   2. Installing it and reloading `xeu` takes `enp164s0f0` down for a
      few seconds and drops the other client's connections, so **it
      needs Mike's leave, and the session's permissions may refuse it
      even then** (they did at 64 KiB until he gave the commands
      himself). Give him these to approve or to run, each as its own
      prompt starting with `!`:

          ! rpm -Uvh /var/tmp/zc-run-logs/kmod-xsight-16k-build/RPMS/aarch64/kmod-16k-xsight-2.0.1_*-4.el9.aarch64.rpm
          ! rmmod xeu && modprobe xeu

      `rpm -U` removes every other installed `kmod-16k-xsight`, since
      they share a name (at 64 KiB it took the kmods of builds 454 and
      455 with it). If a region is already bound, first
      `echo none > /sys/module/page_pool/parameters/rx_region_dev`: a
      fresh load gives the port 8 rings of 1024.
   3. Afterwards: `grep -c xeu_rx_refill_timer /proc/kallsyms` is 1,
      `rx_ip_align` is 2, both ports have their addresses, and
      192.168.0.105's connections are back
      (`ss -tn state established '( sport = :2049 )'`).

   Without the patch a ring that runs empty stays empty until the port
   is taken down and up; the scripts here bounce the port when the
   region runs dry and the patch is absent, so the sweep can run
   either way, but say which it was.
4. Nothing has made a region: `/sys/kernel/debug/page_pool_rx_region`
   does not exist. **Run nothing on veth this boot** (the veth harness
   would make the region for `rxsrv`).
5. `./bringup.sh 6144 128`: a 6144-page (96 MiB) region bound to the
   port's PCI device and rings of 4 x 128. One boot has one region
   size; 6144 has room for the widest line of the plans (rings of
   4 x 256, sixteen WRITEs, 512 KiB buffers). The region is only where
   the pages come from: what is measured against the L3 is the pages
   in use at peak (`used_pages`), which follows the rings, the buffers
   and the budget and not the region's size.

## Then, in this order

Everything long is started detached and waited on by PID:

    setsid nohup ./sweep.sh <outdir> <plan> > <log> 2>&1 < /dev/null &
    tail --pid=<pid> -f /dev/null      # as a background command

1. **`./ringsize.sh 64 128 256`** (about 6 minutes): a plain network
   load at each ring size, buffers free and held. It says the smallest
   ring that does not tail-drop. At 64 KiB that was 256 for one stream
   left to tune itself and 128 for sixteen.
2. **`./sweep.sh $L/xeu-16k-smoke plans/smoke.plan`** (3 minutes), and
   read its `geometry.txt`. **The harness's one-frame-per-page geometry
   has never run at 16 KiB.** Expected by arithmetic: 1 MiB is 64
   pages of payload and about 118 frames, so a factor near 185 and a
   charge near 120 pages a WRITE; a 128 KiB buffer is 9 pages. If the
   factor is far from that, stop and look (`FRAME_PAGE` in
   `run-budget.sh`). Then check that a budget "of n WRITEs" admits n:
   during a sixteen-connection loan cell,
   `sysctl net.sunrpc.region_budget_stats` should show `inflight`
   reaching n and no more. `sweep.sh` works the budget out as
   n x CHARGE + reserve + 4; if `inflight` tops out below n, CHARGE is
   low (trace one cell with `credit.bt`: `ADMIT ... credit=` is what
   the kernel charges).
3. **`plans/16k-1-core.plan`** (1 h 50 min; the lines are in order of
   priority and a stopped sweep resumes). Rings of 4 x 128, both
   storages, buffers of 256, 128 and 512 KiB, budgets of 1, 2, 4 and 8
   WRITEs, with each buffer's copies.
4. **`./summarize.py <outdir>`** prints the sizing table: copy and loan
   at one and sixteen connections, the pages in use and their share of
   the L3, CPU per GB, and a note where a cell stalled, dropped, paused
   or ran the region dry.
5. **`plans/16k-2-rings.plan`** (70 min) at the buffer the core showed
   best (edit the plan's buffer first), then **`plans/16k-3-wide.plan`**
   (30 min).
6. **Repeats of the shortlist.** A sweep skips lines listed in its
   `done` file, so a repeat is the same plan into a new outdir;
   `summarize.py` takes several outdirs and shows the median and range.
   `PERF=1` adds cycles and instructions per cell; `PROFILE=1` a 30 s
   flat profile by function per cell (not both).
7. Record as it comes in a "16K" section of `XEU_CONVERGENCE.md` shaped
   like its "64K" one, and tell Mike the table.

## What 64 KiB showed, to set 16 KiB against

The account is `XEU_CONVERGENCE.md` section "64K" (18 numbered
findings). In short, at 64 KiB on this card:

- a 1 MiB WRITE was about 117 pages, 7.3 MiB; rings that did not drop
  were twice the L3; no working setting fitted in half of the L3 (the
  best cells peaked at 1500 pages, 94 MiB);
- sixteen connections on a 128 KiB buffer (two frames) and a budget of
  one WRITE stalled, 9 untraced runs of 9, through TCP backoff on the
  client; a 256 KiB buffer or a budget of four WRITEs ended it;
- with a 512 KiB buffer and a budget of six WRITEs the loan and its
  copy were level, about 1.4 to 1.5 GB/s at one connection and 2.0 to
  2.3 at sixteen; one cell did not repeat (1979, then 569);
- no saving in server CPU showed for the loan (0.84 against 0.84
  kernel CPU-seconds per GB on NVMe/TCP, 0.35 against 0.45 on the local
  NVMe);
- baselines: the drive 10.4 GiB/s, the wire 7.0 GB/s to one connection
  left to tune itself, 3.9 GB/s to sixteen at 128 KiB, 0.9 to 1.2 GB/s
  to one at 256 KiB.

At 16 KiB a frame still has a page to itself, but the page is a
quarter the size: a WRITE is about 1.8 MiB of region, the L3 is 2048
pages, and a 128 KiB buffer is eight frames. Whether the stall, the
level rates and the misses change with that is what the plans show.

## What went wrong before, so it does not again

- **`pgrep -f` and `pkill -f` match the shell that runs them.** A queue
  of jobs each waiting for "no `run-budget.sh` process" waited on
  itself all night, and a `pkill -f` killed its own shell. Wait on a
  PID (`tail --pid`), kill by PID.
- **Ring counters are zeroed by a ring change and by down/up.** Read
  `rx_hw_tail_drop_packets` before, not after. `sweep.sh` reads it
  inside each cell.
- **The harness's derived budget is wrong on this card.** It reads the
  parked pages too early; the pools then keep several hundred more. A
  derived budget ran the region dry and stopped a ring. Always
  `FORCE_BUDGET` (which `sweep.sh` does) and let the guard skip what
  does not fit.
- **`cells.tsv`'s `drops` column is the interface's, not the ring's**,
  and reads about 125 for every cell whatever the load.
- **The `seq-sized` cells are too short to compare rates** (512 MiB,
  under a second). Use the 120 s `step` cells.
- **Attaching `credit.bt` hides the sixteen-connection stall.** Trace
  to read the accounting, never to decide whether a cell stalls.
- **One run is not a result.** The same cell ran at 1979 and at 569.
- **The region is named by the PCI device** (`0000:a1:00.0`), and
  `ethtool -G` works only with the port up, `-L` only with it down.
- **`fio` returns 0 from a stalled cell.** Read the rate and
  `admitted`.

## Where things are

- `common.sh`, `build-kmod.sh`, `bringup.sh`, `ringsize.sh`, `sweep.sh`,
  `summarize.py`, `plans/`: this directory. `rcv.py` and `snd.py` are `ringsize.sh`'s
  sender and receiver (only the server's receive buffer is set).
  `credit.bt` is the admission trace.
- `../run-budget.sh`: the harness (`NETDEV=`, `CLIENT=`, `STORAGE=`,
  `EXPORT_DIR=`, `BACKING=`, `FORCE_BUDGET=`, `FORCE_RCVBUF=`,
  `CELLS=`).
- Run output: `/var/tmp/zc-run-logs/` (`xeu-*`; it is on the root file
  system and survives the reboot). The 64 KiB runs are `xeu-q-*`,
  `xeu-step-*`, `xeu-cpu-*`, `xeu-llc-*`, `xeu-copy-*`.
- The client: `ssh -i /root/.ssh/id_ed25519_snitzer2 root@192.168.1.106`
  (x86_64, 4 KiB pages, 48 CPUs; it has other work, so its idle CPU is
  not zero).
- The artifact: "NFSD Receive Region Budget",
  <https://claude.ai/artifact/3UQkMVoWW8p9A5ZASPnm3g>. Its source is
  `../artifacts/region-budget.html` (also
  `/var/tmp/zc-run-logs/artifact/region-budget.html`); publish to the
  same URL. Its 64 KiB section opens with the summary of the xeu runs
  (`id="xeu64"`); a 16 KiB one belongs at the top of the 16 KiB section
  when there are results, and the page's lede names the rigs.
- Still owed, after the 16 KiB results: `transplant/tardis1-build457-handoff.md`,
  `transplant/README.md` and `RESULTS.md` say nothing of the xeu runs.
