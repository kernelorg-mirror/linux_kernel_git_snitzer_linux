# Round 54: `fh_verify`

**The target (Mike, 2 November):** about 3 us a WRITE, 3.3% of nfsd. Every WRITE and COMMIT decodes its file handle to a dentry and looks up the export, for a file the server resolved a moment ago, because `nfsd_file_do_acquire()` needs the inode to find the open file it has already cached.

**From fl53, % of nfsd:** `xfs_iget` 0.99, `d_obtain_alias` 0.31, the two export lookups 0.75, and `fh_put` only 0.30, so the release side is not the target.

## The trace first (run139)

No code until these are known:

1. How many distinct handles arrive, and do they repeat by CPU, by thread or only overall? Round 13's per-thread reuse cannot carry over if each of 512 threads sees them in turn.
2. How long between uses of a handle on one CPU? It says how cold any per-CPU cache would be.
3. What does a call cost when no probe is on it? PMU counts per function give IPC and L2 refills, to tell a walk from a miss. Only the walk can be cached away: a line bouncing between CPUs may bounce in the cache too.

`fh.bt` answers 1 and 2, with inflated times, in one loaded run. `perf record` on nfsd's CPUs answers 3 in another, with no probes and no call chains.
