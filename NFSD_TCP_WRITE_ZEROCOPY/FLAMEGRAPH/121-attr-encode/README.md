# Round 121: the NFSv3 post-op attribute encode made cheaper

Mike, 18 November: "implement the post-op attribute fast path and A/B
it".

## Why, and what it can save

Round 119 put a READ reply's post-op attributes at about 0.6 us of the
server's 18.4 a READ.  In round 120's profile (t2-r1015) they are 13,764
samples, 2.74%, 0.64 us: `vfs_getattr_nosec` and `xfs_vn_getattr` 30%,
`security_inode_getattr` (SELinux, `avc_lookup`) 22%, the uid and gid
mapping (`map_id_range_up`) 11%, the rest `fill_mg_cmtime`, the encode
and `fsid_source`.  The LSM check stays: dropping it would change what
the server enforces.  What a fast path can safely drop is about 0.15 to
0.2 us, about 1% of a READ, under the spread of the IOPS runs: the A/B
is judged by the profile's share, the IOPS beside it.

## The change

- `ed48261f60deb` "NFSD: Do not ask for direct I/O alignment for a
  reply's attributes": `__fh_getattr()`, whose one caller is the NFSv3
  post-op attribute encode, asked a regular file for STATX_DIOALIGN,
  STATX_DIO_READ_ALIGN and STATX_DIO_SEG_BOUNDARY, which XFS works to
  report and fattr3 does not carry.  `fh_getattr()` still asks: the file
  cache takes NFSD direct I/O's alignment from it when it opens a file
  (`nfsd_file_get_dio_attrs()`), and keeps it in the `nfsd_file`.
- `98af1fb580b6a` "NFSD: Map IDs in the initial user namespace without
  a lookup": a valid ID there is its own value.
- `65ca7a32eb480` "DROP: NFSD: A/B switch for FLAMEGRAPH round 121":
  `nfsd.attr_encode_ab`, Y as before both; to be dropped with the round.

Built into `/var/tmp/svcstack-541-attr` (sunrpc `28651531DD48DEE5D856DED`,
round 120's; nfsd `0BD7C610FFDEE571FEFA7B1`), W=1 on the two files, no
warning.

## The plan

Queued after round 120's WRITE rerun (t2-r1017):

    STACK=/var/tmp/svcstack-541-attr/stack SUNRPC_KO=none \
        ./step.sh /var/tmp/zc-run-logs/541 swap-attr ./swap-sunrpc.sh
    ./settled.sh apply
    ./write-check.sh 1018; ./read-check.sh 1019
    ../FLAMEGRAPH/121-attr-encode/ab-attr.sh 1020:new 1021:old 1022:new 1023:old
    ../FLAMEGRAPH/113-server-per-read/prof-srv.sh 1024          # new
    echo Y > /sys/module/nfsd/parameters/attr_encode_ab
    ../FLAMEGRAPH/113-server-per-read/prof-srv.sh 1025          # old
    echo N > /sys/module/nfsd/parameters/attr_encode_ab

The IOPS runs under `settled-iops.sh` as held (the spill, send_more Y,
the three borrows).  The two profiles give `__svcxdr_encode_post_op_attr`
in us a READ, new against old.
