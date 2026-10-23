# Round 01: the one-port loan64 run, with the one-wake fix

**Profiled 2026-10-23 on tardis2**, build 494 (`7.3.0-7.hs.494.loanpage_pool.el10.aarch64+16k`).
- sunrpc: the tip's, with "SUNRPC: Wake an idle service thread only once" (`2fc9cd2e839b`, srcversion `82ACAC76B5F85A2645B58A4`).
- The settled setting; port a; the four drives `/hs/nvme4`-`7`; 64 WRITEs of budget.
- Run with `xeu-rig/flame.sh /var/tmp/zc-run-logs/t2-tipkernel fl1-loan64`, with perf 7.3.0-7 and FlameGraph `41fee1f`.
- The reports were made again with `flame.sh report` once the tools were final.

**The run: 40.34 GB/s, DRIVE-LIMITED, no drive stepped down.**

| | fl1-loan64 (profiled) | fx-loan64-2 (untraced) |
|---|---|---|
| ring softirq | 88.2% | 87.8% |
| ring µs a WRITE | 91.9 | 91.1 |
| wakes a WRITE | 6.38 | 6.44 |
| refusals a loaned WRITE | 0.63 | 0.62 |

So perf at 999 Hz cost the rings under 1 µs a WRITE.

**Busy time a WRITE** (`cost.txt`):
- the rings (CPUs 0, 2, 4, 6): **92.0 µs**;
- nfsd (CPUs 8-55): **184.1 µs**, iowait left out.

The flamegraphs are [rings.svg](rings.svg) and [nfsd.svg](nfsd.svg). The full rankings are [top-rings.txt](top-rings.txt) and [top-nfsd.txt](top-nfsd.txt).

## Where the rings' 92 µs go

The rings are the next limit, so they are the ones ranked. Most of the time is spent in receive:
- `xeu_rx_poll`: 76.4 µs, of which GRO and the stack above it (`gro_receive_skb`) take 46.3;
- of that, IP and TCP delivery (`netif_receive_skb_list_internal`): 36.4.

What can be named and changed, most costly first:

| # | what | µs a WRITE | % | whose | note |
|---|---|---|---|---|---|
| 1 | page_pool's put and get of the region's pages | 13.8 | 15.0 | the loan (page_pool region) | `napi_pp_put_page` 7.35 self, `page_pool_alloc_frag` 6.48 with what it calls; 126 pages a WRITE |
| 2 | netfilter: conntrack on every packet | 9.0 | 9.8 | the host | podman's `netavark` NAT table (below) |
| 3 | deferred skb frees flushed on the ring | 9.1 | 9.9 | the stack | `skb_defer_free_flush`: skbs nfsd consumed come back to their ring CPU to be freed (`net.core.skb_defer_max=128`, settled) |
| 4 | transmit completions | 7.9 | 8.6 | the stack, the driver | `napi_consume_skb` under `xeu_tx_poll`: the replies and ACKs |
| 5 | `svc_data_ready` | 7.7 | 8.4 | sunrpc | `svc_xprt_enqueue` 5.5, of which `svc_pool_wake_idle_thread` 3.3 |
| 6 | `eth_type_trans` | 6.8 | 7.3 | the stack | the first read of each frame's header, so a cache miss on what the card wrote |
| 7 | `enable_irq` | 4.8 | 5.2 | the xeu driver | the driver re-arms its interrupt after each poll through the irq core: the descriptor lock (`mtree_load`, `__irq_get_desc_lock`) and the GIC |

**A caution on the samples.** Without pseudo-NMI, a sample taken while interrupts are off lands where they are turned back on.
- So `_raw_spin_unlock_irqrestore` shows 6.46 µs a WRITE of self time on the rings: 71% of it under `enable_irq` and 26% under `wake_up_process`. That time belongs to the code that ran with interrupts off before it.
- The kernel has `CONFIG_ARM64_PSEUDO_NMI=y`. With `irqchip.gicv3_pseudo_nmi=1` on the command line, perf's interrupt would be an NMI and those samples would land where they were taken. That is a boot parameter, so it is Mike's to decide.

## nfsd's 184 µs

nfsd has room, so its ranking is for later rounds:
- `svc_tcp_recvfrom`: 80.9 µs, the receive and the loan's page work;
- `nfsd_vfs_write`, the write to XFS and NVMe: 27.0 µs;
- `svc_tcp_sendto`: 21.8 µs;
- netfilter, conntrack again on the replies: 9.4 µs, 5.1%;
- `svc_tcp_rx_refs_valid`: 7.05 µs. It walks every slot of a record's 126 to check each borrowed page, and runs twice a WRITE, from `svc_tcp_rx_exchange` and `svc_tcp_rx_release_refs`. Those checks guard invariants the KUnit suites test.
- sleeping and waking: `schedule` 5.8 µs, `svc_pool_wake_idle_thread` 6.7 µs.

## What was picked: conntrack (item 2)

**Why this one first:**
- One change removes all of it.
- It costs both the rings (9.0 µs a WRITE) and nfsd (9.4 µs).
- It needs no module reload, so no client loses its connections.

**What it is:**
- `nft list hooks` shows `ipv4_conntrack_in` and `ipv4_conntrack_local` on prerouting and output, and `nf_confirm` on input and postrouting. They are there because the `inet netavark` table has NAT chains, and NAT needs conntrack.
- tardis2 has no container: `podman ps -a` is empty and `podman.service` is disabled. The table is left over from podman's default network, and conntrack follows every NFS connection for nothing (`nf_conntrack_count` 31).

**The change to test:**
- either delete `table inet netavark`, so no table needs conntrack and its hooks go away;
- or keep the table and add a raw-priority `notrack` rule for port 2049.

The first takes all of the cost. The second keeps a hook on each packet, but leaves podman alone.

**Not done yet.** The auto-mode classifier refused the change to tardis2's firewall, and changing it is Mike's to decide. The table is saved as `/var/tmp/zc-run-logs/t2-tipkernel/netavark-table.nft` so it can be put back with `nft -f`. Once it is decided:
1. run an A/B, table present against table gone, three runs each, plus one copy-mode pair;
2. if the A/B keeps it, make the change in `settled.sh`.

Whether tardis1 has the same table is not yet known: its host key refused a login from tardis2.

## Outcome

Pending the A/B.
