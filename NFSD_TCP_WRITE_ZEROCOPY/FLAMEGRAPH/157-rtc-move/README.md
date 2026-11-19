# Round 157: the stack with the rtc move fix, and its passes

Mike, 19 November: "yes, build and swap in the fixed stack, then run the
passes".  `cae2f2ea2e5b1` ("SUNRPC: Run a moved rtc connection's work
item on its new CPU", `Fixes: 4c084f9754fe1`) changes `struct
svc_rtc_conn`, so sunrpc's exports are versioned anew and the whole
server stack is built from the tip (`build-svcstack.sh`, in /dev/shm)
and swapped into `updates/`.  The stack it replaces (18 November) is
kept in `/dev/shm/zc-stack-pre-move/`.  The tip also carries the spill's
removal (`143e3df791c88`).  `swap-passes.sh`: build, swap, `settled.sh
apply`, the WRITE pass (t2-r1220) and the READ pass (t2-r1221).

## The swap and the passes (19 November, 21:39 to 21:50)

The stack built from the tip loads clean: every module's imports match
(sunrpc `7ACE0A0B5F50F2537453F12`, 276 of its exports with new
versions; nfsd `78F201D2B9A190D3A538FA9`, its source unchanged), the
running one (sunrpc `C4A23C35035DD3A1E1C967B`) kept in
`/dev/shm/zc-stack-pre-move/`.  After `settled.sh apply`: the settled
setting, before the passes and after.

| run | pass | GB/s | mark | |
|---|---|---|---|---|
| `t2-r1220` | WRITE | 77.80 | 77.6 | PASS, no drive stepped down, no new kernel warning |
| `t2-r1221` | READ | 81.87 | 77.6 | PASS, no new kernel warning |

Against round 104's frame: the drives read 104.58 GB/s with local fio
and the ports send 98.31 with iperf3 `-Z`; the READ pass at 81.87 is
78% of the drives and 83% of the wire, inside rounds 136 to 149's
passes (79.27 to 82.24).
