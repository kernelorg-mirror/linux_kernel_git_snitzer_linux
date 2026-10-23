# Round 02: `svc_tcp_rx_exchange` checks only a record's counts

**The change:** "SUNRPC: Check only a TCP record's counts when it changes hands" (`49f0fc4564ec`).
- `svc_tcp_rx_exchange()` now checks only the counts of the record it moves between the socket and a thread.
- `svc_tcp_rx_release_refs()` still checks each borrowed entry once, before its page goes back.

**Run 2026-10-23 on tardis2**, build 494, in the settled setting with round 01's netfilter change.
- A is the tip before the change: `fix-sunrpc.ko`, srcversion `82ACAC76B5F85A2645B58A4`.
- B is the change: `refs-sunrpc.ko`, srcversion `1CC95EF56506263BC30D23B`. It was built by `build-sunrpc.sh` and is left loaded.
- The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run19.sh`.

## Why: round 01's nfsd ranking

`svc_tcp_rx_refs_valid` cost 7.05 µs a WRITE on nfsd:
- 88.6% of it came from `svc_tcp_rx_exchange`, which walks every entry of the record so far;
- 10.6% from `svc_tcp_rx_release_refs`.

`xchg.bt` counted the calls beside the round's profile (`xchg.txt`):

| | calls | a record |
|---|---|---|
| `svc_tcp_rx_exchange` | 52.86 M | 22.2 |
| `svc_tcp_recvfrom` | 28.81 M | 12.1 |
| `svc_tcp_rx_release_refs` (about one a record) | 2.38 M | 1 |

A 1 MiB WRITE is handed between the socket and a thread about 22 times. That makes the old checks about 22 walks of a record that grows to 126 entries, so their cost grew with the square of the record's length.

## Outcome: kept on the profile. The A/B cannot see it

**KUnit on B:** 7, 50, 7 and 10 pass, with no failure and no skip.

**The profile** (fl2-loan64, B; [nfsd.svg](nfsd.svg), [top-nfsd.txt](top-nfsd.txt)):

| | round 01 | round 02 |
|---|---|---|
| `svc_tcp_rx_refs_valid` | 7.05 µs a WRITE | 0.90 (release only) |
| `svc_tcp_rx_exchange`, with what it calls | 7.52 | 3.10 |

So nfsd spends about 4.4 µs less a WRITE in the hand-offs, and the walk in the exchange is gone.

**The A/B** (untraced; A B A B A B, each after a reload; every run settled and DRIVE-LIMITED, no drive stepped down):

| run | GB/s | ring µs a WRITE | nfsd busy | nfsd µs a WRITE | refusals |
|---|---|---|---|---|---|
| r2-1-loan64-A | 40.37 | 76.1 | 13.04% | 161.7 | 0.97 |
| r2-2-loan64-B | 40.43 | 77.3 | 13.05% | 162.7 | 0.99 |
| r2-3-loan64-A | 40.42 | 78.2 | 12.91% | 161.2 | 0.95 |
| r2-4-loan64-B | 40.39 | 79.2 | 13.09% | 162.9 | 0.89 |
| r2-5-loan64-A | 40.41 | 80.0 | 13.53% | 169.0 | 0.83 |
| r2-6-loan64-B | 40.42 | 80.2 | 12.98% | 161.6 | 0.91 |

- **nfsd:** means of 164.0 for A and 162.4 for B, a difference of -1.6 µs ± 2.6.
- **Why it cannot see 4 µs:** 4 µs a WRITE is 0.3 points of nfsd's 13% over 48 CPUs, and single runs differ by more than that.
- **The rings** were not expected to change, and did not. Both A and B drifted up from 76 to 80 µs over the half hour.
- **The decision:** kept on the profile's evidence. The work it removes is measured directly, and it changes no behaviour.

## What the method learns from this round

- **nfsd's busy time from mpstat is too coarse** for a change of a few µs a WRITE on nfsd. For such a change, compare the function's cost in the two profiles, the before and after rounds. Use the A/B to show that nothing else moved.
- **Annotate against the module that was profiled.** A swap leaves a different `sunrpc.ko` in `updates/`, and perf disassembles that file.
  - `perf buildid-list -i perf.data` names the profiled module's build ID; keep a copy of each module by build ID.
  - Kernel functions need the running kernel's text: there is no vmlinux with symbols for build 494 on tardis2, so copy the bytes from `/proc/kcore`.

## The rings' ranking now (fl2-loan64, [rings.svg](rings.svg)): 83.0 µs a WRITE, profiled

| item | µs a WRITE | note |
|---|---|---|
| `skb_defer_free_flush` | 8.85 | of it `napi_pp_put_page`, 7.01 self |
| `_raw_spin_unlock_irqrestore` self | 8.07 | the end of time with interrupts off: `enable_irq` 6.51 and the wakes |
| `napi_consume_skb` | 7.50 | |
| `svc_data_ready` | 6.98 | |
| `page_pool_alloc_frag` | 6.27 | |
| `eth_type_trans` | 6.19 | the first read of a frame the card wrote: a driver prefetch, for Xsight |
| `nf_hook_slow` | 1.98 | was 8.57 before round 01: what is left is the `zc_notrack` raw chain and SELinux |

**Next:**
- **round 03:** `net.core.skb_defer_max` 0 against 128;
- **round 04:** measure how often the ptr_ring is full;
- **round 05:** the `svc_data_ready` path.
