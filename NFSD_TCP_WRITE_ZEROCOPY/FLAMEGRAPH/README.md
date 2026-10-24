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
| 04 | | the region pools' 256-page stack overflows: 7% of pages back to the region, about 5 us a WRITE | `dc23eac70f34`: `page_pool.rx_region_stack` | waits for the next kernel build (Mike) |
| [05-data-ready](05-data-ready/README.md) | the same, with round 03 | `svc_data_ready`: the `xpt_flags` line, two clock reads, a store into another thread's `svc_rqst` | `eff5f16d4f30`, `0f0ed4159a12` | kept: rings 68.4 to 65.0 us a loaned WRITE (-5%), nfsd 159.7 to 156.4 |
| [06-loan-lowat](06-loan-lowat/README.md) | the same, with round 05 | a WRITE taken up 12 times: wakes, hand-offs, ACKs (about 50 us on nfsd) | `c8cf27841351`: a low-water mark while a loaned record is incomplete | not kept, reverted (`b8c9ee7c6d2a`): nfsd 154.5 to 121.0 us a WRITE, but TCP ACKs every skb below the mark, so the rings 67.0 to 85.0 |
| 10 | | the same churn (06b) | `svc_data_ready` holds `XPT_DATA` back for a short loaned record; `sk_rcvlowat` stays 1 | after round 09 (Mike) |
| 07 | | half of each ACK and reply is the qdisc (fq_codel, its lock) | the server's qdisc `pfifo_fast` | |
| 08 | | the `zc_notrack` chains cost 3-4 us a WRITE with no conntrack to skip | load `zc_notrack` only while a table needs conntrack | |
| 09 | | per-skb costs at about 19 GRO skbs a WRITE | BIG TCP: `gro_ipv4_max_size` 256 KB | |
| [L0-latency](L0-latency/README.md) | round 05's state, 4 in flight | p50 0.46-0.52 ms; about 0.2 ms is the drive, 0.02 the reply; 2.5% of wakes take 128-512 us | `xeu-rig/lat.sh` | the baseline; runs differ by about 10%, so several a side |

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
- Name the change by what it saves a WRITE, measured, and say what it did
  not change (the rate, nfsd's sleeps).

## A round's directory

`NN-name/README.md`: what was profiled (kernel, sunrpc srcversion, the run's
figures), the ranking, what was picked and why, the change (its commit), the
A/B and its fit, and the outcome. The flamegraphs (`rings.svg`, `nfsd.svg`)
and the `self-*.txt` and `incl-*.txt` reports are kept beside it; the raw
`perf.data` and the run directories stay in `/var/tmp/zc-run-logs/`.
