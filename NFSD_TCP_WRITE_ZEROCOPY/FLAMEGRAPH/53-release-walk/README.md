# Round 53: the table walk at release

**The target (Mike, 2 November, from fl52):** `svc_tcp_rx_refs_valid()`, 0.75% of nfsd, about 0.7 us a WRITE, walks every entry of a record's table before the release gives its pages back. For a published record that repeats `svc_tcp_rx_validate_mixed()`, the same kind of repeat as round 52.

## The change

`SUNRPC: Do not check a published record's bvec table again at release`: the walk is skipped when `state->mode` is `SVC_TCP_RX_PUBLISHED`. Mode is set to that only after `svc_tcp_rx_validate_mixed()` has passed, which checks every entry, more strictly than this walk, and round 52 showed the table cannot change after that. Only `svc_tcp_release_ctxt()` and `svc_tcp_rx_release_payload()` reach the release with a published record. An abort, `svc_tcp_rx_state_free()` and the copy path keep the walk. The counts and bitmaps are checked for every record.

This undoes part of what round 02 left: each entry is still checked once, at publish in place of at release.

**What may go wrong:** the walk is the first thing to touch the table on the CPU doing the release, and the release's own loop reads it right after, so its misses may just move there, as round 23's did. The A/B decides.

The test is `svcsock_rx_release_published_test`. Two mutants: one that skips the walk for every record, one that never does.

## Before the A/B

The svcsock suite on B 55/55, twice, and the stock four suites 10/10, 55/55, 7/7 and 10/10, `KUNIT PASS`, no warning. Mutant M1, which skips the walk for every record, failed 4 expectations of `svcsock_rx_release_published_test`, and M2, which never skips it, failed 2, each that test alone (`$L/t2-r137/mutants.out`).

## The A/B (run137)

Build 506 with sunrpc swapped before each run, A `5D39507E93127A6010C9B8E`, round 52's kept module, B `5DDCC44639FC73AF533513F`, in the order A B B A B A A B, one port, `$L/t2-r137/`, all 8 no-step, no-stall, alloc_fail 0, no kernel warning.

| run | GB/s | rings cycles (instr) | nfsd cycles (instr) | peak | mean in use |
|---|---|---|---|---|---|
| 1 A | 40.51 | 127.8k (282.7k) | 281.6k (274.3k) | 6265 | 5056 |
| 4 A, the dip | 40.40 | 125.7k (281.8k) | 277.1k (273.6k) | 6198 | 5191 |
| 6 A | 40.48 | 127.8k (282.5k) | 281.7k (274.7k) | 6133 | 5031 |
| 7 A | 40.50 | 128.1k (283.0k) | 282.2k (274.4k) | 6059 | 5024 |
| **A mean** | **40.47** | **127.4k (282.5k)** | **280.7k (274.3k)** | **6164** | **5076** |
| **A mean without run 4** | **40.50** | **127.9k (282.7k)** | **281.8k (274.5k)** | **6152** | **5037** |
| 2 B | 40.45 | 127.9k (282.8k) | 280.1k (272.6k) | 6047 | 4924 |
| 3 B | 40.50 | 128.0k (282.9k) | 279.9k (271.3k) | 6330 | 4982 |
| 5 B | 40.61 | 127.7k (282.5k) | 280.0k (270.2k) | 6135 | 5263 |
| 8 B | 40.59 | 127.3k (282.5k) | 279.9k (270.9k) | 6162 | 5236 |
| **B mean** | **40.54** | **127.7k (282.7k)** | **280.0k (271.3k)** | **6169** | **5101** |

**nfsd -3.0k instructions a WRITE (-1.1%), no overlap between the sides.** The cycles need run 4 explained: it held one second at 36.3 GB/s (second 22 of `wire-sec.tsv`), the only second of the eight runs below 39.8, with no drive step and no stall, and its rings and nfsd both fell with its rate, as round 52's run 6 did. Without it, **nfsd -1.9k cycles (-0.7%), no overlap**, against about 2.1k expected from the frame. With it, -0.7k, and its 277.1k lies below every other run. The cause of these dips is still not found.

The rings and NVMe interrupts did not change. Peak +5 pages and mean in use +25 against all of A, +64 against A without run 4, in a scatter of 339 between B's own runs: the region is not raised.

## Latency at iodepth 1, in both orders

Rounds 52 and 53 both ran A B B A and both read B higher, 15 and 20 us of p50, with the same order each time, so run138 ran B A A B on the same two modules.

| position | A B B A (run137) | p50, ms | mean | B A A B (run138) | p50, ms | mean |
|---|---|---|---|---|---|---|
| 1 | A | 0.432 | 0.440 | B | 0.436 | 0.445 |
| 2 | B | 0.449 | 0.461 | A | 0.420 | 0.428 |
| 3 | B | 0.444 | 0.457 | A | 0.444 | 0.454 |
| 4 | A | 0.420 | 0.428 | B | 0.420 | 0.434 |

**The order accounts for it, or much of it.** A's four average p50 0.429 and mean 0.438, B's 0.437 and 0.449. Each module read both 0.420 and at least 0.444, and position 4 read 0.420 on either side, so 8 us is well inside what one module does from run to run. No maximum passed 1.7 ms. What moves a run between about 0.420 and about 0.444 was not looked into.

**It also answers round 52.** A here is round 52's kept module, and its 0.429 is below the 0.434 of the module before it, so the 449 that round read was position and scatter too.

## The profile (fl53, on B, against fl52)

| % of nfsd | fl52 | fl53 |
|---|---|---|
| `svc_tcp_rx_refs_valid`, inclusive | 0.76 | 0.09 |
| `svc_tcp_rx_release_refs`, self | 1.66 | 1.84 |
| `svc_tcp_rx_release_refs`, inclusive | 6.15 | 5.64 |
| `svc_tcp_rx_put_run`, inclusive | 3.16 | 3.17 |

As feared, the release's own loop took some of the misses: 0.18 of the 0.67 points moved there, about a quarter, and the release is 0.51 points cheaper.

**Kept:** `3ddd62ce0809`, with its test `5e07fe85df33`. `updates/sunrpc.ko` is B.

Copy was not run.
