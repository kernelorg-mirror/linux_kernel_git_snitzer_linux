# Results across page sizes

One record per test host, in the same form, so that 4 KiB, 16 KiB and
64 KiB can be read side by side. The tables below are generated; the
record for each page size is `results/<page>.tsv`.

**State: all three page sizes are complete: 16 KiB and 4 KiB on
2026-10-05, 64 KiB (tardis1) on 2026-10-07.** The 64 KiB column is of a
later kernel than the other two (`v7.1.13-20` and four fixes found on
tardis1; "What the comparison says" below), so the three columns are not
one code base. The 4 KiB column replaces the older
4 KiB results (`../slipstream/README.md` section 8,
`../slipstream/BUDGET_SCOPE.md` section 8), which were of an older
harness: nvme-loop only, no copy twins, and a test storage ten times
slower than asked. Those are not comparable with what is below.

A status page for readers outside the tree, with the terms, the two kinds
of storage and the 16 KiB results, is "NFSD Receive Region Budget":
https://claude.ai/artifact/3UQkMVoWW8p9A5ZASPnm3g (Mike's, shared by link).
The three earlier pages of the series point to it. Update it when a host's
record lands.

## How a host adds its column

When `run-testplan.sh` has completed on a host (every stage PASS or DONE,
including the second boot):

```sh
cd NFSD_TCP_WRITE_ZEROCOPY/transplant
./results-table.py extract /var/tmp/zc-testplan-<release> > results/4KiB.tsv     # or 64KiB.tsv
./results-table.py compare results/*.tsv                                          # the tables below
```

Commit the `.tsv`, replace everything under "The comparison" with the new
`compare` output, and update "What the comparison says". The run's own
directory stays on the host; the `.tsv` is what travels.

## Reading it

- **Two kinds of storage.** "Needs whole pages" is nvme-loop, standing in
  for NVMe without SGLs (tardis1's kind). "Takes arbitrary pieces" is
  NVMe/TCP, standing in for NVMe with SGLs, with veth placing the payload
  on a 4-byte boundary (`rx_ip_align=2`).
- **Copy and loan.** A WRITE is either *copied* out of the receive pages
  into nfsd's own buffer, or the receive pages are *loaned* to the file
  system. A copy is always written direct. A loan is written direct only
  on storage that takes arbitrary pieces; elsewhere it is written through
  the page cache.
- **Budget.** The limit, in pages, on how much of the fixed receive region
  loans may hold. A budget of 0 means copy. What one WRITE draws against
  the budget is its *credit*.
- **Cell names.** `-on` loans (budget set, receive buffer held). `-copy`
  is its twin that copies (budget 0, everything else the same). `-off` has
  neither budget nor held buffer: it shows what happens without the
  mechanism and is not comparable with the other two. `-sized-` cells
  write files that already have their size.
- **Region use is in MiB**, not pages, so it compares across page sizes.
  The region itself is 16 MiB at 4 KiB, 24 MiB at 16 KiB and 35 MiB at
  64 KiB in phase A, and its configured minimum in phase B.
- **What to compare first:** drops and allocation failures with a budget
  set (must be 0 everywhere); how each cell's WRITEs were issued (must
  match the kind of storage); then copy against loan in the "sized" pairs.
- **What not to compare:** MB/s between kinds of storage as if it measured
  the loan (NVMe/TCP is the slower way to the same test device), and the
  new-file pairs as copy against loan (a direct write to a growing file
  waits on XFS). All of it is a VM with memory for storage: it shows
  whether the mechanism holds, not what it is worth on hardware.

## What the comparison says

**Copying leaves the receive region nearly empty; loaning ties most of it
up.** The most of the region in use at any moment, across the four pairs
of cells that differ only in copy or loan (`seq-sized`, `mixed-sized`,
`step`, `step-n16`), on both kinds of storage:

| page size | region | a copy holds | a loan holds |
|---|---|---|---|
| 4 KiB | 16 MiB | 0.25 to 1.75 MiB (2 to 11%) | 8.3 to 9.6 MiB (52 to 60%) |
| 16 KiB | 24 MiB | 0.25 to 1.75 MiB (1 to 7%) | 15.1 to 16.7 MiB (64 to 70%) |
| 64 KiB | 35 MiB | 0.50 to 1.50 MiB (1 to 4%) | 11.3 to 19.8 MiB (32 to 57%) |

A host fills in its row from its record: the smallest and largest
`used_MiB` of those eight `-copy` cells and of those eight `-on` cells in
phase A, against `region/prp/A/MiB`. The status page on claude.ai leads
with the same table.

4 KiB and 16 KiB (`README.md`, "4 KiB host" and "16 KiB host", have the
accounts) give the same answers:

- The plan passes on both kinds of storage at both page sizes: no verify
  failure, hang or kernel warning.
- With a budget set and the receive buffer held, nothing is dropped and
  the region never runs out, whichever way the loan is written.
- On storage that takes arbitrary pieces every loaned WRITE is written
  direct, at 4 KiB too: veth's frame layout there (head plus two page
  fragments at MTU 9000) still gives 4-byte aligned loans with
  `rx_ip_align=2`.
