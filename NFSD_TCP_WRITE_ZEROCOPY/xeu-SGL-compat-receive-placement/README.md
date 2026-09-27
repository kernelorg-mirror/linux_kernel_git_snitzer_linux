# xeu SGL-compatible receive placement (NVMe SGL sub-project, phase 2)

Phase 2 of [`../NVME_SGL_SUPPORT_PROJECT.md`](../NVME_SGL_SUPPORT_PROJECT.md),
started 2026-09-26. Goal: make the Xsight E1 (`xeu` driver) place received
TCP payload so that NFSD's loaned WRITE pages can go to an SGL-capable NVMe
device as direct I/O. Phase 1 made the kernel accept payloads with mid-page
joints on a device without a virtual boundary; what is left is the NIC side:
every loaned fragment must start and end on the device's DMA alignment,
4 bytes on NVMe.

**Status:** driver change written and build-checked, not run on hardware.
Test procedure for tardis1 in [`TESTING.md`](TESTING.md).

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

[`../xeu-hds-experiment/`](../xeu-hds-experiment/) approximates header-data
split (a short header buffer, then the payload at page offset 0) so payload
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

## Questions for Xsight

1. Does the E1 RX DMA engine accept a `dst_addr` that is 2 mod 4, at full
   rate (no alignment requirement or penalty on the descriptor address)?
2. Can the RX path insert a pad (e.g. 2 bytes) before the frame in the
   **first** buffer only? That would 4-byte align payload in continuation
   buffers too, and so jumbo frames on 4 KiB pages.
3. Encapsulated traffic (VXLAN/Geneve inner Ethernet) shifts the payload by
   14 and breaks the rule; is it in scope for NFS storage traffic on E1?
