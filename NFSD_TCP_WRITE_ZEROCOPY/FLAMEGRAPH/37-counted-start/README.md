# Round 37: round 01's starting point, counted

**The plan (Mike, 28 October):** measure round 01's starting point again with the counted method that round 35 brought in: every CPU's cycles and instructions over each run's window, from `perf stat -a -A` in `twoport.sh` and given a WRITE by `cost.sh`. Then set the start against the current state, counted the same way on the same day, to give the progress since round 01 in counted cycles as well as in tick-sampled µs. Round 38 is then the release's struct page misses (round 36).

## Round 01's starting point

Round 01 was profiled on 23 October ([01-loan64-baseline](../01-loan64-baseline/README.md)), at these points:

| | round 01 | now |
|---|---|---|
| kernel | build 494 (`7.3.0-7.hs.494.loanpage_pool.el10.aarch64+16k`), from the tip of 22 October, `77f82c816493` | build 499 (`bca1a2f143eb`) |
| `rodata` | `full` (no `rodata=`, the default) | `noalias` (round 30) |
| sunrpc | `82ACAC76B5F85A2645B58A4`: `77f82c816493` with "SUNRPC: Wake an idle service thread only once" (`2fc9cd2e839b`) | `E80412A536ADD1E86B16F5A` (round 36) |
| nfsd | build 494's own (`EF2C425552D548DD099D0B3`) | build 499's own (`3F0C341E86E266D5FDF2ADF`) |
| xeu | build 494's own (`E0A4EBC4A7138D7BA4A90DF`): no `irq_msix_mask`, no `rx_prefetch`, the frag API's drain, the long refill loop | `63631721F8B149913C0DC19` (kmod-xsight 2.0.1-12) |
| netfilter | podman's leftover `inet netavark` table, 10 conntrack hooks, no `zc_notrack` | no table, untracked (round 01) |
| `skb_defer_max` | 128 | 0 (round 03) |
| reply cache | nfsd's own size | 4096 (round 12) |
| the window share | half (no `svc_tcp_rx_want_eighths`) | 6 eighths (round 25) |

- **The sunrpc:** `t2-tipkernel/fix-sunrpc.ko` is the module round 01 ran. A rebuild from `2fc9cd2e839b`'s `net/sunrpc` and headers against build 494's `-devel` (28 October, `$L/t2-r37-sunrpc-r01/`) gives the same srcversion, `82ACAC76B5F85A2645B58A4`. The rebuild has no BTF, because `-devel` carries no vmlinux and the running kernel's is 499's, so the run loads the original, which has 494's.
- **The nfsd:** build 494's own is the tree at `77f82c816493`. `updates/nfsd.ko` on 494 is round 13's (`8EF3B4156F6AAF38EC5CD9E`, 25 October), later than round 01; `swap-sunrpc.sh NFSD_KO=none` takes it out. A copy is kept as `t2-tipkernel/r13b-nfsd.ko`.
- **The xeu:** a boot of 494 loads its own `extra/xsight/xeu.ko`, which its initrd carries too. 494 has no `updates/xeu.ko`. That xeu has none of rounds 20 and 28's parameters, so it cannot run them by mistake.
- **The netavark table** is in `t2-tipkernel/netavark-table.nft`, as round 01's own A/B (`run18.sh`) put it back.

## How the runs are held to it

