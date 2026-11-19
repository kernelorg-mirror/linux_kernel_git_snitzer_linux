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
