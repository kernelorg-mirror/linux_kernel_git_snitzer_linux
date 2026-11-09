# Round 80: the CPUs a loaned record's pieces and release run on

Round 79 found about 2.9k cycles a WRITE of `__rx_loan_table_append_ref()`
and `rx_loan_table_release()` waiting on the loan table's own lines, and
guessed that a record's pieces run on different CPUs.  This round
measures it.  tardis2 has one NUMA node (CPUs 0 to 63), a 512 KiB L2 a
core and a 32 MiB L3 shared, so the question is cores, not nodes.

A record's table goes where its state goes: a partly received record's
`struct svc_tcp_rx_state` waits in `svsk->sk_rx` and is swapped into the
thread that takes the next piece (`svc_tcp_rx_exchange()`), so the table
is keyed by its bvec array.  [record-cpus.bt](record-cpus.bt), fentry on
`svc_tcp_recvfrom()` (one call is a piece), `svc_tcp_rx_walk_skb()` (the
first walk of a piece: an empty table starts a record) and
`svc_tcp_rx_release_refs()`, aggregated in the kernel, five seconds in
the window of one two-port loan run on the canary's own stack
([run335.sh](run335.sh), `t2-r335`, 71.08 GB/s, no new kernel warning;
the trace's own cost is not a figure to compare).
[record-cpus.txt](record-cpus.txt) is its output.

## What it found

| | |
|---|---|
| records started, released in the 5 s | 350,489 and 335,141 (67k WRITEs a second, as the run) |
| pieces a record | **5.92** (6 for 72%, 5 for 18%, 7 for 10%) |
| a piece after the first on **another CPU** than the piece before | **93.0%** (1,601,867 of 1,722,796) |
| the gap from one piece to the next | mostly 32 to 64 us, some to 256 us |
| release on the CPU of the record's last piece | **97.1%** (325,362), after 128 us to 1 ms |
| entries in a table at release | 128 to 255 |
| first pieces | spread evenly over nfsd's CPUs 8 to 55 |

So a 1 MiB WRITE's record is walked on about six cores in turn, a new
core for nearly every piece, and released on the last of them.

## What that does to the table

- **At receive:** each piece takes up the table where another core left
  it.  The new entries go into bvec lines, 4 entries a 64-byte line,
  last written by whichever core used or released the table before; a
  store there misses, and the next append's read of the entry just
  written (`rx_loan_table_can_coalesce()`, `rx_loan.h:179`, round 79's
  18% of cycles) waits for that line.  The two bitmap words (`borrowed`,
  `page_pool`) and the table's counts are taken over the same way at
  each piece.
- **At release:** the last piece's core reads all 128 to 255 entries,
  of which about five sixths were written on five other cores: the
  `t->bvec[i].bv_page` loads that round 79 found on release's prefetches.

That fits round 79's figures: about 40 bvec lines a record, nearly all
remote at release and each new one remote at receive, plus a few lines
of bitmap and counts at each of five takeovers.

## Suggested next (round 81)

Two changes, both on the table's own lines, holding no region page longer
and moving no work between CPUs:

1. **Fetch the table's next bvec line for writing ahead of the append**
   (`prefetchw(&t->bvec[t->count + 4])`, one line ahead, or two), so the
   line arrives before the entry is written, and at each takeover fetch
   the tail's line and the two bitmap words.
2. **At release, fetch the bvec array's lines ahead of the loop that
   reads them** (`prefetch(&t->bvec[index + 2 * RX_LOAN_PUT_BATCH])`),
   so the page prefetches no longer wait for their addresses.

Both are in `include/net/rx_loan.h` and `net/core/rx_loan.c`: the first is
inlined into `sunrpc.ko` and can be A/B'd as round 78 was, the second is
built in and needs a kernel.  The larger change, keeping a record's
pieces on one thread, would change how the pool hands transports to
threads, which rounds 65 to 73 tuned; it is not suggested first.
