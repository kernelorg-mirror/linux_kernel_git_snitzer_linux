# Round 79: where `__rx_loan_table_append_ref()` and `rx_loan_table_release()` spend

Round 77 found these two the heaviest per-entry functions of the loan's
bookkeeping, at high IPC, and asked for an annotation before any change.
One counted run on the canary's own stack, as round 74's, that keeps its
`perf.data` ([run334.sh](run334.sh), [run334.out](run334.out)):
`t2-r334`, 69.45 GB/s, far shares 2.7% and 1.5%, no new kernel warning.
`__rx_loan_table_append_ref` 4230 cycles a WRITE, `rx_loan_table_release`
3272.  The data is `/var/tmp/zc-run-logs/t2-r334/pmu.data` (103 MB, kept
for more annotation); `perf annotate --stdio --percent-type local-period`
with the debug vmlinux, and source lines by `addr2line` on
`/usr/lib/debug/.../vmlinux` and `sunrpc.ko.debug`
([annotate-__rx_loan_table_append_ref.txt](annotate-__rx_loan_table_append_ref.txt),
[annotate-rx_loan_table_release.txt](annotate-rx_loan_table_release.txt):
cycles, instructions, L1D and L2D refills in turn).

Without SPE a sample is taken on the instruction after the one that held
it up, so each hot spot below is read as the load just before it.

## `__rx_loan_table_append_ref()` (in `sunrpc.ko`, inlined from `rx_loan.h`)

| after the load of | `rx_loan.h` | cycles | L2D refills |
|---|---|---|---|
| `t->bvec[t->count - 1].bv_page`, in `rx_loan_table_can_coalesce()` | 179-180 | **18%** | **23%** |
| the `t->page_pool` bitmap's word, `__set_bit()` | 269 | 8% | 10% |
| the `t->borrowed` bitmap's word, `__set_bit()` | 259 | 7% | 11% |
| the page's pool's `rx_region`, `page_pool_page_in_rx_region()` | 263 | 4% | 4% |
| `t->count` and the table's other counts | | about 8% | about 9% |

**About 45% of its cycles, near 1.9k a WRITE, wait on the loan table's
own memory** (the bvec array and its two bitmaps), not on the struct
pages being loaned, which `rx_loan_prefetch_pages()` has already fetched.
The first is the entry the previous append wrote, so if it misses, the
entry was written somewhere else or long before.

## `rx_loan_table_release()` (`net/core/rx_loan.c`)

| at | `rx_loan.c` | cycles | L2D refills |
|---|---|---|---|
| `prefetchw(t->bvec[index + RX_LOAN_PUT_BATCH].bv_page)` in the loop | 310 (267) | **13%** | 10% |
| `prefetchw(t->bvec[index].bv_page)`, the first batch | 305 | 5% | **12%** |
| `rx_loan_prefetch_again()` | 307 (267) | | 8% |
| `netmem_compound_head()` of the page | 325 | 7% | 7% + 5% |
| the batch's loop control | 312 | 7% + 4% | 7% + 3% |
| the run of a page's references | 327-330 | 5% + 6% | 4% + 4% |

A prefetch does not stall, but it waits for its address: the samples on
the `prfm`s are the loads of `t->bvec[i].bv_page` they need.  **So about
30% of release, near 1.0k a WRITE, waits on the same bvec array**, and
less on the pages it fetches (line 325).

## What it says

The bookkeeping is not slow in its instructions: about **2.9k cycles a
WRITE of the two functions' 7.5k are misses on the table's own lines**,
at receive and at release.  A WRITE's record arrives in about 9 pieces
(9.1 enqueues and 5.8 wakes a WRITE), each taken by whichever nfsd
thread is woken, and the record is released by the thread that ran the
WRITE.  If those are on different CPUs, each one finds the table's lines
in another CPU's cache.  That is a guess the profile cannot confirm.

## Suggested next (round 80)

Measure before changing anything: for each loaned record, the CPU of
each `svc_tcp_recv_skb()` piece and of `svc_tcp_rx_release_refs()`,
keyed by the table (a bpftrace or kprobe trace on one port for a few
seconds), and the NUMA node of the table's memory
(`rx_loan_table_alloc()`'s `node`) against those CPUs.  If pieces move
between CPUs, the candidates are: prefetching the table's tail (the last
bvec line and the two bitmap words) when a thread takes up a partly
received record, the way release prefetches its pages; or keeping a
record's pieces on one thread.  Neither holds a region page longer.
