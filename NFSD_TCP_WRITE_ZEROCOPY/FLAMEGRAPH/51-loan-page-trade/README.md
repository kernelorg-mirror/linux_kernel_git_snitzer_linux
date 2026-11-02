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

## The runs, to do

Three sunrpc builds, `build-sunrpc.sh`'s way, one source but for `svcsock.c`: A without the change, B with it, M the mutant. `$L/t2-r130/`.
