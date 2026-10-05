# On-die receive copy: one CPU copy, landing area kept on-die

Sub-project of the NFSD TCP write-path zero-copy (page-loan) series, started
2026-09-27 from a proposal by **David Flynn** (relayed by Mike Snitzer).
Goal: on hardware whose NVMe cannot take a loaned (non-page-tiled) payload
direct, accept one CPU copy of every WRITE payload, but arrange that the copy
reads from a small receive landing area that stays in on-die cache, so that
the only DRAM round trip per written byte is the back-end one (copy
destination written to DRAM, NVMe DMA reads it). That is 2 DRAM touches per
written byte (3 if the copy's destination stores read-for-ownership, 3.3),
the same as the read path, with no SGL-capable SSD needed.

This document is a **scoping spec only**. Code state for every citation:
`/root/kernel/linux` on `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`, tip
`5119bf775071` (v7.1.13-14 + page-loan series + NVMe SGL phase 1). Xsight
driver citations are from the scratchpad copy of `kmod-xsight-2.0.1`
(`$XK` below: `/tmp/claude-0/-root-kernel-linux/45306a20-20b9-45e8-9187-13ca8461c9c9/scratchpad/xeu/kmod-xsight-2.0.1`),
whose `xeu_drv.c` already carries the phase-2 `rx_ip_align` patch, so its
line numbers are a few lines off those quoted in `PROJECT.md`. Nothing here
was run on tardis1; the box this was written on has no CMN, SPE or E1.

## 0. Status / handoff

- **Scoping only. No code, no commits.** This file is the only artifact.
- **Adversarial review: DONE 2026-09-27** (ultracode; 20 findings, section
  10). All 20 verified against the code and folded in; none rejected; a few
  items are left as open questions. Implementation does not start until
  Mike approves; the spec further recommends running the phase-0 gates in
  section 6 before any code.
- **Headline finding (section 2):** the tree's *copy* path
  (`sunrpc.svc_tcp_rx_loan_pages=0`, or any record that is not loan
  eligible) already is David's data path in shape: one CPU copy, the NIC
  buffer released right after the copy (before decode, storage and reply),
  and a page-tiled destination that goes O_DIRECT on a PRP-only NVMe **when
  the WRITE's file offset is a multiple of 4** (2.3). What it does **not**
  have is David's *physics*: the copy is not prompt (it waits for a free
  nfsd thread, and those threads sit in NVMe latency), and the landing area
  is not small (tens of MB to GBs of posted NIC buffers plus autotuned
  socket queues). So the sub-project probably reduces to **making the
  landing set small and recycled, and making the copy prompt**, not to a
  new data path. That is a hypothesis to be tested by measurement first
  (section 6), not a conclusion.
- **Cheaper route found in review (3.6, Z1):** with the existing, untested
  xeu paired-posting (HDS) patch and an MTU of 8244 (MSS 8192) on both
  ends, every loaned payload fragment would be `[off 0, len 8192]`, which
  passes the PRP gate with no SGL and **no CPU copy at all**. That is one
  module parameter and one MTU change, and it needs neither R1 nor any
  SUNRPC work. It is now the **first** thing to try on tardis1, next to
  the R1 gate. If it goes direct, this sub-project becomes the fallback.
- **Blocking unknown (R1, section 8):** whether E1 NIC RX DMA writes are
  allocated into an on-die system cache at all. The driver requests
  write-allocate (section 5), but whether the SoC honours it is
  UNVERIFIED. Decided by the **phase-0 R1 gate** (6, 0a-R1): an iperf3
  small-ring test with memory-controller counters, no NFS, no NVMe. If NIC
  writes do not allocate, this sub-project stops there.
- **Asks outstanding:**
  - Mike / David: where is David's "confine the NFS RPC receive buffers to
    a specific pool" change? Nothing in this repo confines receive buffers
    (section 2.5). If it is an out-of-tree patch we need it.
  - Mike / David: is the client fleet TCP-only (why NFS/RDMA is a
    non-goal, 3.2), and can the storage network run MTU 8244 (Z1)?
  - Hammerspace / Xsight: the hardware questions in section 5.3.
  - tardis1 access for the phase-0 measurements (section 6).
- **Rules carried over from the SGL sub-project:** incremental commits at
  the tip, feature commits separate from KUnit commits, no HS-only
  shortcuts (interfaces properly exposed), and this line of work gets its
  own report artifact when there is something to report.

## 1. The idea, and why now

### 1.1 Motivating finding

tardis1's NVMe is PRP-only (Mike, 2026-09-27; `NVME_SGL_SUPPORT_PROJECT.md`
section 0, `xeu-SGL-compat-receive-placement/README.md` "tardis1 result").
All nine namespaces report `virt_boundary_mask=4095`, `dma_alignment=3`,
and since `bc840b21a25a` a 4095 boundary means the controller advertises no
SGLs. Consequences:

- A loaned WRITE payload is the skb frag pages themselves, one fragment per
  TCP segment. With today's xeu placement each fragment starts at the
  receive headroom and ends mid-page. On a 4095 boundary those joints fail
  the admission gate (`nfs_dio_iter_aligned_and_splittable()`,
  `fs/nfs_common/nfs_dio.c:77-104`) and the WRITE is demoted to one
  buffered (DONTCACHE by default, `nfsd_direct_misaligned_dontcache = true`,
  `fs/nfsd/vfs.c:60`) segment. So on tardis1 the loan removes the SUNRPC
  copy only to put a page-cache copy in its place.
- SGL phase 1 cannot help (it only relaxes the gate when the device has no
  boundary), and xeu phase 2 (`rx_ip_align`) fixes 4-byte offsets but not
  the mid-page joints.
- The gate tests joints against the **4 KiB segment boundary, not
  PAGE_SIZE** (`nfs_dio.c:90-99`: a joint counts only if `((off+len) |
  next->bv_offset) & seg_mask` is non-zero, `seg_mask = 4095`). So the
  remaining zero-copy routes on this hardware are (a) payload placed at
  page offset 0 with a TCP MSS that is a multiple of 4096 (Z1, 3.6: HDS
  paired posting + MTU 8244, HYPOTHESIS, untested), and (b) E1 header-data
  split with payload coalescing into whole pages (firmware ask, not
  available; only needed at MSS values like 1448 that are not 4 KiB
  multiples, `xeu-hds-experiment/README.md:117-121`). NFS/RDMA is a third
  (3.6, Z2), blocked on software availability.

### 1.2 David Flynn's proposal, restated

1. **Accept one CPU copy.** Two data movements per written byte (NIC → RAM
   and RAM → NVMe) plus one CPU copy is acceptable, provided only **one**
   of those movements goes to DRAM and back.
2. **Make the copy prompt:** within ~10 us of the data landing.
3. **Keep the landing area small** (1-4 MB total), so it can stay resident
   in on-die cache between the NIC write and the CPU read.
4. **Release the landing buffer immediately after the copy**, before
   storage acknowledges the write.
5. **The copy destination is where storage alignment is decided.** Only
   once nfsd and the file system are involved is the storage geometry
   known; the destination (a buffer, possibly the page cache) is what goes
   to DRAM and then to NVMe. NVMe latency (multi-ms tails) is absorbed by
   the destination, where it cannot be controlled anyway, not by the
   landing area.
6. Supporting changes David named: dedicate a core or two and a MB or two
   of memory to the copy; use his "confine receive buffers to a pool"
   enhancement; stop holding the RPC receive buffer across storage
   completion on stable writes; possibly raise nfsd thread priority.
7. Claimed payoff: no SGL-capable SSDs needed, and effectively one round
   trip to DRAM per written byte.

The 10 us and 1-4 MB figures are David's initial guesses, not derived from
the cache; section 3.1 replaces them with a residency model.

Where it came from: tardis1's measured write path is **memory-stall
bound** (IPC 0.35, 72% of cycles in `stall_backend_mem`, `memmove` 41% of
write-path CPU, writes ~24 GiB/s vs reads ~53 GiB/s, `PROJECT.md:30-38`).
**DRAM-bandwidth saturation is UNVERIFIED:** `STALL_BACKEND_MEM` counts
cycles with a pending demand load in the core's last-level cache
(`tools/perf/pmu-events/arch/arm64/arm/neoverse-n2-v2/stall.json:27-28`),
which is just as high for a latency-bound copy short of memory-level
parallelism; no DRAM bandwidth figure was measured. The write path touches
DRAM 4 times per byte against the read path's 2 by accounting
(`PROJECT.md:40-50`), not by measurement.

## 2. What the tree already does

### 2.1 Doc drift first

`PROJECT.md:35-60` describes the copy as `svc_tcp_read_msg() ->
sock_recvmsg -> tcp_recvmsg`, and tardis1's 41% memmove stack was recorded
on a kernel with that path. This tree receives with a `->read_sock` actor
instead (`2deb9439b7f2` "SUNRPC: Receive RPC records with ->read_sock"):
`svc_tcp_recvfrom()` takes `lock_sock()` and calls
`sock->ops->read_sock(sk, &desc, svc_tcp_recv_actor)`
(`net/sunrpc/svcsock.c:2444-2445`), and the copy is
`svc_tcp_rx_copy_skb()` → `skb_copy_datagram_iter()` (`svcsock.c:1698`).
The bytes moved are the same; the timing and lifetime statements below are
for the new path. **Any tardis1 baseline for this sub-project must be
re-taken on this tree** (or the HS twin), not taken from `PROJECT.md`.

### 2.2 The copy path is already "copy, then release the landing buffer"

