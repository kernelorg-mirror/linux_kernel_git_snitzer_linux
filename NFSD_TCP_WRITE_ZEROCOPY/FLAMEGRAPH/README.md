# FLAMEGRAPH: analyze, fix, test, repeat

A reference for improving the server's CPU cost a step at a time, started
2026-10-23 on tardis2 (build 494, 16 KiB pages) after "SUNRPC: Wake an idle
service thread only once" (`2fc9cd2e839b`) cut nfsd's wakes 2.6 times and
took 1 to 2.5% off the rings. Mike: "every win helps! Remember sometimes it
takes 5 or 20 things to come together to realize full performance".

**The target is loan mode (Mike, 2026-10-23: "I want to squeeze out all
lost performance for loan mode").** Every round profiles, ranks and tests
the loan run, and a change is judged by what it saves a loaned WRITE.
AGENTS.md's rule runs one way: copy mode is not to be made faster at the
loan's expense, not the reverse.

**No copy runs (Mike, 2026-10-27: "copy is not important at this
time").** Do not run copy mode in a round, its A/B, a look, the checks
after a boot, or the measure of a host change (a boot setting, a module, a
kernel). Do not profile it and do not put it in a driver beside the loan
runs. This holds even where a round's text, a handoff or an older plan
asks for "copy and loan": run the loan side only, and say in the round
that copy was not run. Copy comes back only when Mike asks for it.

Each round is a directory here, numbered in order:

| round | what was profiled | the most costly thing found | the change | outcome |
|---|---|---|---|---|
| [01-loan64-baseline](01-loan64-baseline/README.md) | one-port loan64, with the one-wake fix: rings 92.0 us a WRITE, nfsd 184.1 | page_pool's put and get of the region's pages (13.8 us); conntrack from podman's unused netavark table (9.0 on the rings, 9.4 on nfsd) | conntrack: the leftover table deleted and port 2049 untracked, in settled.sh | kept: rings 92.6 to 78.7 us a loaned WRITE (-15%), copy 67.9 to 63.4 (-7%) |
| [02-refs-valid](02-refs-valid/README.md) | the same, with round 01 | `svc_tcp_rx_refs_valid` walked the record on each of 22 hand-offs a WRITE (7.05 us on nfsd) | `49f0fc4564ec`: the exchange checks only counts | kept on the profile: refs_valid 7.05 to 0.90, the exchange 7.52 to 3.10 us on nfsd; the A/B (mpstat) cannot resolve it |
| [03-skb-defer](03-skb-defer/README.md) | the same, with round 02 | deferred skb frees on the ring (8.85 us), `napi_pp_put_page` a remote line | `net.core.skb_defer_max` 0, in settled.sh | kept: rings 80.0 to 68.0 us a loaned WRITE (-15%), nfsd 167.5 to 159.8 |
| [04-region-stack](04-region-stack/README.md) | build 499, one-port loan64 | the region pools' 256-page stack overflows: 7% of pages back to the region, about 5 us a WRITE | `dc23eac70f34`: `page_pool.rx_region_stack` 1024 | **not kept** (Mike, round 19): rings 60.0 to 56.5 us a WRITE (-6%), nfsd 111.5 to 107.5, but the region's peak +890 pages (14 MiB), against the L3. Not through the overflow, which happens only in the first run after the port opens. settled.sh holds 0 |
| [05-data-ready](05-data-ready/README.md) | the same, with round 03 | `svc_data_ready`: the `xpt_flags` line, two clock reads, a store into another thread's `svc_rqst` | `eff5f16d4f30`, `0f0ed4159a12` | kept: rings 68.4 to 65.0 us a loaned WRITE (-5%), nfsd 159.7 to 156.4 |
| [06-loan-lowat](06-loan-lowat/README.md) | the same, with round 05 | a WRITE taken up 12 times: wakes, hand-offs, ACKs (about 50 us on nfsd) | `c8cf27841351`: a low-water mark while a loaned record is incomplete | not kept, reverted (`b8c9ee7c6d2a`): nfsd 154.5 to 121.0 us a WRITE, but TCP ACKs every skb below the mark, so the rings 67.0 to 85.0 |
| [10-wait-rest](10-wait-rest/README.md) | round 05's state | the same churn (06b) | `c0995ea94798`: `svc_data_ready` holds `XPT_DATA` back for a short loaned record, `sk_rcvlowat` stays 1; `dd4fef2000a0` (10b): at most half the open window | 10 not kept: the rate fell to 36-38 GB/s and p90 to 4.3 ms, since what was wanted outgrew the window. 10b kept: nfsd 153.8 to 127.5 us a WRITE (-17%), rings 63.1 to 61.2, half the segments out, p90 0.537 against 0.569-0.594 |
| [11-socket-hold](11-socket-hold/README.md) | round 10b's profile | TCP's receive in the ring's softirq (`tcp_v4_do_rcv`, 11.05 us a WRITE) | hold the socket across a loaned record's arrival, so the backlog is processed on nfsd | not pursued, no run: it moves CPU without saving any, the rings are not the limit at 40.4 GB/s, a thread owning the socket is not woken by the backlog, and TCP removed its prequeue, the same idea, in 2017 |
| [12-drc-size](12-drc-size/README.md) | build 499, one-port loan64 | the v3 duplicate reply cache: 63 MB, full, 0 hits, about 4.5 us a WRITE of cold misses | `635e44bf37bf`: `nfsd.drc_max_entries`, an nfsd restart for each size; v3 stays, for flexfiles data servers | **kept at 4096** (settled.sh, after round 19): nfsd 111.5 to 108.1 us a WRITE (-3%); 32768 saves nothing; rings, rate and latency the same |
| [13-cred-cache](13-cred-cache/README.md) | round 10b's profile | `fh_verify` on every WRITE and COMMIT: handle decode, a new cred, two export lookups (5.7 us) | `916c11a2d1fc`: each nfsd thread installs its last cred again for the same user, and the decode's raised copy with it | **kept**: nfsd 126.5 to 118.3 us a WRITE (-6.5%); `prepare_creds` and `rcu_core` gone from the profile; latency the same |
| [14-page-prefetch](14-page-prefetch/README.md) | round 13's profile | each borrowed page's `struct page` missed on nfsd up to three times: `svc_tcp_rx_region_span` (2.7 us a WRITE on its `pp_magic` load), `svc_tcp_rx_borrow_segment` (3.4 of its 4.7 us self on `page_pool_page_is_pp()`), `napi_pp_put_page` (2.7) | `02a445664e23`: `prefetchw()` the `struct page` of each page a loaned skb's record bytes lie in, before they are examined; reusing the page-pool answer and one ref a page a skb not done | **kept**: nfsd 117.2 to 114.9 us a WRITE (-2%); `region_span` 3.2 to 1.0, the borrow 4.6 to 3.2, the walk 1.9; latency the same |
| [15-region-line](15-region-line/README.md) | build 499, one-port loan64 (fl15) | `svc_tcp_rx_append`'s region check reads `pool->rx_region` (offset 864) on the line with `rx_stack` and `rx_stack_n`, which the ring writes for every region page it takes or returns: about 0.6 us a WRITE of false sharing on nfsd | `085734569d9d`: `rx_region` moved to the struct's last line, written only at registration | in the kernel, no switch: the load 0.31 to 0.23 us a WRITE (fl17 against fl15), about 0.1, not 0.6 |
| [16-release-prefetch](16-release-prefetch/README.md) | round 14's profile | `page_pool_put_netmem_bulk` 5.5 us a WRITE, 85% one load of each page's `compound_head` behind the pool's ordered atomics, before the reply | `e5e84a05b686`: `prefetchw()` each borrowed page's `struct page` before the references are dropped | not kept: the release 1.6 us cheaper in the profile, nfsd 114.2 against 113.8 us a WRITE, inside the scatter; reverted (`a081854bc12c`). A 14.5 s stall in one B run, with the server's replies unacknowledged, cause not found |
| [17-read-on](17-read-on/README.md) | round 14's profile, and `xeu-rig/wakesrc.bt` | the wake path: 7.5 wake calls and 3.5 wakes a WRITE, about 1 us each to the waker; 4.4 of 8.7 passes through `svc_recv` find nothing; one partial hand-back in seven finds `XPT_DATA` set again | `5fde85ef4076`: a thread whose read left a loaned record incomplete reads on if `XPT_DATA` is set again when the socket lock is released | **kept**: 6.0 to 5.4 enqueues a WRITE, 4.56 to 4.34 wakes; nfsd 115.5 to 113.2 us a WRITE (-2%); latency the same |
| [18-looking-count](18-looking-count/README.md) | round 17's wake trace | wakes of idle threads for work a running thread takes anyway: 0.55 empty passes a WRITE that slept and were woken | `331bd9a8d93a` (18): no wake while a thread looking for work is counted; `05bb553779e2` (18b): counted only until it dequeues; `0fae825c58f7` (18c): no wake only while two are counted | not kept, all three reverted: 18 nfsd -6.9 us a WRITE at saturation but p50 at iodepth 1 0.465-0.498 against 0.42-0.44; 18b no gain; 18c nfsd -3.1 and rings -2.2, but 2 of 4 clean latency runs slow (+11 us mean) |
| [19-stack-and-drc](19-stack-and-drc/README.md) | build 499, rounds 04 and 12 together | | both at once, A both 0, B 1024 and 4096, 4 runs a side | rings -3.15 ± 0.75 us a WRITE, nfsd -3.95 ± 1.10 (not the -7.4 of the two apart), region +940 pages; latency the same. Mike keeps the reply cache, not the stack: "we cannot afford to increase the region" |
| [20-msix-mask](20-msix-mask/README.md) | build 499 at the settled setting | `enable_irq` on the rings, 10.4 us a WRITE: an ITS mask, unmask and resend for most polls | xeu `irq_msix_mask` (kmod-xsight 2.0.1-9): the ring masks its own MSI-X entry in the card, 4 runs a side | rings -9.97 ± 0.74 us a WRITE (-17%), nfsd, rate, region and latency the same; twice the Rx interrupts, each cheap. Kept at Y (settled.sh) |
| [21-credit-order](21-credit-order/README.md) | round 20's state | `svc_tcp_rx_credit_admit_to` 2.15 us a WRITE, nine tenths of it the walk over the records in flight | `c1b4326c9b99`: the credit lists walked from their newest end, 4 runs a side with sunrpc swapped | the walk gone from the profile (2.60 to 0.36 inclusive), but nfsd -0.13 ± 0.85: the misses moved, not saved. Kept: the walk out from under the lock |
| [22-ref-takeover](22-ref-takeover/README.md) | round 21's state | the page references: each page's `pp_ref_count` written by up to two frames' loans and skbs on several CPUs (two buffers to a page), so `napi_pp_put_page` 2.5, the borrow 3.5 and the release 6.0 us a WRITE are mostly one missed atomic each | `5ea50b07c428` (`skb_frags_hand_over()`), `5c004783a05d`: a frag borrowed whole inherits the skb's reference, 4 runs a side with sunrpc swapped | **kept**: nfsd -3.18 ± 1.57 us a WRITE (-3%), skb frees of a page 0.99 to 0.12 a release; rings, rate, region and latency the same. One 4.6 s WRITE stall in a B run, cause not found |
| [23-release-pairs](23-release-pairs/README.md) | round 22's state | the release, 10.1 us a WRITE: 37% of the pages it returns are the page of the entry before, both references the record's, and the first of each pair took the atomic | `3e95efcd28c0`: a run of entries on one page dropped with one `page_pool_unref_netmem()`, a read when the record holds them all; 4 runs a side with sunrpc swapped | **kept**: the bulk put's atomics 24.5 to 4.9 M in 10 s, but the miss moved to the read (release self 0.92 to 4.37); nfsd -1.33 ± 0.88 us a WRITE; rings, rate, region and latency the same |
| [24-release-prefetch](24-release-prefetch/README.md) | round 23's state | the release's own frame, 4.37 us a WRITE: each page's `struct page` miss, about 68 ns, taken one after another | `57f883cc3294`: `prefetchw()` each entry's `struct page` a batch (16 entries) ahead of its drop; 4 runs a side with sunrpc swapped | **kept**: the release 9.28 to 6.48 us a WRITE inclusive; nfsd -3.23 ± 1.14 (some of it A's drift); rings, rate, region and latency the same |
| [25-later-wake](25-later-wake/README.md) | round 24's state | the transmit path, 4.87 us a WRITE on nfsd, uncontended: 3.58 of it ACKs nfsd's read sends, about 4.8 a WRITE (7.98 in all), a 1 MiB loaned record taken up in about four passes | `2a75926c1458`: `svc_tcp_rx_want_eighths`, the share of the open window an incomplete loaned record waits for (4, round 10b's half); 4 against 6 on one sunrpc, 4 runs a side | **6 kept and settled** (settled.sh): nfsd -5.30 ± 0.89 us a WRITE (-5%), rings -2.2, one ACK a WRITE fewer; rate, region and latency the same. 7 held WRITEs at iodepth 1 until the delayed-ACK timer |
| [26-idle-timer](26-idle-timer/README.md) | round 25's state | the refused pass (about 1.05 a WRITE; a refusal paces every loaned WRITE: not pursued, Mike), then the wake: 2.0 us a WRITE thread wakes, the scheduler's, and the idle sleep's 5 s timer, about 0.5, which a pool without a minimum ignores | `6c6dc284fed0`: a thread its pool could not lose sleeps without a timer; A/B round 25 against it, 4 runs a side | **kept**: the timer's frames gone from the idle sleep (about 0.5 us a WRITE); the A/B nfsd -0.67 ± 1.31, inside the session's drift; rate, region and latency the same |
| [27-recv-prefetch](27-recv-prefetch/README.md) | round 26's state | the receive pass's loan bookkeeping: the skb's frag list (`nr_frags`, about 0.95 us a WRITE) and each frag's `struct page` (about 0.7) miss | `66bc91c765fa`: the shared info of the skb two ahead and the pages of the one ahead prefetched; A/B round 26 against it, 4 runs a side; a diagnostic with the pages loaded; skbs a pass probed | **not kept**: the A/B nfsd -1.90 ± 1.00, but the profile shows the pass 0.6 dearer, the look-ahead's own `sk_buff` loads missing; the `struct page` stall unmoved even when loaded ahead (the two halves of a page likely on different connections); 56% of passes are two skbs, so a deeper prefetch is not warranted |
| [28-rx-prefetch](28-rx-prefetch/README.md) | round 27's state | the rings' first touch of each received frame: `eth_type_trans` 4.16 us a WRITE (82% the header's load), `napi_build_skb` 1.81 (the `struct page`), `__build_skb_around` 1.28 (clearing the shared info) | kmod-xsight 2.0.1-10 (kernel-rpmtree `6d37e4d`): xeu's `rx_prefetch` prefetches the next frame's headers, shared info and `struct page`; A/B N against Y on one xeu, 4 runs a side | **kept** (`rx_prefetch` Y, settled.sh): rings -2.45 ± 0.56 us a WRITE (-5%); nfsd, rate, region and latency the same |
| [29-page-line-watch](29-page-line-watch/README.md) | round 28's state, the tip's sunrpc | round 27's question: is a region page's `struct page` line taken by the other half's connection between one thread's first touch and its borrow? | none: hardware watchpoints (perf `mem:`) on four live region pages' lines for 3 s each in a loan run, 109k accesses with CPU, time and call chain | **a look**: one writer before nfsd, the ring's `page_pool_alloc_frag_netmem` (92% of the borrows' last writes, 253 us before); no other CPU between the first touch and the borrow; the other half released 1.1 ms later. No sharing, nothing in xeu's layout to change; why the borrow's read is charged a stall is left |
| [30-tlb-look](30-tlb-look/README.md) | round 28's state | where round 28's prefetch waits: not on the next descriptor's entry (a few percent) but at the header's prefetch, a TLB walk; the rings walk page tables 69 times and miss the L1 TLB 1,171 times a WRITE, 37% of the walks in `gro_list_prepare`; the linear map is in 16 KiB pages (`rodata_full` on an N2, which has no BBML3) | none: `perf stat` and sampled `dtlb_walk` and `l1d_cache_refill_rd` on the ring CPUs, and a walk of `swapper_pg_dir`; then an A/B across a reboot into `rodata=noalias` (the linear map in 32 MiB blocks), 4 loan runs a side, no copy runs | **a look**: the descriptor prefetch not built. `rodata=noalias`: nfsd −5.23 ± 0.83 µs a WRITE (−4.7%); the rings' walks 69 to 0.5 a WRITE, but the rings' cost the same; rate, region and latency the same. **kept** (settled.sh). Again on the tip's sunrpc with the look on nfsd's CPUs on both sides (run64.sh): nfsd −8.83 ± 1.47 µs a WRITE (−8.8%), rings −2.95 ± 0.57 (−6.2%); nfsd's CPUs walk 108 to 2 times a WRITE |
| [31-no-qdisc](31-no-qdisc/README.md) | the current state: the tip's sunrpc, `rodata=noalias`, `rx_prefetch` Y (fl60) | sending, 14.9 us a WRITE on nfsd, 6.4 of it the ACKs its reads send; under every send `__dev_xmit_skb` 4.24, the qdisc's root lock 1.10 and its code 0.86 | `noqueue` as the port's root qdisc (`cd909d9ae8e2` lets settled.sh set it); 4 runs a side | **not kept**: nfsd −2.58 ± 1.46 us a WRITE, suggestive; but every `noqueue` run dropped 101 to 719 sends at a stopped transmit queue, three of four had a ~300 ms WRITE |
| [32-tx-queues](32-tx-queues/README.md) | the current state | nfsd's 48 CPUs on port a's 4 transmit queues, 44 of them by the flow's hash (rounds 07 and 31) | `XPS=spread` (12 CPUs a queue), and 8 rings with frames to 4 only (`RX_RINGS=4`, 8 queues, 6 CPUs a queue), against the settled setting; 4 loan runs a side | **not kept** (Mike): neither saves anything (nfsd +0.80 ± 1.17 and +1.17 ± 1.33 µs a WRITE), and 8 rings hold about 400 more region pages at peak. The settled setting measured again: rings 44.50, nfsd 90.30 |
| [33-cr-prefetch](33-cr-prefetch/README.md) | the current state | the completion entry, 31% of `xeu_rx_poll`'s L1 refills (round 30), for a prefetch several entries ahead | none: `napi:napi_poll` counted on port a's receive and transmit polls apart | **a look**: 94% of frames arrive in polls of 4 or more and 32% in polls at the budget, so inside a poll round 28's prefetch already has the next entry; what is left is about one miss a poll, where the card has not written the entry yet. Not built |
| [34-ring-stall](34-ring-stall/README.md) | the current state | the rings' memory stall, located by sampled stall cycles and refills on the ring CPUs; then xeu cutting `rx_buf_size`'s 8 KiB buffers from whole pages it counts once, without `page_pool_alloc_frag()`'s drain atomic | none | the look: the drain's atomic on `pp_ref_count` is the largest single stall, 20% of the rings' memory stall. The A/B, four runs a side: rings −1.60 ± 1.63, −0.17 without a pair that flow placement put off, nfsd +0.93 ± 1.46, the region's peak the same. **Kept** (Mike): the step `rx_buf_size` below a page should take, with no parameter; round 35 traces where the drain's wait went and what in the rings comes after it |
| [35-after-split](35-after-split/README.md) | the current state, xeu round 34's | where round 34's wait went, then the refill's loop: `xeu_rx_refill_lean()`, what does not change worked out once a refill, the ring's state in registers, each descriptor two eight-byte stores | r69 sampled; r70 to r72 diagnostic xeus timing the refill's loads, stores, the drain at its start and its loop | traced: the `dma_addr` load and the descriptor writes are near, nothing is inherited, the loop is 49 ns (98 cycles) a buffer. The A/B, counted (three pairs): ring cycles −2.7k a WRITE (−1.9%), instructions −6.9k, nfsd the same; the region's peak about +40 pages in five pairs of six. Also found: `cost.sh`'s µs are tick-sampled and read low. **Kept** (Mike), with no parameter: kmod-xsight 2.0.1-12 |
| [36-loan-frames](36-loan-frames/README.md) | the current state, xeu round 35's | nfsd's loan frames: append 3.7k, borrow 3.2k, release 2.6k cycles a WRITE (sampled), 7.8% of nfsd with the walk and prefetch; then the borrow's page-pool test from one load, the append's coalesce and region tests with their cheap conditions first | r75 sampled; r77 a diagnostic sunrpc timing the struct page loads, with the knob's two sides sampled in one run | the A/B, counted (four pairs): nfsd instructions −2.75k ± 0.54 a WRITE, cycles −0.45k ± 0.77, the peak not raised. Traced: the borrow's struct page loads are L1 hits (31.8 ns, the floor), so the look's stall was a chain of near loads; the borrow's own cycles fall 0.86k and the time goes to idle, the drives being the limit; the release's loads miss (12% at 64 ns or more). **Kept** (Mike): `eae183aaaff3` |
| [07-srv-qdisc](07-srv-qdisc/README.md) | round 05's state | half of each ACK and reply is `__dev_xmit_skb` | the server's qdisc `pfifo_fast` | not kept: no change (rings 66.9 against 66.1, latency inside L0's scatter); the cost is the transmit path's lock with 48 CPUs on 4 queues, not the qdisc |
| [08-notrack-needed](08-notrack-needed/README.md) | round 05's state | the `zc_notrack` chains run with no conntrack to skip | `zc_notrack` only while a table registers conntrack, in settled.sh | kept: rings 67.6 to 65.1 us a loaned WRITE (-3.7%), nfsd 160.6 to 154.2; latency unchanged |
| [09-big-tcp](09-big-tcp/README.md) | round 08's state | per-skb costs at about 20 GRO skbs a WRITE | BIG TCP: the server's `gro_max_size` and `gro_ipv4_max_size` 256 KB | not kept: no change (rings 63.3 against 63.4, latency the same); aggregates end at the client's 64 KB TSO bursts, so it needs the client's `gso_ipv4_max_size` too (Mike); the wake itself measured, 98.5% under 2 us |
| [L0-latency](L0-latency/README.md) | round 05's state, 4 in flight | p50 0.46-0.52 ms; about 0.2 ms is the drive, 0.02 the reply; the apparent wake tail is the tracepoint's stale stamp | `xeu-rig/lat.sh` | the baseline; runs differ by about 10%, so several a side |

## The queue (Mike, 2026-10-27)

The next rounds, in this order. Each starts when the one before it is
decided.

1. **Round 32: more transmit queues for nfsd's sends.** Done, not kept
   ([32-tx-queues](32-tx-queues/README.md)).
2. **Round 33: prefetch the completion entry further ahead.** A look, not
   built ([33-cr-prefetch](33-cr-prefetch/README.md)).
3. **Round 34: the rings' memory stall.** Located; its largest single
   place, page_pool's frag drain, taken out of xeu's refill. Kept, with no
   saving of its own that shows ([34-ring-stall](34-ring-stall/README.md)).
4. **Round 35: xeu's rings after round 34** (Mike: keep chasing the
   rings' wins in xeu that build on it). Traced: no single access in the
   refill waits far and nothing is inherited; the loop is long. A lean
   refill loop, the rings 2.7k cycles a WRITE cheaper; kept
   ([35-after-split](35-after-split/README.md)).
5. **Round 36: nfsd's own loan frames.** The borrow's and append's
   checks with their cheap conditions first and one load of the page's
   word: nfsd 2.75k instructions a WRITE fewer, the borrow 0.86k cycles
   cheaper, the time going to idle at the drives' limit. The borrow's
   struct page loads are L1 hits; the release's miss. Kept
   ([36-loan-frames](36-loan-frames/README.md)).
6. **Round 37: round 01's starting point, counted** (Mike, 28 October).
   The start replayed on build 494 with `rodata=full`, round 01's
   sunrpc, nfsd, xeu and settings, and set against the current state
   set against round 36's counts, to give the progress in counted cycles
   ([37-counted-start](37-counted-start/README.md)). Done: the rings
   192.5k to 137.9k cycles a WRITE (−28%), nfsd 481.4k to 294.6k (−39%);
   a fresh profile taken for round 38.
7. **Round 38: the release's struct page misses:** 12% of its first touches of a
   page at 64 ns or more despite round 24's prefetch a batch ahead, at
   most about 2k cycles a WRITE; its prefetch distance and the order it
   gives pages back in. Round 37's profile puts `svc_tcp_rx_region_span`
   and `svc_tcp_rx_skb_region_pages_after` close behind the release in
   nfsd's L3 misses. Done ([38-release-and-region](38-release-and-region/README.md)):
   the deep pages' misses were prefetches the core drops. A second
   request for each struct page is kept, "SUNRPC: Fetch a released page's
   struct page a second time": on arm64 a PSTL2KEEP two batches ahead,
   about 3.5k nfsd cycles a WRITE less with the rate, ring and peak
   unchanged; elsewhere prefetchw again 4 ahead. The pfn range test for
   the region count was not kept.
8. **Round 39: the receive side's struct page misses** (Mike, 28
   October): `svc_tcp_rx_region_span` and
   `svc_tcp_rx_skb_region_pages_after`, whether
   `svc_tcp_rx_prefetch_pages()`'s prefetches are dropped as the
   release's were, from the tip counted and profiled again
   ([39-receive-prefetch](39-receive-prefetch/README.md)). The tip
   counted at 297.1k nfsd cycles a WRITE; the receive side's prefetches
   are not dropped (without them nfsd costs about 10k cycles a WRITE more);
   a lead of one skb was not kept (Mike).
9. **Round 40: the wake path** (Mike, 28 October): about 4 threads woken
   and 11 context switches a WRITE, and `finish_task_switch` about a
   tenth of nfsd's samples outside idle: where the wakes come from, what
   each lands on, and changes judged on latency at iodepth 1 as well as
   cycles ([40-wake-path](40-wake-path/README.md)). Done, nothing
   changed: woken threads land warm (97% on their last CPU), the wake is
   fast untraced, and the wakes woken for nothing (about 1%) and the
   socket lock's sleeps (under 0.5%) are too small to change.
10. **Round 41: the rings' GRO** (Mike, 28 October): about 20% of the
   rings' busy cycles, about 28k a WRITE, and 44% of their L3 misses:
   the shape of the aggregates, then where GRO's misses are, from a
   diagnostic xeu ([41-rings-gro](41-rings-gro/README.md)). GRO's lines
   are all in the cache (its misses are xeu's prefetches in flight), so it
   costs instructions. Kept: xeu hands GRO page fragments
   (`rx_gro_frags`, kmod-xsight 2.0.1-13), 6.5k ring instructions a WRITE
   fewer, the rings' cycles the same.
11. **Round 42: where the rings' stalls are** (Mike, 29 October): 42% of
   the ring CPUs' cycles are backend stalls, only 18% of them on memory;
   counted by group first, then SPE for the instructions and data
   ([42-ring-stalls](42-ring-stalls/README.md)). Closed: the stalls are
   not in the driver's poll; a refill prefetch of the struct page saved
   nothing and was not kept; 9.89 receive interrupts a WRITE, 1.82 of
   them stale and 2.93 lost to NAPI already polling.
12. **Round 43: the receive region's size** (Mike, 29 October;
   [43-region-size](43-region-size/README.md)). Copy mode carries 40.8
   GB/s at every size down to 32 MiB with about 1200 pages in use; loan
   mode, its budget fitted to the region, loses 1.6% at 100 MiB, 6.4% at
   64 MiB and 32% at 32 MiB, where even a budget of 5 WRITEs filled it.
13. **Round 44: NAPI's deferral and the rings' DRAM reads** (Mike, 29
   October; [44-defer-and-dram](44-defer-and-dram/README.md)).
   `napi_defer_hard_irqs` 2 with `gro_flush_timeout` 10 us takes the
   receive interrupts from 10.7 to 1.25 a WRITE and the rings 10.0k
   cycles a WRITE (7.1%) lower, at the same rate and peak: **kept** on
   port a (settled.sh `DEFER`). 30 us stalled the port for seconds. The
   rings' DRAM reads are the RX CPUs': `struct page`'s `dma_addr` in the
   refill, the completion, the buffer's `skb_shared_info` and header
   lines past the first.
14. **Round 45: what holds the region's pages** (Mike, 29 October;
   [45-region-holders](45-region-holders/README.md)). A loaned record
   holds about 64 pages, not the 126 the budget charges; the rings hold
   about 500 posted and their pools about 500 idle on their stacks in
   every mode, so a small region fills with fixed holders, not loans.
   A stack of 64 takes a third off copy mode's working set for nothing,
   but costs loan mode 3 to 3.5%, most of it the pool's DMA-index
   xarray on each page's round trip through the region. Step 4,
   `0c375e06bf64` (a region page kept mapped for its device, on a free
   list per device), ran on build 504 (30 October): the xarray is gone
   from the round trip, B against A costs loan mode +0.15k ring and
   +2.75k nfsd cycles a WRITE (0.9%, was 3 to 3.5%) for 432 fewer pages
   in use, copy mode 379 fewer for nothing, the rate the same; A itself
   came in 1.6k and 4.3k under build 499's. `settled.sh` holds the stack
   at 64 since 30 October (Mike). Step 5, `a4fb143d7852` (a bulk's
   overflow returned under one hold of the region's lock), ran on build
   505 (31 October): 1.34 holds a WRITE, not 14, and at one kernel and
   boot stack 64 costs nfsd 291.4k cycles a WRITE against stack 256's
   291.7k, the rate the same, for 270 fewer pages in use. **Kept**
   (Mike). Not run on tardis1, where two ports share the lock.
15. **Round 46: a fresh profile of the tip** (Mike, 31 October;
   [46-tip-profile](46-tip-profile/README.md)). Build 505, nfsd 94.1 and
   the rings 33.3 us a WRITE busy. The profile is flat, no one thing on
   nfsd over about 3% read by caller. Taking the record is 28.7 us, the
   write 26.3, the reply 14.2; the region's lock is 1.13. The write
   below nfsd, 19.1 us through XFS, iomap, the block layer and NVMe, is
   the largest path no round has taken up. Nothing changed.
16. **Round 47: the write below nfsd** (Mike, 31 October;
   [47-write-path](47-write-path/README.md)). Counted: 0.75 XFS
   transactions a WRITE for timestamps, four requests of 256 KiB, four
   NVMe interrupts, 13 us on nfsd's CPUs that perf cannot see.
   `lazytime` **kept** (settled.sh): nfsd 292.0k to 287.8k cycles a WRITE
   (-1.5%), the rate and peak the same. `nomerges` 2 not kept: one doorbell
   a WRITE, the same four interrupts. NVMe interrupt coalescing not kept:
   nfsd -6.7k cycles a WRITE at saturation, but at iodepth 1 p50 +137 us
   at 1 MiB and +280 us at 64 KiB.
17. **Round 48: a profile with pseudo-NMI** (Mike, 31 October;
   [48-pseudo-nmi](48-pseudo-nmi/README.md)). A look: pseudo-NMI adds
   nfsd +5.0k cycles a WRITE (+1.7%, one run a side). Hard interrupts on
   nfsd's CPUs are 27.8k cycles a WRITE (NVMe 11.4, IPIs 6.5, entry 7.7),
   96% on an idle CPU, so earlier profiles dropped them with the idle
   stacks. Sleeping and waking is the largest single thing, about 37.7k
   (13%), with `sched_balance_newidle` 3.81k unranked before. Nothing
   changed; the parameter is still on the command line.
18. **Round 49: nfsd on fewer CPUs** (Mike, 31 October;
   [49-fewer-nfsd-cpus](49-fewer-nfsd-cpus/README.md)). 512 threads on 48,
   16 and 8 CPUs, on the pseudo-NMI boot. Counted nfsd cycles 292.9k to
   266.5k and 246.5k a WRITE, but most of that is idle: the threads' own
   work falls 3.7% and 6.3%, instructions 0.8% and 1.8%. The rate and
   latency are the same, the region not raised on 16. The interrupts are
   not cheaper: 4.08 a WRITE and 27.8k to 26.6k cycles, moved from idle
   CPUs onto threads. Repeated without pseudo-NMI: the same result, 288.3k
   to 261.5k and 242.4k, instructions -0.8% and -1.7%, all nine runs
   counting, and no region change that stands above the scatter. Decided
   (Mike, 1 November): a non-starter, the server has to carry much more
   load than one client. `NFSD_CPUSET` stays 8-55.
19. **Round 50: the 1 MiB load on build 506** (Mike, 2 November;
   [50-build-506](50-build-506/README.md)). 40.48 GB/s, the rings 128.2k
   and nfsd 290.2k cycles a WRITE, instructions level with build 505, no
   regression. The next target: parking and restoring a loaned record
   trades pages by `sk_datalen` though its body is in the region, about
   2.3 us a WRITE, 2.4% of nfsd, all in sunrpc.
20. **Round 51: the page trade of a parked loaned record** (Mike, 2
   November; [51-loan-page-trade](51-loan-page-trade/README.md)). Kept,
   `7ca95cca4c1a`: a loaned record with no copied bytes trades one page.
   A/B, 4 runs a side: nfsd -3.3k cycles and -3.3k instructions a WRITE,
   -1.1%, no overlap; the rings, rate, region and latency the same. About
   1 us of the 2.3 us round 50 expected: `svc_fill_pages` did not fall,
   since what it allocates now is the reply's page, 1.2 a WRITE, none for
   the arguments. Next is round 50's second target, the repeated bvec
   walks.
21. **Round 52: the repeated walks of a bvec table** (Mike, 2 November;
   [52-bvec-walks](52-bvec-walks/README.md)). Kept, `e3c7a5faa02f` and
   `1eac016ea851`: a table checked at publish is not walked again, and a
   payload that reaches the end of the pages is neither counted nor
   stepped through. It needed no new kernel after all: a bit in
   `xdr_buf.flags` changes no layout. A/B, 4 runs a side: nfsd -5.9k
   cycles (-2.1%) and -21.7k instructions (-7.3%) a WRITE, no overlap;
   the rings, rate and region the same; p50 at iodepth 1 0.449 against
   0.434 ms, inside the scatter, to be read again. Payloads with and
   without a pad verified by crc32c through NFSv3 and NFSv4.1.
22. **Round 53: the table walk at release** (Mike, 2 November;
   [53-release-walk](53-release-walk/README.md)). Kept, `3ddd62ce0809`:
   a published record's entries were checked at publish, so the release
   checks only its counts and bitmaps. A/B, 4 runs a side: nfsd -3.0k
   instructions (-1.1%), and -1.9k cycles (-0.7%) without one run's
   one-second dip, no overlap; the rings, rate and region the same.
   Latency in both orders found that B's higher p50 here and in round
   52 was the order of the runs.
23. **Round 54: `fh_verify`, the trace** (Mike, 2 November;
   [54-fh-verify](54-fh-verify/README.md)). No code yet. 16 handles, each
   on all 48 CPUs, repeat at chance (6%) by thread or CPU, and the path
   runs at 0.2 to 0.6 IPC with up to 15 times nfsd's refill rate: the cost
   is memory, so a cache of handles is not worth its lifetime problems.
   Then `exp_find()`'s reference on the expkey: every way out of its fast
   path was checked on the rig, but it saved 0.13 points of nfsd, about
   0.11 us, against 0.35 estimated, and the A/B cannot see it. Dropped
   (Mike), in `patches/dropped/` as 0007 to 0009. Latency stopped at 3 of
   8 runs for tardis2's shutdown for four SGL drives and the second port.
24. **Round 55: the walk per piece in the NVMe SGL write path** (plan,
   2 November; [55-nvme-sgl-walk](55-nvme-sgl-walk/README.md)). About 5 us
   a WRITE, 5.9% of nfsd, at IPC 3 to 5: the kind of target that paid in
   round 52. After a new baseline on tardis2's added drives and second
   port, starting from a trace. 1 to 2 us at a guess. It likely needs a
   new kernel. Waits (Mike, 3 November) after round 56.
25. **Round 56: eight SGL drives on two ports** (Mike, 3 November;
   [56-eight-sgl-two-ports](56-eight-sgl-two-ports/README.md)). The sweeps
   that settled tardis2's two ports at 12 connections a port, 512 KiB and
   rings of 128.
26. **Round 57: the ACKs nfsd's reads send, at the two-port setting**
   ([57-two-port-acks](57-two-port-acks/README.md)). The new baseline
   (67.42 GB/s, nfsd 273.1k cycles a WRITE, 49 MiB in use), its profile and
   page life; about six ACKs a WRITE, 9.1 us of nfsd. The trace: each ACK
   is a pass and a window update the client waits for, no pass waits for
   credit; no sunrpc change. The next targets need a kernel.

## Where the kept rounds stand (2026-10-28)

What a loaned WRITE costs after each kept round, from that round's B
runs (the profile where the A/B cannot resolve it):

| after | rings us/W | nfsd us/W | what was kept |
|---|---|---|---|
| 01 base | 92.6 | 184.1 (profile) | |
| 01 | 78.7 | | podman's leftover conntrack table deleted, port 2049 untracked (settled.sh) |
| 02 | | refs_valid 7.05 to 0.90 | `49f0fc4564ec`: the hand-off checks only counts, not the whole record |
| 03 | 68.0 | 159.8 | `skb_defer_max` 0: no deferred skb frees (settled.sh) |
| 05 | 65.0 | 156.4 | `eff5f16d4f30`, `0f0ed4159a12`: a cheaper `svc_data_ready` |
| 08 | 65.1 | 154.2 | `zc_notrack` only while a table registers conntrack (settled.sh) |
| 10b | 61.2 | 127.5 | `c0995ea94798`, `dd4fef2000a0`: no wake for a partial loaned record, held back at most half the open window |
| 13 | | 118.3 | `916c11a2d1fc`: each nfsd thread reuses its last credentials |
| 14 | about 59 | 114.9 | `02a445664e23`: `prefetchw()` of the loaned pages' `struct page` |
| 17 | 58.2 | 113.2 | `5fde85ef4076`: a thread reads on when a loaned record's rest arrives during its read |
| 12 | | 111.5 to 108.1 on build 499 | `drc_max_entries` 4096: a reply cache small enough to stay hot (settled.sh) |
| 20 | 59.95 to 49.98 on build 499 | | xeu `irq_msix_mask` Y (kmod-xsight 2.0.1-9): a ring's interrupt held off at the card, not through the ITS (settled.sh) |
| 22 | | 108.3 to 105.1 on build 499 | `5ea50b07c428`, `5c004783a05d`: a loaned frag inherits the skb's page-pool reference, two atomics a frame fewer on each page's count |
| 23 | | 106.1 to 104.8 on build 499 | `3e95efcd28c0`: a page's neighbouring loaned buffers returned with one read of its count, not an atomic |
| 24 | | 104.8 to 101.6 on build 499 | `57f883cc3294`: the release prefetches each `struct page` a batch ahead, so its misses overlap |
| 25 | 51.1 to 48.8 on build 499 | 103.1 to 97.8 on build 499 | `svc_tcp_rx_want_eighths` 6 (`2a75926c1458`, settled.sh): an incomplete loaned record taken up at three quarters of the open window, one ACK a WRITE fewer |
| 26 | | about 0.5 by the profile (A/B 97.2 to 96.5, inside the drift) | `6c6dc284fed0`: an nfsd thread its pool could not lose sleeps without the 5 s timer |
| 28 | 50.9 to 48.5 on build 499's own sunrpc | | xeu `rx_prefetch` Y (kmod-xsight 2.0.1-10, settled.sh): the next received frame's headers, shared info and `struct page` prefetched while a frame is built |
| 30 | 47.6 to 44.6 on the tip's sunrpc | 100.5 to 91.7 on the tip's sunrpc | `rodata=noalias` on the kernel's command line (settled.sh): the linear map in 32 MiB blocks, page-table walks 108 to 2 a WRITE on nfsd's CPUs and 61 to 0.3 on the rings |
| 34 | 44.55 to 42.95, inside the spread (−0.17 without a pair put off) | | xeu (`DC23D10E104D370F14FB06E`, on 2.0.1-10): `rx_buf_size`'s buffers cut by the ring from whole pages counted once, no frag drain atomic |
| 35 | 140.6k to 137.9k cycles a WRITE, counted (−1.9%; tick-sampled µs could not see it) | the same | xeu (`63631721F8B149913C0DC19`, 2.0.1-12): the refill's lean loop, its state in registers, each descriptor two eight-byte stores |
| 36 | the same | 307.4k to 304.6k instructions a WRITE, counted; cycles 295.1k to 294.6k, inside the spread (the borrow 0.86k cheaper, the time to idle) | `eae183aaaff3`: the borrow's page-pool test from one load, the append's coalesce and region tests with their cheap conditions first |
| 38 | | about 3.5k cycles a WRITE less | `93b10c1877fc`: a released page's struct page fetched a second time (PSTL2KEEP two batches ahead on arm64) |
| 41 | 6.5k instructions a WRITE fewer, cycles the same | | xeu `rx_gro_frags` Y (kmod-xsight 2.0.1-13, settled.sh): GRO given page fragments, no skb a frame |
| 44 | 140.2k to 130.2k cycles a WRITE (−7.1%) | the same | NAPI deferral 2/10000 on port a (settled.sh `DEFER`): receive interrupts 10.7 to 1.25 a WRITE |

- **Since round 01:** the rings 92.6 to about 58 us a WRITE (-37%),
  and to about 50 on build 499 with the interrupt held off at the card
  (-46%); nfsd 184 to about 113 (-39%), to about 108 on build 499
  with the smaller reply cache (-41%), to about 105 with round 22's
  reference takeover and round 23's paired release (-43%), to about
  102 with round 24's prefetch in the release (-45%), and to about 98
  with round 25's later wake (-47%), with round 26's untimed idle
  sleep about 0.5 less; the rings to about 49 (-47%), and to about 46.5
  with round 28's prefetch in xeu (-50%). With round 30's `rodata=noalias`
  on the tip's sunrpc, the rings are at 44.6 (-52%) and nfsd at 91.7
  (-50%).
- The rate stayed at the drives' 40.5 GB/s throughout. Latency at
  iodepth 1 is the same or a little better.
- Each round's A/B had its own A, and A drifts a couple of us between
  sessions. The totals hold to about 2 us.
- Rounds 04, 12 and 15 ran on build 499 on 25 October (`run33.sh`), and
  04 and 12 together in round 19: `drc_max_entries` 4096 is kept (nfsd
  -3.4 us a WRITE); `rx_region_stack` 1024 is not, as it holds 940 more
  of the region's pages (Mike); the `rx_region` move saves about 0.1.

## Why CPU a WRITE, not GB/s

On one port the four drives under it (`/hs/nvme4`-`7`) hold every run at
40.0 to 40.4 GB/s, DRIVE-LIMITED, so no change to the CPU path moves the
rate. What a change can move is what a WRITE costs the CPUs that carry it:

- **the rings**: port a's ring CPUs (0, 2, 4, 6),
  which run at 87-90% softirq at 40 GB/s. They are the next limit once the
  drives stop being it: faster drives, or two ports on faster drives.
- **nfsd**: CPUs 8-55, about 14% busy, where a WRITE's threads sleep and
  wake about 8 times.

Every figure is a cost per 1 MiB WRITE: ring softirq microseconds a WRITE
(`4 x soft% / WRITEs a second`, from the run's `srv-mpstat.txt` and
`window.txt`; `xeu-rig/cost.sh RUNDIR...` gives it and nfsd's busy
microseconds a WRITE), a frame's microseconds a WRITE from the profile (`top-*.txt`),
and nfsd's wakes and voluntary context switches a WRITE (`pool_stats` and
`/proc/<pid>/status` read around the run).

**Those microseconds are the 100 Hz tick's samples, and read low (round
35).** This kernel has no `CONFIG_IRQ_TIME_ACCOUNTING`, so `/proc/stat`'s
busy time, and mpstat's from it, is what the tick happened to land on:
under load the ring CPUs' counted cycles say 67 to 74% busy where the
tick says 40 to 63%, about 43 µs a WRITE on the rings where the cycles
say about 70, and the figure moves 2 to 4 µs between runs. From round
35 `twoport.sh` counts every CPU's cycles and instructions over its
window and `cost.sh` gives them a WRITE beside the µs; rounds are judged
on the counted cycles (the cores run at 2.0 GHz). The µs columns of the
earlier rounds stand as they were measured, comparable with each other.

**How a profile becomes microseconds.** The cycle counter does not stop in
an idle CPU on this machine (CPU 1, 95% idle, has as many samples as CPU 0
at 90% softirq), so neither samples nor cycles are busy time. `flame.sh`
weights each stack by its cycles, leaves out the idle ones (`do_idle` with
no interrupt on it), and turns a frame's share of what is left into
microseconds a WRITE with mpstat's busy time (100 - idle - iowait) for the
same CPUs over the run's window. On the rings that gives 92.0 us a WRITE,
against 91.9 of softirq alone from the same run.

## Two yardsticks: CPU a WRITE, and latency (Mike, 2026-10-23: "latency is going to matter a lot on a per IO level")

- **CPU a WRITE at saturation:** the one-port loan64 run, as above.
- **Latency at low depth:** `xeu-rig/lat.sh OUTDIR TAG`. It runs 4 writers, one a drive, at iodepth 1 with 1 MiB writes, so the drives are far from saturation and what is measured is the server's own time an I/O.
  - The untraced run gives fio's completion p50, p90, p99, p99.9 and mean.
  - A run with `lat.bt` gives the server's split: the wait for a thread (`svc_xprt_dequeue`'s wakeup and qtime), `svc_process`, the write into the file system, and the reply (`svc_tcp_sendto`).
- **Why they differ:** at saturation, latency is the drives' queue (Little's law), so CPU savings cannot show in it.
  - A saving on the critical path is both a CPU saving and a latency saving.
  - Work moved off the critical path, or reordered, changes latency alone. For example, the loan is given back before the reply is sent, so every reply waits for it.
  - Waiting (for a thread, for the rest of a record, for credit) costs latency and no CPU.
- **Each round reports both yardsticks.**

## The method

1. **Profile the one-port loan64 run as it stands, untraced except for perf.**
   `xeu-rig/flame.sh OUTDIR TAG [WRITES]` runs the battery's run (port a, the
   four drives, `WARM=10 RUNTIME=30`, which keeps it inside the drives' fast
   region, after a trim and 120 s of rest) and records every CPU for 8 s at
   999 Hz with call chains inside twoport.sh's 10 s window. It draws the
   flamegraphs with Brendan Gregg's FlameGraph (installed in `/opt/FlameGraph`
   from github.com/brendangregg/FlameGraph, commit `41fee1f`; `flamegraph.pl`
   needs `perl-open`): one for the ring CPUs (0, 2, 4, 6), one for nfsd's
   (8-55), one for all, each with its `.folded` stacks (`difffolded.pl`
   compares two rounds), and ranks each group's frames, self and with what
   they call (`top-rings.txt`, `top-nfsd.txt`). `flame.sh report DIR` makes
   the reports again from a round's `perf.data`.
