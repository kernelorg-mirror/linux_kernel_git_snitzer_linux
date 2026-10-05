# Scoping the region's budget to the region

Slipstream patch 18 ("SUNRPC: Region credit admission (TCP flow control
instead of ring drops)") limits how many pages loaned WRITEs may hold: the
budget. (The code calls what one WRITE draws against the budget its
credit.) As delivered the limit
applied to every connection on the host. This is the design that confines
it to the region it exists to protect, with Mike's decisions (2026-10-03)
and the measurements. It is implemented; `README.md` section 5 lists the
commits.

Words: a **region** is the fixed block of memory that named network devices
receive into; a **loan** hands a WRITE's receive pages to the file system
and holds them until the write is done; a **copy** moves the data into
nfsd's own buffer and gives the pages straight back; the **budget** is the
limit on region pages held by loans. `../transplant/README.md`, "Words used
here" and "Two kinds of storage", has the longer version and says which
storage wants loans and which wants copies.

## 1. Why the scope matters

The region (patch 16, "page_pool: Fixed receive-buffer region for one NIC;
recycle loaned pages") is one physically contiguous range of pages, bound to
one NIC's order-0 page pools, created once per boot and never freed. Its
purpose is the on-die receive copy sub-project (`../on-die-receive-copy/SPEC.md`):
a small landing area kept resident in on-die cache. The region is a finite,
small, shared resource whatever its final size.

That size is **not settled.** The on-die SPEC has not sized it: its landing
set target (G2, David's guess of 1–4 MB) is to be measured on tardis1
(phase 0a-R1 and the G1/G2 probes), and the on-die SRAM available may also
come from the vendor's specifications. Patch 16's message describes the
target platform's region as a 16 MiB slice of L3; that is the patch's
assumption, not a measured size. On tardis1's 64 KiB kernel 16 MiB would be
256 pages, much of it parked in the NIC's rings. The design below must hold
for any region size, including a much smaller one.

So the budget must answer for exactly one thing: pages of the region. Only
the NICs designated for it should draw on it — today the Xsight
high-performance NICs, but the mechanism names devices, not a vendor;
nothing else should be throttled by it or consume it, and its scope should
carry netns affinity (Mike, 2026-10-03).

## 2. What the delivered patch did

- One static `svc_rx_budget`: one budget, reserve, in-flight list and waiter
  FIFO for every svc TCP transport, in every netns and every `svc_serv`.
- `svc_tcp_rx_credit_admit()` runs for every record above
  `svc_tcp_rx_copy_below`, at its first body byte, before the record is
  classified for loaning; `svc_tcp_rx_credit_charge()` counts every page an
  skb may add (`svc_tcp_rx_skb_pages()`), region or not.
- `svc_tcp_rx_rcvbuf`'s locked buffer, window clamp and scaling ratio go on
  every TCP transport set up while it is set.
- A transport waiting for credit is re-queued by every arriving skb
  (`svc_data_ready()` sets `XPT_DATA` and enqueues), runs, and is refused
  again: `denied` runs at about 2.5–4 per admission.

Measured before the fix (`run-budget.sh` `PHASE=X`): a
client whose pages never come from the region is admitted and refused
against the region's budget, and falls from 70 to 19 MB/s alone on a
tight budget. When both run, the region client falls too (118 → 49 MB/s
at the phase A budget, 19 → 10 on the tight one); section 8 separates how
much of that is the shared budget.

## 3. Requirements

- **S1. Only region pages are credited.** Only traffic from the NICs
  designated for the region draws on its budget, and a record is admitted,
  charged and counted only for the region pages it may hold. The
  designation names devices (today `page_pool.rx_region_dev`, one device),
  never a vendor or driver. Traffic that holds no
  region page is never admitted, refused, paused or woken by the budget, and
  gets no locked buffer.
- **S2. No unbudgeted region pages.** A loan must not hold region pages
  without a budget to count them against, or the bound is gone.
  A budget of 0, which is the default, therefore means copy and never
  "no limit": unlimited loans from a fixed region can hold all of it in
  half-received WRITEs and stop the server
  (`../transplant/README.md`, "Problems found and fixed", 4).
- **S3. Netns affinity.** Which netns may draw on the region's budget is
  explicit (section 5, decision D1).
- **S4. Forward progress kept.** The oldest credited record still always
  finishes (reserve, then overrun), so a minimum-size region cannot
  deadlock (phase B).
- **S5. No re-queue storm.** A transport waiting for credit is run by the
  credit wake, not by every arriving skb.
- **S6. Nothing new for netdev to object to.** No new SUNRPC writes into
  TCP internals; a page_pool helper is acceptable if it is small and
  generic.

## 4. Design

### 4.1 Identify region pages (page_pool)

A pool bound to the region allocates only from it, so a page that such a
pool owns is a region page wherever it is (ring, skb, loan). The predicate
is an inline in `include/net/page_pool/helpers.h`:

```c
static inline bool page_pool_page_in_rx_region(struct page *page)
{
	return page_pool_page_is_pp(page) &&
	       netmem_get_pp(page_to_netmem(page))->rx_region;
}
```

(Reading the pool's binding needs no export, which matters because
page_pool is built into vmlinux.)

### 4.2 Credit only what holds region pages (S1, S2)

- `svc_tcp_rx_skb_pages()` gains a region-only mode: it counts only the
  region pages in the byte range (head and frags, through `frag_list`), so
  `charge()` asks for exactly the region pages an skb adds.
- Admission moves from "the record's first body byte" to "the first skb of
  the record that carries a region page". In the actor, a record that is
  not credit-linked and is about to consume an skb with region pages is
  admitted then; refusal stops reading there exactly as a refused admission
  does today. A record that never sees a region page is never admitted
  and never linked.
- The admission estimate stays the record's page estimate (`factor`); only
  when it is applied changes. Records already linked are charged as today,
  but only for region pages.
- S2 in the rare case of a record whose region pages arrive while credit
  cannot be granted and the record cannot pause (none today: every charge
  point can pause at an skb boundary): materialize, as the copy fallback
  already does, so no region page is held unbudgeted.

### 4.3 Locked buffer only where the budget applies (S1)

Apply `svc_tcp_rx_rcvbuf`, the clamp and the scaling ratio only to a
transport once it has received a region page, rather than to every
transport at setup. The actor sees the first region skb under the socket
lock, but `tcp_set_window_clamp()` is not exported and
`setsockopt(TCP_WINDOW_CLAMP)` takes the socket lock itself, so the actor
only marks the transport; `svc_tcp_recvfrom()` applies the buffer and clamp
after `read_sock` returns, the way `svc_tcp_init()` does today (no new TCP
internals, S6).

### 4.4 Netns affinity (S3)

The region is bound to a device, and the device to a netns. Two shapes;
D1 asks which:

- **N1 — one budget, owned by the region's netns.** Only transports whose
  `xpt_net` is the region device's netns (at region creation) draw on the
  budget. Region pages arriving in any other netns (forwarded through
  macvlan or a bridge) are not loaned: those records are copied, so they
  hold region pages only for the skb's lifetime (S2). Simple, and the
  budget is still one number for one region.
- **N2 — per-netns budgets that partition the region.** Budget state moves
  into `struct sunrpc_net`; each netns that serves region traffic sets its
  own budget, and setting it fails if the netns budgets would sum to more
  than the region. Fits several nfsd instances (containers) sharing one
  designated NIC; more tunables and more state.

N1 is the smaller change and matches "only the designated NICs draw"; N2
is the answer if Hammerspace runs several nfsd netns on one region.

### 4.5 No re-queue while waiting (S5)

`svc_tcp_recvfrom()` already leaves `XPT_DATA` clear when a record is
refused (re-arming it would spin the threads), but `svc_data_ready()` set
it and enqueued the transport on every arriving skb. Now
`svc_data_ready()` leaves a transport on the credit wait list alone: the
credit wake sets `XPT_DATA` and enqueues the head waiter when credit
returns, and `svc_xprt_received()` re-queues a transport the wake marked
while it was busy. The wait list is read without the credit lock
(`list_empty_careful()`); a stale answer costs one extra enqueue, or none
the wake does not make.

With arriving data no longer running a waiting transport, a budget change
has to: a pool applied a changed budget only at its own next admit, charge
or release, so a pool with waiters and nothing in flight would never
notice. Every write that can change a budget now applies it at once and
wakes the oldest waiter. Both are in "SUNRPC: Do not re-queue a transport
that waits for region credit".

Runtime A/B (`PHASE=X` before and after, same boot and region):
refusals halve in every with a budget cell, admissions and throughput are
unchanged, no drops.

| cell | refused, before → after | MB/s, before → after |
|---|---|---|
| on, R alone | 7039 → 3441 | 118 → 124 |
| on, N alone | 6760 → 3374 | 88 → 85 |
| on, R + N | 13855 → 6775 | 49 / 49 → 50 / 50 |
| tight, R alone | 10577 → 5575 | 19 → 19 |
| tight, N alone | 11282 → 5571 | 19 → 19 |
| tight, R + N | 22138 → 11151 | 10 / 10 → 10 / 10 |

The refusals left are about one per admission: the woken head waiter
finding the credit taken by a record admitted first. S5 does not touch the
scope problem (N is still admitted, refused and throttled; R still loses
half to N): that is S1–S3.

## 5. Decisions

Decided by Mike, 2026-10-03:

- **D1 — N2.** Per-netns budgets partitioning the region: budget state in
  `struct sunrpc_net`, each netns that serves region traffic sets its own
  budget and reserve, and the netns budgets together may not exceed the
  region. N1 falls out of it (one netns with a budget), and it fits the
  expected container use.
- **D3 — yes.** S5 (no re-queue while waiting for credit) lands first, as
  its own fix to 18.
- **D4 — a device list.** Several high-performance NICs (or ports) share
  one region and its budgets: the region binds a list of devices, not the
  one `rx_region_dev` names today.

- **D2 — (b), a default budget.** Region pages reaching a netns without
  a budget of its own are credited against a default budget, one more
  partition of the region under N2 (Mike, 2026-10-03). The question and
  every option considered are kept in full in 5.1, so that David (or a
  reviewer) who prefers another answer can argue for it from the same
  ground.

### 5.1 D2 in detail (decided: (b); options kept for review)

**When it happens.** With N2, a netns may receive region pages without
having a budget of its own:

- a macvlan or ipvlan device on top of a designated NIC is moved into a
  container's netns;
- traffic from a designated NIC is bridged or routed (veth, a Linux bridge,
  an XDP redirect) into another netns: forwarding keeps the received
  page-pool pages, so the skbs that reach the container still carry region
  pages;
- a netns has a budget of 0 (never configured, or turned off) while the
  designated NIC's traffic reaches it.

(A designated NIC moved wholesale into a container is not this case: its
netns is then the region's netns and has its budget. SR-IOV VFs are their
own devices, with their own pools, and hold region pages only if listed in
D4's device list.)

**What a transport holds of the region.** Three things, and the budget bounds
only the second:

1. **Unread data** in its TCP receive queue, up to its receive buffer:
   autotuned, it can grow to `tcp_rmem[2]` (MBs per connection), far more
   than a small region. Today `svc_tcp_rx_rcvbuf` is what bounds this, and
   phase B's minimum counts it per connection.
2. **Loaned pages**, held from read to the end of the RPC (or to 19's
   early release): bounded by the budget.
3. **Copied records** hold region pages only for the time the actor takes
   to copy them out of the skb.

So "copy instead of loan" removes 2 but not 1: an unbudgeted netns with
autotuned buffers can still fill the region with unread data, and the
budgeted netns then see ring drops. Any answer has to bound 1.

**Options.**

- **(a) Copy, with the locked buffer.** Records in an unbudgeted netns are
  never loaned; the transport gets the small locked buffer as soon as it
  receives a region page, so its unread data is bounded per connection.
  Nothing is admitted or refused. The region still has to have room for
  those connections' buffers (sizing counts them as phase B does), but
  nothing is accounted, so an administrator cannot see or cap the total
  except through the number of connections. A counter and a warn-once
  make the case visible.
- **(b) Draw on a default budget.** Region pages in a netns without its own
  budget are credited against one shared default budget (for example the
  initial netns's, or a separate "unassigned" budget that is part of the
  N2 partition). They are admitted, refused and bounded like any other,
  with the locked buffer. Every region page is accounted; unbudgeted
  containers share and compete for the default slice.
- **(c) Copy, and cap the netns as a whole.** As (a), plus a per-netns cap
  on the unread region pages its transports may hold (summing receive
  buffers), refusing to read past it. Bounded and accounted per netns,
  but a second mechanism beside the budget.
- **(d) Refuse service.** A transport that receives region pages in a netns
  without a budget is closed. Strict and simple; clients in such a netns
  fail.
- **(e) Loan without a budget** (today's behaviour). Unbounded: rejected by
  S2.

**Recommendation: (b), decided by Mike on 2026-10-03.** It keeps one
mechanism (the budget) for every region page, makes the case visible in the
same counters, and with N2 the default budget is simply one more partition
the administrator sizes. (a) is the fallback if a default budget is
unwanted: smaller, but it leaves part of the region unaccounted.

**Open details of (b)** (to settle in the implementation, not new
decisions): which netns owns the default budget (the initial netns, or a
standalone "unassigned" budget outside any netns); whether a default
budget of 0 means "copy and lock the buffer" (falling back to (a)) or
"refuse"; and how the default budget's counters are reported next to the
per-netns ones.

**Why copy rather than backpressure at a budget of 0** (Mike asked,
2026-10-03, whether pushing back on the network would do instead):

- Backpressure works by not reading: a transport refused credit leaves
  its data in the TCP receive queue and the window closes. Those queued
  skbs are region pages, so backpressure holds region pages in the
  socket queue for as long as the stall lasts (up to the transport's
  receive buffer, which step 3 bounds but does not release). Copying
  reads each skb, copies the payload out and returns its region pages
  at once: for a namespace with no share of the region it keeps the
  region footprint smallest, where stalling makes it largest.
- With a budget of 0 nothing ever returns credit, so a transport that
  stops reading never resumes. Forward progress would need a floor (one
  record at a time, say), which is a small implicit budget the
  partition sum does not count: it undoes the sum check.
- Backpressure is still available, by the budget's value: a small
  budget (its reserve alone, as in phase B) admits records one at a
  time and holds the rest with the window closed, every page accounted.
  So the budget is the dial: 0 copies, a small budget gives
  backpressure, a larger one loans more.

## 6. Test plan

- `PHASE=X` is the A/B. Pass: the non-region client behaves the same at
  every budget (no admissions, no refusals, no locked buffer), and the
  region client matches its alone numbers when both run.
- Phases A and B rerun unchanged (region-only traffic): same verdicts, and
  `denied` per admission falls with S5.
- KUnit: region-only page counting (head, frags, `frag_list`, mixed);
  admission at the first region skb; no credit for a record without region
  pages; affinity refusal; no enqueue while waiting.
- A netns cell for N1/N2: a region client in a second netns (macvlan on
  `rxsrv`), expected to copy under N1 or draw its own budget under N2.

## 7. Risks

- Admission later in the record: a record may already hold non-region
  pages when it is refused. Bounded by the transport's receive buffer, and
  it is what a pause does today.
- The region size is open (section 1). Nothing here depends on it, but
  the phase B minimum (parked + receive buffers + reserve + slack) does,
  and on a small region the reserve alone may be most of it; the tardis1
  sizing work should report parked pages and the reserve together.
- 4.3 changes when the locked buffer appears on a transport (first region
  skb, not setup): a few segments arrive before the window is held. The
  clamp is re-applied on every receive today, so this is the same
  mechanism started later.

## 8. Steps 1 and 2: results (2026-10-03)

"SUNRPC: Credit only the region pages a TCP receive loan holds":
`charge()` counts only region pages (head, frags, `frag_list`); a loaned
record is admitted at its first skb carrying one.

`PHASE=X` with them (`/var/tmp/credit-hs451-X-s1/`; S5 included in both
columns' code):

| cell | R MB/s before → after | N MB/s before → after | admitted / refused |
|---|---|---|---|
| on, R alone | 124 → 117 | — | 2785 / 3441 → 2785 / 3404 |
| on, N alone | — | 85 → 91 | 2785 / 3374 → **0 / 0** |
| on, R + N | 50 → 49 | 50 → **71** | 5570 / 6775 → **2785** / 3318 |
| tight, R alone | 19 → 19 | — | 2785 / 5575 → 2785 / 5572 |
| tight, N alone | — | 19 → **92** | 2785 / 5571 → **0 / 0** |
| tight, R + N | 10 → **16** | 10 → **87** | 5570 / 11151 → **2785** / 5577 |

- N never touches the budget: no admission, no refusal, its throughput no
  longer depends on the region's budget (91–92 MB/s at every setting).
- R is credited exactly as before when alone.
- When both run, R gains on the tight budget (10 → 16) now that N no
  longer draws on it; the rest of R's loss (16 against 19 alone, 49
  against 117 at `on`) is not the budget, since N no longer touches it:
  the two clients share the nfsd threads and the one export.
- The locked receive buffer still goes on every transport, N's included
  (step 3 scopes it).

Note for testing modules on a running kernel: the page_pool commits change
vmlinux, and a module build relinks vmlinux, so its BTF no longer matches
the running kernel and sunrpc fails to load ("failed to validate module
[sunrpc] BTF: -22"). Boot a kernel built from the same tree.

### 8.1 Device list (D4): `PHASE=D` (2026-10-03)

Kernel: `7.1.13-19.hs.451.el9.aarch64+` built from tip `0a6ae668f06d`
(build #31, with the device list), installed modules = tree build (sunrpc
`9C10CC39D7A6391F47D75E3`). Fresh boot; the warm-up created the region
with `rx_region_dev=rxsrv,rxsrv2` (debugfs: `dev rxsrv,rxsrv2`), 4096
pages, 310 parked. `rxsrv2` gets XDP too, so its pools are created after
the region and bind it: N is a second region client. Every write
verified. `/var/tmp/credit-hs451-D/`.

| cell | N MB/s | R + N MB/s | peak pages | alloc_fail | admitted / refused |
|---|---|---|---|---|---|
| on (2698), N alone | 120 | — | 2232 | 0 | 2785 / 3435 |
| on, R + N | — | 59 / 59 | 2933 | 0 | 5570 / 6842 |
| tight (390), N alone | 19 | — | 912 | 0 | 2785 / 5571 |
| tight, R + N | — | 10 / 10 | 1661 | 0 | 5570 / 11152 |

- `rxsrv2`'s pools draw from the shared region (in `PHASE=X`, unlisted, N
  never left the 64 idle pages).
- N is credited exactly like the first device's client
  (compare `x-on-R` 117 MB/s, 2785 / 3404), and two devices drawing on one
  budget stay inside parked + budget (2933 ≤ 310 + 2698) with no
  allocation failures; the two clients share it evenly.
- The ring-drops column reads `rxsrv` only; `alloc_fail` is the region's
  count across both devices.

### 8.2 Step 3: the locked buffer only on region transports (2026-10-03)

"SUNRPC: Hold the receive buffer only on transports that receive region
pages" (`Fixes:` 18): the actor marks a transport at its first skb with a
region page, and `svc_tcp_recvfrom()` applies `svc_tcp_rx_rcvbuf`'s
buffer, clamp and ratio after `read_sock()` returns (section 4.3);
`svc_tcp_init()` no longer applies it. No KUnit case: the hold needs a
real TCP socket; the runtime cell checks it with `ss -tm` mid-run
(`PHASE=X` now saves `ss.txt` per cell).

`PHASE=X` (`/var/tmp/credit-hs451-X-s3/`, sunrpc `7ADDB04C2A8F37831AA00B2`
on the tip-built hs.451; the region is bound to `rxsrv,rxsrv2` this boot,
but `rxsrv2` has no XDP in `PHASE=X`, so N stays non-region):

| cell | R MB/s, steps 1–2 → 3 | N MB/s, steps 1–2 → 3 | N server sockets |
|---|---|---|---|
| on, alone | 117 → 119 | 91 → 69 | `rb131072` → autotuned, 2–9 MB |
| on, R + N | 49 → 48 | 71 → 77 | |
| tight, alone | 19 → 19 | 92 → 69 | autotuned |
| tight, R + N | 16 → 15 | 87 → 67 | |

- R's 16 sockets are held at `rb131072` from the first region skb, and
  R's numbers are unchanged.
- N's sockets autotune; N now behaves the same at every budget (69–77
  MB/s, never admitted); its retransmits fall (on, alone: 345 → 92).
- N's 91–92 MB/s before was the locked buffer it should never have had.

### 8.3 Step 4: a budget per network namespace, and the default budget (2026-10-03)

What was built:

- "page_pool: Export page_pool_rx_region_size()": the region's pages once
  it exists, else the configured size; the budgets are checked against it.
- "SUNRPC: Give each network namespace its own region budget": one
  budget per network namespace (in `struct sunrpc_net`) plus one default
  budget. A transport draws on the budget it waits on, else its
  namespace's if that has (or is draining) one, else the default; a record
  keeps the budget it was admitted to. The initial namespace's budget is
  `svc_tcp_rx_budget_pages` / `_reserve`; the default's is
  `svc_tcp_rx_budget_default_pages` / `_default_reserve`; every
  namespace's, the initial one included, is
  `net.sunrpc.region_budget_pages` / `_reserve`, with
  `net.sunrpc.region_budget_stats` to read it. A write that would make the
  budgets together exceed the region fails with `ENOSPC`.
- "SUNRPC: Copy region records when the budget drawn on is 0": a transport
  whose budget is 0 has its region records copied at their first region skb
  (trace reason `no_budget`), so they hold region pages only while being
  copied; its receive buffer is held as in step 3. This applies whether or
  not any other budget is set.

**Choices (Mike, 2026-10-03), with the alternatives not taken:**

| question | chosen | alternative recorded |
|---|---|---|
| where a netns's budget is set | per-netns sysctls `net.sunrpc.region_budget_*` (provisional, see below) | a `/proc/net/rpc/region_budget` file; sunrpc or nfsd netlink |
| who owns the default budget | a standalone default budget, one more partition of the region | the initial netns's budget doubling as the default |
| a default budget of 0 | copy the region records and hold the buffer (option (a) of 5.1) | refuse loans to such transports |
| budgets larger than the region | refuse the write (`ENOSPC`), checked against `page_pool_rx_region_size()` | accept and let the region run short |

**The interface is an open upstream question.** New `/proc` files are
discouraged; netlink (sunrpc or nfsd) is awkward for something this tied
to netdev, though it might be acceptable if it speaks only in sunrpc
terms; per-netns sysctls are the netdev norm for per-namespace knobs
(`net.ipv4.*`) and were chosen for now, kept thin so they can be replaced
when review settles it (Mike: "something we'll need upstream review to
settle").

**Checked on the rebuilt kernel (2026-10-03):**

- Sysctls, with `rx_region_pages=4096` and no region yet: the initial
  namespace's `net.sunrpc.region_budget_pages` and
  `svc_tcp_rx_budget_pages` are the same value; a new namespace gets its
  own sysctls; 3000 (initial) + 1000 (namespace) is accepted, raising the
  namespace to 2000 is refused with `ENOSPC`; the default budget counts in
  the same sum; deleting the namespace frees its budget without a warning.
- `PHASE=D` and `PHASE=X` match their step-3 runs cell for cell.

A second nfsd in its own server-side namespace drawing on its own budget
is step 5, below.

### 8.4 Step 5: a second nfsd in its own namespace (2026-10-03)

`run-budget.sh` `PHASE=N`: an nfsd in network namespace `zcsrvB` with its
own mount namespace (`ip netns exec` + `unshare -m`: tmpfs `/run` and
`/var/lib/nfs`, its own `/etc/exports` and `/etc/exports.d`, its own nfsd
fs; NFSv4.2 only, grace ended at once), serving the same export to client
namespace `rxcl3` over veth `rxsrv3` (XDP, in the region's device list).
A UBI9 container was considered (Mike's suggestion) but not needed: the
namespaces give the same isolation with the host's nfs-utils. Kernel
`7.1.13-19.hs.451` #33, modules = tip (sunrpc `A9C3F1EBC54FB424C3AE4FB`),
region 4096 pages. `/var/tmp/credit-hs451-N3/`; every write verified.

| cell | budgets init / default / B | B MB/s | admitted against | `no_budget` copies |
|---|---|---|---|---|
| n-own | 0 / 0 / 2000 | 149 | **B: 2786** (init 0, default 0) | 0 |
| n-default | 0 / 2000 / 0 | 146 | **default: 2785** (init 0, B 0) | 0 |
| n-copy | 1000 / 0 / 0 | 126 | none | **5570**; region peak 449 pages against ~1690 loaned |
| n-both | 1600 / 0 / 1600, plus the initial namespace's client | 115 (init client 115) | **init: 2785 and B: 2786**, separately | 0 |
| sum check | init 2000, then B 2500, region 4096 | — | refused: "No space left on device", B unchanged | — |

- Each namespace's records are admitted against exactly the budget the
  design says (its own, else the default), and two namespaces under load
  at once keep separate accounts.
- At 0 with a budget in use elsewhere, B's region records are copied
  (reason `no_budget`), and the region holds far fewer pages.
- B's server sockets are held at `rb131072` whenever its records carry
  region pages (step 3).

**Two harness hazards found, both rpcbind reached by path rather than by
namespace** (`/run/rpcbind.sock`, shared with the host unless the mount
namespace has its own `/run`); neither is a fault of the series:

- An nfsd in another network namespace registers in the host's rpcbind,
  and on shutdown unregisters NFS from it: the first runs left the
  host's nfs-server with no NFS, mountd or nlockmgr registration, and v3
  clients of the host failed ("requested NFS version or transport
  protocol is not supported"). Fixed by the private `/run`; NFSv4.2 does
  not need rpcbind.
- A v3 mount in a client namespace starts lockd there, which registers
  nlockmgr in the host's rpcbind and on unmount unregisters it, the host
  server's entry included (confirmed: 6 entries before, 0 after; with
  `nolock`, 6 and 6). Every v3 cell (phases A, X, D, and David's
  harness) has done this; the harness's client mounts now use `nolock`.
  After such a run, `systemctl restart nfs-server` restores the
  registrations.


### 8.5 Changing a budget under load, and a namespace going away (2026-10-03)

Two more `PHASE=N` cells:

- **n-reconf**: B's budget 2000 (default 1500), then under load B=400 at
  6 s, B=0 at 12 s, B=2000 at 18 s.
- **n-exit**: B under load on a soft mount; at 5 s B's nfsd is stopped
  and its namespace deleted.

What they check:

- **A budget change reaches waiting transports.** Nothing else runs a
  transport that is waiting for credit (4.5), so every change is a wake
  event.
- **A transport whose budget goes to 0 stops waiting.** It is woken, finds
  no budget and copies; it must also leave the wait list and wake the next
  one, or everything behind it is stranded.
- **A namespace can go away under load** without leaking region pages or
  credit.

Result on 4 KiB (`/var/tmp/credit-hs451-N7/`), every write verified:

| cell | budgets init / default / B | B MB/s | fio rc | admitted against | copied (`no_budget`) |
|---|---|---|---|---|---|
| n-own | 0 / 0 / 2000 | 147 | 0 | B: 2785 | 0 |
| n-default | 0 / 2000 / 0 | 150 | 0 | default: 2785 | 0 |
| n-copy | 1000 / 0 / 0 | 116 | 0 | none | 5570 (region peak 445) |
| n-both | 1600 / 0 / 1600 | 113 (init 112) | 0 | init 2785, B 2785 | 0 |
| n-reconf | 0 / 1500 / 2000, then B 400, 0, 2000 | 144 | 0 | B 5937, default 2401 | 30 |
| n-exit | 0 / 0 / 2000, B removed at 5 s | 88 | 4 (soft mount, expected) | — | 0 |

n-reconf's budget after each write: 400 with records in flight keeping what
they hold and 10 waiters; 0 with the waiters draining; 2000 with no waiters
and nothing in flight. Both budgets ended idle and whole, and the sum check
refused 2000 + 2500 over 4096 (`ENOSPC`). n-exit: region free 4096 of 4096,
0 escaped. No warning in any cell.

n-reconf writes 768 MiB per job, not 1024: four jobs of 1024 MiB fill the
4096 MiB test export and fio then fails with `ENOSPC` for a reason that has
nothing to do with the budget.

### 8.6 Qualification on 4 KiB (2026-10-03)

Kernel `7.1.13-19.hs.451` #38, modules = the tip (sunrpc
`AE90AC1AAA0C7B7C61E4BFC`, installed = loaded = tree).

- **KUnit, all four suites:** svcsock 38 pass + 1 skip of 39 (the skip is
  `arena_bound`, by design on 4K), xdr 7/7, nfsd receive 6/6, nfsd4
  receive 10/10.
- **checkpatch `--strict`** on every new SUNRPC commit: 0 errors, 0
  warnings; the only CHECKs are the `kunit_add_action_or_reset(`
  continuation alignment the suite already uses.
- **`PHASE=N` (N7):** every cell passes (§8.5).
- **`PHASE=X` (X3, `/var/tmp/credit-hs451-X3/`):** all nine cells match
  step 4's run (X-s4) on admissions and drops: the non-region client is
  never admitted, and there are no drops with a budget set. Throughput
  too, but for x-tight-RN's non-region client, 62
  MB/s against 78; neither fix touches a transport that never waits, so
  this is read as run-to-run variation.
- **`PHASE=D` (D4, `/var/tmp/credit-hs451-D4/`, fresh boot of #38, same
  modules):** the cells with a budget match step 4's run (D-s4):

  | cell | D-s4 | D4 |
  |---|---|---|
  | d-on-N | 121, admitted 2785 | 115, admitted 2785, 0 alloc_fail |
  | d-on-RN | 58 / 58, admitted 5570 | 59 / 59, admitted 5570, 0 alloc_fail |
  | d-tight-N | 19, admitted 2785 | 19, admitted 2785 |
  | d-tight-RN | 10 / 10, admitted 5570 | 10 / 10, admitted 5570 |
- **Not re-run since the branch was tidied (2026-10-04):** the per-commit
  bisect walk and sparse.

## 9. 16 KiB (2026-10-05)

The whole plan, these phases included, was run on a 16 KiB host:
`../transplant/README.md`, "16 KiB host". Two things there bear on this
design:

- **A budget of 0 has to mean copy even when no budget is set anywhere.**
  The first form of the rule copied only while some other budget was set,
  and a region with no budget at all hung a server. S2 as worded above is
  the rule now.
- **Which storage the harness models.** The results in this document are
  from nvme-loop, which needs its data in whole pages, so its loaned WRITEs
  are written buffered: they show the budget working, not loans avoiding a
  copy. `run-budget.sh STORAGE=sgl` now runs the same phases on NVMe/TCP,
  where loans are written direct; the 16 KiB run did both.
