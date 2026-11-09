# Round 86: the loan table's lines fetched ahead

9 November, tardis2, canary `7.3.3-13.hs.519.canary`, stock nfsd.

Staying is dropped (rounds 82 to 85).  This round tries round 80's other
suggestion.  Round 79 found about 2.9k cycles a WRITE of
`__rx_loan_table_append_ref()` and `rx_loan_table_release()` waiting on the
loan table's own lines (its bvec array and its two bitmaps).  Round 80
showed why: a record's pieces are taken up on a new core for 93% of them,
and it is released on the last piece's core.  So at each step the lines
the next writer or reader needs were last written on another CPU.

## The change (`kernel-7.3/canary.TP`, `44cc481e8746`, `table-prefetch.patch`)

All in `net/sunrpc/svcsock.c`, so only `sunrpc.ko` changes (built by
`build-sunrpc.sh` into `$L/sunrpc-TP`, srcversion
`689D648F4C1FE9E11CAD5B9`, no warning, every export's version unchanged).
`sunrpc.svc_tcp_rx_table_prefetch` selects:

- **bit 0, ahead of the writes:**
  - when a thread takes up a partly received record
    (`svc_tcp_restore_pages()`), it fetches for writing the table's counts
    before the page trade, then the tail's bvec lines (the entry an append
    may coalesce into and one line more) and the two bitmap words after it;
  - before each loaned skb's appends (beside `rx_loan_prefetch_pages()`),
    it fetches the bvec lines for the skb's fragments and one line beyond.
- **bit 1, ahead of the release:** `svc_tcp_rx_release_refs()` fetches every
  bvec line of the table and its bitmap words for reading, before
  `rx_loan_table_release()` walks it.

Neither holds a region page longer.  If kept, the release half would
belong in `net/core/rx_loan.c`, which takes a kernel build; it is in
sunrpc here so the A/B keeps the boot.

## The runs (`run355.sh`)

The module is swapped in once.  A is the switch at 0, B at 3, in the
order A B A B A B with perf as round 82's (`t2-r355..r360`), then A and B
with the held mark (`t2-r361`, `t2-r362`) for the region's exact peak.
The stock sunrpc is put back at the end.  Judged on cycles a WRITE by
function (`76-canary-baseline/symdiff.py`), with the rate beside it:
rounds 82 and 85 showed the rate moves by up to 2.2 GB/s from run to run
on its own.  Round 81's upper bound for the gain is about 2.9k cycles a
WRITE, about 1.2% of nfsd's.
