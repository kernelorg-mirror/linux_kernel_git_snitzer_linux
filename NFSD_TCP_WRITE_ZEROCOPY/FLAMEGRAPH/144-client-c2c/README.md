# Round 144: the client's cache misses by cache line

Mike, 19 November: "yes, run perf c2c on the client as round 144".
Rounds 138, 141 and 143 each removed a function's share of the
client's profile and saved nothing, because the samples sat on loads
of lines (an `rpc_rqst`'s above all) last written by another CPU.
This round maps those misses by line instead of by function: which
lines take hits on another CPU's modified copy (HITM), at which
offsets, read and written where.  No code change, so no passes.

`c2c.sh 1163`: one 4 KiB random READ run at iodepth 96 on the client's
`updates/` stack (round 139's code), and from its eighth second 5 s of
`perf c2c record -a` on the client (`/dev/shm/zc-c2c-1163.data`,
kept); the report coalesced by instruction across threads.
