# Round 54: `fh_verify`

**The target (Mike, 2 November):** about 3 us a WRITE, 3.3% of nfsd. Every WRITE and COMMIT decodes its file handle to a dentry and looks up the export, for a file the server resolved a moment ago, because `nfsd_file_do_acquire()` needs the inode to find the open file it has already cached.

**From fl53, % of nfsd:** `xfs_iget` 0.99, `d_obtain_alias` 0.31, the two export lookups 0.75, and `fh_put` only 0.30, so the release side is not the target.

## The trace first (run139)

No code until these are known:

1. How many distinct handles arrive, and do they repeat by CPU, by thread or only overall? Round 13's per-thread reuse cannot carry over if each of 512 threads sees them in turn.
2. How long between uses of a handle on one CPU? It says how cold any per-CPU cache would be.
3. What does a call cost when no probe is on it? PMU counts per function give IPC and L2 refills, to tell a walk from a miss. Only the walk can be cached away: a line bouncing between CPUs may bounce in the cache too.

`fh.bt` answers 1 and 2, with inflated times, in one loaded run. `perf record` on nfsd's CPUs answers 3 in another, with no probes and no call chains.

**What went wrong first.** run139's `fh.bt` also timed each step with kretprobes. bpftrace 0.26 joins a kprobe and a kretprobe of one function into a session and runs the kprobe's body at both ends, and never the kretprobe's (shown on `do_sys_openat2`, 5 runs of a process, 20 hits for 10 calls). So its counts were doubled, half its handles were a clobbered `arg5`, and its "same as the last" figures could only read 0. `fh-run139-spoiled.txt` is kept for that. run140 is the trace without the timing. run139's `perf record`, a separate load, stands.

## What the trace found (run140, 8 s; run139's PMU)

| of 348052 handle decodes | |
|---|---|
| distinct handles | 16, one to a fio file, 21.3k to 22.0k each |
| CPUs that used each handle | all 48 |
| same as this thread's last | 6.1% |
| same as this CPU's last | 6.1% |
| same as anyone's last | 6.2% |
| 1 in 16, by chance | 6.25% |
| a handle's last use on this CPU | typically 4 to 32 ms ago |
| a handle's last use on any CPU | typically 128 us to 1 ms ago |

**A handle does not repeat where a cheap cache could see it.** Round 13's reuse per thread would hit at chance, 6%. A table of all 16 per CPU would hit every time, but each entry would be about 10 ms cold, and another CPU would have used that file in the last 300 us or so. This is the rig's load: 16 files spread evenly over 48 CPUs. A load with one hot file would repeat far more, and it was not measured.

**The cost is memory, not work.** From `pmu.txt`, with no probes:

| | % of cycles | IPC | L2 refills a thousand instructions |
|---|---|---|---|
| all of nfsd's CPUs | 100 | 1.26 | 14.6 |
| `svc_tcp_rx_append_ref`, for scale | 2.29 | 3.04 | 2.9 |
| `sunrpc_cache_find_rcu` | 0.56 | 0.69 | 24.6 |
| `nfsd_set_fh_dentry` | 0.30 | 0.57 | 52.8 |
| `__radix_tree_lookup` | 0.20 | 0.51 | 68.0 |
| `lockref_put_return` | 0.19 | 0.27 | 128.1 |
| `xfs_iget` | 0.17 | 0.39 | 94.9 |
| `xfs_iget_cache_hit` | 0.12 | 0.18 | 131.3 |
| `exportfs_decode_fh_raw` | 0.09 | 0.23 | 130.3 |
| `cache_check_rcu` | 0.05 | 0.09 | 216.0 |

These are short paths that stall. A refill here is often a line another CPU wrote: `dentry` lockrefs, the inode's locks and count, `nfsd_file` refcounts, and the export caches' krefs, all written by whichever of 48 CPUs came last. Refills do not say which, and the PMU cannot tell a bounced line from a cold one here: that is inferred from the gaps above.

