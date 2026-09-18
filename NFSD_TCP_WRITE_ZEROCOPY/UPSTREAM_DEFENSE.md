# Upstream defense — how the loan survives review without E1

Captures the robustness question asked 2026-09-06, the assessment it
produced, and the pre-submission checklist that fell out of it. The code
references below are against branch tip `27dcc24ecf27` (kernel
`v7.1.8-3` base).

## The question (Mike Snitzer, 2026-09-06)

> How defensive or robust are this project's core feature changes (the
> loan pages capability) WITHOUT the underlying hardware capabilities
> that are scoped to be required from the E1? My concern is that
> upstream may reject the changes due to them not being adequately
> benign if/when the underlying hardware cannot cope.

## Short answer

**The loan capability has no hardware dependency at all — E1 was never
a correctness requirement, only a hit-rate multiplier — and the
fallback lattice is complete.** The one place upstream can legitimately
press is not "what if the NIC can't cope" but "what if the *block
driver* can't cope": the silent-corruption class already found in
brd/zram. Plus one gap to close before posting: there is no
transport-level kill-switch for the loan.

### E1 is not load-bearing anywhere

The investigation report's "risk #4" (gate on header-data-split
capability) belongs to the *original* direction, which the loan series
superseded. The implemented design loans **post-TCP-reassembly socket
pages** — whatever geometry any NIC plus GRO produces. Nothing in
`svc_tcp_rx_classify()` (`net/sunrpc/svcsock.c:1706`) or the walker
consults a device capability. On a NIC with awkward payload placement,
the only consequence is that more WRITE segments fail
`nfsd_dio_iter_is_aligned()` and take the DONTCACHE-buffered path —
which still nets one copy total instead of today's two (receive copy +
buffered write copy). Correctness never depends on placement.

### The fallback lattice is genuinely defensive

- **Classify-time**: anything not plaintext-TCP + `rpc_call` + known
  program/version + `pc_xdr_bvec` procedure + permitted auth flavor
  drops to `SVC_TCP_RX_COPY` with a named reason — byte-for-byte
  today's receive behavior. Unknown programs, gss, TLS, callbacks: all
  copy.
- **Walk-time**: zerocopy/managed frags →
  `svc_tcp_rx_materialize_fallback()` + copy; locked linear heads → the
  merged whole-page copy; bvec-table capacity overflow → materialize
  into the `rq_pages` arena (`svcsock.c:1670`). The materializer
  (`svcsock.c:337`) is a universal mid-receive escape from MIXED to
  fully-copied, with bitmap/refcount invariants checked at every step.
- **Fail-closed, never silent**: `net_iov`, NULL frag page, copy fault,
  or any invariant breach → `svc_tcp_rx_abort` + deferred connection
  close (`svcsock.c:2508–2527`). The net_iov/unreadable cases are
  unreachable on an nfsd-owned socket anyway (devmem TCP is per-socket
  opt-in by the receiving application).
- **NFSD write side is triple-guarded**: whole-iterator gate →
  DONTCACHE-buffered on misalignment; block-layer `-EINVAL` → iterator
  restored and the segment retried buffered (`fs/nfsd/vfs.c:1473`),
  client never sees a geometry error; DONTCACHE preserved through the
  fallback. And the whole DIO path sits behind `nfsd_io_cache_write`,
  whose default is `NFSD_IO_BUFFERED` (`vfs.c:58`) — direct writes are
  an explicit debugfs opt-in before the loan is even relevant.

### The two things to fix in the upstream story

**1. The real "not adequately benign" vector is
acceptance-with-corruption, and the EINVAL fallback cannot catch it.**
The fallback handles a driver that *rejects* loaned geometry; brd and
zram *accepted* it and corrupted silently. The defense today is (a) the
completed in-tree bio-driver audit, (b) `dma_alignment` keeping nfsd's
gate from ever sending loans to the exposed dm sites, and (c) the fixes
leading the branch. The honest framing for the cover letter — and it's
a strong one — is that **this class predates the loan**: stock
`NFSD_IO_DIRECT`'s copied-arena geometry (`bv0=(160, PAGE−160)`,
sub-sector lengths) corrupts brd today. The loan widens the offset
variety, not the class. Sequencing the five fixes first and explicitly
raising the iomap `ITER_BVEC` validate/bounce question *in the series
cover letter* turns the weakness into evidence of diligence. What
cannot be offered is a structural guarantee for out-of-tree drivers;
only the iomap-layer answer provides that, and it's fair to say so.

