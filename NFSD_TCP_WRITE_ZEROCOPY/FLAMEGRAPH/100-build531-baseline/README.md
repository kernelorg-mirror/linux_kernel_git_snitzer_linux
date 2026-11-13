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
