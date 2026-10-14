# The code series: survey and reordering

The series' code group, on `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`
over `v7.3-3`: what each commit is, which are not needed, and the order
to rebase them into (Mike, 14 October). The goal of the reordering:

- **A logical flow**: foundational changes first, then the features
  that build on them.
- **A fix right after what it fixes**: every in-series `Fixes:` placed
  immediately after its target where it can be.
- **Features that are off by default last** in the code group.

This file records the survey (step 1) and the trial rebase (step 2),
and is to be kept up to date through the reordering itself. The groups stay as `AGENTS.md` has them,
code, then KUnit interface, then KUnit tests, then docs, and one group
to a commit. The branch itself has not been changed yet.

## Step 1: the survey (14 October)

At `11ded7305126` the branch is 475 commits over `v7.3-3`: 66 code
commits (65 at the bottom of the series and one at the tip, `#66`), 44
KUnit (42 after the code and two of 14 October after the docs) and 365
docs. The numbers below are the code commits' order in the branch
today.

| # | commit | subject | lines | notes |
|---|---|---|---|---|
| 1 | `804337ca897a` | SUNRPC: distinguish authoritative XDR bvecs | +30/-1 | |
| 2 | `35d13946eb7c` | SUNRPC: decode XDR from authoritative bvecs | +464/-9 | |
| 3 | `57e1da8f9133` | SUNRPC: poison invalid XDR decode streams | +1 | |
| 4 | `9cfc14eec1cb` | nfsd: accept immutable receive bvec requests | +70/-45 | fixed by 19 |
| 5 | `f830dc5d450b` | nfsd: require interior payload discontinuities to be logical-block aligned | +47/-23 | |
| 6 | `88a3b0ea0f4e` | SUNRPC: loan eligible TCP receive pages | +1200/-22 | the loan; fixed by 18, 20 |
| 7 | `dc72421fb1ad` | SUNRPC: merge locked-head copies into a whole-page loan bvec | +158/-1 | fixed by 42 |
| 8 | `bb17236427cb` | nfsd: accept receive bvecs for NFSv4 COMPOUND | +14/-5 | |
| 9 | `037f7f5cc3aa` | SUNRPC: add a runtime switch to disable TCP receive page loans | +17 | `svc_tcp_rx_loan_pages`, on |
| 10 | `ed42ca4c8830` | block: add bdev_dio_seg_boundary() | +17 | |
| 11 | `388d9c180230` | fs: add STATX_DIO_SEG_BOUNDARY | +41/-4 | |
| 12 | `5af8f3fd0693` | block: report STATX_DIO_SEG_BOUNDARY for block devices | +5/-1 | |
| 13 | `afde964fae63` | xfs: report the direct I/O segment boundary | +4/-1 | |
| 14 | `85bde8a734fd` | nfs_common: let the direct I/O policy carry the device's segment boundary | +79/-14 | |
| 15 | `4066c940a926` | nfsd: record the direct I/O segment boundary of an nfsd_file | +23/-7 | |
| 16 | `c2f75320d2b6` | nfsd: split direct writes by the device's segment boundary | +2 | |
| 17 | `ea72a31c91f1` | loop: take the backing device's virtual boundary for direct I/O | +29/-4 | |
| 18 | `3b80699d33d9` | SUNRPC: size rq_bvec for 4 KiB fragments, not pages | +28/-1 | Fixes 6 |
| 19 | `f904a540b8c7` | nfsd: convert a WRITE payload into rq_bvec by its own size | +1/-1 | Fixes 4 |
| 20 | `14c4fa69dfb7` | SUNRPC: size the TCP receive loan table for fragments, not pages | +20/-4 | Fixes 6 |
| 21 | `c84cc87f5105` | SUNRPC: Publish prefix-only loan records as COPY | +7 | a fix to 6 with no tag; message to rewrite (below) |
| 22 | `ac560819d57a` | page_pool: Fixed receive-buffer region for one NIC; recycle loaned pages | +267/-7 | the region; `rx_region_pages` |
| 23 | `405009496e90` | SUNRPC: Region credit admission (TCP flow control instead of ring drops) | +529/-4 | the budget; fixed by 27, 28, 30, 32, 36 |
| 24 | `bff6e0e23f84` | nfsd: Release a WRITE's receive loan after the page-cache copy, before storage | +94/-2 | fixed by 26 |
| 25 | `c59293e1fe58` | SUNRPC: Copy small RPC records instead of loaning a receive page | +25/-2 | `svc_tcp_rx_copy_below`, 1024 |
| 26 | `032a08a2f5b5` | nfsd: keep a WRITE's receive loan while a later operation may read it | +16/-3 | Fixes 24 |
| 27 | `f00504046ada` | SUNRPC: Orphan a credited record whose loans cannot be accounted for | +34/-4 | Fixes 23 |
| 28 | `b9581988624c` | SUNRPC: Apply region budget changes while records are in flight | +22/-5 | Fixes 23 |
| 29 | `9dd367d4c162` | page_pool: Add page_pool_page_in_rx_region() | +14 | helper for 30 |
| 30 | `78a2a0b985c4` | SUNRPC: Credit only the region pages a TCP receive loan holds | +46/-36 | Fixes 23; fixed by 40, 41 |
| 31 | `1057421174ba` | page_pool: Let several devices share the fixed receive region | +30/-16 | |
| 32 | `465163e8d519` | SUNRPC: Hold the receive buffer only on transports that receive region pages | +56/-23 | Fixes 23 |
| 33 | `ff643ab2540a` | page_pool: Export page_pool_rx_region_size() | +27 | helper for 34 |
| 34 | `db41b8c114bb` | SUNRPC: Give each network namespace its own region budget | +373/-55 | |
| 35 | `6c8a4633a068` | SUNRPC: Copy region records when the budget drawn on is 0 | +48/-3 | |
| 36 | `384c23ff8de4` | SUNRPC: Do not re-queue a transport that waits for region credit | +126/-17 | Fixes 23 |
| 37 | `838bd0cbdb9b` | tcp: let a kernel socket cap the receive scaling ratio | +34 | used by 38, still by HEAD |
| 38 | `8c9a24c566b1` | SUNRPC: Hold a region transport's receive window through TCP's interfaces | +8/-34 | |
| 39 | `a057f0952ad0` | veth: add rx_ip_align, extra headroom for frames copied for XDP | +19/-2 | test rig only; 0, off |
| 40 | `af3843ce958d` | SUNRPC: Charge a TCP receive loan for the region pages it holds, not its fragments | +66/-19 | Fixes 30 |
| 41 | `bfa0e3ee56b9` | SUNRPC: Take a transport that reads on without admission off the credit wait list | +9 | Fixes 30 |
| 42 | `55688fb1ebcb` | SUNRPC: Count a merge page's bytes once when a TCP receive record is materialized | +35/-8 | Fixes 7 |
| 43 | `6fd12aa87801` | nfs_common: do not issue a write that starts beyond EOF as direct I/O | +20/-1 | reverted by 44 |
| 44 | `8c79bb185e75` | Revert "nfs_common: do not issue a write that starts beyond EOF as direct I/O" | +1/-20 | |
| 45 | `23871cb87de9` | page_pool: Give the fixed receive region back when it is resized | +67/-8 | |
| 46 | `2e9af09d3257` | block: Let a stacking device take its members' memory alignment | +8/-1 | stands alone |
| 47 | `1b062140c3a2` | SUNRPC: Trade pages with the socket when a partial record changes hands | +17/-15 | |
| 48 | `87fa242b3e50` | SUNRPC: Return a loaned record's page-pool pages in batches | +22/-3 | fixed by 52 |
| 49 | `1900843944ab` | page_pool: Recycle a region-bound pool's pages last in, first out | +84/-2 | |
| 50 | `45e48770d02d` | SUNRPC: Let a NUMA node have several thread pools | +43/-14 | `pools_per_node`, 1; 4 in use |
| 51 | `7554e4ce6244` | SUNRPC: Let a thread wait for the rest of a partly received TCP record | +57/-3 | `svc_tcp_rx_linger_us`, 0; 50 in use on one port |
| 52 | `72330b2d351d` | SUNRPC: Keep page-pool return batches within the bulk API's bound | +8/-2 | Fixes 48 |
| 53 | `1b949fb90e2b` | SUNRPC: Refuse to build a page-pool return batch the bulk API cannot take | +3 | the build-time check for 52's bound; no tag |
| 54 | `c02c85e0d27f` | SUNRPC: Trace how long a thread lingers for the rest of a TCP record | +84/-7 | tracepoints the rig uses |
| 55 | `384086e570a7` | SUNRPC: Learn a loaned record's charge from the records | +124/-12 | |
| 56 | `afcf88ab9cc7` | SUNRPC: Hold a transport's receive window by what its packets occupy | +140/-33 | |
| 57 | `ff794a9cab34` | SUNRPC: Remove svc_tcp_rx_credit_factor | +18/-32 | removes what 23 added, once 55 and 56 replace it |
| 58 | `48ce6e5c445c` | SUNRPC: Show what receive buffers are held to in a budget's statistics | +3/-2 | |
| 59 | `4fad48bed76f` | SUNRPC: Measure a held receive buffer by the skbs that carry data | +22/-12 | |
| 60 | `a1a2e6d3554d` | SUNRPC: Open a held transport's window to half of what its buffer holds | +32/-17 | |
| 61 | `a9fe067f1882` | SUNRPC: Trace what a transport's receive buffer is held to | +36 | tracepoint the rig uses |
| 62 | `e8bb4b0bf2ed` | SUNRPC: Copy a received TCP record with the socket unlocked | +74/-8 | added `svc_tcp_rx_copy_unlocked`, removed by 66 |
| 63 | `8439fe6d5edc` | SUNRPC: Let a thread claim a TCP record's data and copy it afterwards | +170/-14 | `svc_tcp_rx_copy_threads`, 0; not adopted |
| 64 | `273a1b9f355c` | SUNRPC: svc_tcp_rx_keep: a thread that finds a record short does not wake another | +51/-5 | `svc_tcp_rx_keep`, N; Y in use |
| 65 | `ad121a71159b` | SUNRPC: svc_tcp_rx_lowat: wake a thread for more than one piece of a short record | +46 | `svc_tcp_rx_lowat`, 0; costs rate, not adopted |
| 66 | `1db924c7059d` | SUNRPC: Copy with the socket unlocked only when records are claimed | +7/-13 | at the tip; reworks 62 |

