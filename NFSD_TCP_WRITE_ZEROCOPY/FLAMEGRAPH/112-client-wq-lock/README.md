# Round 112: the client's workqueue lock at 2.5 M IOPS

Mike, 18 November: "profile the client's workqueue lock at 2.5 M IOPS".

## Why

Round 111's profiles (t2-r963, t2-r964) put about 10% of the client's
samples in spin locks (`_raw_spin_lock` 5.75%,
`native_queued_spin_lock_slowpath` 3.89%), most of them under
`queue_work_on` (6.16% with children), the lock of a workqueue pool as
the client queues work for rpciod and xprtiod.  Round 108 found that lock
the limit on build 469 (63.5% of the client's cycles); build 541's
canary-WIP commits cut it, and at 2.5 M IOPS it is back.  This round
finds which pool's lock, who takes it and how long they wait.

## The run

On the server at the tip's stack (`/var/tmp/svcstack-541-kunit`),
both borrows held by `settled-iops.sh`; the client on build 541,
`cache_shard`.  `prof-wq.sh 965`: one 4 KiB READ run, RUNTIME=35, and on
the client from the load's second 8, one after another: `perf record -a
-g` (6 s), `perf lock contention -a -b -v` (6 s, by caller with stacks),
`perf lock contention -a -b -l` (6 s, by lock).  Its rate is taken
under the probes.
