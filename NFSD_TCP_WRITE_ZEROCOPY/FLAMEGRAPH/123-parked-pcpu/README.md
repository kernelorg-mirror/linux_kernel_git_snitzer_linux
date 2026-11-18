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
