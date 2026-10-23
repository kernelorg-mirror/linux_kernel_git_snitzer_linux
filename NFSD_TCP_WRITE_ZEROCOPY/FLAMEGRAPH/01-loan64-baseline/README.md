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

**Both were done (Mike: "do both, why not do both?").** The table is deleted while no container runs, so its hooks go, and `xeu-rig/zc-notrack.nft` keeps port 2049 untracked whenever a container brings the table back.

## Outcome: kept. The rings 15% cheaper a loaned WRITE, copy mode 7%

The A/B is `run18.sh`, in `/var/tmp/zc-run-logs/t2-tipkernel/`.
- A is the host as it was: the netavark table, 10 conntrack hooks.
- B is the table deleted and `zc_notrack` loaded: no conntrack hook.
- Each run's `-netfilter.txt` records which it was.
- Untracked loan runs alternated, then a copy-mode pair.
- Every run was settled and DRIVE-LIMITED, with no drive stepped down.

| run | GB/s | ring softirq | ring µs a WRITE | nfsd busy | refusals a loaned WRITE |
|---|---|---|---|---|---|
| nf1-loan64-A | 40.32 | 88.3% | 91.9 | 14.2% | 0.57 |
| nf2-loan64-B | 40.45 | 76.7% | 79.5 | 13.3% | 0.93 |
| nf3-loan64-A | 40.32 | 90.0% | 93.8 | 14.8% | 0.46 |
| nf4-loan64-B | 40.43 | 75.6% | 78.4 | 13.6% | 0.89 |
| nf5-loan64-A | 40.32 | 88.1% | 92.0 | 14.4% | 0.59 |
| nf6-loan64-B | 40.39 | 75.3% | 78.2 | 13.3% | 0.93 |
| nf7-copy-A | 40.81 | 66.0% | 67.9 | 28.4% | - |
| nf8-copy-B | 40.81 | 61.6% | 63.4 | 25.5% | - |

**Loan.**
- The rings went from 92.6 to 78.7 µs a WRITE (-13.9 µs, -15%), with no overlap between the two sets.
- nfsd went from 14.5% to 13.4% busy, about 13.7 µs a WRITE over its 48 CPUs.
- The rings saved more than the 9.0 µs the profile put under `nf_hook_slow`. The rest is hooks it did not count (`nf_confirm`, defrag) and conntrack's cache footprint.
- No fit was needed: every B run had more refusals than every A run (0.89-0.93 against 0.46-0.59). Refusals raise the rings' cost, so the fit could only make the gain larger.
- Why B refuses more is not known. A guess: rings that empty sooner put more records on the connections at once, against the same 64-WRITE budget.

**Copy mode** (one pair, run before the target was made loan mode alone): the rings went from 67.9 to 63.4 µs a WRITE (-4.5 µs, -7%), and nfsd from 28.4% to 25.5% busy.

**The setting.** It is in `xeu-rig/settled.sh`:
- apply loads `zc-notrack.nft` and deletes the leftover table while `podman ps -q` is empty;
- the audit marks either one BAD;
- both directions were checked on tardis2.

tardis1 takes it at its next `./settled.sh apply`.

**What this A/B did not separate:** with the table gone there was no conntrack to skip. So `notrack`'s own worth, with a container's table present, is not measured.

## The next rounds (Mike)

- **02:** `svc_tcp_rx_refs_valid` checks only the counts in `svc_tcp_rx_exchange`. That needs a sunrpc reload.
- **03:** `net.core.skb_defer_max` 0 against 128, for item 3 and page_pool's put on the ring, on the loan run alone.
- **04:** measure how often a region-bound pool's ptr_ring is full when a page comes back. Only if that justifies it, add a knob for its size, which needs a kernel build and a boot.
