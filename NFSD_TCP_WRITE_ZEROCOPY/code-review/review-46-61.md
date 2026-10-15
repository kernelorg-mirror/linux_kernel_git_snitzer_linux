# Review of positions 46 to 61 (kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-code)

Reviewed as Chuck Lever / Christoph Hellwig would for SUNRPC, and as
Eric Dumazet / Jakub Kicinski would for anything touching TCP, page_pool
or veth. Proposed messages are in `new-46.txt` .. `new-61.txt`.

## Series-level view of this stretch

This stretch is not one thing. It is five independent topics, interleaved
in the order they were found on the rig:

| Topic | Commits | Verdict |
|---|---|---|
| page_pool bulk return | 46, 47, 48 | Keep, **fold 47 and 48 into 46** |
| Several thread pools per node | 49 | Generic SUNRPC scheduling change with no tie to zero-copy. **Send separately** (or drop) |
| Hand-off cost of a partial record | 50, 51, 58, 59, 61 | Four alternative knobs for one problem, all default off. **Pick one mechanism, make it the behaviour, drop the rest** |
| Learn/measure the hold and credit factors | 52, 53, 54, 55, 56, 57 | Right direction (removes a hand-set knob), but 52's hold half is rewritten by 53, 55 fixes a stall 53 introduced, and 56 rewrites 53's clamp. **Squash 52 to 56 into one or two commits, ideally folded back into 31/35/44 where the factor and the hold first appear.** 57 can be folded into the squash or kept |
| veth rx_ip_align | 60 | Self-described test aid, a new netdev module parameter. **Drop from the upstream series** |

Ordering problems:

- 60 (veth, dated 5 Oct) sits between SUNRPC receive-path commits it has
  nothing to do with. Wherever it goes, it does not go in an NFSD/SUNRPC
  pull: it would need to go through netdev, and netdev will not take it.
- 49 changes `svc_pool_for_cpu()` to `svc_pool_for_xprt()` and the pool map,
  with no dependence on anything else in the series. It belongs before the
  whole series as its own patch (or its own series), so it can be judged on
  its own.
- 50/51 (linger) are followed by 52 to 57 (geometry) and then 58/59 (keep,
  lowat) and 61 (claim/copy unlocked), all four of which address the same
  cost: a large record arriving in many pieces gets handed between threads
  and copied under the socket lock. Put whatever survives together.
- 52 to 56 each rewrite the previous commit's result within a day (the
  measured history of one design). Upstream wants the end state.

Module parameters. This stretch adds `pools_per_node`,
`svc_tcp_rx_linger_us`, `svc_tcp_rx_keep`, `svc_tcp_rx_lowat`,
`svc_tcp_rx_copy_threads` and veth's `rx_ip_align`, and (to its credit)
removes `svc_tcp_rx_credit_factor`. Chuck has said for years that he does
not want new sunrpc module parameters, and especially not ones that only a
person with a perf trace can set; netdev forbids new driver module
parameters outright. Every one of the four receive-path knobs defaults to
"off", so as posted the series ships code that no default configuration
runs. A maintainer will ask, for each, "if this is better, why is it not
the behaviour; if it is not, why is it here?" They should be resolved
before posting:

- `svc_tcp_rx_keep` (58): almost certainly should just be the behaviour
  (see below); drop the knob.
- `svc_tcp_rx_copy_threads` (61): split; the "copy with the lock dropped"
  half should be the behaviour if it survives netdev review, and the
  "give the transport up before copying, N at a time" half is a separate
  design question.
- `svc_tcp_rx_lowat` (59): either the behaviour, with the cap derived
  (e.g. from the receive buffer/window), or dropped.
- `svc_tcp_rx_linger_us` (50): a busy-wait in nfsd threads. Drop, or
  reimplement on the networking stack's busy-poll (`sk_busy_loop()`, honoring
  `SO_BUSY_POLL`/`net.core.busy_read`) so no new knob is needed.
