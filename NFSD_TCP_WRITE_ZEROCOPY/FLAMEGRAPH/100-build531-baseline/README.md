# Round 100: build 531's baseline and profile, for the next round's focus

13 November, tardis2, `7.3.3-16.hs.531.canary` (the tree's code built
in, nothing in `updates/`: sunrpc `6521F092BF4BEB6575F6802`, nfsd
`D95233FF10702BC0EC82B7B`, xeu Release 15 `C3071685F0211EF8EBA2A7F`
with 4 rings from the kernel's own `extra/`).  Both ports 5 of 5 before
`bringup.sh 32768 256`; kdump armed; `settled.sh apply` ends in "the
settled setting".  The client: sunrpc `AB674CEE87351D814F68CEE` (the
splice), `affinity_strict` 1 on `rpciod` and `xprtiod`.

**The ask (Mike):** a baseline and a profile on the new kernel, to
verify the settled two-port setting still carries about 78 GB/s
(round 99's `t2-r597`, `r598`: 78.06, 78.13) and to see what stands
out for the next round.

## The runs

- Baseline: `CELLS=":10 :10" N0=599 ../99-held-split-splice/run577.sh`,
  as `t2-r597`, `r598`: `t2-r599`, `r600`.
- Profile: [run601.sh](run601.sh), round 95's `run501.sh` at the
  settled setting: `t2-r601` counted on nfsd's CPUs, `t2-r602` call
  graphs on nfsd's CPUs, `t2-r603` call graphs on the rings' CPUs.

## Result: 78 GB/s as on build 522's stack; the drives are the wall

[run599.out](run599.out), [run601.out](run601.out).  Every run clean:
no ring drop, no loan denied, no drive stepped down, no new kernel
warning, 184 to 228 client retransmits (the spurious tail loss probes
of round 99).

| run | kind | GB/s | WRITEs/s | held median, most | to memory |
|---|---|---|---|---|---|
| `t2-r599` | baseline | 78.03 | | 48 MiB, 3345 | 114.4 GB/s, 1.46 a byte |
| `t2-r600` | baseline | 77.99 | | 48 MiB, 3364 | 114.2, 1.46 |
| `t2-r601` | counted | 77.96 | 148,898 | | |
| `t2-r602` | call graph | 77.98 | 148,375 | | |
| `t2-r603` | rings' call graph | 78.05 | 148,862 | | |
| round 99, `t2-r597`, `r598` | baseline | 78.06, 78.13 | | 48 MiB, 3397, 3365 | 115.7, 116.1 |

**Build 531 carries round 99's rate** to within 0.1%, at the same held
median and slightly fewer bytes through memory (1.46 against 1.48).

**Every drive writes 9.6 to 9.8 GB/s of its 10.2, busiest 97 to 98%
busy, and each group of the server's CPUs is 17 to 24% busy (rings of
a, rings of b, nfsd); the client is 49%.**  Nothing on the server's
CPUs holds the rate: what is left is the drives' last 4% (81.6 GB/s
for eight), and of that the 3% of the time a drive is empty (round
99's "When a drive runs empty").

**nfsd's cycles a byte are a third higher than round 95's**
([symmib-r501-r601.txt](symmib-r501-r601.txt), with
[symmib.py](symmib.py): cycles on nfsd's CPUs, idle out, a MiB
written): 331k a MiB against `t2-r501`'s 248k (1 MiB WRITEs, window
0, 71.6 GB/s).  It is not build 531's: twice the WRITEs for the same
bytes doubles what each WRITE pays once, as
[read65-r601.txt](read65-r601.txt) shows (165k a WRITE, about two
thirds of 1 MiB's 248k).  The growth a MiB, largest first:

- `finish_task_switch` +8.9k: the cycles just after a switch.
  Context switches are 1.05 M/s against 0.81 M/s; by
  [schedwhy-r602.txt](schedwhy-r602.txt) 69% of them leave the idle
  loop, 13% a thread waiting for work, 12% `nfsd_direct_write`
  waiting for its I/O.  With nfsd's CPUs 78% idle this is the price
  of waking from idle, not of contention.
- What a WRITE does once: the DRC (`nfsd_cache_insert` +1.3k, with
  `_raw_spin_lock_bh` +1.2k), the export and auth caches
  (`sunrpc_cache_find_rcu` +1.2k), XFS's extent lookup
  (`xfs_iext_lookup_extent` +1.0k), `nfsd_direct_write` and
  iomap's (+2.5k together), the reply's transmit (`xeu_start_xmit`,
  `xeu_tx`, `xeu_maybe_stop_tx` +2.5k, absent from `t2-r501`'s
  symbols only because xeu's were unresolved then).
- The window, on here and off in round 95:
  `svc_tcp_rx_target_admit` 1.4k a MiB and `svc_tcp_rx_target_wake`
  0.5k, 0.6% of nfsd's cycles.

Down: the region's page puts (`rx_region_page_put`,
`rx_region_slot_put`, -1.8k), the shorter records' lighter return.

The rings' CPUs ([rings-sym-r603.txt](rings-sym-r603.txt), idle in):
`handle_softirqs` 9.8% (the skid at irq enable), `xeu_rx_poll` 6.0%,
then the build of the skb and GRO (`napi_build_skb`, `skb_gro_receive`,
`inet_gro_receive`, `gro_list_prepare`, `tcp_gro_receive`,
`dev_gro_receive`, about 16% together), `xeu_rx_refill` 3.2%,
`xeu_tx_poll` 2.7%; 17% idle in the sample.  Round 98's order, and
nothing new.

## What stands out for the next round

1. **The rate is the drives'.**  At 78 of 81.6 GB/s with every drive
   98% busy, a software change can win at most the 3% a drive is
   empty.  Round 99 found that time is a WRITE still arriving when
   the drive runs dry; the window (10) and the wsize (512 KiB) are
   what bound it, and both were swept.  More rate needs more drives
   or faster ones.
2. **If the round is for the server's CPU instead (the room it leaves
   the product), the target is the cost each WRITE pays once,**
   doubled by the 512 KiB wsize: the DRC's insert and its lock, the
   export cache's lookups, XFS's extent lookup, and the wakes from
   idle (seven switches a WRITE).  The DRC is the plainest: an NFSv3
   WRITE over TCP is cached and inserted under a lock on every call.
