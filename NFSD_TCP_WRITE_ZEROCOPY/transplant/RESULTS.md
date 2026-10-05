# Results across page sizes

One record per test host, in the same form, so that 4 KiB, 16 KiB and
64 KiB can be read side by side. The tables below are generated; the
record for each page size is `results/<page>.tsv`.

**State: 16 KiB is complete (2026-10-05). 4 KiB and 64 KiB have not been
run with this harness.** Earlier 4 KiB results exist
(`../slipstream/README.md` section 8, `../slipstream/BUDGET_SCOPE.md`
section 8), but they are of an older harness: nvme-loop only, no copy
twins, and a test storage ten times slower than asked. They are not
comparable with what is below, and a 4 KiB run replaces them.

A status page for readers outside the tree, with the terms, the two kinds
of storage and the 16 KiB results, is "NFSD Receive Region Budget":
https://claude.ai/artifact/3UQkMVoWW8p9A5ZASPnm3g (Mike's; private until he
shares it). Update it when a host's record lands.

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

Only 16 KiB so far (`README.md`, "16 KiB host", has the account):

- The plan passes on both kinds of storage: no verify failure, hang or
  kernel warning.
- With a budget set and the receive buffer held, nothing is dropped and
  the region never runs out, whichever way the loan is written.
- On storage that takes arbitrary pieces every loaned WRITE is written
  direct.
- Copy and loan are level in throughput on this rig; a copy holds 0.25 to
  1.75 MiB of the region and a loan about 15 to 17 MiB.

What the other two page sizes should be read for, when they exist:

- **4 KiB:** the same answers, and whether veth's frame layout there
  (head plus two page fragments at MTU 9000) still gives 4-byte aligned
  loans with `rx_ip_align=2`.
- **64 KiB (tardis1):** the loan table (sized for 4 KiB fragments since
  2026-10-02, never run), the split of a misaligned write smaller than a
  page, the credit factor (150, computed, unmeasured), and on real
  hardware whether a small region stays in the CPU's cache.

## The comparison

### What each column is

|  | 16 KiB |
| --- | --- |
| kernel | 7.1.13-19.hs.451.el9.aarch64+ |
| built from | v7.1.13-7420-g82d7a6d21207 |
| boots used | 4 |
| last stage finished | 2026-10-05_18:02:10 |
| page budgets | geometry: page 16384, 1 MiB = 64 pages; factor 190 (MTU 9000) 170 (MTU 1500); record cost 123 / 110, reserve 130 / 110; rcvbuf pages 256K 32 128K 16 64K 8 (256K at MTU 1500 32); region default 1519; collapse tcp_rmem 16384 131072 262144 |
| nvme-loop: virt_boundary_mask | 4095 |
| NVMe/TCP: virt_boundary_mask | 0 |
| NVMe/TCP: veth rx_ip_align | 2 |

### The plan, stage by stage

