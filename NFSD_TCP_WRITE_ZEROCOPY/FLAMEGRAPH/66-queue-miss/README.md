# Round 66: hiding the queue's miss, by the handoff or by a prefetch

**The variants (Mike, 7 November: keep S and chase where its cycles
went),** from round 65's annotation, which found the miss on the
transport queue's line moved from `svc_pool_can_shrink()` (B) to
`lwq_empty()` at the top of `__lwq_dequeue` (S):

- **B**, round 64's flag change, as round 65's B.
- **SH**, S and H together: most transports are handed to the woken
  thread and skip the dequeue.
- **SP**, S with `prefetch(&pool->sp_xprts)` at the top of `svc_recv()`,
  before `svc_alloc_arg()` (`e3934786b007`).

## The runs ([run247.sh](run247.sh), [run247.out](run247.out))

Whole stacks built by `build-svcstack.sh`, swapped in B SH SP B SH SP
(`t2-r247..r252`), each run as round 65's.  No run had a new kernel
warning; the stock stack was put back and the audit found the settled
setting.  Read by [read65.py](read65.py); the client's far share is the
share of its packets sent from the far node's CPUs (`pmu.log`).

| run | | GB/s | nfsd | `__lwq_dequeue` | `svc_recv` | enqueues | wakes | far |
|---|---|---|---|---|---|---|---|---|
| `t2-r247` | B | 70.82 | 252.6k | 493 | 4772 | 7.96 | 4.85 | 1.3% |
| `t2-r250` | B | 70.39 | 247.0k | 421 | 4482 | 7.95 | 4.89 | 2.0% |
| `t2-r248` | SH | 71.21 | 251.4k | 1042 | 3159 | 8.99 | 5.60 | 1.4% |
| `t2-r251` | SH | 71.37 | 251.4k | 953 | 3061 | 9.06 | 5.57 | 0.8% |
| `t2-r249` | SP | 69.97 | 245.3k | 1462 | 3255 | 7.86 | 4.83 | 2.3% |
| `t2-r252` | SP | 70.42 | 244.6k | 1614 | 3168 | 7.91 | 4.83 | 2.2% |

- **The prefetch does not hide the miss.**  SP's dequeue costs as S's
  did (1.5k cycles a WRITE): most likely the line is written again by
  another CPU between the prefetch and the dequeue.
- **SH's two runs were the highest of the 16 since round 64,** at no
  fewer cycles a WRITE.  Round 68 asks whether that is real.
- B's first run counts 1.9k cycles a WRITE in `[unknown]`, which the
  others do not; its nfsd figure is high for it.