**2. There is no loan chicken bit.** No `module_param`, sysctl, or
static_branch gates the classifier — every eligible receive loans,
unconditionally, the moment the series is in. `io_cache_write` only
controls the write side. For a change of this size in the svcsock
receive path, upstream will very plausibly ask for a runtime disable
(static_branch defaulting on, or a sunrpc sysctl) so a field regression
can be mitigated without a rebuild. That's cheap to add now and
expensive to be asked for on list.

One minor cover-letter item: borrowed pages (including page_pool pages)
stay pinned from receive to request release — bounded by
`nthreads × sv_max_mesg`, held only across a synchronous write, same
class as splice-from-socket — worth one pre-emptive sentence, unlikely
to be a blocker.

## Pre-submission checklist

- [x] **Add a transport-level loan kill-switch (chicken bit).**
  **Done (2026-09-06)** as the incremental pair "SUNRPC: add a runtime
  switch to disable TCP receive page loans" + "SUNRPC: cover the loan
  kill-switch in the svcsock KUnit suite" (placement footnotes in both
  for the final rebase). Shape chosen: a writable module parameter,
  `sunrpc.svc_tcp_rx_loan_pages` (bool, default Y, 0644), read once
  per RPC record in `svc_tcp_rx_classify()` right after the XID is
  captured; clear → `SVC_TCP_RX_COPY` with new named reason
  `disabled` (enum + tracepoint string). Off state is byte-identical
  to the pre-loan receive; flipping never affects a record already
  classified. KUnit: new `disabled` exclusion-table case, and a
  suite-level init/exit that forces the switch on per test and
  restores the host's setting, so the suite stays valid on a server
  running with loans off. Qualification record: TESTING.md → "The
  loan kill-switch".
- [ ] **Write the cover letter with the corruption class front and
  center.** State plainly: (a) nfsd's statx-based DIO gate admits
  iterators the block stack may reject — handled by the buffered
  `-EINVAL` fallback; (b) two in-tree drivers *accepted* sub-sector
  bvec geometry and corrupted silently (brd, zram) — found by this
  project's byte-verification, fixed in patches 1–2 of the posting;
  (c) the exposure predates the loan (stock `NFSD_IO_DIRECT` corrupts
  on brd today); (d) the open systemic question — should iomap
  validate/bounce `ITER_BVEC` direct I/O with sub-sector bvec
  boundaries? — is raised for maintainer input, with the audit results
  (`BIO-DRIVER-AUDIT.md`) as the data.
- [x] **Post the four standalone fixes first**, as their own series
  — **POSTED 2026-09-08** to the lists (brd/zram → linux-block, nfsd
  pair → linux-nfs); awaiting review/merge. Remaining half of this
  item: track review, re-post as needed, and rebase the branch once
  they land.
  (brd byte-position, zram sub-page bvec, nfsd filecache DIO-alignment
  fetch, nfsd `-EINVAL` buffered fallback) — they are bugs upstream
  today, they don't depend on the loan, and landing them first removes
  the "your feature needs unmerged fixes" objection. **Drafted
  2026-09-06** in `upstream-fixes/` (unversioned iteration dir) with
  cover letter; loan/internal references reworded in the patch files
  only. The fifth branch fix (DONTCACHE clobber) is **dropped from the
  posting** — verified not upstream-exposed: mainline's `no_dio`
  fallback never sets DONTCACHE; the bug arrived with the hs-carried
  hch heuristic RFC (`eaa2a4f9efb7`) and the fix travels with that
  patch instead.
- [x] **Report the DM exposures to dm-devel** — **covered 2026-09-08**:
  dm-devel was Cc'd on the four-fix posting, which carries the
  corruption class and the audit pointer. Disposition (Mike, who is a
  DM maintainer): it is the DM maintainers' call to care; the coarse
  up-front `dma_alignment >= 511` gate keeps NFSD's DIO path away from
  every exposed dm site (dm-io/do_region, dm-log-writes,
  dm-writecache-pmem, dm-integrity at default block size), so the DM
  bvec-iter issues are **deliberately not pursued at this time**.
- [ ] **One pre-emptive cover-letter sentence on page pinning** (bound,
  duration, splice precedent — see "Memory pinning" below).
- [ ] Qualify the chicken-bit commit like every other: `checkpatch`,
  `W=1`, WERROR build, bisect-walk step, KUnit + `run-loan-assert.sh`
  with the switch toggled both ways.
