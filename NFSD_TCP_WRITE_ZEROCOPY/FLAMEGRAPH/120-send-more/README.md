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

## The WRITE pass

t2-r1007 (send_more N): **RERUN, 74.53 GB/s** (no drive stepped down, no
new kernel warning), as round 111's t2-r945 at 74.52.  Rerun as t2-r1017
after t2-r1016.

## Results (18 November, 06:26 to 07:01)

READ pass t2-r1008 (send_more N): **PASS, 79.47 GB/s** (76% of the
drives' 104.58 with local fio, 81% of iperf3's 98.31).  No new kernel
warning in any run.

| Run | send_more | M IOPS (ports) | p99 a; b (ms) | Work / ring CPUs | Segments out / in a reply |
|---|---|---|---|---|---|
| t2-r1009 | Y | 2.76 (1.37 / 1.40) | 3.00; 3.03 | 81.6% / 75.7% | 0.929 / 1.225 |
| t2-r1010 | N | 2.67 (1.26 / 1.41) | 3.42; 3.26 | 81.2% / 70.9% | 1.108 / 1.254 |
| t2-r1011 | Y | 2.75 (1.36 / 1.39) | 3.19; 3.03 | 80.2% / 80.9% | 0.926 / 1.224 |
| t2-r1012 | N | 2.70 (1.37 / 1.33) | 3.36; 3.69 | 84.5% / 59.8% | 1.105 / 1.277 |
| t2-r1013 | Y | 2.71 (1.34 / 1.37) | 3.23; 3.19 | 82.1% / 66.6% | 0.927 / 1.218 |
| t2-r1014 | N | 2.74 (1.31 / 1.42) | 3.26; 3.19 | — | — |

Y 2.740 M on the mean, N 2.703: +1.4%, inside the spread; p99 lower in
every pair; segments out a reply 16% fewer (0.93 against 1.11), in 4%.
Every reply had another after it (100% with MSG_MORE): under load a
connection always has calls parked.  Mike: "I can already say its worth
adding to settled-iops.sh" (`228291423164e`).

### The per-READ breakdown, with it Y (t2-r1015)

2.76 M, server 79% busy: 1% of the samples is 0.232 us a READ, 18.3 us a
READ in all (t2-r1006: 18.4).

| Stage (with children) | t2-r1015 | us | t2-r1006 | us |
|---|---|---|---|---|
| rtc work items (`svc_rtc_fast`) | 49.8% | 11.6 | 46.1% | 10.9 |
| - processing (`svc_process`) | 30.2% | 7.0 | 27.7% | 6.5 |
| -- `nfsd3_proc_read` | 21.1% | 4.9 | 19.3% | 4.6 |
| --- the XFS read | 13.5% | 3.1 | 12.4% | 2.9 |
| --- the file (`nfsd_file_lookup_cached`) | 6.0% | 1.4 | 5.5% | 1.3 |
| -- the reply's encode | 3.3% | 0.8 | 3.0% | 0.7 |
| - the reply's send (`tcp_sendmsg_locked`) | 11.1% | **2.6** | 14.9% | 3.5 |
| - the call's receive | 11.9% | 2.8 | 11.5% | 2.7 |
| softirq (`handle_softirqs`) | 27.5% | **6.4** | 25.0% | 5.9 |
| idle (`default_idle_call`) | 9.0% | | 11.2% | |
| `tcp_write_xmit` (in both) | 13.1% | 3.0 | 13.7% | 3.2 |

The send fell 0.9 us, but `tcp_write_xmit` hardly moved: with MSG_MORE
the partial tail is pushed by the ACK, in softirq (+0.5 us), not by the
send.  Fewer segments, about the same transmit work; the sum a READ is
unchanged, and the rest moves within the noise of the arithmetic (the
stages are shares of a host that is busier).

### The READ pass with send_more Y

t2-r1016 (Mike: "yes, add a READ pass with send_more Y after the A/B"):
**PASS, 76.65 GB/s**, no new kernel warning, 2.8 GB/s (3.5%) under
t2-r1008's 79.47 at N.  Holding the partial tail may cost 1 MiB READs a
little; send_more is Y only in `settled-iops.sh`, and the passes run at
N.  `settled.sh apply` after it: the settled setting, send_more N.

### The WRITE rerun

t2-r1017 (send_more N): **PASS, 75.10 GB/s**, no drive stepped down, no
new kernel warning.  Round 120's commit has both its passes.
