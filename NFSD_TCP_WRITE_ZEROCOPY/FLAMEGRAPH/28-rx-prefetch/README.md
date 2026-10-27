# Round 28: xeu prefetches the next received frame while it builds a frame

**What was profiled:** fl52-loan64, round 27's B at the settled setting. The rings cost 49.3 µs a WRITE.
- **The receive poll's first touch of each frame:** `xeu_rx_poll` builds each skb straight from the buffer the card has just written, and nothing touches it before.
- **Three cache misses on the rings** (CPUs 0, 2, 4, 6), µs a WRITE:

| frame | self | where it stalls |
|---|---|---|
| `eth_type_trans` | 4.16 | 82% on the first load of the Ethernet header |
| `napi_build_skb` | 1.81 | about half on the buffer's `struct page` (`virt_to_head_page`) |
| `__build_skb_around` | 1.28 | 67% on the store that clears the `skb_shared_info` at the buffer's end |

- **Why it is worth doing:** drivers usually prefetch the header and the shared info when they take a completion. xeu prefetches nothing.
- **Unlike round 27:** the next frame is always the next completion of the same ring, so finding it costs no extra miss.

## The change

**kmod-xsight 2.0.1-10**, "xeu: prefetch the next received frame while a frame is built" ([the patch](xeu-prefetch-the-next-received-frame-while-a-frame-is-built.patch); kernel-rpmtree `6d37e4d`, Patch10 on release 9). Mike agreed to change Xsight's driver for this round, as in round 20.
- **The parameter:** `rx_prefetch` (bool, 0644, default N), read at each frame.
- **What it does:** once the next completion is valid, the poll prefetches three things for that frame while it builds and hands on the current one:
  - the two cache lines of its headers, from the same headroom the skb is reserved with, so with `rx_ip_align` as well;
  - its `skb_shared_info`, for writing;
  - its `struct page`.
- **Finding the next frame:** its first descriptor follows the current frame's last. A completion found at any other descriptor is reported once, and the run's driver stopped at that report; none came.
- **What it doesn't touch:** no page is held longer and none more are kept, so the region is not touched. Copy mode is received the same way and gains too.
- `settled.sh` holds `xeu.rx_prefetch` at Y and `apply` writes it (`7aca1030c33d`). With a kmod that lacks it, the audit is BAD.

