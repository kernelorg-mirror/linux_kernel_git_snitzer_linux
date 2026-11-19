# Round 152: where the server's softirqs run

Mike, 19 November: "yes, extend packets.sh and run it as round 152 and
document in XSIGHT-FOLLOWUP.md as needed".  Round 151 counted 0.80
NET_RX softirqs a READ on the server with almost no NIC interrupts.
The question: how many are xeu's ring polls on the CPUs that host its
vectors (0-7 and 56-63, "ring"), and how many the RPS/RFS hop to
nfsd's CPUs (8-55, "other"), each woken by an IPI?

`../151-packets/packets.sh` now also snapshots `/proc/net/softnet_stat`
(packets processed, RPS IPIs received, squeezes) and NET_RX from
`/proc/softirqs`, split between the two CPU groups.  It only reads
counters: no WRITE or READ pass.

## t2-r1201

3.60 M IOPS (1.85 / 1.76 M), p99 3.13 / 3.98 ms, server busy 87%,
client 84%; 30.27 M READs in the window.  Round 151's counts again
(0.87 packets in, 0.79 out, pure ACKs 0.09), and by CPU group, per READ:

| | server ring CPUs (16) | server nfsd CPUs (48) | client |
|---|---|---|---|
| NET_RX softirqs | 0.348 | 0.447 | 0.282 |
| RPS IPIs received | 0.077 | 0.447 | 0 |
| packets up the stack | 0.125 | 0.742 | 0.571 |
| squeezes | 0 | 0 | 0 |

## Reading

- **Every softirq on nfsd's CPUs is an RPS IPI** (0.447 and 0.447),
  and they carry 86% of the received packets up the stack (0.742 of
  0.868), 1.66 a softirq.  RFS's hop from the ring's CPU is 56% of the
  server's NET_RX softirqs.
- **The rings' own polls batch reasonably:** about 0.27 a READ on the
  ring CPUs, for 0.87 frames in and 0.79 completions out, some 6 a
  poll.  Round 151's "polling handles very few packets" was wrong in
  its mechanism: the one-packet softirqs are the hop, not the polls.
- So the large lever is landing a frame on its nfsd CPU in the first
  place (accelerated RFS, or a ring a CPU), which is the card's and
  xeu's to give: `XSIGHT-FOLLOWUP.md`, X9.  Merging each ring pair's
  two NAPI contexts (xeu Patch16, `tx_in_rx_napi`) halves the ring
  polls' count, a smaller win, to be measured as round 153.
