# Round 95: the profile at the budget of 64, for the next round's focus

**The ask (Mike, 12 November):** "keep the changes, but put the window
back to 0 and settle the budget at 64.. then re-run 3 runs and do a
profile to see what should be focus of the next round".

## The setting

- The window of loaned WRITEs per export (round 94, `05844df27a1f`,
  `db48abad7ef5`, `9e4eb880bb91`) stays in the tree and in the loaded
  stack (sunrpc `FC78BAE8A6C2B0A92A1366A`), at
  `svc_tcp_rx_target_window` 0: off.
- **The two-port loan budget is 64 WRITEs** (`xeu-rig/common.sh`,
  `TWO_WRITES`), from 128: round 94's sweep (`t2-r490..r493`) found 64
  halves the region's peak (7,362 pages against 11,511) with the rate,
  the median held and the bytes through memory unchanged.  The runners
  pass `WRITES=$TWO_WRITES`.

## The runs

[run501.sh](run501.sh), round 88's `run367.sh` at this setting: three
two-port loan runs, `t2-r501` counted (cycles, instructions, L1D and
L2D refills on nfsd's CPUs 8-55 for 8 s of the window), `t2-r502`
cycles with call graphs, `t2-r503` the held mark for the exact peak;
the region sampled beside each.  Read with round 76's `read65.py`,
`top.py`, `groups.py`, `symdiff.py` (against round 88's `t2-r367`,
`t2-r368`, the same stack's predecessor at 128) and round 77's
`callers.py`, `under.py`, round 88's `schedwhy.py`.
