# Round 122: the fh borrow looked up in one set

Mike, 18 November: "the fh borrow using a hash seems like a quick win",
then "just do a quick A/B, spare me all the overkill in testing".

`8c924c2147dec` "NFSD: Look up a borrowed file handle in one set of the
slot": the 48 entries a thread as 12 sets of 4 by a hash of the handle,
so a lookup past the MRU entry reads one set, not every entry (each its
own cache line); round 119 put `nfsd_fh_borrow` at about 0.2 us a READ.
`003a55b14db27` rewrites the KUnit case for eviction to the sets.  Built
into `/var/tmp/svcstack-541-fhset` (nfsd `4731FE33B5DE44840A066EB`), W=1
clean.

The A/B: one `prof-srv.sh` run on it (t2-r1026) against round 121's
t2-r1024 (the same setting on the nfsd before it), with the borrow's
counters over the run for the sets' misses and evictions.

## Result: t2-r1026 (18 November, 07:42 to 07:46)

2.71 M IOPS (1.40 / 1.32), p99 3.16 / 3.19 ms, server 79% busy, no new
kernel warning; t2-r1024 2.71 M.  `nfsd_fh_borrow` 0.74% against 0.87%:
**about 0.03 us a READ saved**.  But the sets miss: over the run fh_hit
70.7 M, fh_miss 963 K, of them 879 K evictions (98.7% hits, where the
scan of all 48 entries kept 99.8% with no eviction).  Each miss is a full
decode (about 1.7 us), about 0.03 us a READ: the saving goes back in
misses.  A slot sees about as many distinct files as it has entries, and
four ways do not hold them.

## The sets dropped; a tag array instead

Mike: "revert the sets and implement the tag array".  The two commits
were dropped from the branch by rebase (the tip before is
`kernel-7.3/canary.pre-drop-fh-sets`).  In their place "NFSD: Scan a tag
for each borrowed file handle, not the handle": a word a entry, a hash of
the handle, scanned past the MRU entry, an entry read only on a tag
match; any entry still takes any handle, so no conflict misses.  48
tags are three cache lines.  Built into `/var/tmp/svcstack-541-fhtag`
(nfsd `6C1DB2E1B6B2CC84026E7B1`), W=1 clean.  The KUnit cases stand as
they were.  One `prof-srv.sh` run (t2-r1027) against t2-r1024.

## Result of the tag array: t2-r1027 (18 November, 07:51 to 07:54)

2.74 M IOPS (1.45 / 1.30), p99 2.93 / 3.29 ms, server 77% busy, no new
kernel warning.  fh_hit 72.6 M, fh_miss 109 K, no eviction (99.85%).
`nfsd_fh_borrow` **0.88%**, against 0.87% with the scan (t2-r1024): no
saving.  `perf annotate`: the function's samples are on loads every hit
makes anyway, the matched entry's dentry (about 24%), the MRU entry's
(10%), the inode's i_nlink for the unlinked check (about 14%); the scan
was never the cost.  Proposed to Mike: revert it too.