- Copy and loan are level in throughput on this rig. A copy holds 0.25 to
  1.8 MiB of the region; a loan about 8 to 10 MiB of 16 MiB at 4 KiB and
  15 to 17 MiB of 24 MiB at 16 KiB.
- `mtu1500-on` is copied at both page sizes.

Throughput is not compared across the two hosts: they are different
machines.

`n-reconf` in both columns is from a re-run of phase N with the harness
fix that keeps its load running through the budget changes (`README.md`,
"4 KiB host", "Problem found and fixed"). On both hosts and both kinds of
storage the changes then landed with records in flight, the cell ended
with no waiters and the pool whole, and fio verified the data.

**64 KiB (tardis1, 2026-10-07; `README.md`, "64 KiB host", has the
account) gives the same answers on safety and one different answer on
throughput:**

- The plan passes on both kinds of storage: every stage PASS or DONE,
  `analyze.py` OVERALL PASS, no verify failure, hang or kernel warning.
- With a budget set and the receive buffer held, nothing is dropped and
  the region never runs out. No cell overran its credit: the factor of
  150, computed and until now unmeasured, is not low.
- On storage that takes arbitrary pieces every loaned WRITE is written
  direct; on storage that needs whole pages every loan goes through the
  page cache and every copy is direct. `mtu1500-on` is copied.
- **On one connection copy and loan are level** (`seq-sized` 2174 / 2191
  and 2174 / 2165 MB/s, `step` 1315 / 1323 and 1303 / 1323).
- **On sixteen connections the loan is well behind its copy, which it
  is not at 4 KiB or 16 KiB:** `mixed-sized` 2594 / 1236 and 2521 / 1142
  MB/s, `step-n16` 1912 / 1229 and 1846 / 1009. Those loan cells pause
  (about 1000 times in the `mixed` cells, about 95000 to 100000 in the
  `step` ones); no 4 KiB or 16 KiB cell pauses. The region holds 16 to
  18 MiB of 35 MiB at the time, one 17-page record or so per connection.
  The cause is not established; the host has 64 CPUs, far more than the other two,
  and its 16-connection copies run about three times as fast as theirs.
- **The cells with no budget and no held buffer (`-off`) do not drop at
  64 KiB** except with two clients (`x-off-RN`, 4137 and 3914 drops):
  `mixed-off`, `x-off-R` and `d-off-N` drop thousands of packets at
  4 KiB and 16 KiB and none here, holding 2 to 3 MiB of the region. Not
  examined.
- **LOCALIO reads back more at 64 KiB:** 0.33 to 0.42 reads per WRITE
  for 47008-byte records and 0.73 to 0.77 for 6000-byte ones, both
  smaller than a page here. That is a cost of misaligned records smaller
  than a page, not a failure; nothing stays resident.
- `n-reconf` copied 20 and 27 records for want of a budget (0 at 4 KiB
  and 16 KiB), and `n-own`, `n-default` and `n-copy` all show exactly
  12.00 MiB of the region held. Neither was examined.

**The 64 KiB column is of a later kernel.** Build 458 is `v7.1.13-20`
with this series as of the `-WIP` branch: it charges a loan for the
region pages it holds (not its fragments), takes a transport that reads
on without admission off the wait list, counts a merge page once when a
record is materialized, and cuts a split write per end with the gate in
bytes. The first of those matters only where frames share a page; the
second and third are not specific to a page size, and **the 4 KiB and
16 KiB hosts have not been run with them.** Build 458 does not have the
rule that issues a write beyond EOF buffered, so its new-file copies are
at about 440 MB/s like the other columns'.

Throughput is not compared across hosts: they are different machines.
What veth cannot answer stays open for tardis1's card and real NVMe:
whether a small region stays in the CPU's cache, and what a copy costs
there.

## The comparison

### What each column is

|  | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- |
| kernel | 7.1.13-19.hs.451.el9.aarch64+ | 7.1.13-19.hs.451.el9.aarch64+ | 7.1.13-20.hs.458.loanpages_pool.el9.aarch64+64k |
| built from | v7.1.13-7433-g26e799b10e8eb | v7.1.13-7420-g82d7a6d21207 | - |
| boots used | 2 | 5 | 2 |
| last stage finished | 2026-10-05_20:16:18 | 2026-10-05_20:52:11 | 2026-10-07_09:27:12 |
| page budgets | geometry: page 4096, 1 MiB = 256 pages; factor 150 (MTU 9000) 280 (MTU 1500); record cost 385 / 718, reserve 390 / 720; rcvbuf pages 256K 128 128K 64 64K 32 (256K at MTU 1500 128); region default 4096; collapse tcp_rmem 4096 32768 65536 | geometry: page 16384, 1 MiB = 64 pages; factor 190 (MTU 9000) 170 (MTU 1500); record cost 123 / 110, reserve 130 / 110; rcvbuf pages 256K 32 128K 16 64K 8 (256K at MTU 1500 32); region default 1519; collapse tcp_rmem 16384 131072 262144 | geometry: page 65536, 1 MiB = 16 pages; factor 150 (MTU 9000) 150 (MTU 1500); record cost 25 / 25, reserve 30 / 30; rcvbuf pages 256K 8 128K 4 64K 2 (256K at MTU 1500 8); region default 555; collapse tcp_rmem 65536 524288 1048576 |
| nvme-loop: virt_boundary_mask | 4095 | 4095 | 4095 |
| NVMe/TCP: virt_boundary_mask | 0 | 0 | 0 |
| NVMe/TCP: veth rx_ip_align | 2 | 2 | 2 |

