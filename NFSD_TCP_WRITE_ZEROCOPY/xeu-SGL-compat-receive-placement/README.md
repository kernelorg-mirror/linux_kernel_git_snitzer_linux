# xeu SGL-compatible receive placement (NVMe SGL sub-project, phase 2)

Phase 2 of [`../NVME_SGL_SUPPORT_PROJECT.md`](../NVME_SGL_SUPPORT_PROJECT.md),
started 2026-09-26. Goal: make the Xsight E1 (`xeu` driver) place received
TCP payload so that NFSD's loaned WRITE pages can go to an SGL-capable NVMe
device as direct I/O. Phase 1 made the kernel accept payloads with mid-page
joints on a device without a virtual boundary; what is left is the NIC side:
every loaned fragment must start and end on the device's DMA alignment,
4 bytes on NVMe.

**Status (2026-10-02):** run on tardis1 (hs.441, 64 KiB pages). The
placement works: with `rx_ip_align=2` every NIC-loaned fragment is 0 mod 4
(was 2 mod 4). WRITEs still could not go direct from loaned pages, for a
reason outside this sub-project: on 64 KiB pages the loan table holds only
20 fragments, so every WRITE falls back to a full copy. Results and cause
in [`TESTING.md`](TESTING.md) section 6. The parent series now sizes the
table for fragments (2026-10-02, not yet run); see the same section.

## tardis1 result: its NVMe is PRP-only

**tardis1's NVMe is PRP-only (Mike, 2026-09-27).** All nine namespaces
report a 4 KiB virtual boundary:

| device | virt_boundary_mask | dma_alignment | logical_block_size |
|---|---|---|---|
| nvme0n1 | 4095 | 3 | 4096 |
| nvme1n1 | 4095 | 3 | 4096 |
| nvme2n1 | 4095 | 3 | 4096 |
| nvme3n1 | 4095 | 3 | 512 |
| nvme4n1 | 4095 | 3 | 4096 |
| nvme5n1 | 4095 | 3 | 4096 |
| nvme6n1 | 4095 | 3 | 4096 |
| nvme7n1 | 4095 | 3 | 4096 |
| nvme8n1 | 4095 | 3 | 512 |