| stage | 16 KiB |
| --- | --- |
| `preflight` | PASS |
| `budget-D` | DONE |
| `kunit` | PASS |
| `bvecrepro` | PASS |
| `rig-cmp` | PASS |
| `loan-assert` | PASS |
| `killswitch` | PASS |
| `system-correctness` | PASS |
| `zc-down` | DONE |
| `sgl-up` | DONE |
| `sgl-brd` | DONE |
| `sgl-tcp` | DONE |
| `sgl-loop` | DONE |
| `sgl-down` | DONE |
| `localio-tcp` | DONE |
| `localio-loop` | DONE |
| `budget-A` | DONE |
| `budget-TLP` | DONE |
| `budget-X` | DONE |
| `budget-N` | DONE |
| `budget-A-sgl` | DONE |
| `budget-X-sgl` | DONE |
| `budget-D-sgl` | DONE |
| `budget-TLP-sgl` | DONE |
| `budget-N-sgl` | DONE |
| `budget-B` | DONE |
| `budget-B-sgl` | DONE |
| `analyze-AB` | PASS |
| `analyze-AB-sgl` | PASS |
| `summary` | DONE |
| KUnit `sunrpc-xdr-bvec` | 7/7 pass, 0 fail, 0 skip |
| KUnit `sunrpc-svcsock-rx` | 41/41 pass, 0 fail, 0 skip |
| KUnit `nfsd-receive-bvec` | 6/6 pass, 0 fail, 0 skip |
| KUnit `nfsd4-receive-bvec` | 10/10 pass, 0 fail, 0 skip |
| LOCALIO tcp/rs47008/reads_per_write | 0.004 |
| LOCALIO tcp/rs24000/reads_per_write | 0.009 |
| LOCALIO tcp/rs6000/reads_per_write | 0.549 |
| LOCALIO tcp/boundary_exit | 0 |
| LOCALIO tcp/short-read_exit | 0 |
| LOCALIO tcp/stable_exit | 0 |
| LOCALIO loop/rs47008/reads_per_write | 0.002 |
| LOCALIO loop/rs24000/reads_per_write | 0.006 |
| LOCALIO loop/rs6000/reads_per_write | 0.478 |
| LOCALIO loop/boundary_exit | 0 |
| LOCALIO loop/short-read_exit | 0 |
| LOCALIO loop/stable_exit | 0 |
| fio verify failures, prp phase A | 0 |
| fio verify failures, prp phase B | 0 |
| fio verify failures, prp phase D | 0 |
| fio verify failures, prp phase N | 0 |
| fio verify failures, prp phase TLP | 0 |
| fio verify failures, prp phase X | 0 |
| fio verify failures, sgl phase A | 0 |
| fio verify failures, sgl phase B | 0 |
| fio verify failures, sgl phase D | 0 |
| fio verify failures, sgl phase N | 0 |
| fio verify failures, sgl phase TLP | 0 |
| fio verify failures, sgl phase X | 0 |

### Copy against loan

Each loan cell (`-on`, `edge-at-`) beside its twin that copies (`-copy`): MB/s, and the
most of the region either held, in MiB. "sized" pairs write files that already have their size.

Storage that needs whole pages (nvme-loop):

| pair (phase) | 16 KiB: copy / loan MB/s | 16 KiB: region MiB copy / loan | loan written |
| --- | --- | --- | --- |
| `seq-sized-copy` / `seq-sized-on` (A) | 1832 / 1802 | 0.25 / 15.09 | buffered |
| `mixed-sized-copy` / `mixed-sized-on` (A) | 792 / 799 | 1.75 / 16.67 | buffered |
| `step-copy` / `step-on` (A) | 1194 / 1298 | 0.50 / 15.16 | buffered |
| `step-copy-n16` / `step-on-n16` (A) | 1439 / 1439 | 1.75 / 15.78 | buffered |
| `seq-copy` / `seq-on` (A) | 414 / 1766 | 0.25 / 15.11 | buffered |
| `mixed-copy` / `mixed-on` (A) | 656 / 746 | 1.75 / 16.83 | buffered |
| `edge-copy-n1` / `edge-at-n1` (B) | 394 / 285 | 0.25 / 2.25 | buffered |

Storage that takes arbitrary pieces (NVMe/TCP):

| pair (phase) | 16 KiB: copy / loan MB/s | 16 KiB: region MiB copy / loan | loan written |
| --- | --- | --- | --- |
| `seq-sized-copy` / `seq-sized-on` (A) | 1662 / 1612 | 0.25 / 15.11 | direct |
| `mixed-sized-copy` / `mixed-sized-on` (A) | 790 / 781 | 1.75 / 16.48 | direct |
| `step-copy` / `step-on` (A) | 1085 / 1070 | 0.50 / 15.14 | direct |
| `step-copy-n16` / `step-on-n16` (A) | 1207 / 1128 | 1.75 / 15.83 | direct |
| `seq-copy` / `seq-on` (A) | 358 / 339 | 0.25 / 15.00 | direct |
| `mixed-copy` / `mixed-on` (A) | 624 / 603 | 1.75 / 16.38 | direct |
| `edge-copy-n1` / `edge-at-n1` (B) | 340 / 279 | 0.25 / 2.25 | direct |

### Every cell

MB/s · packets dropped · most of the region held (MiB) · how nfsd issued the WRITEs.

