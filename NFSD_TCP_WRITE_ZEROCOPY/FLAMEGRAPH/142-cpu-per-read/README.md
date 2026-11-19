# Round 142: the client's CPU a READ, profiled to pick the next target

Mike, 19 November: "profile an IOPS pass to pick the next CPU-per-READ
target".  Rounds 140 and 141 showed that only less client CPU a READ
raises 4 KiB IOPS (the client's CPUs 85 to 88% busy, 11.8 to 11.9 us a
READ).  The client on its `updates/` stack (round 139's code; sunrpc
`2355B960B6A9951899AF6B4`), the server at the settled setting.

`prof-both137.sh 1154 96`: one 4 KiB random READ run at iodepth 96 and,
from its eighth second, 8 s of `perf record -a -g` at 999 Hz on both
hosts (the client's in its `/dev/shm/zc-cli137-1154.data`, kept).  The
client's samples are then grouped by what a READ pays for, each group
in us a READ (its share of the samples x 48 CPUs x busy / IOPS).

## The profile (t2-r1154)

3.53 M IOPS (1.73 / 1.80 M), p99 3.33 / 3.36 ms, the client 88% busy
(busiest 93%), the server 88% (busiest 99%), no new kernel warning.
The client spends 11.97 us a READ (88% x 48 / 3.53 M), so 1% of its
samples is 0.12 us a READ.

Where a READ's client CPU goes, inclusive (`cli-children.txt`):

| Stage | Share | us a READ |
|---|---|---|
| rpciod running the task (`rpc_async_schedule`) | 40.5% | 4.86 |
| of it, the send (`xs_tcp_send_request`) | 11.0% | 1.32 |
| of it, the completion (`rpc_exit_task`, NFS's done) | 5.0% | 0.60 |
| of it, `xprt_request_enqueue_receive` | 4.1% | 0.49 |
| fio's submission (`io_uring_enter`, `nfs_file_direct_read`) | 16.4% | 1.97 |
| the receive worker (`xs_stream_data_receive`, the copy out) | 13.0% | 1.56 |
| the NIC's softirq (`net_rx_action`) | 8.9% | 1.07 |
| idle | 12.2% | |

Spin locks are 12.7% of the samples (1.52 us), by caller (`perf script
--no-inline` with `lockcallers.awk`): `release_sock` 1.63% (the socket
lock, the sender against the receive worker), `xprt_alloc_slot` 1.47%
(the slot table), `sch_direct_xmit` 0.83% (the qdisc),
`lock_timer_base` 0.63%, `rpc_count_iostats_metrics` 0.53%,
`try_to_wake_up` 0.43%, `nfs_direct_read_completion` 0.43%, the
transport's `queue_lock` (enqueue, transmit, the reply's lookup)
about 1.6% in all.

### The candidates, by us a READ

| Target | Share | us a READ | What |
|---|---|---|---|
| **the receive queue's rb-tree** | **3.5%** | **0.42** | the insert's walk in `xprt_request_enqueue_receive` (2.05%: its samples are at +0x124 and +0x12f, the loads of a node's child and of its `rq_xid` at -0x30), `xprt_lookup_rqst` 0.68%, `rb_insert_color` 0.41%, `rb_erase` 0.36% |
| the socket lock, send against receive | 2.7% | 0.32 | `release_sock`'s lock 1.63%, `lock_sock_nested` 0.93% and its slowpath |
| the slot table's lock | 1.5% | 0.18 | `xprt_alloc_slot` |
| timers | 0.8% | 0.10 | `lock_timer_base` 0.63% and `__mod_timer`: the task's timer, and `timer_delete_sync()` of the autodisconnect timer on every enqueue |
| NFS's per-READ setup | 6.5% | 0.78 | `nfs_fattr_init` 1.76%, `nfs_pgheader_init` 1.31%, `rpcauth_refreshcred` 1.28%, `rpc_prepare_reply_pages` 1.12%, `nfs_refresh_inode_locked` 1.05%: spread over five functions |

**The receive queue's tree is the clearest target.**  About 4,400 READs
are in flight (3.5 M a second x 1.26 ms) over 24 transports, some 185 a
transport, so each insert walks about eight nodes, and each node is a
`rpc_rqst` last written by another CPU: a miss each.  A transport's
xids are handed out in order (`xprt->xid++`), so a table indexed by the
xid's low bits finds a request in one bucket and one request, the
insert and the reply's lookup alike.