Every `Fixes:` tag in the code group names a commit inside it; none
points outside the series.

### Not needed

- **43 and 44 cancel exactly**: the diff from before 43 to after 44 is
  empty. They are the "beyond EOF through the page cache" rule, measured
  once and taken out. Drop both. The region budget page says it is "kept
  in the history to come back to", and it stays so on
  `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY.pre-rebase-v7.3-3` (and the
  7.1.13 branches); that page's wording should say where.

Nothing else is dead. 37's `tcp_sock_set_scaling_ratio_max()` is still
called by `net/sunrpc/svcsock.c` at the tip, and the tracepoints of 54
and 61 are read by the rig's scripts and documents.

### To fold or recut

- **62 with 66**: 66 removes 62's knob, so the unlocked copy is now
  made only when records are claimed (63). Fold 66 into 62 and place
  the result after 63, as the claim's fallback for what the actor
  cannot claim; or fold both into 63. Either way no commit adds a knob
  that a later one removes.
- **57 into the commits that replace it**: 23 adds
  `svc_tcp_rx_credit_factor`, 55 (the charge learned) and 56 (the
  window measured) make it unneeded, 57 removes it. Folding 57 into 56
  keeps the history honest at less cost than recutting 23 without it.
- **53 with 52**: 52 bounds the page-pool return batch, 53 makes a
  batch over the bound fail the build. Place 53 right after 52 or fold
  it in; tag it `Fixes: 48` if kept apart.
