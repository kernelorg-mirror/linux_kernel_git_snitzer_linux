# Round 50: the 1 MiB load on build 506

**The ask (Mike, 2 November):** run `settled.sh`, do a proper 1 MiB performance test, and commit what the best issue to fix next is.

tardis2, build 506, sunrpc `5CFC092E63911EC723D2A63`, nfsd `3F0C341E86E266D5FDF2ADF`, xeu `CA0931345E46C929B11D01F`, no pseudo-NMI, no KUnit suite loaded. `run128.sh` runs four untraced loan runs, two of `lat.sh`, one profile; logs in `$L/t2-r128/`. Copy is not run.

**This boot's drives:** only seven NVMe devices are on PCI, so two SN8100s are gone (they went before the boot of the crash in `TESTING.md`, and no PCI rescan was tried). The four DPRP5s are `nvme2` to `nvme5`, mounted at `/hs/nvme4` to `7` as every script expects. `data_drives` was fixed for it.

## Saturation (run128)

Four untraced runs at the settled setting: 1 MiB WRITEs, 64 WRITEs of budget, port a onto the four DPRP5s, `WARM=10 RUNTIME=30`. Every run counts: no drive stepped down, no stall, every flow placed, `alloc_fail` 0.

| run | GB/s | ring cycles (instructions) a WRITE | nfsd cycles (instructions) a WRITE | peak | mean in use | completion, ms mean, p99 |
|---|---|---|---|---|---|---|
| 1 | 40.52 | 127.7k (282.2k) | 289.4k (299.5k) | 6224 | 4994 | 51.45, 82.31 |
| 2 | 40.44 | 128.3k (282.8k) | 289.8k (300.4k) | 6012 | 4734 | 51.56, 81.26 |
| 3 | 40.50 | 128.6k (283.3k) | 290.5k (300.0k) | 6083 | 4983 | 51.47, 82.31 |
| 4 | 40.47 | 128.3k (283.0k) | 291.0k (300.4k) | 5983 | 4841 | 51.56, 82.31 |
| mean | 40.48 | 128.2k (282.9k) | 290.2k (300.1k) | | 4888 | |

**No regression from build 505.** Round 49's untraced A, same load and no pseudo-NMI: 40.47 to 40.52 GB/s, rings 128.1k (283.1k), nfsd 288.3k (300.8k), peak 6059 to 6172, mean in use 4927. Instructions are level, so nfsd's +1.9k cycles (0.7%) is not more work. The hunt runs on this build earlier the same day, on another boot, were 288.3k to 289.4k. The rate is the drives' limit: four drives each writing about 10.1 GB/s, 99% busy.

A WRITE: 4.08 NVMe interrupts, 4.8 IPIs, 11.5 context switches, 4.0 threads woken. Loans: 1.54M admitted against 1.56M to 1.59M denied in the window, about one refusal a WRITE, which paces the loans and is not to be pursued (round 26, Mike).

## iodepth 1 (lat.sh, 1 MiB)

| run | GB/s | p50 | p90 | p99 | mean, ms |
|---|---|---|---|---|---|
| l1, untraced | 7.72 | 0.453 | 0.537 | 0.594 | 0.469 |
| l2, untraced | 8.24 | 0.436 | 0.481 | 0.553 | 0.443 |

Round 49's A on 505: p50 0.432 and 0.420, p99 0.578 and 0.553, 8.04 and 8.28 GB/s. These two sit at the top of that scatter, with round 49's own 0.457 above them, and two runs cannot say more. A later round should look again if it stays there.

## The profile (fl50)

nfsd 94.4 and the rings 33.7 us a WRITE busy, as round 46's 94.1 and 33.3: it is still flat. Read by path on nfsd: `svc_tcp_recvfrom` 30.0, of which the socket read 21.8; `nfsd3_proc_write` 23.8; `svc_send` 14.6, of which the release 6.9.

The large self frames, by caller, and why none is next:

| frame | us a WRITE | what it is | why not |
|---|---|---|---|
| `_raw_spin_unlock_irqrestore` | 4.81 | `wake_up_process` 1.95, `page_pool_recycle_ring_bulk` 1.09, NVMe SGL 0.47 | the wake is rounds 17 and 18, not kept on latency; page_pool is in the kernel |
| `finish_task_switch` | 3.52 | sleeping | round 48's 13%, the largest thing there is, and no one fix; fewer CPUs is a non-starter (round 49) |
| `_raw_spin_lock` | 2.37 | `__dev_xmit_skb` 1.07 | rounds 07, 31, 32 |
| `blk_map_iter_next`, DMA | 3.4 | the block layer's 256 KiB requests | in the kernel, and round 47 |

## What to fix next

**The page trade for a record that has nothing to trade: about 2.3 us a WRITE, 2.4% of nfsd, all in sunrpc, so no new kernel.**

A record taken up in several passes is parked and restored on each. `svc_tcp_save_pages()` and `svc_tcp_restore_pages()` size that by `sk_datalen`, as if the record's bytes were in `rq_pages`, as they are in copy mode, so a loaned 1 MiB record's later passes walk up to 64 slots of `rq_pages` and `sk_pages`, twice in the restore, and `rq_pages_nfree = npages` makes the next `svc_alloc_arg()` refill them.

| frame | us a WRITE |
|---|---|
| `svc_tcp_restore_pages` | 1.11 |
| `svc_tcp_save_pages` | 0.37 |
| `svc_fill_pages`, from `svc_recv` | 0.82 (it also refills the reply's pages) |

A loaned record's body is in the region. What it has in `rq_pages` is its fixed bytes, in the first page, and any arena copy, at `rq_pages[body_offset >> PAGE_SHIFT]`. So the trade has to keep those pages and can leave the rest. That is to be shown, not assumed: how many a record really has decides the saving, and a sliver late in the record could leave a high-water mark no better than `sk_datalen`.

**Second, not picked:** the checks of the same bvec table. `xdr_buf_validate_page_mode()` walks the whole table on each of `svc_process`'s decode set-up, `xdr_stream_subsegment`, `read_bytes_from_xdr_buf` for the reply cache's checksum, and `nfsd_vfs_write`'s `xdr_buf_to_bvec`, four walks, 1.15 us a WRITE, after `svc_tcp_rx_validate_mixed()` checked it at publish. 47% of its cycles are the loop's compare, so the loads miss. `xdr_stream_subsegment` is another 1.00 us self, stepping to the end of the payload one bvec at a time. A flag on `xdr_buf` would end the walks, but it changes a structure every sunrpc user shares, which needs a new kernel, and the malformed-buffer cases in `xdr_kunit` rely on these functions refusing.

**Not next:** the refusal (round 26), the wake (rounds 17, 18), the transmit lock (rounds 07, 31, 32), fewer CPUs (round 49).

Copy was not run.
