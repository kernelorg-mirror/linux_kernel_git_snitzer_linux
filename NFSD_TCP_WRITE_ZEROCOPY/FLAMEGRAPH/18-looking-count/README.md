# Round 18: the wake path. No wake for work a running thread will take

**The idea** comes from round 17's "What the wake path still has": an
enqueue skips its wake while a running thread is about to look for work.
- Threads that are about to look are counted in `sp_nrlooking`, a new field
  of `struct svc_pool`.
- The enqueuer and a thread that stops looking use the store-buffering
  pattern: `lwq_enqueue()`, `smp_mb()`, then read the count; the stopping
  thread decrements, `smp_mb__after_atomic()`, then reads the queue.
- Whoever stops last wakes an idle thread if a transport is still queued, so
  no work is left unseen.

Three versions were tried, each against round 17 (`821534936DB054AC699BF52`,
with round 13's nfsd):

| version | commit | a thread counts itself | the enqueue skips its wake while |
|---|---|---|---|
| 18 | `331bd9a8d93a` | from `svc_recv()`'s start until it goes idle or holds a request to process, the receive included | one thread is counted |
| 18b | `05bb553779e2` | only until it dequeues a transport | one thread is counted |
| 18c | `0fae825c58f7` | as in 18 | two threads are counted |

The count is a new field of `struct svc_pool`, and the kernel versions its
exports with gendwarfksyms from the types' layout. So the B side of each A/B
was the whole server stack (sunrpc, auth_rpcgss, rpcsec_gss_krb5, rpcrdma,
lockd, nfs_acl, nfs_localio, nfsd), built by
[`xeu-rig/build-svcstack.sh`](../../xeu-rig/build-svcstack.sh) and swapped
in with `swap-sunrpc.sh STACK=`.

**Runs on 2026-10-24 and 25 on tardis2**, build 494, the settled setting.
The drivers are `run36.sh` (18), `run37.sh` (18b), `run38.sh` (18c),
`run38b.sh` and `run38c.sh` (18c's latency again) in
`/var/tmp/zc-run-logs/t2-tipkernel/`. KUnit 7, 10, 52 and 7 passed on each B.

## Outcome: not kept. 18c saves 3 us of nfsd a WRITE at saturation, but costs latency at low depth

Reverted at the tip: `333543a94fd6` (18b), `d953afce90b0` (18c) and
`92c84ceb5847` (18). The code is round 17's again.

**At saturation** (loan64, 40.5 GB/s, every run DRIVE-LIMITED with no drive
stepped down; [cost.txt](cost.txt)), the change against its own A runs, per
WRITE:

| | 18 | 18b | 18c |
|---|---|---|---|
| nfsd us, A then B | 112.0 to 105.1 (**-6.9**) | 113.1 to 111.9 (-1.2) | 114.4 to 111.3 (**-3.1**) |
| rings us | 59.7 to 57.1 (-2.6) | 58.6 to 59.8 (+1.2) | 59.6 to 57.4 (-2.2) |
| threads woken | 4.35 to 1.44 | 4.36 to 4.13 | 4.34 to 2.97 |
| voluntary switches | 5.85 to 2.93 | 5.87 to 5.64 | 5.84 to 4.48 |
| enqueues | 5.42 to 4.98 | 5.45 to 5.42 | 5.41 to 5.29 |

- No stall: no WRITE in any of the 18 runs took over 130 ms.
- 18b counts a thread only for the few hundred ns between the look and the
  dequeue, so few enqueues land inside it, and it saves nearly nothing.

**The wake trace** ([wakesrc.txt](wakesrc.txt), per WRITE at saturation):

| | round 17 | 18 | 18b | 18c |
|---|---|---|---|---|
| passes through `svc_recv()` | 7.70 | 5.63 | 7.29 | 6.42 |
| empty passes that slept | 0.55 | 0.02 | 0.44 | 0.15 |
| sleeps | 2.48 | 0.39 | 2.14 | 1.06 |
| wake calls | 6.59 | 0.92 | 5.09 | 2.99 |

**Latency, 4 writers at iodepth 1** ([latency.txt](latency.txt), untraced
runs):
- Another user's load reached the shared client during 18c's first set of
  runs and during one later run, with the client 28-96% busy against 4-6% in
  a clean run. Those runs show tails to 2-7 ms on round 17's code too, and
  they are left out below. `run38b.sh` and `run38c.sh` start each run only
  once the client has been under 10% busy for 10 s.
- Every clean run either stays fast (p50 0.42-0.44 ms) or is slow for the
  whole run (0.47-0.50 ms, about 7.5 GB/s rather than 8.3):

| code | clean runs | p50 of each run | mean completion, averaged | slow runs |
|---|---|---|---|---|
| round 17 | 9 | 0.424, 0.432, 0.436, 0.420, 0.477, 0.432, 0.436, 0.432, 0.428 | 0.445 ms | 1 in 9 |
| 18 | 2 | 0.465, 0.498 | 0.482 | 2 in 2 |
| 18b | 2 | 0.428, 0.420 | 0.433 | 0 in 2 |
| 18c | 4 | 0.473, 0.440, 0.420, 0.481 | 0.456 (+11 us) | 2 in 4 |

- p99 is the same in all of them, 0.55-0.65 ms.
- How often a version runs slow follows how often it skips a wake. Four runs
  of 18c are suggestive, not proof.
- At this depth the change skips almost no wakes: about 6.3 wakes a WRITE
  under every version (`pool_stats`). So the slow runs come from the rare
  skips. Work queued while the only counted threads are in a long read waits
  for one of them, rather than taking an idle thread at once. 18's queue time
  in the trace moved from 2-4 us to 4-16 us. Why a whole run stays slow is
  not known.

Latency per I/O matters more here than 3 us of CPU at saturation, where the
drives set the rate (Mike: "not loving added latency"). So none of the three
is kept.

## What is left in the wake path

- Finding what keeps a whole run slow would need a slow run traced against a
  fast one. If the cause is placement (which threads and CPUs end up
  counted), the skip could be limited to the work that does not wait. That
  was not tried.
- The passes themselves are still about 4 reads a 1 MiB record, set by the
  open window and so by `svc_tcp_rx_rcvbuf` (Mike's setting), as round 17
  found.
