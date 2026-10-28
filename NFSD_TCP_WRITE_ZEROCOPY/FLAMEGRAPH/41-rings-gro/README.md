# Round 41: the rings' GRO

**The plan (Mike, 28 October).** What software GRO costs the ring CPUs for each loaned WRITE, where that cost goes, and what of it can be cut. The rings are the next limit once the drives stop being it (faster drives, or two ports). Since round 01 they have had only xeu's changes and `rodata=noalias`. GRO itself was last looked at in round 09, which tried BIG TCP. The starting point is round 39's state: build 499 with `rodata=noalias`, the tip's sunrpc (`DDE22984B5905541BB2CD35`), nfsd the kernel's, xeu round 35's (`63631721F8B149913C0DC19`), the settled setting, one loan64 run at a time on port a onto the four DPRP5s.

## Why here

In run89's profile (round 39), the ring CPUs' GRO frames:

| frame | rings' cycle samples | rings' L3 miss samples |
|---|---|---|
| `inet_gro_receive` | 3.54% | **9.77%** |
| `skb_gro_receive` | 2.88% | 4.74% |
| `tcp_gro_receive` | 2.37% | 7.38% |
| `gro_list_prepare` | 2.36% | 4.88% |
| `dev_gro_receive` | 2.34% | 3.74% |
| `tcp4_gro_receive` | 1.67% | 6.63% |
| `__gro_receive_network_flush` | 1.14% | 5.22% |
| `gro_receive_skb` | 0.86% | 1.30% |
| GRO in all | **17.2%** (20.5% of the busy samples) | **43.7%** |

- **Size.** At run89's 139.3k ring cycles a WRITE, GRO is about 28k a WRITE. A 1 MiB WRITE is about 135 frames (MTU 7792, 7740 bytes of payload each), so about 210 cycles a frame.
- **What kind of cost.** GRO holds 44% of the rings' L3 read misses for 17% of their samples. Its cost is mostly memory, not instructions.
- **Which lines it misses is not known.** Round 28's `rx_prefetch` already fetches the next frame's header line, its `struct page` and its shared info while the current frame is built. `eth_type_trans`, the first reader of the header line, holds only 2.8% of the misses. Yet `inet_gro_receive`, which reads the IP header in the same line, holds 9.8%. So the misses are on something else: the new skb itself (`NAPI_GRO_CB` in its `cb`), the held aggregate it is compared against and merged into, or the aggregate's shared info and `frag_list` as it grows.

**The shape of the traffic:**
- **Few flows a ring.** The client mounts with `nconnect=16`, 16 connections on port a, and `rss-place.py` places them 4 a ring. GRO holds at most 8 aggregates a NAPI context (`MAX_GRO_SKBS`) in 8 hash buckets. So no aggregate is pushed out by a newer flow, and `gro_list_prepare` compares a frame with at most a few held aggregates.
- **Bursts end the aggregates.** By round 09, an aggregate ends at 8–9 frames, about 62 KB, because the client's TSO bursts are at most 64 KB. A burst's last segment can carry PSH, which ends the aggregate.
- **No hardware help.** Software GRO only: the E1 reports `rx-gro-hw` and LRO off [fixed]. Its completion carries per-flow hardware GRO context state that xeu does not read (`PROJECT.md`, "Hardware GRO context state").

## The steps

1. **The shape, traced** (one loan64 run, not counted), on the ring CPUs:
   - frames a NAPI poll, and aggregates a poll;
   - frames merged a WRITE, and why each aggregate ended: PSH, the size limit, a frame that could not merge, or the poll's end flushing it;
   - frames a WRITE that start a new aggregate rather than merge into a held one;
   - the held aggregates `gro_list_prepare` walks for each frame;
   - `xeu-rig/grolen.bt` again, for aggregate lengths and segments.

   run89's counted 139.3k ring cycles a WRITE is the untraced yardstick.
2. **Where GRO's misses are**, with a diagnostic xeu, not the kernel (GRO is built in; xeu is a module and can be swapped without a reboot). Just before xeu hands a frame to `napi_gro_receive()`, it times the lines GRO's first touches load, with the 1 GHz counter as rounds 35 and 38 did:
   - the frame's header line;
   - the skb's lines, `cb` included;
   - the shared info.

   The floor is a line of the CPU's own. If these come in at the floor, GRO's misses are on the held aggregates, and it is the list and the merge that cost. If not, the prefetch's distance or kind is short, as round 38 found for the release.
3. **Then, by what 1 and 2 show, one change at a time, each an interleaved A/B on counted ring cycles:**
   - **A deeper or different prefetch in xeu** of what the new frame's GRO touches. This is xeu's, as round 28 was: a change for Xsight.
   - **Fewer, larger aggregates.** The client's TSO bursts (`gso_max_size`, `gso_ipv4_max_size` on its port) with round 09's server limit raised. That changes the client's setting, which is Mike's to decide.
   - **The held aggregate's lines,** if step 2 puts the misses there: what `skb_gro_receive` writes into the aggregate for each frame (its shared info, `frag_list`, the head's `len` and `truesize`).
   - **Hardware GRO** is a question for Xsight: whether the E1's per-flow GRO contexts can do the aggregation, which would remove most of this.

**Rules for the round, as for the others:**
- Loan mode only. A change is judged on the rings' counted cycles a WRITE, with nfsd's, the rate and the region's peak beside it. A change that raises the peak is not kept.
- An xeu swap reopens the port and drops the client's connections, so each one is Mike's to ask for. `settled.sh` puts the port's layout back after it.
- Copy mode gains from anything here too, but is not measured.
