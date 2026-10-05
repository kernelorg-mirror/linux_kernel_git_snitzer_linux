# Slipstream code drop review (David Flynn, 2026-09-28)

Sub-project of the NFSD TCP write-path zero-copy series. Reviews David
Flynn's "Slipstream kernel overlays" delivery and records what can go onto
`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`. The drop itself sits beside
this file in
[`slipstream-kernel-overlays-hs-90059375-2026-09-28/`](slipstream-kernel-overlays-hs-90059375-2026-09-28/)
(open its `index.html` for David's per-patch pages).

## 0. Status / handoff

- **Taken from the drop:** the seven receive-path patches less one (05),
  as David wrote them, plus fixes on top. Everything else in the drop is
  listed in section 6 and not applied.
- **What it adds to the page-loan series, in plain terms:**
  - a **region**: a fixed, small block of memory that named network devices
    must receive into, so that the place packets land can stay in the CPU's
    cache (patch 16);
  - a **budget** on how much of that region loaned WRITEs may hold at once,
    with TCP slowing the sender when it is used up (patch 18). The code
    calls what one WRITE draws against the budget its credit;
  - giving a WRITE's receive pages back before storage finishes, in the
    buffered write modes (patch 19);
  - copying small RPCs instead of loaning for them (patch 20).
- **Which of these matters depends on the storage.** Storage that takes its
  data in arbitrary pieces (NVMe with SGLs) writes a loaned WRITE directly,
  and there the budget is what keeps loans from using the region up.
  Storage that needs whole pages (NVMe without SGLs, which is what tardis1
  has) cannot: there the region is meant to be used with a budget of 0, so
  that each WRITE is copied out of it and nfsd's own buffer is written
  directly. `../transplant/README.md`, "Two kinds of storage", sets this
  out with measurements; `../on-die-receive-copy/SPEC.md` is the design for
  the second case.
- **Tested:** KUnit and the whole runtime plan on 4 KiB (sections 7 and
  8, 2026-10-02 to -04) and on 16 KiB (`../transplant/README.md`,
  2026-10-05), there on both kinds of storage. **Every result in this file
  was measured on nvme-loop**,
  which stands for storage that needs whole pages (tardis1's kind). What a
  WRITE does there depends on the budget:
  - **budget 0:** the WRITE is copied once, out of the region into the
    nfsd thread's own buffer (`rq_pages`). That buffer is laid out in whole
    pages, so it **is written direct**. This is the intended setup for
    this storage. On 16 KiB every WRITE of two such cells went direct
    (153215 and 2785).
  - **budget set:** the WRITE is loaned, its pieces end part-way through
    pages, this storage cannot take that direct, and it is written
    buffered: copied anyway, through the page cache.

  The tables in section 8 are all of the second kind, and their MB/s
  columns were taken before the test storage's delay was fixed
  (`../transplant/README.md`, problem 8): they compare nothing. The budget
  on storage where loans stay direct was first run on 16 KiB; its results
  are in `../transplant/README.md`.
- **Not yet carried to the HS twin**
  (`kernel-7.1/hs-7.1.13-19.NFSD_TCP_WRITE_ZEROCOPY`).
- **Open:** a 64 KiB run on tardis1; a 4 KiB run on both kinds of storage; the
  credit factor for 16 and 64 KiB (section 7, finding 3); netdev review of
  the `page_pool` region (section 6).

## 1. The drop

| | |
|---|---|
| Base | `90059375b637` — our docs commit on the pre-rebase `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY` (v7.1.13-14), now on `….v7.1.13-14` |
| Patches | 20, strictly ordered, 167 files, +8,201 / −970 |
| Author | David Flynn; several carry "Claude Opus 5.5" as co-author in the text |
| Sign-off | **none**; each carries `Slipstream-Work-Item:` trailers |
| Built/tested by David | 01–18 on this base (aarch64 4K and 16K, x86_64; KUnit incl. 63/63 svcsock). **19 and 20 only on his previous base** (`df36c8d75594`), ported here unbuilt |

**Integrity, checked 2026-10-02:** all 87 `SHA256SUMS` entries verify. The
20 raw overlays, replayed onto `90059375b637` in a temporary git index (no
checkout), produce exactly the 20 trees listed in `overlays/TREES`.

## 2. Triage

| Group | Patches | On the public zerocopy branch? |
|---|---|---|
| NFSD TCP receive loans, region, budget, early release | 04, 05, 06, 16, 18, 19, 20 | **yes, after review** (05 and 19 need decisions) |
| NFS client LOCALIO (+ block, iomap, XFS, io_uring, task_work, treewide KCSAN marks) | 02, 03, 07, 08, 09, 10, 11, 12, 13, 14, 15 | **no**: an independent client-side topic; 09 alone is 52 files; belongs with the LOCALIO sub-projects, reviewed separately |
| FUSE `FICLONE`/`FICLONERANGE` | 01 | **no**: unrelated subsystem |
| Hammerspace private extensions, forward-ported | 17 | **never on the public branch**; 76 files of HS-private code (auth_name, nfs_ioctl uAPI, XFS, syscall tables). At most the HS twin, and only as a port check against what `kernel-7.1/hs-7.1.13-19` already carries |

The seven receive-path patches are self-contained: applied alone to
`90059375b637` (without 01–03, 07–15, 17) all seven apply cleanly.

## 3. Per-patch review (receive path)

### 04 — SUNRPC: publish prefix-only loan records as COPY

A loan-eligible record that is only the 28-byte prefix no longer publishes
an empty loan context; it is published as COPY. Small, sound fix. Its KUnit
case is in the same commit (our rule splits feature and KUnit commits).
**Recommend: take, splitting the KUnit case into its own commit.**

### 05 — nfsd: retry a direct write segment as buffered on -EINVAL

This is Mike's original "nfsd: fall back to buffered I/O when a direct
write gets -EINVAL", carried forward and ported onto the `nfs_dio` helpers.
That fallback was **NAK'd by Chuck Lever and withdrawn**; the series
replaced it with the admission-time gate
(`nfs_dio_iter_aligned_and_splittable()`), which demotes the geometries
that produced `-EINVAL` before a bio is built. **Recommend: drop.** If a
retry is still wanted as a safety net, it is a separate discussion with
Chuck.

### 06 — kill-switch oracle

Test-harness only (`NFSD_TCP_WRITE_ZEROCOPY/run-killswitch.sh`): correlates
classify/publish per record on one transport. **Recommend: take.**

### 16 — page_pool: fixed receive-buffer region; recycle loaned pages

David's implementation of the on-die landing idea
([`../on-die-receive-copy/SPEC.md`](../on-die-receive-copy/SPEC.md)): an
operator can confine one NIC's order-0 page pools to one physically
contiguous region (`page_pool.rx_region_dev`, `rx_region_pages`; on his
platform a 16 MiB L3 slice, cache-locked), and SUNRPC loans take
page-pool-aware references (`page_pool_ref_netmem()`), so a loaned page
returns to its pool instead of leaving it. Off unless configured. Adds
module parameters and 228 lines to `net/core/page_pool.c`: a netdev-core
change that needs netdev review before any upstream path.
**Recommend: take (off by default); review the page_pool part separately.**

*Since the drop, on this branch* (the triage above describes 16 as David
sent it): `page_pool: Let several devices share the fixed receive region`
made `rx_region_dev` a comma-separated device list. It names devices by
parent bus device (a PCI address), or by interface name when there is no
parent, and every listed device's order-0 pools share the one region.
`page_pool: Add page_pool_page_in_rx_region()` and `page_pool: Export
page_pool_rx_region_size()` give SUNRPC a way to tell region pages and to
check budgets against the region's size. Network-namespace affinity lives
in SUNRPC, not page_pool: per-namespace budgets and a default
budget, with namespace budgets that together may not exceed the region
(`BUDGET_SCOPE.md`). The region still applies only to pools without a
memory provider (`mp_ops`).
Interaction with our fragment-sized loan table (320 entries on 64 KiB):
more loaned fragments per record means more region pages per record, which
18's credit factor has to account for.

### 18 — SUNRPC: region budget admission

Bounds the region pages SUNRPC may hold (`svc_tcp_rx_budget_pages`), stalls
senders through the TCP window instead of ring drops, and reserves part of
the budget for the oldest record so a small region cannot deadlock. Off by default.
558 lines in `svcsock.c`. **Recommend: take; needs a careful read of the
admission/forward-progress logic before any runtime use.**

### 19 — nfsd: release a WRITE's receive loan after the page-cache copy

In buffered and DONTCACHE modes, `nfsd_vfs_write()` strips DSYNC, copies
into the page cache, returns the loan (`svc_rqst_release_payload()`), then
does `vfs_fsync_range()` if the client asked for stability.

**Finding: use-after-free window for NFSv4.** `nfsd4_payload_shared()`
keeps the loan only when a later operation in the COMPOUND is another
`OP_WRITE`. But decoded NFSv4 arguments of other operations alias the
receive buffer: `svcxdr_savemem()` returns a pointer into `rq_arg` memory
for names (LOOKUP, OPEN, CREATE, RENAME, LINK, REMOVE…), opaques and
layoutreturn bodies, and in loan mode that memory is loaned NIC pages (see
`../on-die-receive-copy/SPEC.md` section 2.5 and review finding #1). A
COMPOUND with a WRITE followed by any such operation reads pages already
returned to the NIC page pool, which may have been re-filled. NFSv3 WRITE
(single operation) looks safe.

Fix options (same as the spec's 4.4): release only after the whole
procedure (`pc_func`) has returned, or make every `rq_arg`-aliasing decoded
argument a copy in loan mode. KUnit needs a WRITE followed by a
name-bearing op in one COMPOUND. Also: built and tested only on David's
previous base. **Recommend: hold until fixed.**

### 20 — SUNRPC: copy small RPC records instead of loaning

Whole single-fragment records of at most `svc_tcp_rx_copy_below` (1024)
bytes are copied, not loaned, and skip budget admission. Sound. Depends on
18 (`svc_tcp_rx_credit_admit()`). Built only on David's previous base.
**Recommend: take after 18.**

## 4. Decisions (Mike, 2026-10-02)

1. Scope: receive-path patches only; LOCALIO (02, 03, 07–15) and FUSE (01)
   not on this branch; 17 never on the public branch.
2. Patch 05: dropped.
3. Patch 19: taken as delivered, with a fix on top (`Fixes:`).
4. Metadata: David stays author; Mike's `Signed-off-by:` added;
   `Slipstream-Work-Item:` trailers stripped; 04's KUnit case split into its
   own commit. Mike then authorised adding David's `Signed-off-by:` to every
   commit David authored.

## 5. Applied commits

The receive-path patches went on as David's commits, with his sign-off
added on Mike's authorisation, `Slipstream-Work-Item:` trailers stripped,
and each patch's KUnit part split into its own commit, as the page-loan
series has it: code, then KUnit (the exposure first, then one test commit
per code commit), then documentation.

David's commits and the fixes made straight after them:

| SHA | Commit | From the drop |
|---|---|---|
| | **Code** | |
| `9bd275e24b51` | SUNRPC: Publish prefix-only loan records as COPY | 04 |
| `3a7fa74d9651` | page_pool: Fixed receive-buffer region for one NIC; recycle loaned pages | 16 |
| `f0878b187a79` | SUNRPC: Region credit admission (TCP flow control instead of ring drops) | 18 |
| `f80b0bb7b0d1` | nfsd: Release a WRITE's receive loan after the page-cache copy, before storage | 19 |
| `0ce59d199e48` | SUNRPC: Copy small RPC records instead of loaning a receive page | 20 |
| `0533626ec4ea` | nfsd: keep a WRITE's receive loan while a later operation may read it | fix for 19 (Mike) |
| `b90761719994` | SUNRPC: Orphan a credited record whose loans cannot be accounted for | fix for 18, finding 1 (Mike) |
| `6829ef72dd9c` | SUNRPC: Apply region budget changes while records are in flight | fix for 18, finding 2 (Mike) |
| | **KUnit** | |
| `b0e959e4bea0` | SUNRPC: expose the Slipstream receive internals for the KUnit suites | 18, 19, 20 |
| `c36f0172d33a` | nfsd: expose nfsd4_payload_shared() for the KUnit suites | fix for 19 |
| `f8dfc2218498` | SUNRPC: test prefix-only loan records; nfsd: test the stable_how decode | 04 |
| `862a2a5da6bb` | SUNRPC: test page-pool loan recycling in the svcsock KUnit suite | 16 |
| `ede9488b21a7` | SUNRPC: test region credit admission in the svcsock KUnit suite | 18 |
| `d249cc6f9a6d` | SUNRPC: test early receive payload release in the svcsock KUnit suite | 19 |
| `42fd3cadf62b` | SUNRPC: test copying small RPC records in the svcsock KUnit suite | 20 |
| `24dcb0575714` | nfsd: test which COMPOUND operations keep a WRITE's receive loan | fix for 19 |
| `8daa87052aa5` | SUNRPC: test orphaning a credited record in the svcsock KUnit suite | finding 1 |
| `a20382dea81d` | SUNRPC: test region budget changes in flight in the svcsock KUnit suite | finding 2 |

The later work, all Mike's, follows as a second run of code, KUnit exposure
and KUnit tests. By subject (their SHAs change whenever the branch is
tidied):

| Commit | Why |
|---|---|
| page_pool: Add page_pool_page_in_rx_region() | lets SUNRPC tell a region page from any other |
| SUNRPC: Credit only the region pages a TCP receive loan holds | the budget is for the region; traffic that holds none of it is never limited |
| page_pool: Let several devices share the fixed receive region | several fast ports, one region |
| SUNRPC: Hold the receive buffer only on transports that receive region pages | the fixed TCP receive buffer is for region traffic only |
| page_pool: Export page_pool_rx_region_size() | budgets are checked against the region's size |
| SUNRPC: Give each network namespace its own region budget | containers each get a share; one default budget for the rest |
| SUNRPC: Copy region records when the budget drawn on is 0 | no budget means copy, never unlimited loans; this is the setup for storage that needs whole pages |
| SUNRPC: Do not re-queue a transport that waits for region credit | a waiting connection is run when credit returns or the budget changes, not by every packet |
| tcp: let a kernel socket cap the receive scaling ratio | a TCP interface for the next commit |
| SUNRPC: Hold a region transport's receive window through TCP's interfaces | SUNRPC no longer writes TCP's internal state (finding 4) |

Four more changes found on the 16 KiB host are not part of this run; they
lead the series. "null_blk: keep each memory-backed copy within one block"
(`../BIO-DRIVER-AUDIT.md`) and "dm-delay: do not round a short delay up to
the next timer tick" (`../transplant/README.md`, problem 8) are fixes to block-layer test devices that stand on their own. The write-split fix and its gate ("nfs_common:
split a write's direct middle at page boundaries", "NFSD: count the
misaligned-WRITE split gate in bytes, not pages") follow, so that they can
be folded into the baseline they correct (`NFSD_LOCALIO_DIRECT_SPLIT.md`).
Their two KUnit commits open the first KUnit section.

**How David's patches met the series.**
- 16, KUnit: David's loans take page-pool references, and the series'
  capacity tests fill the whole fragment-sized table, so
  `svcsock_rx_capacity_release_test` expects `state->capacity` page-pool
  references; the sizing test uses plain pages and passes
  `page_pool=false`.
- 18: the new credit fields follow `arena_pages` in
  `struct svc_tcp_rx_state`; `svc_tcp_rx_reset()` releases credit and keeps
  both.
- 19: as delivered it kept the loan only when a later operation in the
  COMPOUND was another WRITE, but other operations' decoded arguments point
  into the same receive pages (names, opaques, layoutreturn bodies). The
  fix keeps the loan unless every later operation is GETATTR or COMMIT. The
  usual client sequence SEQUENCE, PUTFH, WRITE, GETATTR still releases
  early.
- checkpatch: no errors after David's sign-offs; style warnings are left as
  delivered.

**Not re-run since the branch was tidied (2026-10-04):** the per-commit
bisect walk (`run-bisect-walk.sh`) and sparse. Each rebuilt commit was
compiled in sequence; see `../transplant/README.md`, "State of the branch".

## 6. TODO: the rest of the drop

Nothing below is applied. Groups follow David's topics (`DELIVERY.md`);
patch numbers are the drop's (`patches/00NN-*.patch`). Each group needs its
own target branch, review and build on our current base (v7.1.13-19); the
drop was built on `90059375b637` (v7.1.13-14), so expect conflicts with
what landed since, including the LOCALIO DIRECT+DONTCACHE port.

### A. Correctness fixes (NFS client, LOCALIO)

- [ ] **02** NFS: make fsync wait for in-flight direct I/O (3 lines;
      bug fix, low risk; independent)
- [ ] **10** NFS: hold backing-filesystem freeze protection until LOCALIO
      async write completion (bug fix)
- [ ] **11** NFS: extend the freeze hold to NOWAIT LOCALIO writes and make
      its release conditional (bug fix; builds on 09 and 10)
- [ ] **12** NFS: complete the LOCALIO header after init failure
      (superseded by 13, which removes its cleanup again; take only if 13
      is not taken)
- [x] **05** nfsd: buffered retry on direct-write `-EINVAL` — **dropped**
      (the withdrawn fallback; see section 3)

### B. NFS client LOCALIO performance

Overlaps our LOCALIO sub-projects: 15 (completion without the nfsiod hop)
is close to `LOCALIO_IO_SUBMISSION.md`, and 03 touches the direct-write
path that `NFS_LOCALIO_DONTCACHE/` reworked.

- [ ] **07** treewide: mark shared-state accesses for strict-KCSAN-clean
      LOCALIO (31 files across fs, block, kernel, net/sunrpc; prerequisite
      David's later LOCALIO patches assume)
- [ ] **03** NFS: LOCALIO direct-write stability policy (sync / dsync /
      unstable); `nfs.localio_direct_write_policy`, default unchanged
- [ ] **08** NFS: per-CPU admission instead of per-I/O shared locks and
      refcounts (high risk)
- [ ] **09** NFS: io_uring NOWAIT admission for NFSv3 O_DIRECT writes over
      LOCALIO (52 files, +1,839, incl. block, iomap, XFS; high risk; needs
      block/iomap/XFS review, not just NFS)
- [ ] **13** NFS: default-off LOCALIO O_DIRECT request batching
      (`nfs.localio_direct_request_batching`)
- [ ] **14** task_work: export `task_work_add()` / `task_work_run()` to
      GPL modules (needed by 15; a core-kernel export that needs its own
      justification upstream)
- [ ] **15** NFS: opt-in LOCALIO completion without the nfsiod hop
      (`nfs.localio_optimized_completion`, default off), plus always-on
      completion lifetime fixes

### E. FUSE

- [ ] **01** fuse: strict `FICLONE`/`FICLONERANGE` via
      `FUSE_REMAP_FILE_RANGE` (protocol 7.46); unrelated to NFS; decide
      whether Hammerspace carries it at all

### F. Hammerspace private extensions

- [ ] **17** Hammerspace private extensions forward-ported onto
      `90059375b637` (76 files): never on the public branch. When Mike asks
      for HS-twin work, compare it as a port against what
      `kernel-7.1/hs-7.1.13-19` already carries (David ported from the
      older `df36c8d75594` base) and take only real differences.

### Follow-ups on the applied receive-path patches

Done:

- The KUnit case for the 19 fix (`nfsd4_payload_shared_test`).
- A careful read of 18's admission and forward-progress logic (section 7),
  and its two correctness findings fixed.
- Runtime with the region and a budget configured: phases A, B and TLP
  (section 8), then the scope work, phases X, D and N (`BUDGET_SCOPE.md`).
- Finding 4, SUNRPC writing TCP's internal state: fixed by a small TCP
  interface, `tcp_sock_set_scaling_ratio_max()`, and validated on 4 KiB by
  a full run of the test plan that matched the run before it stage for
  stage.
- Finding 5, the budget applying to every connection on the host: designed
  and fixed, `BUDGET_SCOPE.md`.
- A 16 KiB run of the whole plan, `../transplant/README.md`.

Open:

- [ ] **The budget on storage where loans stay direct, at 4 KiB and
      64 KiB.** `run-budget.sh STORAGE=sgl` does it and has run on 16 KiB
      (`../transplant/README.md`). Every result in this file is on
      nvme-loop.
- [ ] **A 64 KiB pass on tardis1**, with the packaged kernel, per
      `../transplant/README.md`.
- [ ] **Finding 3, the credit factor per page size.** `run-budget.sh`
      derives 190 (MTU 9000) and 170 (MTU 1500) for 16 KiB and 150 for
      64 KiB. At 16 KiB no cell paused or overran with a budget set, so 190
      and 170 are not too low. 64 KiB is unmeasured.
- [ ] netdev review of the `page_pool` region as it now stands, before any
      upstream path. The surface is about 260 lines in
      `net/core/page_pool.c`, 23 in `helpers.h` and two in `types.h`:
      - the `rx_region_dev` (device list) and `rx_region_pages` module
        parameters;
      - one shared physically contiguous region (`alloc_contig_pages()`),
        created at the first matching pool, never freed;
      - region pages that a holder keeps past their return leave the region
        and are counted as escaped;
      - a debugfs file;
      - for SUNRPC, one export, `page_pool_rx_region_size()`
        (`EXPORT_SYMBOL_GPL`), and a `static inline` in `helpers.h`,
        `page_pool_page_in_rx_region()`, which reads the pool's
        `rx_region` field from outside page_pool.

      The questions to expect:
      - whether module parameters naming devices are acceptable, or it
        should be per-device configuration through page_pool netlink or
        ethtool;
      - whether the region should be a memory provider (`mp_ops`, as
        devmem TCP and io_uring zero-copy receive use) rather than a
        branch in the default pool's allocation path;
      - how a region that is never freed fits device removal and module
        reload;
      - how the device name matching behaves (parent bus device, else
        interface name) for virtual devices and renames.

      The budgets' own interface (sysctls, possibly netlink) is the
      separate open question in `BUDGET_SCOPE.md`.
- [ ] The sparse `netmem_ref` cast in a KUnit helper:
      `svcsock_rx_pp_refs()` passes `page_to_netmem(page)` and sparse
      reports `cast to restricted netmem_ref`. Test code only.

## 7. Review: 18, region budget admission (2026-10-02)

Read against 18 (`626118a5cc7b`) and everything applied after it
(the fragment-sized loan table, 19 and 20).

**Mechanism.** A global pool (`svc_tcp_rx_budget_pages`, minus a
`svc_tcp_rx_budget_reserve` only the oldest in-flight record may draw).
`svc_tcp_rx_credit_admit()` charges a record at its first body byte an
estimate of `DIV_ROUND_UP(reclen, PAGE_SIZE) × factor / 100 + 1` pages,
capped at `state->capacity`; `svc_tcp_rx_credit_charge()` then tops the
record's credit up before each skb is consumed, by an upper bound on the
loan entries that skb can add (`svc_tcp_rx_skb_pages()`). A denied or paused
transport stops reading and waits in a FIFO by ticket; credit returning wakes
the oldest waiter. The oldest admitted record never waits: it takes the
reserve, then overruns the budget (`overrun` counter), so a small budget
cannot deadlock. `svc_tcp_rx_rcvbuf` (≥ 128 KiB, else budgets stay off) locks
`SO_RCVBUF` and holds TCP's `window_clamp` and `scaling_ratio` from
`svc_data_ready()`, so a waiting socket's unread data stays within its
buffer.

**Verified correct.**
- *No deadlock:* the oldest record always progresses (reserve, then
  overrun); a paused admitted record keeps its older ticket, so it is woken
  before unadmitted records.
- *No lost wakeup:* `svc_tcp_recvfrom()` clears `XPT_DATA` on entry, before
  reading; on a credit wait it does not re-arm it. A wake that lands while
  the thread holds `XPT_BUSY` sets `XPT_DATA`, and `svc_xprt_received()`
  re-queues the transport when it drops `XPT_BUSY`.
- *Credit returned exactly once:* `svc_tcp_rx_reset()` (end of RPC),
  `svc_tcp_rx_release_payload()` (19's early release), the actor's
  classify-as-COPY and start-failure paths, and `svc_tcp_rx_abort()` (via
  `svc_tcp_clear_pages()` on detach) all call `svc_tcp_rx_credit_release()`,
  which is idempotent on `credit_linked`.
- *Teardown:* `svc_tcp_rx_credit_forget()` runs from `svc_tcp_sock_detach()`,
  under `svc_delete_xprt()` while the transport holds a reference and owns
  `XPT_BUSY`, so no receive can re-queue it afterwards and the waker's
  `svc_xprt_get()` never sees a freed transport.
- *Interaction with the fragment-sized table:* the cost cap uses
  `state->capacity`, which is now entries (320 on 64 KiB); charge counts
  entries too, so units agree.

**Findings, and where each stands.**

1. **A record whose loans could not be accounted for blocked everyone
   else.** It stayed the oldest in flight, and only the oldest may use the
   reserve. Fixed: "SUNRPC: Orphan a credited record whose loans cannot be
   accounted for".
2. **Budget changes took effect only when nothing was in flight.** Fixed:
   "SUNRPC: Apply region budget changes while records are in flight".
3. **The admission estimate assumes several packets per page.** Where each
   packet takes a page of its own (16 and 64 KiB) a record holds more
   entries than estimated; `charge()` corrects it per skb, so it is not a
   correctness problem, but the factor needs guidance per page size. Open,
   see section 6.
4. **SUNRPC wrote TCP's internal state** to hold the receive window.
   Fixed: "tcp: let a kernel socket cap the receive scaling ratio" and
   "SUNRPC: Hold a region transport's receive window through TCP's
   interfaces". One read of TCP state remains, in `svc_tcp_at_urg_mark()`.
5. **The budget applied to the whole host**: every connection in every
   network namespace was admitted against it, whether or not its packets
   came from the region. Fixed by the scope work; the requirement, the
   design, Mike's decisions and the before-and-after measurements are in
   [`BUDGET_SCOPE.md`](BUDGET_SCOPE.md).

## 8. Runtime: the budget (18) with the fixed region (16)

Harness: [`../run-budget.sh`](../run-budget.sh), David's stress campaign
(`tools/stress.sh`, `rig.sh`, `veth.sh` in the drop) adapted to run safely
on a shared host: its own rig names (null_blk and dm-delay `zcbudget`, nvmet
subsystem `zc-budget` on its own loop port with its own `addr_traddr`,
export `/export/zcbudget` via `exportfs -o` to the veth subnet), no edits to
`/etc/exports` or `nfs.conf`, and every tunable it touches restored on exit.
Same cells (phase A: 16 MiB region, 1 and 16 connections, MTU 9000 and
1500, 0 → 10 ms → 0 latency steps, receive-queue collapse), same counters
and the same
acceptance criteria as David's report, plus one early-release cell (19)
with a budget set.

Prerequisites on the 4 KiB `hs.451` kernel: `CONFIG_VETH`, `CONFIG_DM_DELAY`
and `CONFIG_BLK_DEV_NULL_BLK` were not set, so they were enabled `=m`, built
with `make modules` and loaded with `insmod` (no new vmlinux, no reboot), and
`fio` was installed. The region is one object per boot, created at the first
matching pool and never freed, so phase B (region at the configured
minimum) needs a fresh boot.

**Read these tables with two things in mind.**

- The harness's storage is nvme-loop, which like NVMe without SGLs needs
  its data in whole pages. A **loaned** WRITE cannot be written directly
  there and is written buffered. So the cells with a budget show the
  budget doing its job (nothing dropped, nothing stuck, the region never
  used up); they do not show loans avoiding a copy, because on this
  storage they cannot. The setup that suits this storage is a budget of 0:
  one copy into nfsd's page-aligned buffer, then a direct write. None of
  the cells below measure that; `../transplant/README.md` does.

### Phase A results — 7.1.13-19.hs.451, 4 KiB pages (2026-10-02)

Code state: public tip with the Slipstream receive-path commits, built
tree `nfsd.ko` (srcversion `BEC1A80BAD947DE96F0545C`), sunrpc
`14AB89F90FEAE59BE81C272`. Region 4096 pages (16 MiB); 309 pages stay parked
in the idle pool after warm-up. Output in `/var/tmp/credit-hs451-A/`.
David's `tools/analyze.py` on it: **OVERALL PASS**.

The first attempt failed every mount with "No route to host": firewalld
puts the new `rxsrv` in the `public` zone. The harness now adds it to
`trusted` at runtime for as long as it exists, and stops the campaign when
the warm-up cell fails.

| cell | conn | budget | MB/s | ring drops / alloc_fail | peak pages | RTO | retrans / DSACK | admitted / denied |
|---|---|---|---|---|---|---|---|---|
| seq-on | 1 | set | 416 | 0 | 2457 | 0 | 0 / 0 | 512 / 0 |
| mixed-on | 16 | set | 119 | 0 | 2189 | 0 | 405 / 403 | 2785 / 7078 |
| mixed-on-v42 | 16 | set | 131 | 0 | 2185 | 0 | 467 / 467 | 2785 / 6689 |
| step-on | 1 | set | 1426 | 0 | 2460 | 0 | 648 / 648 | 163183 / 0 |
| step-on-n16 | 16 | set | 2020 | 0 | 1855 | 0 | 9475 / 9474 | 230163 / 567498 |
| mtu1500-on | 1 | set | 56 | 0 | 596 | 0 | 252 / 243 | 512 / 0 |
| collapse-on | 16 | set | 121 | 0 | 2164 | 0 | 409 / 409 | 2785 / 6978 |
| early-release-m1-d10 | 1 | set | 419 | 0 | 518 | 0 | 19 / 18 | 256 / 0 |

Against David's acceptance criteria:

- Every cell's writes verified (`fio_rc` 0, crc32c verify).
- Zero ring drops, zero `alloc_fail`, zero `escaped`, zero RTOs, zero
  collapse/RcvQDrop/ZeroWindowDrop/BacklogDrop, peak region use at most
  60%.
- No deadlock or stall: all cells finished; 16-connection cells denied
  hundreds of thousands of records and still completed.
- Latency step 0 → 10 ms → 0: recovery 0.0 s (step-on) and 0.5 s
  (step-on-n16); post-step throughput back at pre-step level in both.
- Region accounting is closed: at the end `free` is back to 4096 and
  `escaped` 0.
- Early release (19) with a budget set: peak region use 518 pages against 2457
  for the same sequential load without it (seq-on, 1 ms delay; this cell
  uses 10 ms).

Observation, not a failure: with a budget set, the client's retransmits are
almost all spurious — retransmits ≈ tail-loss probes ≈ DSACKs (step-on-n16:
9475 / 10181 / 9474). Explained and measured in the next subsection.

### Spurious tail-loss probes with a budget set (2026-10-02)

Run: `PHASE=TLP run-budget.sh` on the same hs.451 boot and region (parked
307), output `/var/tmp/credit-hs451-tlp/`. Arms on step-on-n16 (16
connections, 0 → 10 → 0 ms) and mixed-on (16 connections, 1 ms):

| cell | MB/s | GB written | retrans | TLPs | DSACKs | srv DelayedACKLost | srv segs out | ring drops | RTO |
|---|---|---|---|---|---|---|---|---|---|
| step-on-n16 (baseline repeat) | 2004 | 239.5 | 9449 | 10207 | 9448 | 9449 | 31.6 M | 0 | 0 |
| + server route `quickack 1` | 1758 | 206.3 | 1 | 0 | 0 | 0 | 46.4 M | 0 (1 RcvQDrop, 3 collapsed) | 0 |
| + client `tcp_early_retrans=0` | 2003 | 239.1 | 0 | 0 | 0 | 0 | 31.6 M | 0 | 0 |
| mixed-on (repeat) | 123 | 1.1 | 399 | 401 | 399 | 399 | 144 K | 0 | 0 |
| + server route `quickack 1` | 123 | 1.1 | 0 | 0 | 0 | 0 | 236 K | 0 | 0 |

Mechanism (confirmed by the counters, not just inferred):

- With the locked rcvbuf and window clamp, any unread data makes the new
  window smaller than `rcv_wnd`, so `__tcp_ack_snd_check()` drops its
  "ACK every second full segment" rule ("tcp_recvmsg() will send ACK
  otherwise"); the ACK waits for the reader or for the delayed-ACK timer
  (`TCP_DELACK_MIN` = HZ/25 = 40 ms at HZ=100).
- A transport parked for credit does not read, so neither comes in time.
  The client's probe timeout is 2·srtt + 2 ms, a single 10 ms jiffy on
  veth, and fires first: it resends the tail segment, the server sees a
  duplicate (`DelayedACKLost` = `TCPDSACKOldSent` = retransmits, 9449 each)
  and ACKs it at once with a DSACK. One stall, one TLP, one DSACK. Only 1
  of 10207 probes recovered a real loss (`TCPLossProbeRecovery`).
- Per-route quickack removes all of them, as does turning TLP off at the
  client — so nothing else is retransmitting.

Cost: none worth fixing. The probes are at most 9449 × 9000 B ≈ 85 MB of
239.5 GB written (< 0.04%), and throughput with TLP off is unchanged (2003
vs 2004 MB/s), so they neither rescue nor hurt. The obvious server-side
fix is worse: quickack on every segment raises the server's segment count
47% (501510 window updates the server wanted to send as zero-window
advertisements), costs 12% in the high-rate step cell (equal at 1 ms), and
breaks the clamp's no-overflow promise (1 RcvQDrop, 3 collapsed).
`tcp_enter_quickack_mode()` is static, and the exported
`tcp_sock_set_quickack()` only pushes one pending ACK, so anything in
between would mean sunrpc writing more TCP internals — the same netdev
concern as the clamp writes (section 7, finding 4). Leave it; on real NICs, with
srtt in tens of microseconds and HZ=1000, PTO is ~2 ms and the ratio is
worse, so recheck on tardis1 before upstream.

**Harness note.** Deleting the client namespace while server sockets still
hold unacknowledged FINs leaves them orphaned in FIN-WAIT-1 with
out-of-order segments queued; those keep a few region pages until the
orphan retries expire. Not a leak, but it skews the peak of the next cell.

### Phase B results — 7.1.13-19.hs.451, region at the configured minimum (2026-10-02)

Code state: fresh boot of `7.1.13-19.hs.451` with its installed modules
(sunrpc `14AB89F90FEAE59BE81C272`, the tree's build; nfsd
`B54161B99157BEC33284446`, which carries the 19 fix and differs from the
tree's only by the KUnit export commit). Output in
`/var/tmp/credit-hs451-B/`. David's `analyze.py`: **OVERALL PASS**.

Phase B follows David's stress.sh: the region is sized to the configured
minimum, parked + 256 receive-buffer pages (4 × 128 KiB) + one record's
reserve (390) + 64 slack = **1019 pages** with phase A's 309 parked
(David: 306, so 1016). Budget = reserve = 390, so ordinary admission has
nothing and records only start as the oldest, on the reserve: one at a
time. The first cell creates the region; the harness refuses to start if
a region already exists this boot and stops if the first cell does not
get exactly 1019 pages.

| cell | conn | budget | MB/s | ring drops / alloc_fail | peak pages | RTO | admitted / denied | verdict |
|---|---|---|---|---|---|---|---|---|
| edge-at-n1 | 1 | 390 (= reserve) | 52 | 0 | 416 | 0 | 256 / 581 | PASS |
| edge-at-n4 | 4 | 390 | 52 | 0 | 476 | 0 | 256 / 856 | PASS |
| edge-at-mixed-n4 | 4 | 390 | 19 | 0 | 478 | 0 | 2785 / 8948 | PASS |
| edge-at-step (0 → 10 → 0 ms) | 1 | 390 | 1139 | 0 | 421 | 0 | 130368 / 290045 | PASS, recovery 0.5 s (1697 → 52 → 1699 MB/s) |
| edge-below-n1 (budget for a region 64 pages larger) | 1 | 646 | 52 | 0 | 415 | 0 | 256 / 589 | REPORT: clean |

- Forward progress: with only the reserve, every cell completed and every
  write verified (`fio_rc` 0, crc32c); no hang, no `paused` or `overrun`.
- The minimum holds: at 1019 pages, no ring drops,
  allocation failures, RcvQDrop, ZeroWindowDrop or BacklogDrop, including
  4 connections and the latency step. Peak use stays at 415–478 pages
  (parked plus about one record), far below the region.
- Margin: budgeting for a region 64 pages larger is still clean.
- Accounting closed: the region ended at 1019 free, `escaped` 0.
- Spurious TLPs (retransmits ≈ DSACKs) again, as explained above.

To rerun: on a fresh boot, before anything attaches XDP to `rxsrv`,
`PARKED=$(cat /var/tmp/credit-hs451-A/parked.txt) PHASE=B bash
NFSD_TCP_WRITE_ZEROCOPY/run-budget.sh <outdir>`, then
`tools/analyze.py <outdir>`. The harness loads nfsd and nfs itself
(a fresh boot has neither) and veth, dm-delay and null_blk from the
tree's build.

