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
