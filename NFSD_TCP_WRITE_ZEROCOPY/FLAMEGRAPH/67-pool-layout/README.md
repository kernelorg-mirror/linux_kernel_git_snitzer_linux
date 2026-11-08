# Round 67: whether struct svc_pool's layout matters

**The variants (Mike, 7 November: "I'm not loving growing the struct
with excessive holes ... build the 320-byte layout variant and test it
too"),** without the handoff or the prefetch:

- **B**, as round 66's: the queue shares its line with the thread
  counts.
- **S**, the queue on a line of its own (`4d81ec261bf0`), 256 bytes with
  a 40-byte hole.
- **S5**, "SUNRPC: Lay out struct svc_pool by who writes each line"
  (`7a8f1f776f25`): five lines, 320 bytes, 96 of them holes.

## The runs ([run253.sh](run253.sh), [run253.out](run253.out))

Swapped in B S S5 B S S5 (`t2-r253..r258`), each run as round 65's.  No
new kernel warning, the stock stack put back, the setting audited.

| run | | GB/s | nfsd | `__lwq_dequeue` | `svc_recv` | far |
|---|---|---|---|---|---|---|
| `t2-r253` | B | 70.67 | 246.1k | 383 | 3974 | 2.0% |
| `t2-r256` | B | 70.79 | 248.0k | 457 | 4560 | 1.0% |
| `t2-r254` | S | 71.09 | 250.1k | 1656 | 3154 | 0.9% |
| `t2-r257` | S | 71.47 | 248.7k | 1142 | 2880 | 0.5% |
| `t2-r255` | S5 | 70.70 | 248.7k | 1500 | 2975 | 2.0% |
| `t2-r258` | S5 | 69.83 | 247.3k | 1544 | 2887 | 2.4% |

- **S5 gains nothing over S** in cycles, for 64 bytes more a pool.  Its
  dequeue pays the queue line's miss as S's does.
- **The rate follows the client's far share,** not the variant: S's two
  runs, the fastest, had the lowest far shares (0.9% and 0.5%), and the
  slowest run, S5's 69.83, the highest (2.4%).  SH's high runs in round
  66 had low far shares too, so round 68 alternates more runs of B and
  SH and looks at where the client sends from.
