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
