# Test plan — xeu `rx_ip_align` on tardis1 (E1)

Goal: show that with `rx_ip_align=2` the E1 hands NFSD loaned WRITE payload
whose fragments are all 4-byte aligned, and that on an SGL-capable NVMe
export those WRITEs then go direct — with the data intact. Not yet run.

## 0. Access hazard

Reloading `xeu` takes the NIC down. Follow section 0 of
[`../xeu-hds-experiment/TESTING.md`](../xeu-hds-experiment/TESTING.md): work
from a console or out-of-band path, and stage the deadman switch that
reloads the known-good module before every load.

## 1. Gates — check these first

1. **Kernel:** `uname -r`; the running kernel must carry the page-loan
   series, the merge top-up fix and NVMe SGL phase 1 (see `README.md` →
   "What it depends on"). Check the loan switch exists
   (`/sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages`) and that
   `statx-dio` (`../statx-dio.c`) knows `STATX_DIO_SEG_BOUNDARY` on this
   kernel (exit status 1 means the kernel does not report it).
2. **Export device without a virtual boundary:** for the export's NVMe
   namespace, `cat /sys/block/nvmeXnY/queue/virt_boundary_mask` must be `0`
   (and `nvme id-ctrl /dev/nvmeX | grep sgls` non-zero); `statx-dio` on a
   file in the export must show `dio_mem_align=4 dio_seg_boundary=0`. If it
   shows `4096`, stop: this device cannot take the placement, whatever the
   NIC does.
3. **Page size and MTU — use the 64 KiB arm64 kernel.** A frame must fit
   one RX buffer: 3710 bytes on a 4 KiB kernel (MTU 1500 only), 65150 on a
   64 KiB kernel (any MTU, including 9000). On 4 KiB with jumbo frames every
   continuation buffer is posted at the same 66-byte headroom, so its payload
   is 2 mod 4 and nearly every WRITE is demoted (see `README.md`); the driver
   logs `rx_ip_align: MTU ... spans N RX buffers`. Run the main passes on the
   64 KiB kernel at MTU 9000; a 4 KiB kernel is only an optional control.
4. **Traffic shape:** plain Ethernet (optionally VLAN) TCP, no tunnel.

## 2. Build and load

Apply `0001-xeu-add-rx_ip_align-...patch` at `-p0` from the extracted
`kmod-xsight-2.0.1` root (or add it to `xsight.spec` as a `PatchN:`), build
for the running kernel, and keep the unmodified `xeu.ko` for the deadman
switch. `modinfo -p xeu.ko | grep rx_ip_align` confirms the parameter.

## 3. Two passes, same workload

On the 64 KiB kernel at MTU 9000, drive steady NFS WRITE load (O_DIRECT,
1 MiB records) from a client to the
export, with loans on (`svc_tcp_rx_loan_pages=Y`) and
`/sys/kernel/debug/nfsd/io_cache_write=2`. For each pass, collect for the
same interval:

- `bpftrace loan-geometry.bt` (this directory): per loaned fragment, page
  offset mod 4, length mod 4, whether it ends mid-page, and page_pool
  origin. Only `@page_pool[1]` fragments are NIC loans.
- the `nfsd:nfsd_write_dio_split` dispositions (`direct` vs
  `mem_misaligned`) and `nfsd_write_direct`/`nfsd_write_vector` counts, e.g.
  with `../zcstat.sh`;
- `bpftrace ../split_einval.bt`: there must be no `-EINVAL`.

**Pass A — `rx_ip_align=0`** (baseline): expect NIC-loan fragments with
offset 2 mod 4 and WRITEs demoted (`mem_misaligned`).

**Pass B — `rx_ip_align=2`:** reload with the parameter, check
`dmesg` for no `spans N RX buffers` warning, repeat.

Then verify data: write known content from the client and `cmp` it on the
server (as `../run-rig-cmp.sh` does locally) for both passes.

## 4. Reading the result

| Pass B NIC-loan offsets | Pass B dispositions | Meaning |
|---|---|---|
| all 0 mod 4 | mostly `direct` | placement works; the loan series pays off on E1 with an SGL NVMe export |
| all 0 mod 4 | still `mem_misaligned` | lengths or another geometry fail the gate: check `@len_mod4`, and the NVMe `virt_boundary_mask` / `statx-dio` gate again |
| still 2 mod 4 | — | the parameter did not take (check `modinfo`, `/sys/module/xeu/parameters/rx_ip_align`) or the E1 ignores the low address bits (Xsight question 1) |
| any `-EINVAL`, or `cmp` mismatch | — | stop and capture: a correctness problem |

Note: `loan-geometry.bt` counts every loaned fragment, including RPC header
and small-record fragments, not only WRITE payload; judge the payload by the
`@page_pool[1]` totals and the dispositions together. On loopback (no
page_pool) a few percent of fragments are misaligned while every 1 MiB WRITE
still goes direct.

## 5. What to record

In this file: the kernel release, NIC firmware/driver version, page size,
MTU, the export's `virt_boundary_mask`, `sgls` and `statx-dio` output, both
passes' histograms and disposition counts, the `split_einval.bt` result and
the `cmp` result; then the answer to Xsight question 1 that the run implies.