- **`settled.sh`** takes round 01's state as settings off the settled one, on purpose, through its `EXP_` variables:
  - `EXP_RODATA=default`;
  - `EXP_DRC_MAX_ENTRIES=0` (494's nfsd has no such parameter, so it is not audited);
  - `EXP_XEU_irq_msix_mask=absent` and `EXP_XEU_rx_prefetch=absent`. These are new: `absent` holds a run to an xeu without that parameter.
  - `svc_tcp_rx_want_eighths` and the region stack are absent from 494's sunrpc and page_pool, which the audit already allows.
- **`twoport.sh`** sets `SKB_DEFER_MAX=128` and `NOTRACK=absent` for each run, and passes them to its own audits.
- **The driver** puts the netavark table back after each `settled.sh apply`, which deletes it.
- **`EXP_NETAVARK=present`** (added on 494, 28 October) holds the audit to the netavark table with no container running. `twoport.sh` audits the host at rest before it changes anything, and that audit sees only what is exported, so the driver exports it and `EXP_NOTRACK=absent` to `twoport.sh`. The first launch, without them, was refused at that audit before any load (kept in `t2-r78/refused-1/`).
- **`twoport.sh`'s audit after each run** reports `zc_notrack` back (its cleanup loads it while conntrack is on) and `skb_defer_max` 128 as BAD. Both are the run's own settings, reported after its window; the driver's next `settled.sh apply` puts them back.

## The runs

- **Start** ([run78.sh](run78.sh), on build 494): four untraced loan64 runs at round 01's state. Each run is set up as every later round's runs are: one port, the four DPRP5s, `WARM=10 RUNTIME=30`, 64 WRITEs of budget, a trim and 120 s of rest first.
- **Now:** round 36's own counts at the current state (build 499, the same modules), not counted again. `run79.sh` was to count it a second time on the same day; it was stopped after its first run (Mike: round 36 already tested this state, and the drift between sessions is a couple of percent against the change).
- **A profile** ([run80.sh](run80.sh), on build 499): one loan64 run with `twoport.sh`'s `PROFILE=1`, for round 38 to start from.
- **Reported:** each run in the table the rounds use (GB/s, rings and nfsd µs tick-sampled, ring and nfsd cycles and instructions a WRITE, peak), the two ends set against each other, and round 01's tick-sampled µs (92.0 rings, 184.1 nfsd) against the replay's.

## The start, counted (build 494, 28 October)

[run78.sh](run78.sh), four untraced loan64 runs at round 01's state, from 06:13 to 06:29. Runs are in `/var/tmp/zc-run-logs/t2-r78/`. Every run had no drive stepping down and no WARNING in dmesg. The end was the settled setting, with the four exceptions the driver names. All lines are in [cost-r78.txt](cost-r78.txt).

| run | GB/s | rings µs (tick) | ring cycles / instructions a WRITE | nfsd µs (tick) | nfsd cycles / instructions | peak |
|---|---|---|---|---|---|---|
| r78-1 | 40.35 | 91.1 | 191.9k / 339.6k | 176.2 | 477.5k / 451.3k | 6372 |
| r78-2 | 40.34 | 91.7 | 191.8k / 339.4k | 182.4 | 479.6k / 449.3k | 6542 |
| r78-3 | 40.29 | 90.9 | 193.7k / 340.8k | 181.0 | 484.1k / 455.3k | 6527 |
| r78-4 | 40.33 | 93.4 | 192.6k / 339.2k | 181.9 | 484.2k / 454.7k | 6591 |
| **mean** | **40.33** | **91.8** | **192.5k / 339.8k** | **180.4** | **481.4k / 452.7k** | **6508** |

- **The replay is round 01's start.** Its tick-sampled µs (91.8 rings, 180.4 nfsd) are within the spread of round 01's own (92.0 and 184.1).
- **The loans:** about 1.54M admitted a run and 0.85M to 1.06M denied, at 64 WRITEs of budget.
- **The counted start:** 192.5k cycles a WRITE on the rings and 481.4k on nfsd. The cycles-per-instruction ratio is 0.57 on the rings and 1.06 on nfsd.
- **For the other end:** round 36's four A runs on 499 counted about 137.6k ring cycles and 295k nfsd cycles a WRITE. They are the current state's figures, set against the start below.

## Since round 01, counted (28 October)

| cycles a WRITE | round 01's start (494) | now (499, round 36) | change |
|---|---|---|---|
| rings | 192.5k | 137.9k | −28% |
| nfsd | 481.4k | 294.6k | −39% |

- **Now** is round 36's counts: its A/B on 499 with the tip's sunrpc (`E80412A536ADD1E86B16F5A`), nfsd 499's own and xeu round 35's (round 35's B for the rings).
- **`run79.sh`'s one run** (`$L/t2-r79/r79-1-loan64-now`, before it was stopped) agrees: 40.51 GB/s, rings 139.1k / 310.5k and nfsd 299.8k / 301.4k cycles / instructions a WRITE, 44.3 and 90.1 µs tick-sampled, no drive stepped down, peak 6438.
- **The rate** is the drives' 40.3 to 40.5 GB/s at both ends. Both are loan64 on port a onto the four DPRP5s.

## The profile for round 38 (run80, 28 October)

[run80.sh](run80.sh), one loan64 run with `PROFILE=1` and `RUNTIME=55`: 40.44 GB/s, no drive stepped down, no WARNING, peak 6344, the settled setting at the end. Its counts over the window (rings 140.0k, nfsd 300.3k cycles a WRITE) are taken while perf runs and are not part of the table above. Runs are in `$L/t2-r80/`, perf data kept. Kept here: `perf-sym-*-r80.txt` (cycles by symbol, nfsd's CPUs 8-55 and the rings' 0-7,56-63), `perf-lmiss-*-r80.txt` (L3 read misses by symbol) and `perf-stat-*-r80.txt`.

- **L3 read misses a WRITE:** about 7.3k on nfsd's CPUs and 2.9k on the rings' (8 s of `perf stat`, at 38.5k WRITEs a second).
- **nfsd's misses, past idle and the scheduler:** `svc_tcp_rx_release_refs` 2.70%, `svc_tcp_rx_region_span` 1.99%, `svc_tcp_rx_skb_region_pages_after` 1.79%, `page_pool_put_netmem_bulk` 1.52%, `svc_tcp_rx_append_ref` 1.35%, `svc_tcp_rx_prefetch_pages` 1.14%. The release leads, as round 36 found. The region's span and pages-after tests are close behind and were not in round 38's scope.
- **nfsd's cycles:** idle 28%, `finish_task_switch` 7.4%, `_raw_spin_unlock_irqrestore` 2.6%; the loan frames 1.1 to 1.6% each.
- **The rings' misses** are GRO's (`inet_gro_receive`, `tcp_gro_receive`, `tcp4_gro_receive`, `__gro_receive_network_flush`, `gro_list_prepare`, `skb_gro_receive`, about 40% between them) and xeu's refill (7.65%) and poll (5.39%).
- **For round 38:** the release's struct page misses stay first. Its scope could take in `svc_tcp_rx_region_span` and `svc_tcp_rx_skb_region_pages_after`, whose misses together are larger than the release's.

## The reboot

A reboot kills this Claude session. Its transcript is on the root file system, so `claude --resume` picks it up after the reboot; this README is the plan if it does not.

1. **Before (on 499):** no NFS client connected and no NFS mounts on the host; the client's other mounts are left alone. Nothing is changed on 499.
2. **Into 494, once:** `bootctl set-oneshot ofp-rocky-7.3.0-7.hs.494.loanpage_pool.el10.aarch64+16k.conf && systemctl reboot`. The default entry stays 499 with `rodata=noalias`, so any later boot, a hang and power cycle included, comes back to 499.
3. **On 494:**
   - check `uname -r`, that `/proc/cmdline` has no `rodata=`, and that `getconf PAGESIZE` is 16384;
   - `for n in 0 1 2 3 4 5 6 7; do mount -o noatime /dev/nvme${n}n1 /hs/nvme$n; done` and `systemctl start nfs-server`;
   - in `xeu-rig/`: `./bringup.sh 32768 256`, then `EXP_RODATA=default EXP_DRC_MAX_ENTRIES=0 EXP_XEU_irq_msix_mask=absent EXP_XEU_rx_prefetch=absent ./settled.sh apply`. It must end in the settled setting with those named off. A BAD item stops the plan: it is looked at, not got past by hand.
   - then `../FLAMEGRAPH/37-counted-start/run78.sh`. It swaps in round 01's sunrpc and 494's own nfsd, and stops on anything off.
4. **Back to 499:** `systemctl reboot`. Then the same mounts, `nfs-server`, `./bringup.sh 32768 256` and `./settled.sh apply`, which must end in "the settled setting"; then `run80.sh`.

**What 494 is left with:** its `updates/` holds round 01's sunrpc in place of round 17's, and no nfsd. Both of the earlier modules are kept in `t2-tipkernel/` (`r17-sunrpc.ko`, `821534936DB054AC699BF52`, and `r13b-nfsd.ko`). Nothing of 499's is changed.

## Not in this plan: the steps between

The two ends give the progress in counted terms, not each round's share. Most of the steps could be counted on the same 494 boot, from modules that are still on disk:
- the settings: round 01's table and untracking, round 03's `skb_defer_max` 0;
- the sunrpc builds in `t2-tipkernel/` (`r5`, `r10w`, `r14`, `r17`);
- the later rounds need 499: `rodata`, page_pool, and xeu 2.0.1-9 to -12.

That is a run of eight or so states at two runs each, about an hour and a half. It is Mike's call whether to add it.
