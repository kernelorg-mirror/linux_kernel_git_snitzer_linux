# Review of positions 31-45: region budget, early loan release, small records, window hold, page swap

Tree: `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-code` on `v7.3-3`. Proposed
messages are in `new-31.txt` .. `new-45.txt`.

## Series-level observations for this stretch

**This stretch is a dev log, not a patch series.** Commit 31 adds the
admission design. Then 32, 33, 34, 35, 36, 37 and 39 carry `Fixes:` tags
against 31 or 34, and 43 and 44 correct 31's design without saying so
(43: "budget 0 = unlimited loans" deadlocks the server; 44: SUNRPC writing
`tcp_sock` fields from `->sk_data_ready`). Upstream, a reviewer reading 31
would reject it for exactly the problems 32-44 fix, and Fixes tags on
unmerged commits are noise that stable tooling would act on. **Fold
32, 33, 34, 35, 36, 37, 39, 43 and 44 into 31 (and 38)**, then re-split
the result along design lines, not along the order bugs were found:

1. `SUNRPC: count the fixed-region pages an skb range adds`: the helper
   from 34/36 (`svc_tcp_rx_skb_region_pages()`, `region_count`,
   `region_last`).
2. `SUNRPC: bound TCP receive loans of region pages with a budget`: the
   admission and charge, reserve and overrun, oldest-first wake, the
   orphan path (32), live reconfiguration (33/39), region-only admission
   (34), wait-list hygiene (37, 39's `svc_data_ready()` change), and
   "budget 0 copies" (43) as the semantics from day one.
3. `SUNRPC: hold a region transport's receive buffer and window`: the
   receive buffer, clamp and ratio, applied lazily (35) and only through
   TCP's helpers (44, which uses the TCP helper added in commit 07).
4. Per-namespace budgets (38), if they are kept at all (see below).

**Ordering bugs within the stretch, if it is not folded:**
- 37's message says "svc_data_ready() no longer enqueues it", but that
  change is in **39**, which comes later. 37's new helper
  `svc_tcp_rx_credit_waiting()` carries a comment about the data_ready
  behaviour that does not exist yet at 37. 39 (dated Oct 3) belongs
  before 37 (dated Oct 6).
- 39 says `Fixes: 941958c6324f` (31), but its code depends on 38
  (`svc_rx_budget_pools`, `svc_rx_budget_cfg_lock`, the
  `net.sunrpc.region_budget_*` sysctls, `svc_tcp_rx_budget_initial()`).
  It actually fixes 38's "a budget is applied only when the pool is next
  used" as much as it fixes 31.
- 43 has no Fixes tag, but it fixes a server hang in the design of 31/38
  (budget 0 meant unlimited loans of region pages).
- Commit 07 (`tcp: let a kernel socket cap the receive scaling ratio`)
  justifies itself by "SUNRPC ... rewrote tp->scaling_ratio ... from its
  data_ready callback". Once 44 is folded into 31, that code never
  exists, so 07's message needs rewording to state the need directly.

**Tunables.** By position 45 this stretch has added the module parameters
`svc_tcp_rx_budget_pages`, `svc_tcp_rx_budget_reserve`,
`svc_tcp_rx_budget_default_pages`, `svc_tcp_rx_budget_default_reserve`,
`svc_tcp_rx_credit_factor`, `svc_tcp_rx_rcvbuf`, `svc_tcp_rx_copy_below`
and the read-only `svc_tcp_rx_budget_stats`/`_default_stats`, plus the
sysctls `net.sunrpc.region_budget_{pages,reserve,stats}`, on top of 24's
`svc_tcp_rx_loan_pages`. Some of these are coupled (budgets are silently
off unless `svc_tcp_rx_rcvbuf >= 128 KiB`, with only a `pr_warn_once`).
Chuck has been moving nfsd configuration to netlink (`nfsdctl`) and away
from module parameters. Writable module parameters whose `set` callbacks
take mutexes and walk per-netns lists (38/39) will be questioned, and
statistics exported as a module-parameter string will be rejected
outright (use tracepoints, or netlink/procfs stats). Expect demands to:
- derive the reserve from the maximum record size (`sv_max_mesg`) rather
  than configure it;
- drop `svc_tcp_rx_credit_factor`, a guess whose valid range of 0..22400
  is set by TCP's fixed-point scale, in favour of measuring
  (`region_count` already measures what the factor estimates);
- drop `svc_tcp_rx_copy_below` (42) in favour of a constant;
- choose **one** configuration interface: per-netns, nfsd netlink or
  sysctl. Not module parameters for init_net, sysctls for every netns and
  a third "default" budget for the rest.

**Global spinlock and O(n) list walks.**
`svc_tcp_rx_credit_wait_locked()` and `svc_tcp_rx_credit_link_locked()`
insert in ticket order by linear walk under the per-budget `spin_lock_bh`,
and the stats getter walks the whole waiters list under that lock. With
thousands of connections a maintainer will ask why the lists are not
FIFO with `list_add_tail()`. Tickets are handed out monotonically, so the
only out-of-order insert is a paused record re-queuing with its old
ticket, and that case could get its own head-of-queue rule.

**Layering.** 31's `svc_tcp_rx_hold_window()` writes `tp->scaling_ratio`,
`tp->window_clamp` and calls `__tcp_adjust_rcv_ssthresh()` from SUNRPC's
`->sk_data_ready`. Netdev (Eric Dumazet) would NAK that on sight. 44
removes it, but 31 must never contain it. Even after 44,
`svc_tcp_rx_hold_buffer()` reaches TCP_WINDOW_CLAMP through
`sock->ops->setsockopt(... KERNEL_SOCKPTR ...)`. `kernel_setsockopt()` was
removed precisely to stop that, and the preferred form is a
`tcp_sock_set_window_clamp()` helper next to `tcp_sock_set_nodelay()`
(`tcp_set_window_clamp()` already exists in net/ipv4/tcp.c and only needs
a locked, exported wrapper).

**Is per-namespace (38) needed for a first merge?** The region itself is
global (a page_pool property of the devices), and the budget only makes
sense for loans of region pages. Per-netns budgets with a default budget
and a "sum <= region, else ENOSPC" check is a lot of policy for a
first submission. I would defer 38 to a follow-up, or reduce the whole
interface to the per-netns sysctl, if the series must have containers
from day one.

**Independent commits that could leave this stretch:** 42 (copy small
records) concerns the classifier and belongs right after 15 ("Publish
prefix-only loan records as COPY"). 45 (page swap on save and restore) is a
generic svcsock change that applies to plain upstream without any
loan machinery and could go to Chuck on its own, ahead of the series.

---

## 31 941958c6324f SUNRPC: Region credit admission (TCP flow control instead of ring drops) (David Flynn)

**Sequencing.** It sits after the page_pool region commits (25-30) it
depends on, which is the right place. But, as above, it is the head of a
chain of nine follow-up fixes. In its present form it contains:
- admission at the first body byte for all transports (34 reverses that);
- one global budget (38 replaces it);
- the buffer and window clamp applied at `svc_tcp_init()` to every
  transport (35 moves it);
- the per-skb `tcp_sock` rewrite (44 removes it);
- budget 0 meaning "no limit" (43 reverses that);
- configuration applied only when idle (33 reverses that);
- per-fragment charging (36 corrects that);
- no orphan handling (32 adds it).

Folding is mandatory for upstream. It also mixes two changes that should
be split: credit admission (svcsock record accounting) and receive buffer
and window locking (TCP socket configuration).

**Upstream merit.** The mechanism is defensible: flow control by not
reading, so the window closes instead of the NIC ring starving, and a
reserve so the oldest record always completes. Problems a maintainer will
raise:
- `svc_tcp_rx_hold_window()` writing TCP internals from `sk_data_ready`:
  a netdev NAK.
- The coupling "budget is silently off unless `svc_tcp_rx_rcvbuf >= 128 KiB`"
  (`svc_tcp_rx_budget_on()` / `svc_tcp_rx_budget_configure_locked()`).
  A budget that is configured but not in force, reported only by
  `pr_warn_once`, will be called a trap. Either the budget implies the
  buffer lock, or the write is refused.
- `svc_tcp_rx_credit_factor`: a guess at overhead, with
  `SVC_TCP_RX_CREDIT_FACTOR_MAX` 22400 derived from
  `TCP_RMEM_TO_WIN_SCALE`, and "an eighth kept back" in
  `svc_tcp_rx_window_clamp()`. These magic numbers need justification
  or have to go.
- `svc_tcp_rx_budget_stats` as a module parameter: reject; use a
  tracepoint or netlink.
- The "overrun" (`credit_over`) lets the oldest record exceed the budget
  without limit. That makes it a soft budget, which should be said plainly
  in the code comment ("never holds more than its credit" in the block
  comment is not true).
- In `svc_setup_socket()`, `INIT_LIST_HEAD()` sits under `if (svsk)` before
  the NULL check; move it after the check.
- The `credit_*` fields are added to a `____cacheline_aligned` state
  struct, but there is no word on layout.

**Message.** Unacceptable as is: "patch 16", "Lima qualification", "region
dossier" (a sentence that ends in a colon with nothing after it),
"Origin: ours", "Authors/attribution", an AI co-author line,
"Slipstream overlay ... series 18/20", "navigator page ... delivery
package", and three `[snitzer:]` notes that are internal bookkeeping (base
adaptation to the series' own earlier commits, the KUnit split, a naming
history). The subject is not in imperative form, and its parenthetical
reads like a marketing line. The body does not explain why stopping the
reader is safe (the locked buffer). The proposed message fixes all of that.

## 32 2cf8d648e6af SUNRPC: Orphan a credited record whose loans cannot be accounted for

**Sequencing.** A fix to 31 (Fixes: 31, **within the series: fold into
31**). Self-contained.

**Upstream merit.** This handles a state that should be impossible
(`svc_tcp_rx_refs_valid()` failing is already a `WARN_ON_ONCE`). Making
the impossible path not wedge the host is reasonable, but the extra
`orphaned` stats field that exists only to count a WARN is debugging
weight; the WARN suffices. The deeper issue a reviewer will raise is that
the callers leak the record (`svc_tcp_rx_state_free()` "leaks it"). That
is the bug, and the credit is a symptom.

**Message.** Good, clear and short. The closing sentence about
`svc_tcp_rx_reset()` being unchanged is noise. "Orphan" is fine.

## 33 cc12e16f3e32 SUNRPC: Apply region budget changes while records are in flight

**Sequencing.** Fix to 31 (in-series Fixes: **fold into 31**).

**Upstream merit.** Correct direction; applying changes only when idle
was simply wrong. Negative `avail`/`reserve` counts are a subtle
invariant. They are documented in the comment, which is good. The
`!c->budget` branch in `svc_tcp_rx_credit_charge()` that turns everything
into overrun is fine once 43's "budget 0 copies" semantics are in, but at
this point it makes "off" mean "unbounded", which 43 later calls a
deadlock. Fold both and settle the semantics once.

**Message.** Adequate. "turning no budget" is a typo for "turning the
budget off".

## 34 4175422d80e7 SUNRPC: Credit only the region pages a TCP receive loan holds

**Sequencing.** Fix to 31 (in-series Fixes: **fold into 31**). It also
moves the admission point, and that move is what makes 37 necessary.
Folded, the design is simply "admit at the first region page".

**Upstream merit.** Necessary: a region budget that throttles loopback
and other NICs is wrong. `page_pool_page_in_rx_region()` on every frag of
every skb in the actor is a per-packet cost for all svc TCP traffic once
`svc_tcp_rx_rcvbuf` is set (35 adds a second walk per skb for the hold
check). A reviewer will ask for a cheaper per-skb test, for instance a
flag on the skb or the page_pool.
`virt_to_page(skb->data + ...)` for the linear head assumes a
lowmem head, which is true for skb heads but worth a comment.

**Message.** Lab measurement ("390 pages ... 70 to 19 MB/s") is
specific to a test rig; the problem statement stands without it.

## 35 360337514116 SUNRPC: Hold the receive buffer only on transports that receive region pages

**Sequencing.** Fix to 31 (in-series Fixes: **fold into 31**, into the
buffer and window half once 31 is split).

**Upstream merit.** It is right to stop overriding TCP autotuning on
unrelated transports. However, `sk_rx_hold_pending`/`sk_rx_held` are plain
`bool`s written in the actor under the socket lock and read in
`svc_tcp_recvfrom()` after release. That is fine because the same thread
reads them, but it deserves a sentence. `svc_tcp_rx_hold_buffer()` is
forward-declared mid-file. Its parameter description still says "for TCP
transports set up afterwards", which is no longer the behaviour
(transports that already exist pick it up at their next region page).
The once-only lock means a later change of `svc_tcp_rx_rcvbuf` never
reaches a transport that is already held; a reviewer will ask about
that.

**Message.** Fine; minor tightening.

## 36 df7b94354b4f SUNRPC: Charge a TCP receive loan for the region pages it holds, not its fragments

**Sequencing.** Fixes 34 (in-series: **fold into 34, and so into 31**).

**Upstream merit.** The accounting change is right. `region_last` makes
the count depend on fragment order, so it is an upper bound and not an
exact count. That is acceptable but must be documented, and it is.
Clearing `region_last` on any materialize or retract is coarse but safe.
The motivating case (veth packing frames into 64 KiB pages) is a test
topology; real NICs with page_pool fragment support (`page_pool_alloc_frag`)
do the same, and the message should say that instead.

**Message.** Overdone: host name, build number, pause counts, MB/s pair.
Replace with the generic statement (done in the proposed message).

## 37 76bb156fc1f1 SUNRPC: Take a transport that reads on without admission off the credit wait list

**Sequencing.** Fixes 34 (in-series: **fold**). Out of order with 39, as
noted above: its message and its helper's comment describe 39's
`svc_data_ready()` change, which has not happened yet.

**Upstream merit.** The fix is a band-aid on a wait-list protocol that is
hard to reason about. A reader that "reads on without asking" while still
queued indicates that queue membership is not tied to the state that
justifies it. Folded into a redesign, membership should be cleared
whenever the actor makes progress without a pause, which is what this
does. Fine as a final rule, but it should be designed in, not appended.
`list_empty_careful()` on another CPU's list node, without the lock, is
acceptable only because a false answer is benign; the comment says so.

**Message.** Heavily overdone: tardis1, the build string, "drgn", "14
transports", a narrative of how it was found. The mechanism paragraph is
good; keep only that.

## 38 0168f65db212 SUNRPC: Give each network namespace its own region budget

**Sequencing.** Correct after the budget mechanics. 39 then fixes it
(without saying so). 43 changes what 0 means in it.

**Upstream merit.** This is the commit most likely to be pushed back on
as policy:
- There are three configuration paths for one thing: module parameters
  for init_net, module parameters for a "default" budget used by
  namespaces without their own, and per-netns sysctls that *also* cover
  init_net and write through to the init_net module parameters
  (`svc_tcp_rx_budget_sysctl()`). Pick one.
- `svc_tcp_rx_budget_set()` sums the budgets against
  `page_pool_rx_region_size()` under a mutex, but the region can be
  resized (29) and nothing re-checks the sum then.
- `svc_tcp_rx_budget_for()` picks the budget per call from mutable
  configuration. The rule "the one it waits on, else its namespace's if
  set, else default" will look arbitrary to a reviewer.
- The `sunrpc_net` init error label is `err_credit` but undoes pipefs;
  name it for what it unwinds.
- A stats sysctl that returns a formatted multi-field string through
  `proc_dostring` is not how netns sysctls are meant to be used.

Recommend deferring this commit, or collapsing configuration to per-netns
sysctls only and dropping the default budget and the module parameters.

**Message.** Clear about what it does. It does not justify why
containers need separate shares of a NIC-global region (who runs nfsd
in multiple netns on one region NIC?). Add one sentence of motivation or
drop the commit.

## 39 c6cf07b4bfa7 SUNRPC: Do not re-queue a transport that waits for region credit

**Sequencing.** Fixes 31 (in-series: **fold**). It depends on 38 despite
its Fixes target, and must precede 37. It mixes two changes: (a) the
`svc_data_ready()` skip and (b) "a configuration change is a wake event",
which converts three module parameters to callbacks and hooks the sysctls.
(b) is needed because of (a), so the two belong together, but both belong
in the base design.

**Upstream merit.** (a) is a real improvement: the threads should not
spin a transport that cannot be admitted. The early return in
`svc_data_ready()` is still after `sk_odata()` and the tracepoint, which
is correct. (b) adds `set` callbacks on writable module parameters that
take locks and walk lists (`svc_tcp_rx_budget_apply_all()` takes
`svc_rx_budget_cfg_lock` from a param `set`, under the kernel_param
lock). A reviewer will dislike that, and it is a reason to move
configuration off module parameters entirely.
`svc_tcp_rx_budget_apply_all()` calls `svc_tcp_rx_budget_apply()` while
holding the mutex, which takes `spin_lock_bh` and enqueues an xprt;
lockdep-legal, but heavy for a sysfs write.

**Message.** The figure "567498 refused against 230163 admitted" is
precise lab data. "Several refusals per admitted record" carries the
point. Otherwise clear.

## 40 b20e046c9136 nfsd: Release a WRITE's receive loan after the page-cache copy, before storage (David Flynn)

**Sequencing.** Logically independent of the budget (it only calls
`svc_tcp_rx_credit_release()` in the transport). It could sit right after
the nfsd loan commits (23), with the credit release added when the
budget lands; where it is, it is acceptable. 41 must be folded into it.

**Upstream merit.** The idea is attractive, but several things will draw
fire from Chuck and Christoph:
- **It changes the stable-write path for every transport.**
  `nfsd_vfs_write()` now always strips `IOCB_DSYNC|IOCB_SYNC` from buffered
  and DONTCACHE writes and calls `vfs_fsync_range()` itself, even on RDMA,
  on UDP and on TCP with loans off, where `svc_rqst_release_payload()` is
  a no-op. The result is equivalent to `generic_write_sync()`, but the
  restructuring has to be justified for all of them or made conditional
  on the request actually holding a loan (an `RQ_` flag that the
  transport sets, or a return value from `svc_rqst_release_payload()`).
- `RQ_PAYLOAD_SHARED` is set and cleared around one call in
  `nfsd4_write()` to pass one bit to `nfsd_vfs_write()`. Chuck will ask
  for a parameter or for the decision to be made in the caller (call
  `svc_rqst_release_payload()` from the NFSv3 and NFSv4 WRITE procs after
  `nfsd_vfs_write()` returns, rather than from inside the VFS helper,
  which is also used by other paths). The latter is cleaner layering, but
  it gives up releasing before the fsync. In that case pass a flag.
- **Possible use after release in NFSv4 (not fixed by 41):** the COMPOUND
  tag. `nfsd4_decode_compound()` saves `argp->tag` with
  `svcxdr_savemem()`, which returns a pointer into the receive buffer
  unless the bytes were in scratch; `nfsd4_proc_compound()` sets
  `resp->tag = args->tag`, and `nfs4svc_encode_compoundres()` copies
  `resp->tag` into the reply *after all operations have run*, so after
  the early release. Only the first 28 bytes (`SVC_TCP_RX_FIXED_BYTES`)
  are copied; the tag follows the credentials and can lie in a loaned
  page. Linux clients send an empty tag, but others do not. This needs
  verifying and fixing (copy the tag at decode, or never release early
  when `taglen != 0`), and the same audit is needed for anything else
  that is encoded at the end of a COMPOUND, or traced from `rq_arg` after
  the WRITE.
- `svc_tcp_rx_release_payload()` silently does nothing for a deferred
  request (`rqstp->rq_deferred`) and for anything not PUBLISHED. That is
  fine, but there is no tracepoint for the "kept" case.

**Message.** Unacceptable as is: "Origin: ours", attribution with an AI
co-author, "Slipstream overlay", "navigator page", "Tunables: ...
(existing) (default unchanged by this patch)" (it adds no tunable), the
p99 figure, and the KUnit-split `[snitzer:]` note. The first paragraph
duplicates the second. The proposed message states the problem, the new
API and the stable-write restructuring.

## 41 d00eb537d4e0 nfsd: keep a WRITE's receive loan while a later operation may read it

**Sequencing.** Fixes 40 (in-series: **fold into 40**). It is a
use-after-free fix to an unmerged commit; upstream must never see 40
without it.

**Upstream merit.** The allow-list (GETATTR, COMMIT) is conservative and
correct in direction. Chuck may prefer the opposite approach: make
`svcxdr_savemem()` copy when the receive buffer is loaned, or mark ops
in `nfsd4_ops[]` (an `OP_` flag such as `OP_ARGS_BY_VALUE`) rather than a
switch in nfs4proc.c that will rot as operations are added. See the tag
issue under 40: scanning only the *later* operations misses state that
the COMPOUND itself keeps.

**Message.** Good. The lower-case "keep" in the subject is inconsistent
with 40's "Release"; nfsd subjects upstream are mixed, but be consistent.

## 42 5f4e4323ad1f SUNRPC: Copy small RPC records instead of loaning a receive page (David Flynn)

**Sequencing.** Independent of 31-41 in code (it changes
`svc_tcp_rx_classify()` only). Its *motivation* (region credit) depends on
the region, but the general argument (a few hundred bytes are cheaper to
copy than to pin a page) stands alone. Move it next to 15.

**Upstream merit.** It is sensible. The module parameter
`svc_tcp_rx_copy_below` is a tunable nobody should need; make it a
constant (or tie it to `PAGE_SIZE`/`SVC_TCP_RX_FIXED_BYTES`). The
check is placed before the RPC direction/version checks in
`svc_tcp_rx_classify()`, so a malformed small record now reports "small"
instead of its real reason; that is harmless but worth ordering after the
sanity checks.

**Message.** Unacceptable as is: "early write loan release feature smoke
(patch 19, AWS early-rc2)", cell counts, p99, "Origin: ours",
attribution, Slipstream, navigator page, KUnit note. The duplicate first
paragraph should go.

## 43 6457c46b484f SUNRPC: Copy region records when the budget drawn on is 0

**Sequencing.** A design correction to 31/38 with no Fixes tag. It fixes
a hang that the earlier design allowed by default (configure a region and
nothing else, and the server can deadlock). **Fold into 31 (+38)**, so
that "no budget means copy" is the rule from the first commit.

**Upstream merit.** It is the right default. A maintainer will now ask
why 24's `svc_tcp_rx_loan_pages` switch also exists: for region pages,
budget 0 already means copy. The two knobs overlap. Also,
`svc_tcp_rx_budget_copy()` calls `svc_tcp_rx_credit_forget()` (takes the
budget lock) on every region record when the budget is 0, which is
another lock round trip per record in the default configuration.
Check `sk_budget` before taking the lock (`forget` does return early when
`sk_budget` is NULL, so this is mostly fine).

**Message.** Far too long: a design essay, a host build string,
"run-budget.sh PHASE=TLP, cell step-rcvbuf-n16", a drgn dump, harness
timing, and "it hung in three runs of three". The deadlock explanation is
the justification and should stay in two sentences; the storage
discussion (SGL vs whole pages) is useful context for one paragraph.
Proposed message trimmed accordingly.

## 44 f3effc4e538c SUNRPC: Hold a region transport's receive window through TCP's interfaces

**Sequencing.** Corrects 31's TCP layering violation; depends on commit
07. **Fold into 31** (or into the split-out "hold receive buffer"
commit). As a separate commit it advertises that the series once wrote
`tcp_sock` from SUNRPC.

**Upstream merit.** It is required, and the result is much better. It
still uses `svsk->sk_sock->ops->setsockopt(... TCP_WINDOW_CLAMP,
KERNEL_SOCKPTR ...)`. Add a `tcp_sock_set_window_clamp()` helper in
commit 07 (with `tcp_set_window_clamp()` under `lock_sock()`), as netdev
did for `tcp_sock_set_nodelay()`/`_keepidle()`. `sock_set_rcvbuf(sk,
rcvbuf / 2)` with the comment "doubled on the way in" is correct, but it
truncates odd values; trivial.

**Message.** Remove "section 9 finding 4 of the Slipstream review:
netdev would want ...". Otherwise fine.

## 45 68ed55b7bad5 SUNRPC: Trade pages with the socket when a partial record changes hands

**Sequencing.** Unrelated to region budgets or loans: a generic
svcsock optimisation of `svc_tcp_save_pages()`/`svc_tcp_restore_pages()`
that applies to plain upstream. Move it out of the stretch and submit it
separately (or very early in the series).

**Upstream merit.** The cost being removed is real, but the trade-off
stated in the message ("Nothing bounds or shrinks that yet") will get it
bounced. Each TCP connection now keeps up to `sk_maxpages` pages
(roughly `sv_max_mesg`, 1 MiB+) pinned for its lifetime, including idle
connections. With thousands of mounts that is gigabytes held by idle
sockets. Chuck will ask for a bound, a release on idle, or a per-pool
page cache instead (the pages could go back to the `svc_rqst`'s pool or
rely on the PCP lists). Also:
- `svc_tcp_clear_pages()` now walks all `sk_maxpages` entries on every
  close, and returns early (leaking the spare pages) whenever
  `svc_tcp_rx_abort_active()` returns non-zero.
- The comment in `svc_tcp_recvfrom()` was adjusted but the
  `WARN_ON_ONCE(i < npages)` semantics now depend on `sk_datalen` having
  been zeroed there; fragile.
- Pages swapped in from a different NUMA node's thread defeat the
  per-node pools that 49 later introduces; mention or handle it.

**Message.** Good structure. "27.7 GB/s ... 64-CPU ... 16 KiB pages and
256 KiB receive buffers" can shrink to one generic figure. "Nothing bounds
or shrinks that yet" is honest, but reads as an admission that the patch
is not finished. Either fix the bound or state why it is acceptable.

---

## Summary table

| Pos | Verdict |
|---|---|
| 31 | Keep, but fold 32-37, 39, 43, 44 into it and re-split by design (accounting / admission / buffer+window). Rewrite message. |
| 32 | Fold into 31 (in-series Fixes). Consider dropping the `orphaned` counter. |
| 33 | Fold into 31 (in-series Fixes). |
| 34 | Fold into 31 (in-series Fixes); defines admission point. |
| 35 | Fold into 31's buffer/window part (in-series Fixes). |
| 36 | Fold into 34/31 (in-series Fixes to 34). |
| 37 | Fold (in-series Fixes to 34); currently out of order with 39. |
| 38 | Defer or reduce to one config interface; merit questionable for a first merge. |
| 39 | Fold (in-series Fixes to 31, really also to 38); must precede 37. |
| 40 | Keep with 41 folded in; make the sync restructuring conditional; audit COMPOUND tag use after early release. |
| 41 | Fold into 40 (in-series Fixes; fixes a use-after-release). |
| 42 | Move next to 15; drop the module parameter. |
| 43 | Fold into 31/38 (an unflagged fix to the design); reconcile with 24's switch. |
| 44 | Fold into 31; add a TCP window-clamp helper to 07 instead of `ops->setsockopt`. |
| 45 | Split out as an independent svcsock patch; needs a bound on per-socket retained pages. |
