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

## 6. Results — tardis1, 2026-10-02

Raw outputs: [`results-tardis1-2026-10-02/`](results-tardis1-2026-10-02/).
Not recorded: NIC firmware version, the `split_einval.bt` result and a `cmp`
of the written data.

**State, every run:** kernel `7.1.13-14.hs.441.loanpages_SGL.el9.aarch64+64k`
(64 KiB pages), `kmod-64k-xsight-...-2.rxipalign.el9`, loans on,
`io_cache_write=4`, MTU 9000 on both E1 ports. Export `/hs/nvme6`: XFS
directly on `nvme6n1`, `virt_boundary_mask` 4095 (PRP-only). Client: pNFS
flexfiles through the HS metadata server (no striping), `fio --direct=1
--bs=1MiB --iodepth=1 --ioengine=libaio --numjobs=32 --rw=write` over 8
hsvol mounts.

### Placement: works

`trace-loans.sh`, 60 s each; pass B (10:53) ran before pass A (10:57).

| | pass A, `rx_ip_align=0` | pass B, `rx_ip_align=2` |
|---|---|---|
| NIC-loan fragments (`@page_pool[1]`) | 23,044,884 | 21,787,252 |
| offset 0 mod 4 | 0 | 21,787,252 (100%) |
| offset 2 mod 4 | 23,044,884 (100%) | 0 |
| length 0 mod 4 | 100% | 100% |
| ends off a 4 KiB boundary | 100% | 100% (6 exceptions) |
| `rx_ip_align` warnings in dmesg | none | none |
| loaned bytes in 60 s | 400.5 GB | 376.9 GB |

The E1 accepts a receive `dst_addr` that is 2 mod 4 (Xsight question 1,
first half). Pass B moved ~6% fewer bytes, but that is one run each, back
to back: no basis yet for a rate penalty.

### Dispositions: unchanged, and not for the reason expected

Per-segment counts were the same shape in both passes (pass A 1,088,516
direct / 2,207,739 dontcache; pass B 1,028,235 / 2,101,056). A follow-up
breakdown with `rx_ip_align=2` (`followup-dio-split-and-rx-bytes.txt`, 60 s):

- 612,491 WRITEs, every one exactly 1,000,000 bytes; 1 in 64 offsets is
  4 KiB-aligned (multiples of 1,000,000). So each WRITE splits into a
  buffered head, a direct middle and a buffered tail: the 1:2 segment
  ratio. fio issues 1 MiB writes; where the 1,000,000-byte chunking comes
  from (client or metadata server) is open and outside this sub-project.
- Disposition: 607,940 direct (99.3%), 4,551 memory-misaligned (0.7%).
- Borrowed bytes: 212.5 GB, i.e. **346,882 bytes per WRITE**. Copy-mode
  bytes do not pass through `svc_tcp_rx_append`, so copied bytes are not in
  this count.
- NIC-loan fragment sizes: mostly 8-16 KB (one MTU-9000 segment), with
  ~14% at 32-64 KB.

### The cause: the loan table is sized in pages

`svc_tcp_rx_state_alloc()` sizes each record's loan bvec table at
`svc_serv_maxpages()` = `DIV_ROUND_UP(sv_max_mesg, PAGE_SIZE) + 3`
entries: 260 on a 4 KiB kernel, **20 on a 64 KiB kernel**. Every loaned
fragment takes one entry whatever its length, so on 64 KiB pages a WRITE
fills the table after about 20 fragments (~347 KB, matching the measured
borrowed bytes per WRITE). The receive then hits the capacity fallback
(`SVC_TCP_RX_REASON_CAPACITY`): `svc_tcp_rx_materialize()` copies the
record into the page-tiled `rq_pages` arena, which goes direct on PRP NVMe.
Confirmed (`followup-materialize.txt`, 30 s): **478,415
`svc_tcp_rx_materialize` calls against 465,720 WRITEs**, about one per
WRITE.

So on the 64 KiB kernel a loaned WRITE never completes as a loan: it
borrows about a third of the record, then copies all of it. The receive
placement cannot change that, and neither can SGL NVMe. On this hardware
the loan series gives no zero-copy at all on 64 KiB pages until the table
is sized for fragments rather than pages. That is a change to the parent
series, not to this sub-project; not made.