| step | where | context |
|---|---|---|
| NIC completion → IRQ (or `XEU_POLL` timer) → NAPI → `napi_build_skb` → GRO | `$XK/xeu-esdk-v2.0.1/xeu_drv.c:795-946`; E1 completion coalescing on by default (`xsl-core-esdk-v2.0.1/epu_hw/ll_drivers/dma_drv.c:40-46`) | hardirq / softirq on the NAPI CPU |
| skb queued (or coalesced into the queue tail and freed, `tcp_queue_rcv()` → `tcp_try_coalesce()`, `net/ipv4/tcp_input.c:5529-5543`), `svc_data_ready()` sets `XPT_DATA`, `svc_xprt_enqueue()` | `svcsock.c:815-831`, `svc_xprt.c:484-529` | softirq; **no copy here**; enqueue refused if the socket has `SOCK_NOSPACE` or the slot limit is hit (2.4) |
| idle nfsd thread woken on the pool of the softirq CPU | `svc_pool_for_cpu()` `svc.c:263`, `svc_pool_wake_idle_thread()` `svc.c:691-713` | scheduler; no idle thread → trace only (2.4) |
| `svc_recv` → `xpo_recvfrom` → `svc_tcp_recvfrom()` → `read_sock` actor | `svc_xprt.c:870`, `svcsock.c:2444-2445` | nfsd process context, under the socket lock |
| payload copied into the thread's own `rq_pages` | `svc_tcp_recv_iter_init()` `svcsock.c:1591-1605`, `skb_copy_datagram_iter()` `:1698` | nfsd thread, SCHED_NORMAL |
| skb unlinked; **free deferred to the skb's `alloc_cpu`** (the NAPI CPU) unless the reader runs on that CPU; ACK/window update | `tcp_eat_recv_skb()` → `skb_attempt_defer_free()` (`net/ipv4/tcp.c:1615-1625`, `net/core/skbuff.c:7303-7348`), `tcp_cleanup_rbuf()` `tcp.c:1734-1736` | unlink in the same `read_sock` pass; the page returns to the page_pool later (below) |
| publish: COPY mode releases any refs and sets `rq_xprt_ctxt = NULL` | `svc_tcp_rx_publish()` `svcsock.c:2199-2234` | before decode |
| transport re-queued for the next record, then this thread runs the RPC | `svc_xprt_received()` `svcsock.c:2523`, `svc_process()` `svc_xprt.c:886` | |

So in copy mode, **David's step 4 (release the landing buffer before
storage) already holds for NIC memory**, with a deferral: the skb is
unlinked in the `read_sock` pass that copied it, but `tcp_eat_recv_skb()`
hands it to `skb_attempt_defer_free()`, which queues it on the defer list
of `skb->alloc_cpu` (`skbuff.c:7322-7340`; freed at once only if the reader
is on that CPU or the list is full). The list holds up to
`net.core.skb_defer_max` = 128 skbs per CPU/node (`net/core/hotdata.c:24`)
and is flushed at that CPU's next `net_rx_action`/busy-poll, or by an IPI
at half capacity. When the flush runs in softirq on the NAPI CPU,
`page_pool_napi_local()` (`net/core/page_pool.c:877-902`; xeu sets
`.napi`, `xeu_drv.c:443-455`) lets pages go to the pool's direct alloc
cache, which is LIFO and small (`PP_ALLOC_CACHE_SIZE` = 128 entries on 4K
pages, 8 on 64K, `include/net/page_pool/types.h:56-64`), otherwise to the
ptr_ring. The recycle order, and so the landing reuse distance, therefore
depends on page size and on which path the free takes: **UNVERIFIED**,
measured in phase 0b. An skb that the actor consumes only in part at a
record boundary stays queued (`tcp.c:1675-1717`) and is landing memory too.

What is held until after the reply is the copy *destination*, the
thread's `rq_pages` arena — which is exactly the buffer David wants to
absorb NVMe latency.

`rq_pages` are per thread and reused: `svc_alloc_arg()` refills only NULL
slots and lays out `head = rq_pages[0]`, `pages = rq_pages + 1`,
`page_base = 0` (`svc_xprt.c:709-744`). The footprint is about
`nthreads × sv_max_mesg` plus partial records parked in `svsk->sk_pages[]`.

### 2.3 The copy destination goes O_DIRECT on PRP-only NVMe — for 4-byte-aligned file offsets

- The destination is page-tiled: whole-page bvecs at the linear offset
  (`svc_tcp_recv_iter_init()`, `svcsock.c:1600-1601`). After
  `nfs3svc_decode_writeargs` / `nfsd4_decode_write` carve out the payload,
  `xdr_buf_to_bvec()` (`net/sunrpc/xdr.c:213-261`) yields
  `[head page + H, PAGE_SIZE - H] [page,0,PAGE_SIZE] ... [page,0,tail]`,
  where H (the RPC+NFS header length) is a 4-byte multiple.
- In the gate (`nfs_dio.c:77-104`) every joint is `(PAGE_SIZE | 0) &
  seg_mask = 0` for a 4095 mask (4K and 64K kernels alike). The memory
  test `(off|len) & (mem_align-1)` (`nfs_dio.c:88-94`, `mem_align = 4`) is
  applied to the **middle** segment, whose first offset is `H + prefix`,
  with `prefix = round_up(pos, offset_align) - pos` (`nfs_dio.c:297-302`).
  - If `pos ≡ 0 (mod 4)`, the middle passes and is `IOCB_DIRECT`; a file
    offset that is not block aligned only adds buffered prefix/suffix
    segments.
  - If `pos` is **not** a multiple of 4 (e.g. `pos = 4096k + 2` gives
    `prefix = 510` for a 512 offset_align, `off ≡ 2 mod 4`), the middle
    fails and `nfs_dio_split()` issues the **whole WRITE** as one
    buffered/DONTCACHE segment (`NFS_DIO_MEM_MISALIGNED`,
    `nfs_dio.c:333-340`).