- `pools_per_node` (49): if kept, it is a property of the service, and nfsd
  already has a netlink/`nfsdctl` interface for thread and pool
  configuration; a module parameter that is 0644 but "read when the first
  pooled service is created" is the worst of both.

Debug-only additions: 51 and 57 add tracepoints and 54 adds a statistics
field. Tracepoints are cheap and Chuck accepts them in SUNRPC, but each one
here exists to tune a knob; if the knob goes, the tracepoint goes. They also
use `__string(addr, xprt->xpt_remotebuf)` rather than the existing
`SVC_XPRT_ENDPOINT_FIELDS()`/`SVC_XPRT_ENDPOINT_ASSIGNMENTS()` helpers every
other svcsock tracepoint in `include/trace/events/sunrpc.h` uses; convert.

Code hygiene that will be noticed at the tip:

- `net/sunrpc/svcsock.c` at the tip still has a code comment naming
  "an Xsight E1" with lab throughput figures (in the comment over
  `svc_tcp_rx_scaling_ratio()`, added by 56). Must go.
- 53 reindents the continuation lines of `svc_tcp_rx_credit_admit()`,
  `svc_tcp_rx_credit_charge()`, `svc_tcp_rx_skb_region_pages()`,
  `svc_tcp_rx_release_payload()`, `svc_tcp_restore_pages()`,
  `svc_tcp_save_pages()`, `svc_tcp_rx_publish()`,
  `svc_tcp_rx_geometry_account()` (and its forward declaration) to a
  column that no longer matches the opening parenthesis, adds four stray
  double blank lines and deletes the blank line after the declarations in
  `svc_tcp_rx_copy_to_arena()`. These are artifacts of splitting
  `VISIBLE_IF_KUNIT` out of the code group and they remain at the tip of the
  code branch. checkpatch will flag the alignment; fix them in 53 itself.
- 53 adds `#define svc_tcp_rx_fixed_factor() 0U`, a stub whose only reason
  to exist is the KUnit suite. In the code group it is dead code
  (`0 ?: x`). Either the KUnit interface commit introduces the hook in full,
  or the code reads `READ_ONCE(c->cost_factor)` directly.
- 53 also hides two unrelated refactors: `svc_tcp_recv_record_done()` is
  split out of `svc_tcp_recvfrom()`, and `svc_tcp_recv_actor()` is renamed
  `svc_tcp_recv_skb()` behind a new wrapper. Neither is mentioned in the
  message. The rename is needed for the geometry accounting; the helper is
  not (it is used by later commits). Move each to where it is needed, or
  say so.

---

## 46 d120a8bf8c90 SUNRPC: Return a loaned record's page-pool pages in batches

**Sequencing.** Sensible place: after the loan release path exists. But as
committed it overflows a stack array in `page_pool_put_netmem_bulk()`
(fixed by 47 and guarded by 48). Fold 47 and 48 into it; upstream must
never see a commit that writes 48 entries past `bulk[XDP_BULK_QUEUE_SIZE]`.

**Merit.** The change is justified: `skb_page_unref()` per page from
process context goes through `page_pool_put_unrefed_netmem()` and the
ptr_ring producer lock each time. Points a netdev reviewer will raise:

- `page_pool_put_netmem_bulk()` is documented as suitable for "the driver
  NAPI tx completion loop for the XDP_REDIRECT use case". Using it from an
  nfsd thread is fine (`page_pool_napi_local()` returns false outside
  softirq, and `page_pool_producer_lock()` takes `_bh`), but say so; Jakub
  will ask.
- The real bug is that `page_pool_put_netmem_bulk()` accepts an unbounded
  `count` and silently overflows `bulk[]` when more than 16 entries belong
  to one pool. The better fix is a small page_pool patch that flushes
  `bulk[]` when it fills (or at minimum a kernel-doc statement of the
  bound), sent to netdev first; then SUNRPC need not know about
  `XDP_BULK_QUEUE_SIZE` from `<net/xdp.h>`, which is an XDP constant and a
  layering smell in svcsock.c.
- The batch bypasses `skb_page_unref()`'s `netmem_is_pp()` check and relies
  on the per-entry `state->page_pool` bit recorded at loan time; worth one
  sentence.
