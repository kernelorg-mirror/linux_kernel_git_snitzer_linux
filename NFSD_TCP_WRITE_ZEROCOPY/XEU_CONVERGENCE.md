# Converging the region and budget testing on the Xsight driver

Scoped on tardis1 on 2026-10-07 (build 458, 64 KiB pages, branch
`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`). The aim for the next 24
hours is to run the copy-against-loan cells on the `xeu` driver and
real clients, where every result so far is of veth and memory-backed
storage.

**The goal (Mike, 2026-10-07):** converge the testing on

- the `xeu` driver as the receiving device,
- an NVMe/TCP test backend, only to cover storage that takes arbitrary
  pieces (SGLs),
- one of the local NVMe devices, to cover storage that needs whole
  pages (PRP).

Nothing on the card or the drives was changed in scoping this. The work
items are in section 5; the decisions that block some of them are in
section 4.

## 0. Rules for this rig (Mike, 2026-10-07)

- **Generate load only against mounts of tardis1 that Mike has given
  leave to use.** That is the harness's own mount on the client
  (`/mnt/rxr`) of `/hs/nvme0/zcbudget`, or of the export in front of a
  backing file on `/hs/nvme1`. fio and the like never run against
  anything else. The client has other mounts (`/mnt/save`,
  `/mnt/ci-repos`, `/mnt/hs_test` of 192.168.1.161) and tardis1 has
  other exports and another client (192.168.0.105): none of those are
  ours.
- On tardis1's drives only `/hs/nvme0` and `/hs/nvme1` may be used, as
  files in them. No drive is reformatted.
- A port is not taken down, and its rings are not changed, until Mike
  has said yes to that (decision 4).

## 1. What tardis1 has now

Read on 2026-10-07 unless a source is named.