Phase A, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 16 KiB |
| --- | --- | --- | --- |
| `warmup` | 1 | no | 1598 · 0 · 4.50 · direct |
| `seq-off` | 1 | no | 390 · 0 · 3.00 · direct |
| `seq-copy` | 1 | no | 414 · 0 · 0.25 · direct |
| `seq-on` | 1 | yes | 1766 · 0 · 15.11 · buffered |
| `seq-sized-copy` | 1 | no | 1832 · 0 · 0.25 · direct |
| `seq-sized-on` | 1 | yes | 1802 · 0 · 15.09 · buffered |
| `mixed-off` | 16 | no | 540 · 7196 · 23.73 · direct |
| `mixed-copy` | 16 | no | 656 · 0 · 1.75 · direct |
| `mixed-on` | 16 | yes | 746 · 0 · 16.83 · buffered |
| `mixed-sized-copy` | 16 | no | 792 · 0 · 1.75 · direct |
| `mixed-sized-on` | 16 | yes | 799 · 0 · 16.67 · buffered |
| `mixed-on-v42` | 16 | yes | 792 · 0 · 16.86 · buffered |
| `step-off` | 1 | no | 1482 · 0 · 22.86 · direct |
| `step-copy` | 1 | no | 1194 · 0 · 0.50 · direct |
| `step-on` | 1 | yes | 1298 · 0 · 15.16 · buffered |
| `step-copy-n16` | 16 | no | 1439 · 0 · 1.75 · direct |
| `step-on-n16` | 16 | yes | 1439 · 0 · 15.78 · buffered |
| `mtu1500-off` | 1 | no | 391 · 0 · 3.00 · direct |
| `mtu1500-on` | 1 | yes | 414 · 0 · 1.50 · direct |
| `collapse-on` | 16 | yes | 766 · 0 · 16.73 · buffered |
| `floor-refused` | 16 | yes | 632 · 531 · 23.73 · buffered |
| `collapse-off` | 16 | no | 639 · 0 · 3.75 · direct |
| `early-release-m1-d10` | 1 | yes | 251 · 0 · 2.75 · - |

Phase B, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 16 KiB |
| --- | --- | --- | --- |
| `edge-at-n1` | 1 | yes | 285 · 0 · 2.25 · buffered |
| `edge-at-n4` | 4 | yes | 292 · 0 · 2.50 · buffered |
| `edge-at-mixed-n4` | 4 | yes | 124 · 0 · 2.50 · buffered |
| `edge-at-step` | 1 | yes | 812 · 0 · 2.25 · buffered |
| `edge-below-n1` | 1 | yes | 311 · 0 · 2.25 · buffered |
| `edge-off-n1` | 1 | no | 382 · 0 · 3.25 · direct |
| `edge-copy-n1` | 1 | no | 394 · 0 · 0.25 · direct |

Phase D, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 16 KiB |
| --- | --- | --- | --- |
| `warmup` | 1 | no | 1342 · 0 · 4.25 · direct |

Phase TLP, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 16 KiB |
| --- | --- | --- | --- |
| `warmup` | 1 | no | 1525 · 0 · 4.00 · direct |
| `step-on-n16` | 16 | yes | 1364 · 0 · 15.75 · buffered |
| `step-on-n16-quickack` | 16 | yes | 1012 · 0 · 15.77 · buffered |
| `step-on-n16-notlp` | 16 | yes | 1439 · 0 · 15.77 · buffered |
| `step-rcvbuf-n16` | 16 | no | 1351 · 0 · 1.75 · direct |
| `mixed-on` | 16 | yes | 752 · 0 · 16.41 · buffered |
| `mixed-on-quickack` | 16 | yes | 695 · 0 · 16.42 · buffered |
| `mixed-rcvbuf` | 16 | no | 602 · 0 · 1.75 · direct |

Phase X, storage that needs whole pages (nvme-loop):

| cell | conn | budget set | 16 KiB |
| --- | --- | --- | --- |
| `warmup` | 1 | no | 2397 · 0 · 4.00 · direct |

Phase A, storage that takes arbitrary pieces (NVMe/TCP):

