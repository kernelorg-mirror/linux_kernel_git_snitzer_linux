# Round 87: the appends' prefetch as committed, a confirming pair

9 November, tardis2, canary `7.3.3-13.hs.519.canary`, stock nfsd.

Round 86 ran the table prefetch behind a switch.  Mike kept its appends'
half, committed without the switch as "SUNRPC: Prefetch a loaned record's
table lines ahead of its appends" (`87ebdd82721b`).  This pair runs it as
committed.

Both modules are built by `build-sunrpc.sh` (no warning, every export's
version unchanged):

- **A:** the commit before it, the canary's own sunrpc source
  (`$L/sunrpc-K-A`, srcversion `BF7C91691266656E8821694`, the same as
  round 78's A);
- **B:** `87ebdd82721b` (`$L/sunrpc-K-B`, `55B4437DF708ECC9DABBE9E`).

`run363.sh` swaps in each before its run: A and B with perf (`t2-r363`,
`t2-r364`), then A and B with the held mark (`t2-r365`, `t2-r366`).  The
stock sunrpc is put back at the end.  Expected from round 86: about 1.2k
fewer cycles a WRITE in `__rx_loan_table_append_ref()`, about 0.35k in the
prefetch, and no higher region peak.

## Result: confirmed

`run363.out`, `symdiff-AB.txt`.  No new kernel warning, and the stock
sunrpc is back in the settled setting with `updates/` empty.

| | A (`t2-r363`) | B (`t2-r364`) | B - A |
|---|---|---|---|
| rate | 71.03 GB/s | 71.29 GB/s | +0.26 |
| `__rx_loan_table_append_ref`, cycles a WRITE | 4,361 | 3,283 | **-1,078** |
| `svc_tcp_rx_prefetch_entries` | 0 | 195 | +195 |
| `svc_tcp_recvfrom` (the takeover's, inlined) | 1,618 | 1,754 | +136 |
| `rx_loan_table_release` (unchanged code) | 3,259 | 3,359 | +100 |
| nfsd, all | 244,963 | 248,180 | +3,217 |
| region's exact peak (`t2-r365`, `t2-r366`, held mark) | 11,928 | 9,843 | not raised |

- **The appends save what round 86 found:** -1,078 against -1,212 there,
  for 331 in the prefetches (about 350 there): about **-750 cycles a WRITE
  net** on the functions it touches.
- The total's +3.2k is the spread of the rest from run to run, a third of
  it in `[unknown]` (+1,061, unsymbolized samples), as in round 86; one
  pair cannot resolve 0.75k in it.  The held-mark runs ran at 69.99 (A)
  and 69.85 (B) GB/s.
- `napi_pp_put_page` is again lower with the prefetch (-431), as in all
  three of round 86's pairs; why is still not known.
