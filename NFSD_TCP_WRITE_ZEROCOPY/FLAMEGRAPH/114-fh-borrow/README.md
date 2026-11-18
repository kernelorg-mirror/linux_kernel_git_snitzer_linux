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
unchanged; nfsd `EA90A54F716F92B04DE45E6` before the sweep was factored,
rebuilt below), W=1, no warning; the test nfsd with the suite in
`nfsd-ebt/`.

## The plan

1. The suite on the host, as round 111: `kunit-export.sh
   /var/tmp/svcstack-541-fhb` (the test nfsd, kunit filtered to the
   suite; it puts the stack's own nfsd back, this round's).
2. `settled.sh apply`; the WRITE pass `write-check.sh 973` and the READ
   pass `read-check.sh 974` (the change is on the path both take).
3. The A/B: `ab-fh.sh 975:16 976:0 977:16 978:0 979:16 980:0`.