- [ ] Optional strengtheners, not blockers: force `tls`/gss copy
  reasons live (needs krb5 or `xprtsec=tls`); tardis1 performance
  numbers for the cover letter (the series makes no throughput claim,
  but the 41%-memmove motivation reads better with a measured delta).

## Longer answer

### What "hardware cannot cope" actually means, layer by layer

There are three layers where hardware or geometry could "not cope", and
the series answers each differently:

1. **NIC payload placement** (the E1 concern as originally framed).
   Irrelevant to correctness. The loan operates on socket receive pages
   after TCP reassembly; the walker takes page references on whatever
   frag geometry exists (`svc_tcp_rx_walk_skb()`, `svcsock.c:1996`).
   Placement only moves the ratio of `nfsd_write_direct` to
   `nfsd_write_vector` — observed live: page-aligned anchors go
   16/16 direct, mid-page anchors go 0/16 buffered, both byte-correct.
2. **Storage DMA constraints** (`dma_alignment`, logical block size,
   segment limits). Two independent guards: nfsd's whole-iterator
   `STATX_DIOALIGN` gate demotes to DONTCACHE-buffered before issue,
   and the block layer's own `bio_split_io_at()` checks reject at
   submit — which the `-EINVAL` fallback converts into a buffered
   retry of the same segment with the restored iterator. The client
   cannot observe either demotion. This pair was validated by the
   16K EINVAL finding and its fix (TESTING.md).
3. **Block-driver internal iteration** — the only layer with a
   demonstrated silent failure mode, discussed below.

### Complete classify/walk reason inventory

Every copy or abort is named on the `svcsock_tcp_rx_lifetime`
tracepoint. Reachability on a stock server:

| reason | trigger | behavior | reachable in practice |
|---|---|---|---|
| `short-prefix` | record smaller than the fixed prefix | copy | yes (tiny RPCs) |
| `tls` | `XPT_TLS_SESSION` set | copy | with xprtsec=tls |
| `direction` | not `rpc_call` (a reply) | copy | callback replies |
| `rpc-version` | not RPC v2 | copy | malformed/ancient clients |
| `program` / `version` / `procedure` | lookup fails | copy | probes, version churn |
| `auth` | flavor not AUTH_SYS/AUTH_NULL, or proc not `pc_xdr_bvec` | copy | every fresh connection's AUTH_NONE ping (verified live, x86_64 run) |
| `locked-head` | locked skb linear head overlaps payload | merged whole-page copy, loan continues | common on loopback (0–3 events/receive observed) |
| `managed-frags` | zerocopy/managed skb | materialize + copy | MSG_ZEROCOPY senders |
| `capacity` | bvec table would overflow | materialize into arena | odd geometry bursts |
| `unreadable` / `net-iov` / `null-page` | devmem TCP or impossible frag | abort + connection close | unreachable for nfsd-owned sockets |
| `copy-fault` | `skb_copy_datagram_iter` failure | abort + connection close | same class as today's receive failure |
| `invariant` | internal state violation (WARN_ON_ONCE) | abort + connection close | never, by construction; fail-closed if wrong |

Two properties worth stating explicitly on list: **the COPY mode is not
a degraded mode — it is literally the pre-series receive**, an iterator
over `rq_pages` filled by `skb_copy_datagram_iter`; and **every abort
is connection-scoped, never data-scoped** — a failed receive can only
kill the connection (client retransmits on reconnect), it cannot
deliver wrong bytes, because the materializer's invariants
(`svcsock.c:380–386`) verify byte totals, refcount balance, and bitmap
emptiness before the state is allowed to publish.

### The corruption class, precisely

The loan's novel exposure is the *shape* of the iterator reaching the
block layer: `ITER_BVEC` with interior bvecs at nonzero offsets and
sub-sector lengths — a shape user-space `O_DIRECT` cannot produce
(alignment checks reject it at the syscall boundary) and the copied
arena produces only in its first/last bvec. Defense-in-depth, in order:

1. `nfsd_dio_iter_is_aligned()` vs `STATX_DIOALIGN` — admits only what
   the *filesystem* claims to handle.
2. `bio_split_io_at()` and queue limits — rejects what the *queue*
   cannot handle; converted to buffered by the `-EINVAL` fallback.
