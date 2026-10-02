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
   **tardis1 (2026-09-27): all nine namespaces show `4095` — PRP-only.**
   This gate fails there; see `README.md` → "tardis1 result".
3. **Page size and MTU — use the 64 KiB arm64 kernel.** A frame must fit
   one RX buffer: 3710 bytes on a 4 KiB kernel (MTU 1500 only), 65150 on a
   64 KiB kernel (any MTU, including 9000). On 4 KiB with jumbo frames every
   continuation buffer is posted at the same 66-byte headroom, so its payload
   is 2 mod 4 and nearly every WRITE is demoted (see `README.md`); the driver
   logs `rx_ip_align: MTU ... spans N RX buffers`. Run the main passes on the
   64 KiB kernel at MTU 9000; a 4 KiB kernel is only an optional control.
4. **Traffic shape:** plain Ethernet (optionally VLAN) TCP, no tunnel.

### Checking the export NVMe

    findmnt -no SOURCE /path/to/export        # use the parent namespace of a partition
    d=/sys/block/nvme0n1/queue
    cat $d/virt_boundary_mask $d/dma_alignment $d/logical_block_size
    nvme id-ctrl /dev/nvme0 | grep -w sgls    # sysfs does not expose this
    cat /sys/module/nvme/parameters/sgl_threshold

`virt_boundary_mask` must be `0` (`4095` = PRP only: stop), `dma_alignment`
`3`. `sgls` low 2 bits non-zero (`1` = supported, `2` = supported with
dword alignment; `0` = no SGLs, and the mask is then `4095`). Leave
`sgl_threshold` at its default `32768`: it only affects gap-free requests,
so loaned WRITEs get SGLs regardless, but `0` disables SGLs. On dm/md,
check the top device's `queue/` too (it stacks the strictest limit). For
comparison, this qualification box's `nvme0n1` shows `4095` and `sgls : 0`.

## 2. Build and load

Apply `0001-xeu-add-rx_ip_align-...patch` at `-p0` from the extracted
`kmod-xsight-2.0.1` root (or add it to `xsight.spec` as a `PatchN:`), build
for the running kernel, and keep the unmodified `xeu.ko` for the deadman
switch. `modinfo -p xeu.ko | grep rx_ip_align` confirms the parameter.

Setting it persistently (it is read-only after load, so it takes effect at
the next load of `xeu`):

- **modprobe.d:** `echo 'options xeu rx_ip_align=2' > /etc/modprobe.d/xeu.conf`
  (not `/etc/modules-load.d/`, which takes no parameters); if `xeu` is in
  the initramfs, `dracut -f --kver <release>`.
- **Kernel command line:**
  `grubby --update-kernel=/boot/vmlinuz-<release> --args="xeu.rx_ip_align=2"`
  (`--remove-args="xeu.rx_ip_align"` to undo); `modprobe` applies
  `module.param=` entries from `/proc/cmdline` to loadable modules.

For the A/B, prefer the command line on one grub entry and keep the
known-good entry without it. Verify with
`cat /sys/module/xeu/parameters/rx_ip_align`.

### Built for `7.1.13-14.hs.441.loanpages_SGL` (2026-10-01)

Built through kernel-rpmtree as `KMOD-DIR-DEVEL.md` describes, against the
el9 kernel RPMs Nexus holds in repository `kernel`, path
`el9/kernel-7.1/hs-7.1.13-14.NFSD_TCP_WRITE_ZEROCOPY/441/aarch64/Packages/`
(staged on the build host in `/root/hs/7.1.13/RPMS-hs441/aarch64/`). The
patch went in as `Patch2:`, and the build used `-D "dist .rxipalign.el9"`
so the packages cannot be mistaken for the stock ones of the same kernel:

    /root/hs/7.1.13/RPMS-ubi9-hs441-rxipalign/aarch64/
      kmod-64k-xsight-2.0.1_7.1.13_14.hs.441.loanpages_SGL-2.rxipalign.el9.aarch64.rpm
      kmod-xsight-2.0.1_7.1.13_14.hs.441.loanpages_SGL-2.rxipalign.el9.aarch64.rpm
      (+ -debuginfo for each)

Checked: 64K `xeu.ko` vermagic `7.1.13-14.hs.441.loanpages_SGL.el9.aarch64+64k`
and `parm: rx_ip_align` (the 4K one too), `Requires: kernel-64k-core`,
`depmod -a "...+64k"` scriptlet. The stock package for the same kernel
(`...-2.el9...`, no `rx_ip_align`) was fetched beside the kernel RPMs for
the A/B and to revert.

Install on tardis1 by **replacing** the stock package, not adding beside it.
The kmod provides `installonlypkg(kernel-module)`, so `dnf install` would try
to keep both and fail on the shared `xeu.ko` path. Plain `rpm -Uvh` upgrades
by name (`2.rxipalign.el9` sorts after `2.el9`); revert with
`rpm -Uvh --oldpackage` on the stock RPM. Reload `xeu` or reboot afterwards.

The patch prints nothing when `rx_ip_align` takes effect, only warnings for
a bad value or an MTU that spans RX buffers, so check positively:
`rpm -qf $(modinfo -F filename xeu)` names the `.rxipalign` package and
`/sys/module/xeu/parameters/rx_ip_align` reads `2`.

Build-host gotcha: the build host was running a tree-built
`6.12.93-7.hs.441.el9.aarch64+` with no `veth` module, so Docker's bridge
network could not start the build container. The build ran with a `docker`
wrapper first on `PATH` that adds `--network host` to `docker run`.

## 3. Two passes, same workload

On the 64 KiB kernel at MTU 9000, drive steady NFS WRITE load (O_DIRECT,
1 MiB records) from a client to the
export, with loans on (`svc_tcp_rx_loan_pages=Y`) and
`/sys/kernel/debug/nfsd/io_cache_write=2`. For each pass, collect for the
same interval. `trace-loans.sh LABEL [SECONDS]` (this directory) does the
first two below and saves them with the run's state (kernel, xeu package,
`rx_ip_align`, loan switch, `io_cache_write`, MTU) under
`out-LABEL-<timestamp>/`:

    ./trace-loans.sh passA-rx0 60
    ./trace-loans.sh passB-rx2 60

On `hs.441` the probe target is sound: `svc_tcp_rx_append` is a real local
function in the 64K `sunrpc.ko`, both `svc_tcp_rx_append_borrowed()` and
`_copied()` call it, and the `nfsd_write_{direct,vector,dontcache}`
tracepoints exist.

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

On tardis1 the NVMe is PRP-only (see `README.md`), so pass B can only show
the offsets moving to 0 mod 4; dispositions stay demoted and
`@ends_mid_page[1]` stays high (it tests against 4 KiB, the PRP boundary,
not the 64 KiB page). Judge that run by `@offset_mod4` for `@page_pool[1]`.

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
