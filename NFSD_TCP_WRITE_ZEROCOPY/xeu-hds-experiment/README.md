# The xeu paired-RX-posting experiment

A software-only attempt to make the Xsight E1 hand NFSD **page-aligned**
payload, so the page-loan series can actually engage on tardis1-class
hardware. No firmware change, no kernel change — one patch to the
out-of-tree `xeu` driver, defaulting to today's behavior.

Carried here because it is this project's experiment and its result
decides what we ask Xsight for. It is **not** part of the kernel series.

## Why

The loan series eliminates the receive copy only if the loaned pages can
then be written with O_DIRECT. On E1 they cannot: the driver posts one
uniformly shaped RX buffer per frame (a whole page at
`dst_addr = dma_addr + XEU_RX_PAGE_HEADROOM`), so headers and the start
of payload share the first buffer and payload begins at
`headroom + parsed_header_length` — e.g. `64 + 54 = 118`, and
`118 & 3 == 2`, failing even the 4-byte DMA alignment gate. Every WRITE
is demoted to buffered, the copy comes back on the writeback side, and
the DRAM-touch count never drops. See `../PROJECT.md` →
"Xsight E1 RX capabilities" for the source-pinned capability review.

That geometry is the **driver's choice**, not a hardware limit. The RX
submit descriptor carries a per-buffer address and length (up to 1 MB),
the engine already spreads one frame over several buffers, and receive
already walks multi-buffer frames. So we can try header-data split
without Xsight: post buffers in **pairs** — a short header buffer at the
usual headroom, then a payload buffer at **offset 0** of its own page.

## What the patch adds

| Knob | Default | Meaning |
|---|---|---|
| `hds_hdr_len` | `0` (off) | Header-buffer length in bytes. Non-zero enables paired posting. Load-time only. |
| `hds_hdr_eof` | `N` | Set `RX_SR_BD_EOF` on the header buffer too. `N` clears it, reading the bit as "more buffers follow". |

With `hds_hdr_len=0` the driver posts exactly what it always posted —
the patch is inert until asked.

Set `hds_hdr_len` to the stream's **exact** L2+L3+L4 header size: 54 for
plain Ethernet/IPv4/TCP, 66 with TCP timestamps. Confirm with
`tcpdump -i <dev> -c1 -v` on the real traffic before trusting a number.

## Build and load

Paths in the patch are relative to the extracted tarball root and use the
`.orig` convention of the vendor's own spec patches, so it applies at
**`-p0`** from `kmod-xsight-<ver>/` — exactly where
`%autosetup -n kmod-xsight-%{kmodver} -p0` leaves you. It can therefore
be dropped into `xsight.spec` as another `PatchN:` unchanged.

```sh
# from the extracted tarball root (holds xeu-esdk-v2.0.1/, xsl-core-esdk-v2.0.1/, ...)
patch -p0 < 0001-xeu-EXPERIMENTAL-paired-RX-posting-to-land-payload-p.patch

cd xeu-esdk-v2.0.1
make KERNELDIR=/lib/modules/$(uname -r)/build

modprobe -r xeu
insmod ./xeu.ko hds_hdr_len=54          # add hds_hdr_eof=Y for the other EOF reading
```

The driver logs its decision per ring at open:

```
xeu: EXPERIMENTAL paired RX posting active: 54-byte header buffer (EOF clear) + page-aligned payload buffer
```

and refuses the pairing — with the reason — if the MTU is too large for
a header+payload pair to hold a full frame (roughly MTU ≤ 3.8 KB on a 4K
page kernel). Jumbo frames need the payload buffer to span multiple
pages, which this patch does not do.

## The measurement

One `netdev_info` per ring, on its first frame, is the whole point:

```
xeu: paired RX posting: ring 0 frame_len=1514 num_desc=2 posted_hdr=54 parsed_hdr(L0-L4)=54 linear=54 payload_frag_off=0
```

Read it as follows.

- **`num_desc=2`** — the engine used the pair. `num_desc=1` means it
  ignored the second buffer (frame truncated into the header buffer):
  stop, this reading of `RX_SR_BD_EOF` is wrong; retry with
  `hds_hdr_eof=Y`.
- **`payload_frag_off=0`** — payload landed page-aligned. This is the
  result the experiment is after.
- **`posted_hdr` vs `parsed_hdr`** — equal means the two possible engine
  semantics coincide and the assembled skb is unambiguous. If
  `parsed_hdr` is consistently different from what you posted, re-set
  `hds_hdr_len` to the parsed value and reload.

Then check the data actually survives, because a wrong assumption about
where the engine broke the frame corrupts skbs rather than just
misplacing them:

```sh
ping -c5 <peer>; curl -sS -o /dev/null <peer>    # basic liveness
ethtool -S <dev> | grep -iE 'csum|drop'          # must not climb
```

Finally, observe the loan with the rig's own instrument
(`../zcstat.sh`), which reports bvecs per large receive:

```sh
../zcstat.sh 5
```

## What a success looks like — and what it does not fix

Success is narrow and worth stating plainly: **payload arriving at page
offset 0**, proving the placement half of header-data split is reachable
in software on this hardware.

It will **not** restore direct I/O, and `zcstat.sh` should still show
`dirW=0`. Payload still lands one frame per page, so each inter-frame
joint sits at a payload position that is not a multiple of the logical
block size (1448 is not), and NFSD's admission gate correctly demotes
the write. Closing that needs **payload coalescing across frames** —
many frames' payload packed contiguously into one page. The packet
processor already maintains per-flow GRO aggregation state (1024
contexts, `GRO_ADD_TO_AGG`, unread by this driver), which is the
specific capability to ask Xsight about.

So the experiment's value is evidence, not throughput: it converts
"please add header-data split" into "your descriptor path already places
payload where we need it — here is it working; what we still need is
aggregation into page-sized payload buffers."

## Status

**Untested on hardware.** Built warning-free against Linux 7.1.8
(`W=1` introduces nothing over the unmodified driver) and the patch
applies cleanly to a pristine `xeu-esdk-v2.0.1`, but no E1 was available
to run it. Everything above about engine behavior is a hypothesis the
run is designed to confirm or kill.

Risks to weigh before loading on a shared machine: a wrong `EOF` reading
could truncate every frame (loud, immediate, recovered by reloading
without `hds_hdr_len`); paired posting consumes two pages per frame, so
effective ring depth halves.
