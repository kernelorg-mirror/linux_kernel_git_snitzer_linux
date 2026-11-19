# Round 142: the client's CPU a READ, profiled to pick the next target

Mike, 19 November: "profile an IOPS pass to pick the next CPU-per-READ
target".  Rounds 140 and 141 showed that only less client CPU a READ
raises 4 KiB IOPS (the client's CPUs 85 to 88% busy, 11.8 to 11.9 us a
READ).  The client on its `updates/` stack (round 139's code; sunrpc
`2355B960B6A9951899AF6B4`), the server at the settled setting.

`prof-both137.sh 1154 96`: one 4 KiB random READ run at iodepth 96 and,
from its eighth second, 8 s of `perf record -a -g` at 999 Hz on both
hosts (the client's in its `/dev/shm/zc-cli137-1154.data`, kept).  The
client's samples are then grouped by what a READ pays for, each group
in us a READ (its share of the samples x 48 CPUs x busy / IOPS).