Every 7.1 kernel carries `bc840b21a25a` ("nvme: remove virtual boundary for
sgl capable devices"), and `nvme_pci_get_virt_boundary()` returns 0 exactly
when `nvme_ctrl_sgl_supported()`; `sgl_threshold` does not enter into it.
So `4095` means these controllers do not advertise SGLs (`nvme id-ctrl
/dev/nvmeX | grep -w sgls` should read 0), not a kernel or setting
problem. Consequences:

- On these exports, phase 1 reports `dio_seg_boundary=4096` and the gate
  keeps the joint rule, so loaned WRITEs with mid-page joints are demoted
  to buffered, as before phase 1. That is the correct behaviour; phase 1
  still pays off only on SGL-capable NVMe (or NVMe/TCP, NVMe/FC).
- The phase 2 `rx_ip_align` change cannot make WRITEs direct on tardis1:
  it fixes the 4-byte offsets, but every frame boundary remains a mid-page
  joint that a 4096 boundary refuses. A tardis1 run would confirm the
  placement (`loan-geometry.bt`) but not a disposition change.
- A PRP-only device needs page-tiled payload: joints on 4096. On E1 that
  is header-data split with payload coalescing (the firmware ask in the
  page-loan status document), or SGL-capable NVMe in tardis1.

The patch and test plan stand for an E1 host with SGL-capable NVMe. On
tardis1 as equipped, pass B of `TESTING.md` can only show that the offsets
become 0 mod 4.

## The same misplacement, and the same fix, without the hardware (veth, 2026-10-05)

The problem this sub-project fixes in `xeu` can be reproduced on any host,
with no Xsight card, by the budget harness on its NVMe/TCP storage. So can
the fix:

```
RX_IP_ALIGN=0 STORAGE=sgl PHASE=A CELLS=seq-on bash NFSD_TCP_WRITE_ZEROCOPY/run-budget.sh /var/tmp/out0   # the problem
RX_IP_ALIGN=2 STORAGE=sgl PHASE=A CELLS=seq-on bash NFSD_TCP_WRITE_ZEROCOPY/run-budget.sh /var/tmp/out2   # the fix
```

- **Why it reproduces.** The harness receives over a veth pair with an XDP
  program attached, which makes veth copy each frame into a page-pool page
  at a fixed headroom of `XDP_PACKET_HEADROOM`, 256 bytes
  (`drivers/net/veth.c`). 256 is 0 mod 4, as xeu's 64 is, so after the
  14-byte Ethernet header the IP header and the TCP payload are 2 mod 4,
  exactly as in the table below.
- **What it shows.** NVMe/TCP storage has no virtual boundary and wants
  memory aligned to 4 bytes (`dio_mem_align=4`), like NVMe with SGLs. Every
  loaned WRITE is refused as a direct write for its memory alignment alone
  and is written through the page cache.
- **The fix, in veth.** "veth: add rx_ip_align, extra headroom for frames
  copied for XDP" gives veth the same load-time parameter as the `xeu`
  patch here, for testing only: `rx_ip_align=2` adds 2 bytes of headroom.
  `RX_IP_ALIGN` in `run-budget.sh` sets it (reloading veth), and defaults
  to 2 with `STORAGE=sgl`. It is a private parameter in one more driver,
  not a general answer; there is none in the network stack today.
- **Measured** on the 16 KiB host (aarch64, `7.1.13-19.hs.451`, MTU 9000,
  1 MiB WRITEs, classified by `nfsd_write_dio_split`):

  | `rx_ip_align` | loaned WRITEs | first fragment | written direct |
  |---|---|---|---|
  | 0 | 512 | 2 mod 4 (offsets 822, 7834, 8490, 8522, 8554, ...) | 0 (`mem_misaligned`) |
  | 2 | 512 | 0 mod 4 | 512 |

  The same WRITEs copied into nfsd's own pages (budget 0) went direct
  either way.
- **How to read it.** `cells.tsv` ends with `wr_direct`, `wr_dontcache` and
  `wr_buffered`, the number of segments nfsd issued each way. A loaned cell
  with `wr_direct` 0 on `STORAGE=sgl` is this problem. For the offsets,
  enable the `nfsd:nfsd_write_dio_split` tracepoint and read `mem_offset`
  and `disposition`.
- **What differs from the card.** veth copies a whole TCP send, up to
  64 KiB, into one head page and then whole pages, where the card delivers
  one frame per buffer. So this does not exercise the continuation-buffer
  case below, nor one fragment per frame.

## Why a 2-byte change is enough (for frames that fit one buffer)

The driver posts every RX buffer identically, at
`dst_addr = dma_addr + XEU_RX_PAGE_HEADROOM`, with
`XEU_RX_PAGE_HEADROOM = NET_SKB_PAD + NET_IP_ALIGN` (`xeu_drv.h:56`), and the
receive path reserves the same headroom (`skb_reserve()`, and
`skb_add_rx_frag()` for continuation buffers). Measured on this arm64
kernel (a probe module, 2026-09-26): `NET_SKB_PAD=64`, `NET_IP_ALIGN=0`,
`SKB_DATA_ALIGN(sizeof(struct skb_shared_info))=320`. So:

| | offset in page | mod 4 |
|---|---|---|
| frame start | 64 | 0 |
| IP header (after 14-byte Ethernet) | 78 | 2 |
| TCP payload, no TCP options (+20 IP +20 TCP) | 118 | 2 |
| TCP payload, timestamps (+20 IP +32 TCP) | 130 | 2 |

IPv4 (with options), IPv6 and TCP (with options) headers are always
multiples of 4 bytes, and a VLAN tag adds 4, so **the payload is 2 mod 4 for
every plain Ethernet TCP frame, and 2 more bytes of headroom make it 0 mod 4
for every one** — the classic `NET_IP_ALIGN = 2` placement, which aligns the
IP header too. Fragment lengths are TCP segment payload sizes: MSS 1448
(MTU 1500 with timestamps) and 8948 (MTU 9000) are both multiples of 4.

The joints between fragments are mid-page (one fragment per frame), which a
device with a virtual boundary cannot take in one bio — that is exactly what
phase 1 lets an SGL device do.

## The change

[`0001-xeu-add-rx_ip_align-to-4-byte-align-received-IP-head.patch`](0001-xeu-add-rx_ip_align-to-4-byte-align-received-IP-head.patch)
against `kmod-xsight-2.0.1` (tarball-relative `.orig` paths, applies at
`-p0` from the extracted tarball root — the level `xsight.spec`'s
`%autosetup -p0` leaves the build at, so it drops in as another `PatchN:`).

- New load-time module parameter `rx_ip_align`: `0` (default) posts exactly
  what the driver always posted; `2` adds two bytes of RX headroom. Other
  values are ignored with a warning. Read-only after load, because a buffer
  must be parsed with the headroom it was posted with.
- Every use of the RX headroom and buffer data size (posting in the refill
  path, `skb_reserve()`, the linear and fragment lengths,
  `skb_add_rx_frag()`, `max_rx_buf_per_pkt`) goes through two helpers.
- **Limit:** a frame larger than one buffer continues in further buffers
  posted at the same headroom, so its continuation data stays 2 mod 4. The
  driver warns when the MTU makes a frame span several buffers. One buffer
  holds `PAGE_SIZE - 66 - 320` bytes: **3710 on 4 KiB pages** (MTU 1500
  fits, MTU 9000 does not) and 65150 on 64 KiB pages (any supported MTU
  fits).

### 4 KiB arm64 with jumbo frames: continuation buffers stay 2 mod 4

| Kernel | MTU 1500 (1514-byte frame) | MTU 9000 (9014-byte frame) |
|---|---|---|
| arm64, 4 KiB pages | 1 buffer: all payload 0 mod 4 | 3 buffers: continuation payload 2 mod 4 |
| arm64, 64 KiB pages | 1 buffer: all payload 0 mod 4 | 1 buffer: all payload 0 mod 4 |

With `rx_ip_align=2` on a 4 KiB kernel at MTU 9000, the first buffer's
payload runs from 132 to 3776 (both 0 mod 4), but the frame continues into
two more buffers posted at the same 66-byte headroom, so their payload
fragments start at 66 = **2 mod 4**. Every jumbo frame contributes a
misaligned fragment, so nearly every 1 MiB WRITE fails the gate and is
demoted (`mem_misaligned`) — correct data, just not direct. Only a NIC that
pads a frame's first buffer alone could fix that (Xsight question 2).

**Therefore test on tardis1's E1 with the 64 KiB `PAGE_SIZE` arm64
kernel:** it is the only configuration where jumbo frames — what a storage
network runs — land entirely 4-byte aligned. A 4 KiB kernel is useful only
at MTU 1500 (or as a control showing jumbo WRITEs demoted).

Build check (2026-09-26, against `7.1.13-14.hs.440.loanpages`): builds with
0 warnings, and `W=1` adds nothing over the unmodified driver (11
pre-existing warnings in both, none in `xeu_drv.c`). The IPsec offload part
of the driver needs `CONFIG_XFRM_OFFLOAD`, which this host's config lacks,
so the check used `XEU_IPSEC_ENABLE=n`; the unmodified driver fails the
same way without it. `xsl-core` needs the spec's own
`remove_newline_from_xsl_rm_version_h.patch`.

## Relation to the paired-posting experiment

That experiment, dropped unrun on 2026-11-01 (on the branch
`.pre-drop-hds`), approximates header-data split (a short header buffer, then the payload at page offset 0) so payload
lands **page-aligned** — the placement a device *with* a virtual boundary
needs. This sub-project targets devices *without* one, which only need
4-byte alignment, so no buffer pairing, no header-length tuning and no
change to the buffer count per frame. The two are independent; this one is
far smaller and does not depend on how E1 treats `RX_SR_BD_EOF`.

## What it depends on

- A kernel carrying the page-loan series, the merge top-up fix and NVMe SGL
  phase 1 (the gate's relaxed joint rule plus the attribute plumbing). For
  tardis1 that is the Hammerspace branch
  `kernel-7.1/hs-7.1.13-14.NFSD_TCP_WRITE_ZEROCOPY`: the whole of
  `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY` through `3350af53be4b` (the
  series, the folded fix `dd2e42c5f547` and all of SGL phase 1) with the
  Hammerspace DP commits on top. A kernel is being built from it
  (2026-09-26); tardis1 needs the 64 KiB page build (see above).
- An export whose device reports no virtual boundary: `statx-dio` on a file
  shows `dio_seg_boundary=0` (for NVMe PCIe, a controller advertising SGLs).
  On such a controller the NVMe driver forces SGLs for any request with a
  gap (`nvme_pci_use_sgls()`), whatever `sgl_threshold` says.

## A setting the network stack owns: one path forward (designed, not built)

`rx_ip_align` now exists twice, as a private module parameter in `xeu` and
in veth. Every other driver whose card should feed loaned WRITEs to NVMe
would need a third, a fourth. This section records what a single,
driver-neutral setting could look like, with veth as its first user. It
is a design only: nothing here is implemented.

**Mike's read (2026-10-05): this is unlikely to be acceptable upstream.**
It is written down as one path forward, not as the plan.

### What can and cannot be neutral

Where a frame lands is decided by the address the driver hands the card
for each receive buffer. The stack never sees that address, so no change
in the core alone can move the payload: **every driver that takes part
has to add the pad where it posts its buffers.** What the core can own is
everything else: the setting, how it is configured and reported, its
validation, and the arithmetic. A driver then opts in with a flag and a
couple of lines, instead of inventing a parameter.

### The pieces

1. **Configuration, next to header-data split.** The stack already has
   per-device receive settings that drivers opt into: `tcp-data-split` and
   `hds-thresh`, set with `ethtool -G`, carried in `struct netdev_config`
   (`dev->cfg`, with `dev->cfg_pending` while a change is applied;
   `include/net/netdev_queues.h`, `net/ethtool/rings.c`). The new setting
   would sit beside them:
   - a field in `struct netdev_config`, say `rx_ip_pad` (0 to 3 bytes);
   - a ring attribute, `ETHTOOL_A_RINGS_RX_IP_PAD`, so it is
     `ethtool -G <dev> rx-ip-pad 2` and shows in `ethtool -g`;
   - a capability flag, `ETHTOOL_RING_USE_RX_IP_PAD`, in the driver's
     `supported_ring_params`. The core refuses the setting on a device
     without it, as it does for the others.
2. **One helper for the arithmetic**, for example
   `netdev_rx_headroom(dev, base)`, returning the driver's own headroom
   plus the configured pad. A driver calls it in the two places that must
   agree: where it posts a buffer, and where it builds the skb or XDP
   buffer over it.
3. **The driver's side of the bargain**, which is the contract the flag
   stands for: the pad goes in front of the frame in every buffer that
   starts a frame; the driver's XDP headroom and its largest frame account
   for it; and changing it re-posts the receive ring the way a ring-size
   change does.
4. **Reporting.** `ethtool -g` shows the value, so a test, or NFSD's
   documentation, can say what a host needs without naming a driver.

### veth as the first user

veth is the natural first user because it has no hardware: the whole
change is visible and testable anywhere.

- Set `ETHTOOL_RING_USE_RX_IP_PAD` and give veth the ring operations it
  lacks today (it has none).
- In `veth_convert_skb_to_xdp_buff()` replace the headroom passed to
  `skb_pp_cow_data()`, and in `veth_xdp_set()` the headroom in the MTU
  limit, with the helper. That is the whole of today's `rx_ip_align`
  commit, reading the device's setting where it now reads a module
  parameter. It also becomes per device and changeable without reloading
  the module.
- `run-budget.sh` would then run `ethtool -G rxsrv rx-ip-pad 2` where it
  now reloads veth, and the `RX_IP_ALIGN=0` / `2` pair above is the test.

`xeu` would be the second user: `XEU_RX_PAGE_HEADROOM` becomes the helper,
and the patch in this directory shrinks to that and the flag.

### Why upstream may well say no

- **It asks every driver for something only one consumer needs.** The
  consumer is direct I/O from loaned receive pages, which is itself not
  upstream.
- **`NET_IP_ALIGN` already names this idea**, and arm64 and x86 set it to
  0 on purpose: an unaligned IP header is free there, and a receive buffer
  that starts off a 4-byte boundary costs some DMA engines. Bringing it
  back as a runtime setting would have to answer that.
- **A pad is not the direction receive placement is going.** Header-data
  split, where the card supports it, puts the payload at the start of a
  page. That is aligned to 4 bytes and to the page, which a pad never
  gives, and it is already a neutral, opt-in setting.
- **It only works for frames that fit one buffer.** Continuation buffers
  are not padded ("4 KiB arm64 with jumbo frames" above), and neither is
  anything inside a tunnel.

### What else there is

- **Header-data split** where the hardware has it: no new interface at
  all. For the Xsight card this is a question for Xsight.
- **Keep the private parameters.** Two exist. It is the least work and
  needs nobody's agreement, at the price of one per driver.
- **Do it in the pool, not the driver.** Page pools already carry an
  `offset` that many drivers use as their headroom
  (`struct page_pool_params`). A pad there reaches those drivers with no
  new ethtool attribute, but only those, and it is a less visible place to
  put policy.
- **Not need it.** Copying out of a small receive region into nfsd's own
  pages gives aligned memory whatever the card does
  (`../on-die-receive-copy/SPEC.md`). The pad matters only where the loan
  itself is to be written direct.

## Questions for Xsight

1. Does the E1 RX DMA engine accept a `dst_addr` that is 2 mod 4, at full
   rate (no alignment requirement or penalty on the descriptor address)?
2. Can the RX path insert a pad (e.g. 2 bytes) before the frame in the
   **first** buffer only? That would 4-byte align payload in continuation
   buffers too, and so jumbo frames on 4 KiB pages.
3. Encapsulated traffic (VXLAN/Geneve inner Ethernet) shifts the payload by
   14 and breaks the rule; is it in scope for NFS storage traffic on E1?