## What it means for a fix

**A cache of handles is not worth it on this evidence.** Its table would be cold too, it would have to keep the permission checks, and it would still take `nfsd_file_get()`, one more write to a shared line. The saving would be a fraction of the 2.2 us that can be skipped at all, at the price of holding dentries and exports across unexport and unmount, which upstream would look at hardest. No code was written for it.

**The better target is the export lookup's references.** `exp_find()` takes a reference on the `svc_expkey`, which it holds only across `exp_get_by_name()`. That is 2 atomic writes a call on one of 4 entries from 48 CPUs, and `struct cache_head` keeps the `kref` on the line with `expiry_time`, `last_refresh` and `flags`, which every lookup reads. That fits `cache_check_rcu`'s 0.09 IPC, the lowest of the path. The expkey has nothing to hold it for on the fast path, so its line could go read-mostly: its lookup, about 0.35 us by fl52's `exp_find_key`, is what might come back, an estimate.

It is not a one-liner. `svc_export_lookup()` may allocate on a miss, so the fast path needs a lookup that does not take a reference and does not allocate, in sunrpc's cache and nfsd's export code, falling back to today's path on a miss, an entry not valid or one due for refresh. So it swaps both `sunrpc.ko` and `nfsd.ko`.

`svc_export`'s own reference stays, since the file handle holds it for the whole request.

## The change (Mike, 2 November)

`b91623167b04` adds `sunrpc_cache_peek_rcu()`, which returns only what `cache_check()` would pass with nothing to do: valid, positive, not expired or flushed, not due for a refresh. `1ffe1b8da005` uses it in a new `exp_find_rcu()`, for the key and for the export, and takes a reference on the export alone; anything else takes today's path, including the upcall.

**Why it is safe:** `svc_expkey` is freed by `kfree_rcu()`. A key may have dropped its path by the time it is read, but `svc_export_match()` and `svc_export_hash()` only compare and hash the pointers, and the export found holds that path itself. Skipping `cache_put()` skips its update of `nextcheck`, which `cache_clean()` and `sunrpc_cache_update()` keep too.

Test: `svcsock_cache_peek_test`, with a fake one-bucket cache. `svcsock_kunit` has 56 tests.

`run141.sh` checks it before any A/B: KUnit, two mutants, the paths a second under load (`paths.bt`) with a flush and keys set to expire in 14 s in the window, and an unexport seen by a live mount. So this round swaps `nfsd.ko`: `build-nfsd.sh` needs `KBUILD_EXTRA_SYMBOLS` for the new export.

## Before the A/B

On `sunrpc` `1C40775238B884E7ED53863` and `nfsd` `FB4139BC86377896E24C64D`, `t2-r141/`:

- **KUnit:** svcsock 56/56, twice. M1, which lets an invalid or negative entry through, and M2, which ignores a refresh due, each failed `svcsock_cache_peek_test` alone.
- **Under load:** about 43k lookups a second, none slow outside the events below, 40.50 GB/s, no fio error, by `paths.bt`, whose probes cost the run a little.
- **`exportfs -f` in the window:** one second of 15 slow lookups and 12 upcalls, then fast again.
- **A key due for refresh (`run141b.sh`):** set to expire in 16 s, it went on the fast path until 9 s, just past half its life, then 2 slow lookups and 2 upcalls, `mountd` answered, and fast again, with no write failing. run141's own attempt lost this to the channel's format: an fsid goes down it as raw bytes in hex.
- **An unexport on a live mount:** a write worked, `exportfs -u` and `-f` gave `Permission denied` at once, and exporting again gave a working write.

So each way out of the fast path has run.

`run142.sh` is the A/B: both sides on the new sunrpc, A the kernel's nfsd, B this one. Latency runs in the same A B B A B A A B, since A B B A alone misled rounds 52 and 53.
