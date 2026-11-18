# Round 113: the server's per-READ cost at 2.54 M IOPS

Mike, 18 November: "profile the server at 2.54 M with both borrows".

## Why

Round 112's A/B left 4 KiB READs at 2.54 to 2.55 M IOPS with the
client's pool lock gone (scope `cpu`), and the server's busiest work
CPU at 95 to 98% in every run: the server is the limit (`IOPS.md`,
"Rounds 110 to 112", TODO 1).  Round 111's t2-r963 is the last server
profile, with only the auth borrow's symbols drawn out.

## The run

On the tip's stack (`/var/tmp/svcstack-541-kunit`: sunrpc
`85537945BAC1207EDA22E45`, nfsd `DC178C9D921DD1CDFF947D7`), both
borrows held and the client at scope `cpu` by `settled-iops.sh`.
`prof-srv.sh 972`: one 4 KiB READ run and 8 s of `perf record -a -g`
at 999 Hz from the load's second 8; reports by symbol, by command, by
CPU, and what runs on the four busiest CPUs.  A profile changes no
path, so it owes no WRITE or READ pass.
