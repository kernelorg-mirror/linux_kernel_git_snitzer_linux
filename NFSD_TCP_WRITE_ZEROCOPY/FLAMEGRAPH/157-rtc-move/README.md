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
