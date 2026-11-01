# Round 49: nfsd on fewer CPUs

**The ask (Mike, 31 October):** after round 48, is anything to be done about interrupts to idle CPUs? The first thing to try: nfsd's work is about 3.4 CPUs' worth spread over 48. Each NVMe queue is one CPU's, so the interrupts follow the threads. Run on fewer CPUs and see.

tardis2, build 505, sunrpc `666175BED7A343E376C1742`, one-port loan64 at the settled setting, with `twoport.sh`'s `NFSD_CPUS`: 512 threads on A 8-55 (48), B 8-23 (16), C 8-15 (8). All three hold 8-11, port a's XPS CPUs. Copy was not run. **This boot has pseudo-NMI on (round 48), on every side**, so the sides compare with each other and not with another boot's figures.

The first try stopped at its second run: `settled.sh place` undid `NFSD_CPUS`, and the audit refused the run. `twoport.sh` now pins after placing. `run126-stopped.out` is that try; `run126.sh` and `run126.out` the run, logs in `$L/t2-r126b/`.

## At saturation (run126)

A B C C B A C A B, untraced.

| | runs that count | GB/s | ring cycles a WRITE | nfsd cycles (instructions) a WRITE | context switches a WRITE | peak | mean in use |
|---|---|---|---|---|---|---|---|
| A, 48 CPUs | 3 | 40.43 to 40.49 | 130.4k | 292.9k (300.1k) | 11.4 | 5967 to 6225 | 4935 |
| B, 16 CPUs | 2 | 40.47 | 130.3k | **266.5k (297.8k)** | 11.2 | 6064 to 6111 | 4904 |
| C, 8 CPUs | 2 | 40.42 to 40.55 | 129.7k | **246.5k (294.7k)** | 10.2 | 6026 to 6316 | 5001 |

Runs 4 (C) and 5 (B) dipped together: 39.64 and 39.76 GB/s, 36.2k and 36.6k WRITEs/s in the window, the wire falling to about 36 GB/s for a moment, one ring at 32 to 34% where the others hold 25%. Neither drive nor stall lines show anything and the cause was not looked for further. They are left out of the means. Their instructions match their sides.

NVMe interrupts are **4.08 to 4.09 a WRITE on all three sides**, and IPIs 4.7 to 4.9, 4.6 on C. Unused CPUs add almost nothing: CPUs 24-55 count 0.2 to 0.4 G cycles in 10 s when no thread is on them.

## What the fall in cycles is

**Nfsd's counted cycles fell 9% and 16%, but most of that is not work:** instructions fell 0.8% and 1.8%, and mpstat's busy time did not fall. One profiled run a side (fl48 on 48 CPUs, [fl49b-16cpu/](fl49b-16cpu/), [fl49-8cpu/](fl49-8cpu/)), stacks' weights over the WRITEs in the 8 s, k cycles a WRITE, as round 48 reads them:

| | 48 CPUs | 16 CPUs | 8 CPUs |
|---|---|---|---|
| threads | 200.2 | **192.7** (-3.7%) | **187.5** (-6.3%) |
| hard interrupts | 27.8 | 27.5 | 26.6 |
| of them on an idle CPU | 26.7 (96%) | 23.3 (85%) | 18.4 (69%) |
| `nvme_irq` | 11.4 | 12.5 | 12.9 |
| IPIs | 6.5 | 7.0 | 6.9 |
| the idle task with no interrupt | 69.2 | 49.1 | 33.0 |
| of it `cpu_do_idle` | 52.7 | 34.7 | 20.5 |
| all | 297.1 | 269.3 | 247.2 |

- **Of the 49.9k fall on 8 CPUs, 36.2k is idle, 12.7k threads, 1.2k hard interrupts.** On 16: 20.1k, 7.5k, 0.3k.
- **The saving in work is the threads': 3.7% on 16 CPUs, 6.3% on 8,** from one profile a side. It is spread, with no frame standing out: `svc_send` -2.5k and `nfsd3_proc_write` -1.8k on 8 CPUs, with `svc_tcp_rx_append_ref`, `svc_tcp_rx_walk_skb`, `svc_tcp_rx_borrow_segment`, `__page_pool_rx_region_put` each -0.2 to -0.4k. It fits work staying warmer on fewer CPUs; that is a reading, no cache event was counted.
- **The interrupts are not cheaper.** The expectation that a completion would find its CPU awake and share an interrupt was half right: more land on a running thread (31% on 8 CPUs against 4%), but the count does not move, 4.08 a WRITE, and the total is within 1.2k. `nvme_irq` is a little dearer. So this does not answer the question it came from: it moves interrupts from idle CPUs onto work, at the same cost.
- **`sched_balance_newidle` barely moves:** 3.81k to 3.42k on 8 CPUs.

## Latency (lat.sh, iodepth 1, 1 MiB)

Untraced, A B C C B A:

| | run | GB/s | p50, ms | p99, ms | mean, ms |
|---|---|---|---|---|---|
| 48 CPUs | 1, 6 | 8.34, 7.74 | 0.420, 0.457 | 0.553, 0.578 | 0.432, 0.458 |
| 16 CPUs | 2, 5 | 8.35, 8.52 | 0.416, 0.420 | 0.545, 0.528 | 0.429, 0.426 |
| 8 CPUs | 3, 4 | 8.46, 8.55 | 0.420, 0.420 | 0.537, 0.528 | 0.426, 0.426 |

No worse on fewer CPUs. Run 6's 0.457 is A's traced partner's 0.428, so it is scatter, not a difference. Saturation's completion latency is the same on all sides (51.4 to 51.7 ms mean).

## What it leaves

- **The region:** 16 CPUs does not raise it. 8 CPUs may: its peaks are 6204, 6316, 6026 and 6296 in fl49, against 48 CPUs' 5967 to 6225, its mean in use 1.3% up. Inside the scatter, but on the wrong side of it, and fl49b on 16 CPUs had 6343 in a profiled run, so a peak under a profile is not the measure.
- **Room:** by counted cycles the CPUs in use run at about 59% of 2 GHz on 8 CPUs and 32% on 16. These are one port's load at the drives' limit. Two ports were not run, and two ports carry about 1.5 times the WRITEs: 8 CPUs would not be a setting to run them on without a test.
- **Boot:** run with pseudo-NMI on; a boot without it should repeat the A/B before anything is settled.

**Nothing changed in `settled.sh`:** `NFSD_CPUSET` stays 8-55, and the host was left there at the settled setting. Hammerspace's own layout puts threads on 8-55 too, which AGENTS.md's reconciliation note covers.

## The repeat, on a boot without pseudo-NMI

**Mike, 1 November: take pseudo-NMI off, reboot, repeat the A/B.** The parameter is off `/etc/kernel/cmdline` and build 505's entry, restored from round 48's kept copies. After the boot:

1. `grep gicv3_pseudo_nmi /proc/cmdline` finds nothing, and `rodata=noalias` is there.
2. `for n in 0 1 2 3 4 5 6 7; do mount -o noatime,lazytime /dev/nvme${n}n1 /hs/nvme$n; done`, `systemctl start nfs-server`, `./bringup.sh 32768 256`, `./settled.sh apply`, which must end in "the settled setting". Check that no NFS client is connected first.
3. `./run127.sh`: the same A B C C B A C A B and lat.sh A B C C B A, about 90 minutes, logs in `$L/t2-r127/`, refused on a pseudo-NMI boot. No profiles: without pseudo-NMI they cannot split hard interrupts from idle.
4. Read it for what was in doubt: the instructions a WRITE, and how the counted cycles fall beside round 48's split.
