# Round 129: wait for the rest of a call header on the work item

Mike, 18 November: "implement the no_peek fix, quick A/B".  Round 128
found 9.6% of the work item's calls handed to an nfsd thread because
the next call's header was not all in (`fallback_no_peek`), every READ
still served on the fast path: thread wakeups for nothing, about 0.5 us
a READ in the threads alone.

`d986e1869d0b9` "SUNRPC: Wait for the rest of a call header on the rtc
work item": `svc_tcp_peek_call()` returns -ENODATA for a fresh record
with less than the header queued (XPT_DATA cleared, peeked again), and
the work item releases the transport as for a held receive; the rest's
arrival enqueues it.  `80fe61a1facf8` "DROP: SUNRPC: A/B switch for
FLAMEGRAPH round 129": `svc_rtc_partial_wait_ab` N hands it to a thread
as before.  Built into `/var/tmp/svcstack-541-partial` (sunrpc
`99424F2CE6E026FE151DCE7`), W=1 clean.

The quick A/B: `ab-partial.sh 1048:Y 1049:N 1050:Y 1051:N` under
`settled-iops.sh`.

## Result (18 November, 09:58 to 10:12)

| Run | Wait | M IOPS (ports) | p99 a; b (ms) | Busy (busiest) | us a READ | Partial waits | To threads | Work CPUs min / median / max | Ring CPUs min / median / max |
|---|---|---|---|---|---|---|---|---|---|
| t2-r1048 | Y | 3.01 (1.56 / 1.45) | 2.41; 2.47 | 74% (100%) | 15.73 | 8,502,439 | 0 | 40 / 81 / 92 | 27 / 77 / 95 |
| t2-r1049 | N | 2.95 (1.44 / 1.51) | 2.87; 2.93 | 78% (92%) | 16.92 | 0 | 7,598,462 | 62 / 82 / 92 | 65 / 80 / 93 |
| t2-r1050 | Y | 3.07 (1.56 / 1.50) | 2.41; 2.47 | 76% (100%) | 15.84 | 8,714,729 | 0 | 56 / 75 / 88 | 68 / 83 / 93 |
| t2-r1051 | N | 2.95 (1.54 / 1.41) | 2.83; 3.03 | 78% (100%) | 16.92 | 0 | 7,497,247 | 59 / 83 / 92 | 63 / 79 / 95 |

Waiting: **3.04 M IOPS and 15.79 us a READ; handing to threads: 2.95 M
and 16.92 us.  -1.13 us a READ (-6.7%), +3.0% IOPS, p99 0.4 to 0.5 ms
lower on both ports**, both pairs the same way and each side within
0.11 us of itself.  11.3% of reads waited for a partial header; no
call went to a thread.  No new kernel warning.

The DROP switch is dropped from the branch (the tip before is kept as
`kernel-7.3/canary.pre-drop-partial-ab`); tardis2 runs the round's
sunrpc with it at Y, which is the commit's behaviour.
