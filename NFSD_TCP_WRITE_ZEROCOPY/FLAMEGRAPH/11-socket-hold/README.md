# Round 11: hold the socket while a loaned record arrives

**The idea:** an nfsd thread would own the socket (`lock_sock`) while a loaned record arrives, so the ring's softirq only adds each skb to the backlog. TCP's receive processing (`tcp_v4_do_rcv`) and its ACKs would then run on nfsd, which has 48 CPUs, instead of on the 4 ring CPUs.

## Outcome: not pursued. Assessed on round 10b's profile, no run

**What it would move** (fl10w, [round 10](../10-wait-rest/README.md)):
- `tcp_v4_do_rcv` costs 11.05 µs a WRITE on the rings, out of 63.4.
- Adding to the backlog is not free, so perhaps 8 µs a WRITE would leave the rings, and at least that much would land on nfsd. The total CPU does not fall.
- At 40.4 GB/s on one port the drives are the limit and the rings are about 60% busy, so moving the work buys nothing here. It would matter only where the rings saturate (two ports, faster drives).

**Why it does not fit the receive path:**
- **No wake.** A thread that sleeps owning the socket is not woken by skbs going to the backlog: `sk_add_backlog()` wakes nobody, and `svc_data_ready()` is not called until the backlog is processed. The owner would have to poll or keep releasing the socket, and releasing it is what the hold was to avoid.
- **ACKs wait for the owner.** Segments in the backlog are not acknowledged until the owner processes them. Rounds 06 and 10 showed how sensitive the sender is to when the server acknowledges and opens the window.
- **It ties a thread to a connection.** nfsd's threads serve any transport from the pool; holding one per arriving loaned record costs a thread for the whole of the record's arrival, about 0.2 ms at iodepth 1.
- **TCP had this and removed it.** The prequeue moved receive processing into the reader's context. Florian Westphal removed it in 2017 ("tcp: remove prequeue support", `e7942d0633c4`): it works only for a process blocked in `recv()` on that one socket, which servers rarely are, and showed no change between physical hosts.

Round 10b took the pass count from 12 to 5.4 a record without holding anything. If the rings become the limit, the next step is fewer receive skbs, not moving their processing: round 09's BIG TCP with the client's `gso_ipv4_max_size` raised (Mike's call), and XSIGHT-FOLLOWUP's X8 (the driver's first-touch miss in `eth_type_trans`).