### The plan, stage by stage

| stage | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- |
| `preflight` | PASS | PASS | PASS |
| `budget-D` | DONE | DONE | DONE |
| `budget-D-sgl` | DONE | DONE | DONE |
| `kunit` | PASS | PASS | PASS |
| `bvecrepro` | PASS | PASS | PASS |
| `rig-cmp` | PASS | PASS | PASS |
| `loan-assert` | PASS | PASS | PASS |
| `killswitch` | PASS | PASS | PASS |
| `system-correctness` | PASS | PASS | PASS |
| `zc-down` | DONE | DONE | DONE |
| `sgl-up` | DONE | DONE | DONE |
| `sgl-brd` | DONE | DONE | DONE |
| `sgl-tcp` | DONE | DONE | DONE |
| `sgl-loop` | DONE | DONE | DONE |
| `sgl-down` | DONE | DONE | DONE |
| `localio-tcp` | DONE | DONE | DONE |
| `localio-loop` | DONE | DONE | DONE |
| `budget-A` | DONE | DONE | DONE |
| `budget-TLP` | DONE | DONE | DONE |
| `budget-X` | DONE | DONE | DONE |
| `budget-N` | DONE | DONE | DONE |
| `budget-A-sgl` | DONE | DONE | DONE |
| `budget-TLP-sgl` | DONE | DONE | DONE |
| `budget-X-sgl` | DONE | DONE | DONE |
| `budget-N-sgl` | DONE | DONE | DONE |
| `budget-B` | DONE | DONE | DONE |
| `budget-B-sgl` | DONE | DONE | DONE |
| `analyze-AB` | PASS | PASS | PASS |
| `analyze-AB-sgl` | PASS | PASS | PASS |
| `summary` | DONE | DONE | DONE |
| KUnit `sunrpc-xdr-bvec` | 7/7 pass, 0 fail, 0 skip | 7/7 pass, 0 fail, 0 skip | 7/7 pass, 0 fail, 0 skip |
| KUnit `sunrpc-svcsock-rx` | 40/41 pass, 0 fail, 1 skip | 41/41 pass, 0 fail, 0 skip | 43/43 pass, 0 fail, 0 skip |
| KUnit `nfsd-receive-bvec` | 6/6 pass, 0 fail, 0 skip | 6/6 pass, 0 fail, 0 skip | 6/6 pass, 0 fail, 0 skip |
| KUnit `nfsd4-receive-bvec` | 10/10 pass, 0 fail, 0 skip | 10/10 pass, 0 fail, 0 skip | 10/10 pass, 0 fail, 0 skip |
| LOCALIO tcp/rs47008/reads_per_write | 0.002 | 0.004 | 0.418 |
| LOCALIO tcp/rs6000/reads_per_write | 0.000 | 0.549 | 0.766 |
| LOCALIO tcp/boundary_exit | 0 | 0 | 0 |
| LOCALIO tcp/short-read_exit | 0 | 0 | 0 |
| LOCALIO tcp/stable_exit | 0 | 0 | 0 |
| LOCALIO tcp/rs24000/reads_per_write | - | 0.009 | - |
| LOCALIO tcp/rs96000/reads_per_write | - | - | 0.003 |
| LOCALIO loop/rs47008/reads_per_write | 0.005 | 0.002 | 0.334 |
| LOCALIO loop/rs6000/reads_per_write | 0.000 | 0.478 | 0.726 |
| LOCALIO loop/boundary_exit | 0 | 0 | 0 |
| LOCALIO loop/short-read_exit | 0 | 0 | 0 |
| LOCALIO loop/stable_exit | 0 | 0 | 0 |
| LOCALIO loop/rs24000/reads_per_write | - | 0.006 | - |
| LOCALIO loop/rs96000/reads_per_write | - | - | 0.005 |
| fio verify failures, prp phase A | 0 | 0 | 0 |
| fio verify failures, prp phase B | 0 | 0 | 0 |
| fio verify failures, prp phase D | 0 | 0 | 0 |
| fio verify failures, prp phase N | 0 | 0 | 0 |
| fio verify failures, prp phase TLP | 0 | 0 | 0 |
| fio verify failures, prp phase X | 0 | 0 | 0 |
| fio verify failures, sgl phase A | 0 | 0 | 0 |
| fio verify failures, sgl phase B | 0 | 0 | 0 |
| fio verify failures, sgl phase D | 0 | 0 | 0 |
| fio verify failures, sgl phase N | 0 | 0 | 0 |
| fio verify failures, sgl phase TLP | 0 | 0 | 0 |
| fio verify failures, sgl phase X | 0 | 0 | 0 |

### Copy against loan

Each loan cell (`-on`, `edge-at-`) beside its twin that copies (`-copy`): MB/s, and the
most of the region either held, in MiB. "sized" pairs write files that already have their size.

Storage that needs whole pages (nvme-loop):

