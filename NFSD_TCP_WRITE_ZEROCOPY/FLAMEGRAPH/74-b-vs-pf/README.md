# Round 74: PF against B directly

**Why (Mike, 8 November: "run B against PF first"):** rounds 66 to 73
compared each change only with the one before it, each pair in a series
of its own, and series drift by about 1 GB/s.  Chained through rounds
66, 68, 69 and 70, PF against B came out inside that noise, so before
deciding what to keep the two were run against each other.

- **B**: build 514's source with round 64's flag change, and none of
  the pool work since (round 65's B; stack `svcstack-pool-B`).
- **PF**: round 70's: B with the queue on its own line, the handoff,
  the prefetch at the start of `svc_recv()`, the packed layout and the
  pool's flags read before they are cleared (stack `svcstack-pool-PF`).

## The runs ([run311.sh](run311.sh), [run311.out](run311.out))

B PF B PF B PF (`t2-r311..r316`), each run as round 73's.  No new kernel
warning, the stock stack put back, the setting audited.

| run | | GB/s | far (a, b) |
|---|---|---|---|
| `t2-r311` | B | 69.48 | 2.4%, 2.8% |
| `t2-r313` | B | 69.82 | 2.6%, 2.4% |
| `t2-r315` | B | 69.47 | 2.7%, 1.5% |
| `t2-r312` | PF | 70.67 | 1.7%, 1.7% |
| `t2-r314` | PF | 70.42 | 2.2%, 2.1% |
| `t2-r316` | PF | 70.36 | 2.7%, 2.3% |

B 69.59 GB/s on average, PF 70.48: PF is ahead by 0.89 GB/s (1.3%) in
all three pairs, and its slowest run is above B's fastest.  The far
shares overlap (`t2-r316`, about the highest of the six, is still ahead
of every B run), so the lead is not the client's placement.  Cycles a
WRITE are left to be read with round 75's, so that reading the profiles
does not load the host during a measured run.

## What was kept

PF's code is kept, and round 75 measures a layout of `struct svc_pool`
on top of it.  The branch was then consolidated (Mike, 8 November):

- The commits that only reorder `struct svc_pool` (rounds 65, 67, 69 and
  71, and the two of round 75) are one, "SUNRPC: Keep each of struct
  svc_pool's counters within one cache line", first after B.  With it,
  line 0 and line 1 are B's again, `sp_flags` starts line 2 with
  `sp_sockets_queued`, and `sp_threads_woken` has line 3, so no counter
  spans two lines.  The comment above the struct went into that
  commit's message.
- Rounds 71 to 73's code and their reverts are gone: "SUNRPC: Keep a
  pool's flags on the line with its read-mostly fields", "SUNRPC: Add
  svc_thread_chosen_to_stop()", "NFSD: Check only the thread's own flag
  in nfsd()'s loop" and "SUNRPC: Take a handed transport's reference in
  the enqueuer".  Their docs stay; the SHAs they cite are on
  `kernel-7.3/canary.pre-fold-pool-layout` (before the fold) and
  `kernel-7.3/canary.pre-drop-reverted` (before the drop).

The code commits since B are now the layout, "SUNRPC: Hand an enqueued
transport to the idle thread claimed for it", "SUNRPC: Prefetch a
pool's transport queue at the start of svc_recv()" and "SUNRPC: Read a
pool's flags before clearing them on each wakeup".