| | |
|---|---|
| driver | `xeu` loaded from `kmod-64k-xsight`, `rx_ip_align=2` (`/etc/modprobe.d`: `options xeu rx_ip_align=2`), `num_rings=8` |
| ports | `enp164s0f0` 192.168.0.211/24 and `enp161s0f0` 192.168.1.211/24, both up, MTU 9000 (the driver's maximum) |
| rings | 8 combined channels (the maximum), 1024 receive descriptors each (range to 32768) |
| receive buffers | page pools (`ring_N_rx_pp_*` in `ethtool -S`), so the region can bind them |
| client | **192.168.0.106 / 192.168.1.106** (`plsm121c-06`): x86_64, 4 KiB pages, 48 CPUs, 1 TB, two 400G `mlx5_core` ports at MTU 9000, kernel `6.12.76-2.hs.212.el8`, fio 3.35, nstat, iperf3. `ssh -i /root/.ssh/id_ed25519_snitzer2 root@192.168.0.106`. **192.168.0.105 is another machine, not ours**; it holds NFS connections to tardis1 on `enp164s0f0` |
| local NVMe | eight WD SN8100 2 TB, `sgls: 0`, `virt_boundary_mask` 4095: all need whole pages. XFS at `/hs/nvme0` to `/hs/nvme7`, 68% full, exported to both client subnets |
| NVMe/TCP | `nvmet-tcp` and `nvme-tcp` are built for this kernel; neither is loaded |
| region this boot | 1180 pages bound to veth `rxsrv` (`page_pool.rx_region_dev`, `rx_region_pages`). A region is made once per boot |

From `on-die-receive-copy/SPEC.md` sections 4.2 and 5.1, read from the
driver's source and **not checked on this host**:

- one order-0 page per receive descriptor, so one frame per 64 KiB page
  (`napi_build_skb(page, PAGE_SIZE)`), truesize `PAGE_SIZE` per buffer;
- ring size 16 to 32768; refill runs only when 32 slots are free;
- no XDP, no receive flow steering for TCP, no interrupt coalescing
  controls.

## 2. Why the veth results do not carry over

These are arithmetic from section 1, not measurements. Work item 5
measures them.

- **A frame has a page to itself.** veth packs about seven jumbo frames
  into a 64 KiB page. On `xeu` a 1 MiB WRITE at MTU 9000 is about 117
  frames, so about 117 region pages (7.3 MiB), where on veth it held 25
  to 29 and was charged 61. The credit factor of 350 and the bound of a
  548-page budget (handoff section 20) are veth figures.
- **Half of the L3 is about two WRITEs.** 256 pages in use holds two
  1 MiB loans at 117 pages each, not eight.
- **The posted buffers are all in use.** veth parked 4 to 12 pages and
  took the most recently freed first. A hardware ring goes round every
  buffer posted to it, so rings x entries pages are in use before any
  loan. Today that is 8 x 1024 = 8192 pages, 512 MiB, on each port.
- **So the rings shrink first.** Four rings of 64 entries are 256 pages,
  half of the L3 with no loan in it. Whether the card drops at such a
  size under load is not known; the refill threshold of 32 sets a floor.
- **The charge fix is unproven here.** "Where a frame has a page to
  itself the charge is unchanged" is from the code and not from a run
  (tardis1's first defect, `transplant/README.md`).
- **The held receive buffer costs more.** TCP charges a whole 64 KiB
  page per frame, so a 128 KiB buffer is two frames. The harness takes
  16 x 4 pages for sixteen connections; that sum is veth's too.

## 3. The rig to build

| part | veth rig (now) | xeu rig |
|---|---|---|
| receiving device | veth `rxsrv`, XDP_PASS | one `xeu` port, small rings |
| client | network namespace `rxcli` on the same host | 192.168.1.106 over the wire, driven by `ssh` |
| storage that needs whole pages | nvme-loop over null_blk | a local NVMe, XFS |
| storage that takes arbitrary pieces | NVMe/TCP to 127.0.0.1 over null_blk | NVMe/TCP to 127.0.0.1, backing to be decided |
| storage delay | dm-delay, 1 ms and a 10 ms step | the device's own; dm-delay only on the NVMe/TCP backend if wanted |
| region | `rx_region_dev=rxsrv`, 555 to 1180 pages | `rx_region_dev=<port>`, sized from the rings and the budget, on a fresh boot |

The cells stay the ones compared so far: `seq-sized`, `mixed-sized`,
`step` with one and sixteen connections, each as `-copy` (budget 0) and
`-on` (budget set), with the receive buffer held in both.

## 4. Decisions

Answered by Mike on 2026-10-07:

1. **The client and the ports.** The load generator is `plsm121c-06`,
   192.168.0.106 on `enp164s0f0`'s network and 192.168.1.106 on
   `enp161s0f0`'s (Mike first said 105 and corrected it; 105 is not
   ours). Key: `/root/.ssh/id_ed25519_snitzer2`. Another client can be
   added later; first see how far one goes. **Use `enp161s0f0` and
   192.168.1.106**: `enp164s0f0` carries 105's connections.
2. **The local NVMe.** `/hs/nvme0` (and `/dev/nvme0n1`): test files in
   a directory of it, no mkfs.
3. **The NVMe/TCP target's backing.** `/hs/nvme1` may be used. The
   harness puts a 4 GiB file there (`BACKING=`), so NVMe/TCP is in
   front of a real drive and nothing is reformatted. The raw
   `/dev/nvme1n1` is not touched.

Still open:

4. **Taking a port down** to bind the region and shrink the rings
   (work item 4): not yet said in so many words, and it waits for the
   client's credentials in any case.
5. **The share of the L3 on this card.** Half (256 pages) is the share
   picked for veth. With the rings inside the region it may leave room
   for one or two WRITEs; decide once work item 5 has the numbers.

## 5. Work items

In order. Items 1 to 3 need no decision and no change to the card.

1. **Harness: a real device and a remote client. Done 2026-10-07,
   not yet run against a real client.** `run-budget.sh` takes
   `NETDEV=<port> CLIENT=<[user@]host>`: no veth, namespace or XDP, the
   client driven over `ssh`, its fio output and bandwidth log fetched
   back, its counters read there. The port's MTU, rings and module
   parameters are never changed; a cell that wants another MTU is
   skipped. Checked with a stand-in for `ssh` that runs the commands on
   tardis1 (`NOREGION=1`, traffic over loopback): the plumbing works
   and nothing on the port changed.
2. **Harness: an existing export. Done and run.** `EXPORT_DIR=<dir>`
   exports a directory of a mounted file system: no mkfs, only the
   cells' own files, the directory removed after if the script made
   it, a mount point refused. `BACKING=<file or device>` puts the NVMe
   target in front of that in place of null_blk. Neither has dm-delay,
   so cells run with the device's own latency and the step cells are a
   steady 120 s load. `rig.txt` records the device, its
   `virt_boundary_mask` and `sgls`.
3. **Harness: the region's sum for a hardware ring. Done, provisional.**
   `FRAME_PAGE=1` (the default when `NETDEV` is an `xeu` port) derives
   the geometry for one frame per page: credit factor 740, a 1 MiB
   record costing 120 pages, 3 pages for a 128 KiB held buffer and 5
   for 256 KiB. These are from the arithmetic of section 2 and are what
   work item 5 checks. `PARKED` is still read after the warm-up.
4. **Bring-up on one port** (decision 1). Stage the deadman switch of
   `xeu-hds-experiment/TESTING.md` section 0. Set `num_rings` and the
   ring size, boot with `page_pool.rx_region_dev=<port>` and
   `rx_region_pages`, and check `/sys/kernel/debug/page_pool_rx_region`
   shows the pages and no `alloc_fail`. Find the smallest ring that
   does not drop under an iperf3 load from the client.
5. **Measure the geometry.** One connection, 1 MiB WRITEs, budget set:
   the parked pages, the pages one WRITE holds (`credit.bt`: credit at
   release, pauses, overruns), `used_pages`, drops. From that the
   credit factor and reserve for this card, and how many WRITEs a
   256-page share admits.
6. **PRP coverage** (decision 2). The copy and loan pairs on the local
   NVMe: copies written direct, loans through the page cache
   (`wr_direct` / `wr_dontcache`), data verified, nothing dropped.
7. **SGL coverage** (decision 3). The same pairs on the NVMe/TCP
   export: every loan written direct with `rx_ip_align=2`
   (`../xeu-SGL-compat-receive-placement/` has the fragment trace), and
   a control at `rx_ip_align=0` if a reload is allowed.
8. **The bound on this card.** As handoff section 20: budgets around
   the share with `FORCE_BUDGET=`, peak pages in use with one and
   sixteen connections, and what the loan costs against its copy.
9. **Record.** A section in `transplant/tardis1-build457-handoff.md`
   per run, the account in `transplant/README.md` and `RESULTS.md`, and
   the artifact when Mike asks.

## 5a. Bring-up after the reboot of 2026-10-07 (work item 4): START HERE

Mike said yes to `enp161s0f0` (rings and region) and rebooted tardis1
for it. `enp164s0f0` is left alone: 192.168.0.105's connections are on
it. Nothing below has been run; each step says what to check.

The region is made at the first pool that matches, once per boot, and
`page_pool` is built in, so its parameters can be written after boot.
`xeu` is loaded from `/etc/modules-load.d`, so its pools exist by then;
they must be made again with the parameters set. Unknown until tried:
whether `xeu` lets the rings be changed with the port down, and whether
a ring change alone makes the pools again.

1. Check first: `cat /sys/kernel/debug/page_pool_rx_region` must not
   exist (no region yet this boot); `rx_ip_align` reads 2; the client
   answers (`ssh -i /root/.ssh/id_ed25519_snitzer2 root@192.168.1.106`).
   Run nothing on veth before this: the veth harness would make the
   region for `rxsrv`.
2. `ip link set enp161s0f0 down`
3. `ethtool -L enp161s0f0 combined 4` and `ethtool -G enp161s0f0 rx 64`
   (256 posted pages). If the driver refuses with the port down, do it
   after step 5 and then take the port down and up once more.
4. `echo 0000:a1:00.0 > /sys/module/page_pool/parameters/rx_region_dev`
   (the port's PCI function, not `enp161s0f0`: see 5b) and `echo 2048 > /sys/module/page_pool/parameters/rx_region_pages`.
   2048 pages (128 MiB) is for measuring, not the bound: room for rings
   up to 8 x 128 and a budget of several 120-page WRITEs. One boot has
   one region size.
5. `ip link set enp161s0f0 up`; then `page_pool_rx_region` must show
   `dev enp161s0f0`, `pages 2048`, `alloc_fail 0`, and `free` lower by
   about the posted pages. `dmesg | grep 'rx region'`.
6. From the client, `iperf3` to 192.168.1.211 (start `iperf3 -s` on
   tardis1 bound to that address): drops (`ip -s link`, `ethtool -S`),
   `alloc_fail`, `escaped`. This is traffic to tardis1 itself, not to a
   mount. If it drops, lengthen the ring (128) before adding rings.
7. Then work item 5 with the harness, which needs no `NOREGION` now:

       export SSH_OPTS="-i /root/.ssh/id_ed25519_snitzer2"
       NETDEV=enp161s0f0 CLIENT=root@192.168.1.106 STORAGE=prp \
           EXPORT_DIR=/hs/nvme0/zcbudget \
           CELLS="seq-sized-copy seq-sized-on" ./run-budget.sh <outdir>

   Read `parked`, `used_pages`, `paused`, `overrun`, drops, and whether
   the copy cell is now written direct (it was not with no region,
   section 6). Trace one loan cell with
   `/var/tmp/zc-run-logs/credit-factor-1180/credit.bt` for the credit a
   1 MiB WRITE holds at release: that is the factor for this card (740
   is the arithmetic).

To put the port back: `ethtool -L enp161s0f0 combined 8`,
`ethtool -G enp161s0f0 rx 1024`; a reboot clears the region.

## 5b. What the bring-up found (2026-10-07, after the reboot)

Steps 1 to 7 of 5a were run. The port is left at **4 rings of 256**,
the region is **2048 pages bound to `0000:a1:00.0`**, and `enp164s0f0`
was not touched. A deadman that restored only `enp161s0f0` was staged
for the changes and cancelled once the port answered; the session
arrives on `xemi0`, which `xeu` does not drive.

- **The region is named by the parent device.** A pool whose netdev has
  a parent matches `rx_region_dev` against the parent's name
  (`page_pool_rx_region_get()`), so the name is `0000:a1:00.0`
  (`readlink /sys/class/net/enp161s0f0/device`). With `enp161s0f0`
  written there the port came up with new pools and no region.
  `run-budget.sh` now accepts either name for `NETDEV`.
- **What the driver allows.** `ethtool -L` works with the port down.
  `ethtool -G` is refused with the port down ("requested ring size
  exceeds maximum") and works with it up. Either a down and up or a
  ring change with the port up makes the pools again, so the ring can
  be changed in place under a bound region.
- **`alloc_fail` is not zero this boot** and the harness reports its
  change per cell, so that does not matter: 2 from the first up at
  4 x 1024 (4096 posted against 2048), 1 from a ring change, and
  1336988 from the sixteen-stream iperf3 run at 4 x 256 below.
- **iperf3 from the client** to 192.168.1.211, 20 s, MSS 8948, 2048-page
  region; drops are `rx_hw_tail_drop_packets`, the pages in use are
  2048 less `min_free`:

  | rings x entries | streams | Gbit/s | tail drops | pages in use at peak |
  |---|---|---|---|---|
  | 4 x 64 | 1 | 41.7 | 115191 (1.0%) | 754 |
  | 4 x 64 | 16 | 45.2 | 231 | 1739 |
  | 4 x 128 | 1 | 41.3 | 12349 (0.1%) | 1034 |
  | 4 x 128 | 16 | 49.8 | 0 | 1993 |
  | 4 x 256 | 1 | 49.0 | 0 | 1262 |
  | 4 x 256 | 16 | 40.9 | 0 | 2048, and 1336988 `alloc_fail` |

  The smallest ring that does not drop is 256 for one stream and 128
  for sixteen. 4 x 256 is 1024 posted pages, twice the whole L3, so
  the share of decision 5 cannot hold the rings at a size that does
  not drop under this load. iperf3 at 40 to 50 Gbit/s with autotuned
  receive buffers is harder than the NFS cells; whether a 64-entry
  ring drops under those is not measured.
- **Pages stay with the pools at idle.** After the runs at 4 x 256,
  with iperf3 stopped, `free` was 531: 1024 posted and 493 more held
  by the pools (`pool_size` is the ring size, so each pool's own ring
  can keep up to 256, and its cache more). The harness read
  `parked=1517` after its first warm-up and 1628 in the next run: on
  this card "parked" is the posted pages plus what the pools keep, and
  it moves between runs.
- **The cells** (`STORAGE=prp`, `/hs/nvme0/zcbudget`, one connection,
  1 MiB WRITEs, 256 KiB held buffer; `/var/tmp/zc-run-logs/xeu-geometry-4x256`):

  | cell | budget | MB/s | drops | used_pages | admitted | denied | paused | overrun | written |
  |---|---|---|---|---|---|---|---|---|---|
  | seq-sized-copy | 0 | 1196 | 0 | 1517 | 0 | 0 | 0 | 0 | 512 direct |
  | seq-sized-on | 462 | 929 | 0 | 1647 | 512 | 135 | 0 | 0 | 512 dontcache |

  The `drops` column of `cells.tsv` reads 2 or 3 for every cell,
  warm-up included: it is the interface's `dropped`, not the ring's
  tail drops, and is not from the load. With a region bound the copy
  cell is written direct (it was not in section 6). Data verified,
  `fio_rc` 0, no retransmits.
- **The credit factor is bounded, not measured.** The trace
  (`xeu-geometry-4x256-trace.bt/credit.log`, 512 WRITEs) has every
  WRITE admitted with credit 127 and released with credit 127, `over`
  0, and no PAUSE. 127 is the estimate at factor 740, so one WRITE
  holds no more than that; how many pages it does hold is still to be
  measured (lower the factor until charges pause, or trace `need` in
  `svc_tcp_rx_credit_charge`). `used_pages` rose 130 over the copy
  cell with two WRITEs in flight at the denials, and 54 in the traced
  run.

Next: the measure of what one WRITE holds (the rest of work item 5),
then the ring size for the NFS cells, then decision 5 with those
numbers.

## 6. The real drives behind veth (2026-10-07)

The two storage modes were run with the veth client, on this boot's
1180-page region with credit factor 350, so the storage is real and the
network is not. No storage delay; MB/s, copy / loan; all fio rc 0, no
drops. `/var/tmp/zc-run-logs/harness-modes/`.

| pair | needs whole pages: `EXPORT_DIR=/hs/nvme0/zcbudget` | takes arbitrary pieces: NVMe/TCP, `BACKING` a file on `/hs/nvme1` |
|---|---|---|
| `seq-sized`, 1 connection | 1851 / 1754 | 1851 / 1726 |
| `mixed-sized`, 16 connections | 1884 / 1385 | 1729 / 2130 |
| `step-n16` (steady, 16 connections, 1 MiB) | 2231 / 2029 | 2156 / 2174 |
| how the loan was written | every one through the page cache (DONTCACHE) | every one direct |
| most of the region in use, copy / loan | 4 to 12 / 316 to 428 pages | 4 to 12 / 316 to 432 pages |

- On the drive that needs whole pages a loan is slower than its copy
  in all three pairs (95%, 74%, 91%); the copy is written direct.
- On NVMe/TCP every loan is written direct and is level with its copy
  or ahead with sixteen connections (123%, 101%), behind with one
  (93%).
- Each cell is one run, and the 1 ms veth cell has been seen to vary by
  a fifth between runs, so the single figures are not findings yet.
- A first run with the backing file made sparse was slow in its first
  cells (the copy at 432 MB/s): mkfs discarded the file and the cells
  paid for allocating it. The file is now written through once and
  mkfs told not to discard.

**First run over the card (2026-10-07, no region, nothing on the port
changed).** `NOREGION=1`, `enp161s0f0` (8 rings of 1024, MTU 9000,
`rx_ip_align=2`), client 192.168.1.106, `EXPORT_DIR=/hs/nvme0/zcbudget`;
`/var/tmp/zc-run-logs/xeu-plumbing/copy-prp`. The whole path works:
`ssh`, the mount, fio with verification, the client's counters and logs
fetched back, the directory removed after.

| cell (budget 0) | MB/s | fio rc | retransmits | port drops | WRITE segments direct / through the page cache |
|---|---|---|---|---|---|
| `seq-sized-copy`, 1 connection | 1598 | 0 | 0 | 2 | 0 / 512 |
| `mixed-sized-copy`, 16 connections | 1561 | 0 | 0 | 2 | 624 / 2161 |
| `step-copy-n16`, steady 1 MiB | 1523 | 0 | 0 | 126 | 0 / 174278 |

- **These are not copies.** With a budget of 0 the WRITEs went through
  the page cache, where a copy on this drive is written direct (as on
  veth, above). The budget governs region pages only
  (`page_pool_page_in_rx_region()` in `net/sunrpc/svcsock.c`), and with
  no region bound to the port its pages are loaned with no budget.
  That is read from the code and the counts; it was not traced.
- So **a copy against a loan on the card needs the region bound** (work
  item 4). Until then the only copy twin is
  `sunrpc.svc_tcp_rx_loan_pages=N`, which the harness does not switch.
- The port's drop counter is the whole port's, not this load's alone.

**To run on the card** with a region bound to the port (work item 4):

    export SSH_OPTS="-i /root/.ssh/id_ed25519_snitzer2"
    NETDEV=enp161s0f0 CLIENT=root@192.168.1.106 STORAGE=prp \
        EXPORT_DIR=/hs/nvme0/zcbudget ./run-budget.sh <outdir>
    NETDEV=enp161s0f0 CLIENT=root@192.168.1.106 STORAGE=sgl \
        BACKING=/hs/nvme1/zcbudget-nvmet.img ./run-budget.sh <outdir>

`NOREGION=1` runs the same before a region is bound, to check the
plumbing; its loan cells have a budget of 0 and so copy.

## 7. What this does not cover

- Whether the region stays in the CPU's cache, and what a copy costs
  once the data has left it: `on-die-receive-copy/SPEC.md` section 6,
  with the memory-controller counters. This rig is what that
  measurement would run on.
- The paired-posting (HDS) experiment, `xeu-hds-experiment/`.
- The 4 KiB and 16 KiB hosts, which have no Xsight card.
- 400G line rate: one client cannot fill the port.
