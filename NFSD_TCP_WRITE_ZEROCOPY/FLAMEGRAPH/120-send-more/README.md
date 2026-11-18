# Round 120: the rtc replies batched with MSG_MORE

Mike, 18 November: "implement reply send batching, test and then show me
the updated Per-READ breakdown, compared with round 119".

## The change

`852fb67105d5b` "SUNRPC: Let the rtc send FIFO batch replies with
MSG_MORE": `svc_rtc_send_more` (default N).  A drain of the send FIFO
carries 1.3 replies on the average (rtc_stats: 658 M sends in 510 M
batches), so batching within a drain alone gains little.  With it Y, a
FIFO reply that has another after it (the FIFO holds more, or the
connection has calls parked on async I/O, each of whose completions runs
the work item again) is sent with MSG_MORE: TCP sends its full segments
and keeps the partial tail for the next reply to join.  The transport
adds MSG_MORE only while the socket has data in flight, whose ACK pushes
a tail left waiting (the socket is TCP_NODELAY); a reply with none after
it goes without, which pushes the tail.  Not under kTLS.  No timer: a
tail waits at most for the next ACK or the next reply.

Round 119 put the reply's send at 3.5 us a READ and softirq at 5.9.
Built into `/var/tmp/svcstack-541-more` (sunrpc
`28651531DD48DEE5D856DED`, nfsd `DFC417DB5E771ABF21E2D4D`), W=1, no
warning.  No KUnit case: the rtc suite is not to be run on tardis2.
`settled.sh` holds it N (`EXP_RTC_SEND_MORE` for a run).

## The plan

    cd xeu-rig
    STACK=/var/tmp/svcstack-541-more/stack SUNRPC_KO=none \
        ./step.sh /var/tmp/zc-run-logs/541 swap-more ./swap-sunrpc.sh
    ./settled.sh apply
    ./write-check.sh 1007; ./read-check.sh 1008
    ../FLAMEGRAPH/120-send-more/ab-more.sh 1009:Y 1010:N 1011:Y 1012:N 1013:Y 1014:N
    EXP_RTC_SEND_MORE=Y ../FLAMEGRAPH/113-server-per-read/prof-srv.sh 1015
    EXP_RTC_SEND_MORE=Y ./read-check.sh 1016; ./settled.sh apply

The passes at N (the setting at rest); the A/B under `settled-iops.sh`
(the spill at weight 1, the three borrows); then round 119's profile with
it Y, for the per-READ breakdown beside round 119's.  Then a READ pass
with it Y (Mike: "yes, add a READ pass with send_more Y after the A/B"):
the passes at N show the change costs nothing off, this one what it does
to 1 MiB READs, whose replies the FIFO also sends.  Segments out a
reply (TcpOutSegs over the replies sent) is the batching's own measure:
about 1 now.
