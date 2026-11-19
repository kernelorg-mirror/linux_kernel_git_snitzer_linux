# Round 161: where the client's 11.4 us of CPU a 4 KiB READ goes

Mike, 19 November: "profile the client's 11.4 us per READ too".  Round
159's untraced runs put the client at 11.29 to 11.42 us a READ (mean
11.36), 83 to 84% busy, its busiest CPU 96 to 98%.  The client is 106:
2 x Xeon Gold 6542Y (48 cores, no SMT, 120 MiB L3), 1 TiB DDR5 at
5200 MT/s, two ConnectX-7 at 400 Gb/s; it runs `idle=poll`.
`profile-client.sh 1232`: round 160's run, with 8 s of `perf record -a
-g` on the client from the load's eighth second; `cstages.awk` charges
each sample to a stage as `stages.awk` does on the server, with
user-space frames (fio, liburing) apart, and groups by command (fio,
kworker, interrupts on idle CPUs, other).
