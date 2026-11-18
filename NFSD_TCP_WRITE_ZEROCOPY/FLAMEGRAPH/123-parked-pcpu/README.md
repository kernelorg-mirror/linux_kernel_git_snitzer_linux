# Round 123: the rtc pool's parked count per CPU

Mike, 18 November: "revert the tag array and look at park/unpark".

Round 119 put the rtc work items' park and unpark at about 0.56 us a
READ.  `perf annotate` on t2-r1027: 95.7% of `svc_rtc_park`'s samples
on the instruction after its `atomic_inc_return(&pool->parked)`, 97.9%
of `svc_rtc_unpark`'s on the load after its `smp_mb__after_atomic()`,
which waits for its `atomic_dec(&pool->parked)`: one atomic for the
whole service, written twice a READ from every CPU.

`5e3fbfc0a2895` "SUNRPC: Count the rtc pool's parked calls per CPU": a
`percpu_counter` (batch 64), read approximately for the per-call
ceiling and the high water, summed exactly for the pool's growth and
decay.  `dd1b3b59fe056` has the rtc suite read it with
`percpu_counter_sum()` (built, not run: the suite stays off tardis2).
Built into `/var/tmp/svcstack-541-parkpc` (sunrpc
`FFEE48BFDC01865BA2CA28A`, nfsd `7CD98A622C9D89E1DF46039`), W=1 clean.

The quick A/B (Mike: no overkill): one `prof-srv.sh` run (t2-r1028)
against t2-r1027.

## Result: t2-r1028 (18 November, 08:04 to 08:08)

**2.83 M IOPS** (1.42 / 1.40), p99 3.06 / 3.29 ms, server 78% busy, no
new kernel warning: the most yet at this shape (t2-r1027 2.74, the
spill's runs 2.71 to 2.78).  Park and unpark, now inlined beside their
`percpu_counter_add_batch()`: 0.26% (park, under
`svc_rtc_after_process`) and 0.24% (unpark), **0.50% against 2.76%**
in t2-r1027, about **0.5 us a READ saved** (1% is 0.226 us at 2.83 M).

The rest of `percpu_counter_add_batch` (1.85% in all): nfsd's read
bytes from `nfsd_finish_read` 0.68%, `svc_xprt_recv_call` 0.32%, the
DRC's from `nfsd_cache_lookup` 0.23%.  A byte count of 4096 a READ is
past the default batch, so each such add folds into the shared count
under its spinlock: the next shared line to look at.
