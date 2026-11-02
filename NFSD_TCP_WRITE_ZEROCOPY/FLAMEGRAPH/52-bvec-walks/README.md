# Round 52: the repeated walks of a bvec table

**The target (rounds 50, 51):** after `svc_tcp_rx_validate_mixed()` has checked a record's table at publish, one NFSv3 WRITE of 1 MiB walks it again in full, and in fl51 that costs:

| where | % of nfsd | what it does |
|---|---|---|
| `xdr_buf_validate_page_mode` | 1.23 | four walks: `xdr_init_decode`, `xdr_stream_subsegment`, `read_bytes_from_xdr_buf` for the reply cache, `xdr_buf_to_bvec` |
| `xdr_stream_subsegment` self | 1.03 | `xdr_advance_authoritative` steps to the payload's end |
| `xdr_set_next_authoritative_buffer` | 0.36 | a call for each of those steps |
| `xdr_buf_subsegment` | 0.32 | `xdr_buf_bvec_subsegment` counts the payload's entries |

About 2.9% of nfsd at most. Not all of it can go: the walk that is left, the copy into `rq_bvec`, will take cache misses the walks before it used to take.

**Round 50 said this needs a new kernel. It does not.** That assumed a new field. `xdr_buf.flags` has bits free, so nothing changes in the structure's layout: `build-sunrpc.sh` finds 328 exports, 0 with another version, 0 missing, and nfsd is untouched. It goes by module swap on build 506.

## Why skipping the walk is safe

`svc_tcp_rx_validate_mixed()` checks more than the walk does (`bv_offset` against `PAGE_SIZE` and not past it, the sum against `body_bytes`). After that nothing can change the buffer:

- `svc_tcp_rx_publish()` and `svc_tcp_rx_clear_xdr()` are the only places a real buffer enters or leaves `XDRBUF_PAGE_BVECS`.
- Every function in `xdr.c` that changes a buffer refuses this mode, and `xdr_bvec_mutation_rejection_test` compares the buffer byte for byte after calling them all.
- The other writers of `page_len` work on `rq_res`, or on `rq_arg` in array mode.

A buffer without the flag gets the full walk, so `xdr_bvec_malformed_matrix_test` means what it did. The buffer's own checks (mode, `page_base`, lengths, a table present) take no walk and stay.

## The changes

- `e3c7a5faa02f`, "SUNRPC: Check a received record's bvec table only once": `XDRBUF_BVECS_CHECKED`, set at publish, cleared at release, handed to a segment.
- `1eac016ea851`, "SUNRPC: Do not step to the end of a bvec-backed buffer's pages": a segment reaching the end of the pages takes `bvec_count - index`, and the stream goes straight to the tail.
- `a85c18d5b456`: the tests.

Two things the shortcuts could have broken, both caught before a run and both pinned in `xdr_bvec_end_of_pages_test` and `xdr_bvec_advance_test`:

- **`buf` and `subbuf` may be one buffer.** `subbuf->page_len` is written before the table is looked at, so `buf->page_len` read there is the segment's length, and any segment from the pages' start would look like it reaches their end and claim the whole table.
- **An empty tail may have no address.** `xdr_inline_decode(xdr, 0)` returns `xdr->p`, so landing on a tail with a NULL base would make a zero-length decode at the end of a stream an overflow. With an empty tail the stream stays on its fragment, at `xdr->end`.

**Not for NFSv4:** a compound's GETATTR after the WRITE follows the payload in the pages, so the count and the advance still walk. The validations go there too. The rig runs NFSv3.

## Verified writes: a mistake to set right

KUnit on B: 10/10, 54/54, 7/7, 10/10. Seven mutants, each failing the test written for it.

`run132.sh`, `run133.sh` and `run134.sh` count for nothing but the NFSv4.1 job of run132. They wrote to `/mnt/rxr-a0`, and `twoport.sh` mounts from `rxr-a1`: that path is a directory on the client's own XFS. So the "NFSv3 verified" of run132 was the client's disk, the odd `O_DIRECT` `EINVAL` was XFS's alignment, and the file that went missing was `twoport.sh`'s `rm` of `rxr-a[0-9]*/f-*`. What showed it: `padded.bt` found no WRITE but the load's own 1 MiB. `run135.sh` is the same check on the mount, refusing a path that is not NFS.

On the mount (`run135.sh`, on B, a light loan64 run, `verify.txt`, `padded.txt`), crc32c-verified `O_DIRECT` from the client, each read back, all nine rc 0:

| job | length | decoded in bvec mode, with the flag |
|---|---|---|
| NFSv3 | 1048572 | 256 |
| NFSv3 | 1048573, pad 3 | 256, and 256 from NFSv4.1 |
| NFSv3 | 1048574, pad 2 | 256 |
| NFSv3 | 1048575, pad 1 | 256 |
| NFSv3 | 524289 | 511 |
| NFSv3 | 262147 | 1023 |
| NFSv3, NFSv4.1 | 1048576 | among the load's own |

So a pad reaches the end-of-pages path on real loaned records, and NFSv4.1's payload, which does not end the record, takes the walk, and both come back whole.

