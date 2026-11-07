# Round 64: the KUnit fault hook's flag off the spare counter's line

**The change (Mike, 7 November):** round 62 found
`svc_tcp_rx_append_ref()` spending 1.76k of its 2.81k cycles a WRITE on one
load, of `svc_tcp_rx_kunit_fault`, the KUnit fault hook's flag.  It
shares a cache line in sunrpc's `.bss` with `svc_tcp_spare_pages`, which
every save and restore of a partial record writes.  "SUNRPC: keep the fault
hook's flag off hot lines for the KUnit suites" marks the flag
`__read_mostly`.

## The A/B ([run237.sh](run237.sh), [run237.out](run237.out))

Two `sunrpc.ko` built on this host by `xeu-rig/build-sunrpc.sh` from build
514's own source (`6089265ea2c1`, worktree `/dev/shm/wt-flag`): A as 514
has it, B with the change.  Both builds are this host's (gcc 14.3.1), so
neither is set against gitlab's stock module.  Every one of the 439 exports
has 514's version.  In A the flag is at `.bss`+0x120 and the counter at
+0x130, one line; in B the flag is in `.data..read_mostly`.  Swapped in by
`swap-sunrpc.sh` in turn, A B A B, each run a loan run on both ports at
`twoport.sh`'s defaults after the settled setting, a trim and 120 s of rest,
sampled as `t2-r235`.  The stock sunrpc was put back at the end, with
nothing in `updates/`, and the audit found the settled setting.

| run | | GB/s | WRITEs a second | `svc_tcp_rx_append_ref` cycles a WRITE | the five loan functions | all of nfsd |
|---|---|---|---|---|---|---|
| `t2-r237` | A | 70.36 | 67089 | 2733 | 16.72k | 252.1k |
| `t2-r238` | B | 70.56 | 67263 | **1206** | 15.25k | 250.7k |
| `t2-r239` | A | 71.42 | 67989 | 3044 | 17.98k | 258.9k |
| `t2-r240` | B | 71.19 | 67798 | **1139** | 15.74k | 254.5k |

Read with `../62-map-walk/annotate62.py` (`annotate-r237.txt` to
`annotate-r240.txt`) and `pmu62.py`.

- **The miss is gone:** `svc_tcp_rx_append_ref` falls from 2.73k and 3.04k
  cycles a WRITE to 1.21k and 1.14k, the 1.76k the flag's load cost.  The
  hottest instruction in B is its `ret`, with 2.2 L2 refills a WRITE,
  where A's flag load had 16.
- **nfsd as a whole** is 255.5k cycles a WRITE on A and 252.6k on B, about
  1.1% fewer.  Each B is lower than the A before it, but the two A runs
  are 6.7k apart, so the function's figure is the measure.
- **The rate does not move,** as on every run of this rig, which memory
  bandwidth holds: the saving is in CPU.
- `svc_tcp_spare_pages` itself still bounces between CPUs; its writers
  (`svc_tcp_restore_pages()`, `svc_tcp_save_pages()`) pay for that, the
  KUnit flag's readers no longer do.
