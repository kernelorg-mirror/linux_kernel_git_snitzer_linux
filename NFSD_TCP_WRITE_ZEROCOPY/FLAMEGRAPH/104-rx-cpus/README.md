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

`PROBE=none RUNS="651:read:0 652:read:64 653:read:0 654:read:64
655:read:16" run645.sh`, then the WRITE pass at 64
(`write-check.sh 656`), the sysctl put back to 0 at the end. 0 is the
default (each CPU's list high 874 to 889 pages, batch 15); 64 gives a
high of 2,005 pages (31 MiB a CPU) and 16 of 8,021 (125 MiB). Nothing
is profiled in these runs (`PROBE=none`), so the busy figures are the
load's own. `settled.sh` does not hold the sysctl.
