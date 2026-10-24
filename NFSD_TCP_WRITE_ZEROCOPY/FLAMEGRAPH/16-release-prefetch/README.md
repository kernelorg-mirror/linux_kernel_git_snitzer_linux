# Round 16: prefetch a loaned record's struct pages before releasing them

**The change:** "SUNRPC: Prefetch a loaned record's struct pages before releasing them" (`e5e84a05b686`).
- **The target:** nfsd's largest self frame in round 14's profile, `page_pool_put_netmem_bulk` at 5.5 µs a WRITE, all from `svc_tcp_rx_release_refs()` returning a loan's pages before the reply.
  - `perf annotate`: 85% of it is the first loop's load of each page's `compound_head`, about 68 ns a page.
  - The pool drops each reference with a fully ordered atomic, so one page's miss cannot overlap the next.
- **The change itself:** `svc_tcp_rx_release_refs()` first `prefetchw()`s the `struct page` of each borrowed page, once a page.

**Runs 2026-10-24 on tardis2**, build 494, the settled setting. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run34.sh`.
- A is round 14's sunrpc (`FBEBC68D6628C0CE72E3E00`), B round 16's (`A424C5B9D6A36B7BE512166`).
- nfsd is round 13's on both. KUnit on B: 7, 10, 52 and 7 pass.

## Outcome: not kept. The release is cheaper, the WRITE is not

| run | GB/s | ring µs a WRITE | nfsd µs a WRITE |
|---|---|---|---|
| r16-1-loan64-A | 40.53 | 60.2 | 113.5 |
| r16-2-loan64-B | 40.62 | 59.6 | 114.8 |
| r16-3-loan64-A | 40.60 | 57.8 | 115.0 |
| r16-4-loan64-B | **25.37** | 61.4 | 113.3 |
| r16-5-loan64-A | 40.49 | 59.5 | 114.0 |
| r16-6-loan64-B | 40.49 | 58.3 | 113.2 |

- **nfsd:** 114.2 against 113.8 µs a WRITE, inside the runs' scatter. **The rings:** 59.2 against 59.8.
- **The profile** (fl14 against fl16):
  - `page_pool_put_netmem_bulk` self fell from 5.49 to 3.88 µs, and the release as a whole from 10.2 to 8.7.
  - No other frame took the 1.6 µs up: the largest rise is 0.2 (`napi_pp_put_page`). Both profiles' nfsd totals are the same (118.1 and 118.4).
- **Latency** ([latency.txt](latency.txt)): p90 0.553 and 0.528 ms for B against 0.537 and 0.522 for A, so the reply comes no sooner.
- It is reverted at the tip (`a081854bc12c`). tardis2 stays on round 14's sunrpc with round 13's nfsd.

## A stall in r16-4 (B), cause not found

- r16-4's 10 s window was normal: 40.8 GB/s on the wire, 38,637 WRITEs a second. But the run as a whole averaged 25.37 GB/s.
  - One WRITE took 14.5 s, and the drives wrote unevenly for the run's first 24 s.
  - The server retransmitted 294 segments in the run, where the others had 0-4. Those are its replies going unacknowledged by the client.
- **Another, smaller one:** r14-2 (round 14's B) had a 1.3 s WRITE, with 4 server retransmits and 308 delayed ACKs against about 50-200.
- **No other run since round 10 had a WRITE over 180 ms.** Both stalls were in runs with round 14's prefetch, but so were nine runs without one.
- **Why the receive path looks unlikely:** a server that stops reading does not retransmit its own replies. The retransmits point at the path to the client, or the client.
- **On the client:** a podman container started at 09:17:20, during r16-4's fio. Containers start there every few minutes all day, though, and none started near r14-2's stall.
- **To watch:** `max completion` over 1 s in any run, with the server's `TcpRetransSegs`.