2. **Rank by share of each group's busy cycles**, as microseconds a WRITE
   (`top-*.txt`). Read the self cost (where the
   cycles are spent) and the inclusive cost (what a function and what it calls
   take) together: a wide frame low in the graph is a path, a wide frame at the
   top is a function to make cheaper or call less often.
3. **One change at a time, then an interleaved A/B**, as for the one-wake
   fix: at least three runs each way, A and B alternating, every run audited by
   `settled.sh` and its `drives.txt` read (no drive stepped down). Fit ring
   (and nfsd) microseconds a WRITE against the refusals a loaned WRITE, with one
   slope for both and a term for the change: refusals vary from 0.2 to 0.96
   between runs of the same setting, and that alone moves ring cost by as much
   as most single gains. A change is kept if its term is beyond its standard
   error; each kept change is its own code commit at the tip (one group to a
   commit, an upstream-style message, Signed-off-by only).
4. **Profile again after each kept change**: the ranking moves.

A sunrpc or nfsd change is tested by reloading the module
(`xeu-rig/swap-sunrpc.sh`), which drops every NFS client's connections,
tardis1's included: that is Mike's to ask for, each time.

## Rules a round keeps

- Enterprise-storage mode: the drives `write through`, the export `sync`.
- `./settled.sh apply` ends in "the settled setting" before a run, and
  `flame.sh` refuses to run off it. Never get past a BAD item by hand.
