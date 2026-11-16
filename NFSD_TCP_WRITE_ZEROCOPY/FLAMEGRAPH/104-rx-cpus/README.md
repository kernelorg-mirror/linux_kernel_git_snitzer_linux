# Round 104: what the receive rings' CPUs do on reads

Round 103 settled `xps_rxqs` on tardis2, and two-port reads on all
eight drives went from 54 to 60.5 GB/s. On those reads all eight
receive-ring CPUs (0, 2, 4, 6 on port a; 56, 58, 60, 62 on port b)
were 81 to 93% busy, all of it softirq, while the transmit rings'
CPUs were 11 to 22% and the drives 85 to 87%. The frames a read
server receives are the client's READ calls and ACKs, a small
fraction of the bytes, so the question is what that softirq time is.

## The runs

`run645.sh`, on build 531 at the settled setting (xps_rxqs, xeu
Release 17, the DRC `nfsd.ko` in `updates/`): t2-r645 reads, then
t2-r646 writes for the same profile with data coming in, eight
drives on two ports, `WARM=10 RUNTIME=40`. In each run's window it
records the sixteen ring CPUs with call graphs for 8 s at 1999 Hz,
and reads both ports' per-ring counters and `/proc/softirqs` on
either side of it, so packets and softirqs a second can be set
beside the profile.

## t2-r645 and r646: profiled before the load, not in it

[run645.out](run645.out): t2-r645 read 60.51 GB/s (30.21 / 30.30),
ring drops 249, the receive CPUs 79 to 94% busy; t2-r646 wrote 78.09
GB/s (39.05 / 39.04), the receive CPUs 29 to 54%. Both at the settled
xps_rxqs; no drive stepped down, no new kernel warning.

**The profiles are not of the load.** The runner started `perf` 75 s
after twoport.sh, as round 103's queue trace did, and the load began
later: in t2-r645 the profile ran from 1794684305.7 to 318.4 and the
load's first second was 342.8. It caught twoport.sh's flow placement:
a short write job, with the placement's `bpftrace` on
`net:napi_gro_receive_entry` running on the receive CPUs (its program,
`trace_call_bpf`, the nofault copies and their faults are 10% of them).
Round 103's queue traces had the same fault (its README is corrected).
The runner now waits for the load's 25th second (`wire-sec.tsv`'s
rows) and traces each connection's transmit queue after the profile.

## t2-r647: in the load

[run647.out](run647.out): 60.54 GB/s (30.22 / 30.32), ring drops 131,
no drive stepped down, no new kernel warning. The profile ran from
24.4 to 36.8 s into the load. The receive CPUs were 71 to 90% busy
(CPU 0 90%), the transmit CPUs 30 to 38% (above round 103's 11 to 22%:
this time the profile and the queue trace were running in the window).

**The sends spread evenly under xps_rxqs** ([txq-sk-r647.txt](txq-sk-r647.txt),
2 s of each port in the load): every queue 25% of its port's sends on
both ports, and every connection on exactly one queue, three a queue.

