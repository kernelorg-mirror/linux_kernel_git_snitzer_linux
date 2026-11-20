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
