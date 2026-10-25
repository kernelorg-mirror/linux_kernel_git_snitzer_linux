# FLAMEGRAPH: analyze, fix, test, repeat

A reference for improving the server's CPU cost a step at a time, started
2026-10-23 on tardis2 (build 494, 16 KiB pages) after "SUNRPC: Wake an idle
service thread only once" (`2fc9cd2e839b`) cut nfsd's wakes 2.6 times and
took 1 to 2.5% off the rings. Mike: "every win helps! Remember sometimes it
takes 5 or 20 things to come together to realize full performance".

**The target is loan mode (Mike, 2026-10-23: "I want to squeeze out all
lost performance for loan mode").** Every round profiles, ranks and tests
the loan run, and a change is judged by what it saves a loaned WRITE. Copy
mode is not profiled or run beside it. AGENTS.md's rule runs one way: copy
mode is not to be made faster at the loan's expense, not the reverse.

Each round is a directory here, numbered in order:

| round | what was profiled | the most costly thing found | the change | outcome |
|---|---|---|---|---|
| [01-loan64-baseline](01-loan64-baseline/README.md) | one-port loan64, with the one-wake fix: rings 92.0 us a WRITE, nfsd 184.1 | page_pool's put and get of the region's pages (13.8 us); conntrack from podman's unused netavark table (9.0 on the rings, 9.4 on nfsd) | conntrack: the leftover table deleted and port 2049 untracked, in settled.sh | kept: rings 92.6 to 78.7 us a loaned WRITE (-15%), copy 67.9 to 63.4 (-7%) |
| [02-refs-valid](02-refs-valid/README.md) | the same, with round 01 | `svc_tcp_rx_refs_valid` walked the record on each of 22 hand-offs a WRITE (7.05 us on nfsd) | `49f0fc4564ec`: the exchange checks only counts | kept on the profile: refs_valid 7.05 to 0.90, the exchange 7.52 to 3.10 us on nfsd; the A/B (mpstat) cannot resolve it |
| [03-skb-defer](03-skb-defer/README.md) | the same, with round 02 | deferred skb frees on the ring (8.85 us), `napi_pp_put_page` a remote line | `net.core.skb_defer_max` 0, in settled.sh | kept: rings 80.0 to 68.0 us a loaned WRITE (-15%), nfsd 167.5 to 159.8 |
| [04-region-stack](04-region-stack/README.md) | build 499, one-port loan64 | the region pools' 256-page stack overflows: 7% of pages back to the region, about 5 us a WRITE | `dc23eac70f34`: `page_pool.rx_region_stack` 1024 | **not kept** (Mike, round 19): rings 60.0 to 56.5 us a WRITE (-6%), nfsd 111.5 to 107.5, but the region's peak +890 pages (14 MiB), against the L3. Not through the overflow, which happens only in the first run after the port opens. settled.sh holds 0 |
| [05-data-ready](05-data-ready/README.md) | the same, with round 03 | `svc_data_ready`: the `xpt_flags` line, two clock reads, a store into another thread's `svc_rqst` | `eff5f16d4f30`, `0f0ed4159a12` | kept: rings 68.4 to 65.0 us a loaned WRITE (-5%), nfsd 159.7 to 156.4 |
| [06-loan-lowat](06-loan-lowat/README.md) | the same, with round 05 | a WRITE taken up 12 times: wakes, hand-offs, ACKs (about 50 us on nfsd) | `c8cf27841351`: a low-water mark while a loaned record is incomplete | not kept, reverted (`b8c9ee7c6d2a`): nfsd 154.5 to 121.0 us a WRITE, but TCP ACKs every skb below the mark, so the rings 67.0 to 85.0 |
| [10-wait-rest](10-wait-rest/README.md) | round 05's state | the same churn (06b) | `c0995ea94798`: `svc_data_ready` holds `XPT_DATA` back for a short loaned record, `sk_rcvlowat` stays 1; `dd4fef2000a0` (10b): at most half the open window | 10 not kept: the rate fell to 36-38 GB/s and p90 to 4.3 ms, since what was wanted outgrew the window. 10b kept: nfsd 153.8 to 127.5 us a WRITE (-17%), rings 63.1 to 61.2, half the segments out, p90 0.537 against 0.569-0.594 |
| [11-socket-hold](11-socket-hold/README.md) | round 10b's profile | TCP's receive in the ring's softirq (`tcp_v4_do_rcv`, 11.05 us a WRITE) | hold the socket across a loaned record's arrival, so the backlog is processed on nfsd | not pursued, no run: it moves CPU without saving any, the rings are not the limit at 40.4 GB/s, a thread owning the socket is not woken by the backlog, and TCP removed its prequeue, the same idea, in 2017 |
| [12-drc-size](12-drc-size/README.md) | build 499, one-port loan64 | the v3 duplicate reply cache: 63 MB, full, 0 hits, about 4.5 us a WRITE of cold misses | `635e44bf37bf`: `nfsd.drc_max_entries`, an nfsd restart for each size; v3 stays, for flexfiles data servers | **kept at 4096** (settled.sh, after round 19): nfsd 111.5 to 108.1 us a WRITE (-3%); 32768 saves nothing; rings, rate and latency the same |
| [13-cred-cache](13-cred-cache/README.md) | round 10b's profile | `fh_verify` on every WRITE and COMMIT: handle decode, a new cred, two export lookups (5.7 us) | `916c11a2d1fc`: each nfsd thread installs its last cred again for the same user, and the decode's raised copy with it | **kept**: nfsd 126.5 to 118.3 us a WRITE (-6.5%); `prepare_creds` and `rcu_core` gone from the profile; latency the same |
| [14-page-prefetch](14-page-prefetch/README.md) | round 13's profile | each borrowed page's `struct page` missed on nfsd up to three times: `svc_tcp_rx_region_span` (2.7 us a WRITE on its `pp_magic` load), `svc_tcp_rx_borrow_segment` (3.4 of its 4.7 us self on `page_pool_page_is_pp()`), `napi_pp_put_page` (2.7) | `02a445664e23`: `prefetchw()` the `struct page` of each page a loaned skb's record bytes lie in, before they are examined; reusing the page-pool answer and one ref a page a skb not done | **kept**: nfsd 117.2 to 114.9 us a WRITE (-2%); `region_span` 3.2 to 1.0, the borrow 4.6 to 3.2, the walk 1.9; latency the same |
| [15-region-line](15-region-line/README.md) | build 499, one-port loan64 (fl15) | `svc_tcp_rx_append`'s region check reads `pool->rx_region` (offset 864) on the line with `rx_stack` and `rx_stack_n`, which the ring writes for every region page it takes or returns: about 0.6 us a WRITE of false sharing on nfsd | `085734569d9d`: `rx_region` moved to the struct's last line, written only at registration | in the kernel, no switch: the load 0.31 to 0.23 us a WRITE (fl17 against fl15), about 0.1, not 0.6 |
| [16-release-prefetch](16-release-prefetch/README.md) | round 14's profile | `page_pool_put_netmem_bulk` 5.5 us a WRITE, 85% one load of each page's `compound_head` behind the pool's ordered atomics, before the reply | `e5e84a05b686`: `prefetchw()` each borrowed page's `struct page` before the references are dropped | not kept: the release 1.6 us cheaper in the profile, nfsd 114.2 against 113.8 us a WRITE, inside the scatter; reverted (`a081854bc12c`). A 14.5 s stall in one B run, with the server's replies unacknowledged, cause not found |
| [17-read-on](17-read-on/README.md) | round 14's profile, and `xeu-rig/wakesrc.bt` | the wake path: 7.5 wake calls and 3.5 wakes a WRITE, about 1 us each to the waker; 4.4 of 8.7 passes through `svc_recv` find nothing; one partial hand-back in seven finds `XPT_DATA` set again | `5fde85ef4076`: a thread whose read left a loaned record incomplete reads on if `XPT_DATA` is set again when the socket lock is released | **kept**: 6.0 to 5.4 enqueues a WRITE, 4.56 to 4.34 wakes; nfsd 115.5 to 113.2 us a WRITE (-2%); latency the same |
| [18-looking-count](18-looking-count/README.md) | round 17's wake trace | wakes of idle threads for work a running thread takes anyway: 0.55 empty passes a WRITE that slept and were woken | `331bd9a8d93a` (18): no wake while a thread looking for work is counted; `05bb553779e2` (18b): counted only until it dequeues; `0fae825c58f7` (18c): no wake only while two are counted | not kept, all three reverted: 18 nfsd -6.9 us a WRITE at saturation but p50 at iodepth 1 0.465-0.498 against 0.42-0.44; 18b no gain; 18c nfsd -3.1 and rings -2.2, but 2 of 4 clean latency runs slow (+11 us mean) |
| [19-stack-and-drc](19-stack-and-drc/README.md) | build 499, rounds 04 and 12 together | | both at once, A both 0, B 1024 and 4096, 4 runs a side | rings -3.15 ± 0.75 us a WRITE, nfsd -3.95 ± 1.10 (not the -7.4 of the two apart), region +940 pages; latency the same. Mike keeps the reply cache, not the stack: "we cannot afford to increase the region" |
| [20-msix-mask](20-msix-mask/README.md) | build 499 at the settled setting | `enable_irq` on the rings, 10.4 us a WRITE: an ITS mask, unmask and resend for most polls | xeu `irq_msix_mask` (kmod-xsight 2.0.1-9): the ring masks its own MSI-X entry in the card, 4 runs a side | rings -9.97 ± 0.74 us a WRITE (-17%), nfsd, rate, region and latency the same; twice the Rx interrupts, each cheap. Kept at Y (settled.sh) |
| [07-srv-qdisc](07-srv-qdisc/README.md) | round 05's state | half of each ACK and reply is `__dev_xmit_skb` | the server's qdisc `pfifo_fast` | not kept: no change (rings 66.9 against 66.1, latency inside L0's scatter); the cost is the transmit path's lock with 48 CPUs on 4 queues, not the qdisc |
| [08-notrack-needed](08-notrack-needed/README.md) | round 05's state | the `zc_notrack` chains run with no conntrack to skip | `zc_notrack` only while a table registers conntrack, in settled.sh | kept: rings 67.6 to 65.1 us a loaned WRITE (-3.7%), nfsd 160.6 to 154.2; latency unchanged |
| [09-big-tcp](09-big-tcp/README.md) | round 08's state | per-skb costs at about 20 GRO skbs a WRITE | BIG TCP: the server's `gro_max_size` and `gro_ipv4_max_size` 256 KB | not kept: no change (rings 63.3 against 63.4, latency the same); aggregates end at the client's 64 KB TSO bursts, so it needs the client's `gso_ipv4_max_size` too (Mike); the wake itself measured, 98.5% under 2 us |
| [L0-latency](L0-latency/README.md) | round 05's state, 4 in flight | p50 0.46-0.52 ms; about 0.2 ms is the drive, 0.02 the reply; the apparent wake tail is the tracepoint's stale stamp | `xeu-rig/lat.sh` | the baseline; runs differ by about 10%, so several a side |

## Where the kept rounds stand (2026-10-25)

What a loaned WRITE costs after each kept round, from that round's B
runs (the profile where the A/B cannot resolve it):

| after | rings us/W | nfsd us/W | what was kept |
|---|---|---|---|
| 01 base | 92.6 | 184.1 (profile) | |
| 01 | 78.7 | | podman's leftover conntrack table deleted, port 2049 untracked (settled.sh) |
| 02 | | refs_valid 7.05 to 0.90 | `49f0fc4564ec`: the hand-off checks only counts, not the whole record |
| 03 | 68.0 | 159.8 | `skb_defer_max` 0: no deferred skb frees (settled.sh) |
| 05 | 65.0 | 156.4 | `eff5f16d4f30`, `0f0ed4159a12`: a cheaper `svc_data_ready` |
| 08 | 65.1 | 154.2 | `zc_notrack` only while a table registers conntrack (settled.sh) |
| 10b | 61.2 | 127.5 | `c0995ea94798`, `dd4fef2000a0`: no wake for a partial loaned record, held back at most half the open window |
| 13 | | 118.3 | `916c11a2d1fc`: each nfsd thread reuses its last credentials |
| 14 | about 59 | 114.9 | `02a445664e23`: `prefetchw()` of the loaned pages' `struct page` |
| 17 | 58.2 | 113.2 | `5fde85ef4076`: a thread reads on when a loaned record's rest arrives during its read |
| 12 | | 111.5 to 108.1 on build 499 | `drc_max_entries` 4096: a reply cache small enough to stay hot (settled.sh) |
| 20 | 59.95 to 49.98 on build 499 | | xeu `irq_msix_mask` Y (kmod-xsight 2.0.1-9): a ring's interrupt held off at the card, not through the ITS (settled.sh) |

- **Since round 01:** the rings 92.6 to about 58 us a WRITE (-37%),
  and to about 50 on build 499 with the interrupt held off at the card
  (-46%); nfsd 184 to about 113 (-39%), and to about 108 on build 499
  with the smaller reply cache (-41%).
- The rate stayed at the drives' 40.5 GB/s throughout. Latency at
  iodepth 1 is the same or a little better.
- Each round's A/B had its own A, and A drifts a couple of us between
  sessions. The totals hold to about 2 us.
- Rounds 04, 12 and 15 ran on build 499 on 25 October (`run33.sh`), and
  04 and 12 together in round 19: `drc_max_entries` 4096 is kept (nfsd
  -3.4 us a WRITE); `rx_region_stack` 1024 is not, as it holds 940 more
  of the region's pages (Mike); the `rx_region` move saves about 0.1.

## Why CPU a WRITE, not GB/s

On one port the four drives under it (`/hs/nvme4`-`7`) hold every run at
40.0 to 40.4 GB/s, DRIVE-LIMITED, so no change to the CPU path moves the
rate. What a change can move is what a WRITE costs the CPUs that carry it:

- **the rings**: port a's ring CPUs (0, 2, 4, 6),
  which run at 87-90% softirq at 40 GB/s. They are the next limit once the
  drives stop being it: faster drives, or two ports on faster drives.
- **nfsd**: CPUs 8-55, about 14% busy, where a WRITE's threads sleep and
  wake about 8 times.

Every figure is a cost per 1 MiB WRITE: ring softirq microseconds a WRITE
(`4 x soft% / WRITEs a second`, from the run's `srv-mpstat.txt` and
`window.txt`), a frame's microseconds a WRITE from the profile (`top-*.txt`),
and nfsd's wakes and voluntary context switches a WRITE (`pool_stats` and
`/proc/<pid>/status` read around the run).

**How a profile becomes microseconds.** The cycle counter does not stop in
an idle CPU on this machine (CPU 1, 95% idle, has as many samples as CPU 0
at 90% softirq), so neither samples nor cycles are busy time. `flame.sh`
weights each stack by its cycles, leaves out the idle ones (`do_idle` with
no interrupt on it), and turns a frame's share of what is left into
microseconds a WRITE with mpstat's busy time (100 - idle - iowait) for the
same CPUs over the run's window. On the rings that gives 92.0 us a WRITE,
against 91.9 of softirq alone from the same run.

## Two yardsticks: CPU a WRITE, and latency (Mike, 2026-10-23: "latency is going to matter a lot on a per IO level")

- **CPU a WRITE at saturation:** the one-port loan64 run, as above.
- **Latency at low depth:** `xeu-rig/lat.sh OUTDIR TAG`. It runs 4 writers, one a drive, at iodepth 1 with 1 MiB writes, so the drives are far from saturation and what is measured is the server's own time an I/O.
  - The untraced run gives fio's completion p50, p90, p99, p99.9 and mean.
  - A run with `lat.bt` gives the server's split: the wait for a thread (`svc_xprt_dequeue`'s wakeup and qtime), `svc_process`, the write into the file system, and the reply (`svc_tcp_sendto`).