- Evidence: `NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md` (the A/B at `:21-52`)
  on an nvme-loop-over-brd XFS export with `virt_boundary_mask=4095
  dma_alignment=3`: copied payloads went direct with zero rejections.
  `NVME_SGL_SUPPORT_PROJECT.md` section 1 says the same ("the copied arena
  needs no SGL").
- **UNVERIFIED on tardis1:** no recorded `nfsd_write_direct` /
  `nfsd_write_vector` / `nfsd_write_dontcache` counts or
  `nfsd_write_dio_split` dispositions from tardis1 exist in the docs. The
  24 GiB/s `io_cache_write=4` run is consistent with the copy path going
  direct (a 1 MB wsize client writes at 4-byte-aligned offsets) but was not
  traced. Phase 0b records them.

**Implication.** On PRP NVMe with aligned client offsets, today's copy path
already has the structure David asks for — one CPU copy into a buffer that
absorbs storage latency, landing buffer freed right after the copy (with
the deferred free above), destination written with O_DIRECT (so the
"second copy" is NVMe DMA from the destination, not another CPU copy). The
DRAM touch count is still 4 (section 3.3) because of *when* the copy runs
and *how large* the landing set is, not because of where the data goes.

### 2.4 Why the copy is not prompt today

- **The copy runs only when an nfsd thread reaches the transport.** The
  thread that copies a record then runs `svc_process()` on it
  (`svc_xprt.c:886`), which for `io_cache_write=2/3/4` is a synchronous
  kiocb (`init_sync_kiocb`, `fs/nfsd/vfs.c:1464`) waiting for every DIO
  segment (`nfsd_direct_write`, `vfs.c:1334-1401`) plus
  `vfs_fsync_range()` for stable writes (`vfs.c:1393-1397`). The next
  record's copy needs a different idle thread. tardis1 workers were 86% busy
  (`PROJECT.md:34`); the thread count used there is not recorded
  (UNVERIFIED).
- **Threads are dynamic.** `svc_pool_wake_idle_thread()` does not spawn a
  thread when none is idle; it only fires `svc_pool_thread_noidle`
  (`net/sunrpc/svc.c:691-713`). A new thread is created only after some
  running thread returns from `svc_recv()` with `-EBUSY`, up to
  `sp_nrthrmax`; idle threads are killed after 5 s timeouts down to
  `sp_nrthrmin` (`fs/nfsd/nfssvc.c:919-955`; limits set at
  `svc.c:822-827`). So the no-idle-thread case costs kthread creation
  latency on top of the wait, and the thread set is not stable.
- **One reader per transport** (`XPT_BUSY`, `svc_xprt.c:472-499`, `:519`).
- **Reply backlog gates the receive.** `svc_xprt_ready()` refuses to
  enqueue a transport with `XPT_DATA` if `xpo_has_wspace()` fails (TCP:
  `SOCK_NOSPACE`, `svcsock.c:849-856`) or `svc_xprt_slots_in_range()` fails
  (`svc_xprt.c:488-497`, traced as `svc_xprt_no_write_space`). A slow reply
  path therefore also stalls the receive copy.
- **No priority:** no `sched_set_*` / `set_user_nice` anywhere in
  `fs/nfsd` or `net/sunrpc` (grep); threads loop on `svc_recv(rqstp, 5*HZ)`
  (`fs/nfsd/nfssvc.c:919`). New kthreads start SCHED_NORMAL, so a `chrt`
  applied to today's threads does not carry to threads spawned later.
- **Nothing bounds the unread backlog to the copy latency.** TCP svc
  sockets never call `svc_sock_setbufsize()` (only UDP: `svcsock.c:1051`,
  `:1268`), so each connection autotunes to `tcp_rmem[2]`. With 32
  connections (`PROJECT.md:32`) the landing area is the sum of socket
  queues plus posted NIC rings plus the skb defer lists (2.2).
- Existing instrumentation for this latency: `svc_xprt_dequeue` reports
  `wakeup-us` and `qtime-us` (`include/trace/events/sunrpc.h:2033-2055`),
  plus `svc_pool_thread_wake` (`:2152`), `svc_pool_thread_noidle`,
  `svc_xprt_no_write_space`, `nfsd_dynthread_*`, and
  `svcsock_tcp_rx_lifetime` (`:2390-2453`). None of these sees the NIC side
  (IRQ/NAPI/GRO), and xeu exposes no ethtool coalescing control and no RX
  hardware timestamps (`$XK/xeu-esdk-v2.0.1/xeu_ethtool.c:623-637`).

### 2.5 The loan path, and "confine receive buffers to a pool"

- Eligibility: plaintext, AUTH_NULL/UNIX, procedure with `pc_xdr_bvec`
  (NFSv3 WRITE, NFSv4 COMPOUND; `svc_proc_accepts_xdr_bvec`,
  `include/linux/sunrpc/svc.h:452-456`).
- Loaned NIC pages are released only by `svc_tcp_release_ctxt()`
  (`svcsock.c:559-585`), called from `svc_tcp_sendto()` before the reply is
  sent (`svcsock.c:2626`) or from `svc_xprt_release()` (`svc_xprt.c:583`).
  Either way that is after the whole VFS write, for every stability level.
- **Loans do not confine anything.** The loan takes a plain page reference
  (`get_page()`, `svc_tcp_rx_append`). When the driver returns the page
  while nfsd still holds it, `__page_pool_page_can_be_recycled()` fails
  (`net/core/page_pool.c:818-823`), the page leaves the pool, and the pool
  refills from the buddy allocator. The NIC's landing address set therefore
  grows with in-flight loans × payload.
- There is a pool-aware reference that would keep loaned pages in the pool:
  `page_pool_ref_netmem()` (`include/net/page_pool/helpers.h:315`), as
  io_uring zcrx uses (`io_uring/zcrx.c:1445`). Noted as a mechanism, not a
  recommendation (section 4).
- **Nothing in this repo caps or confines the receive buffer set**
  (searched `git log --all`; `page_pool` fields in `svc_tcp_rx_state` are
  provenance for tracing only). If David has a
  separate change, we need it (section 0).
- Consequence on tardis1: a loaned payload fails the PRP gate and is
  written buffered/DONTCACHE from the loaned pages — i.e. loan + CPU copy
  into the page cache, with the loan held until the reply. That is already
  the loan-plus-buffered-IO combination David suggests, minus early
  release, and it is
  *worse* for landing residency than copy mode.
- **Decoded NFSv4 arguments alias the receive buffer.** `svcxdr_savemem()`
  returns the raw pointer into `rq_arg` memory unless the bytes went
  through the decode scratch buffer (or HIGHMEM is on)
  (`fs/nfsd/nfs4xdr.c:158-175`). File names (`nfsd4_decode_component4()`,
  `:232-249`), opaques (`:218-229`) and layoutreturn bodies (`:865-875`) are
  kept that way. In loan mode everything past `SVC_TCP_RX_FIXED_BYTES` is
  bvecs over loaned NIC pages (`svcsock.c:2236-2240`), so those pointers
  can point into NIC pages. This bounds any early-release design (4.4).

### 2.6 What is NOT known

- Whether NIC RX DMA on E1 allocates into an on-die cache (section 5).
- The system-cache size on tardis1's SoC, and any I/O allocation limit.
- Whether the `memmove` source loads hit cache or DRAM today.
- The actual landing-to-copy latency distribution on tardis1, including
  the NIC-side part (completion coalescing, IRQ, NAPI, GRO).
- The nfsd thread limits (`sp_nrthrmin`/`sp_nrthrmax`) and `tcp_rmem` used
  for the 24 GiB/s run.
- The WRITE disposition counts (direct/vector/dontcache) on tardis1.
- Per-core copy bandwidth on N2 from cache and to DRAM, and whether the
  copy's destination stores read-for-ownership.
- Absolute DRAM bandwidth of the write and read runs, and the DRAM peak.
- Whether xeu was built with `XEU_POLL` (timer-driven interrupts,
  `int_usecs` default 1000, `xeu_drv.c:98-101`, `:1358-1386`,
  `Makefile:19-21`).

## 3. Goals and non-goals

### 3.1 Goals (quantitative)

**Residency model.** Let C be the system-cache (SLC) bytes available to
I/O-allocated lines (net of any I/O way limit), R_nic the NIC landing rate,
and R_alloc the total rate of allocating writes into the SLC: NIC writes +
destination lines written back from L2 + NVMe DMA reads if they allocate.
At a 50 GB/s write target, R_nic ≈ 50 GB/s and R_alloc ≈ 100-150 GB/s,
because the destination (D1 holds ~100 MB per port across NVMe latency, far
beyond any plausible SLC) streams through the SLC continuously. Then:

- **A → B** (the copy read hits): landing-to-copy age ≪ C / R_alloc, the
  SLC turnover time.
- **B → B'** (the landing line is overwritten in cache, never written
  back): landing set L (the reuse distance, in bytes actually written) ≪
  C × R_nic / R_alloc, i.e. about C/2 to C/3.

C is unknown (5.3 q1). For illustration only, not a hardware fact: C = 32
MB gives a turnover of ~210-320 us and an L bound of ~10-16 MB; C = 8 MB
gives ~55-80 us and ~2.5-4 MB. David's 10 us / 1-4 MB are carried as
initial guesses; they may be too strict (large C) or too loose (small C).
Phase 0a must obtain C before any target is fixed, and phase 0c's knee is
predicted at L ≈ C × R_nic / R_alloc.

