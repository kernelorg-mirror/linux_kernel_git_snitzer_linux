# Round 163: the client's idle timer stopped only when pending

Mike, 19 November: "yes, run the slot-table A/B, then do 2".  Round
161: `xprt_request_enqueue_receive()` calls `timer_delete_sync()` on the
transport's autodisconnect timer for every request, taking the timer
base's lock (`lock_timer_base`, the top `_irqsave` caller) though the
timer is armed only while the receive queue is empty: 0.07 us of the
client's CPU a READ.  `4352483fa4691`, "SUNRPC: Stop a transport's idle
timer only when it is pending", checks `timer_pending()` first; a timer
armed by a racing `xprt_schedule_autodisconnect()` that the check misses
finds the request queued when it fires and does nothing.

Built on the client from round 149's tree (`f5c0543b9f377`) with that
commit only, `/root/zc-ckern/TD` (`cbuild136.sh`, 17 modules, no
warnings but compiler notes; sunrpc `72B6522F5FD84CCD583F45D`).
`ab-timer.sh`: B (the client's `updates/`, sunrpc
`1EFC577AC45E5EEF6EC1592`) against T, B T B T B T (t2-r1237 to r1242),
then T's WRITE pass (r1243) and READ pass (r1244), the client back on B.

## The A/B and T's passes (20 November, 00:30 to 00:59)

| run | arm | IOPS | a / b M | p99 ms a / b | server us a READ | client us a READ |
|---|---|---|---|---|---|---|
| `t2-r1237` | B | 3.54 M | 1.76 / 1.78 | 3.13 / 3.10 | 14.82 | 11.39 |
| `t2-r1238` | T | 3.59 M | 1.80 / 1.79 | 3.00 / 3.00 | 14.62 | 11.36 |
| `t2-r1239` | B | 3.53 M | 1.78 / 1.75 | 3.13 / 3.19 | 14.69 | 11.29 |
| `t2-r1240` | T | 3.56 M | 1.80 / 1.76 | 3.03 / 3.06 | 14.74 | 11.33 |
| `t2-r1241` | B | 3.37 M | 1.71 / 1.66 | 3.26 / 3.33 | 15.38 | 11.54 |
| `t2-r1242` | T | 3.53 M | 1.75 / 1.77 | 2.97 / 3.03 | 14.87 | 11.42 |
| mean | B | 3.48 M | | | 14.96 | 11.41 |
| mean | T | 3.56 M (+2.3%) | | | 14.74 | 11.37 |

On T: the WRITE pass t2-r1243 **77.84 GB/s PASS** (no drive stepped
down), the READ pass t2-r1244 **81.76 GB/s PASS**, no new kernel
warning in either; the client back on B after.

- Every T run is at or above every B run, and T's p99 is lower in
  every pair (2.97 to 3.06 ms against 3.10 to 3.33).  But most of the
  2.3% is B's t2-r1241, whose server CPU a READ rose to 15.38 us (the
  server's run-to-run spread, as t2-r1234 in round 162); without it B
  reads 3.535 M, under 1% below T.
- The client's CPU a READ is 0.04 us lower on T, the way round 161's
  0.07 us points and below what six runs can resolve.
- **Kept (Mike, 20 November: "yes, let's keep 4352483fa4691")**: a
  small change with no cost that passes both passes.  Installed in the client's `updates/` on
  20 November (Mike: "do install it in client's updates/"), sunrpc
  `72B6522F5FD84CCD583F45D` loaded; round 149's stack is kept in
  `/root/zc-ckern/updates-f5c0543b9f377/`.