3. The window's own cost (0.6% of nfsd's cycles) is small but is now
   on in the settled setting, where round 95 measured it off.

## The DRC's cost for an NFSv3 WRITE (t2-r602)

An NFSv3 WRITE is `RC_REPLBUFF`: every one is looked up, inserted,
and its reply copied into the cache.  From `t2-r602`'s call graphs
([drc-r602.txt](drc-r602.txt), round 77's `under.py`), cycles on
nfsd's CPUs a WRITE:

| where | cycles a WRITE |
|---|---|
| `nfsd_cache_lookup` | 4,331 |
| ... the rbtree walk and insert (`nfsd_cache_insert`, IPC 0.09) | 1,400 |
| ... the prune of the oldest entry (`rb_erase`, list) | 953 |
| ... freeing the pruned entry and its reply (`kfree`, `kmem_cache_free`) | 668 |
| ... the checksum of the call (`read_bytes_from_xdr_buf`, `do_csum`) | about 640 |
| ... the new entry (`nfsd_cacherep_alloc`) | 314 |
| ... the stats' per-CPU counters | 162 |
| `nfsd_cache_update` (the reply's `kmalloc` and copy, LRU tail) | 662 |
| the bucket lock, taken from `nfsd_dispatch` | 463 |
| **all** | **about 5,500** |

That is 3.3% of nfsd's 165k busy cycles a WRITE, about 11k a MiB, or
a quarter of one CPU at 149k WRITEs a second.  The bucket lock is
not contended (`queued_spin_lock_slowpath` is under
`__dev_xmit_skb`'s and `svc_recv`'s locks).  The rest is the cost of
cold memory and of the allocator, not of the lock.

**Why cold:** at `drc_max_entries` 4096 (round 12) the cache is
always full (`reply_cache_stats`: 4097 entries, 0 hits, 48.6 M
misses; about 1 MiB), so every WRITE prunes the oldest entry of its
bucket and inserts one.  At 149k WRITEs a second an entry lives about
27 ms, so each is visited three times (insert, update, prune), each
time cold.  At round 12's 40 GB/s of 1 MiB WRITEs an entry lived
about 100 ms; at nfsd's own 262144 it would be 1.8 s.  None of these
reaches the 60 s of an NFS-over-TCP client's timeout.  The cache has
never hit on this rig, because the client retransmits nothing, and
how much protection 27 ms gives a flexfiles data server is not tested
here.

**What could be cut without changing what is cached** (estimates
from the profile, not measured):

- Reuse the pruned entry for the new one instead of a free and an
  allocation: about 0.6 to 1k.
- Keep a WRITE's reply (`ST+WC+4` words, under 256 bytes) inside the
  entry, with no `kmalloc` at update and no `kfree` at prune: about
  0.5k, at the same memory, since the kmalloc'd buffer is the same
  size.
- Drop the per-CPU `mem_usage` add and subtract on each call (four a
  WRITE) and compute the figure from the entry count when it is read:
  about 0.3 to 0.4k.

Together that is about 1.5 to 2k of the 5.5k, about 1% of nfsd's
cycles.  The cold walk, prune and checksum stay: they are what a
cache that turns over every 27 ms costs.  Neither changes the rate:
the drives hold it.

**Decided (Mike, 13 November): the DRC is not to be weakened because
this workload never hits it.**  What it caches, how long and how many
entries stay as they are; caching less (no WRITE, or a bound by time)
is off the table.  Only changes that keep its behaviour exactly, as
the three above, are candidates.

## The three DRC changes, as commits (13 November)

On Mike's word, with the inline buffer at 144 bytes, at the tip of
`kernel-7.3/canary` (after his rebase of that afternoon, which this
round's docs commits were cherry-picked back onto):

- `NFSD: Keep a short cached reply in its DRC entry` (`5d45831853ecb`):
  `c_inline[36]` in `struct nfsd_cacherep`, 136 bytes to 280; a reply
  of up to 144 bytes, a WRITE's (136 at most), is kept there.  An rtc
  attempt allocates its reply buffer at lookup only when its
  procedure's `pc_xdrressize` does not fit.
- `NFSD: Reuse a pruned DRC entry for the next call` (`ba4993950dab0`):
  the lookup searches with the call's key on the stack and, on a miss,
  takes the first entry the prune evicted; it allocates only when
  nothing was pruned, with the lock dropped and the bucket searched
  again.
- `NFSD: Count the DRC's entries apart from its reply buffers`
  (`2615638a3c798`): "mem usage" is the entries times their size plus
  the per-CPU counter, which now counts only reply buffers allocated
  apart.
- `nfsd: follow the DRC's inline reply and reused entry in the rtc
  KUnit` (`67322116a5f09`, KUnit tests).

Each built as `nfsd.ko` against build 531 (`build-nfsd.sh`, the rtc
suite linked in), no warning; checkpatch clean.  Not loaded, and the
KUnit suite not run (tardis2's KUnit crashes, `TESTING.md`).  To try
them, an `nfsd.ko` from the tip with `swap-sunrpc.sh NFSD_KO=`, which
drops every client's connections (Mike's to do), then round 100's
profile again: `nfsd_cache_lookup` and `nfsd_cache_update` against
their 4,331 and 662 cycles a WRITE in `t2-r602`.

## The DRC commits loaded (13 November, on Mike's word)

Build 531 was built from the rebased tree (its `nfsd.ko` carries the
READ path's `range_too_large`), and the tip's `fs/nfsd` differs from
`8efa530e9ab01`'s by the four DRC commits alone.  `nfsd.ko` built from
the tip as `build-nfsd.sh` builds it (`$L/nfsd-drc-531/`, srcversion
`603083CAF5F5B5E8DE898AC`), put in `updates/` with
`SUNRPC_KO=none NFSD_KO=... swap-sunrpc.sh` (no client connected),
then `settled.sh apply`: the settled setting.  An out-of-tree build's
srcversion differs from the kernel's own (`8efa530e9ab01` built the
same way reads `EF174CABCDAC245CD82D155`, the kernel's
`D95233FF10702BC0EC82B7B`), so only the diff says what changed.

The profile again with `run601.sh`: `RUNS="604:C 605:G 606:C"`, read
against `t2-r601` and `t2-r602`.

### Result: the DRC 1.3k cycles a WRITE cheaper, the rate the same

[run604.out](run604.out).  Every run clean: no ring drop, no loan
denied, no drive stepped down, no new kernel warning.

| run | kind | GB/s | WRITEs/s | nfsd cycles a WRITE |
|---|---|---|---|---|
| `t2-r601` (531's nfsd) | counted | 77.96 | 148,898 | 165,391 |
| `t2-r604` (DRC commits) | counted | 78.01 | 148,820 | 162,461 |
| `t2-r606` (DRC commits) | counted | 78.03 | 148,748 | 163,213 |
| `t2-r605` (DRC commits) | call graph | 78.12 | 149,045 | |

The DRC's own cycles a WRITE, `t2-r602` against `t2-r605`
([drc-r602.txt](drc-r602.txt), [drc-r605.txt](drc-r605.txt)):

| | `t2-r602` | `t2-r605` |
|---|---|---|
| `nfsd_cache_lookup` | 4,331 | 3,260 |
| ... the tree's search (was `nfsd_cache_insert`) | 1,400 | 1,406 |
| ... the prune | 953 | 608 |
| ... the free and allocation of an entry and reply | 982 | 189 |
| ... the call's checksum | about 640 | about 550 |
| ... the per-CPU counters | 162 | 117 (`rc_misses`) |
| `nfsd_cache_update` | 662 | 409 |
| the bucket lock, from `nfsd_dispatch` | 463 | 506 |
| **all** | **about 5,460** | **about 4,180** |

- **1.3k cycles a WRITE less, 24% of the DRC and 0.8% of nfsd's
  cycles**, below the 1.5 to 2k estimated: the free and allocation are
  gone as meant (982 to 189), the prune is cheaper (it no longer frees
  a reply buffer), and the per-call memory counters are gone (214 in
  `nfsd_cacherep_unlink_locked`, none now).  The search, the cold walk
  of entries no other call has touched for 27 ms, is unchanged, as
  expected.
- nfsd's cycles a WRITE fell 2.6k, 165.4k to 162.5k and 163.2k
  ([read65-r604.txt](read65-r604.txt),
  [symmib-r601-r604.txt](symmib-r601-r604.txt)).  The 1.3k beyond the
  DRC is within the spread of one baseline run against two.  Also,
  531's nfsd is the kernel's own build and this one is an out-of-tree
  build; an A/B against `8efa530e9ab01`'s `nfsd.ko` built the same way
  would separate the two.
- **The rate is unchanged, 78.0 to 78.1 GB/s**: the drives hold it.

`updates/nfsd.ko` (the DRC commits) is still loaded.  To go back:
`NFSD_KO=none SUNRPC_KO=none swap-sunrpc.sh`, which drops every
client's connections.

### The A/B with both nfsd.ko built the same way

To separate the DRC commits from the difference between the kernel's
own build and an out-of-tree one: `8efa530e9ab01`'s `fs/nfsd` built
as the DRC one was (`$L/nfsd-base-531/`, srcversion
`EF174CABCDAC245CD82D155`), loaded from `updates/`, three runs
(`t2-r607` counted, `r608` call graph, `r609` counted), then the DRC
`nfsd.ko` again for three more (`r610` counted, `r611` call graph,
`r612` counted): [ab607.sh](ab607.sh).  It ends on the DRC `nfsd.ko`.

### Result: about 1.0k cycles a WRITE, not 1.3k; the totals cannot see it

[ab607.out](ab607.out).  The first start stopped before any run:
root had under 2 GB free, filled by the `nfsd.ko` builds; their
objects were removed (the session's own build trees, nothing else)
and it ran from the start.  Every run clean: no ring drop, no loan
denied, no drive stepped down, no new kernel warning.

| run | `nfsd.ko` | kind | GB/s | nfsd cycles a WRITE |
|---|---|---|---|---|
| `t2-r601` | 531's own | counted | 77.96 | 165,391 |
| `t2-r607` | base, out of tree | counted | 77.91 | 162,182 |
| `t2-r609` | base, out of tree | counted | 78.10 | 164,299 |
| `t2-r604` | DRC commits | counted | 78.01 | 162,461 |
| `t2-r606` | DRC commits | counted | 78.03 | 163,213 |
| `t2-r610` | DRC commits | counted | 78.06 | 163,032 |
| `t2-r612` | DRC commits | counted | 78.02 | 162,952 |
| `t2-r608` | base, out of tree | call graph | 77.97 | |
| `t2-r611` | DRC commits | call graph | 78.07 | |

[read65-r607.txt](read65-r607.txt).  The base built alike averages
163.2k, the DRC commits 162.9k over four runs: two runs of the same
module differ by 2.1k, so the totals cannot resolve a 1k change.
Most of the 2.6k first seen against `t2-r601` was the one baseline
run and the kernel's own build, not the DRC.

The DRC's own cycles a WRITE, built alike
([drc-r608-r611.txt](drc-r608-r611.txt)), beside the first pair:

| | `t2-r602` (531's own) | `t2-r608` (base) | `t2-r611` (DRC) | `t2-r605` (DRC) |
|---|---|---|---|---|
| `nfsd_cache_lookup` | 4,331 | 4,225 | 3,268 | 3,260 |
| ... the tree's search | 1,400 | 1,308 | 1,346 | 1,406 |
| ... the prune | 953 | 970 | 574 | 608 |
| ... free and allocation | 982 | 915 | 203 | 189 |
| ... the call's checksum | about 640 | about 620 | about 630 | about 550 |
| `nfsd_cache_update` | 662 | 579 | 488 | 409 |
| **lookup and update** | **4,993** | **4,804** | **3,756** | **3,669** |

- **Like for like, the DRC commits save about 1.0 to 1.1k cycles a
  WRITE** (4,804 to 3,756 and 3,669), 22% of the DRC and 0.7% of
  nfsd: the free and allocation (915 to about 200) and the prune
  (970 to about 590) as before.  The first pair's 1.3k was about
  0.2k larger for the kernel's own build of the base.
- The search is unchanged, as expected.  `nfsd_cache_update`'s
  remaining cost is its list add (`__list_add_valid_or_report`, 181)
  and its own copy of the reply into the entry.
- **The rate is unchanged, 77.9 to 78.1 GB/s** on either module.

The DRC `nfsd.ko` is loaded from `updates/` again.

### Decision: the DRC commits are kept (Mike, 14 November)

"keep the DRC changes, they help, even if minimally."  The three code
commits (`NFSD: Keep a short cached reply in its DRC entry`, `NFSD:
Reuse a pruned DRC entry for the next call`, `NFSD: Count the DRC's
entries apart from its reply buffers`) and their KUnit change stay on
the branch.  tardis2 keeps running their `nfsd.ko` from `updates/`
(srcversion `603083CAF5F5B5E8DE898AC`) until a kernel built with them
is booted.

## What stands out after the DRC

From the DRC module's runs: counted `t2-r604`, `r606`, `r610`, `r612`
([top-drc.txt](top-drc.txt), 162.9k cycles a WRITE, IPC 0.94), and the
call graph `t2-r611` ([callers-r611.txt](callers-r611.txt),
[under-r611-send.txt](under-r611-send.txt)).  k cycles a WRITE:

| | k | what is in it | worked before |
|---|---|---|---|
| sleep and wake, interrupts hidden at irq enable | 16.0 | `finish_task_switch` (all self, 69% leaving idle by round 100's schedwhy), `wake_up_process` 3.0 | 17, 18, 40, 47, 61, 82 |
| the reply's transmit | 15.8 | `__svc_tcp_send`; 9.6 of it in `__dev_queue_xmit`, the qdisc lock 2.6 and `__dev_xmit_skb` 1.8 | 07, 31, 32, 57: closed |
| the loan's release | 8.6 | `svc_tcp_release_ctxt`: `rx_loan_table_release` 2.1, the pool's bulk return 1.7, the region's puts 1.0, irqs restored 1.6 | 45, 58, 86, 89, 90 |
| block submit | 12.3 | `submit_bio`; the SGL map `blk_rq_dma_map_iter_next` 2.6 at IPC 4.0 | 55, 62 |
| the loan's receive walk | about 15 | borrow 3.5, walk 1.7, append 1.7, prefetch 1.8, span 0.9 | 77 to 87 |
| the DRC | 4.2 | the cold search 1.3 | this round |

Every large block has been worked before.  Two of round 61's levers
have never been tried: the NVMe completions by polling instead of an
interrupt each (`nvme.poll_queues` with `RWF_HIPRI` for nfsd's direct
writes), and PSI off (`psi=0`, 3.2k of round 61's 276.8k), a host
setting.  Both need a boot.
