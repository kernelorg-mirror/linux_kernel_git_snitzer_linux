# Round 55: the walk per piece in the NVMe SGL write path (the plan)

**Nothing here has been run.** This is the plan written on 2 November before tardis2 went down for four more SGL drives and its second port (Mike).

**The target:** about 5 us a WRITE, 5.9% of nfsd, stepping one piece at a time through a table of about 128 pieces to split the bio and to map it for DMA. On these drives (`max_segments` 65, `max_sectors_kb` 256, `dma_alignment` 3) a 1 MiB WRITE is about four 256 KiB requests.

## Why this one

From fl54 (`/var/tmp/zc-run-logs/t2-r142/fl54-flame`), with IPC and refills from round 54's `perf record` (`54-fh-verify/pmu.txt` is that run, `t2-r139/pmu.script` has every symbol):

| frame | us a WRITE | % of nfsd | IPC | L2 refills a thousand instructions |
|---|---|---|---|---|
| `blk_rq_dma_map_iter_next`, inclusive | 1.64 | 1.87 | 5.24 | 1.2 |
| `blk_map_iter_next` | 1.62 | 1.85 | 4.23 | 1.2 |
| `bio_split_io_at` | 0.84 | 0.96 | 3.18 | 2.6 |
| `dma_map_phys`, under SGL setup | 0.25 | 0.29 | 3.95 | 1.7 |
| `nvme_pci_setup_data_sgl`, self | 0.31 | 0.36 | 4.83 | 1.3 |
| the rest of `nvme_map_data` | 0.87 | 1.0 | about 1 | about 21 |

(The last row's PMU figures are rough: `nvme_map_data` itself reads 1.12 and 21.0, and `blk_rq_dma_map_iter_start`, most of the row, was not counted.)

- **It is work, not misses.** IPC 3 to 5 with almost no refills. Round 52's walks were like that and saved 2.1% of nfsd. Round 54's handle path ran at 0.2 to 0.6 and its fix saved a third of the estimate, because the misses stayed.
- **It is the largest group left.** `nfsd_direct_write` is 18.7 us, 21% of nfsd. Sleep and wake (rounds 17, 18, 40) and the transmit lock (rounds 7, 31, 32) have been worked through, and `page_pool_put_netmem_bulk` by rounds 23, 24, 38 and 45.
- **It is what SGL drives run on,** and any O_DIRECT user with fragmented buffers takes this path, so a fix would matter beyond this series.

This corrects the ranking after round 53, which set "the block layer's 128 pieces" aside as following from the 8 KiB buffers and the 256 KiB limit. The number of pieces does follow from them. What it costs each piece was not measured then.

## How it is to go

1. **Bring-up.** `bringup.sh` (kdump first), then `settled.sh`, which will need the new drives and the second port. Any change to the setting is made there.
2. **A new baseline.** Four loan runs, latency and a profile. Figures before the new hardware are not comparable. Note which CPUs nfsd and the rings run on if the port's layout changes.
3. **Profile again.** Does this path grow or shrink with more drives? If the ranking changes, this plan changes with it.
4. **Trace before code,** as round 54 did. Pieces per request and per bio. Whether neighbouring pieces are ever contiguous: they probably are not, since headers sit between payloads in the 8 KiB buffers, but it is unmeasured. Which walks repeat what an earlier one worked out, starting with `bio_split_io_at` and the DMA iterators, and whether `nr_phys_segments` or a segment boundary could carry that over. Also where `dma_direct_map_phys` spends its time.
5. **A change only if the trace shows a repeat to remove.** The table must still be walked once to write the SGL, so only the repeats and the overhead of each step can go.

**Estimate: 1 to 2 us, 1 to 2% of nfsd, and only a guess.** Round 54's was three times too high.

**What it costs to test:** `block/` is built in, so a change there needs a new kernel and only `nvme.ko` can be swapped. Its reviewers are block and NVMe, not Chuck Lever. Do not grow the region's footprint, and A/B latency in both orders, as rounds 52 to 54 showed it has to be.

**The fallback:** `svc_tcp_rx_append_ref` and `svc_tcp_rx_borrow_segment`, about 4.1 us together at IPC 3 to 4, and a `sunrpc` swap to test. Round 27's prefetch did not help, and about a third of the second one's cost is on `struct page`. Nobody has tried trimming the per-fragment checks, which may repeat what the skb already guarantees.
