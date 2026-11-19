# Round 144: the client's cache misses by cache line

Mike, 19 November: "yes, run perf c2c on the client as round 144".
Rounds 138, 141 and 143 each removed a function's share of the
client's profile and saved nothing, because the samples sat on loads
of lines (an `rpc_rqst`'s above all) last written by another CPU.
This round maps those misses by line instead of by function: which
lines take hits on another CPU's modified copy (HITM), at which
offsets, read and written where.  No code change, so no passes.

`c2c.sh 1163`: one 4 KiB random READ run at iodepth 96 on the client's
`updates/` stack (round 139's code), and from its eighth second 5 s of
`perf c2c record -a` on the client (`/dev/shm/zc-c2c-1163.data`,
kept); the report coalesced by instruction across threads.

## The misses (t2-r1163)

3.52 M IOPS (1.78 / 1.73 M), p99 3.39 / 3.59 ms, the client 88% busy,
no new kernel warning.  5 s of samples (`/dev/shm/zc-c2c-1163.data`):
527,882 loads, 46,551 of them hitting a line another CPU had modified
(HITM), 670 of those on the other socket; 27,661 lines shared.

**Two global lines carry almost all the cross-socket traffic** (533 of
the 670 remote HITMs), and at 850 to 2,200 cycles a load where the
other shared lines cost 210 to 260:

| Line | HITM (remote) | Who writes it | Who reads it |
|---|---|---|---|
| `nfs`'s .bss at `nfs_attr_generation_counter` | 765 (300) | `nfs_fattr_init()`, `atomic_long_inc_return()` on every READ | `nfs_refresh_inode_locked()` (the counter, 37%); `nfs_initiate_pgio()` (52%), reading `nfsiod_workqueue`, which shares the line: **false sharing** |
| `unix_auth`, the one `struct rpc_auth` of AUTH_UNIX | 617 (233) | `unx_validate()`, storing `au_verfsize`, `au_rslack` and `au_ralign` on every reply, the same values each time | `call_allocate()` (35%), `rpc_prepare_reply_pages()` (36%), `rpcauth_refreshcred()` (29%) |

The other HITMs are spread over thousands of lines, by function:

| Where | Share of HITM loads |
|---|---|
| the receive queue (`xprt_request_enqueue_receive` 6.8%, `rb_insert_color` 2.1%, `rb_erase` 0.9%, `xprt_lookup_rqst` 0.7%) | 10.5% |
| **the round-robin choice** (`xprt_switch_find_next_entry` 3.4%, `xprt_iter_next_entry_roundrobin` 2.6%, `xprt_iter_get_next` 1.6%): it walks the switch's list from its head to the last transport used, reading each transport's line, which every task's `rpc_task_set_transport()` and `rpc_task_release_client()` write (the transport's `kref` and `queuelen`) | 7.6% |
| `xprt_request_transmit` | 3.7% |
| list debug checks (`__list_del_entry_valid_or_report` 3.3%, `__list_add_valid_or_report` 1.3%) | 4.6% |
| `xs_tcp_send_request` 2.1%, `nfs_direct_read_completion` 2.0%, `memcpy` 1.8%, lock slowpath 1.8% | |

Round 143 already took on the receive queue and saved nothing; that
cost is the request moving from the CPU that sent it to the one that
receives its reply.

### The targets

1. **`unx_validate()` writes the shared `unix_auth` on every reply**,
   with values that do not change.  Storing only when a value differs
   leaves the line read-only, so its 617 HITMs, a third of them
   cross-socket at about 1,000 cycles each, go.
2. **`nfs_attr_generation_counter` shares its line with
   `nfsiod_workqueue` and `nfs_inode_cachep`**, read on every READ.
   Its own line (`____cacheline_aligned_in_smp`) ends the false
   sharing, about half of that line's HITMs; the counter's own
   increment stays.
3. **The round-robin walk**, 7.6% of the HITMs: a larger change (an
   array of the switch's transports and a cursor, so a choice reads one
   transport's line, not every one before it).
