# Round 22: a loan takes over the skb's page-pool references

**What was profiled:** fl41-loan64, round 21's B at the settled setting. nfsd cost 112.2 µs a WRITE.
- The largest self frames were page references: `page_pool_put_netmem_bulk` 6.0 µs a WRITE (the loan's release), `svc_tcp_rx_borrow_segment` 3.5 (57% of it the `struct page` check, round 20's annotate) and `napi_pp_put_page` 2.5 (the skb's free).
- In both put frames the hot instruction is one `ldaddal` on the page's `pp_ref_count`: 70% of the bulk put's samples and 77% of `napi_pp_put_page`'s.

**Why each atomic misses** ([refs.txt](refs.txt), `refs.bt`, the count just before each drop):
- xeu's `rx_buf_size` 8192 cuts each 16 KiB page into two receive buffers (`page_pool_alloc_frag()`), so a page holds two frames, usually loaned by two records on different nfsd CPUs.
- At the skb's free the count was 3 or 4 (60% and 39%); at the loan's release it was 2 in 85% of drops. One frame to a page would give 2 and 1, and a release at 1 takes `page_pool_unref_netmem()`'s read-only path.
- So one page's count is written by the ring's allocator and by every frame's loan and skb from several CPUs: each frame's borrow (+1), skb free (-1) and release (-1).
- A page to a frame is ruled out: it doubles the region (the footprint rule).

## The change

- **"net: add skb_frags_hand_over()"** (`5ea50b07c428`), in `include/linux/skbuff_ref.h`: an skb's last frags leave it with their references, and freeing it no longer drops them. Only `nr_frags` changes: `skb->len` has to stay, as `tcp_read_sock()` finds the skb's end by it before freeing it. Kept separate for netdev.
- **"SUNRPC: Take over an skb's page-pool references for a loaned frag"** (`5c004783a05d`): when a read takes an uncloned, unshared, recycling skb to its end, each frag a record borrows whole as a new entry holds the skb's reference instead of taking its own, and those that end the skb are handed over before `->read_sock` frees it. Every other entry takes its own reference, and so does each that was to inherit one if the walk materializes, merges or fails.
- For such a frag the count goes 1 (the skb's) to 1 (the loan's) to 0, where it went 1, 2, 1, 0: two of each frame's three atomics on nfsd are gone.
- `build-sunrpc.sh` takes the tree's `skbuff_ref.h` for the helper (`0dbbda67c6b4`).
- **The module measured** was the change as first committed, before the helper was split out (`52133f1dd8e0`, kept as `….pre-split-r22`). The tip's build has the same machine code: the two modules' disassembly is the same, all 96,351 instructions.

**Runs 2026-10-26 on tardis2**, build 499 (`bca1a2f143eb`), the settled setting, nfsd the kernel's own (`3F0C341E86E266D5FDF2ADF`). The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run43.sh`, the runs `/var/tmp/zc-run-logs/t2-r42/`.
- A is the tip before the change (`1D9C18007E10E362F8EFD03`, round 21's B), B the change (`4BC3A057D06EC2EA1655F73`). Before every run sunrpc was swapped (`swap-sunrpc.sh`) and the host settled; every load checked dmesg for a new WARNING, and there was none.
- KUnit on B: 7, 10, 52 and 7 pass.

## Outcome: kept. nfsd 3 µs cheaper a WRITE

**The takeover works** ([refs.txt](refs.txt), 10 s of a run on each side):

| | loan releases | skb frees of a page | skb frees a release |
|---|---|---|---|
| A | 48.8 M | 48.4 M | 0.99 |
| B | 45.3 M | 5.5 M | 0.12 |

The other 12% are the frames in an skb's head and the frags of skbs a record ends part-way through. At release the count is still mostly 2: the page's other frame is on loan.

| run | sunrpc | GB/s | ring µs a WRITE | nfsd µs a WRITE | region peak in use, pages |
|---|---|---|---|---|---|
| r42-1-loan64-A | before | 40.54 | 50.1 | 107.9 | 6469 |
| r42-2-loan64-B | takeover | 40.51 | 49.1 | 107.9 | 6372 |
| r42-3-loan64-A | before | 40.50 | 49.6 | 110.9 | 6540 |
| r42-4-loan64-B | takeover | 40.53 | 49.4 | 104.1 | 6491 |
| r42-5-loan64-A | before | 40.49 | 49.2 | 106.1 | 6388 |
| r42-6-loan64-B | takeover | **38.52** | 48.6 | 106.1 | **7551** |
| r42-7-loan64-A | before | 40.53 | 49.4 | 108.2 | 6459 |
| r42-8-loan64-B | takeover | 40.50 | 49.0 | 102.3 | 6430 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt)).

| | rings µs a WRITE | nfsd µs a WRITE |
|---|---|---|
| A, 4 runs | 49.58 ± 0.39 | 108.28 ± 1.98 |
| B, 4 runs | 49.02 ± 0.33 | 105.10 ± 2.43 |
| B - A | -0.55 ± 0.25 | **-3.18 ± 1.57 (-3%)** |

- **nfsd:** three of B's four runs (102.3, 104.1, 106.1) are at or below A's lowest (106.1).
- **The profile** (fl42-loan64 against fl41-loan64, [top-nfsd.txt](top-nfsd.txt)): the receive (`svc_tcp_recvfrom` with what it calls) 38.2 to 34.5 µs a WRITE; `napi_pp_put_page` 2.5 to 1.1; the borrow 3.5 to 2.0. The release is the same (8.8 to 8.6).
- **The region:** not raised. B's peaks are 6372 to 6491 without r42-6, A's 6388 to 6540; r42-6's 7551 is its stall, below.
- **Not changed:** the rings, the rate, refused loans, hand-offs and wakes a WRITE.
- The Rx and Tx interrupts in [cost.txt](cost.txt) are not valid, as in round 21.

## A stall in r42-6 (B), cause not found

- One WRITE took 4.58 s; every other run's slowest took 115 to 138 ms. The run averaged 38.52 GB/s, but its 10 s window was normal (40.49 GB/s on the wire, 38,303 WRITEs a second).
- The drives wrote 75 to 90% of their rate for about 13 s early in the run (about 07:47:53 to 07:48:06), then recovered.
- **Ruled out:** a connection dropped by a fault in the hand-over (all 16 connections carried 80 to 95 GB, as in A's runs, so none reconnected); a kernel message on either host (none about RPC or NFS, no WARNING); the client's containers (none started between 07:44:50 and 07:48:29); retransmits (the client's 4.6 MB is within the other runs' 3 to 27 MB).
- It is like the stalls of rounds 14 (1.3 s) and 16 (14.5 s), also not found and seen with and without their changes. One B run in four here; r42-8 (B) had none.
- **To watch:** the slowest WRITE of every run, now in [cost.txt](cost.txt).

## Latency

[latency.txt](latency.txt):

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, the takeover | 453 µs | 528 | 594 | 463 |
| A1, before | 457 | 537 | 610 | 464 |
| B2, the takeover | 432 | 502 | 569 | 442 |
| A2, before | 440 | 514 | 578 | 451 |

- B's p50 is 4 to 8 µs below A's in both pairs, inside what runs differ by: the same.

## Decision

- **Kept (Mike, 26 October), before the A/B finished: "there is no way this isn't needed long-term".** It is code with no parameter, so `settled.sh` has nothing to hold.
- Split as Mike asked: the skb change is its own `net:` commit, and the SUNRPC commit opens with the reference count's life before and after.
- **Open:** netdev's view of a `->read_sock` actor lowering `nr_frags` of an skb still on the receive queue, and the stall above.