- **Why they differ:** at saturation, latency is the drives' queue (Little's law), so CPU savings cannot show in it.
  - A saving on the critical path is both a CPU saving and a latency saving.
  - Work moved off the critical path, or reordered, changes latency alone. For example, the loan is given back before the reply is sent, so every reply waits for it.
  - Waiting (for a thread, for the rest of a record, for credit) costs latency and no CPU.
- **Each round reports both yardsticks.**

## The method

1. **Profile the one-port loan64 run as it stands, untraced except for perf.**
   `xeu-rig/flame.sh OUTDIR TAG [WRITES]` runs the battery's run (port a, the
   four drives, `WARM=10 RUNTIME=30`, which keeps it inside the drives' fast
   region, after a trim and 120 s of rest) and records every CPU for 8 s at
   999 Hz with call chains inside twoport.sh's 10 s window. It draws the
   flamegraphs with Brendan Gregg's FlameGraph (installed in `/opt/FlameGraph`
   from github.com/brendangregg/FlameGraph, commit `41fee1f`; `flamegraph.pl`
   needs `perl-open`): one for the ring CPUs (0, 2, 4, 6), one for nfsd's
   (8-55), one for all, each with its `.folded` stacks (`difffolded.pl`
   compares two rounds), and ranks each group's frames, self and with what
   they call (`top-rings.txt`, `top-nfsd.txt`). `flame.sh report DIR` makes
   the reports again from a round's `perf.data`.
