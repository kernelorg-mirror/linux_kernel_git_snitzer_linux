# Round 75: struct svc_pool with each counter within one line

**The change (Mike, 8 November):** PF's layout had two of the pool's
percpu counters across two cache lines (`sp_messages_arrived` at 96 to
135, `sp_threads_woken` at 176 to 215), and B's had one
(`sp_threads_woken` at 168 to 207).  **PL** is PF with line 0 in B's
order, line 1 as B's (`sp_all_threads`, `sp_idle_threads`,
`sp_messages_arrived`, which ends the line), `sp_flags` aligned to start
line 2 with `sp_sockets_queued`, and `sp_threads_woken` aligned to line
3: 256 bytes as before, and no counter across a line
([pahole-PL.txt](pahole-PL.txt)).  It is the layout of "SUNRPC: Keep
each of struct svc_pool's counters within one cache line", the one
layout commit of the consolidated branch (round 74's README); PL was
built from PF with the two commits it was folded from (worktree
`/dev/shm/wt-PL`, branch `kernel-7.3/canary.PL-514`).

## The runs ([run317.sh](run317.sh), [run317.out](run317.out))

PF PL PF PL PF PL (`t2-r317..r322`), each run as round 74's.  No new
kernel warning, the stock stack put back, the setting audited.  Round
74's runs are beside them, now with their cycles ([read65.py](read65.py)):

| run | | GB/s | nfsd | `svc_recv` | enq/W | woken/W | far (a, b) |
|---|---|---|---|---|---|---|---|
| `t2-r311` | B | 69.48 | 244.1k | 4343 | 7.98 | 4.97 | 2.4%, 2.8% |
| `t2-r313` | B | 69.82 | 243.0k | 3983 | 8.00 | 4.96 | 2.6%, 2.4% |
| `t2-r315` | B | 69.47 | 244.1k | 4137 | 7.97 | 4.97 | 2.7%, 1.5% |
| `t2-r312` | PF | 70.67 | 245.8k | 2638 | 9.19 | 5.80 | 1.7%, 1.7% |
| `t2-r314` | PF | 70.42 | 241.1k | 2314 | 9.14 | 5.81 | 2.2%, 2.1% |
| `t2-r316` | PF | 70.36 | 247.5k | 2426 | 9.10 | 5.80 | 2.7%, 2.3% |
| `t2-r317` | PF | 71.02 | 250.9k | 3142 | 9.14 | 5.68 | 0.9%, 1.3% |
| `t2-r319` | PF | 69.39 | 240.0k | 2224 | 9.21 | 5.92 | 3.2%, 3.4% |
| `t2-r321` | PF | 71.37 | 246.6k | 2651 | 9.08 | 5.70 | 0.9%, 0.8% |
| `t2-r318` | PL | 70.00 | 245.7k | 2496 | 9.20 | 5.86 | 0.9%, 1.2% |
| `t2-r320` | PL | 71.51 | 249.5k | 2685 | 9.00 | 5.68 | 1.3%, 0.8% |
| `t2-r322` | PL | 70.42 | 248.7k | 3120 | 9.16 | 5.80 | 2.6%, 1.7% |

- **PL against PF: the same.**  PF 70.59 GB/s and 245.8k cycles a
  WRITE on average, PL 70.64 and 248.0k; the pairs split.  The rate
  follows the client's far share as before: the slowest run of each,
  `t2-r319` and `t2-r322`, had the highest.
- **Round 75's PF repeats round 74's** (70.59 against 70.48 GB/s), so
  the two series agree, and B's 69.59 stands.
- **B against PF:** PF is 0.9 GB/s faster at about the same cycles a
  WRITE (244.8k against 243.7k in round 74), so fewer at matched rates.
  `svc_recv()` falls from about 4.15k cycles a WRITE to 2.46k, the
  dequeues the handoff saves; the enqueues and wakes a WRITE rise with
  it (7.98 to 9.14 and 4.97 to 5.80).

## Where PL's cycles went

[symdiff.py](symdiff.py), PF's three runs against PL's
([symdiff-PF-PL.txt](symdiff-PF-PL.txt)): of the pool's own
functions only `svc_pool_idle_add()` moves past run-to-run noise
(`svc_recv()` rises 95), 620 cycles a WRITE in PF and 935 in
PL ([fsum.py](fsum.py)), higher in each of PL's runs (818 to 1025) than
in any of PF's six (569 to 717).  `percpu_counter_add_batch()`, which
every percpu counter in the kernel goes through, is the same in both
(about 1.5k).

The annotation (`annotate-idle-add-r321.txt` for PF, `-r322.txt` for
PL) puts it on the idle list's line: the `casal` of `llist_add()` on
`sp_idle_threads` waits 544 cycles a WRITE in PF and 758 in PL, and the
load of the list's head before it, 9 and 232.  In PL that line also
holds all of `sp_messages_arrived`, whose `counters` pointer (at 120)
every increment reads; in PF the pointer was at 128, on line 2.  So a
thread going idle more often finds the line shared away from it.

About 300 cycles a WRITE, 0.13% of nfsd's, with the rate and the rest
unchanged: the layout is kept (Mike, 8 November: "that is ~300 cycles
isn't a show stopper if the rate and other metrics are OK").