**The receive CPUs mostly free the READ replies' pages**
([perf-rx-self-r647.txt](perf-rx-self-r647.txt),
[perf-rx-callers-r647.txt](perf-rx-callers-r647.txt), the eight
receive CPUs' samples):

| | share |
|---|---|
| `_raw_spin_unlock_irqrestore`, 36.0 of its 39.1% under `free_frozen_page_commit`, from `skb_release_data` in `tcp_clean_rtx_queue` | 39.1% |
| `skb_release_data` itself (the frags' refcount drops) | 20.8% |
| `free_frozen_page_commit`, `__free_frozen_pages` | 5.6% |
| `tcp_clean_rtx_queue`, `kfree`, `kmem_cache_free`, `rb_erase`, `kfree_skbmem` | 7.6% |
| xeu's receive poll and GRO | under 3% |

The ACKs a client sends free the sent skbs (`tcp_ack()` ->
`tcp_clean_rtx_queue()`), and with nfsd's `io_cache_read` at 2
(direct) the skbs' frags hold the last reference to pages nfsd
allocated for the READ, not page-cache pages, so every page sent goes
back to the page allocator on the receive CPU that took the ACK. At
60.5 GB/s that is about 3.7 million 16 KiB pages a second, allocated on
nfsd's CPUs and freed on the eight receive CPUs. Without pseudo-NMI a
sample taken with interrupts off is placed where they come back on, so
the 36% at `_raw_spin_unlock_irqrestore` is the time spent inside the
free's locked section: the per-CPU list is full (high 889, batch 15)
and is drained to the zone under `zone->lock`, and tardis2 has one
node and one Normal zone, so that lock is the one all 64 CPUs take,
nfsd's allocations included. Whether the time is waiting on that lock
or doing the drain is not yet measured.

## t2-r650: the zone lock

`PROBE=lock RUNS=650:read run645.sh`: in the load, `perf lock
contention -a -b` for 4 s (who waits on which lock, and how long), then
3 s of `free_pcppages_bulk()` and `rmqueue_bulk()` on the ring CPUs and
on nfsd's: the pages each moves and the time each takes.

[run650.out](run650.out): 60.45 GB/s (30.18 / 30.27), ring drops 427,
no drive stepped down, no new kernel warning; the probes ran from 24.4
to 37.4 s into the load. (Its queue trace is missing: the bulk probe's
leftover map took the time it would have run in.)

**The zone lock is contended from both sides**
([lock-contention-r650.txt](lock-contention-r650.txt), 4 s, all CPUs):

| caller | contended | total wait | avg wait | per second of the 4 |
|---|---|---|---|---|
| `rmqueue_bulk` (refilling a per-CPU list: nfsd's allocations) | 614,347 | 26.29 s | 42.8 us | 6.6 CPUs waiting |
| `free_pcppages_bulk` (draining one: the receive CPUs' frees) | 614,233 | 21.91 s | 35.7 us | 5.5 CPUs waiting |

The drains are on the receive CPUs: in 3 s
([zone-bulk-r650.txt](zone-bulk-r650.txt)) they gave the zone 9.6
million pages and took back 1,365, while nfsd's CPUs took 8.7 million
and gave none (3.2 and 2.9 million a second, beside the 3.7 million
the rate implies, with the probes slowing it). 5.5 CPUs of waiting
over eight receive CPUs is about 68% of each, which with the drains'
own work is the 71 to 90% they are busy. nfsd's CPUs wait about as
long again in their refills. That is with `perf lock contention`'s
BPF probes on every contended lock, which add to the time a holder
holds it, so the shares are an upper bound; the order is not in
doubt: no other spinlock comes within a factor of twenty. (The
probe's per-call times for the drains are not usable: a softirq's
drain is keyed by the task it interrupted.)

So each page of a READ crosses the zone lock twice, out to nfsd's CPU
in a refill and back from the receive CPU in a drain, because the CPU
that frees it never allocates and the one that allocates never frees:
neither per-CPU list can reuse a page, and with one zone for 64 CPUs
every crossing takes the same lock, about 1.2 million acquisitions a
second at 15 pages a batch.

Also there, but not CPU: nfsd threads waited 6.4 min in 4 s on
`__svc_tcp_send`'s transport mutex (about 95 threads at a time, 56 ms
each on average), queued to send on their connection while another
thread's reply is being sent. That is the READ latency (70 ms mean)
rather than a ceiling on the rate.

## What would take the zone lock out (not tried)

1. **Bigger per-CPU lists, `vm.percpu_pagelist_high_fraction`** (0
   now: high 889 pages, batch 15). A sysctl, at once and reversible:
   a fraction f gives each CPU a high of about 8.2 million / f / 64
   pages, so the drains move more pages a lock. It holds more free
   pages on each CPU (f 64: about 2,000 pages, 31 MiB a CPU, 2 GiB in
   all; these are not the receive region's pages and nothing touches
   them while they wait). It cannot stop the crossing, only batch it.
2. **Fewer, larger allocations for a READ's pages** (code, in SUNRPC
   and nfsd): an order-3 folio (128 KiB at 16 KiB pages, still cached
   on the per-CPU lists) for each eight pages of a direct read's
   buffer would take the crossings and the refcount drops in
   `skb_release_data` down by eight.
3. **Giving the pages back to nfsd instead of the zone** (code): a
   reply's skbs carry a completion (as `MSG_ZEROCOPY`'s `ubuf_info`
   does) that returns its pages to the nfsd thread's or pool's own
   list, so no page crosses the zone at all. The largest change.
4. **`io_cache_read` other than direct**: buffered reads send page-cache
   pages, which a free does not return, but the page cache's own fill
   and reclaim allocate and free as many pages, so this is not
   expected to help; not measured.

Each would be an A/B on two-port reads with the WRITE pass beside it
(AGENTS.md): (1) touches the page allocator every WRITE uses, and (2)
and (3) SUNRPC's send path.

## The A/B of `vm.percpu_pagelist_high_fraction` on reads

`PROBE=none RUNS="651:read:0 652:read:8 653:read:0 654:read:8"
run645.sh`, then the WRITE pass at 8 (`write-check.sh 655`), the
sysctl put back to 0 at the end. At 0, the default, each CPU's list
tunes its own high between `high_min` 874 and `high_max` 16,043 pages
(the kernel raises it on a CPU that frees much and lets it fall
back); a fraction f fixes both at about 8.2 million / f / 64, so 64
would hold it at 2,005, under what the default may reach, and 8 (the
least the sysctl takes) holds it at 16,043 (250 MiB a CPU, 16 GiB if
every CPU's list were full; none of it the receive region's). So the
A/B is 0 against 8, and each run records every CPU's high in the load
(`/proc/zoneinfo` either side of the window), which says what the
default reached. Nothing is profiled (`PROBE=none`), so the busy
figures are the load's own. `settled.sh` does not hold the sysctl.

[run651.out](run651.out), [wcheck655.out](wcheck655.out):

| run | fraction | GB/s | a / b | receive CPUs' high in the load | receive CPUs busy | transmit CPUs busy |
|---|---|---|---|---|---|---|
| `t2-r651` | 0 | **60.63** | 30.31 / 30.32 | 874 to 1,188 | 77 to 92% | 15 to 24% |
| `t2-r652` | 8 | 58.52 | 29.16 / 29.35 | 16,043 | 89 to 96% | 5 to 16% |
| `t2-r653` | 0 | 58.59 | 29.15 / 29.43 | 7,176 to 13,122 | 90 to 95% | 5 to 15% |
| `t2-r654` | 8 | 58.49 | 29.00 / 29.49 | 16,043 | 87 to 95% | 5 to 14% |
| `t2-r655` | 8 | write 77.97 | 38.91 / 39.05 | | | |

No drive stepped down and no new kernel warning in any; ring drops 278
to 406. nfsd's CPUs were at 16,043 in every run, the default too.

- **Fixing the high at 16,043 lowers reads by 2.1 GB/s**, 58.5 against
  60.6, with the receive CPUs busier (to 96%).
- **The default's second run read the same as 8**, because the
  receive CPUs' highs did not come back down: setting 0 again leaves
  each CPU's high where it is, and the default's own tuning let them
  rise to 7,000 to 13,000 in r653 (after the window they were 7,008 to
  12,927, and after the WRITE pass 7,815 to 15,693 at rest). In r651,
  before anything was changed, they stayed at 874. So the rate followed
  the receive CPUs' high, not the sysctl: low, 60.6; high, 58.5.
  Why a longer list on a CPU that only frees costs more (longer drains
  under the zone lock, or the list's pages and struct pages out of
  cache) was not measured.
- **The WRITE pass at 8: 77.97 GB/s, RERUN.** 8 is not to be kept, so
  it was not run again.

`t2-r656` holds the fraction at 147 (high fixed at 873, about the
default's floor) to see whether the 60.6 comes back with the highs
held low, and `t2-r657` reads at 0 straight after it.

[run656.out](run656.out):

| run | fraction | GB/s | a / b | receive CPUs' high | nfsd CPUs' high | receive CPUs busy |
|---|---|---|---|---|---|---|
| `t2-r656` | 147 | 57.42 | 28.50 / 28.92 | 873 | 873 | 71 to 86% |
| `t2-r657` | 0 | 60.27 | 30.06 / 30.22 | 874 to 2,510, CPU 0 9,589 | 14,533 to 16,043 | 85 to 92% |

No drive stepped down, no new kernel warning; ring drops 41 and 303.

- **Low everywhere is worse still: 57.4 GB/s.** The fraction fixes
  every CPU's high, nfsd's too, and nfsd's CPUs, which only allocate,
  then refill from the zone 18 times as often; the receive CPUs were
  less busy (71 to 86%), so the time moved to nfsd's refills.
- **The default from that low state is back at 60.3**, with most
  receive CPUs' highs at 874 again and nfsd's at 14,533 to 16,043.
- So the best mix is a low high on the CPUs that only free and a high
  one on the CPUs that only allocate, which the default reaches when
  nothing has raised the receive CPUs' highs, and which no value of
  the sysctl gives, since it sets every CPU alike. **The sysctl stays
  at 0**; the way out of the zone lock is in code (2 or 3 above).
- **The host:** the fraction is 0 and the receive CPUs' highs are back
  at 874 but for CPU 0 (7,994) and 60 (3,585) at rest, about as
  before the A/B (r651's CPU 60 was 999 to 1,188). The runs after this
  start from that.

## The A/B of nfsd's `rtc_read` on reads

The run-to-completion READ fast path (`2372a16b60b3c`) serves an
NFSv3 READ on a work item on the CPU of the connection's latest
receive, a receive ring's CPU, with an rqst from a preallocated pool.
Its pages would then be allocated and freed on the same CPU, where its
per-CPU list could reuse them, instead of crossing the zone lock; but
it also moves nfsd's READ work onto the receive CPUs, already 71 to
94% busy. It was measured only on WRITEs (build 512: the hop of every
receive to a work item cost about 55k nfsd cycles a loaned WRITE), and
`settled.sh` holds `rtc_read` at N. `settled.sh` now takes
`EXP_RTC_READ`, which holds apply and the audit to another value and
marks the run off the setting.

`PROBE=none RUNS="658:read::N 659:read::Y 660:read::N 661:read::Y"
run645.sh`, recording `rtc_stats` either side of the window (how many
READs were offered and run to completion, and why the rest fell back),
then the WRITE pass at Y (`EXP_RTC_READ=Y write-check.sh 662`), and
`settled.sh apply` back to N at the end.

[run658.out](run658.out), [wcheck662.out](wcheck662.out):

| run | `rtc_read` | GB/s | a / b | p99 ms | ring drops | receive CPUs busy | transmit CPUs busy | nfsd CPUs busy |
|---|---|---|---|---|---|---|---|---|
| `t2-r658` | N | 60.61 | 30.20 / 30.41 | 79 to 80 | 214 | 78 to 93% | 10 to 24% | 80% |
| `t2-r659` | **Y** | **63.41** | 31.66 / 31.75 | 87 to 95 | 0 | 31 to 78% | 14 to 97% | 24% |
| `t2-r660` | N | 60.38 | 30.12 / 30.26 | 78 to 79 | 329 | 86 to 94% | 10 to 19% | 77% |
| `t2-r661` | **Y** | **63.16** | 30.15 / 33.00 | 115 to 124 | 19 | 34 to 79% | 15 to 100% | 23% |
| `t2-r662` | Y | write 77.95 | 39.01 / 38.94 | | 0 | | | 25% |

No drive stepped down and no new kernel warning in any.

- **Reads rise 2.8 GB/s, 4.6%: 63.2 to 63.4 against 60.4 to 60.6.**
  Every READ in the window ran to completion (`read_offered` 485,277
  and 483,562, `fast_completed` as many, 6 and 12 `budget_exhausted`,
  no fallback counted), and nfsd's CPUs went from 77 to 80% busy to 23
  to 24%: the READs' work left nfsd's threads.
- **It moved to the transmit CPUs, not the receive ones**, and
  unevenly: four of the eight at 87 to 100% in each Y run (r659: 3,
  57, 59, 61; r661: 1, 5, 59, 61), the others at 14 to 29%, while the
  receive CPUs dropped to 31 to 79%. Those four are the ones whose
  per-CPU page lists were at the full 16,043 in the load, the
  allocating side, so the work items run where the reply's sends
  complete, not where its call was received; why, and why four, was
  not looked into. With four CPUs at 100% the next ceiling is there,
  and the uneven spread is likely why the p99 went up (87 to 124 ms
  against 78 to 80) and port b outran a in r661.
- **The WRITE pass at Y: 77.95 GB/s, RERUN** (nfsd 25% busy, as at
  N). Run again as `t2-r663`.

[wcheck663.out](wcheck663.out): **77.89 GB/s, the second under 78.0,
so by AGENTS.md's WRITE pass `rtc_read=Y` regresses WRITEs** (the
drives 98% busy, no drive stepped down). The N passes on this boot
read 78.07 and 78.10 (`t2-r648`, `r649`) and the 40 s run 78.09
(`t2-r646`), so Y sits about 0.15 GB/s lower. It is not kept: the
setting stays N unless Mike decides otherwise, or the hop that every
WRITE's receive takes with Y on is taken out of the WRITE path.

## The A/B of `svc_tcp_rx_region_reads` on reads

`PROBE=none RUNS="664:read:::N 665:read:::Y 666:read:::N 667:read:::Y"
run663.sh`, then the WRITE pass with it on (`write-check.sh 668` with
the parameter written Y before), the parameter back to N at the end.
`rtc_read` stays N. With it on, a direct READ's payload pages come
from the receive region, are sent zero-copy under a completion, and go
back to the region when TCP frees the reply, so they never reach the
page allocator. The region is not split (`rx_region_read_pages` 0:
splitting needs the region made again, the ports down), so READs draw
on the same 32,768 pages as the receive rings. Each run records the
READ budget's counters (admitted, parked, granted, unconfined, short)
and the region's pages in use at most, which is held against the rule
not to grow the region's footprint. `settled.sh` does not hold this
parameter.

[run664.out](run664.out), [wcheck668.out](wcheck668.out):

| run | `region_reads` | GB/s | a / b | p99 ms | ring drops | receive CPUs busy | nfsd CPUs busy | region pages in use at most |
|---|---|---|---|---|---|---|---|---|
| `t2-r664` | N | 60.53 | 30.32 / 30.21 | 79 to 80 | 345 | 81 to 92% | 80% | 1,786 |
| `t2-r665` | **Y** | 52.02 | 26.23 / 25.79 | 95 to 96 | 0 | 16 to 18% | 72% | **4,924** |
| `t2-r666` | N | 60.38 | 30.06 / 30.32 | 78 to 79 | 436 | 86 to 95% | 78% | 1,767 |
| `t2-r667` | **Y** | 55.45 | 28.18 / 27.27 | 92 to 95 | 0 | 17 to 19% | 71% | **4,948** |
| `t2-r668` | Y | write **78.16, PASS** | 39.08 / 39.08 | | 0 | | | |

No drive stepped down and no new kernel warning in any.

- **With READs confined, the receive CPUs drop from 81 to 95% busy to
  16 to 19%**: the frees to the page allocator are gone, as expected.
- **But reads fall 5 to 8.5 GB/s, to 52.0 and 55.4**, at a higher p99,
  and the region's peak in use nearly triples (1,767 to 1,786 against
  4,924 to 4,948 pages), against the rule not to grow the region's
  footprint. WRITEs are untouched (78.16 with it on).
- **The READ counters in the runner read 0 because they read the wrong
  budget**: with the region unsplit a confined READ draws on its
  connection's receive budget (`svc_tcp_rx_budget_read_for()`), here the
  initial namespace's at 8,198 pages, not the read pool. That budget's
  counters (`svc_tcp_rx_budget_stats`), since the boot, so over r665 and
  r667: 2,533,496 READs admitted and completed, **none parked for
  credit and none unconfined, and 2,381,055 `share_stalls`**, almost one
  for each READ. So credit is not the limit; the connection's share is
  (`svc_tcp_rx_region_conn_share`, 0: auto, two maximum READs, 130
  pages, a connection), which holds each connection to two confined
  READs at a time while the client keeps about 20 in flight on each.
  That is read from the counters and the code, not tried.
- **Set aside (Mike, 15 November): "lets ignore use of
  svc_tcp_rx_region_conn_share for now, seems to have excessive use of
  the region."** Confinement already holds 4,924 to 4,948 region pages
  at the default share, against 1,767 to 1,786 without it, and a share
  large enough for the client's READs in flight would come near the
  whole region. No share A/B is run; `svc_tcp_rx_region_reads` stays N.
  (The share is David Flynn's, from "sunrpc: READ region-confinement
  hooks", 27 October, with its later rx_loan commits.)

## rtc_read without the WRITEs' hop (`278b74ad8a0d9`)

Mike: with `rtc_read=Y`, WRITEs should not use the work item. The hop
comes from `svc_rtc_should_divert()`, which diverts every receive while
either direction is on (`nfsd3_rtc_want()`), before the call is known;
the work item's peek then hands a call it does not serve (a WRITE with
`rtc_write` N) to a thread unreceived, counted as `fallback_not_fast`.
`rtc_write=Y` is the same work item serving the WRITE itself; the hop
should follow the switch of the call's own direction, as its admission
(`nfsd3_rtc_want_call()`) already does.

`SUNRPC: Send calls the rtc hook does not serve straight to a thread`
marks a connection when the peek finds a call the hook does not serve
and sends a marked connection's receives to a thread with no hop
(counted `undiverted` in `rtc_stats`); a thread that receives a call
the hook would serve clears the mark. Built as `sunrpc.ko` against
build 531 (`build-sunrpc.sh`, `$L/sunrpc-rtcw-531/`; the tree's
`net/sunrpc` and `include` are `8efa530e9ab01`'s, build 531's, but for
this commit; every export's version matches the installed module's).
Put in place on Mike's word (14 November: "Swap now") with
`swap-sunrpc.sh` (no client connected; the DRC `nfsd.ko` stays in
`updates/`), then `settled.sh apply`. Then `PROBE=none RUNS="669:read::N
670:read::Y 671:read::N 672:read::Y" run645.sh` and the WRITE pass at Y
(`EXP_RTC_READ=Y write-check.sh 673`, again if it reads RERUN), with
`undiverted` and `fallback_not_fast` read from `rtc_stats`.

On the new `sunrpc.ko` (srcversion `BE117461F8916452F175925`, in
`updates/` with the DRC `nfsd.ko`; swapped 03:44 with no client
connected, then the settled setting): [run669.out](run669.out),
[wcheck673.out](wcheck673.out):

| run | `rtc_read` | GB/s | a / b | p99 ms | ring drops | busiest ring CPUs | nfsd CPUs busy |
|---|---|---|---|---|---|---|---|
| `t2-r669` | N | 60.60 | 30.20 / 30.40 | 80 | 292 | receive, 79 to 93% | 79% |
| `t2-r670` | **Y** | **63.91** | 32.09 / 31.81 | 87 to 96 | 0 | CPU 0 93%, 61 97% | 29% |
| `t2-r671` | N | 60.52 | 30.23 / 30.28 | 79 to 80 | 246 | receive, 80 to 93% | 79% |
| `t2-r672` | **Y** | **63.71** | 32.13 / 31.59 | 93 to 96 | 75 | 7 97%, 57 91%, 61 92% | 26% |
| `t2-r673` | Y | write **78.11, PASS** | 39.13 / 38.98 | 80 to 81 | 0 | | 23% |

No drive stepped down and no new kernel warning in any.

- **The WRITE pass at `rtc_read=Y` passes**, 78.11 GB/s at the first
  run, against 77.95 and 77.89 on the old `sunrpc.ko`.
- **WRITEs no longer take the work item**: over the four read runs
  (each with its placement write job) and the pass, `rtc_stats` counted
  35,094,398 receives sent straight to a thread (`undiverted`) and 162
  WRITEs peeked and handed back (`fallback_not_fast`), 69 of them by the
  end of the first Y run; `fallback_no_peek` 262. Build 512's
  `rtc_read=Y` had 51.2 million `fallback_no_peek` and 11.9 million
  `fallback_not_fast`.
- **Reads keep the gain, a little larger: 63.7 to 63.9 GB/s against
  60.5 to 60.6**, 5.3%, every READ in the window run to completion
  (491,807 offered in r670's). The p99 is still higher at Y (87 to 96
  ms against 79 to 80), and the work items still crowd a few CPUs (91
  to 97%) while others idle: the next thing to look at under `rtc_read`.

`settled.sh` still holds `rtc_read` at N, and the host is at the
settled setting with the new `sunrpc.ko` in `updates/` (going back is
`SUNRPC_KO=none swap-sunrpc.sh`). Settling `rtc_read=Y`, which needs
this `sunrpc.ko` (or a kernel with `278b74ad8a0d9`), is Mike's to
decide.

**Settled (Mike, 15 November): "make rtc_read=Y the settled setting".**
`settled.sh` holds `rtc_read` at Y on tardis2 (N elsewhere) and, with
it Y, audits that sunrpc sends WRITEs past the work item (`undiverted`
in `rtc_stats`), so a sunrpc without `278b74ad8a0d9` audits BAD.
`settled.sh apply` at rest: the settled setting. Its WRITE pass is
`t2-r673`, 78.11 GB/s.

## Where the rtc work items run

At `rtc_read=Y` the work items crowd a few CPUs: three or four of the
ring CPUs at 91 to 100% in each Y run while the rest of the ring CPUs
sit at 15 to 75% and nfsd's at 23 to 29%. `svc_rtc_place()` gives each
connection one work CPU, hashed over its node's CPUs that have not taken
a receive in the softirq (`svc_rtc_pick_cpu()`), and every pass of its
work item runs there (`svc_rtc` is a per-CPU workqueue). `PROBE=rtc
RUNS=675:read run645.sh` at the settled setting: 3 s of bpftrace on
`svc_rtc_work()` (runs and time by CPU, each connection's chosen CPU)
and `svc_rtc_place()` (each placement's receive CPU, softirq or not, and
the CPU chosen), then 5 s of every CPU sampled by task and symbol.

[run675.out](run675.out), [rtc-place-r675.txt](rtc-place-r675.txt):
64.40 GB/s, ring drops 0, no drive stepped down, no new kernel
warning. In 3 s of the load the work items ran on 19 CPUs, each pass on
its connection's chosen CPU (`conn->cpu` was the running CPU in every
sample):

| connections placed on the CPU | CPUs | busy |
|---|---|---|
| 1 | 11, 14, 21, 29, 38, 41, 52, 54 (nfsd's); 7, 57 (transmit) | 70 to 78% (7: 97%, 57: 98%) |
| 2 | 12, 17, 55 (nfsd's) | 96 to 98% |
| 3 | 19 (nfsd's), 5 (transmit) | 98 to 100% |

The other 29 of nfsd's 48 CPUs were 0 to 2% busy. Two things crowd the
work items:

- **The transmit CPUs are placed on.** `svc_rtc_pick_cpu()` leaves out
  only CPUs that have taken a receive in the softirq (here the eight
  receive CPUs, the only softirq placements counted), so the transmit
  CPUs, which take each queue's completions, are candidates like any
  other: CPUs 5, 7 and 57 got five of the 24 connections and ran at 97 to
  100%.
- **The hash collides.** Each connection is hashed to one of 56 CPUs on
  its own, so 24 connections land on 19 CPUs, and one connection's work
  item alone takes 70 to 78% of a CPU: two or three on one CPU saturate
  it.

Mike: "use nfsd's CPUs for the work items". `SUNRPC: Let the rtc work
items be confined to a set of CPUs` adds `sunrpc.svc_rtc_cpus`, the
CPUs a connection's work item may be placed on (empty: as before);
`settled.sh` is to hold it at nfsd's 8-55. That answers the first; the
second needs placement that counts what each CPU already holds.

Built against build 531 (`$L/sunrpc-rtccpus-531/`, with
`278b74ad8a0d9` under it; every export's version as the installed
module's). `settled.sh` holds `svc_rtc_cpus` at nfsd's CPUs (8-55) on
tardis2 and audits a sunrpc without it BAD; `EXP_RTC_CPUS` (and the
runner's fifth field, `none` for empty) holds a run to another list.
Put in place on Mike's word (15 November: "Swap now, mask only"), then
`PROBE=none RUNS="676:read:::none 677:read:::8-55 678:read:::none
679:read:::8-55" run645.sh` at `rtc_read=Y` (settled), `PROBE=rtc
RUNS=680:read run645.sh` at the setting for the placement, and the
WRITE pass (`write-check.sh 681`).

On the `sunrpc.ko` with `2e2811d21f8c4` (srcversion
`8722BACFAF3A69EC2A110A6`; swapped 04:41 with no client connected, then
the settled setting): [run676.out](run676.out), [run680.out](run680.out),
[wcheck681.out](wcheck681.out), [rtc-place-r680.txt](rtc-place-r680.txt):

| run | `svc_rtc_cpus` | GB/s | a / b | p99 ms | busiest CPUs | transmit CPUs busy |
|---|---|---|---|---|---|---|
| `t2-r676` | empty | 64.96 | 34.08 / 30.88 | 105 to 128 | 57, 53, 46, 10 at 100% | 18 to 100% |
| `t2-r677` | 8-55 | 63.66 | 31.62 / 32.04 | 81 | 53 97%, 14 96%, 43 93% | 12 to 27% |
| `t2-r678` | empty | 63.93 | 32.27 / 31.66 | 125 to 132 | 59, 1 at 100%, 15, 12 97% | 21 to 100% |
| `t2-r679` | 8-55 | 64.00 | 32.46 / 31.53 | 88 to 90 | 48 99%, 52, 51, 44 98% | 12 to 29% |
| `t2-r680` | 8-55, traced | 64.31 | 31.42 / 32.89 | 92 to 93 | 55, 39 100%, 51, 41 99% | 9 to 36% |
| `t2-r681` | 8-55 | write **78.05, PASS** | | | | |

No drive stepped down and no new kernel warning in any.

- **The mask keeps the work items off the ring CPUs**: the transmit
  CPUs drop from up to 100% to 9 to 36%, and every connection's work
  item in r680's trace runs on one of nfsd's CPUs.
- **The rate does not move**: 63.7 to 64.3 GB/s with it, 63.9 to 65.0
  without; nor does the spread of the two ports' share.
- **The p99 comes down**, 81 to 93 ms against 105 to 132 at the empty
  default, toward the 79 to 80 of `rtc_read=N`.
- **The collisions are still there.** In r680, 24 connections on 19 of
  nfsd's CPUs: the five holding two (39, 41, 51, 52, 55) at 98 to 100%,
  the 14 holding one at 59 to 76%, the other 29 idle. With a connection's
  work item taking 60 to 78% of a CPU, a pair saturates one, so the next
  change is placement that takes the eligible CPU holding the fewest
  connections rather than a hash.
- **The WRITE pass at the setting: 78.05 GB/s, PASS.**

## Least-loaded placement (`svc_rtc_balance`, `fe25e474fa3de`)

Mike, 15 November: "write the least-loaded placement".  `t2-r680`
showed the hash leaving five of nfsd's CPUs with two connections each
(98 to 100% busy) and 29 with none.

"SUNRPC: Optionally place each rtc work item on the least-loaded CPU"
keeps a per-CPU count of the connections whose work item is placed
there (raised and lowered as `svc_rtc_place()` moves a connection,
lowered when its transport is freed) and, with `sunrpc.svc_rtc_balance`
Y, places a connection on the eligible CPU holding the fewest, the scan
starting at the connection's hash so that ties spread.  `svc_rtc_cpus`
still bounds it.  A connection keeps its CPU once placed, as before;
two placements that race may still pick one CPU.  The default stays
the hash.  The counts are on a new `placed` line of `rtc_stats`.

Built for build 531 as `$L/sunrpc-rtcbal-531/` (srcversion
`B1AE4F3F0262A3E5AD387FA`).  Not loaded: the swap is Mike's to allow.
`settled.sh` holds `svc_rtc_balance` at N (`EXP_RTC_BALANCE` for a
run), and `run645.sh` gains a sixth field, `rb`.

The plan, after the swap: reads at `svc_rtc_cpus` 8-55 with balance N
then Y, twice each (`RUNS="682:read::::N 683:read::::Y 684:read::::N
685:read::::Y"`), then `PROBE=rtc` at Y to see the placement, and the
WRITE pass at Y.  Y is kept only if reads gain and the pass holds 78.

## Result: balance spreads the connections and reads less

[run682.out](run682.out), sunrpc `B1AE4F3F0262A3E5AD387FA` swapped in
at 15:06 (Mike: "Swap and run"), `svc_rtc_cpus` 8-55 throughout.  No
drive stepped down, no new kernel warning; the host at the settled
setting after.  Connections a CPU from `rtc_stats`' `placed` line in
the load.

| run | balance | GB/s | p99 ms (a / b) | ring drops | connections a CPU |
|---|---|---|---|---|---|
| `t2-r682` | N | **65.72** | 100 / 105 | 0 | 17 CPUs: three on 21, 37 and 48, two on 26 |
| `t2-r683` | Y | 63.55 | 73 / 72 | 100 | 24 CPUs, one each |
| `t2-r684` | N | **64.35** | 115 / 114 | 9 | 19 CPUs: four on 17, two on 18 and 50 |
| `t2-r685` | Y | 63.28 | 79 / 73 | 47 | 24 CPUs, one each |
| `t2-r686` | Y, traced | 63.17 | 75 / 74 | 43 | 24 CPUs, one each |

- **Balance places as written**: one connection a CPU in every Y run,
  all inside 8-55.
- **It reads 1 to 2 GB/s less**, 63.2 to 63.6 against 64.4 and 65.7,
  with the two ports even and the p99 down to 72 to 79 ms.  The hash
  runs read more with three and four connections on one CPU, so the
  work items' CPU is not what limits reads; the zone lock the receive
  CPUs drain to (r647, r650 above) is still the lead.
- No WRITE pass: Y is not kept.  `settled.sh` holds N; the code stays
  in the tree at its default, the hash.

Mike, 15 November: "so far balance seems like a dead end".

## The A/B of RFS on reads (Mike, 15 November: "run the RFS A/B on reads")

Each READ's pages are allocated on the connection's work-item CPU
(rtc_read Y: the pool rqst's fill and the send are both there) and
freed on the receive-ring CPU that takes the client's ACK, so neither
CPU's per-CPU list can reuse a page and every page crosses the zone
lock twice (r650).  TCP records the CPU a connection last sent from
(`inet_send_prepare()`), and with Receive Flow Steering the
connection's frames are processed there after the ring CPU's GRO: the
ACKs, and with them the frees, would land on the CPU that allocated
the pages.

`settled.sh` gains `EXP_RFS=<flow table entries>`: every receive
queue's `rps_cpus` at nfsd's CPUs (8-55), `rps_flow_cnt` an eighth of
the entries, and `net.core.rps_sock_flow_entries`; the setting stays
off.  `run645.sh` takes it as a seventh field, and with `RESTART=1`
restarts nfs-server before each run and after the last: the rtc
placement marks every CPU that takes a softirq receive and never
clears the mark, and under RFS nfsd's own CPUs take them, so each run
starts from an empty mark.

RFS also moves the WRITE payload's receive processing from the ring
CPUs onto nfsd's, so a setting kept would need the WRITE pass as much
as anything in this round.

The plan: `RESTART=1 PROBE=none RUNS="687:read:::::0
688:read:::::32768 689:read:::::0 690:read:::::32768"`, then
`PROBE=lock` at 32768 (r691) for the zone lock, then `write-check.sh`
at 32768 if reads gain.

## Result: RFS piles the work items onto two transmit CPUs; stopped

[run687.out](run687.out).  No drive stepped down, no new kernel
warning.

| run | RFS | GB/s | p99 ms | work items a CPU (`placed`) |
|---|---|---|---|---|
| `t2-r687` | off | 64.16 | 99 to 105 | 20 of nfsd's CPUs, at most three |
| `t2-r688` | 32768 | **38.27** | 148 | **13 on CPU 3, 7 on CPU 63**, one each on 13, 35, 42, 46 |
| `t2-r689` | off | 64.11 | 81 (a) | (stopped before its snapshot was summed) |

Under RFS nfsd's CPUs take softirq receives, so `svc_rtc_place()`
marked them one by one as receive CPUs; with all of 8-55 marked no
CPU of `svc_rtc_cpus` was eligible and placement fell back to the
node's unmarked CPUs, the transmit rings' (which take no receives).
So r688 measures that pile-up, not whether frees on the allocating
CPU take the zone lock out.

Mike, 15 November: "ok, dead end.. stop this testing at next stop".
The batch was stopped during `t2-r689` (off): its loop killed,
twoport.sh left to finish its run, then nfs-server restarted and
`settled.sh apply` (RFS off).  r690 (on) and r691 (the lock probe) did
not run, and no WRITE pass was run: RFS is not kept.  `EXP_RFS` stays
in `settled.sh` for a run.

## Freeing a sent skb on the cpu that built it (Mike: "write the TCP change")

Mike, 15 November: "write the TCP change to free sent skbs on the
allocating CPU", then "I'll build a new kernel".

- **`ddb07c31c1b39` "net: add skb_attempt_defer_free_tx()"**: the
  queueing of `skb_attempt_defer_free()` becomes a helper that takes
  its limit, and `skb_attempt_defer_free_tx()` releases a sent skb's
  head state and queues it to `skb->alloc_cpu` on the same per-cpu
  lists, bounded by a new `net.core.skb_tx_defer_max` (0, the default:
  freed at once).  It is apart from `skb_defer_max`, which tardis2
  holds at 0 for WRITEs (round 03), and zerocopy skbs are never
  deferred, as before.
- **`7bd353fb130ef` "tcp: free acknowledged skbs on the cpu that built
  them"**: `tcp_rtx_queue_unlink_and_free()` uncharges the socket and
  hands the skb to it, so `tcp_clean_rtx_queue()` on a receive CPU
  sends each READ reply's skbs, and their pages, back to the CPU that
  sent them: the rtc work item's, which allocates the next READ's
  pages there.  The list is flushed there in `NET_RX` softirq, raised
  by an IPI when it is half full.

TCP and `net/core` are in the kernel image, so this needs a new
kernel and a boot; the objects were compiled against build 531's
headers (`skbuff.c`, `sysctl_net_core.c`, `tcp_input.c`), no error.
`settled.sh` holds `skb_tx_defer_max` at 0 (`EXP_SKB_TX_DEFER_MAX` for
a run; a kernel without it audits BAD only when a run asks for it),
and `run645.sh` gains an eighth field, `txd`.

The plan, on the new kernel: two-port reads at 0 and 128 in turns, two
each (`RUNS="n:read:::::::0 n+1:read:::::::128 ..."`), then
`PROBE=lock` at 128 for the zone lock's refills and drains beside
r650's, then the WRITE pass at 128 (the change frees WRITE replies'
skbs on nfsd's CPUs too).  128 is kept only if reads gain and the pass
holds 78.

## Build 534: `skb_tx_defer_max` 0 against 128 (15 November)

On build 534 (the gitlab build of the tip, with `ddb07c31c1b39` and
`7bd353fb130ef`), at the settled setting (`rtc_read` Y,
`svc_rtc_cpus` 8-55).  At 0 the kernel is at build 531's rates: the
WRITE pass `t2-r707` 78.06 GB/s (`r706` 77.89, RERUN), reads `r708`
63.48 and `r709` 64.20 against 531's `r696` 64.16 and `r697` 63.91.
Then reads in turns ([run710.out](run710.out)), and `PROBE=lock` at
128 ([run714.out](run714.out)):

| run | `skb_tx_defer_max` | GB/s | ring CPUs busy (busiest) | CPUs 8-55 busy, mean | drives, busiest |
|---|---|---|---|---|---|
| `t2-r710` | 0 | 64.11 | 73 to 81% | 29% | 93% |
| `t2-r711` | 128 | 60.52 | 5 to 36% | 43% | 82% |
| `t2-r712` | 0 | 63.46 (47 ring drops) | 82 to 92% | 30% | 93% |
| `t2-r713` | 128 | 60.33 | 5 to 38% | 40% | 81% |
| `t2-r714` | 128, lock probe | 60.34 | 4 to 37% | 41% | 82% |

**128 does what it was written to do and loses 3.4 GB/s of reads.**
The zone lock is no longer contended
([lock-contention-r714.txt](lock-contention-r714.txt)): `rmqueue_bulk`
3,652 contended, 27 ms of waiting in 4 s, and `free_pcppages_bulk` 579,
5 ms, against r650's 614,000 a side and 26 and 22 s.  In 3 s
([zone-bulk-r714.txt](zone-bulk-r714.txt)) the ring CPUs gave the zone
143k pages (r650: 9.6 million) and nfsd's took 918k (8.7 million): the
pages now come back to the per-CPU lists of the CPUs that send, as
meant.  But the freeing went with them, to CPUs 8-55 (their mean busy
from 29 to 41%), and the drives are asked for less (busiest 93 to 81%),
so the limit is now ahead of the drives, on the sending side.  Not yet
shown: why.  The probe at 128 has long waits on `__svc_tcp_send`'s
transport mutex (8,639 contended, 2.77 min of waiting in 4 s,
`svc_rtc_drain_sends` 10.5 s more), but r650 is not its comparison
(build 531, `rtc_read` N); the guess, that the deferred frees' softirq
runs on a sending CPU while its send holds the mutex, is untested.

Not kept: `settled.sh` stays at 0.  The WRITE pass at 128 was not run,
128 having failed on reads.  To find the cause: the same lock probe at
0 on build 534, and a profile of CPUs 8-55 at 0 and 128 (Mike's to
ask for).

## Why 128 loses: the rtc's connections fill and READs fall back to nfsd

Mike: "run the lock probe at 0 and profile CPUs 8-55".  `run645.sh`
gains `PROBE=nfsd` (8 s of CPUs 8-55 with call graphs, reports for
them).  [run715.out](run715.out): `t2-r715` the lock probe at 0
(62.59 GB/s), `t2-r716` CPUs 8-55 at 0 (63.05), `t2-r717` at 128
(60.16, CPUs 8-55 49% busy against 31%).

**At 0 the transport mutex is not contended**
([lock-contention-r715.txt](lock-contention-r715.txt)): the zone lock
has 628,906 / 628,931 contended (24.2 and 22.0 s of waiting in 4 s,
r650's picture on this kernel), and no mutex comes near the list.  At
128 (r714) the zone lock is quiet and `__svc_tcp_send`'s mutex has
2.77 min of waiting.  So 128 trades the one for the other.

**What makes the mutex contended is READs the rtc gives back to
nfsd's threads.**  In the profile window
([rtc-stats-window-r716.txt](rtc-stats-window-r716.txt),
[rtc-stats-window-r717.txt](rtc-stats-window-r717.txt)):

| | at 0 (r716, 12.2 s) | at 128 (r717, 11.3 s) |
|---|---|---|
| READs diverted to the rtc | 733,466 | 648,768 |
| completed and sent by the rtc | 733,476 | 530,371 |
| `fallback_conn_full` (to nfsd's threads) | 0 | **118,068 (18%)** |
| `budget_exhausted` | 4 | 20,330 |
| send batches | 441,912 | 188,600 |

At 0 the rtc sends every READ; at 128 18% find the connection's rtc
queue full and go to an nfsd thread, which sends through
`svc_send()` -> `__svc_tcp_send()` against the rtc's own
`svc_rtc_drain_sends()` on the same transport mutex (`nfsd` is 14% of
CPUs 8-55's samples at 128 and not in the list at 0;
[perf-nfsd-comm-r716.txt](perf-nfsd-comm-r716.txt),
[perf-nfsd-comm-r717.txt](perf-nfsd-comm-r717.txt)).

**Why the rtc falls behind: the frees land on its CPUs.**  At 128,
`skb_defer_free_flush()` in `NET_RX` is 15.4% of CPUs 8-55
([perf-nfsd-children-r717.txt](perf-nfsd-children-r717.txt)), with
`skb_release_data()` 10.0% self (putting the pages); most of it is
taken from idle (the IPI wakes an idle CPU), but it is the rtc's CPUs
it runs on.  And the pages the rtc then allocates come off long
per-CPU lists, cold: `__list_del_entry_valid_or_report()` under
`__rmqueue_pcplist()` from `svc_fill_pages()` is 6.6% self at 128,
under 1.5% at 0, where the cost was the zone lock's instead
(`_raw_spin_unlock_irqrestore` 15.7%,
[perf-nfsd-self-r716.txt](perf-nfsd-self-r716.txt),
[perf-nfsd-self-r717.txt](perf-nfsd-self-r717.txt)).  The rtc's work
runs take longer, its connections fill, and the overflow costs more
than the zone lock did.

**Decided (Mike, 15 November): leave it disabled.**  Build 534 stays
tardis2's default kernel, `skb_tx_defer_max` at 0 (`settled.sh`
holds it); at 0 it is at build 531's rates, so no reboot to 531.

## The transport mutex at 128: head-of-line blocking, and two fixes

Mike: "write the probe for the head-of-line blocking first, feel free
to then try 1 and 2", then "try 1 and 2 in conjunction with the
feedback your traces and analysis provide".

**The probe**, [hol.bt](hol.bt) (`run645.sh`'s `PROBE=hol`, 5 s): the
rtc's wait for a connection's `xpt_mutex` (its `mutex_lock()` of that
mutex to the hold's first `svc_tcp_sendto_locked()`), split by whether
an nfsd combiner (`svc_tcp_combine_sends()`, which only nfsd senders
run) was sending on the connection; the rtc's hold; each nfsd
`__svc_tcp_send()`, split by whether the rtc held the mutex; and
whether a waiting nfsd sender was woken or timed out.  `t2-r718` and
`r719` ran without it (a relative path after `run645.sh`'s `cd`), and
`r720`, `r721` with a first version whose rtc side did not work
(`svc_rtc_drain_sends()` is inlined into `svc_rtc_work()`, and the
kretprobes on `__svc_tcp_send()` ran out of instances with hundreds of
threads asleep in it); from `r722` on it uses fentry/fexit.
(`comb_us` is not usable: it is keyed by connection, and the
combiner's fentry and fexit do not pair on it.)

**At 0 there is none** ([hol-r722.txt](hol-r722.txt), 63.02 GB/s): the
rtc's wait is 1 to 4 us, never behind nfsd; a hold is 281 us for 1.6
replies.  **At 128 there is, rare but long**
([hol-r723.txt](hol-r723.txt), 60.48): 2% of the rtc's holds begin
behind an nfsd combiner, and those wait 5.9 ms on average, most of
them 8 to 16 ms (11.7 s of waiting in 5 s, about two of the 24
connections' work items stalled at a time); nfsd's sends take 15.6 ms
on average, and in `r721` a quarter of nfsd's waits timed out (7,592
of 30,974) and took the mutex themselves.  The rtc's own hold grows to
921 us for 2.7 replies: each reply takes about twice as long to send.

**The two fixes**, [rtc-sender-fixes.diff](rtc-sender-fixes.diff)
(against `05b95f7da2983`'s tree; built as
`/var/tmp/zc-run-logs/sunrpc-hol-534/`, sunrpc
`A1A8B4C0AFA6B304A6E40D2`, in build 534's `updates/` from 22:24), each
behind a sunrpc parameter that is N by default:

1. `svc_rtc_conn_full_wait`: a connection with `SVC_RTC_CONN_PARKED`
   (32) calls parked keeps its next call for its work item (XPT_BUSY
   and `SVC_RTC_RECV` kept) instead of handing it to a thread; a pass
   that releases one below the ceiling kicks the receive.
2. `svc_rtc_single_sender`: an nfsd thread's reply on a connection
   with a work item is queued and the work item kicked to send it
   (`svc_tcp_xpo_send_unlock()` combines up to
   `SVC_TCP_COMBINE_BUDGET` of them and kicks itself again for the
   rest); the thread waits 100 ms before it takes the mutex itself.
   Both its wait and the re-kick were set from r721's timeouts.

[run724.out](run724.out), [run728.out](run728.out),
[wcheck729.out](wcheck729.out), `hol-r724.txt` to `hol-r728.txt`:

| run | `skb_tx_defer_max` | fix 1 | fix 2 | GB/s | p99 ms | to threads (`conn_full`) | rtc behind nfsd | rtc hold |
|---|---|---|---|---|---|---|---|---|
| `t2-r724` | 128 | N | N | 60.19 | 118 | 75,411 | 2,437, 6.4 ms | 934 us |
| `t2-r725` | 128 | Y | N | 60.69 | 80 | 0 | 0 | 930 us |
| `t2-r726` | 128 | N | Y | 60.48 | 100 | 78,140 | 1 | 912 us |
| `t2-r727` | 128 | Y | Y | 60.72 | 80 | 0 | 0 | 896 us |
| `t2-r728` | 0 | Y | Y | 62.60 | | 0 | 0 | 321 us |
| `t2-r729` | 0 | Y | Y | write **78.12, PASS** | | | | |

No drive stepped down and no new kernel warning in any.

**The fixes take the blocking out and give 0.5 GB/s back of the 3.4.**
With fix 1 no READ goes to a thread and nothing waits on the mutex;
with fix 2 the rtc is behind a thread once in 5 s and no thread times
out.  p99 falls from 118 to 80 ms.  But the rate stays at 60.5 to 60.7
GB/s, because the rtc's hold stays at about 900 us (the 281 us of 0
doubled per reply): what 128 costs is the sending itself on the rtc's
CPUs, where the deferred frees now run, not the mutex.  So the
fallback and the head-of-line blocking were what made the mutex
contended, and they cost little; the 2.8 GB/s of `rtc_read` (above)
was the work leaving nfsd's threads, not the mutex alone.  At 0
neither fix ever acts (no `conn_full`, no nfsd send in the window):
r728 is within the probe runs at 0 (62.86 to 63.45), and the WRITE
pass with both on passes.

Not kept, for Mike: neither changes what the settled setting does.
The code is not on the branch, only the diff here.  Build 534 runs that
sunrpc from `updates/` until it is taken out
(`SUNRPC_KO=none xeu-rig/swap-sunrpc.sh`, which drops the client's
connections), with both parameters N, which is build 534's own
behaviour.

Taken out on 16 November (Mike): `updates/sunrpc.ko` removed and
`SUNRPC_KO=none xeu-rig/swap-sunrpc.sh` run with no client connected;
the running sunrpc is build 534's own (`19A83B182CB392E30E42C81`),
`updates/` is empty, and `settled.sh apply` ends in the settled
setting.

## The read check, the read size and the drives' local ceiling (16 November)

Build 534, `skb_tx_defer_max` 0, its own sunrpc again.  The WRITE pass
after the swap: r730 77.91 (RERUN), r731 78.08 PASS.

`xeu-rig/read-check.sh <n>` is the READ twin of `write-check.sh` (Mike:
reads locked at 64 GB/s): round 104's read run without the profile,
`WARM=10 RUNTIME=40`, the drives trimmed and 120 s of rest.  PASS at
64.0, RERUN from 63.5, FAIL below.  fio reads 1 MiB at a time since
r734 (Mike: 8 MiB was for the MD stripes); `BS`, `RSIZE` and `IODEPTH`
override.  `rcheckNNN.out` here; each records `nr_file_pages` before
and after.

| run | fio bs | iodepth | rsize | GB/s | mean / p99 ms | a drive GB/s |
|---|---|---|---|---|---|---|
| r732 | 8 MiB | 16 | 1 MiB | 63.71 | 66.8 / 105 | 7.8 to 8.1 |
| r733 | 8 MiB | 16 | 1 MiB | 63.81 | 66.8 / 105 | 7.9 to 8.0 |
| r734 | 1 MiB | 16 | 1 MiB | 63.50 | 8.4 / 13.8 | |
| r735 | 1 MiB | 128 | 1 MiB | 63.50 | 68.3 / 96 | |
| r736 | 1 MiB | 16 | 512 KiB | 67.59 | 7.9 / 12.3 | 8.3 to 8.7 |
| r737 | 1 MiB | 16 | 256 KiB | 73.47 | 7.2 / 11.0 | 9.1 to 9.2 |

The client's 8 MiB split is worth 0.2 to 0.3 at most and costs eight
times the latency; eight times the depth at 1 MiB (r735) adds nothing.
The READ size is what moves the rate: 512 KiB +4.1, 256 KiB +10.0.

No readahead: fio reads `direct=1` on the client, `io_cache_read` is 2
(direct) on the server, and the rtc's READs are async
`IOCB_DIRECT|IOCB_NOWAIT` (r710: 8.10M fast, `fallback_unaligned` 0).
The page cache grew by 124 to 567 pages (2 to 9 MiB) over each run.

**The drives read 104.5 GB/s locally** ([readprobe.sh](readprobe.sh),
[readprobe.out](readprobe.out)): all eight at once, direct, on files
written first.  13.0 to 13.1 a drive from 16 outstanding on, whatever
the order or size (1 MiB in order or at random, 512 KiB, 8 MiB);
98.56 at 8 outstanding a drive and 70.54 at 3.  So NFS at rsize 1 MiB
(7.8 to 8.1 a drive) reads as the drives do with about 3 outstanding,
and a smaller rsize fills them further.  Not yet measured: how many
reads each drive has outstanding under NFS at each rsize (round 98's
`inflight.py`), and why a larger READ leaves them fewer (the rtc's
32 parked calls a connection is one candidate).

The 64 mark and the check's rsize are Mike's to set from this.

## What holds NFS READs: probes at rsize 1 MiB and 256 KiB (16 November)

The read check is locked at rsize 256 KiB and 73.0 GB/s (Mike; AGENTS.md
"READs are locked at 73 GB/s"; `read-check.sh`'s default since then).
Every run below is fio 1 MiB direct reads at depth 16, two ports, eight
drives, `WARM=10 RUNTIME=40`, through `run645.sh` with `PROBE=rtcread`:
[inflight-read.py](inflight-read.py) samples each drive's queue every
1 ms for 4 s, [rtcread.bt](rtcread.bt) times each rtc READ's stages for
5 s, and ten `ss` snapshots give each NFS socket's unread (Recv-Q) and
unsent (Send-Q) bytes.  The probes cost about 0.7 GB/s (r739 against
the plain check r737, the same setting).  `runNNN.out` here.

| run | change from r739 | GB/s | drive idle | 8-55 busy >50% / <5% | rx CPUs mean / max | tx-compl. CPUs | RTT ms | sndbuf-limited |
|---|---|---|---|---|---|---|---|---|
| r739 | (rsize 256 KiB, nconnect 12, rtc, balance N) | **72.76** | 0.5% | 21 / 27 | **36 / 67** | 26 | 0.07 | 0.1% |
| r738 | rsize 1 MiB | 63.81 | 9.3% | 18 / 30 | 63 / 93 | 15 | 0.27 | 10.5% |
| r740 | nconnect 16 | 66.43 | 1.0% | 28 / 20 | 97 / 100 | 28 | 0.56 | 16.9% |
| r741 | nconnect 16, rsize 1 MiB | 60.43 | 11.9% | 9 / 25 | 99 / 100 | 11 | 0.50 | 20.3% |
| r742 | rtc_read N (nfsd threads) | 59.52 | 10.2% | 48 / 0 (93% mean) | 67 / 82 | 43 | 0.57 | 49.2% |
| r743 | rtc_read N, rsize 1 MiB | 59.42 | 13.1% | 48 / 0 (68% mean) | 84 / 93 | 16 | 0.80 | 42.6% |
| r744 | svc_rtc_balance Y | 67.63 | 1.7% | 24 / 21 | 80 / 94 | 39 | 0.26 | 12.5% |
| r745 | a server address per drive (`SRV_ALIAS=1`) | 55.42 | 9.3% | 14 / 3 | 100 / 100 | 20 | 0.97 | 0.1% |

"rx CPUs" are the eight ring CPUs that take each port's receive
interrupts (0, 2, 4, 6, 56, 58, 60, 62); "tx-compl." the eight that take
the transmit completions (the odd ones), mean busy.  RTT and
sndbuf-limited are from the server's `ss -ti` at the run's end (a
connection's life, the layout writes included).

The rtc's stages (rtcread.bt, mean us):

| run | submit → I/O done | → work item picks up | → send starts | send | total |
|---|---|---|---|---|---|
| r738 (1 MiB) | 474 | 358 | 238 | 196 | 1271 |
| r739 (256 KiB) | 234 | 50 | 57 | 21 | 366 |
| r740 (nconnect 16) | 225 | 97 | 82 | 42 | 451 |
| r741 (nconnect 16, 1 MiB) | 460 | 583 | 332 | 373 | 1753 |
| r744 (balance Y) | 286 | 65 | 81 | 27 | 463 |
| r745 (an address a drive) | 285 | 50 | 30 | 22 | 392 |

What it shows:

- **The drives' queue is not what separates the read sizes.**  At rsize
  1 MiB the drives have more queued on average (9.5 to 11.7 commands of
  256 KiB, against 7.6 to 8.3 at 256 KiB) but sit idle 9.3% of the
  samples, against 0.5%.  The drives' largest command is 256 KiB (MDTS
  6, `max_hw_sectors_kb` 256), so a 1 MiB READ is four commands.
- **Calls do not wait at the server; replies do.**  Each socket's
  Recv-Q is a call or less (0.1 to 6 KB), its Send-Q 1 to 3.5 MB of
  replies TCP has not yet sent.  The server holds an rtc READ 0.4 to
  1.8 ms of the 8 to 10 ms the client sees; the rest is in the closed
  loop, most of it in the send queues.
- **The rtc is worth keeping.**  nfsd's threads (r742) take all 48 of
  CPUs 8-55 at 93% and read 59.52; the rtc reads 72.76 with about 21
  CPUs busy and 27 idle.
- **The rtc uses at most one CPU a connection, and there are 24.**  All
  four mounts of a port share one set of the client's `nconnect=12`
  connections (one server address), so 12 a port.  With the settled
  hash placement 24 connections land on 21 CPUs (r739: one CPU carried
  three).  About 80 us of one CPU a 256 KiB READ.
- **More connections or other placement lose, and each time the
  receive CPUs saturate.**  nconnect 16 (32 connections) drops 86,758
  frames on the rings; an address a drive (96 connections, r745) spreads
  the rtc over 45 CPUs but drops 649,754, with p99 213 ms.  balance Y,
  each connection on a CPU of its own, reads 5 GB/s less, with the rx
  CPUs at 80% against 36.
- **The transmit path's cost lands on the receive CPUs.**  Across all
  eight runs the rate falls as the rx CPUs' busy rises (36% at 72.76,
  97 to 100% at 55 to 66), while the transmit-completion CPUs stay at
  11 to 43%.  In a READ load what arrives is the calls and the ACKs of
  the replies, and an ACK frees the sent data it covers
  (`tcp_clean_rtx_queue()` in the receive softirq).  RTT and send-buffer
  limiting rise with it: r739 is the only run with RTT under 0.1 ms and
  no send-buffer limiting.  This is the lead, not yet measured as a
  cause.

Not yet done, for the transmit path: the card's transmit ceiling without
NFS ([iperf-tx.sh](../../xeu-rig/iperf-tx.sh): iperf3 `-R -Z`, written,
not run), each transmit queue's bytes, packets (TSO size) and stops, and
a profile of the rx CPUs at rsize 256 KiB (what share is ACK processing
and freeing sent pages).

`twoport.sh` takes `SRV_ALIAS=1` (each filesystem mounted from its own
server address, 192.168.{1,0}.23n, added for the run and removed after
it); not part of the settled setting.

**The read check after these runs fails twice** ([rcheck746.out](rcheck746.out),
[rcheck747.out](rcheck747.out)): r746 72.25 GB/s and r747 69.82, at
the settled setting (balance N, rtc_read Y, the aliases gone,
`settled.sh` clean before and after), against r737's 73.47 PASS.  By
the rule that is a regression, but nothing in the kernel, its modules or
`settled.sh` changed between them: the geometry and the audit are line
for line r737's, the client as busy (mean 70 to 75%), the rx CPUs 36 to
52%.  The drives read 8.6 to 9.3 GB/s each against r737's 9.1 to 9.2.
So the settled setting reads 69.8 to 73.5 from run to run, and 73.0 is
not a mark it holds.  One candidate, not measured: the rtc places a
connection by a hash of its transport's address, new at every run, so
each run has its own collisions (r739: 24 connections on 21 CPUs).  The
check records no placement; `rtc_stats`' `placed` line in it would
test that.  The mark and what to do about the spread are Mike's.

## Probes after the failed read checks: r756, r757, r754, r755, r758 (16 November)

Five two-port read runs from 16:10 to 16:27, all on build 534 with
build 534's own sunrpc, `run645.sh` with shortened runs (fio's
runtime 20 s for r756, 35 for r754 and r755, 30 for r758, each with a
5 s ramp), rsize 256 KiB, fio 16 jobs at iodepth 16.  Their logs are in
`/var/tmp/zc-run-logs/t2-r75[4-8]`; the session that ran them was lost
in the hang below, so this account is from the logs alone, and what
each was meant to answer is not recorded beyond what it measured.

| run  | setting                                    | nconnect | fio bs | GB/s  | probe |
|------|--------------------------------------------|----------|--------|-------|-------|
| r756 | `svc_rtc_cpus=8-39`, balance Y, RPS 40-55  | 16       | 8M     | 68.72 | rx perf (empty) |
| r757 | as r756                                    | 24       | 8M     | -     | mount failed |
| r754 | settled                                    | 12       | 8M     | 68.57 | client: ss, locks, workqueue |
| r755 | settled                                    | 12       | 1M     | 73.79 | ring CPUs' profile |
| r758 | settled                                    | 12       | 1M     | 71.87 | `rtcpass.bt` |

No drive stepped down in any of them, and the settled runs audited
clean before and after.

- **r756**, the rtc on 8-39 with balance and RPS on 40-55, placed one
  connection to a CPU across 8-38 and read 68.72 GB/s, nfsd's CPUs 64%
  busy against 33 to 36% at the settled setting.  Nothing gained.
- **r757** (nconnect 24) never ran: `mount.nfs: an incorrect mount
  option was specified`.  The client takes nconnect up to 16.
- **r754**, the settled setting with fio reading 8 MiB at a time,
  read 68.57, about what r756 did; with 1 MiB (r755, r758) the same
  setting reads 71.9 to 73.8.  The client's probe (its 24 sockets sampled
  ten times): about a quarter of the samples hold over 10 MB of
  replies unread in Recv-Q (up to 29 MB), most hold under 2 MB, and
  every socket's sends are limited by the server's receive window 47 to
  72% of the time.  The client's lock
  waits are in its workqueues (`worker_thread`, `queue_work_on`) and
  in sunrpc's transmit (`call_encode`, `xprt_transmit`,
  `xs_stream_data_receive_workfn`, waits up to 250 to 400 us).
- **r755** read **73.79 GB/s**, over the 73.0 mark that r746 and r747
  missed, at the same setting: more of the run-to-run spread noted
  above.  The profile of the ring CPUs (0-7, 56-63; 31 to 38% busy)
  puts about half their cycles in freeing the pages of sent replies
  when their ACK arrives: `tcp_ack` -> `tcp_clean_rtx_queue` ->
  `skb_attempt_defer_free_tx` -> `skb_release_data` ->
  `__free_frozen_pages` -> `free_frozen_page_commit`, 29% under
  `_raw_spin_unlock_irqrestore` and 17% in `skb_release_data` itself.
  Without pseudo-NMI a sample taken with interrupts off lands where they
  are turned back on, so the 29% is time inside the page free's
  interrupts-off section, not the unlock.  This is the lead of "What
  holds NFS READs" measured: the transmit path's cost on the receive
  CPUs is mostly the freeing of sent pages (`skb_tx_defer_max` 0).
- **r758** (`rtcpass.bt`, 8 s) read 71.87.  24 connections on 20 CPUs
  (CPU 40 holds three, 9 and 23 two each); each of those CPUs spent 75
  to 83% of the 8 s in rtc passes.  1.85 million passes, nearly every
  one a single call and its reply.  A pass takes 66 us on average:
  receive 8, `svc_process` 11, the send 20, and 25 in the rest (the
  completions, inlined, and the pass's own cost).  A kicked pass waits
  58 us on average to start (most 16 to 128 us).  The server's sockets
  hold only 4.6 KB unread on average (9 KB at most) under the probe, so
  calls are not waiting in them.  45 of a pass's 66 us are completion
  and send, on the CPU that also receives; that is what the split below
  was written to move.

## The send work item split off (`svc_rtc_tx_split`): tardis2 hung (16 November)

**The change** ([svc-rtc-tx-split.patch](svc-rtc-tx-split.patch),
committed on 16 November as `008493ad5d9a6`, "SUNRPC: Optionally run a
connection's completions and sends apart"): `sunrpc.svc_rtc_tx_split`, latched per
connection at attach, gives each connection a second work item, on a
CPU of its own on the receive CPU's node, for its completions (the
READ's reply encoded, `svc_rtc_run_completions()`) and its send FIFO
(`svc_rtc_drain_sends()`).  The first work item then only receives and
submits.  The idea is that one READ costs its connection's CPU its
receive, submit, completion and send, so a connection's calls are
limited by one CPU doing all four.  The new fields sit in a wrapper
private to `svc_xprt.c` (`struct svc_rtc_conn_tx`), so
`include/linux/sunrpc/svc_xprt.h` is unchanged and the module's 442
export versions match build 534's: nfsd and lockd load against it.
It was built at `/var/tmp/zc-run-logs/sunrpc-split-534` with no
warning, and `swap-sunrpc.sh` put it in build 534's `updates/` with no
client connected (srcversion `1907D42B9BE624B41C23DA2`, split N at
load).

**The run:** t2-r759, the first of a strobe pair (rsize 256 KiB, then
1 MiB, then the control at N), with split set on: `run645.sh` with
`RSIZE=262144 TRIM=0 REST=10 WARM=5 RUNTIME=20 FILL_REST=3` (the rest
of the command line, the probe included, is lost).  The settled setting
audited clean, both ports were set up and the eight filesystems mounted
(the last file written, `run/geometry.txt`, at 16:51:26).  Then the
host hung: `wrote.0` is empty, so fio never reported, and the console
showed nothing more.  tardis2 came back at 17:00:57 into build 534.

**Nothing was saved.**  kdump was armed (`kexec_crash_loaded` 1,
`crashkernel=1G`), and `softlockup_panic`, `hardlockup_panic`,
`hung_task_panic` and `panic_on_oops` are all 1, yet `/var/crash` is
empty, pstore is empty and the journal holds only the new boot.  An
oops, a soft lockup or a hung task would have panicked into kdump.  But
`nmi_watchdog` is 0: build 534's command line has no
`irqchip.gicv3_pseudo_nmi=1`, so a CPU spinning with interrupts off is
never detected and the host just stops.  That fits what was seen; the
other reading is kdump failing as it did on build 506 ("KUnit crashes
on build 506" in `TESTING.md`).  `dmesg.0`, taken before the load,
ends with the swap's sunrpc and nfsd reload and no warning.

**The suspect is the split**, not proven.  With it a connection's
completions and sends run on another CPU while its receive runs,
where before one work item did them in turn.  Read against the code,
no fault stands out: `conn->pending` still has one owner (only the send
work item drains, and `svc_rtc_xprt_stop()` flushes both work items),
the sends still hold `xpt_mutex`, and the rqst pool and
`svc_tcp_release_ctxt()` are per rqst and already used from nfsd
threads beside the receive.  What is new and unread: nfsd's completion
(`ops->complete`) running alongside the same connection's receive, and
`svc_rtc_park()` from the send work item where the comment in
`svc_rtc_fast()` says only the receive work item parks.

**The host now:** build 534 with the split `sunrpc.ko` still in
`updates/` (Mike: leave it), so it loads at every boot, split N, which
is the control mode.  `settled.sh` does not audit sunrpc's srcversion.

**Before split is run again:** boot with `irqchip.gicv3_pseudo_nmi=1`
(round 61's kernel line) so that a hard lockup panics and kdump has
something to save, and start with one port and a few connections
rather than the two-port read.

## Next boot: build 534 with pseudo-NMI, the split on one port (Mike, 16 November)

Mike: "reboot with pseudo-NMI and retry the split on one port".  The
boot is the one-shot entry `ofp-rocky-534-pnmi.conf`: build 534's own
entry with `irqchip.gicv3_pseudo_nmi=1` added (`bootctl set-oneshot`
on it; build 534's default entry is unchanged, so a boot that fails
falls back to it without pseudo-NMI).  The split `sunrpc.ko` stays in
`updates/` and loads at boot with split N.

After the boot, before any run:

1. `grep gicv3_pseudo_nmi /proc/cmdline`; dmesg `GICv3: Pseudo-NMIs
   enabled` and `NMI watchdog: Enabled`; `kernel.nmi_watchdog` 1.  If
   the boot has no pseudo-NMI, it fell back to build 534: say so and
   stop.
2. The boot's checks: kdump armed (`./bringup.sh` first), both ports
   pinged, `./settled.sh apply` ending in "the settled setting", sunrpc
   `1907D42B9BE624B41C23DA2` with `svc_rtc_tx_split` N, dmesg with no
   `WARNING:`, `BUG:` or `Oops`, no NFS client connected.  Commit the
   boot's outcome here before the run.
3. `NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/104-rx-cpus/run-split1.sh 760` (it changes to `xeu-rig/` itself): it refuses a boot
   without pseudo-NMI, the NMI watchdog, kdump or the split sunrpc, then
   applies the settled setting, sets split Y and reads on port a only,
   four connections (`NCONN`), eight fio jobs, 1 MiB at rsize 256 KiB,
   `WARM=5 RUNTIME=20`; split goes back to N on exit.  It prints the
   rate, any new kernel warning, and the audit after.
4. If it hangs again, the NMI watchdog should panic it into kdump:
   after the reboot read `/var/crash/*/vmcore-dmesg.txt` (its name's
   date is the boot clock's, not the day's) and rename it once read.
   If it passes, more connections next (`NCONN=12`), then two ports,
   each a run of its own, and the split's rate against split N at the
   same shape before anything is kept.

### The boot (16 November, 18:00): port a dead, no run

The boot is the one meant: `irqchip.gicv3_pseudo_nmi=1` on the command
line, `GICv3: Pseudo-NMIs enabled`, `NMI watchdog: Enabled`,
`kernel.nmi_watchdog` and `hardlockup_panic` 1, kdump armed (1024 MiB),
the split sunrpc `1907D42B9BE624B41C23DA2` with `svc_rtc_tx_split` N,
and no `WARNING:`, `BUG:` or `Oops` in dmesg.

The client still held t2-r759's eight mounts and its fio (started
16:51:36), stuck since the hang; the fio was killed and the mounts
taken off (port a's lazily, then the bash still waiting in its
`rm -f` on port a killed), leaving the client with no NFS mount and
nothing in D state.  `bringup.sh 32768 256` set up port a's region and
rings and stopped before port b on the client's connections.

**Port a is dead after boot** (`PORT-DEAD-AFTER-BOOT.md`): carrier 1,
`rx_packets` 0, `rx_dropped` 16, 13 packets sent, the neighbour
`FAILED`; port b answers.  The client's port b mounts had reconnected
when nfsd started and port a's never had, so it was dead before
`bringup.sh`.  Nothing else was tried on it.  `run-split1.sh` runs on
port a, so the split was not run: the host waits for a power reset.
The one-shot entry `ofp-rocky-534-pnmi.conf` is set again for it.

## Next boot: after the power reset (16 November)

Mike power-cycles tardis2 for the dead port a.  The one-shot entry
`ofp-rocky-534-pnmi.conf` is set again (`bootctl status` showed
`OneShot Entry: ofp-rocky-534-pnmi.conf`), so the boot should be build
534 with pseudo-NMI, the split `sunrpc.ko` loading from `updates/` with
split N.  The client (192.168.0.106 / 192.168.1.106, management
10.20.12.106) was left clean: no NFS mount, no fio, nothing in D state.

1. Step 1 of "Next boot: build 534 with pseudo-NMI" above: pseudo-NMI
   on the command line, `Pseudo-NMIs enabled` and `NMI watchdog:
   Enabled` in dmesg, `kernel.nmi_watchdog` 1.  If not, the boot fell
   back to build 534's default entry: say so and stop.
2. **Ping both ports before anything else**, from the server:
   `ping -c3 -W2 192.168.1.106` (port a, `enp161s0f0`) and
   `192.168.0.106` (port b, `enp164s0f0`).  If one does not answer,
   read its `rx_packets` (0 is the dead port again), add a row to the
   table in `PORT-DEAD-AFTER-BOOT.md`, commit, and hand back for another
   power reset (set the one-shot again first).  Try nothing else on it.
3. If the client has NFS mounted from this host at that point (an
   earlier run's mounts reconnecting when nfsd starts), `bringup.sh`
   stops before port b ("NFS connections are up"): clear the client
   first (`pkill -9 -x fio`, then `umount -f`, or `umount -l` when busy,
   on each `/mnt/rxr-*`).
4. Then step 2 of the section above as written: `./bringup.sh 32768
   256` (it arms kdump first and stops if it cannot), both ports pinged
   again, `./settled.sh apply` ending in "the settled setting", sunrpc
   `1907D42B9BE624B41C23DA2` with `svc_rtc_tx_split` N, no
   `WARNING:`, `BUG:` or `Oops`; commit the boot's outcome here and add
   the boot to `PORT-DEAD-AFTER-BOOT.md`'s table (a row with port lost
   "none" counts too).
5. Then steps 3 and 4 of that section: `run-split1.sh 760`, and if it
   hangs, the vmcore in `/var/crash` after the reboot.

The working tree holds uncommitted changes that predate this boot
(`xeu-rig/iperf-tx.sh`, `read-check.sh`, `settled.sh` with `EXP_RPS`,
`twoport.sh`, `net/sunrpc/svc_xprt.c`, which is the split, and
`fml.txt`): leave them as they are.

### The boot after the power reset (16 November, 18:14): port a dead again

The boot is the one meant: `irqchip.gicv3_pseudo_nmi=1` on the
command line, `GICv3: Pseudo-NMIs enabled`, `NMI watchdog: Enabled`,
`kernel.nmi_watchdog` and `hardlockup_panic` 1, kdump armed, the split
sunrpc `1907D42B9BE624B41C23DA2` with `svc_rtc_tx_split` N, and no
`WARNING:`, `BUG:` or `Oops` in dmesg.

Pinged as the boot left them, before `bringup.sh`: port b
(192.168.0.106) answered 3 of 3, **port a (192.168.1.106) 0 of 3**,
`Destination Host Unreachable`, neighbour `FAILED`.  Port a was not
silent, as on 7 November's fourth boot: 48 frames received (21, 24, 2
and 1 on rings 0 to 3), not rising while pinged, 119 dropped and
rising, 20 sent.  Its `ring_0_rx_hw_pp_drop_packets` (102) is no clue:
the working port b shows 50 there too.  `port_rx_fifo_tc0_packets` 0
on both.  Counters and dmesg are in
`/var/tmp/zc-run-logs/t2-514-canary/port-dead/534-pnmi-boot2/`.

Nothing was tried on it, `bringup.sh` was not run and the split was
not run.  The one-shot entry `ofp-rocky-534-pnmi.conf` is set again
(`bootctl status`: `OneShot Entry: ofp-rocky-534-pnmi.conf`) and the
host waits for another power reset; the plan after it is "Next boot:
after the power reset" above, unchanged.

### The boot after the second power reset (16 November, 18:21): both ports

The boot is the one meant: `irqchip.gicv3_pseudo_nmi=1` on the
command line, `GICv3: Pseudo-NMIs enabled`, `NMI watchdog: Enabled`,
`kernel.nmi_watchdog` and `hardlockup_panic` 1, kdump armed, the split
sunrpc `1907D42B9BE624B41C23DA2` with `svc_rtc_tx_split` N.  **Both
ports answered 3 of 3 before `bringup.sh`** and again after it.  The
client had no NFS mount, no fio and nothing in D state, and nfsd no
connection.  `./bringup.sh 32768 256` set up both ports' region and
rings, `./settled.sh apply` ended in "the settled setting", and dmesg
has no `WARNING:`, `BUG:` or `Oops`.

The working tree's leftovers were committed before the run (Mike: "commit
the various changes ... rather than let them linger in the tree"): the
split as `008493ad5d9a6`, and the rig's `iperf-tx.sh`, the quick-strobe
knobs (`TRIM`, `FILL_REST`) and `settled.sh`'s `EXP_RPS` as three docs
commits.  The split `sunrpc.ko` in `updates/` is that commit's
`svc_xprt.c`.  Next: `run-split1.sh 760`.

### t2-r760: the split on one port, four connections, no hang

`run-split1.sh 760` (split Y; port a, `/hs/nvme0` to 3, four
connections, eight jobs, 1 MiB at rsize 256 KiB, `WARM=5 RUNTIME=20`)
ran to the end: **21.74 GB/s** read, 21.93 on the wire, 6.12 ms mean
completion (7.31 at the 99th percentile), no FIFO or ring drop, 9
client retransmits, no new `WARNING:`, `BUG:`, `Oops` or lockup in
dmesg, and `settled.sh audit rest` clean after with split back at N.
The drives decide the figure: each read 5.4 GB/s at 96% busy, so this
says nothing of the split's rate, only that one port at four
connections does not hang.  rtc stats over the run: 2.08 M diverted,
2.07 M sends in 1.55 M batches, `budget_exhausted` 549 k (26% of
runs), `fallback_no_peek` 1607.  Logs in
`/var/tmp/zc-run-logs/t2-r760/`.  Next, as planned: `NCONN=12`.

### t2-r761: twelve connections hung the host (16 November, 18:32)

`run-split1.sh` with `NCONN=12` (split Y, port a, `/hs/nvme0` to 3,
eight jobs, 1 MiB at rsize 256 KiB) got as far as "a: placed 12
flows" (`run.log`, 18:32) and the host went down: `wrote.0` is
empty, so fio never reported.  It came back at 19:03 **on build 534's
default entry, without pseudo-NMI**: the one-shot entry was spent by
the boot of 18:21.  `/var/crash` is empty, the journal holds only
this boot, and the console's last lines are not on the host, so
nothing says what the hang was.  On this boot `kernel.hardlockup_panic`
and `softlockup_panic` read 1 (`nmi_watchdog` 0, no pseudo-NMI); on
the boot that hung they were 1 too, with the NMI watchdog on, so a
hard or soft lockup should have panicked into kdump, which was armed.
Either the hang was neither, or kdump did not fire: for the next
split run, the BMC's serial console (`console=ttyAMA0`) is the record
to keep.  Port a was dead on this boot before anything was run (96
frames received, 161 dropped, neighbour `FAILED`; a row in
`PORT-DEAD-AFTER-BOOT.md`); nothing was tried on it.

### Audit of the split: the send work item releases a call the receive pass is still in

Mike asked for an audit of `008493ad5d9a6` for every way it could take
the host down.  One bug explains both hangs.  Everything in the
run-to-completion path rests on one rule, written in its comments
("Once published, a running pass may finish @r and reuse it", "The
work item drains the FIFO after this call returns", "Only the
connection's work item parks"): a call is finished by the same work
item that received it, so a completion cannot run until the receive
pass is out of `svc_process()` and `svc_rtc_after_process()`.  The
split breaks it, because the send work item runs on another CPU at the
same time as the receive work item:

- **An async READ completing before `svc_process()` returns.**  nfsd
  sets `RQ_RTC_ASYNC` before the submit ("the completion may run
  before it returns"); `svc_rtc_complete()` then queues the send work
  item, which runs `ops->complete` and `svc_rtc_process_resume()`
  while the receive pass is still unwinding nfsd on the same `rqstp`,
  at an `rq_rtc_aoffset` that `svc_process_common()` stores only after
  dispatch returns.  It sends the reply and releases the rqst
  (clearing `RQ_RTC_ASYNC` and `r->queued`, `rq_xprt` NULL, the rqst
  back on the free list).  The receive pass's `svc_rtc_after_process()`
  then calls nfsd's `rqst_done()` on the released rqst, finds neither
  `ASYNC` nor `MISS` nor `queued`, and **releases it a second time**.
- **A synchronous reply.**  `svc_send()` puts it on `conn->sendq`
  inside `svc_process()`, with no kick.  A send work item already
  draining sends and releases it before the receive pass is done, with
  the same second release.

A second release does one of: `svc_xprt_release()` on `rq_xprt` NULL
(an oops, which kdump would have saved); the rqst queued twice on the
pool's lwq, a cycle that `lwq_dequeue()` walks for ever under its
lock (a hang with no oops, as t2-r759 and t2-r761 were); or, if the
rqst was already taken by another call, that call's pages released
under its direct I/O (memory overwritten by DMA, a fault later and
elsewhere).  It also puts the transport and the active count twice
(the xprt freed while in use), and a late `svc_rtc_park()` leaks
`conn->parked`.  t2-r760 at four connections showed
`fallback_conn_full` 0, so the race likely never hit there; twelve
connections on the same four drives, with more interrupts on the
receive CPUs, widen the window.

Nothing else in the split was found that would crash:
`svc_rtc_xprt_stop()` flushes the receive work item and then the send
work item, and every queued pass holds a pin, so `kfree()` of the
connection cannot run under a queued `tx_work`; `svc_rtc_tx_place()`
from hard IRQ keeps the per-CPU counts balanced through `xchg()` and
picks only online CPUs; `conn->pending` is touched only by the send
work item, under `xpt_mutex`, as a thread's send is.  No KUnit case
ran with the split set.

### The fix, its KUnit cases, and the module

Three commits at the tip:

- `254cead34a672` "SUNRPC: Do not let a split send work item finish a
  call still received" (code).  The receive pass marks its rqst
  `RQ_RTC_RXOWN` (a new `rq_flags` bit, last, so no other bit moves).
  While it is set, `svc_rtc_queue_reply()` holds the reply back and
  the pass puts it on the FIFO in `svc_rtc_after_process()`.  Of the
  pass and an async completion, whichever is last puts the call on
  `conn->done`: each does `test_and_clear_bit(RQ_RTC_RXOWN)`, and the
  one that finds it clear publishes (`svc_rtc_publish_done()`).  The
  pass parks the call and `rq_rtc_aoffset` is stored before that, so
  both are seen by the send work item.  A connection that is not split
  never sets the bit and runs as before.
- `acb791346c0bd` "SUNRPC: expose svc_rtc_tx_split for the KUnit
  suites" (interface).
- `8193d1e76ac48` "SUNRPC: test a split connection's handoff to its
  send work item" (tests): five cases in `sunrpc_svc_rtc`, the reply
  held back while the pass is in its rqst, the early completion not
  run until the pass is done, a completion after the pass, and the
  in-flight teardown and completion race on a split connection.  The
  first two queue a send of their own from the hook's `rqst_done()`
  on the receive pass and record what the send work item has done
  when it goes out.

Built with `xeu-rig/build-sunrpc.sh /var/tmp/zc-run-logs/sunrpc-split2-534`
against build 534 with no warning; the 442 export versions match build
534's, so nfsd and lockd load against it.  `sunrpc.ko` srcversion
`C3403E1043259A7524692BC`, with `svc_rtc_kunit.ko` beside it.
**Neither has been loaded or run**: the module in `updates/` is still
`1907D42B9BE624B41C23DA2`, the split with the bug (split N at boot, so
it is safe as it is).

### Next (Mike's to decide)

1. Port a is dead on this boot: a power reset, with the one-shot
   `ofp-rocky-534-pnmi.conf` set again first.
2. On that boot, before any run, the `sunrpc_svc_rtc` suite from
   `sunrpc-split2-534` on its own, after committing that it is about
   to run ("KUnit crashes on build 506" in `TESTING.md`; it needs the
   new `sunrpc.ko` loaded, so `swap-sunrpc.sh` first, with no client
   connected:
   `swap-sunrpc.sh /var/tmp/zc-run-logs/sunrpc-split2-534`).
3. Then `run-split1.sh` at four connections and at twelve again, with
   the serial console logged, and the WRITE and READ checks if the
   split is to be kept.
