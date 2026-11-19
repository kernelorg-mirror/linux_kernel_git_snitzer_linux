# Round 151: packets a READ

Mike, 19 November: "yes, run the packet-count measurement as round
151".  Round 150 put some 9 of the server's 15.8 us of CPU a 4 KiB READ
in the network (softirq 4.6, the reply's send 2.6, the call's receive
2.1).  The question: does a READ pay for extra packets (ACKs, transmit
completions), or for what each packet costs?

`packets.sh <n>`: one IOPS run (iodepth 96, the settled setting, the
client on its `updates/` stack), two snapshots 8 s apart on both hosts
from the load's eighth second; `packets.awk` divides each difference by
the server's NFSv3 READs in the window.  It only reads counters, so no
WRITE or READ pass.

## t2-r1200

3.55 M IOPS (1.80 / 1.74 M), p99 3.52 / 3.29 ms, server busy 86%,
client 84%; 29.56 M READs in the window.

| per READ | server | client |
|---|---|---|
| packets received (link) | 0.877 | 0.777 |
| packets sent (link) | 0.779 | 0.875 |
| TCP segments in | 0.877 | 0.551 |
| TCP segments out | 0.778 | 0.876 |
| pure ACKs in | 0.082 | 0.000 |
| delayed ACKs | 0.000 | 0.000 |
| retransmits | 0.000 | 0.000 |
| autocorked sends | 0.203 | 0.152 |
| header-predicted (fast path) segments | 0.454 | 0.040 |
| NIC interrupts | 0.001 | 0.321 |
| NET_RX softirqs | 0.801 | 0.288 |

## Reading

- **No extra packets.**  Fewer than one packet a READ each way: calls
  share segments (0.88 a READ in) and replies share them too (0.78
  out).  Pure ACKs are 0.08 a READ, delayed ACKs and retransmits none.
  ACK coalescing has nothing to win.
- **The client's GRO merges, the server's does not:** the client takes
  0.78 packets as 0.55 segments, the server takes 0.88 as 0.88.  Calls
  are small (about 1.1 a packet), so every received packet goes up the
  stack alone.
- **The server runs a NET_RX softirq for nearly every READ (0.80), with
  almost no interrupts (0.001):** its softirqs are raised from xeu's
  polling (the `bh_worker` path in round 150's profile), not by the
  card, and each handles about one packet in and one out.  The client,
  on interrupts, runs one softirq per 3.5 READs.
- So the server's ~9 us a READ in the network is **per-packet cost**,
  about 5.4 us for each of its 1.66 packets (in and out), and a poll
  that batches little.  The levers: more packets per poll on the
  server (xeu's polling and deferral), GRO on xeu's receive, and what
  each packet costs in TCP (only 0.45 of 0.88 segments take the
  header-predicted path).