| cell | conn | budget set | 16 KiB |
| --- | --- | --- | --- |
| `warmup` | 1 | no | 1864 · 0 · 7.84 · direct |
| `seq-off` | 1 | no | 347 · 0 · 4.25 · direct |
| `seq-copy` | 1 | no | 358 · 0 · 0.25 · direct |
| `seq-on` | 1 | yes | 339 · 0 · 15.00 · direct |
| `seq-sized-copy` | 1 | no | 1662 · 0 · 0.25 · direct |
| `seq-sized-on` | 1 | yes | 1612 · 0 · 15.11 · direct |
| `mixed-off` | 16 | no | 566 · 7916 · 23.73 · direct |
| `mixed-copy` | 16 | no | 624 · 0 · 1.75 · direct |
| `mixed-on` | 16 | yes | 603 · 0 · 16.38 · direct |
| `mixed-sized-copy` | 16 | no | 790 · 0 · 1.75 · direct |
| `mixed-sized-on` | 16 | yes | 781 · 0 · 16.48 · direct |
| `mixed-on-v42` | 16 | yes | 787 · 0 · 16.50 · direct |
| `step-off` | 1 | no | 1262 · 0 · 23.25 · direct |
| `step-copy` | 1 | no | 1085 · 0 · 0.50 · direct |
| `step-on` | 1 | yes | 1070 · 0 · 15.14 · direct |
| `step-copy-n16` | 16 | no | 1207 · 0 · 1.75 · direct |
| `step-on-n16` | 16 | yes | 1128 · 0 · 15.83 · direct |
| `mtu1500-off` | 1 | no | 327 · 0 · 6.59 · direct |
| `mtu1500-on` | 1 | yes | 362 · 0 · 1.50 · direct |
| `collapse-on` | 16 | yes | 624 · 0 · 16.41 · direct |
| `floor-refused` | 16 | yes | 534 · 875 · 23.73 · direct |
| `collapse-off` | 16 | no | 603 · 0 · 3.75 · direct |
| `early-release-m1-d10` | 1 | yes | 247 · 0 · 3.25 · - |

Phase B, storage that takes arbitrary pieces (NVMe/TCP):

| cell | conn | budget set | 16 KiB |
| --- | --- | --- | --- |
| `edge-at-n1` | 1 | yes | 279 · 0 · 2.25 · direct |
| `edge-at-n4` | 4 | yes | 270 · 0 · 2.50 · direct |
| `edge-at-mixed-n4` | 4 | yes | 124 · 0 · 2.50 · direct |
| `edge-at-step` | 1 | yes | 626 · 0 · 2.25 · direct |
| `edge-below-n1` | 1 | yes | 275 · 0 · 2.25 · direct |
| `edge-off-n1` | 1 | no | 339 · 0 · 4.00 · direct |
| `edge-copy-n1` | 1 | no | 340 · 0 · 0.25 · direct |

Phase D, storage that takes arbitrary pieces (NVMe/TCP):

| cell | conn | budget set | 16 KiB |
| --- | --- | --- | --- |
| `warmup` | 1 | no | 1598 · 0 · 6.75 · direct |

Phase TLP, storage that takes arbitrary pieces (NVMe/TCP):

| cell | conn | budget set | 16 KiB |
| --- | --- | --- | --- |
| `warmup` | 1 | no | 1814 · 0 · 4.30 · direct |
| `step-on-n16` | 16 | yes | 1081 · 0 · 15.80 · direct |
| `step-on-n16-quickack` | 16 | yes | 864 · 0 · 15.81 · direct |
| `step-on-n16-notlp` | 16 | yes | 1087 · 0 · 15.77 · direct |
| `step-rcvbuf-n16` | 16 | no | 1138 · 0 · 1.75 · direct |
| `mixed-on` | 16 | yes | 592 · 0 · 16.77 · direct |
| `mixed-on-quickack` | 16 | yes | 609 · 0 · 16.73 · direct |
| `mixed-rcvbuf` | 16 | no | 624 · 0 · 1.75 · direct |

Phase X, storage that takes arbitrary pieces (NVMe/TCP):

