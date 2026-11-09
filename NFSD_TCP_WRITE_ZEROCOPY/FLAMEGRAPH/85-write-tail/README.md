# Round 85: staying's slow writes and its SGLs, in three pairs

9 November, tardis2, canary `7.3.3-13.hs.519.canary`, round 82's stack
(`$L/svcstack-ST`, `kernel-7.3/canary.ST`).

Round 84 found the time staying adds to a WRITE inside
`nfsd_vfs_write()`, all of it in writes over 2 ms (1,515 off, 2,572 on).
That was one run each.  This round asks two things in three pairs:

1. does staying's larger tail of slow writes repeat, and if so, does it
   form in the drives (a request's issue to its completion, by drive) or
   before them (a wait for a tag)?
2. does staying change a WRITE's SGLs?  Round 55 found about 125 entries
   a WRITE on stock sunrpc, one a received frame, so a record's pieces
   should not change them; it has not been measured with staying.

`tail-sgl.bt` times, for five seconds in each run's window:

- `nfsd_vfs_write()`: WRITEs, their time, the count and time over 2 ms;
- `nvme_pci_setup_data_sgl()`: requests, `nr_phys_segments`, the SGL
  entries written (read from the command as round 55 did), request bytes;
- `block_rq_issue` to `block_rq_complete`, writes only, by drive's minor
  number: the time a request and the count over 2 ms;
- `blk_mq_get_tag()`: its time, and the count and time over 100 us.

The minor numbers on this boot: nvme0n1 0, nvme1n1 2, nvme2n1 1, nvme3n1
4, nvme4n1 3, nvme5n1 6, nvme6n1 7, nvme7n1 5.

`run349.sh` swaps the stack in once, runs A (`svc_tcp_rx_stay_us=0`) and
B (1000) in the order A B A B A B (`t2-r349` to `t2-r354`), with the
client's mountstats (`84-reply-time/ms.py`), and puts the stock stack
back.

## Result: no drive, tag or SGL difference; staying is lower in every pair

`run349.out`; the traces are `tail-sgl-r349.txt` to `tail-sgl-r354.txt`.
No new kernel warning, no drive stepped down, and the stock stack is back
in the settled setting.

| run | staying | GB/s | WRITEs over 2 ms | `nfsd_vfs_write()` mean, us | drive request mean, us | requests over 2 ms | client RTT, ms |
|---|---|---|---|---|---|---|---|
| `t2-r349` | off | 69.80 | 0.87% | 375.0 | 308.4 | 13,329 | 1.341 |
| `t2-r350` | on | 69.69 | 0.81% | 376.8 | 306.8 | 11,958 | 1.346 |
| `t2-r351` | off | 68.99 | 0.75% | 364.1 | 291.1 | 9,219 | 1.322 |
| `t2-r352` | on | 68.47 | 0.79% | 365.5 | 289.7 | 9,156 | 1.286 |
| `t2-r353` | off | 71.22 | 0.70% | 382.6 | 313.0 | 9,833 | 1.310 |
| `t2-r354` | on | 69.72 | 0.95% | 388.1 | 317.5 | 13,096 | 1.300 |

- **Round 84's larger tail does not repeat.**  WRITEs over 2 ms are 0.70
  to 0.87% with staying off and 0.79 to 0.95% with it on, and the first
  pair has fewer with staying on.
- **The slow writes are the drives' own phases.**  In each 5 s window a
  drive either has about 2,000 to 2,700 requests over 2 ms or almost
  none, and which drives are in a slow phase changes from run to run,
  not with staying:

  | run | nvme0 | nvme1 | nvme2 | nvme3 | nvme4 | nvme5 | nvme6 | nvme7 |
  |---|---|---|---|---|---|---|---|---|
  | r349 off | 2122 | 220 | 2420 | 0 | 2402 | 1986 | 2072 | 2107 |
  | r350 on | 620 | 2040 | 319 | 1894 | 2043 | 2462 | 0 | 2580 |
  | r351 off | 2240 | 2724 | 2198 | 0 | 0 | 0 | 2057 | 0 |
  | r352 on | 0 | 0 | 1227 | 1658 | 150 | 1978 | 2460 | 1683 |
  | r353 off | 2410 | 2450 | 173 | 0 | 2393 | 0 | 2407 | 0 |
  | r354 on | 2243 | 30 | 1959 | 2626 | 1974 | 1582 | 0 | 2682 |

  Neither the count nor the temperature (35 to 52 C, nvme2 hottest)
  follows the rate: r351 had the fewest and was the slower run of its
  pair.  A drive request's mean follows the rate (more queued at a higher
  rate), not staying.
- **Nothing waits for a tag:** `blk_mq_get_tag()` averages 0.6 us and
  never takes over 100 us, in any run.
- **The SGLs are the same.**  Every run: 3.81 requests of 256 KiB a
  WRITE, 34.87 `nr_phys_segments` a request, about 133 SGL entries a
  WRITE.  The entries written fall short of `nr_phys_segments` by 0.01 to
  0.08% (merges round 55 did not see), alike in A and B.
- **The rate.**  Staying off: 69.80, 68.99, 71.22 (mean 70.00).  On:
  69.69, 68.47, 69.72 (mean 69.29).  B is lower in every pair, by 0.11,
  0.52 and 1.50 GB/s.  With round 82's three untraced pairs (2.0, 3.2,
  1.6), staying is lower in all six; the run-to-run spread of A alone
  (2.2 GB/s) is as large as the gap, so its size is not known better
  than 1 to 3%.
- **Where that leaves it.**  Neither the drives, the block layer's tags,
  nor the SGLs differ with staying, and the client's RTT does not show
  it either (B lower in two pairs).  What staying does change is the
  CPU's work: round 82's context switches (+30%) and `finish_task_switch`
  (+7k cycles a WRITE).  That the trace, which loads the CPUs, narrows
  the gap rather than widening it is not explained.
- `writes timed` counts about 445k WRITEs in a "5 s" window at 66k a
  second: the window runs about 6.7 s while the probes attach.  Ratios
  and means are not affected; the counts compare between runs.