2. **Rank by share of each group's busy cycles**, as microseconds a WRITE
   (`top-*.txt`). Read the self cost (where the
   cycles are spent) and the inclusive cost (what a function and what it calls
   take) together: a wide frame low in the graph is a path, a wide frame at the
   top is a function to make cheaper or call less often.
3. **One change at a time, then an interleaved A/B**, as for the one-wake
   fix: at least three runs each way, A and B alternating, every run audited by
   `settled.sh` and its `drives.txt` read (no drive stepped down). Fit ring
   (and nfsd) microseconds a WRITE against the refusals a loaned WRITE, with one
   slope for both and a term for the change: refusals vary from 0.2 to 0.96
   between runs of the same setting, and that alone moves ring cost by as much
   as most single gains. A change is kept if its term is beyond its standard
   error; each kept change is its own code commit at the tip (one group to a
   commit, an upstream-style message, Signed-off-by only).
4. **Profile again after each kept change**: the ranking moves.

A sunrpc or nfsd change is tested by reloading the module
(`xeu-rig/swap-sunrpc.sh`), which drops every NFS client's connections,
tardis1's included: that is Mike's to ask for, each time.

## Rules a round keeps

- Enterprise-storage mode: the drives `write through`, the export `sync`.
- `./settled.sh apply` ends in "the settled setting" before a run, and
  `flame.sh` refuses to run off it. Never get past a BAD item by hand.
