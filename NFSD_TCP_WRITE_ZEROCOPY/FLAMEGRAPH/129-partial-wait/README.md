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