- The client is shared with tardis1: `client_free` first (flame.sh does).
- The profile costs the rings a little of their rate (perf at 999 Hz on 64
  CPUs): take a round's run figures from untraced runs, and its shares from
  the profile.
- Without pseudo-NMI (not on tardis2's command line but for round 48), a
  sample taken with interrupts off lands where they are turned on again:
  a wide `_raw_spin_unlock_irqrestore` or `handle_softirqs` self cost
  belongs to what ran before it. Read its callers before ranking it. A
  hard interrupt of the idle task lands on an idle stack and `flame.sh`
  drops it, so no profile without pseudo-NMI counts those at all: 27.8k
  cycles a WRITE on nfsd's CPUs (round 48).
- A change of a few us a WRITE on nfsd is below what mpstat resolves over
  48 CPUs: compare the function's cost in the profiles before and after,
  and use the A/B to show nothing else moved (round 02).
- Annotate a module against the build that was profiled (`perf
  buildid-list -i perf.data`); a later reload leaves a different
  `sunrpc.ko` in `updates/`. The kernel's own text is in `/proc/kcore`
  (no vmlinux with symbols on tardis2).
- With the kernel's debuginfo installed perf expands inlined frames from
  the line tables, and the ranking comes out wrong (round 46): `flame.sh`
  passes `--no-inline`; do the same by hand.
- A function under two parents is counted once: read its callers before
  adding it to a ranking (round 01 counted `napi_consume_skb` under
  `skb_defer_free_flush` as a second item).
- A driver's counters are not wire frames until shown to be: count with
  TCP's own (`nstat TcpInSegs TcpOutSegs`).
- Per-run counters of the port (interrupts, page_pool) start again when
  the port opens: read them after the run, not as a difference.
- A change that holds pages longer or returns them later also reports
  the region's peak pages in use (Mike: keep the L3 hot; a buffer's
  reuse is LIFO, so returning it later makes the ring reuse colder ones).
- **Do not grow the region's footprint (Mike, 25 October: "we cannot
  afford to increase the region").** A change that raises the region's
  peak pages in use is not kept, whatever it saves; round 04's deeper
  stack (rings -3 us a WRITE for 940 more pages) is the case. AGENTS.md
  has the rule.
- Name the change by what it saves a WRITE, measured, and say what it did
  not change (the rate, nfsd's sleeps).

## A round's directory

`NN-name/README.md`: what was profiled (kernel, sunrpc srcversion, the run's
figures), the ranking, what was picked and why, the change (its commit), the
A/B and its fit, and the outcome. The flamegraphs (`rings.svg`, `nfsd.svg`)
and the `self-*.txt` and `incl-*.txt` reports are kept beside it; the raw
`perf.data` and the run directories stay in `/var/tmp/zc-run-logs/`.