| # | target | measured by |
|---|---|---|
| G1 | landing-to-copy age p99 ≪ C / R_alloc (David's guess: ~10 us), measured from the NIC side, per TCP byte | sequence-keyed probe from xeu RX completion to the copy (6, 0b) |
| G2 | landing set L ≪ C × R_nic / R_alloc (David's guess: 1-4 MB), in **bytes actually written** (frame bytes + touched skb_shared_info/header lines), address-recycled; DMA-mapped span and truesize reported separately | ring config × frame bytes, `ss -tm`, defer-list depth, page_pool stats |
| G3 | landing buffers are released **before storage completion** (already true in copy mode; must stay true) | `svcsock_tcp_rx_lifetime` |
| G4 | **2 DRAM touches per written byte** (3 if the copy's destination stores RFO, until measured), down from 4 | split DRAM read and write bytes (6, 0b), per NIC byte landed and per NFS byte written |
| G5 | write throughput rises toward the read path (~50 GiB/s aggregate on tardis1), **conditional** on phase 0b showing the write run within ~10-15% of measured DRAM peak; otherwise the throughput model in 5.2 does not apply | client benchmark + absolute DRAM bandwidth |
| G6 | no SGL-capable SSD required; works on PRP-only NVMe | tardis1 as equipped |

Latency budget G1 must absorb (each item to be measured or asked, none
known): E1 completion coalescing (on by default, timeout 1000 cycles of an
unstated clock, `dma_drv.c:40-46`) + IRQ + NAPI + GRO flush + cross-CPU
wakeup of the copying thread + cpuidle exit
(`/sys/devices/system/cpu/cpu*/cpuidle/state*/latency`) + the copy itself.
No budget is adopted until these are known.

Note on G1: it is per *segment*, not per RPC record. A 1 MiB record cannot
be copied by one core in 10 us (at an assumed 10-20 GB/s per core it takes
50-100 us; UNVERIFIED rate). The copy has to keep pace incrementally as
segments arrive.

Arithmetic (not hardware fact): 400 Gb/s = 50 GB/s = 50 KB/us per port. A
10 us deadline at 2 × 400G needs ~1 MB resident; 40-80 us fills "a few MB";
at 100 us it is ~10 MB. At today's ~24 GiB/s write rate these are ~4x
smaller.

### 3.2 Non-goals

- Designing a new zero-copy path. Zero-copy on PRP NVMe needs payload
  fragments that start at page offset 0 with 4 KiB-multiple lengths (Z1,
  3.6), payload coalescing, SGL, or NFS/RDMA. Z1 is *tested* as a phase-0
  probe because it is nearly free; any work beyond the probe stays with the
  parent series and the SGL sub-project.
- Changing the on-the-wire protocol or the client. NFS/RDMA (Z2, 3.6) is
  out of scope **only** because no XFA verbs driver ships in kmod-xsight
  2.0.1 and client RoCE is unconfirmed; that is a question to Xsight and
  Mike (5.3 q11), not a technical rejection.
- Kernel-bypass or userspace NFS.
- Replacing the loan series. Loans remain the right answer on SGL-capable
  NVMe (and on PRP NVMe if Z1 works); this sub-project is for hardware
  where they cannot go direct.

### 3.3 DRAM-touch budget

W = DRAM write, R = DRAM read; dirty write-backs counted. "+1 RFO" applies
to every row with a CPU copy: if the destination stores read-for-ownership
(N2 write-streaming should avoid it for large memcpy; UNVERIFIED, 5.3 q8),
add one DRAM read.

| path | sequence | touches (no RFO) | with RFO |
|---|---|---|---|
| A. today's copy, landing evicted before the copy | NIC W, copy R, destination W, NVMe R | **4** (`PROJECT.md:43-50`) | 5 |
| B. today's copy, landing still resident at copy time, default rings | copy R hits, but the dirty landing line is written back later because its address is not reused soon enough (reuse distance UNVERIFIED, 2.2), then destination W, NVMe R | **3** | 4 |
| B'. **target**: landing confined and recycled, next NIC write overwrites the dirty line in cache | destination W, NVMe R | **2** | 3 |
| C. loan + O_DIRECT (SGL NVMe, or Z1 on PRP) | NIC W, NVMe R | 2 | 2 (no CPU copy) |
| D. loan demoted to buffered (tardis1 today with loans on) | NIC W, copy R, page-cache W, writeback R | 4, with a larger landing set than A | 5 |

B' requires, all UNVERIFIED on E1: NIC writes allocate into the system
cache; a full-line NIC write to a resident dirty line replaces it without a
DRAM write-back; and the copy's destination stores do not evict the
landing set (the kernel `copy_*_iter` → `memcpy` uses ordinary stores).
**If RFO occurs, the best case of the copy route is 3, not parity with the
read path**; only C reaches 2 regardless.

### 3.4 Mike's aside: "buffered IO doesn't allow zero copy via O_DIRECT, right?"

**Right.** A buffered `->write_iter` copies the source into page-cache
folios and writeback DMAs from the folios; the RPC buffer never reaches the
device. Only O_DIRECT lets NVMe DMA read the RPC buffer itself.

But David's idea **does not need buffered IO**. It accepts one CPU copy by
design, and "the second copy" in it is the NVMe DMA out of the copy
destination. If that destination is a page-tiled buffer written with
O_DIRECT — which is exactly today's copy path, `rq_pages` + `nfsd_direct_write`
— the back end is zero-copy *from the destination*. The page cache is one
possible destination (it frees the thread early for UNSTABLE writes via
async writeback), not a requirement, and on this workload it has a poor
record (`io_cache_write=1` measured 17-38% slower for writes on perflab,
2026-09-23; project memory, not re-verified here). XFS also serialises
buffered writers per file (`XFS_IOLOCK_EXCL`, `fs/xfs/xfs_file.c:1037-1049`)
where aligned DIO takes it shared (`:743-763`).

**Correction (2026-10-04): aligned DIO is shared only when it does not start
beyond EOF.** A direct write whose offset is past the current end of file
goes through `xfs_file_write_checks()` -> `xfs_file_write_zero_eof()`,
which upgrades to `XFS_IOLOCK_EXCL` and calls `inode_dio_wait()` before it
proceeds. nfsd threads issue the WRITEs of a growing file out of order, so
most of them start beyond EOF and queue behind every direct write in
flight. Measured on the 16 KiB host (budget rig: null_blk, dm-delay,
nvmet-loop, XFS; `seq-off`, one connection, 1 MiB sequential WRITEs of a
new file, copied records written direct): all eight nfsd threads writing
65% of the time, a mean WRITE of 120 ms where one such write alone takes
10 ms, 55 MB/s. The same local test from a single in-order submitter runs
1213 MB/s. So for sequential file creation D1's destination writes are
serialised by XFS as well as holding their threads (2.4); a file that is
being overwritten, or preallocated to its size, is not affected. Only
measured on that rig; see `../transplant/README.md`, "Why one copy cell is
slow". Phase 0b should record whether tardis1's workload creates or
overwrites.

### 3.5 David's "stable writes hold the buffer, unstable don't" — checked

`io_cache_write` values: `fs/nfsd/nfsd.h:143-150`.

| mode | UNSTABLE | FILE_SYNC / DATA_SYNC |
|---|---|---|
| 0/1 buffered/DONTCACHE (`vfs.c:1487-1495`) | copy into page cache, return; buffer held only for the copy and reply | `IOCB_DSYNC` → `generic_write_sync()` inside `->write_iter`: **waits for NVMe while holding the RPC buffer**, though the data is already in the page cache. David is right here. |
| 2 DIRECT (`vfs.c:1334-1401`) | sync kiocb waits for device completion: **buffer held across NVMe**, but no fsync. David's premise does not hold here. | same, plus one `vfs_fsync_range()` (`vfs.c:1393-1397`) |
| 3/4 | raised to DATA_SYNC / FILE_SYNC (`nfsd_write_raise_stability`, `vfs.c:1320-1331`) | held through device completion and fsync |

tardis1 ran `io_cache_write=4` (`PROJECT.md:31`), so every RPC buffer lived
through NVMe completion plus fsync. In DIRECT mode the destination pages
*are* the DMA source; they cannot be released before the bio completes
without another copy. In copy mode that is fine — the destination is meant
to absorb storage latency and the NIC buffer is already gone. The cost is
that the **thread** is held, and the thread is also the only thing that can
do the next copy (2.4). The real decoupling needed is thread ↔ copy, not
buffer ↔ storage. `io_cache_write=2` (no per-write fsync for UNSTABLE) and
`=1` (thread freed after the page-cache copy for UNSTABLE) are zero-code
ways to shorten the thread hold, and phase 0c uses them to tell "threads
held by fsync" from "too few threads".

### 3.6 Alternatives considered

| route | DRAM touches | CPU copies | hardware premise | code cost | status |
|---|---|---|---|---|---|
| **Z1. HDS paired posting + MSS 8192** (loans on, MTU 8244 both ends, `hds_hdr_len=66`) | 2 | 0 | E1 honours paired posting (patch untested); fixed 66-byte headers (TCP timestamps, no SACK/other options in data segments); client TSO emits full-MSS segments; network carries MTU 8244 | one out-of-tree driver patch (exists: `xeu-hds-experiment/0001-*.patch`), config | **HYPOTHESIS; cheapest phase-0 probe** |
| Z2. NFS/RDMA | 2 | 0 | an XFA verbs driver (none in 2.0.1: no `ib_register_device` in the kmod; E1 has RDMA device IDs `0x82/0x83`, `xsl-core-esdk-v2.0.1/epu_hw/arch/common/dev/epu_pci_ids.h:30-39`); client RoCE (UNVERIFIED) | none in the kernel; vendor driver + client change | blocked on software; question 5.3 q11 |
| Z3. SGL NVMe | 2 | 0 | SGL-capable SSDs | done (SGL phase 1) | not on tardis1 (PRP-only) |
| Z4. on-die copy (this spec) | 2 (3 with RFO) | 1 | NIC writes allocate into SLC and overwrite in place (R1, R2) | configuration first, maybe SUNRPC receive/execute split | phase 0 gated on R1 |

Why Z1 can work: the paired-posting patch lands each frame's payload at
offset 0 of its own page (`xeu-hds-experiment/0001-*.patch:20-32`) and is
usable whenever a header buffer plus one payload buffer hold a full frame
(`xeu_rx_hds_usable()`, patch `:156-180`); on 64 KiB pages one buffer holds
65150 bytes (`xeu-SGL-compat-receive-placement/README.md:100`), so any MTU
up to `XEU_MAX_MTU` 9000 (`$XK/xeu-esdk-v2.0.1/xeu_drv.h:54`) fits. MTU
8244 = 20 (IP) + 32 (TCP with timestamps) + 8192. Every interior payload
fragment is then `[0, 8192]`, whose end `8192 & 4095 = 0`, so the gate
sees no joint (`nfs_dio.c:90-99`); a record's first fragment starts at a
4-byte-aligned stream offset and ends at 8192, its last starts at 0. A
short segment or a header of another length shifts or cuts one fragment
and demotes only the record that contains it (correct data, buffered).
Costs: the loan is held until the reply, so the landing set is bandwidth ×
storage latency and loaned pages leave the page_pool (2.5); on the 64 KiB
kernel each 8192-byte segment occupies a 64 KiB page (truesize ratio
~1/8). The 2.3 rule (`pos ≡ 0 mod 4`) applies here too. The HDS README's
statement that coalescing is needed (`xeu-hds-experiment/README.md:117-121`)
assumes MSS 1448.

Why Z2 would work: svcrdma pulls the Read chunk into `rq_pages`
contiguously from `rc_pageoff` (`net/sunrpc/xprtrdma/svc_rdma_rw.c:771-795`),
i.e. page-tiled, which the PRP gate admits as in 2.3.

## 4. Design options (for Z4)

### 4.1 Copy destination

| | D1: `rq_pages` + O_DIRECT (today's copy path) | D2: file-offset-aligned bounce + O_DIRECT | D3: page cache (buffered/DONTCACHE) | D4: loan, prompt copy out, release loan |
|---|---|---|---|---|
| DRAM touches (B' physics) | 2 (+1 RFO) | 2 (+1 RFO) | 2 (+1 RFO) + page-cache metadata | 2 (+1 RFO), only if the copy runs before the loan is evicted |
| memory held across NVMe | ~bandwidth × latency of `rq_pages` in DRAM (~100 MB per port at 50 GB/s × 2 ms) | same | same, as dirty/writeback folios | the destination; loan only until copy *if* a release hook is added |
| nfsd thread held | whole RPC incl. NVMe + fsync | same | UNSTABLE: copy only; FILE_SYNC: through fsync | per destination |
| PRP-legal | yes if `pos ≡ 0 mod 4` (2.3) | yes | n/a | via destination |
| new code for correctness | none | high: SUNRPC must decode NFS (v4: SEQUENCE, PUTFH) before placing the payload — layering violation | low (already happens for loans on PRP) | medium-high: an early "release rx now" around `svc_tcp_rx_release_refs()` (`svcsock.c:436`) and `svc_tcp_rx_clear_xdr()` (`:496`), constrained by 4.4 |
| gain on NVMe | baseline | **little**: `dio_mem_align` is 4, so H-offset is already fine; would also direct `pos ≢ 0 mod 4` writes; mainly helps devices with `dma_alignment >= 511` | async writeback for UNSTABLE | none over D1: decode happens *before* the copy, which delays it |

**D1 dominates on PRP NVMe.** D4 only moves the copy later (after decode),
which is the wrong direction for G1. D2 buys nothing on NVMe for aligned
clients. D3 is a fallback. This is David's point 5 answered: the file
system *does* decide alignment, but on NVMe with `dma_alignment=3` the only
constraint it imposes on memory is page tiling and a 4-byte-aligned file
offset, which the SUNRPC arena already meets without knowing the offset.

### 4.2 Confining the landing set (G2)

Units. xeu charges a full `PAGE_SIZE` of truesize per RX buffer
(`napi_build_skb(page, PAGE_SIZE)`, `$XK/xeu-esdk-v2.0.1/xeu_drv.c:824`;
`skb_add_rx_frag(..., PAGE_SIZE)`, `:852-857`; one order-0 page per
descriptor, `:443-445`; data area `PAGE_SIZE - headroom - shinfo`,
`:88-96`). On the 64 KiB kernel one ≤9014-byte frame therefore carries
~64 KiB of truesize (len/truesize ≈ 0.14). Three quantities must be kept
apart: **written bytes** (frame bytes + the skb_shared_info and header
lines the CPU touches; what the cache sees), **DMA-mapped span** (buffers ×
PAGE_SIZE), and **truesize** (what TCP charges to `sk_rcvbuf` and what sets
the advertised window through `scaling_ratio`).

| mechanism | exists | fit / risk |
|---|---|---|
| M1: shrink xeu rings (`ethtool -G rx`) and `num_rings` | default 8 rings × 4096 entries (`$XK/xeu-esdk-v2.0.1/xeu_drv.h:60`, `xeu_drv.c:64`), range 16-32768 (`xeu_ethtool.c:326-334`, `XEU_MIN_RING_SIZE` `xeu_ring.h:30`) | no code; sets the posted part of the landing set. At MTU 9000, 4 × 64 entries ≈ 2.3 MB **written** (16 MB mapped on 64 KiB pages) and covers ~46 us at line rate. Risk: RX drops at 400G. **Refill runs only when ≥ 32 slots are free** (`XEU_RX_REFILL_THRESHOLD`, `xeu_drv.c:109`, `:944-946`; the only other refill is the full fill at open, `:1470`): a ring of 16 can never reach 32 free, so **RX stops permanently after the first 16 frames**; at 32-63, half or more of the ring is always empty. Rings below 32 must not be used on the stock driver; phase 1 scales the threshold. |
| M2: clamp socket receive buffers (`tcp_rmem`, or a new svc TCP `sk_rcvbuf` policy) | TCP svc sockets autotune (2.4) | real window backpressure, but sized in **truesize**: rcvbuf ≥ desired window / (len/truesize), ~7x the data window on the 64 KiB kernel. A 256 KB rcvbuf holds ~4 frames (~36 KB of data), 1 MB ~16 (~140 KB); over 32 connections those windows cap throughput at Σwindow/RTT, and a starved rcvbuf triggers `tcp_prune_queue` → `tcp_collapse` (`net/ipv4/tcp_input.c`), an unplanned extra copy. On the 64 KiB kernel rcvbuf clamping is a weak landing-set limiter unless the driver packs frames or uses a smaller buffer. |
| M3: page_pool `pool_size` | sizes the recycle ring only; refills from buddy when empty (`page_pool.c:595-641`) | **not a cap**. Only helps if pages return promptly, which copy mode does (after the deferred free, 2.2). |
| M4: memory provider (devmem / io_uring zcrx style fixed area) | `net/core/devmem.c`, `io_uring/zcrx.c`; binding needs `queue_mgmt_ops` + HDS (`net/core/netdev_rx_queue.c:196-210`) | **not usable now**: xeu has no `queue_mgmt_ops`; frags are `net_iov`, unreadable to the page-based actor. A readable "SUNRPC memory provider" is new cross-subsystem work; exhaustion means drops, not backpressure. |
| M5: flow steering NFS to dedicated queues | xeu `set_rxnfc` takes one rule at location 0, UDP 4791 to a VF only; TCP rejected (`$XK/xeu-esdk-v2.0.1/xeu_ethtool.c:441-490`); no `set_rxfh` | **not possible with this driver.** Use a port dedicated to NFS or RSS over fewer channels. |
| M6: `net.core.skb_defer_max` | sysctl, default 128 (`net/core/hotdata.c:24`) | bounds the per-CPU/node deferred-free backlog (2.2); lower values return pages sooner at some free-path cost. |

### 4.3 Making the copy prompt (G1)

| mechanism | exists | fit / risk |
|---|---|---|
| P1: more nfsd threads than in-flight WRITEs + spare receivers, **with `sp_nrthrmin = sp_nrthrmax`** so the set is fixed | runtime | no code. Bounds the wait for an idle thread and removes dynamic-spawn latency (2.4). Cost: more `rq_pages` arenas (destination memory, which is the DRAM side anyway). |
| P2: raise nfsd priority (`chrt`/`renice` on the kthreads) | nothing in-tree sets it | no code to try. Apply only after the pool is at its fixed size (P1); dynamically spawned threads start SCHED_NORMAL. Cuts wakeup delay only. UNVERIFIED that it is effective on these kthreads. |
| P3: split receive from execution in SUNRPC: a per-transport (or per-CPU) receive stage copies records into arenas and hands complete records to execution threads | no; today recv and process are the same thread (`svc_xprt.c:870-886`) | directly attacks the cause in 2.4. Precedent: nvmet-tcp's `data_ready` → `queue_work_on(queue_cpu(queue), ...)` with `WQ_HIGHPRI` and optional `idle_poll_period_usecs` (`drivers/nvme/target/tcp.c:69-73`, `:1435-1441`, `:1658`). Arena ownership moves between threads; that is the design work. |
| P4: busy-poll / threaded NAPI pinned to a dedicated core | `NAPI_STATE_THREADED_BUSY_POLL` (`include/linux/netdevice.h`, `net/core/dev.c`); no `sk_busy_loop` in `read_sock` or SUNRPC | fits "dedicate a core"; removes NAPI-side variance only. |
| P5: copy in softirq from `sk_data_ready` (strparser pattern, `net/strparser/strparser.c:392-402`) | no | lowest latency, but the copy target must be a SUNRPC arena chosen without NFS context, softirq budget at 50 GB/s per port, and the actor's state machine runs under `lock_sock`. Highest risk. |

The reply-side gate (2.4, `svc_xprt_no_write_space`) is not a knob but must
be watched: if it fires, no receive mechanism helps.

### 4.4 Early release (G3)

Copy mode already meets G3 for NIC memory. For loans (D4, or loans
demoted to buffered on PRP) an early release of loan refs is possible, but
**not inside `nfsd_direct_write()`**: decoded NFSv4 arguments point into
`rq_arg`, which in loan mode is loaned NIC pages (2.5), and a COMPOUND can
carry ops after the WRITE (including a second WRITE, or name-bearing ops
such as RENAME/CREATE/LINK) that run after this WRITE's segment loop.
Dropping the refs there returns the pages to the page_pool, the NIC
re-DMAs over them, and later ops read freed or overwritten memory.

Options, both to be designed in the parent series:
- release after the whole procedure (`pc_func`, `nfssvc.c:1015`) has
  returned and before the reply is sent — this saves nothing over today for
  a single-op NFSv3 WRITE and only the reply time for NFSv4; or
- make every decoded argument that aliases `rq_arg` a copy when the
  `xdr_buf` is in `XDRBUF_PAGE_BVECS` loan mode (e.g. `svcxdr_savemem()`
  always copies then), after which a per-op release is legal.

Invariant, either way: **nothing reachable from `rqstp->rq_argp` may
reference `rq_arg` pages after the release.** UNVERIFIED still: whether the
DRC or any trace path touches `rq_arg` pages after `pc_func`. KUnit must
cover a WRITE followed by a name-bearing op in one COMPOUND. For the
buffered fallback it also requires stripping DSYNC so nfsd fsyncs itself,
as the DIRECT path already does (`vfs.c:1370`). It does **not** help copy
mode and does not shorten the DIO segment wait.

### 4.5 Recommended direction

1. **Try Z1 first** (3.6): paired posting + MTU 8244 + loans on. If
   `nfsd_write_direct` dominates, the write path is zero-copy on PRP NVMe
   and this sub-project becomes the fallback for networks that cannot run
   that MTU.
2. **Decide R1 with the iperf3 small-ring gate** (6, 0a-R1), no NFS, no
   NVMe. If NIC writes do not allocate into the SLC, stop Z4.
3. **Treat D1 (today's copy path) as the Z4 data path.** No new data-path
   code until measurements show it cannot reach B'.
4. **Then configuration only:** shrink rings (M1, ≥ 32), size `tcp_rmem`
   in truesize terms (M2), loans off, fixed thread count (P1), priority
   (P2), dedicated NAPI core (P4), `io_cache_write` 2 vs 4 to separate
   fsync hold from thread shortage. If DRAM touches per byte approach 2
   (3 with RFO) here, the deliverable is a tuning recipe plus the small
   driver fix to the refill threshold.
5. **Only if the copy cannot be made prompt with threads alone** (still
   thread-bound at `io_cache_write=2` with P1), design P3 (receive/execute
   split in SUNRPC). That is the one substantial piece of kernel work this
   sub-project anticipates, and it is upstreamable on its own merits (it
   removes the thread-starvation stall for any storage latency).
6. D4 early-release hook: tracked in the parent series under the 4.4
   invariant, not as the answer here.

Reasons: it tries the zero-code zero-copy route first, it separates the
hardware question (R1) from software work, and every step has a measurable
go/no-go.

## 5. Hardware dependencies and open questions

### 5.1 Verified in source (not on hardware)

- tardis1: ARM Neoverse-N2 64 cores, single NUMA, dual 400G `xeu` on the
  E1 DPU, 8 × WD SN8100, XFS (`PROJECT.md:30-31`). NVMe PRP-only (1.1).
- xeu is a PCI driver: `xeu_pci_probe()` (`xeu_drv.c:3031`),
  `pci_set_master()` (`:3061`), `dma_set_mask_and_coherent()` on the pdev
  (`:3063`), MSI-X via `pci_enable_msix_exact()` (`:301`, `:395`),
  `module_pci_driver` (`:3262`). The host sees xeu DMA as PCIe bus-master
  DMA through a (possibly integrated) root port.
- xeu RX: one order-0 page per descriptor, `pool_size` = ring size, `.napi`
  set (`xeu_drv.c:443-455`), `napi_build_skb()` on the page with no
  `dma_sync_*_for_cpu` in the RX path (`xeu_drv.c:824`), truesize
  `PAGE_SIZE` per buffer (`:824`, `:852-857`), default ring 4096
  (`xeu_drv.h:60`), 8 rings (`xeu_drv.c:64`), max MTU 9000
  (`xeu_drv.h:54`), refill at 32 free (`xeu_drv.c:109`, `:944-946`), no
  XDP (no `ndo_bpf` in the kmod).
- xeu ethtool: no `get/set_coalesce`, no `get_ts_info`
  (`xeu_ethtool.c:623-637`); optional `XEU_POLL` build with timer-driven
  interrupts (`xeu_drv.c:98-101`, `:1358-1386`, `Makefile:19-21`).
- E1 DMA engine: completion coalescing enabled by default, timeout 1000
  cycles (`$XK/xsl-core-esdk-v2.0.1/epu_hw/ll_drivers/dma_drv.c:40-46`);
  RX frame-buffer AXI attributes default to write-back read/write-allocate
  (`XSL_AXCACHE_WRITEBACK_RD_WR_ALLOCATE`), domain 0x2, snoop 0, QoS 0xE
  (`dma_drv.c:50-54`, applied to `FBR_FRM_BFR_AXI_ATR_DFLT` at `:556-559`).
- A ring stash API exists only in the xemi (management NIC) SDK
  (`$XK/xemi-esdk-v2.0.1/dma_drv.c`, `dma_set_ring_stash`) and stashes
  completion descriptors and a header burst, not payload. xeu never calls
  it.
- xeu ethtool flow steering: RoCEv2 UDP to a VF only (4.2 M5).
- E1 RDMA PCI IDs exist (`epu_pci_ids.h:30-39`), no verbs driver ships.
- CMN PMU events available in the driver: `hnf_cache_miss`,
  `hnf_slc_eviction`, `hnf_mc_reqs` (per HN-F node,
  `drivers/perf/arm-cmn.c:932`, `:941`, `:944`). N2 `LL_CACHE_RD` /
  `LL_CACHE_MISS_RD` count the SLC only when firmware sets
  `CPUECTLR.EXTLLC`, otherwise an L3
  (`tools/perf/pmu-events/arch/arm64/arm/neoverse-n2-v2/ll_cache.json:1-10`).
- `CONFIG_PCIE_TPH=y`, `CONFIG_ARM64_MPAM=y`, `CONFIG_ARM_CMN=m`,
  `CONFIG_ARM_SPE_PMU=m` in the HS config (per the hardware brief; config
  file `/boot/config-7.1.13-5.hs.421.el9.aarch64`, not re-read here).

### 5.2 HYPOTHESIS / UNVERIFIED

- **The N2 cores are the E1's own cores and xeu DMA reaches memory over the
  on-chip fabric** (HYPOTHESIS: `PROJECT.md:30-31`; E1 SDK text about
  "local E1 memory" in `xsl_rm_punit.c`). Against a pure-fabric reading:
  the host sees xeu as a PCIe function (5.1), so PCIe TPH/steering tags may
  still apply to the NIC leg, and the P-unit PCIe steering table (q6) may
  cover the xeu function as well as the NVMe. Both levers stay open: TPH on
  the xeu function, and the AXI allocate hint plus SoC cache policy.
- xeu DMA is hardware-coherent (inferred from the missing sync calls).
- The fabric is Arm CMN-700 or similar CHI mesh (11-bit stash node id in
  the AXUSER layout; HYPOTHESIS).
- DRAM ceiling ~100-110 GiB/s: 24 GiB/s × 4 touches ≈ 96, 53 GiB/s × 2 ≈
  106. On that model 3 touches → ~35 GiB/s, 2 → ~50 GiB/s. **HYPOTHESIS
  with weak support:** it is a two-point agreement, the write path is shown
  memory-stall bound but not bandwidth bound (1.2), and the read path at 53
  GiB/s may be limited by NIC TX or NVMe instead. G5 and the R2 cap depend
  on it; phase 0b measures absolute DRAM bandwidth against a STREAM-style
  ceiling.
- "A core or two" of copy: moving 2 × 50 GB/s through `memcpy` probably
  needs several cores (per-core rate UNVERIFIED).
- System-cache size, inclusivity, and any I/O allocation way limit (DDIO-
  like): UNVERIFIED, nothing in the tree. arm64 cacheinfo sysfs usually
  lists only L1/L2 unless PPTT describes the SLC (UNVERIFIED on tardis1),
  so q1 is the real source of C.
- Whether destination stores RFO (3.3).

### 5.3 Questions for Hammerspace / Xsight / SoC vendor

1. Interconnect type; system-cache size C, topology, inclusive/exclusive of
   N2 L2. (Needed for the 3.1 model before any target is fixed.)
2. Do xeu DMA writes with `AxCACHE` write-allocate allocate into the system
   cache? Is I/O allocation way-limited, and tunable? Do NVMe DMA reads
   allocate?
3. Does a full-line I/O write to a resident dirty line replace it without a
   DRAM write-back? Can software invalidate landing lines without
   write-back?
4. Can payload (not just header/completion) be stashed or pinned? Is there
   an MPAM PARTID for E1 DMA traffic, and cache-portion partitioning on the
   system cache?
5. Is xeu DMA coherent with the cores?
6. Do the NVMe root ports **and the xeu PCIe function itself** go through
   the P-unit PCIe steering table (`xsl_rm_punit.c:349-395`), and what do
   its `stash_ovrd`/`cache_ovrd` do? Does the xeu function expose TPH?
7. DRAM channels/speed and achievable read+write bandwidth.
8. Per-core N2 copy bandwidth from system cache and to DRAM; is
   write-streaming (no RFO) enabled, and does it engage for the arm64
   kernel memcpy store pattern?
9. Is there a memory-controller PMU for absolute DRAM read and write bytes?
10. Does E1 support header-data split with payload coalescing (would make
    the loan path direct on PRP at any MSS)? Does E1 honour the paired
    posting in `xeu-hds-experiment` (Z1)?
11. Is there (or will there be) an XFA verbs driver usable by
    svcrdma/xprtrdma? And (Mike/David) does the client side have RoCE, or
    is the fleet TCP-only?
12. What clock drives the E1 DMA completion-coalescing timeout (1000
    cycles), what is the effective completion/interrupt coalescing
    interval, and can it be set from the host?
13. Is `CPUECTLR.EXTLLC` set by tardis1's firmware?

## 6. Measurement plan (before any code)

All on tardis1, on the 64K-page kernel from this tree's HS twin, XFS on the
PRP NVMe, the client load from `PROJECT.md:32` (1 client, nconnect 16 × 2
subnets, 1 MB wsize). Every result is labelled with kernel, loan setting,
`io_cache_write`, MTU, ring config, `sp_nrthrmin`/`sp_nrthrmax`,
`tcp_rmem`, and `skb_defer_max`.

Order is by information value: cheap tests that can end or redirect the
sub-project run first.

### Phase 0a — inventory (minutes)

- `lscpu -C`; `/sys/devices/system/cpu/cpu0/cache/index*/size` (expect
  L1/L2 only unless PPTT describes the SLC; C comes from 5.3 q1).
- `ls /sys/bus/event_source/devices` for `arm_cmn*`, `arm_spe*`, and any
  memory-controller PMU; `/sys/kernel/debug/arm-cmn/map` to locate every
  HN-F and the xeu and NVMe RN-I/RN-D nodes.
- ACPI `_CCA` / DT `dma-coherent` for xeu; `lspci -vv` on the xeu function
  (TPH capability, the root port it sits under).
- Whether xeu was built with `XEU_POLL` (`modinfo xeu` for `int_usecs`).
- `CPUECTLR.EXTLLC` (5.3 q13) before trusting `ll_cache_*`.
- cpuidle exit latencies, `/sys/devices/system/cpu/cpu*/cpuidle/state*/latency`.
- `ethtool -g/-l` on both ports; `sysctl net.ipv4.tcp_rmem
  net.core.skb_defer_max`; nfsd `min_threads`/`threads`; `io_cache_write`;
  `nvme id-ctrl | grep -w sgls`.
- **Go/no-go:** a DRAM-traffic counter exists (memory-controller PMU, or
  CMN `hnf_mc_reqs` summed over all HN-Fs, ideally with read/write split
  via CMN watchpoints). If none, G4 can only be inferred and R1 cannot be
  decided cleanly (R7).

### Phase 0a-Z1 — HDS + MSS 8192 probe (minutes, no NFS code)

- Load xeu with the paired-posting patch and `hds_hdr_len=66`, set MTU
  8244 on both ends (and the switch), loans on, `io_cache_write=4`.
- Count `nfsd_write_direct` vs `nfsd_write_dontcache` (or
  `../zcstat.sh` `dirW`), and `nfsd_write_dio_split` dispositions.
- **Go:** the large majority of WRITEs direct → Z1 is the lead route;
  measure its throughput and DRAM bytes, and this spec becomes the
  fallback. **No-go:** E1 ignores paired posting, or headers vary → record
  why and continue.

### Phase 0a-R1 — does NIC landing allocate and overwrite in cache? (minutes, no NFS, no NVMe)

- One xeu port, loans irrelevant. `ethtool -G rx 256` (never below 32,
  4.2 M1), few channels. iperf3/netperf TCP receive into a small reused
  user buffer (the default 128 KB), so the consumer copies each frame
  almost immediately into a destination that stays in cache. xeu has no
  XDP, so a driver-level drop is not available.
- Count memory-controller **write** bytes and **read** bytes separately (MC
  PMU, or CMN watchpoints on the xeu RN-I/RN-D XP plus `hnf_mc_reqs`,
  `hnf_slc_eviction`, `hnf_cache_miss`, summed over all HN-Fs), per NIC
  byte landed (`ethtool -S` rx bytes). Repeat at default rings (4096) as
  the contrast.
- **Go:** at ring 256, DRAM write bytes per landed byte ≤ 0.3 and read
  bytes ≤ 0.3 → NIC writes allocate and overwrites stay on-die; B' is
  physically possible. **No-go:** ≥ 0.9 write bytes per landed byte → no
  allocation, or write-back on overwrite: stop Z4, take 5.3 q2-q4 to
  Xsight. Between the two: way-limited allocation; get C and the limit
  before continuing. Thresholds carry the error of the counter method;
  quote a background run (link idle) alongside.

### Phase 0b — baseline, current config, loans off vs on

- `svc_tcp_rx_loan_pages=0` and `=1`, `io_cache_write=4`, default rings,
  default MTU.
- Record: throughput; `nfsd_write_direct` / `_vector` / `_dontcache`
  counts and `nfsd_write_dio_split` dispositions (confirms the load is
  4-byte aligned, 2.3); `svc_xprt_dequeue` `qtime-us`/`wakeup-us`
  histograms; `svc_pool_thread_noidle`, `nfsd_dynthread_*`,
  `svc_xprt_no_write_space` counts; `svcsock_tcp_rx_lifetime`; page_pool
  stats via netlink `page-pool-get` (alloc fast/slow, **recycle cached vs
  ring**, `released_refcnt`); `ss -tm` receive-queue sizes; the TCP
  `scaling_ratio` and len/truesize.
- **Absolute DRAM bandwidth** (read and write bytes/s) for the write run
  and the read run, against a STREAM-style ceiling measured on tardis1
  (or the vendor peak, 5.3 q7). G5 applies only if the write run is within
  ~10-15% of the ceiling.
- DRAM touches per byte from **split** read and write counters (as 0a-R1),
  reported per NIC byte landed and per NFS byte written side by side (RX
  drops and retransmits land bytes that are never written).
- **RFO check:** an in-kernel (or user) copy of a cache-resident source to
  a cold destination, DRAM read bytes per destination byte. Sets G4 to 2 or
  3.
- Residency indicators: L2 refills and `LL_CACHE_MISS_RD` (prefetch
  inclusive; valid only with `EXTLLC`) under the copy, plus CMN
  `hnf_cache_miss`. SPE (`perf mem`) data source is recorded but labelled:
  SPE samples demand loads, and a streaming memcpy is prefetch covered, so
  demand loads report L1/L2 hits whether the prefetch was served from the
  SLC or DRAM.
- Landing-to-copy latency, **keyed on TCP sequence space, not the skb
  pointer** (`tcp_queue_rcv()` coalesces in-order skbs into the queue tail
  and frees them, `tcp_input.c:5529-5543`, and GRO has merged segments
  before that): record (socket, seq, end_seq, ktime) at the xeu RX
  completion dequeue (`xeu_drv.c` ~`:800`) or at `napi_gro_receive` entry,
  and look up by the copied sequence range at `svc_tcp_rx_copy_skb`
  (`svcsock.c:1698`). The part before the xeu completion read (E1
  coalescing) is not observable from Linux (no hardware timestamps);
  record it as a vendor number (5.3 q12). Histogram per ring CPU. Check
  `lanl-mem-diag/CLAUDE.md` for probe gotchas first.
- **Age-bucketed residency:** join the latency probe with the L2-refill /
  SPE samples on the copy so that "hit when young, miss when old" can be
  told from "miss always". This is what makes 0b informative about R1 in
  the nfsd setting; 0a-R1 is the clean test.
- **Expected, if the model is right:** copy mode ≈ 4 touches, landing ages
  in the hundreds of us or ms, misses concentrated in old ages.
- **Go/no-go:** go to 0c if 0a-R1 passed and young copies hit. If 0a-R1
  passed but young copies still miss, look for destination eviction (R2)
  before tuning.

### Phase 0c — configuration sweep (no kernel change)

Loans off. First predict the knee: L ≈ C × R_nic / R_alloc (3.1). Run only
the points the 0a-R1 result and that prediction make plausible, one
variable at a time, then combined:

| knob | values |
|---|---|
| `ethtool -G rx` | 4096, 1024, 256, 128, 64 (never < 32 on the stock driver) |
| `num_rings` (module param) | 8, 4 |
| `tcp_rmem[2]` | default, and values computed in truesize terms: desired data window × PAGE_SIZE / frame bytes (~7x on 64 KiB); record segments per connection each allows |
| `net.core.skb_defer_max` | 128, 32 |
| nfsd threads | current, 2x, 4x, each with min = max |
| nfsd priority | default, `chrt -f` on nfsd kthreads after the pool is fixed |
| NAPI | default, threaded + pinned core |
| `io_cache_write` | 4, 2, and 1 as a control |

Plot DRAM read and write bytes per NIC byte and per NFS byte, residency
indicators, landing-to-copy age, RX drops (`ethtool -S`), TCP
collapse/prune counters (`nstat TcpExtTCPRcvCollapsed`,
`TcpExtPruneCalled`, `TCPRcvQDrop`), the advertised window, and throughput
against the landing set in **written bytes** (posted buffers × frame bytes
+ unread socket data + defer-list skbs + partly consumed skbs), with DMA
span and truesize as separate columns.

- **Go:** a knee near the predicted L where touches per byte fall toward 2
  (3 with RFO), without drops or collapse. The deliverable may then be a
  tuning recipe plus minor driver work.
- **No-go (software):** touches stay at 4 even with a small footprint and
  low age, although 0a-R1 passed → destination eviction or reuse order
  (R2); take to Xsight with the numbers.
- **Partial:** touches fall only when age falls, and age is bound by thread
  availability **at `io_cache_write=2` with P1** → phase 2 (P3).

## 7. Implementation phases (sketch only)

Nothing here starts before phase 0.

- **Phase 1 — xeu driver:** scale `XEU_RX_REFILL_THRESHOLD` with ring size
  (e.g. `min(32, size / 4)`), needed for any ring below ~64 and for 16 to
  work at all. If Z1 wins, also productise paired posting. Out-of-tree
  driver, reported to Xsight; not an upstream kernel change.
- **Phase 2 — SUNRPC receive/execute split (P3), only if 0c shows thread
  availability bounds latency.** A receive stage per transport (or per CPU)
  that runs the existing `read_sock` actor into a free arena and hands
  complete records to execution threads. Design questions: arena ownership
  and bounds (arenas become the "destination pool"; its size is bandwidth
  × storage latency), backpressure when arenas run out (stop reading the
  socket → TCP window, not drops), `XPT_BUSY` semantics, the write-space
  gate (2.4), interaction with the loan state machine (`svc_tcp_rx_*`),
  and GSS/TLS. Interfaces are exposed as SUNRPC service options, not
  nfsd-private hacks.
- **Phase 3 — svc TCP receive-buffer policy (M2)** if `tcp_rmem` system-wide
  is too blunt: a per-service `sk_rcvbuf` setting for TCP, like UDP's
  `svc_sock_setbufsize()`, sized in truesize terms.
- **Phase 4 — optional early-release hook** for loans (4.4), in the parent
  series: release after `pc_func` returns, or copy every `rq_arg`-aliasing
  decoded argument in loan mode first; KUnit with a WRITE followed by a
  name-bearing op in one COMPOUND.
- **KUnit** in separate commits after each feature: the existing receive
  KUnit suites extended for the split (arena hand-off, backpressure, record
  boundaries). No HS-only shortcuts; module parameters only as kill
  switches, not as the interface.
- **Upstreamable:** phase 2 (it fixes a general starvation: receive stalls
  whenever storage latency occupies every thread), phase 3, phase 4. Not
  upstreamable: xeu driver changes (vendor), tardis1 tuning.

## 8. Risks, ranked

1. **R1 — E1 NIC writes do not allocate into on-die cache** (or allocate
   into a way-limited slice too small for line rate). Then nothing in Z4
   removes the NIC-write or copy-read DRAM touch. Hard blocker; decided by
   phase 0a-R1.
2. **R2 — Dirty landing lines still get written back** even with a small
   recycled set (fabric writes back before the next NIC overwrite, reuse
   order is not FIFO, or the copy destination evicts them). Caps the gain
   at 3 touches (4 with RFO; ~35 GiB/s by the weak 5.2 model).
3. **R2a — Destination RFO.** Adds one DRAM read per byte to every copy
   route; B' becomes 3. Measured in 0b.
4. **R3 — Small rings drop at 400G.** Landing footprint of a few MB covers
   only 40-80 us of line rate; any stall longer than that is loss and TCP
   retransmit, not backpressure. Mitigation depends on socket-level
   backpressure, which on the 64 KiB kernel is weak (4.2 M2).
5. **R4 — Copy bandwidth:** "a core or two" is likely short of 100 GB/s of
   copy; more cores compete for the same cache.
6. **R5 — P3 is a real SUNRPC redesign** (recv and process currently one
   thread) with correctness exposure in record reassembly, loans and
   deferral.
7. **R6 — `tcp_collapse`** when rcvbuf is clamped: an extra hidden copy.
   Likely with rcvbuf values sized in payload bytes on the 64 KiB kernel.
8. **R7 — Measurement confidence:** without a DRAM-traffic PMU with a
   read/write split, G4 and R1 rest on aggregate CMN counts exposed to
   background traffic (descriptors, ACKs, XFS log/metadata, page tables).
9. **R8 — Scope creep toward M4** (memory provider): large cross-subsystem
   work on a driver that lacks `queue_mgmt_ops`.
10. **R9 — David's pool enhancement unseen:** if it exists and differs from
    what this spec assumes, section 2.5 and 4.2 need revisiting.
11. **R10 — Early release use-after-free** if a loan release is placed
    before the whole COMPOUND finishes (4.4).
12. **R11 — Z1 fragility:** relies on fixed header length, full-MSS
    segments and a network-wide MTU 8244; any deviation demotes records to
    buffered (correct but slow).

## 9. Log

- **2026-09-27** — Scoped from David Flynn's proposal (relayed by Mike), on
  the finding that tardis1's NVMe is PRP-only. Four research briefs
  (code paths, hardware, mechanisms, design space) were used; load-bearing
  citations re-checked against `5119bf775071` and the scratchpad xeu
  source. Main conclusion: the copy path already has the proposal's shape
  (one copy, landing freed after copy, O_DIRECT from a page-tiled
  destination on PRP NVMe); the gap is landing-set size and copy
  promptness, and the hardware premise (NIC writes cache-allocated) is
  unverified.
- **2026-09-27** — Adversarial review (ultracode, three lenses: kernel
  correctness, hardware physics, simplicity/alternatives; 20 findings).
  All verified and folded in (section 10). Main changes: Z1 (HDS + MSS
  8192) added as the first probe and a possible zero-copy route on PRP;
  R1 gate moved to an iperf3 small-ring test at the front of phase 0; a
  residency model replaces the 10 us / 1-4 MB targets; RFO carried on
  every copy row; early release restricted to after the whole COMPOUND;
  landing set counted in written bytes with truesize kept separate; probe
  keyed on TCP sequence; deferred skb free and dynamic nfsd threads
  described.
- **2026-10-04** — What the 16 KiB test host run means for this spec
  (`../transplant/README.md`, "16 KiB host"). That host's budget harness
  writes to nvme-loop, which like tardis1's NVMe needs its data in whole
  pages, so it is this spec's case.
  - **A fixed receive region with no loan budget now copies.** Until today
    such a region loaned its pages without limit and could hang the server.
    Now each WRITE is copied out of the region into the nfsd thread's own
    buffer and that buffer is written direct, which is the data path this
    spec proposes (4.1, D1). Nothing has to be configured for it beyond
    the region itself.
  - **Fixing each connection's TCP receive buffer at 128 KiB keeps the
    region nearly empty** (`svc_tcp_rx_rcvbuf`; mechanism M2 in 4.2). With
    16 connections at most 112-128 region pages were in use, about 2 MiB,
    with no packets dropped, and a region of 306 pages was enough. With
    the receive buffers left to grow, data nfsd had not read yet filled
    the region and the card dropped packets. Every WRITE in those cells
    was written direct.
  - **That host cannot say whether so small a region stays in the CPU's
    cache.** It is a VM with virtual network devices. R1 and R2 stand.
  - **Section 3.4 is corrected:** a direct write that starts beyond the end
    of the file does not share the XFS I/O lock.

## 10. Adversarial review (2026-09-27)

Every finding was checked against the cited code (`5119bf775071` and the
scratchpad xeu source) before being folded in. Lens: KC = kernel
correctness, HP = hardware physics, SA = simplicity/alternatives.

| # | lens | sev | finding | verified | disposition |
|---|---|---|---|---|---|
| 1 | KC | major | 4.4 release "after the last data segment" is unsafe: `svcxdr_savemem()` returns raw `rq_arg` pointers (`nfs4xdr.c:158-175`; names `:232-249`, opaques `:218-229`, layoutreturn `:865-875`), which in loan mode point into NIC pages; later COMPOUND ops use them | yes | **fixed**: 2.5 bullet, 4.4 rewritten (release after `pc_func`, or copy aliasing args in loan mode; invariant; KUnit case), D4 row, phase 4, R10 |
| 2 | KC | major | skb free is deferred to the NAPI CPU (`tcp.c:1615-1625`, `skbuff.c:7303-7348`, `skb_defer_max` 128); page_pool direct cache is LIFO and small (8 entries on 64K); FIFO reuse premise unverified | yes (nuance: freed at once when the reader is on `alloc_cpu`, `skbuff.c:7322-7326`) | **fixed**: 2.2 table and text, G2, B row, M6, 0b page_pool recycle stats, 0c `skb_defer_max` and footprint |
| 3 | KC | major | truesize is PAGE_SIZE per buffer (`xeu_drv.c:824`, `:852-857`); 256 KB / 1 MB rcvbuf holds ~4 / ~16 frames on 64K; footprint formula mixes units | yes | **fixed**: 4.2 units paragraph, M1, M2, G2, 0b ratio/`scaling_ratio`, 0c `tcp_rmem` row and footprint in written bytes |
| 4 | KC | minor | dynamic nfsd threads (`nfssvc.c:919-955`), no spawn on wake (`svc.c:691-713`); write-space/slot gate (`svc_xprt.c:488-497`, `svcsock.c:849-856`) | yes | **fixed**: 2.2, 2.4, P1 (min = max), P2 (apply after fixed), 0b tracepoints, label list |
| 5 | KC | minor | copy path is direct only if `pos ≡ 0 mod 4`; otherwise the whole WRITE is buffered (`nfs_dio.c:88-94`, `:297-302`, `:333-340`) | yes | **fixed**: 2.3 retitled and split, section 0, D1 row, 4.1 text, 0b dispositions |
| 6 | KC | minor | rings < 32 stall RX permanently (`xeu_drv.c:944-946`, only other refill at open `:1470`; ethtool accepts 16) | yes | **fixed**: M1, 0a-R1 (256), 0c ring row, phase 1 (`min(32, size/4)`) |
| 7 | KC | minor | xeu is a PCI driver (`xeu_drv.c:3031`, `:3061`, `:3063`, `:3262`, MSI-X `:301`, `:395`); TPH may apply | yes | **fixed**: 5.1, 5.2 hypothesis revised, 5.3 q6, 0a `lspci -vv` |
| 8 | KC | minor | skb-pointer probe misses NIC-side latency and breaks on coalescing; `XEU_POLL` mode | yes | **fixed**: 0b probe keyed on sequence from xeu completion; 0a `XEU_POLL` check; 2.6 |
| 9 | HP | major | 10 us / 1-4 MB not derived; need residency model with C and R_alloc | yes (targets were David's; model absent) | **fixed**: 3.1 model, G1/G2 restated, 1.2 note, 0c knee prediction. C itself **left open** (5.3 q1) |
| 10 | HP | major | 0b cannot decide R1 by construction; 0d last | yes | **fixed**: 0a-R1 gate first with numeric thresholds; 0b age-bucketed residency; old 0d folded into 0a-R1 |
| 11 | HP | major | single `hnf_mc_reqs` sum cannot split R from W; denominator; SPE prefetch confound; EXTLLC; `STALL_BACKEND_MEM` | yes (`arm-cmn.c:944`, `ll_cache.json:1-10`, `stall.json:27-28`) | **fixed**: 0a-R1/0b split counters, both denominators, EXTLLC check (q13), SPE labelled, L2 refill/`LL_CACHE_MISS_RD`, R7. Whether a read/write split is obtainable at all is **left open** (q9) |
| 12 | HP | major | skb-key probe breaks under `tcp_try_coalesce` (`tcp_input.c:5529-5543`); E1 completion coalescing (`dma_drv.c:40-46`) unobservable; no latency budget | yes | **fixed**: 0b probe, 3.1 latency budget, 2.4 instrumentation note, q12. The pre-completion interval is **left open** (not observable from Linux) |
| 13 | HP | minor | "DRAM-bandwidth bound" stated as fact; only stall-bound shown | yes (`PROJECT.md:30-38` has no DRAM bandwidth) | **fixed**: 1.2 relabelled, 5.2 model weakened, G5 conditional, 0b absolute DRAM bandwidth |
| 14 | HP | minor | RFO caveat only on row A | yes | **fixed**: 3.3 RFO column, 4.1 row, G4 "2 (3 if RFO)", 0b RFO check, R2a |
| 15 | SA | major | HDS paired posting + MSS 8192 passes the PRP gate with no copy (`nfs_dio.c:90-99`, patch `:156-180`, 65150 bytes per 64K buffer); spec never considered MSS | yes as code analysis; E1 behaviour HYPOTHESIS | **fixed**: 1.1, 3.2, 3.6 Z1, 4.5 step 1, 0a-Z1 probe, q10, R11 |
| 16 | SA | major | NFS/RDMA dismissed without reason; no verbs driver in 2.0.1 | yes (no `ib_register_device`; RDMA IDs `epu_pci_ids.h:30-39`); svcrdma page-tiled (`svc_rdma_rw.c:771-795`) | **fixed**: 3.2 reason stated, 3.6 Z2, q11. Client RoCE **left open** (Mike/David) |
| 17 | SA | major | R1 decided late by expensive runs; cheap iperf small-ring test first | yes | **fixed**: 0a-R1 (same fix as #10); 0c runs only plausible points |
| 18 | SA | major | footprint metric inconsistent (2.3 MB vs 16 MB); rcvbuf sweep in payload bytes | yes (duplicate of #3 from another lens) | **fixed**: G2 in written bytes, DMA span separate, 0c `tcp_rmem` in truesize, collapse/prune/window recorded |
| 19 | SA | minor | 0c omits `io_cache_write`; P3 trigger should be conditional on `io_cache_write=2` | yes (`nfsd.h:143-150`, `vfs.c:1480-1495`; note mode 2 still holds the thread through the DIO wait, only the fsync goes) | **fixed**: 3.5 note, 0c row {4, 2, 1}, 0c "Partial", 4.5 step 5 |
| 20 | SA | minor | skb-pointer probe (duplicate of #8/#12) | yes | **fixed** with #8/#12 |

Several findings overlap across lenses (#3/#18 truesize, #8/#12/#20 probe,
#10/#17 R1 ordering); each is listed so every lens's item is traceable.

**Totals (20 findings):** fixed 20, rejected 0, left open as a whole 0. Residual open questions
carried from fixed findings: C and the I/O allocation policy (#9, q1-q2);
whether DRAM reads and writes can be split (#11, q9); the E1-side
completion interval (#12, q12); client RoCE / verbs driver (#16, q11);
whether E1 honours paired posting (#15, q10, answered by 0a-Z1).