3. The driver itself must iterate correctly. brd and zram did not
   (`bio_advance_iter_single()` truncates `bi_sector` advances to
   whole sectors; any driver re-deriving position from
   `bi_iter.bi_sector` per segment skews). Both fixed on this branch;
   full in-tree sweep in `BIO-DRIVER-AUDIT.md` — the four exposed dm
   sites are unreachable because every dm queue advertises
   `dma_alignment >= 511`, which layer 1 already refuses; MD splits to
   limits before any personality sees the bio; request-based drivers
   (real NVMe) do no per-bvec sector arithmetic.

Layer 3 is the residue: it is per-driver correctness, not a structural
guarantee. The two candidate structural answers, both explicitly
deferred to maintainer input rather than baked in unilaterally:

- **iomap validates/bounces** sub-sector-boundary `ITER_BVEC`
  iterators — fixes it for every filesystem and driver at once; costs
  a bounce or rejection on exactly the geometry in question.
- **nfsd requires per-bvec logical-block alignment** for
  direct-eligible iterators — one-line gate, but it also demotes the
  legacy copied-arena geometry (`bv0=(160,…)`) to buffered, i.e. it
  guts DIO for the existing `NFSD_IO_DIRECT` path. Rejected here for
  that reason; the tradeoff belongs to series review.

### Memory pinning, quantified

A loaned receive holds page references from publish to request release
(`svc_tcp_rx_release`, after the reply). Bound per request:
`capacity` = the arena size (`sv_max_mesg`, ~1 MiB server default,
so ~257 pages at 4K + spill). Worst case pinned:
`nthreads × sv_max_mesg` — the same order as the `rq_pages` arenas the
server already preallocates; the loan transiently at-most-doubles the
per-request page footprint while halving the DRAM traffic. Duration is
one synchronous VFS write (the series has no async completion holding
loans open). page_pool pages held out of NIC recycle during that window
are the same class of pressure as `splice(2)` from a socket, which the
kernel has shipped for two decades. `sk_rmem` accounting releases with
the skb while the pages live on — again the splice precedent. None of
this is novel exposure; all of it deserves one sentence in the cover
letter so nobody discovers it for us.

### Anticipated objections and their answers

| likely objection | answer |
|---|---|
| "The receive path rewrite is too risky to take with the feature" | The `->read_sock` conversion is separated at the front of the series (5 commits, no loan content) and is independently useful; the loan commits sit behind it. Every commit bisect-builds WERROR-clean on two architectures with per-step sparse adjudicated to zero new findings. |
| "What if the loan misbehaves in the field" | Chicken bit (checklist item 1): flip it and the receive is byte-identical to today's. Named tracepoint reasons make "why did/didn't this loan" a one-liner to diagnose. |
| "Your DIO gate is weaker than the block stack's" | Agreed, by demonstration (the 16K EINVAL finding); that is why the buffered `-EINVAL` fallback exists, and why the iomap question is raised in the cover letter. |
| "This geometry corrupts driver X" | It corrupted two in-tree drivers; both fixes lead the posting; the class predates the loan (stock `NFSD_IO_DIRECT` corrupts brd today); the full in-tree audit is attached. |
| "GSS/TLS/callbacks?" | Never loaned — classify-time exclusions, KUnit-covered (`svcsock_rx_exclusion_table_test`), `auth` verified firing live. |
| "Struct bloat / cacheline damage" | pahole review on the BTF-faithful build: zero new holes, `svc_procedure` unchanged at one cacheline (bitfield split), `svc_tcp_rx_state` 128B `__aligned(64)`, `xdr_buf` 72→80 holeless (TESTING.md, BTF entry). |
| "Where are the tests" | Four KUnit suites (74 cases) plus a KUnit-decoupled system-correctness suite with model-file byte-verification, on 4K/16K aarch64 and x86_64; the corruption findings are the proof the byte-verification layer works. |
| "Does it help without special hardware" | The copy elimination is NIC-agnostic; qualified entirely on stock virtio/loopback. E1/tardis1 numbers are the throughput demonstration, not the correctness basis. |

### What E1 actually changes

Only the fast-path hit rate on real 400G traffic: how often payload
fragment offsets survive `nfsd_dio_iter_is_aligned()` whole-iterator
gating, and whether a lead-fragment-only copy is worth adding. Those
are the open tardis1/E1 work items in PROJECT.md — performance
characterization, not robustness. A server running this series on
hardware that "cannot cope" (arbitrary placement, no HDS, no page_pool)
is simply a server whose WRITE segments take the DONTCACHE-buffered
path with the receive copy already eliminated — strictly better than
today on every axis measured, and never less correct.