- `IS_ENABLED(CONFIG_PAGE_POOL)` around the test: with `!CONFIG_PAGE_POOL`
  `page_pool_put_netmem_bulk()` is a stub that would leak, so the guard is
  needed, but then the `page_pool` bit should never be set; fine.

**Message.** Mostly good. "nfsd's threads release on other CPUs than the
pool's own, so none of this goes by the pool's lockless cache" is right but
clumsy. One perf figure is enough. Proposed message in `new-46.txt` is
written for the folded commit (batch of `XDP_BULK_QUEUE_SIZE` with the
BUILD_BUG_ON); if 47/48 are not folded, drop its last paragraph.

## 47 55c801e63be8 SUNRPC: Keep page-pool return batches within the bulk API's bound

**Sequencing.** A fix for 46 inside the same unmerged series. **`Fixes:
d120a8bf8c90` points within the series: fold into 46.** Upstream a fix to
an unmerged commit is squashed; the Fixes tag would also reference a SHA
that will not exist after any rebase.

**Merit.** Correct analysis of `page_pool_put_netmem_bulk()` (bulk[] on
stack, unchecked). See 46 for the preferred page_pool-side fix.

**Message.** Accurate and appropriately short. The "16 KiB pages" detail is
the clearest justification and is generic. Minor rewording only.

## 48 5dc599ad5704 SUNRPC: Refuse to build a page-pool return batch the bulk API cannot take

**Sequencing.** **`Fixes:` within the series: fold into 46** (together with
47). On its own it is a two-line BUILD_BUG_ON; it also is redundant with
47's `#define SVC_TCP_RX_PUT_BATCH XDP_BULK_QUEUE_SIZE` unless someone edits
the define, which is precisely what a BUILD_BUG_ON is for, so keep it in
the folded commit.

**Merit.** Fine as a guard; but again, the guard belongs in page_pool, not
in every caller.

**Message.** "Nothing reports it until a loaned record is released, and
then the report is the crash" is narrative; trimmed.

## 49 9c56ecd06a59 SUNRPC: Let a NUMA node have several thread pools

**Sequencing.** Independent of everything else in the series (touches
`svc.c`, `svc_xprt.c`, `svc.h` only). It should be posted on its own,
before or apart from the zero-copy work, and judged on its own.

**Merit.** The problem (contention on one lwq per pool on large single-node
machines) is real and Chuck would be interested in it. The solution will get
pushback:

- Pinning a transport to one sub-pool by `hash_ptr(xprt, 32) % per_node`
  makes the scheduler non-work-conserving: a busy connection can only use
  1/N of the node's threads, and a transport can sit in a queue whose
  threads are all busy while another sub-pool on the same node idles. The
  fallback loop in `svc_pool_for_xprt()` only skips pools with zero
  threads, not busy ones. That latency/throughput trade-off needs data
  beyond the lock-contention figure, e.g. a mixed small-op/large-op load,
  and with few connections.
- Hashing a pointer gives no balance guarantee for small connection counts
  (16 connections over, say, 8 pools is lumpy).
- Every per-pool interface (`/proc/fs/nfsd/pool_threads`,
  `pool_stats`, the netlink `threads` attribute, `nfsdctl`) now exposes
  `nodes * per_node` pools, and userspace that sets threads per pool will
  see a different number of pools depending on a module parameter. That
  needs to be called out and documented.
- Module parameter 0644 that is "read when the first pooled service is
  created": writable but without effect until the service restarts. Make it
  0444, or make it part of the service's configuration (netlink).
- `BUG_ON(pidx + per_node > maxpools)` keeps and extends a BUG_ON; reviewers
  will ask for WARN_ON_ONCE + error.
- Alternatively, ask whether the contention is really the lwq lock or the
  idle-thread llist/wakeups, and whether the fix belongs in lwq.

**Message.** The table of perf numbers is more than needed; one sentence
suffices. Body otherwise clear.