| pair (phase) | 4 KiB: copy / loan MB/s | 16 KiB: copy / loan MB/s | 64 KiB: copy / loan MB/s | 4 KiB: region MiB copy / loan | 16 KiB: region MiB copy / loan | 64 KiB: region MiB copy / loan | loan written |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `seq-sized-copy` / `seq-sized-on` (A) | 2114 / 2139 | 1832 / 1802 | 2174 / 2191 | 0.25 / 9.59 | 0.25 / 15.09 | 0.50 / 11.25 | buffered |
| `mixed-sized-copy` / `mixed-sized-on` (A) | 841 / 850 | 792 / 799 | 2594 / 1236 | 1.71 / 9.15 | 1.75 / 16.67 | 0.50 / 17.00 | buffered |
| `step-copy` / `step-on` (A) | 1277 / 1437 | 1194 / 1298 | 1315 / 1323 | 0.25 / 9.61 | 0.50 / 15.16 | 0.50 / 19.81 | buffered |
| `step-copy-n16` / `step-on-n16` (A) | 1958 / 2009 | 1439 / 1439 | 1912 / 1229 | 1.31 / 8.27 | 1.75 / 15.78 | 1.25 / 17.88 | buffered |
| `seq-copy` / `seq-on` (A) | 436 / 2114 | 414 / 1766 | 438 / 2105 | 0.25 / 9.59 | 0.25 / 15.11 | 0.25 / 11.00 | buffered |
| `mixed-copy` / `mixed-on` (A) | 725 / 814 | 656 / 746 | 440 / 1156 | 1.79 / 9.19 | 1.75 / 16.83 | 0.75 / 16.75 | buffered |
| `edge-copy-n1` / `edge-at-n1` (B) | 446 / 345 | 394 / 285 | 438 / 363 | 0.25 / 1.61 | 0.25 / 2.25 | 0.25 / 1.50 | buffered |

Storage that takes arbitrary pieces (NVMe/TCP):

| pair (phase) | 4 KiB: copy / loan MB/s | 16 KiB: copy / loan MB/s | 64 KiB: copy / loan MB/s | 4 KiB: region MiB copy / loan | 16 KiB: region MiB copy / loan | 64 KiB: region MiB copy / loan | loan written |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `seq-sized-copy` / `seq-sized-on` (A) | 1945 / 1897 | 1662 / 1612 | 2174 / 2165 | 0.25 / 9.59 | 0.25 / 15.11 | 0.50 / 11.75 | direct |
| `mixed-sized-copy` / `mixed-sized-on` (A) | 847 / 829 | 790 / 781 | 2521 / 1142 | 1.75 / 9.22 | 1.75 / 16.48 | 1.50 / 15.75 | direct |
| `step-copy` / `step-on` (A) | 1251 / 1334 | 1085 / 1070 | 1303 / 1323 | 0.50 / 9.60 | 0.50 / 15.14 | 0.50 / 19.81 | direct |
| `step-copy-n16` / `step-on-n16` (A) | 1847 / 1748 | 1207 / 1128 | 1846 / 1009 | 1.47 / 8.27 | 1.75 / 15.83 | 1.25 / 16.88 | direct |
| `seq-copy` / `seq-on` (A) | 394 / 390 | 358 / 339 | 399 / 394 | 0.25 / 9.61 | 0.25 / 15.00 | 0.50 / 19.75 | direct |
| `mixed-copy` / `mixed-on` (A) | 713 / 695 | 624 / 603 | 447 / 847 | 1.75 / 9.10 | 1.75 / 16.38 | 0.50 / 20.75 | direct |
| `edge-copy-n1` / `edge-at-n1` (B) | 392 / 343 | 340 / 279 | 398 / 332 | 0.25 / 1.62 | 0.25 / 2.25 | 0.50 / 1.50 | direct |

### Every cell

MB/s · packets dropped · most of the region held (MiB) · how nfsd issued the WRITEs.

