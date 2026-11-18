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