## 50 e22124ff4db6 SUNRPC: Let a thread wait for the rest of a partly received TCP record

**Sequencing.** OK after 45 (page trading on hand-off). 51 is its
instrumentation and should be folded in (or dropped with it).

**Merit.** Weak for mainline as is:

- It is a busy-wait of up to 1 ms in an nfsd thread, spinning on
  `XPT_DATA` with `cpu_relax()`, default off. Chuck will ask why nfsd
  should spin; Eric will point at `sk_busy_loop()` / `napi_busy_loop()`,
  which already implement "poll for data on this socket for N us",
  integrate with NAPI (actually pulls packets instead of waiting for the
  softirq on another CPU to deliver them) and are configured per socket by
  `SO_BUSY_POLL` / `net.core.busy_read`. If lingering is worth having it
  should use that and need no new knob.
- A sleeping alternative (`sk_wait_data()`-style with a short timeout) would
  give most of the hand-off saving without burning a CPU; the message does
  not say why that was rejected.
- It overlaps 58 (keep), 59 (lowat) and 61 (copy unlocked), which address the
  same hand-off cost. Upstream will not want four.
- The restructuring of the `svc_tcp_recv_ctrl_record()` call is
  behaviour-preserving, fine.

**Message.** The measurement paragraph (17 calls, 89%, 552 us, 242 us, 6 us,
13 us, 27.7 GB/s) is a lab narrative; the mechanism can be stated without
it.

## 51 6e8fc71c2baa SUNRPC: Trace how long a thread lingers for the rest of a TCP record

**Sequencing.** Instrumentation of 50 alone. Fold into 50 (or drop with it).

**Merit.** Exists to tune `svc_tcp_rx_linger_us`. Uses `xpt_remotebuf`
rather than `SVC_XPRT_ENDPOINT_*`. `enum svc_tcp_rx_linger_end` is put in
`include/linux/sunrpc/svcsock.h` only for the tracepoint, fine. The loop
rewrite into `for (;;)` with four breaks is longer than needed; computing
`why` after a `while` loop would do.

**Message.** "The histogram of waits that ended on data is what a setting of
the linger should be chosen from" is advice, not justification; trimmed.

## 52 acba1351f191 SUNRPC: Learn a loaned record's charge from the records

**Sequencing.** Belongs with (ideally folded into) 31, where
`svc_tcp_rx_credit_factor` and the estimate are introduced: upstream should
see a credit scheme that learns its cost from the start, not a hand-set
factor that is replaced 21 commits later. Its second half (hold factor and
`svc_tcp_rx_hold_stale()` keyed off the learned cost factor) is thrown away
by 53 one commit later; do not carry that intermediate state.

**Merit.** Moving from a hand-set knob to self-tuning is the right
direction for upstream. Questions a reviewer will ask:

- Cold start: "Until a budget's first record has ended, a record is charged
  all it could hold, so records go one at a time." Each namespace's budget
  serializes its first loans. Acceptable, but worth a justification.
- The decay rule (max of current and previous 1024-record windows) is
  ad hoc; fine if documented, but the constant `SVC_TCP_RX_COST_WINDOW` lives
  in `include/linux/sunrpc/svcsock.h` while only svcsock.c uses it (the
  header placement is for KUnit). Keep it private in code.
- `sample = max(sample, 100U)`: say why a record cannot hold fewer pages
  than its payload (true for loans; say it).

**Message.** Badly overdone: device name, 136 frames, 68 pages, factors
190/110/150, "157 thousand records part-way in two minutes", "settled at
164". The problem (a factor that depends on NIC buffer geometry and frame
interleaving) can be stated generically. Rewritten.

## 53 e56b22cb2e00 SUNRPC: Hold a transport's receive window by what its packets occupy

**Sequencing.** Rewrites 52's hold logic and removes the module parameter 52
just redefined. 52 + 53 + 55 + 56 should be one change (or fold into 35/44,
where the hold first appears). 55 fixes a connection stall introduced here.