**Runs 2026-10-27 on tardis2**, build 499 (`bca1a2f143eb`), the settled setting. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run55.sh`, the runs `/var/tmp/zc-run-logs/t2-r52/`.
- **The xeu module:** one module for both sides, release 9 with the patch (`2C05DF674C9D013185FDB34`, `r28-xeu.ko`). Release 9 rebuilt from its tarball and the spec's ten patches gives the installed module's srcversion (`E6525B5F7DA25C216AE0DCD`), and the patched module's symbol versions are the installed one's.
- **Installing it:** it went in `updates/` and xeu was reloaded once, with no NFS client connected. Port a was then set up again with `port-setup.sh enp161s0f0 0` and settled.
- **The sides:** A is `rx_prefetch` N (release 9's path), B is Y. Each run set it after `settled.sh apply`.
- **sunrpc and nfsd are build 499's own** (`EFD53C53F61C1F0CDF37D89`, `3F0C341E86E266D5FDF2ADF`), not the tip's sunrpc that rounds 21 to 27 were measured on. So the absolute figures are higher than round 27's: they lack, among others, round 25's later wake, about one segment and 2 µs on the rings a WRITE. A and B differ only in the prefetch, which runs on the rings before anything sunrpc does.
- No run raised a new WARNING.
- **The kmod in the packaging:** the patch in kernel-rpmtree places the new helper above `xeu_rx_poll`'s kernel-doc comment, where the measured module had it between the two. The code is the same.

## Outcome: kept. The rings 2.45 µs cheaper a WRITE

| run | rx_prefetch | GB/s | ring µs a WRITE | nfsd µs a WRITE | segments sent a WRITE | region peak in use, pages |
|---|---|---|---|---|---|---|
| r55-1-loan64-A | N | 40.46 | 52.1 | 111.4 | 6.78 | 6508 |
| r55-2-loan64-B | Y | 40.47 | 48.7 | 110.7 | 6.61 | 6476 |
| r55-3-loan64-A | N | 40.50 | 50.5 | 112.1 | 6.57 | 6381 |
| r55-4-loan64-B | Y | 40.46 | 47.6 | 111.1 | 6.51 | 6364 |
| r55-5-loan64-A | N | 40.50 | 51.2 | 111.5 | 6.58 | 6311 |
| r55-6-loan64-B | Y | 40.44 | 48.8 | 113.6 | 6.69 | 6289 |
| r55-7-loan64-A | N | 40.47 | 49.9 | 112.8 | 6.57 | 6463 |
| r55-8-loan64-B | Y | 39.68 | 48.8 | 112.7 | 6.64 | 7718 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt)).

| per loaned WRITE | A | B | B − A |
|---|---|---|---|
| rings, µs | 50.92 ± 0.95 | 48.48 ± 0.59 | **−2.45 ± 0.56 (−5%)** |
| nfsd, µs | 111.95 ± 0.65 | 112.03 ± 1.36 | +0.08 ± 0.75 |
| segments sent | 6.62 | 6.61 | −0.01 |
| enqueues · wakes · switches | 5.50 · 4.37 · 5.87 | 5.48 · 4.36 · 5.86 | about 0 |

- **The rings:** about 4.4 standard errors. The pairs are −3.4, −2.9, −2.4 and −1.1, and every run with the prefetch (47.6 to 48.8) is below every run without it (49.9 to 52.1).
- **A's first run:** it had page-pool recycle-ring-full events at 0.70 a WRITE, as the first run after a settle does. The other three A runs alone give 50.53, still 2.05 above B.
- **Not changed:** nfsd, the rate, segments, enqueues, wakes and switches.
- **The region's peak:** without r55-8, B's runs are 6289 to 6476 against A's 6311 to 6508. Not raised.
- **r55-8 stalled once:**
  - ring 2 ran out of receive buffers and dropped 2,226 frames at the card, and the client retransmitted;
  - its slowest WRITE took 1.39 s (every other run 112 to 137 ms), the rate fell to 39.68 GB/s and the region's peak rose to 7718;
  - it is the WRITE stall seen before without this change: r39-5-A 1.9 s, r40-4-B 1.4 s, r42-6-B 4.6 s, fl46 5.9 s;
  - its rings figure, 48.8, is the other B runs'.
- The Rx and Tx interrupts in [cost.txt](cost.txt) are not valid, as in round 21.

## The profile

fl55a-loan64 (N) against fl55b-loan64 (Y), one profiled run each, annotated on the ring CPUs while loaded ([top-rings.txt](top-rings.txt), [annotate.txt](annotate.txt)). Rings busy 49.47 and 48.88, µs a WRITE:

| frame | self, N | Y | Y − N | the stall, N → Y |
|---|---|---|---|---|
| `eth_type_trans` | 4.17 | 1.09 | −3.07 | 83% on the header's load → 29%, spread |
| `skb_gro_receive` | 2.38 | 1.59 | −0.79 | |
| `napi_build_skb` | 1.88 | 1.25 | −0.62 | the `struct page` check, partly |
| `__build_skb_around` | 1.38 | 0.79 | −0.59 | 67% on clearing the shared info → 15% |
| `gro_list_prepare` | 0.69 | 1.45 | +0.76 | not explained |
| `xeu_rx_poll` | 3.23 | 5.08 | +1.85 | the prefetch itself |

- **What went:** about 5 µs a WRITE of the misses it was aimed at.
- **What it costs in `xeu_rx_poll`:**
  - about 0.8 µs waits at the header's prefetch, whose address needs the next descriptor's software entry, a miss of its own;
  - `xeu_rx_buf_size()` calls `cache_line_size()` out of line once a frame.
- **The totals:** one profiled run a side cannot resolve 2.45 µs. The A/B measures it.
- **Left:** loading the next descriptor's entry a frame earlier would take most of the 0.8, at the price of a second look-ahead.

## Latency

[latency.txt](latency.txt), untraced runs at iodepth 1:

| | GB/s | p50 | p90 | p99 | mean |
|---|---|---|---|---|---|
| B1, Y | 7.98 | 444 µs | 514 | 602 | 446 |
| A1, N | 7.97 | 440 | 518 | 594 | 451 |
| B2, Y | 8.41 | 416 | 494 | 553 | 429 |
| A2, N | 8.34 | 424 | 502 | 586 | 436 |

- The same.

## Decision

- **Kept (Mike, 27 October):**
  - kmod-xsight 2.0.1-10 is committed in kernel-rpmtree (`6d37e4d`, not pushed);
  - `settled.sh` holds `rx_prefetch` Y (`7aca1030c33d`).
- **Until a 2.0.1-10 rpm is installed,** tardis2 runs the measured module from `/lib/modules/7.3.0-7.hs.499.el10.aarch64+16k/updates/xeu.ko`, which takes precedence over the rpm's `extra/xsight/xeu.ko`.
- **The initrd loads xeu at boot.** Until 27 October it carried an xeu older than release 9, with neither `irq_msix_mask` nor `rx_prefetch`, so every boot needed a reload of xeu before `settled.sh` would pass. It was rebuilt that morning with `dracut -f /boot/ofp-rocky/$K/initrd.new $K` and swapped in; it now carries `updates/xeu.ko` (`2C05DF674C9D013185FDB34`). The one before is beside it as `initrd.pre-xeu-r28-2026-10-27`. Rebuild it again whenever the module in `updates/` changes or a new kmod-xsight rpm is installed.
- It changes nothing for sunrpc or nfsd, so it stands beside the loan series, not in it.
