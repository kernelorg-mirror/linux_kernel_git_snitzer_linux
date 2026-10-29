# Round 45: what holds the receive region's pages

**The ask (Mike, 29 October):** round 43's run109 left the 32 MiB region full with the loan budget at 720 pages, so something besides the loaned WRITEs holds region pages under loans; count what does.

**What there is from before:**

- **Round 43 (run104, run109).** Copy mode's peak is 1168 to 1248 pages at every region size. Loan mode's is 6306 at 512 MiB with a budget of 8198, 4779 at 100 MiB, 3436 at 64 MiB, and all 2048 at 32 MiB with a budget of 1016 or 720. At 720 the rate fell to 21.84 GB/s, lower than 1016's 27.67, with only 6 failed allocations. The sum of copy mode's peak and the budget, about 1890 pages at 720, does not explain a full region.
- **What the region counts.** `/sys/kernel/debug/page_pool_rx_region` gives the free pages, the lowest that reached, and the failed allocations. A page is in use from when a pool takes it until it is given back to the region, whoever has it.
- **Where pages can be, then:**
  1. **Posted on a ring,** waiting for a frame. xeu cuts its 8 KiB buffers two to a 16 KiB page; each ring has 256 entries, so about 128 pages a ring, 512 for port a's four.
  2. **Kept by a ring's page pool for reuse.** Each pool has an alloc cache of up to 32 pages at 16 KiB (`PP_ALLOC_CACHE_SIZE`), and a region stack of the pool's size, 256, since `settled.sh` holds `rx_region_stack` at 0. A page returned by nfsd goes onto its pool's stack, and back to the region only when the stack is full. So up to 4 x 288 = 1152 pages can sit idle in the pools and still count as in use. Round 04 is the same mechanism the other way: a stack of 1024 raised the peak by about 900 pages.
  3. **In the NFS sockets' receive queues,** out-of-order queues and backlogs: records arrived and not yet taken by nfsd, loaned or copied.
  4. **On loan,** held by admitted WRITEs until their write to the drive finishes. The budget charges each 126 pages.
  5. **Elsewhere, briefly:** skbs held by GRO, which now holds them up to 10 µs with round 44's deferral; skbs being received; denied WRITEs being copied.

  The sockets' spare pages (`svc_tcp_spare_bytes_max`) are the threads' own pages, not the region's, so they are not on the list.

## 1. The breakdown, sampled (r110)

[run110.sh](run110.sh), with a diagnostic xeu ([xeu-diag-region-held.patch](xeu-diag-region-held.patch) on round 41's kept xeu, `7CFF720BA3CE35894DD922E`). It adds a read-only parameter, `region_held`, giving for each receive ring the buffers posted to the card, what is left of the page being cut, and its page pool's alloc cache and stack. These are read without the ring's NAPI, so each is a snapshot that may be a poll old.

From 10 s into each timed load, every 0.2 s for 10 s, one sample of:

- the region's free pages;
- the rings' posted buffers, and the pools' cache and stack (`region_held`);
- the NFS sockets' receive and backlog bytes (`ss -tm`: `r` and `bl`);
- the budget's charged credit, `budget - avail - reserve`, and the records in flight (`svc_tcp_rx_budget_stats`).

In pages: posted buffers count two to a page, and the sockets' bytes 16 KiB to a page (each 8 KiB buffer is charged as its truesize, so this is near, not exact). What the region has in use beyond the four is "the rest": GRO and skbs in flight, and whatever the loans hold beyond their charge.

Four runs at the settled setting (round 44's deferral included), counted and untraced, `WARM=10 RUNTIME=30`, after five samples at rest:

| run | region | mode |
|---|---|---|
| r110-1 | 512 MiB | copy |
| r110-2 | 512 MiB | loan64 (budget 8198) |
| r110-3 | 64 MiB | loan, 23 WRITEs (budget 3032), as run104 |
| r110-4 | 32 MiB | loan, budget 720, as run109 |

Then the region at 512 MiB again, the kept xeu and the settled setting.

**What it decides:** if the pools hold several hundred pages under loans and few in copy mode, the region's footprint can be cut at the pools, through `rx_region_stack` (a smaller stack gives pages back to the region sooner, at the cost of mapping them again) or the pool's size. That would be step 2, an A/B judged on the peak, the ring's cycles and the rate. If the queues hold them, the receive buffer under loans is the place to look; if the rest does, a diagnostic sunrpc that counts each loaned record's region pages.