- **21**: a fix to 6 (a loan-eligible record that is only its 28-byte
  prefix published an empty loan that pinned the request's receive
  state) with no `Fixes:` tag, and a 40-line message about an older
  base and a delivery package ("The Hammerspace base `90059375b637`
  already contains..."). Place it after 6, tag it, and rewrite the
  message to say what it fixes. It is David Flynn's commit: keep his
  authorship and Signed-off-by, and note the rewrite in a
  `[snitzer: ...]` line.

### Off by default

Two kinds, which go to different places:

| | commits | default | where |
|---|---|---|---|
| tried and not adopted | 63 (claim, then copy; with 62/66), 65 (the 128 KiB mark) | 0 | **the end of the code group** |
| test rig only | 39 (veth `rx_ip_align`) | 0 | the end of the code group, with the two above, or out of the series |
| off by default, in use at the settled setting | 50 (`pools_per_node` 4), 51 (linger 50 on one port), 64 (`keep` Y), and the held buffer of 23 (`svc_tcp_rx_rcvbuf`) | 1, 0, N, 0 | where they belong in the flow: they are part of what is measured |
| a feature for drives tardis1 lacks | the budget, 23 and after | 0 (copy) | the flow: it is the loan's |

63 and 65 touch the same receive path as 64 and 51 but neither changes
a line 64 changes (65 mentions `keep` only in context), so both can
move past 64 as they stand; the trial rebase will say.

### A first order

By cluster, foundations first. Where a cluster's own order is already
right it is kept. To be checked by a trial rebase before anything is
pushed.

1. **Block and file-system foundations**, independent of NFS: 46
   (stacking alignment), 10, 11, 12, 13 (`STATX_DIO_SEG_BOUNDARY`), 17
   (loop's virtual boundary).
2. **TCP and page-pool foundations**: 37 (`tcp_sock_set_scaling_ratio_max`),
   22, 31, 45, 49 (the region, shared by several devices, resized, LIFO),
   29, 33 (page-pool helpers, or each next to its first user).
3. **XDR from bvecs**: 1, 2, 3.
4. **nfsd and nfs_common, direct writes split by the device**: 4, 19
   (fixes 4), 5, 14, 15, 16.
5. **The loan**: 6, 18, 20, 21 (fix 6), 7, 42 (fixes 7), 8, 9, 25, 24,
   26 (fixes 24), 47, 48, 52, 53 (fix 48).
6. **The budget and the held window**: 23, 27, 28, 30, 40, 41, 32, 36
   (fixes to 23 and 30), 34, 35, 38, 55, 56 with 57, 58, 59, 60, 61.
7. **Receive-path features in use**: 50 (pools), 51 and 54 (the linger
   and its trace), 64 (`keep`).
8. **Off by default, not adopted**: 63 with 62 and 66, 65. Then 39 if
   it stays in the series.

Dropped: 43, 44.

## Step 2: the trial rebase (14 October)

On the scratch branch `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-reorder-1`,
in a worktree of its own (the rig runs its scripts from the main one),
with the tip of the day saved first as
`kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY.pre-reorder` (`a57fe1eca868`).
The code group was cherry-picked onto `v7.3-3` in a new order, then the
KUnit group and the docs group in theirs, the two KUnit commits of 14
October moved into place (the interface one after the interface
commit, the test one after the last test).

**The result: 471 commits, and the tree is identical to the tip's.**
`git diff` between the two is empty: the reorder, the dropped pair,
the folds and the two conflicts resolved by hand change no code.
`transplant/check-grouping.sh v7.3-3` passes ("code, then KUnit, then
docs"). The code group is 61 commits: 66, less 43 and 44, less 57
folded into 56 and 62 and 66 folded into 63.

### The order it settled on

| cluster | commits, by their survey numbers |
|---|---|
| block, file systems, loop, TCP | 46, 10, 11, 12, 13, 17, 37 |
| XDR from bvecs | 1, 2, 3 |
| nfsd and nfs_common: direct writes split by the device | 4, 19 (fixes 4), 5, 14, 15, 16 |
| the loan | 6, 18, 21 (fixes to 6), 7, 20 (fixes 6), 8, 9 |
| the region | 22, 29, 31, 33, 45, 49 |
| the budget and the loan's life under it | 23, 27, 28 (fixes to 23), 24, 26 (fixes 24), 25, 30, 40, 41 (fixes to 30), 42 (fixes 7), 32, 34, 35, 36 (fixes 23), 38, 47, 48, 52, 53 (fix 48) |
| receive-path features in use | 50 (pools), 51 and 54 (the linger and its trace) |
| the held window | 55, 56 with 57, 58, 59, 60, 61 |
| `keep`, in use | 64 |
| off by default, not adopted | 63 with 62 and 66, 65, 39 |

### What moved back, and why

Each was moved where the first order put it, conflicted, and went back
to follow the commit it is written on:

- **20** (fixes 6) after **7**: it adds fields beside `merge_fill`,
  which 7 adds.
- **42** (fixes 7) after **41**, not beside 7: it calls
  `svc_tcp_rx_credit_orphan()` (27) and changes code the region (22)
  wrote.
- **24, 25, 26, 47, 48, 52, 53** after the region and the budget, not
  in the loan's cluster: they are written on 22 and 23.
- **36** (fixes 23) after **34** and **35**, not with 23's other
  fixes: it is written on both.
- **61** after **54**, and so the pools and the linger (50, 51, 54)
  before the held window (55 to 61), as they were: 61's tracepoint sits
  beside 54's.

### Resolved by hand

**64** (`keep`) was written on **63** (the claim): its change to the
error path of `svc_tcp_recvfrom()` sits after the claim's own lines
there. With 63 moved after 64, 64 was applied without the claim's
lines (`svc_xprt_received_keep()` when `keep` is set, as its own
change), and the claim's fold put them back in front of it. The tree
being identical at the end is the check that both were right. git's
rerere has both resolutions.

### Before it can replace the branch

- **The messages of the folds.** 56 with 57 keeps 56's message for
  now and has to say that the factor goes. 63 with 62 and 66 has 63's
  message; the refined one (Mike: "with the commit header refined
  accordingly") is drafted below.
- **21's message** rewritten (above), with `Fixes:` on 6, and **53**
  tagged `Fixes:` on 48.
- **The `Fixes:` tags re-pointed.** 15 carry the old IDs: 13 in the code
  group and 2 in the KUnit group (on "SUNRPC: exhaustively test TCP
  receive page loans"). Each still quotes its target's subject, which is
  why the grouping check passes.
- **The commits made at the tip since the trial began** carried over:
  "the loan page's lede links to the loan in outline" (`85e8ab4b7aef`)
  and this file's own commits.
- **The region budget page's** "kept in the history" for the beyond-EOF
  rule (43, 44) pointed at the branch that keeps it.
- Then step 3 below: each commit built in turn.

### The folded claim commit's message, drafted

    SUNRPC: Let a thread claim a TCP record's data and copy it outside the socket lock

    svc_tcp_recvfrom() holds the socket lock across ->read_sock(), and a
    record that is copied is copied by the actor, skb by skb, under that
    lock: a few hundred microseconds for a 1 MiB WRITE.  TCP counts each
    skb as read, and opens the window for more, only once it has been
    copied; segments that arrive meanwhile wait on the socket's backlog,
    unacknowledged; a reply to the same connection waits for the lock;
    and one thread at a time copies for a transport.

    Add svc_tcp_rx_copy_threads.  At 1 the actor claims the data, a clone
    of each skb, and the thread copies what it claimed once ->read_sock
    has returned, with the socket unlocked: the next part of the record
    is on its way while this one is copied.  At 2 or more, a thread whose
    claim completes a record gives the transport up before it copies, so
    that another reads the next record meanwhile; sk_rx_copying holds the
    threads doing so for one transport to that many less one, and at the
    limit a thread copies before it gives the transport up.  At 0, the
    default, the actor copies under the lock as before.

    What the actor cannot claim, past SVC_TCP_RX_CLAIM_MAX, it copies
    itself with the socket unlocked, from a clone of the skb: TCP may free
    a queued skb once the lock is dropped (tcp_collapse() frees whatever
    references it has), the clone holds the data, and TCP does not
    coalesce into a cloned skb.  tcp_read_sock() allows for an actor that
    drops the lock: it looks the skb up again by sequence number and
    counts nothing as read until the actor returns.

    A complete record's pages are the request's own and nothing reads
    past its header before svc_tcp_recvfrom() returns, which it does
    after the copy.  A backchannel reply is copied before it is read, and
    a record that fails after it was claimed is not copied.  Only TCP's
    own ->read_sock is treated this way, not kTLS's, and only copies of
    4 KiB or more; the record marker, the fixed prefix and a loaned
    record's copies stay under the lock.

    Claimed data is no longer charged to the socket: a transport can have
    as much again as its receive buffer waiting to be copied for each
    thread that may be copying.

## Next steps

1. Mike reviews the survey and the first order.
2. ~~A trial rebase on a scratch branch~~: done, step 2 above. The
   final pass adds the messages, the tags and the commits since.
3. The tree at the new code group's end compared with today's
   (`git diff` empty but for 43/44 and the folds, which change no code),
   then each code commit built in turn (bisect-clean, `W=1`, sparse,
   checkpatch) and the KUnit groups after them, as `README.md`'s
   build and bisect instructions say.
4. `transplant/check-grouping.sh` on the result, and the 7.1.13 branch
   left as it is.
