# Round 125: what softirq does a READ

Mike, 18 November: "yes, profile softirq next".  No run: t2-r1029's
profile (round 124, 2.88 M, `/dev/shm/zc-p113-1029.data`), by
`perf script` callchains; 1% of the samples is 0.222 us a READ.

`handle_softirqs` and below: **30.2%, 6.7 us a READ**, half on the ring
CPUs (14.7%), half on the work CPUs (15.5%).

| What | Share | us a READ |
|---|---|---|
| TCP receive on the work CPUs (RPS/RFS backlog, `process_backlog` > `ip_rcv`) | 13.9% | 3.1 |
| - of it, ACKs and the held replies they push (`tcp_ack`, `__tcp_push_pending_frames`) | 6.5% | 1.4 |
| - of it, the calls' data (`tcp_data_queue`) | 1.6% | 0.4 |
| `handle_softirqs` self, on the instruction after interrupts are enabled again | 4.5% | 1.0 |
| the card's receive poll (`xeu_rx_poll`: GRO, skbs) | 3.5% | 0.8 |
| the card's transmit completion (`xeu_tx_poll`: freeing sent skbs) | 3.5% | 0.8 |
| TCP Small Queues' work (`bh_worker` > `tcp_tsq_handler`) | 2.2% | 0.5 |

Across softirq: `tcp_ack` 3.6% (`tcp_clean_rtx_queue` 2.7%), pushes of
pending frames 2.9% under the receive and 11.9% in all (with the
sends), `tcp_write_xmit` > `__tcp_transmit_skb` 8.5%.

The 4.5% on softirq's own entry is time with interrupts off before it
(the card's and the drives' interrupts, RPS's IPIs), which perf without
pseudo-NMI can only sample where interrupts come back on: a boot with
`irqchip.gicv3_pseudo_nmi=1` would place it.

## Where it points

- **TCP Small Queues (0.5 us):** `tcp_limit_output_bytes` is 256 KiB in
  the settled setting (round 105, for READ throughput); a run with it
  higher under `settled-iops.sh` alone is a sysctl A/B, no build.
- **ACKs (1.4 us, and some of the card's receive and transmit
  completion):** 1.22 segments in a reply (round 120); fewer ACKs from
  the client would cut both; a client setting.
- **The steering (RPS/RFS, 3.1 us of receive on the work CPUs and the
  IPIs):** the receive work is needed wherever it runs; what the
  steering itself costs is in the 1.0 us that only pseudo-NMI places.
