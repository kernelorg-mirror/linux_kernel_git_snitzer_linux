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

## The runs

- **Start** ([run78.sh](run78.sh), on build 494): four untraced loan64 runs at round 01's state. Each run is set up as every later round's runs are: one port, the four DPRP5s, `WARM=10 RUNTIME=30`, 64 WRITEs of budget, a trim and 120 s of rest first.
- **Now** ([run79.sh](run79.sh), back on build 499): four runs at the current state, the same day, so the two ends are not separated by the drift between sessions.
- **Reported:** each run in the table the rounds use (GB/s, rings and nfsd µs tick-sampled, ring and nfsd cycles and instructions a WRITE, peak), the two ends set against each other, and round 01's tick-sampled µs (92.0 rings, 184.1 nfsd) against the replay's.

## The reboot

A reboot kills this Claude session. Its transcript is on the root file system, so `claude --resume` picks it up after the reboot; this README is the plan if it does not.

1. **Before (on 499):** no NFS client connected and no NFS mounts on the host; the client's other mounts are left alone. Nothing is changed on 499.
2. **Into 494, once:** `bootctl set-oneshot ofp-rocky-7.3.0-7.hs.494.loanpage_pool.el10.aarch64+16k.conf && systemctl reboot`. The default entry stays 499 with `rodata=noalias`, so any later boot, a hang and power cycle included, comes back to 499.
3. **On 494:**
   - check `uname -r`, that `/proc/cmdline` has no `rodata=`, and that `getconf PAGESIZE` is 16384;
   - `for n in 0 1 2 3 4 5 6 7; do mount -o noatime /dev/nvme${n}n1 /hs/nvme$n; done` and `systemctl start nfs-server`;
   - in `xeu-rig/`: `./bringup.sh 32768 256`, then `EXP_RODATA=default EXP_DRC_MAX_ENTRIES=0 EXP_XEU_irq_msix_mask=absent EXP_XEU_rx_prefetch=absent ./settled.sh apply`. It must end in the settled setting with those named off. A BAD item stops the plan: it is looked at, not got past by hand.
   - then `../FLAMEGRAPH/37-counted-start/run78.sh`. It swaps in round 01's sunrpc and 494's own nfsd, and stops on anything off.
4. **Back to 499:** `systemctl reboot`. Then the same mounts, `nfs-server`, `./bringup.sh 32768 256` and `./settled.sh apply`, which must end in "the settled setting"; then `run79.sh`.

**What 494 is left with:** its `updates/` holds round 01's sunrpc in place of round 17's, and no nfsd. Both of the earlier modules are kept in `t2-tipkernel/` (`r17-sunrpc.ko`, `821534936DB054AC699BF52`, and `r13b-nfsd.ko`). Nothing of 499's is changed.

## Not in this plan: the steps between

The two ends give the progress in counted terms, not each round's share. Most of the steps could be counted on the same 494 boot, from modules that are still on disk:
- the settings: round 01's table and untracking, round 03's `skb_defer_max` 0;
- the sunrpc builds in `t2-tipkernel/` (`r5`, `r10w`, `r14`, `r17`);
- the later rounds need 499: `rodata`, page_pool, and xeu 2.0.1-9 to -12.

That is a run of eight or so states at two runs each, about an hour and a half. It is Mike's call whether to add it.