**Mixes changes.** (a) measured truesize/payload ratio per transport,
(b) removal of `svc_tcp_rx_credit_factor`, (c) `svc_tcp_recv_actor()` ->
`svc_tcp_recv_skb()` + wrapper, (d) new `svc_tcp_recv_record_done()` helper
unrelated to this commit, (e) whitespace damage across a dozen unrelated
functions (see series-level notes), (f) a KUnit-only stub
`svc_tcp_rx_fixed_factor()`. (c) is needed; (d)-(f) must go or move.

**Merit.**

- Measuring `truesize` vs payload is sound and avoids a knob; netdev will
  note that TCP already learns exactly this in `tcp_measure_rcv_mss()` /
  `tp->scaling_ratio` (from full-size segments only). The case that TCP's
  own estimate is wrong for this workload (short tail segments taking whole
  buffers) is what 7 argued; a reviewer will ask why not fix TCP's estimate
  rather than shadow it in SUNRPC.
- `hold_factor` on the budget is "written without the lock; any
  transport's figure will do": OK with READ_ONCE/WRITE_ONCE, which it uses.
- The message still says "The KUnit suite still fixes a factor through
  svc_tcp_rx_budget_kunit_configure()" while the code carries the stub;
  both should go from the code commit.

**Message.** "rig", "190, worked out for a whole 16 KiB page to a 9000-byte
frame", "483 KiB", "1.09 bytes" and the KUnit sentence go. Rewritten.

## 54 befe7874fb20 SUNRPC: Show what receive buffers are held to in a budget's statistics

**Sequencing.** A one-field addition to the statistics line; fold into 53.

**Merit.** The statistics file is a debugging aid; each extra field is ABI
someone may parse. Fine if the file itself is accepted (it is introduced
earlier in the series), but it will be asked whether it should be in
debugfs.

**Message.** Fine; reworded slightly.

## 55 04c4ac676cde SUNRPC: Measure a held receive buffer by the skbs that carry data

**Sequencing.** Fixes a stall introduced by 53 (the first connection of a
freshly started server stops). No Fixes tag, but it is exactly a fix to an
unmerged commit: **fold into 53**.

**Merit.**

- `SVC_TCP_RX_GEOMETRY_MIN_SKB 1024U` is a magic threshold; justify it (or
  express it relative to MSS).
- The new `max_t(int, clamp, 2 * advmss)` floor can make the window clamp
  exceed what the locked buffer holds for small `svc_tcp_rx_rcvbuf`
  values, which is the very overflow the clamp exists to prevent. Say why
  that is safe (or bound `svc_tcp_rx_rcvbuf` below so it cannot happen).
- Reaches into `tcp_sk(sk)->advmss` from SUNRPC; netdev prefers accessors
  (`tcp_mss_clamp`/`tcp_current_mss` are not exactly that either); expect a
  question.

**Message.** "The KUnit test follows: ..." must go. Otherwise good: problem,
cause, fix.

## 56 89c7d570ae71 SUNRPC: Open a held transport's window to half of what its buffer holds

**Sequencing.** Another rewrite of the clamp from 53; fold into the
52-55 squash.

**Merit.**

- Half the buffer is a defensible policy and simpler than 7/8 of an
  estimate. But the justification is one NIC's ring sizes ("rings of 4 x 256
  frames"); a reviewer will ask why the right window depends on the
  receiver's ring depth at all, and whether this is really a substitute for
  BQL-like pacing or for the client's congestion control.
- **The code comment over `svc_tcp_rx_scaling_ratio()` names "an Xsight E1"
  and quotes lab throughput/drop figures.** Must be removed; code comments
  explain the rule, not a benchmark.
- `SVC_TCP_RX_CREDIT_FACTOR_MAX` changing from 22400 to 12800 is a derived
  value; fine.

**Message.** Three lab measurements with GB/s, frame-drop counts, "the
rig the settings were found on", "the same night", and a KUnit sentence.
Rewritten to the argument plus one generic sentence of effect.

## 57 6b2566d1c796 SUNRPC: Trace what a transport's receive buffer is held to

