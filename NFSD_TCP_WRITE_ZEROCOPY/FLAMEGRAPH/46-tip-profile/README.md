# Round 46: a fresh profile of the tip on build 505

**The ask (Mike, 31 October):** a fresh profile, to see what it puts at the top of the CPU cost, after round 45's per-device mapping, stack 64, deferral and bulk return.

**The run.** `xeu-rig/flame.sh`, one-port loan64, tardis2, build 505 at the settled setting (sunrpc `666175BED7A343E376C1742`): 40.55 GB/s, no drive stepped down, no stall, peak 6151 pages. Reports and flamegraphs are here, `perf.data` in `$L/t2-fl46/fl46-flame/`. The profile raises the cycles (nfsd 296.3k a WRITE, against run117's 291.4k), so the shares are from it and the run figures from run117. Microseconds a WRITE are `flame.sh`'s, a frame's share of mpstat's busy time; `cost.sh`'s tick-sampled microseconds read low (round 35), so read them as shares.

**Debuginfo changes `perf script`.** With `kernel-16k-debuginfo` installed, perf expands inlined frames, and the first report was wrong: `get_current` 10.4% self, `__preempt_count_dec_and_test` 58% with callees, and `xfs_fsb_to_gbno` and `trace_xfs_rmap_find_right_neighbor_result` in the write path. `flame.sh` now passes `--no-inline`, which gives function frames comparable with every earlier round. These reports are that run's, rebuilt from the same `perf.data`. The inlined form is not kept.

## The ranking

**nfsd, 94.1 us a WRITE busy, by path, with callees:**

| path | us a WRITE | share |
|---|---|---|
| `svc_tcp_recvfrom`: taking the record | 28.7 | 30% |
| of it `tcp_read_sock` | 20.9 | 22% |
| of it `svc_tcp_release_ctxt`, nearly all the ACK from `tcp_cleanup_rbuf` | 6.8 | 7% |
| `nfsd3_proc_write`: the write | 26.3 | 28% |
| of it `xfs_file_write_iter` | 19.1 | 20% |
| of it `submit_bio` | 9.4 | 10% |
| `svc_send`: the reply | 14.2 | 15% |

Across those, `__tcp_transmit_skb` is 8.5 us and `dma_map_phys` with `dma_direct_map_phys` 1.7.

**nfsd's frames by self cost, read by caller** (the rule for a sample taken with interrupts off):

| frame | us a WRITE | what it is |
|---|---|---|
| `_raw_spin_unlock_irqrestore` | 4.83 | 1.91 thread wakes (`svc_pool_wake_idle_thread`), 1.13 the region's lock (`page_pool_recycle_ring_bulk`), 0.46 `nvme_pci_setup_data_sgl`, 0.20 and 0.09 XFS's log |
| `finish_task_switch` | 3.47 | 2.97 coming back from `schedule_timeout`, an nfsd thread's sleep |
| `_raw_spin_lock` | 2.27 | 1.12 `__dev_xmit_skb`, the transmit queue's lock |
| `svc_tcp_rx_append_ref` | 2.14 | |
| `svc_tcp_rx_borrow_segment` | 1.87 | |
| `blk_map_iter_next` | 1.60 | |
| `svc_tcp_rx_release_refs` | 1.41 | |
| `svc_tcp_rx_prefetch_pages` | 1.35 | |
| `napi_pp_put_page` | 1.29 | |

**The rings, 33.3 us a WRITE busy:** `gro_receive_skb` with callees 19.6 (59%), which holds `gro_complete` 10.5 and TCP's receive under it (`tcp_v4_rcv` 7.6); by self, xeu 4.51 (13.5%), `skb_gro_receive` 2.11, `handle_softirqs` 2.03, `inet_gro_receive` 1.62, `_raw_spin_unlock_irqrestore` 1.58 (0.60 thread wakes, 0.59 `napi_complete_done`).

## What it shows

- **The profile is flat.** Nothing on nfsd is over about 5% by self cost, and once read by caller no one thing is over about 3%.
- **The region's lock is not at the top:** 1.13 us of nfsd's 94.1, after round 45's step 5.
- **The wake and the sleep are nfsd's largest single item:** 1.91 us in wakes and 2.97 in coming back from the sleep. Round 40 looked at them and changed nothing: threads land warm, and the wakes for nothing are about 1%.
- **The largest path no round has taken up is the write itself:** no round's README names iomap, the block layer or the NVMe driver, and they are 19.1 us on nfsd, 20%. Within it, `blk_mq_flush_plug_list` is 5.95 and the NVMe driver 5.80; `xfs_direct_write_iomap_begin` is 1.01 and building the bios 1.07.

Nothing was changed in this round. What it takes up next is Mike's.
