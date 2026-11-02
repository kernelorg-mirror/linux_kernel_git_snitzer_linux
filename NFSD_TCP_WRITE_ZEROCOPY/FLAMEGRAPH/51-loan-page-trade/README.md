# Round 51: the page trade of a parked loaned record

**The target (round 50):** `svc_tcp_save_pages()` and `svc_tcp_restore_pages()` trade pages by `sk_datalen`, as if a record's bytes were in `rq_pages`. Mike, 2 November: fix it if no new kernel is needed. Sunrpc only, so a module swap tests it; copy is not run.

## What a parked record holds (run129)

`pagetrade.bt`, 8 s in a loan64 run's window, so a look (39.87 GB/s traced, not a run's figure):

| | |
|---|---|
| WRITEs published | 317,926 |
| parks | 1,237,537, 3.9 a WRITE |
| take-ups | 1,190,115 |
| slots a park walks, by `sk_datalen` | 34.2M, 27.6 a park |
| slots with any of the record in them | 1.24M, **1 a park** |
| records with no copied bytes | 1,237,509, **99.998%**; the other 28 are in `COPY` mode or hold a few copied bytes |

`svc_tcp_rx_append_copied()` never ran in the window, so no arena copy at all. A loaned record's body is in the region, and all it has in `rq_pages` is its fixed bytes, in page 0.

**A copied piece has to move with the record.** A bvec entry for one holds the `rq_pages[i]` page itself, so a trade that left it in the thread's array would reuse a page the record still points at, and that is silent corruption of file data. So the rule cannot be "loans need one page"; it is that a `MIXED` record with `copied_bytes` 0 has only page 0. A merge page's bytes count as copied, so a record with one keeps the full trade. A `MIXED` record that later copies makes `copied_bytes` non-zero before its next park, and the pages it copied into are in the thread that holds it then.

## The change

`7ca95cca4c1a`, "SUNRPC: Trade only the pages a partly received loaned record holds": `svc_tcp_rx_held_pages()` gives 1 for a `MIXED` record with `copied_bytes` 0 and `sk_datalen`'s pages otherwise, for the save, the restore and `svc_tcp_clear_pages()`, which counts before its abort empties the state. No field is added, so nfsd is untouched.

The tests are `svcsock_rx_park_loaned_test` and `svcsock_rx_park_copied_test`. A mutant that drops the `copied_bytes` test is the corruption above: `park_copied` has to fail on it, or the test shows nothing.

## The tests

Three sunrpc builds, `build-sunrpc.sh`'s way, one source but for `svcsock.c`: A without the change, B with it, M the mutant. On B the svcsock suite ran 54/54, no warning. On M it ran 53, and only `park_copied` failed, on its 4 expectations, as it must.

## The A/B (run130)

Build 506 with sunrpc swapped before each run, A `866D005D04BF594771A332C`, B `167B8BE080CE8001F6D7E4A`, in the order A B B A B A A B, one port, 256 threads, `$L/t2-r130/`, all 8 no-step, no-stall, placed, alloc_fail 0.

| run | GB/s | rings cycles (instr) | nfsd cycles (instr) | peak | mean in use |
|---|---|---|---|---|---|
| 1 A | 40.56 | 127.8k (282.8k) | 290.5k (299.6k) | 6153 | 5088 |
| 4 A | 40.50 | 128.2k (283.0k) | 289.9k (300.0k) | 6003 | 4919 |
| 6 A | 40.52 | 128.0k (283.2k) | 289.5k (300.0k) | 6146 | 5013 |
| 7 A | 40.51 | 127.8k (282.9k) | 289.0k (299.3k) | 6004 | 4930 |
| **A mean** | **40.52** | **128.0k (283.0k)** | **289.7k (299.7k)** | **6077** | **4988** |
| 2 B | 40.54 | 128.5k (283.1k) | 287.7k (297.2k) | 6182 | 5073 |
| 3 B | 40.49 | 127.7k (282.8k) | 285.7k (296.3k) | 6061 | 4933 |
| 5 B | 40.52 | 127.8k (282.9k) | 287.5k (296.6k) | 6021 | 4919 |
| 8 B | 40.53 | 128.4k (282.9k) | 284.7k (295.6k) | 6103 | 5034 |
| **B mean** | **40.52** | **128.1k (282.9k)** | **286.4k (296.4k)** | **6092** | **4990** |

**nfsd -3.3k cycles and -3.3k instructions a WRITE, -1.1%, and no run of B reaches a run of A in either** (cycles: B at most 287.7k, A at least 289.0k; instructions 297.2k and 299.3k). The rings and the rate do not move. The busy us/W figure is too noisy to read here (A 86.5 to 88.8, B 83.3 to 91.5); the counters are the measure.

The region is not raised: peak +15 and mean in use +2 pages, against a scatter of about 150, and round 50's peak was 5983 to 6224. The change holds no page longer: a parked loaned record holds the one page it did, and the spare it leaves at the socket is one page and not up to 64.

Latency at iodepth 1, A B B A: p50 0.424, 0.436, 0.424, 0.436 ms, p99 0.545, 0.569, 0.569, 0.578, mean A 0.442, B 0.441. The same; these runs differ in the order they ran, not by side.

## The profile (fl51, on B, against fl50)

| % of nfsd, inclusive | fl50 | fl51 | us a WRITE, fl50 to fl51 |
|---|---|---|---|
| `svc_tcp_restore_pages` | 1.18 | 0.43 | 1.11 to 0.38 |
| `svc_tcp_save_pages` | 0.39 | 0.13 | 0.37 to 0.12 |
| `svc_fill_pages` | 0.87 | 0.79 | 0.82 to 0.70 |

**So round 50's 2.3 us was too high: about 1 us came out, the save and the restore.** That is the A/B's 1.1% of about 89 us. `svc_fill_pages` hardly moved. Round 50 counted it in because `rq_pages_nfree = npages` asks for a refill, but a refill of entries still filled costs next to nothing, so what it pays for lies elsewhere.

**Kept:** `7ca95cca4c1a` and its tests, for a saving small but clean of the scatter, and for no cost.

Copy was not run.