- The client is shared with tardis1: `client_free` first (flame.sh does).
- The profile costs the rings a little of their rate (perf at 999 Hz on 64
  CPUs): take a round's run figures from untraced runs, and its shares from
  the profile.
- Without pseudo-NMI (not on tardis2's command line), a sample taken with
  interrupts off lands where they are turned on again: a wide
  `_raw_spin_unlock_irqrestore` or `handle_softirqs` self cost belongs to
  what ran before it. Read its callers before ranking it.
- A change of a few us a WRITE on nfsd is below what mpstat resolves over
  48 CPUs: compare the function's cost in the profiles before and after,
  and use the A/B to show nothing else moved (round 02).
- Annotate a module against the build that was profiled (`perf
  buildid-list -i perf.data`); a later reload leaves a different
  `sunrpc.ko` in `updates/`. The kernel's own text is in `/proc/kcore`
  (no vmlinux with symbols on tardis2).
- A function under two parents is counted once: read its callers before
  adding it to a ranking (round 01 counted `napi_consume_skb` under
  `skb_defer_free_flush` as a second item).
- A driver's counters are not wire frames until shown to be: count with
  TCP's own (`nstat TcpInSegs TcpOutSegs`).
- Per-run counters of the port (interrupts, page_pool) start again when
  the port opens: read them after the run, not as a difference.
- A change that holds pages longer or returns them later also reports
  the region's peak pages in use (Mike: keep the L3 hot; a buffer's
  reuse is LIFO, so returning it later makes the ring reuse colder ones).
- **Do not grow the region's footprint (Mike, 25 October: "we cannot
  afford to increase the region").** A change that raises the region's
  peak pages in use is not kept, whatever it saves; round 04's deeper
  stack (rings -3 us a WRITE for 940 more pages) is the case. AGENTS.md
  has the rule.
- Name the change by what it saves a WRITE, measured, and say what it did
  not change (the rate, nfsd's sleeps).

## A round's directory

`NN-name/README.md`: what was profiled (kernel, sunrpc srcversion, the run's
figures), the ranking, what was picked and why, the change (its commit), the
A/B and its fit, and the outcome. The flamegraphs (`rings.svg`, `nfsd.svg`)
and the `self-*.txt` and `incl-*.txt` reports are kept beside it; the raw
`perf.data` and the run directories stay in `/var/tmp/zc-run-logs/`.
