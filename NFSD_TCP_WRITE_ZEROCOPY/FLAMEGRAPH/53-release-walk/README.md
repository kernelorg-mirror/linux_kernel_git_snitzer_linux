# Round 53: the table walk at release

**The target (Mike, 2 November, from fl52):** `svc_tcp_rx_refs_valid()`, 0.75% of nfsd, about 0.7 us a WRITE, walks every entry of a record's table before the release gives its pages back. For a published record that repeats `svc_tcp_rx_validate_mixed()`, the same kind of repeat as round 52.

## The change

`SUNRPC: Do not check a published record's bvec table again at release`: the walk is skipped when `state->mode` is `SVC_TCP_RX_PUBLISHED`. Mode is set to that only after `svc_tcp_rx_validate_mixed()` has passed, which checks every entry, more strictly than this walk, and round 52 showed the table cannot change after that. Only `svc_tcp_release_ctxt()` and `svc_tcp_rx_release_payload()` reach the release with a published record. An abort, `svc_tcp_rx_state_free()` and the copy path keep the walk. The counts and bitmaps are checked for every record.

This undoes part of what round 02 left: each entry is still checked once, at publish in place of at release.

**What may go wrong:** the walk is the first thing to touch the table on the CPU doing the release, and the release's own loop reads it right after, so its misses may just move there, as round 23's did. The A/B decides.

The test is `svcsock_rx_release_published_test`. Two mutants: one that skips the walk for every record, one that never does.

## The runs, to do

The sunrpc builds, `build-sunrpc.sh`'s way: A round 52's kept module `5D39507E93127A6010C9B8E`, B with this. KUnit on B, the mutants, then 4 runs a side, A B B A B A A B, `lat.sh` A B B A, a profile on B. `$L/t2-r137/`. The latency runs also read round 52's p50 again.
