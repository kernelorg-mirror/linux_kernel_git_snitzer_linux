# Round 33: prefetch the completion entry further ahead (a look; not built)

**The plan, from round 30:** the completion entry the card writes is about 31% of `xeu_rx_poll`'s L1 refills. Prefetch it several entries ahead, in xeu.

**What the code says first:**
- Each completion entry is one 64-byte cache line (`struct rx_pp_cr_desc`).
- Round 28's prefetch already reads the next entry's line a frame early: `xeu_rx_prefetch_next()` checks its valid bit.
- So a deeper prefetch helps only if the entries two or more ahead are already written when it runs, and only if the misses that remain are not where the next entry is not written yet.

## The look

**Run 2026-10-27 on tardis2** (driver [run66.sh](run66.sh), runs `/var/tmp/zc-run-logs/t2-r66/` and `t2-r66-2/`):
- **The state:** the current state (the tip's sunrpc, `rodata=noalias`, `rx_prefetch` Y, the settled setting), one loan64 run each.
- **The probe:** `napi:napi_poll` on port a for 10 s from 5 s after the WRITEs passed 10,000 a second.
- **Two runs:** xeu polls its transmit rings by NAPI too. The first run counted both kinds together ([napi-poll-mixed.txt](napi-poll-mixed.txt)). The second kept them apart by CPU, receive on 0, 2, 4, 6 and transmit on 1, 3, 5, 7 ([napi-poll.txt](napi-poll.txt)).

**Per 10 s, 399,591 WRITEs:**

| | receive polls | transmit polls |
|---|---|---|
| polls | 3,436,279 (8.6 a WRITE) | 1,749,867 |
| frames or completions | 48,158,651 (120.5 a WRITE) | 1,954,120 |
| mean a poll | 14.0 | 1.1 |
| polls at the budget of 64 | 241,865: 7% of the polls, 32% of the frames | none |

- **Most frames are in long polls.** 42% of the receive polls take 0 to 3 frames, but they carry under 1% of the frames; most find none.
- About 94% of the frames are in polls of 4 or more.
- A third are in polls that stopped at the budget with frames still waiting.
- **So inside a poll the card is usually ahead,** and the next entry is already written when round 28's prefetch reads it.

## Why the change is not built

- **The entries inside a poll are covered:** round 28's prefetch brings each one into L1 a frame before the loop reads it.
- **What should be left:** about one miss a poll, roughly 8.6 a WRITE.
  - At a poll's first entry, nothing read it ahead.
  - At a poll's end, the next entry is not yet written, and the card writes it after the prefetch read it.
  - In both the poll waits on the card or on the interrupt. A prefetch further ahead cannot fetch an entry the card has not written.
- **The bound:** at about 100 ns each, roughly 1 µs a WRITE, and that part is inherent. The rest of the 31% is what round 28's prefetch already reaches.
- **So a deeper prefetch** would save a fraction of a µs a WRITE at most, inside the rings' spread of about 1 µs.

## Decision

- **A look (Mike, 27 October):** the change is not built.
- **The caveat:** round 30's refill samples were taken on `rodata=full`. Round 34 samples the ring CPUs' refills at the current state. If the completion entry's loads still rank there, away from a poll's first frame, this round is reopened.
- **Both look runs were traced** (rings 43.7 and 45.1, nfsd 87.3 and 92.7 µs a WRITE), so their costs are not set against untraced runs.
