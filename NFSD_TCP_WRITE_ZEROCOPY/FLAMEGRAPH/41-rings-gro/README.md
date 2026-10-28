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

## 1. The shape (r96)

[`xeu-rig/gro-shape.bt`](../../xeu-rig/gro-shape.bt) records, on the ring CPUs:
- **each frame's outcome** in `dev_gro_receive()`: merged, held as a new aggregate, or passed up alone;
- **what `gro_list_prepare()` walks:** the aggregates held in the frame's hash bucket, and in all eight;
- **each aggregate of more than one frame at `tcp_gro_complete()`:** its frames, whether a frame ended it (inside `dev_gro_receive()`) or the poll's flush did (`__gro_flush()`), and whether it carries PSH and is over 56 KB;
- **per call of `xeu_rx_poll()`:** frames, and the aggregates completed.

Aggregates the flush completes after `xeu_rx_poll()` returns are counted in the next poll's figure.

[run96.sh](run96.sh): one traced loan64 run at the current state, with no module swapped. From 10 s into the timed load it runs 8 s of `gro-shape.bt`, then about 8 s of round 09's `grolen.bt`.

**Run 28 October** (traced, so its rate is not a result): 36.46 GB/s, flows placed, no drive stepping down, counted ring 139.5k and nfsd 298.0k cycles a WRITE (run89's 139.3k and 297.1k), peak 6617, settled at the end ([run96.out](run96.out), [gro-shape-r96.txt](gro-shape-r96.txt), [grolen-r96.txt](grolen-r96.txt)).

**Each frame** (22.2 million in the 8 s):

| outcome in `dev_gro_receive()` | frames | share |
|---|---|---|
| merged into a held aggregate, its skb freed | 19,009,391 | **85.5%** |
| held as a new aggregate | 3,170,662 | 14.3% |
| passed up alone | 51,622 | 0.2% |

**What `gro_list_prepare()` walks:** the frame's bucket held one aggregate for 85% of frames, none for 14%, two for 0.3%. All eight buckets together held one for 82%, two for 4%, and never more than four. The list is short and is not the cost.

**Why the 2.89 million aggregates of more than one frame ended:**

| ended | aggregates | share | frames |
|---|---|---|---|
| by a frame carrying PSH, over 56 KB | 2,175,914 | **75%** | 8 |
| by a frame carrying PSH, 56 KB or less | 468,269 | 16% | 2–7 |
| by the poll's flush | 241,046 | 8% | 2–7, evenly |
| by a frame without PSH (the size limit, or one that could not merge) | 3,323 | 0.1% | 8 |

- **The client's PSH ends them.** 82% of the aggregates are 8 frames (61,920 bytes), ended by the last segment of the client's 64 KB TSO burst. The 64 KB GRO limit almost never decides (3.3k), as round 09 found.
- **`grolen.bt` agrees:** 74% of what TCP receives is 32–64 KB, most of it 8–9 segments.
- **The per-poll counts are missing:** the `kretprobe` on `xeu_rx_poll()` attached but never fired, so `@poll_frames` and `@poll_aggs` are empty. The flush figure above stands without them.
- **The WRITE count in the window does not square with the frames:** 140,804 WRITEs against 22.2 million frames is 158 frames a WRITE, against about 136 for 1 MiB. The 9 probes slowed the window to about 21 GB/s. The shares above are taken from the frames alone and do not depend on it.

**So: GRO's cost is per frame, not in the list or in the merge decisions.** 85% of frames take the merge path. Each has had a full skb built around its buffer by xeu (`napi_build_skb`) only to be freed once its page is moved into the aggregate. In run89's ring profile that build and free is about 7% of the samples beyond the GRO frames: `napi_build_skb`, `__build_skb_around` and `__pi_memset_generic` about 4.6%, and `kmem_cache_free`, `__memcg_slab_free_hook` and `napi_skb_cache_put` about 2.5%. A driver that hands GRO page fragments (`napi_get_frags()` and `napi_gro_frags()`) builds no skb for a frame that merges: an xeu change, a question for Xsight, and a candidate for step 3 beside what step 2 finds about GRO's misses.