## The A/B (run136)

Build 506 with sunrpc swapped before each run, A `167B8BE080CE8001F6D7E4A`, round 51's kept module, B `5D39507E93127A6010C9B8E`, in the order A B B A B A A B, one port, 256 threads, `$L/t2-r136/`, all 8 no-step, no-stall, placed, alloc_fail 0, no kernel warning. The script kept round 51's tag, so the runs are named `r130-N-A` and `r130-N-B`.

| run | GB/s | rings cycles (instr) | nfsd cycles (instr) | peak | mean in use |
|---|---|---|---|---|---|
| 1 A | 40.54 | 127.8k (282.8k) | 287.6k (295.3k) | 6161 | 5005 |
| 4 A | 40.50 | 127.9k (282.7k) | 285.8k (296.0k) | 6008 | 4927 |
| 6 A, the dip | 39.51 | 124.5k (281.1k) | 281.2k (296.9k) | 6136 | 5243 |
| 7 A | 40.47 | 128.2k (283.2k) | 287.4k (297.2k) | 6189 | 5020 |
| **A mean** | **40.50** | **128.0k (282.9k)** | **286.9k (296.1k)** | **6124** | **4984** |
| 2 B | 40.55 | 127.9k (282.6k) | 282.3k (274.6k) | 6175 | 4985 |
| 3 B | 40.54 | 127.7k (282.8k) | 281.4k (273.9k) | 6086 | 5024 |
| 5 B | 40.53 | 128.1k (283.2k) | 280.0k (274.6k) | 6140 | 4997 |
| 8 B | 40.48 | 127.6k (282.7k) | 279.7k (274.3k) | 6100 | 4995 |
| **B mean** | **40.52** | **127.9k (282.8k)** | **281.0k (274.4k)** | **6125** | **5000** |

A's means leave run 6 out of the rate, cycles and mean in use, and keep its instructions and peak.

**nfsd -5.9k cycles a WRITE (-2.1%) and -21.7k instructions (-7.3%), and no run of B reaches a run of A in either,** run 6 aside for cycles (with it, -4.5k, and its 281.2k lies inside B's). Nothing else moves: rings, rate, NVMe interrupts 4.08, IPIs 4.8. The region is not raised: peak +1 page, mean in use +16, against a scatter of about 150.

**Run 6's dip.** The wire fell from about 41 to about 36 GB/s for a stretch late in the run and came back. No drive stepped down or stalled, TCP's drop counters stayed 0, and the client retransmitted 4. Its cycles fell with its rate and its instructions match its side. It looks like the dips of round 49's runs 4 and 5. The cause was not found, here or there.

**Latency at iodepth 1, A B B A:**

| run | GB/s | p50 | p99 | mean, ms | max |
|---|---|---|---|---|---|
| 1 A | 8.01 | 0.440 | 0.586 | 0.452 | 1.125 |
| 2 B | 7.94 | 0.444 | 0.578 | 0.456 | 4.986 |
| 3 B | 8.00 | 0.453 | 0.578 | 0.457 | 1.057 |
| 4 A | 8.35 | 0.428 | 0.545 | 0.433 | 0.883 |

B sits at about the top of A here, p50 0.449 against 0.434, inside the 0.420 to 0.457 that rounds 49 to 51 show for one build. Two runs a side cannot say more, and the change takes work out of the path and adds no wait. Run 2's 4.99 ms maximum did not recur. The next latency runs should read this again.

## The profile (fl52, on B, against fl51)

| % of nfsd, self | fl51 | fl52 |
|---|---|---|
| `xdr_buf_validate_page_mode` | 1.23 | 0.04 |
| `xdr_stream_subsegment` | 1.03 | 0.07 |
| `xdr_set_next_authoritative_buffer` | 0.36 | 0.01 |
| `xdr_buf_subsegment` | 0.32 | 0.13 |
| `xdr_init_decode` | 0.08 | 0.02 |
| together | 3.02 | 0.27 |
| `xdr_buf_to_bvec`, the walk that is left | 0.62 | 0.61 |

**The target's frames fell 2.75 points of nfsd and nothing took their place:** no frame rose by more than 0.16, and those that rose are spread through the path as anyone's share does when the whole shrinks. `xdr_buf_to_bvec` did not take the misses, as this README expected it might.

**Why instructions fell 7.3% and cycles 2.1%.** The walks were tight loops: 21.7k instructions in the 7.9k cycles those frames held, an IPC near 2.7, against about 1 for nfsd as a whole. So they were cheap instructions, and 2.75% of the cycles is 7.3% of the instructions. The A/B's 5.9k is below the profile's 7.9k; where the difference went was not looked for.

The saving is larger than round 51's 3.3k, from a target round 50 ranked second, and this time the estimate held: about 2.9% expected, 2.75 points came out by the profile, 2.1% by the A/B.

**Kept:** `e3c7a5faa02f` and `1eac016ea851`, with `a85c18d5b456` and its follow-up. `updates/sunrpc.ko` is B.

Copy was not run.
