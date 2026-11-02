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

## The runs, to do

The sunrpc builds, `build-sunrpc.sh`'s way: A round 51's kept module `167B8BE080CE8001F6D7E4A`, B with these. KUnit on B, then mutants, then 4 runs a side, A B B A B A A B, then `lat.sh` A B B A, then a profile on B. `$L/t2-r132/`.
