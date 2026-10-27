# Round 32: more transmit queues for nfsd's sends (not kept)

**The question:** port a's 4 transmit queues carry every reply and ACK from nfsd's 48 CPUs. Does giving each sender a queue of its own, and more queues, make a loaned WRITE cheaper?
- **What prompted it:** round 07 put the transmit lock at about 2.6 µs of nfsd's `__dev_xmit_skb` a WRITE. Round 31 put the qdisc layer at about 2.6 a WRITE, inside the spread.
- **XPS as settled:** tx-0 to tx-3 are tied to CPUs 8 to 11 only (`settled.sh`, as recorded), so 44 of nfsd's 48 CPUs pick a queue by the flow's hash.
- **The limit:** xeu pairs a transmit ring with each receive ring (`num_rings`, at most 8 a port). More transmit queues mean more receive rings as well, unless xeu is changed.

## The sides

**Run 2026-10-27 on tardis2** (driver [run65.sh](run65.sh), the runs `/var/tmp/zc-run-logs/t2-r65/`):
- **The current state on every side:** the tip's sunrpc (`A06FE478658242A10E100FF`), the kernel's nfsd (`3F0C341E86E266D5FDF2ADF`), xeu `2C05DF674C9D013185FDB34` with `rx_prefetch` Y, `rodata=noalias`, the settled setting.
- **The runs:** four untraced loan64 runs a side, the sides interleaved, each after `settled.sh apply`, an fstrim and 120 s of rest. No copy runs.

| side | rings | transmit queues | XPS | receive |
|---|---|---|---|---|
| S | 4 | 4 | the settled map: tx-i on CPU 8+i | rings 0-3 |
| A | 4 | 4 | `XPS=spread`: 12 of nfsd's CPUs to each queue | rings 0-3 |
| B | 8 | 8 | `XPS=spread`: 6 of nfsd's CPUs to each queue | rings 0-3 only (`RX_RINGS=4`) |

- **B's layout:** the RSS table and the flow placement name rings 0 to 3 only, and every ring keeps the four-ring interrupt layout. Rx-4 to Rx-7 share CPUs 0, 2, 4 and 6 with Rx-0 to Rx-3, and Tx-4 to Tx-7 share 1, 3, 5 and 7, so nfsd's CPUs take no interrupts.
- **B's receive shares were 25/25/26/24/0/0/0/0:** rings 4 to 7 received nothing. They still post their 256 buffers each from the region: a fresh open of 8 rings held 1,040 pages at rest against 4 rings' 528.
- **The rig:** `twoport.sh` gains `RX_RINGS=` and `XPS=spread`, `ring-irqs.sh` gains `PAIRED=1` for the layout above, and `settled.sh` gains `EXP_RINGS`, so its audit holds a run to the rings it asks for.

## Outcome

Per loaned WRITE ([cost.txt](cost.txt), from `xeu-rig/cost.sh`):

| | S, settled | A, the map alone | B, 8 queues | A − S | B − S |
|---|---|---|---|---|---|
| rings, µs | 44.50 ± 1.01 | 44.97 ± 0.86 | 45.35 ± 1.79 | +0.47 ± 0.66 | +0.85 ± 1.03 |
| nfsd, µs | 90.30 ± 1.94 | 91.10 ± 1.33 | 91.47 ± 1.83 | +0.80 ± 1.17 | +1.17 ± 1.33 |
| region peak in use, pages | 6360–6499 | 6233–6677 | 6778–6980 | about the same | **about +400** |

- **Neither saves anything.** Both are within the spread of S and, if anything, a little dearer.
- **B holds about 400 more of the region's pages at peak.** That is its idle receive rings' posted buffers, against the footprint rule.
- **Unchanged:** the rate (40.44 to 40.57 GB/s, every run DRIVE-LIMITED with no drive stepped down), and no transmit queue was stopped in S's or A's runs. B's per-ring counters do not survive the port's return to 4 rings, so its stops are not known.
- **One A run** (r65-3-A) saw 162 client retransmits in its window, against 0 to 7 in every other run, at the same cost as the other A runs.
- **Latency with B's setting** was not finished. The round was stopped after the twelve loan runs.

## Decision

- **Not kept (Mike, 27 October):** "its not helpful and it bloats the region use". `settled.sh` keeps 4 rings and the recorded XPS map.
- **The xeu change** (transmit rings apart from receive rings) is not pursued. Its gain would be at most what B shows, and B shows none.
- **What it says about rounds 07 and 31:** the transmit path's lock, with 12 of nfsd's CPUs to a queue, is not a cost these runs can see.
- **The current state, measured again** (side S, four runs): rings 44.50 and nfsd 90.30 µs a loaned WRITE, against round 30's 44.60 and 91.67 in its own session.