Phase A, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `warmup` | 1 | no | 2684 · 0 · 2.56 · direct | 1598 · 0 · 4.50 · direct | 2237 · 0 · 0.50 · direct |
| `seq-off` | 1 | no | 432 · 0 · 9.45 · direct | 390 · 0 · 3.00 · direct | 435 · 0 · 0.50 · direct |
| `seq-copy` | 1 | no | 436 · 0 · 0.25 · direct | 414 · 0 · 0.25 · direct | 438 · 0 · 0.25 · direct |
| `seq-on` | 1 | yes | 2114 · 0 · 9.59 · buffered | 1766 · 0 · 15.11 · buffered | 2105 · 0 · 11.00 · buffered |
| `seq-sized-copy` | 1 | no | 2114 · 0 · 0.25 · direct | 1832 · 0 · 0.25 · direct | 2174 · 0 · 0.50 · direct |
| `seq-sized-on` | 1 | yes | 2139 · 0 · 9.59 · buffered | 1802 · 0 · 15.09 · buffered | 2191 · 0 · 11.25 · buffered |
| `mixed-off` | 16 | no | 576 · 7577 · 16.00 · direct | 540 · 7196 · 23.73 · direct | 441 · 0 · 2.25 · direct |
| `mixed-copy` | 16 | no | 725 · 0 · 1.79 · direct | 656 · 0 · 1.75 · direct | 440 · 0 · 0.75 · direct |
| `mixed-on` | 16 | yes | 814 · 0 · 9.19 · buffered | 746 · 0 · 16.83 · buffered | 1156 · 0 · 16.75 · buffered |
| `mixed-sized-copy` | 16 | no | 841 · 0 · 1.71 · direct | 792 · 0 · 1.75 · direct | 2594 · 0 · 0.50 · direct |
| `mixed-sized-on` | 16 | yes | 850 · 0 · 9.15 · buffered | 799 · 0 · 16.67 · buffered | 1236 · 0 · 17.00 · buffered |
| `mixed-on-v42` | 16 | yes | 849 · 0 · 9.16 · buffered | 792 · 0 · 16.86 · buffered | 1091 · 0 · 17.00 · buffered |
| `step-off` | 1 | no | 1649 · 4 · 16.00 · direct | 1482 · 0 · 22.86 · direct | 1308 · 0 · 6.75 · direct |
| `step-copy` | 1 | no | 1277 · 0 · 0.25 · direct | 1194 · 0 · 0.50 · direct | 1315 · 0 · 0.50 · direct |
| `step-on` | 1 | yes | 1437 · 0 · 9.61 · buffered | 1298 · 0 · 15.16 · buffered | 1323 · 0 · 19.81 · buffered |
| `step-copy-n16` | 16 | no | 1958 · 0 · 1.31 · direct | 1439 · 0 · 1.75 · direct | 1912 · 0 · 1.25 · direct |
| `step-on-n16` | 16 | yes | 2009 · 0 · 8.27 · buffered | 1439 · 0 · 15.78 · buffered | 1229 · 0 · 17.88 · buffered |
| `mtu1500-off` | 1 | no | 453 · 0 · 4.40 · direct | 391 · 0 · 3.00 · direct | 436 · 0 · 0.25 · direct |
| `mtu1500-on` | 1 | yes | 458 · 0 · 2.36 · direct | 414 · 0 · 1.50 · direct | 436 · 0 · 1.00 · direct |
| `collapse-on` | 16 | yes | 827 · 0 · 9.12 · buffered | 766 · 0 · 16.73 · buffered | 1092 · 0 · 16.75 · buffered |
| `floor-refused` | 16 | yes | 747 · 372 · 16.00 · buffered | 632 · 531 · 23.73 · buffered | 2182 · 484 · 34.69 · buffered |
| `collapse-off` | 16 | no | 691 · 0 · 2.12 · both | 639 · 0 · 3.75 · direct | 440 · 0 · 2.00 · direct |
| `early-release-m1-d10` | 1 | yes | 251 · 0 · 2.12 · - | 251 · 0 · 2.75 · - | 527 · 0 · 1.75 · - |

Phase B, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `edge-at-n1` | 1 | yes | 345 · 0 · 1.61 · buffered | 285 · 0 · 2.25 · buffered | 363 · 0 · 1.50 · buffered |
| `edge-at-n4` | 4 | yes | 345 · 0 · 1.85 · buffered | 292 · 0 · 2.50 · buffered | 352 · 0 · 2.25 · buffered |
| `edge-at-mixed-n4` | 4 | yes | 144 · 0 · 1.86 · buffered | 124 · 0 · 2.50 · buffered | 146 · 0 · 2.25 · buffered |
| `edge-at-step` | 1 | yes | 1122 · 0 · 1.64 · buffered | 812 · 0 · 2.25 · buffered | 896 · 0 · 1.75 · buffered |
| `edge-below-n1` | 1 | yes | 365 · 0 · 1.62 · buffered | 311 · 0 · 2.25 · buffered | 362 · 0 · 1.75 · buffered |
| `edge-off-n1` | 1 | no | 324 · 482 · 3.27 · direct | 382 · 0 · 3.25 · direct | 440 · 0 · 0.50 · direct |
| `edge-copy-n1` | 1 | no | 446 · 0 · 0.25 · direct | 394 · 0 · 0.25 · direct | 438 · 0 · 0.25 · direct |

Phase D, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `warmup` | 1 | no | 2581 · 0 · 1.75 · direct | 1342 · 0 · 4.25 · direct | 2165 · 0 · 0.50 · direct |

Phase TLP, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `warmup` | 1 | no | 1678 · 0 · 3.17 · direct | 1525 · 0 · 4.00 · direct | 2034 · 0 · 0.50 · direct |
| `step-on-n16` | 16 | yes | 2005 · 0 · 8.28 · buffered | 1364 · 0 · 15.75 · buffered | 1243 · 0 · 17.56 · buffered |
| `step-on-n16-quickack` | 16 | yes | 1785 · 0 · 8.45 · buffered | 1012 · 0 · 15.77 · buffered | 835 · 0 · 17.38 · buffered |
| `step-on-n16-notlp` | 16 | yes | 1999 · 0 · 8.24 · buffered | 1439 · 0 · 15.77 · buffered | 1222 · 0 · 17.62 · buffered |
| `step-rcvbuf-n16` | 16 | no | 1909 · 0 · 1.53 · direct | 1351 · 0 · 1.75 · direct | 1901 · 0 · 1.25 · direct |
| `mixed-on` | 16 | yes | 824 · 0 · 9.18 · buffered | 752 · 0 · 16.41 · buffered | 1211 · 0 · 17.00 · buffered |
| `mixed-on-quickack` | 16 | yes | 813 · 0 · 9.30 · buffered | 695 · 0 · 16.42 · buffered | 872 · 0 · 17.50 · buffered |
| `mixed-rcvbuf` | 16 | no | 691 · 0 · 1.75 · direct | 602 · 0 · 1.75 · direct | 446 · 0 · 0.50 · direct |

