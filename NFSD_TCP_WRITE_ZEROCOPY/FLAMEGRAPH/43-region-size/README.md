# Round 43: the receive region's size

**The ask (Mike, 29 October):** the results from running the region pool at different overall sizes, such as 32 MB, 64 MB and 100 MB.

**What there was before:** none at the current setting. The region has been 32768 pages (512 MiB at 16 KiB pages) since the bring-up. The other sizes in the record are from other boots and are not comparable: 2048 pages on the 64 KiB kernel (128 MiB, 7 October) and 6144 pages (96 MiB) on build 459 (8 October), in copy mode with other drives and builds. Loan64's peak in use is about 6350 pages (99 MiB, run102 and run103), and its budget is 8198 pages (128 MiB), so 32 and 64 MiB regions sit below what loan64 uses today.

## The sweep (r104, to run after run103)

[run104.sh](run104.sh): the region at 32768 pages (512 MiB), 6400 (100 MiB), 4096 (64 MiB) and 2048 (32 MiB), then 32768 again. At each size there is one copy run and one loan run whose budget fits the region with 1024 pages (16 MiB) left for the rings and the copy.

| region | pages | loan run | budget, pages |
|---|---|---|---|
| 512 MiB | 32768 | 64 WRITEs (loan64) | 8198 |
| 100 MiB | 6400 | 41 | 5300 |
| 64 MiB | 4096 | 23 | 3032 |
| 32 MiB | 2048 | 7 | 1016 |

All runs use round 41's kept xeu at the settled setting except for the region's size, and are counted and untraced (WARM=10 RUNTIME=30). Each line reports:

- the rate;
- ring and nfsd cycles a WRITE;
- the region's pages in use at most, against its size;
- allocations the region failed (the ring finding it empty);
- loans admitted and denied;
- the client's retransmits.

**The resize** follows page_pool's comment:

1. Write the new `rx_region_pages`.
2. Take port a down, so its pools go and the old region is given back.
3. Bring the port up, which makes the new region.

`settled.sh` holds the size to `EXP_REGION_PAGES` for the run, and now also checks the size of the region actually made (debugfs). Each resize:

- refuses to run with an NFS client connected;
- waits for the old region to go before the port comes up;
- stops if the new one is not made.

This is the first time the resize runs on the rig: "page_pool: Give the fixed receive region back when it is resized" was queued on 9 October when tardis1 crashed, and never run after. If 32768 contiguous pages cannot be had again at the end, the script stops at the size it could make, and a reboot puts the region back.
