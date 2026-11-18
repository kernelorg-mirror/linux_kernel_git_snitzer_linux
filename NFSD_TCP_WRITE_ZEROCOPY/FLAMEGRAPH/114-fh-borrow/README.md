# Round 114: the file handle borrow

Mike, 18 November: "yes, implement the file handle borrow".

## Why

Round 113 (t2-r972) found the server the limit at 2.54 M 4 KiB READs a
second, about 18 us of CPU a READ spread over all 48 work CPUs, and in
it every READ decoding its file handle again: `exportfs_decode_fh()` ->
`xfs_fs_fh_to_dentry()` -> `xfs_iget()` (a radix lookup, `igrab()`) and
`d_obtain_alias()`, then the puts at `fh_put()`, taking the file's
inode locks each way (the largest leaf of the lookup's 1.7 us).

## The change

`ecd2a7522e0ee` "NFSD: Lend each thread's recently decoded NFSv3 file
handles": the export borrow's slot keeps, beside its exports, the
dentries of the file handles it decoded last (`fh_borrow_slots`, 16).
With an export lent, `nfsd_set_fh_dentry()` looks there before decoding;
a hit takes a reference on the kept dentry (the svc_fh owns its dentry
as before; `fh_put()` is unchanged).  Only regular files under
`no_subtree_check` exports that do not sign their handles (twoport.sh
exports `no_subtree_check`).  A hit matches the whole file id and the
lent export, and misses once the file is unlinked.  An export's kept
handles are dropped before the slot's reference on it; every drain drops
them; a sweep drops those unused for `fh_borrow_ttl` (2 s).  debugfs
`export_borrow` gains `fh_hit fh_miss fh_fill fh_evict fh_stale
fh_aged`.  KUnit: `7fca301ca0d68` (interface: the sweep's pass and the
ttl for the suites), `b6b2f9e6a6147` (ten cases in
`sunrpc-nfsd-export-borrow`, 42 in all).

Built into `/var/tmp/svcstack-541-fhb` (sunrpc `85537945BAC1207EDA22E45`,
unchanged; nfsd `DFC417DB5E771ABF21E2D4D`, the tip with the sweep factored),
W=1, no warning; the test nfsd with the suite in
`nfsd-ebt/`.

## The plan

1. The suite on the host, as round 111: `kunit-export.sh
   /var/tmp/svcstack-541-fhb` (the test nfsd, kunit filtered to the
   suite; it puts the stack's own nfsd back, this round's).
2. `settled.sh apply`; the WRITE pass `write-check.sh 973` and the READ
   pass `read-check.sh 974` (the change is on the path both take).
3. The A/B: `ab-fh.sh 975:16 976:0 977:16 978:0 979:16 980:0`.

## The suite (18 November, 04:35)

`kunit-export.sh /var/tmp/svcstack-541-fhb`: first 41 of 42; `not ok 38
ebt_fh_drain` read the dentry's count after the drain had dropped the
last reference (a dead lockref, 0xffffff80): the case's mistake, fixed
in `b4e35ad8e93fd` (it holds its own reference across the drain).
Then **`sunrpc-nfsd-export-borrow` 42 of 42, no `WARNING:`, `BUG:` or
`Oops`**; the stack's own nfsd back (`DFC417DB5E771ABF21E2D4D`,
`fh_borrow_slots` 16), the settled setting.  Next: the passes and the
A/B, steps 2 and 3.

## The table's size: 64 against 16 (plan)

The passes' counters showed nearly every handle miss evicting a live
entry at 16 (WRITE pass 54% hits, 2.31 M evictions; READ pass 92%, 1.56
M), where the export side evicted none.  Mike, 18 November: "after the
A/B, run fh_borrow_slots 64 against 16":

    ../FLAMEGRAPH/114-fh-borrow/ab-fh.sh 981:64 982:16 983:64 984:16 985:64 986:16

The 16-against-0 A/B was cut after t2-r976 (Mike: "stop after r976 and
go straight to the 64 against 16 runs"), the off side's baseline being
rounds 112 and 113's runs of the same shape on the stack before the
change (t2-r966, r968, r970, r972: 2.54 to 2.55 M).