Phase X, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `warmup` | 1 | no | 1678 · 0 · 3.73 · direct | 2397 · 0 · 4.00 · direct | 2097 · 0 · 0.25 · direct |

Phase N, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `warmup` | 1 | no | - | 1637 · 0 · 5.62 · direct | - |

Phase A, storage that takes arbitrary pieces (NVMe/TCP):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `warmup` | 1 | no | 1814 · 0 · 3.63 · direct | 1864 · 0 · 7.84 · direct | 2237 · 0 · 0.25 · direct |
| `seq-off` | 1 | no | 409 · 0 · 9.45 · direct | 347 · 0 · 4.25 · direct | 398 · 0 · 0.50 · direct |
| `seq-copy` | 1 | no | 394 · 0 · 0.25 · direct | 358 · 0 · 0.25 · direct | 399 · 0 · 0.50 · direct |
| `seq-on` | 1 | yes | 390 · 0 · 9.61 · direct | 339 · 0 · 15.00 · direct | 394 · 0 · 19.75 · direct |
| `seq-sized-copy` | 1 | no | 1945 · 0 · 0.25 · direct | 1662 · 0 · 0.25 · direct | 2174 · 0 · 0.50 · direct |
| `seq-sized-on` | 1 | yes | 1897 · 0 · 9.59 · direct | 1612 · 0 · 15.11 · direct | 2165 · 0 · 11.75 · direct |
| `mixed-off` | 16 | no | 581 · 6329 · 16.00 · direct | 566 · 7916 · 23.73 · direct | 443 · 0 · 3.00 · direct |
| `mixed-copy` | 16 | no | 713 · 0 · 1.75 · direct | 624 · 0 · 1.75 · direct | 447 · 0 · 0.50 · direct |
| `mixed-on` | 16 | yes | 695 · 0 · 9.10 · direct | 603 · 0 · 16.38 · direct | 847 · 0 · 20.75 · direct |
| `mixed-sized-copy` | 16 | no | 847 · 0 · 1.75 · direct | 790 · 0 · 1.75 · direct | 2521 · 0 · 1.50 · direct |
| `mixed-sized-on` | 16 | yes | 829 · 0 · 9.22 · direct | 781 · 0 · 16.48 · direct | 1142 · 0 · 15.75 · direct |
| `mixed-on-v42` | 16 | yes | 832 · 0 · 9.20 · direct | 787 · 0 · 16.50 · direct | 1051 · 0 · 16.25 · direct |
| `step-off` | 1 | no | 1516 · 0 · 13.35 · direct | 1262 · 0 · 23.25 · direct | 1316 · 0 · 8.25 · direct |
| `step-copy` | 1 | no | 1251 · 0 · 0.50 · direct | 1085 · 0 · 0.50 · direct | 1303 · 0 · 0.50 · direct |
| `step-on` | 1 | yes | 1334 · 0 · 9.60 · direct | 1070 · 0 · 15.14 · direct | 1323 · 0 · 19.81 · direct |
| `step-copy-n16` | 16 | no | 1847 · 0 · 1.47 · direct | 1207 · 0 · 1.75 · direct | 1846 · 0 · 1.25 · direct |
| `step-on-n16` | 16 | yes | 1748 · 0 · 8.27 · direct | 1128 · 0 · 15.83 · direct | 1009 · 0 · 16.88 · direct |
| `mtu1500-off` | 1 | no | 395 · 0 · 7.57 · direct | 327 · 0 · 6.59 · direct | 403 · 0 · 0.25 · direct |
| `mtu1500-on` | 1 | yes | 415 · 0 · 2.32 · direct | 362 · 0 · 1.50 · direct | 399 · 0 · 1.00 · direct |
| `collapse-on` | 16 | yes | 683 · 0 · 9.22 · direct | 624 · 0 · 16.41 · direct | 767 · 0 · 22.50 · direct |
| `floor-refused` | 16 | yes | 662 · 432 · 16.00 · direct | 534 · 875 · 23.73 · direct | 648 · 1729 · 34.69 · direct |
| `collapse-off` | 16 | no | 704 · 0 · 2.10 · direct | 603 · 0 · 3.75 · direct | 447 · 0 · 2.75 · direct |
| `early-release-m1-d10` | 1 | yes | 253 · 0 · 1.96 · - | 247 · 0 · 3.25 · - | 534 · 0 · 1.75 · - |