| cell | conn | budget set | 16 KiB |
| --- | --- | --- | --- |
| `warmup` | 1 | no | 1814 · 0 · 4.25 · direct |

### Phases X and D: a region client (R) beside a client that is not (N)

R MB/s · N MB/s · drops · alloc fail · region MiB.

Phase D, storage that needs whole pages (nvme-loop):

| cell | 16 KiB |
| --- | --- |
| `d-off-N` | - · 498 · 0 · 8618 · 23.73 |
| `d-on-N` | - · 709 · 0 · 0 · 16.69 |
| `d-on-RN` | 360 · 355 · 0 · 0 · 20.53 |
| `d-tight-N` | - · 132 · 0 · 0 · 4.00 |
| `d-tight-RN` | 64 · 64 · 0 · 0 · 7.50 |

Phase X, storage that needs whole pages (nvme-loop):

| cell | 16 KiB |
| --- | --- |
| `x-off-R` | 548 · - · 7471 · 7471 · 23.73 |
| `x-off-N` | - · 552 · 0 · 0 · 0.25 |
| `x-off-RN` | 304 · 418 · 7620 · 7620 · 23.73 |
| `x-on-R` | 716 · - · 0 · 0 · 16.45 |
| `x-on-N` | - · 551 · 0 · 0 · 0.25 |
| `x-on-RN` | 284 · 489 · 0 · 0 · 16.44 |
| `x-tight-R` | 127 · - · 0 · 0 · 3.75 |
| `x-tight-N` | - · 525 · 0 · 0 · 0.25 |
| `x-tight-RN` | 102 · 536 · 0 · 0 · 4.00 |

Phase D, storage that takes arbitrary pieces (NVMe/TCP):

| cell | 16 KiB |
| --- | --- |
| `d-off-N` | - · 455 · 0 · 7953 · 23.73 |
| `d-on-N` | - · 559 · 0 · 0 · 16.78 |
| `d-on-RN` | 313 · 315 · 0 · 0 · 22.14 |
| `d-tight-N` | - · 122 · 0 · 0 · 4.25 |
| `d-tight-RN` | 60 · 60 · 0 · 0 · 7.50 |

Phase X, storage that takes arbitrary pieces (NVMe/TCP):

| cell | 16 KiB |
| --- | --- |
| `x-off-R` | 447 · - · 7620 · 7620 · 23.73 |
| `x-off-N` | - · 544 · 0 · 0 · 0.25 |
| `x-off-RN` | 265 · 358 · 7479 · 7479 · 23.73 |
| `x-on-R` | 602 · - · 0 · 0 · 16.39 |
| `x-on-N` | - · 494 · 0 · 0 · 0.25 |
| `x-on-RN` | 278 · 521 · 0 · 0 · 16.33 |
| `x-tight-R` | 121 · - · 0 · 0 · 3.75 |
| `x-tight-N` | - · 483 · 0 · 0 · 0.25 |
| `x-tight-RN` | 98 · 513 · 0 · 0 · 4.00 |

### Phase N: a budget per network namespace

R MB/s · B MB/s · copied (no budget) · region MiB.

Phase N, storage that needs whole pages (nvme-loop):

| cell | 16 KiB |
| --- | --- |
| `n-own` | - · 730 · 0 · 10.45 |
| `n-default` | - · 736 · 0 · 10.56 |
| `n-copy` | - · 803 · 5570 · 4.03 |
| `n-both` | 560 · 544 · 0 · 16.72 |
| `n-reconf` | - · 706 · 0 · 10.61 |
| `n-exit` | - · 443 · 0 · 10.56 |

Phase N, storage that takes arbitrary pieces (NVMe/TCP):

| cell | 16 KiB |
| --- | --- |
| `n-own` | - · 797 · 0 · 8.83 |
| `n-default` | - · 800 · 0 · 8.84 |
| `n-copy` | - · 812 · 5570 · 3.84 |
| `n-both` | 598 · 587 · 0 · 14.48 |
| `n-reconf` | - · 732 · 0 · 9.61 |
| `n-exit` | - · 450 · 0 · 9.44 |