**Sequencing.** Fine anywhere after the hold exists; fold with the hold
squash, or keep as a separate tracepoint patch.

**Merit.** Reasonable, low-cost tracepoint. Should use
`SVC_XPRT_ENDPOINT_FIELDS()` like neighbouring svcsock events. Argument
order (`factor, rcvbuf, clamp`) and printk field name `window_clamp` vs
entry `clamp`: harmonize.

**Message.** Fine; tightened.

## 58 efdb9110f861 SUNRPC: svc_tcp_rx_keep: a thread that finds a record short does not wake another

**Sequencing.** Belongs with the other hand-off commits. Generic SUNRPC
scheduling change (svc_xprt.c), independent of loans.

**Merit.** The idea is good and small, and probably should simply be the
behaviour, without a knob:

- The safety argument holds: the caller returns to `svc_recv()`, which
  either dequeues from `rq_pool` or, on stop, calls
  `svc_thread_wake_next()` which wakes another thread if the queue is not
  empty; and if this thread dequeues some other transport first,
  `svc_thread_wake_next()` again wakes another. Spell this out in the
  message or a comment; it is the first thing Chuck will check.
- The queue chosen by `svc_xprt_enqueue()` is selected from the current
  CPU's node (and, after 49, a hash). It is the caller's own pool only if
  the thread runs on its pool's node; on multi-node machines with a
  threadless-pool fallback it may not be, and then no one is woken for it.
  The kept enqueue should either target `rqstp->rq_pool` or fall back to a
  wake when the pool differs.
- Naming: `svc_xprt_received_keep()` / "keep" says nothing about the
  semantics. Something like `svc_xprt_received_nowake()` or a `bool wake`
  argument to an internal helper is clearer. The module parameter's name in
  the subject (`svc_tcp_rx_keep:`) is not upstream style.
- `svc_xprt_do_enqueue()` forward declaration placed above
  `__svc_xprt_received()`: move the function instead, or just forward-
  declare near the top.

**Message.** "tardis1", "two ports" and the specific rates go. One
sentence of effect is enough.

## 59 1806c79a6de4 SUNRPC: svc_tcp_rx_lowat: wake a thread for more than one piece of a short record

**Sequencing.** With 58. Note it interacts with 50: with the mark raised,
`XPT_DATA` (which linger polls) is set later, so the two knobs together
behave differently from either alone.

**Merit.** Using SO_RCVLOWAT semantics to batch wakeups is a sound idea,
but netdev will object to the implementation:

- It writes `sk->sk_rcvlowat` directly with WRITE_ONCE. The supported way is
  `sk->sk_prot->set_rcvlowat` / `tcp_set_rcvlowat()` (or
  `sock_setsockopt(SO_RCVLOWAT)` via kernel sockptr), which also grows
  `sk_rcvbuf` when needed and re-evaluates readiness (`tcp_data_ready()`).
  Here `sk_rcvbuf` is locked by the hold, so the interaction with the
  clamp must be explained (it relies on `tcp_epollin_ready()`'s "window
  nearly closed" escape to avoid a stall).
- Detecting plain TCP with `sock->ops->read_sock != tcp_read_sock` is a
  function-pointer type test; check `XPT_TLS_SESSION` or `sk_is_tcp()`
  plus TLS state instead (61 repeats the pattern).
- Again a knob defaulting to off; a cap derived from the window would avoid
  it.

**Message.** "tardis1", "two ports", rates and context-switch counts. One
sentence of effect is enough.

## 60 60ca2691eab0 veth: add rx_ip_align, extra headroom for frames copied for XDP

**Sequencing.** Out of place (dated 5 Oct, sits between SUNRPC commits) and
outside the subsystem.

**Merit.** **Drop from the upstream series.** The commit itself says "This
is a test aid. It is a private parameter in one more driver". netdev does
not accept new module parameters in drivers (Jakub has said so
repeatedly), and certainly not one for a test rig. `rx_ip_align & 3`
silently masks invalid values. If NET_IP_ALIGN-style placement for
payload alignment matters, it is a netdev design discussion (e.g. a
queue/page_pool config attribute via netdev netlink), not a veth knob. Keep
it as an out-of-tree test patch.

**Message.** Mentions the out-of-tree `xeu` driver and a lab measurement;
rewritten for completeness in `new-60.txt` but the recommendation is not to
post it.

## 61 d76b21a997fb SUNRPC: Let a thread claim a TCP record and copy it outside the socket lock

**Sequencing.** Depends on 53's `svc_tcp_recv_actor()` wrapper and
`svc_tcp_recv_record_done()`, and on 58's `err_noclose` path. Belongs with
the other hand-off work.

**Mixes changes.** Should be split: (a) copy a large chunk with the socket
lock dropped (`svc_tcp_rx_copy_unlocked()`), (b) claim clones in the actor
and copy after `->read_sock()` (`svc_tcp_rx_claim()`,
`svc_tcp_rx_copy_claims()`), (c) give the transport up before the copy and
let N threads copy one transport's records (`sk_rx_copying`). (c) changes
the receive concurrency model and deserves its own patch and argument.

**Merit.** This is the commit netdev will look at hardest:

- **Memory accounting escape.** Claimed clones keep the skb data after TCP
  has counted it read and uncharged `sk_rmem_alloc`. The commit admits "a
  transport can have as much again as its receive buffer waiting to be
  copied for each thread that may be copying". Uncharged socket memory
  driven by a remote peer is exactly what Eric objects to; and with loans
  and the fixed region it also holds page_pool pages outside the budget.
- **Dropping the socket lock inside a `->read_sock` actor.**
  `tcp_read_sock()` does re-look-up the skb by sequence number (the comment
  there exists for splice), but relying on that from a new in-kernel caller,
  plus "TCP does not coalesce into a cloned skb" and "tcp_collapse() frees
  whatever references it has" as invariants, will be questioned. A netdev
  reviewer will first suggest the established alternative: stay under the
  lock and call `sk_flush_backlog()` periodically (as
  `tcp_recvmsg_locked()` / `tcp_sendmsg_locked()` do), which processes
  the backlog, ACKs and reopens the window without clones or lock drops.
  The message should show why that is insufficient.
- `skb_clone(..., GFP_KERNEL)` per skb under `lock_sock()`: allowed (process
  context), but adds an allocation per GRO skb on the hot path.
- `struct svc_tcp_recv_ctx` now embeds `claims[32]` (~768 bytes) on the
  nfsd thread's stack in `svc_tcp_recvfrom()`. Will be flagged.
- `sock->ops->read_sock == tcp_read_sock` function-pointer test again.
- Claimed copies skip `flush_dcache_page()` on the first pass
  (`flush=false`) but flush on the deferred pass; explain the rule.
- Another knob defaulting to 0.

**Message.** Content is sound and free of lab names; it mainly needs
tightening and the split above. Rewritten.

---

## Fixes: tags within the series

- 47 `Fixes: d120a8bf8c90` -> fold into 46.
- 48 `Fixes: d120a8bf8c90` -> fold into 46.
- 55 has no Fixes tag but fixes a stall introduced by 53 -> fold into 53.

## Summary of recommendations

1. Fold 47, 48 into 46; consider a page_pool-side fix for the unchecked bulk
   bound sent to netdev first.
2. Post 49 separately; expect pushback on non-work-conserving pinning and on
   the module parameter.
3. Squash 52-56 (and 54, optionally 57) into one "learn the credit cost" and
   one "measure the hold ratio" commit, folded back toward 31/35/44 if the
   series is regrouped; remove the Xsight comment, the KUnit stub, the
   whitespace damage and the unrelated helper from 53.
4. Collapse 50/51, 58, 59, 61 into the one or two mechanisms that carry the
   result, as default behaviour without module parameters: 58 is the
   cheapest and most clearly correct; 61 needs a netdev-acceptable design
   (consider `sk_flush_backlog()` first); 50 should use `sk_busy_loop()` or
   be dropped; 59 should use `tcp_set_rcvlowat()`.
5. Drop 60.