Phase B, storage that takes arbitrary pieces (NVMe/TCP):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `edge-at-n1` | 1 | yes | 343 · 0 · 1.62 · direct | 279 · 0 · 2.25 · direct | 332 · 0 · 1.50 · direct |
| `edge-at-n4` | 4 | yes | 329 · 0 · 1.85 · direct | 270 · 0 · 2.50 · direct | 328 · 0 · 2.25 · direct |
| `edge-at-mixed-n4` | 4 | yes | 146 · 0 · 1.85 · direct | 124 · 0 · 2.50 · direct | 144 · 0 · 2.25 · direct |
| `edge-at-step` | 1 | yes | 928 · 0 · 1.64 · direct | 626 · 0 · 2.25 · direct | 764 · 0 · 1.75 · direct |
| `edge-below-n1` | 1 | yes | 333 · 0 · 1.62 · direct | 275 · 0 · 2.25 · direct | 340 · 0 · 1.50 · direct |
| `edge-off-n1` | 1 | no | 170 · 1389 · 3.27 · direct | 339 · 0 · 4.00 · direct | 403 · 0 · 0.25 · direct |
| `edge-copy-n1` | 1 | no | 392 · 0 · 0.25 · direct | 340 · 0 · 0.25 · direct | 398 · 0 · 0.50 · direct |

Phase D, storage that takes arbitrary pieces (NVMe/TCP):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `warmup` | 1 | no | 1974 · 0 · 4.97 · direct | 1598 · 0 · 6.75 · direct | 2237 · 0 · 0.50 · direct |

Phase TLP, storage that takes arbitrary pieces (NVMe/TCP):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `warmup` | 1 | no | 1721 · 0 · 2.96 · direct | 1814 · 0 · 4.30 · direct | 2237 · 0 · 0.25 · direct |
| `step-on-n16` | 16 | yes | 1761 · 0 · 8.26 · direct | 1081 · 0 · 15.80 · direct | 1041 · 0 · 18.00 · direct |
| `step-on-n16-quickack` | 16 | yes | 1531 · 0 · 8.46 · direct | 864 · 0 · 15.81 · direct | 737 · 0 · 17.25 · direct |
| `step-on-n16-notlp` | 16 | yes | 1745 · 0 · 8.21 · direct | 1087 · 0 · 15.77 · direct | 1043 · 0 · 17.69 · direct |
| `step-rcvbuf-n16` | 16 | no | 1830 · 0 · 1.48 · direct | 1138 · 0 · 1.75 · direct | 1834 · 0 · 1.50 · direct |
| `mixed-on` | 16 | yes | 691 · 0 · 9.12 · direct | 592 · 0 · 16.77 · direct | 889 · 0 · 21.50 · direct |
| `mixed-on-quickack` | 16 | yes | 691 · 0 · 9.26 · direct | 609 · 0 · 16.73 · direct | 774 · 0 · 20.44 · direct |
| `mixed-rcvbuf` | 16 | no | 712 · 0 · 1.75 · direct | 624 · 0 · 1.75 · direct | 452 · 0 · 0.50 · direct |

Phase X, storage that takes arbitrary pieces (NVMe/TCP):

| cell | conn | budget set | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- | --- | --- |
| `warmup` | 1 | no | 1721 · 0 · 2.64 · direct | 1814 · 0 · 4.25 · direct | 2165 · 0 · 0.25 · direct |

### Phases X and D: a region client (R) beside a client that is not (N)

R MB/s · N MB/s · drops · alloc fail · region MiB.

Phase D, storage that needs whole pages (nvme-loop):

| cell | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- |
| `d-off-N` | - · 585 · 0 · 7836 · 16.00 | - · 498 · 0 · 8618 · 23.73 | - · 440 · 0 · 0 · 2.50 |
| `d-on-N` | - · 850 · 0 · 0 · 9.39 | - · 709 · 0 · 0 · 16.69 | - · 1193 · 0 · 0 · 18.00 |
| `d-on-RN` | 414 · 416 · 0 · 0 · 12.20 | 360 · 355 · 0 · 0 · 20.53 | 598 · 618 · 0 · 0 · 27.25 |
| `d-tight-N` | - · 145 · 0 · 0 · 3.55 | - · 132 · 0 · 0 · 4.00 | - · 146 · 0 · 0 · 5.00 |
| `d-tight-RN` | 72 · 72 · 0 · 0 · 6.40 | 64 · 64 · 0 · 0 · 7.50 | 73 · 73 · 0 · 0 · 9.00 |

Phase X, storage that needs whole pages (nvme-loop):

| cell | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- |
| `x-off-R` | 572 · - · 6380 · 6380 · 16.00 | 548 · - · 7471 · 7471 · 23.73 | 435 · - · 0 · 0 · 3.25 |
| `x-off-N` | - · 614 · 0 · 0 · 0.25 | - · 552 · 0 · 0 · 0.25 | - · 436 · 0 · 0 · 0.25 |
| `x-off-RN` | 346 · 488 · 7273 · 7273 · 16.00 | 304 · 418 · 7620 · 7620 · 23.73 | 576 · 430 · 4137 · 4137 · 34.69 |
| `x-on-R` | 810 · - · 0 · 0 · 9.14 | 716 · - · 0 · 0 · 16.45 | 1224 · - · 0 · 0 · 17.75 |
| `x-on-N` | - · 615 · 0 · 0 · 0.25 | - · 551 · 0 · 0 · 0.25 | - · 436 · 0 · 0 · 0.25 |
| `x-on-RN` | 322 · 546 · 0 · 0 · 9.16 | 284 · 489 · 0 · 0 · 16.44 | 392 · 427 · 0 · 0 · 17.25 |
| `x-tight-R` | 139 · - · 0 · 0 · 3.28 | 127 · - · 0 · 0 · 3.75 | 146 · - · 0 · 0 · 4.75 |
| `x-tight-N` | - · 612 · 0 · 0 · 0.25 | - · 525 · 0 · 0 · 0.25 | - · 431 · 0 · 0 · 0.25 |
| `x-tight-RN` | 114 · 596 · 0 · 0 · 3.29 | 102 · 536 · 0 · 0 · 4.00 | 128 · 428 · 0 · 0 · 4.75 |

Phase D, storage that takes arbitrary pieces (NVMe/TCP):

| cell | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- |
| `d-off-N` | - · 596 · 0 · 7381 · 16.00 | - · 455 · 0 · 7953 · 23.73 | - · 447 · 0 · 0 · 3.00 |
| `d-on-N` | - · 705 · 0 · 0 · 9.45 | - · 559 · 0 · 0 · 16.78 | - · 814 · 0 · 0 · 21.50 |
| `d-on-RN` | 377 · 374 · 0 · 0 · 12.11 | 313 · 315 · 0 · 0 · 22.14 | 519 · 500 · 0 · 0 · 27.25 |
| `d-tight-N` | - · 143 · 0 · 0 · 3.54 | - · 122 · 0 · 0 · 4.25 | - · 140 · 0 · 0 · 4.75 |
| `d-tight-RN` | 71 · 71 · 0 · 0 · 6.42 | 60 · 60 · 0 · 0 · 7.50 | 71 · 71 · 0 · 0 · 9.00 |

Phase X, storage that takes arbitrary pieces (NVMe/TCP):

| cell | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- |
| `x-off-R` | 564 · - · 8016 · 8016 · 16.00 | 447 · - · 7620 · 7620 · 23.73 | 448 · - · 0 · 0 · 2.50 |
| `x-off-N` | - · 589 · 0 · 0 · 0.25 | - · 544 · 0 · 0 · 0.25 | - · 441 · 0 · 0 · 0.25 |
| `x-off-RN` | 323 · 471 · 7679 · 7679 · 16.00 | 265 · 358 · 7479 · 7479 · 23.73 | 543 · 419 · 3914 · 3914 · 34.69 |
| `x-on-R` | 667 · - · 0 · 0 · 9.17 | 602 · - · 0 · 0 · 16.39 | 749 · - · 0 · 0 · 19.25 |
| `x-on-N` | - · 585 · 0 · 0 · 0.25 | - · 494 · 0 · 0 · 0.25 | - · 438 · 0 · 0 · 0.25 |
| `x-on-RN` | 292 · 507 · 0 · 0 · 9.18 | 278 · 521 · 0 · 0 · 16.33 | 348 · 391 · 0 · 0 · 19.75 |
| `x-tight-R` | 137 · - · 0 · 0 · 3.29 | 121 · - · 0 · 0 · 3.75 | 143 · - · 0 · 0 · 4.25 |
| `x-tight-N` | - · 590 · 0 · 0 · 0.25 | - · 483 · 0 · 0 · 0.25 | - · 439 · 0 · 0 · 0.25 |
| `x-tight-RN` | 111 · 546 · 0 · 0 · 3.29 | 98 · 513 · 0 · 0 · 4.00 | 129 · 427 · 0 · 0 · 4.25 |

### Phase N: a budget per network namespace

R MB/s · B MB/s · copied (no budget) · region MiB.

Phase N, storage that needs whole pages (nvme-loop):

| cell | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- |
| `n-own` | - · 692 · 0 · 6.74 | - · 597 · 0 · 10.64 | - · 646 · 0 · 12.00 |
| `n-default` | - · 781 · 0 · 6.74 | - · 720 · 0 · 10.53 | - · 634 · 0 · 12.00 |
| `n-copy` | - · 846 · 5570 · 1.79 | - · 792 · 5570 · 3.95 | - · 906 · 5570 · 12.00 |
| `n-both` | 547 · 551 · 0 · 11.05 | 500 · 511 · 0 · 16.69 | 833 · 551 · 0 · 23.00 |
| `n-reconf` | - · 1422 · 0 · 8.27 | - · 1327 · 0 · 12.98 | - · 1227 · 20 · 17.50 |
| `n-exit` | - · 476 · 0 · 6.79 | - · 426 · 0 · 10.58 | - · 372 · 0 · 15.31 |

Phase N, storage that takes arbitrary pieces (NVMe/TCP):

| cell | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- |
| `n-own` | - · 757 · 0 · 6.79 | - · 737 · 0 · 9.20 | - · 613 · 0 · 12.00 |
| `n-default` | - · 769 · 0 · 6.75 | - · 800 · 0 · 8.97 | - · 621 · 0 · 12.00 |
| `n-copy` | - · 869 · 5570 · 1.72 | - · 811 · 5570 · 3.81 | - · 884 · 5570 · 12.00 |
| `n-both` | 507 · 615 · 0 · 11.09 | 548 · 539 · 0 · 14.67 | 612 · 490 · 0 · 23.50 |
| `n-reconf` | - · 1465 · 0 · 7.74 | - · 1692 · 0 · 10.28 | - · 1293 · 27 · 13.25 |
| `n-exit` | - · 428 · 0 · 6.79 | - · 437 · 0 · 9.27 | - · 342 · 0 · 13.25 |

