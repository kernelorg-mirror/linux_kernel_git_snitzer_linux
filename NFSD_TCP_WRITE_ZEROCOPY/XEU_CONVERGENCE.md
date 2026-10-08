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

## 64K. Findings at 64 KiB pages (tardis1, build 458, 2026-10-07)

Everything found on the `xeu` port at `PAGE_SIZE` 64 KiB, in one
place. The detail and the tables are in 5b (bring-up), 5c (baselines,
ring stall, step cells), 5d (the sixteen-connection stall), 5e (the
held buffer and the budget varied) and 5f (cache misses) below.
Every figure is from 4 rings and a 2048-page (128 MiB) region; most
are one run.

What holds whatever the page size:

1. **The region is named by the PCI device** (`0000:a1:00.0`), not the
   interface. Rings can be resized with the port up, under a bound
   region (5b).
2. **A receive ring that runs empty stays empty** until the port goes
   down and up: the driver refills only from its own receive poll and
   does not retry a failed allocation. Seen twice; read in the source
   (5c). The pool must never fail on this card.
3. **The pools keep pages.** At 4 x 256, 1024 are posted and the pools
   come to hold about 600 more, so about 1650 of 2048 are gone before
   any loan. The harness reads `parked` too early and derives a budget
   the region cannot cover (5c).
4. **The drive is not a limit** (10.4 GiB/s local). The network gives
   7.0 GB/s to one autotuned connection and 3.9 GB/s to sixteen with
   128 KiB buffers (5c).
5. **Copies and loans both reach the drive direct** where the storage
   allows it: every copy cell was written direct, every loan on
   NVMe/TCP direct, every loan on the local NVMe through the page
   cache (dontcache). No pause, overrun, orphan or escaped page in any
   cell; the trace has every admitted WRITE released (5b, 5d).

What follows from one frame having a 64 KiB page to itself:

6. **A 1 MiB WRITE is about 117 pages, 7.3 MiB of region**, estimated
   at 127 by the budget (factor 740). The estimate was never exceeded;
   what a WRITE really holds was not measured (5b).
7. **The L3 share does not fit.** The L3 is 32 MiB, 512 pages. Rings
   that do not drop under iperf3 (4 x 256) are 1024 pages, and a ring
   of 64 drops 1% of one fast stream (5b). Half of the L3 (256 pages)
   cannot hold both rings and one WRITE.
8. **A held receive buffer is two to four frames.** TCP charges 64 KiB
   a frame, so 128 KiB is two frames and 256 KiB four. One connection
   at 256 KiB is limited to 0.9 to 1.2 GB/s by that alone, and the
   one-connection copy cells are at that ceiling (5c).
9. **One connection, budget with room: the loan beat its copy by 14%**
   on the local NVMe (880 against 771 MB/s, 120 s, one run each). At a
   budget of one WRITE the loan is slower than its copy (5c).
10. **Sixteen connections on a budget of one WRITE collapse** to a
    WRITE every 40 to 90 s when untraced (9 runs of 9, on both
    storages and both ring sizes, the cell alone included), through
    TCP backoff on the client and not through the budget's accounting
    (5d). With the trace attached the same cell runs at 315 MB/s
    (6 runs of 6), against 1977 for its copy.
11. **Either a larger held buffer or a budget of several WRITEs ends
    the collapse, and both together bring sixteen connections' loans
    to their copy** (5e): 576 MB/s with a 256 KiB buffer alone, 1241
    to 1539 with a budget of four to six WRITEs alone, 1779 to 2309
    with both, against 2090 to 2286 for the copy cells. None of it
    fits in half of the L3: the best cells peak at 1500 of the 2048
    pages with rings of 4 x 128.
12. **Rings of 4 x 128 did not drop** in any NFS cell (5e), where
    iperf3 at 41 Gbit/s on one stream dropped 0.1% (5b).
13. **Shrinking the rings barely moves the cache misses** of a copy
    cell: 12 to 14% fewer per megabyte from 4 x 256 to 4 x 64, about
    one L2 refill per line of payload at either (5f).
14. **Not obtained:** a copy cell at a 512 KiB buffer to set the
    512 KiB loans against, and repeats: every figure in 5e is one
    run.

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

- **The same pair on NVMe/TCP** (`STORAGE=sgl`,
  `BACKING=/hs/nvme1/zcbudget-nvmet.img`, `virt_boundary_mask` 0;
  `/var/tmp/zc-run-logs/xeu-geometry-4x256-sgl`), the first of work
  item 7:

  | cell | budget | MB/s | used_pages | admitted | denied | paused | overrun | written |
  |---|---|---|---|---|---|---|---|---|
  | seq-sized-copy | 0 | 1220 | 1636 | 0 | 0 | 0 | 0 | 512 direct |
  | seq-sized-on | 343 | 619 | 1640 | 512 | 511 | 0 | 0 | 512 direct |

  Every loan was written direct, and the loan cell ran at half its
  copy. All but one WRITE was denied once before it was admitted. The
  budget the harness derived was 343, where it was 462 for the PRP
  pair, because `parked` had risen to 1636; at 127 a WRITE and a
  reserve of 120 that is one WRITE in flight and sometimes two. That
  the budget is what halves the rate is the likely reading and is not
  shown: `FORCE_BUDGET=` on this region, or a larger region on the
  next boot, would show it. `fio_rc` 0, no retransmits, no tail drops,
  `alloc_fail` unchanged. The backing file was removed; the `nvmet`
  and `nvme-tcp` modules stay loaded.

- **The budget is not what set that rate, and these cells are too
  short to compare rates.** The NVMe/TCP loan cell again with
  `FORCE_BUDGET=` (`xeu-geometry-4x256-sgl-b600`, `-b900`):

  | budget | MB/s | denied | used_pages | its warm-up, MB/s |
  |---|---|---|---|---|
  | 343 (derived) | 619 | 511 | 1640 | 664 |
  | 600 | 847 | 86 | 1861 | 860 |
  | 900 | 498 | 0 | 1731 | 658 |

  With no WRITE denied the cell was the slowest of the three, so the
  reading above (the budget halves the rate) does not hold. Each
  cell's rate follows its own warm-up, which is the same copy job run
  moments before and itself ranged from 658 to 860 here and 1118 to
  1342 on the PRP runs. A cell writes 512 MiB, under a second at these
  rates, so one run's MB/s is not a measurement of copy against loan.
  What these runs do show: every loan written direct, no pause or
  overrun, no `alloc_fail`, no tail drops, at budgets beyond the 408
  pages the region had free. Longer cells, repeated, are needed before
  any rate is compared.

Next: section 5c.

## 5c. Baselines, the ring stall and the step cells (2026-10-07)

All at 4 rings of 256 and the 2048-page region.

**What a perfect cell is.** Client to 192.168.1.211, 20 s, a receiver
that discards (`iperf3`, and a Python sender and receiver so that only
the server's `SO_RCVBUF` is set); local fio on a scratch file in
`/hs/nvme0`, removed after.

| limit | measured |
|---|---|
| local NVMe, 1 MiB direct writes, iodepth 16 | 10.4 GiB/s |
| network, 1 connection, autotuned buffer | 6997 MB/s (56 Gbit/s), 16811 tail drops |
| network, 16 connections, `sk_rcvbuf` 131072 each | 3878 MB/s (31 Gbit/s), no drops |
| network, 1 connection, `sk_rcvbuf` 262144 | 907 MB/s; 1180 MB/s with `iperf3 -w 131072`, which also limits the sender |

One connection with the buffer held at 256 KiB is limited by that
buffer, at about 0.9 to 1.2 GB/s: a frame is charged a whole 64 KiB
page, so the buffer is four frames. The one-connection copy cells
(1196, 1220 MB/s) are at that ceiling and a loan cannot pass it.
Sixteen connections at 128 KiB have 3.9 GB/s. The drive limits neither.

**A ring that runs empty stays empty.** Twice a load ran the region
dry (`min_free` 0): sixteen connections with autotuned buffers, and
`step-on-n16` with the budget the harness derived. Each time one ring
(ring 0, then ring 3) stopped: `rx_packets` fixed, `rx_hw_tail_drop_packets`
rising, connections that hash to it in retransmit backoff or timing
out, with 400 pages free in the region. Down and up of the port clears
it. The cause, from `xeu_drv.c` (the debuginfo source of
`kmod-64k-xsight` hs.165; the loaded 458 module has the same
`xeu_rx_refill`, `xeu_rx_poll`, `xeu_napi_intr` and no timer or work):

- `xeu_rx_refill()` is called at open and at the end of
  `xeu_rx_poll()`, nowhere else;
- when `page_pool_alloc_pages()` fails, `xeu_rx_post_one()` logs
  "Failed to allocate Rx page from pool" (once per page, not rate
  limited) and the refill stops, with no retry;
- the poll runs only from `xeu_napi_intr()`, and the interrupt is asked
  for per posted buffer (`RX_SR_BD_INTR_EN`);
- so a ring with nothing posted gets no completion, no interrupt, no
  poll and no refill.

Any pool allocation failure can do this; the region makes it easy to
reach. Until the driver retries a short refill, the region and the
budget on this card have to be sized so that the pool never fails.

**The harness's budget is wrong for these pools.** It reads `parked`
after the warm-up and takes the rest for the budget. Just after the
port is brought up that is about 1130 (1024 posted and the caches);
under load the pools come to keep about 1650. A budget of 796 derived
from the first figure is what ran the region dry.

**The step cells** (120 s each; `xeu-step-4x256-prp`,
`xeu-step-4x256-sgl-b250`, `xeu-step-4x256-prp-b250`). `alloc_fail` 0
and `paused`, `overrun` 0 in every row but the one marked.

| storage | cell | conns | budget | MB/s | used_pages | admitted | denied | written |
|---|---|---|---|---|---|---|---|---|
| PRP | step-copy | 1 | 0 | 771 | 1140 | | | 88190 direct |
| PRP | step-on | 1 | 839 (derived) | 880 | 1682 | 100727 | 94 | dontcache |
| PRP | step-on | 1 | 250 | 378 | 1548 | 43302 | 42625 | dontcache |
| PRP | step-copy-n16 | 16 | 0 | 1977 | 1335 | | | 225985 direct |
| PRP | step-on-n16 | 16 | 796 (derived) | 9 | 2048 | 5377 | 9823 | dontcache; `alloc_fail` 15997, ring 3 stopped |
| PRP | step-on-n16 | 16 | 250 | 0 | 1548 | 29 | 81 | dontcache |
| SGL | step-copy | 1 | 0 | 766 | 1124 | | | 87699 direct |
| SGL | step-on | 1 | 250 | 613 | 1228 | 70144 | 69048 | direct |
| SGL | step-copy-n16 | 16 | 0 | 2058 | 1228 | | | 235419 direct |
| SGL | step-on-n16 | 16 | 250 | 0 | 1536 | 43 | 115 | direct |

- With room in the budget one connection's loan beat its copy by 14%
  on the local NVMe (880 against 771), one run of each. That budget
  was the over-promised 839 and the cell peaked at 1682 pages of 2048.
- At a budget of 250 (one WRITE in flight; nearly every WRITE denied
  once) one connection's loan is slower than its copy: 613 against 766
  on NVMe/TCP, 378 against 771 on the local NVMe.
- **Sixteen connections at a budget of 250 do not run.** 43 and 29
  WRITEs were admitted in 120 s, with zero-window drops (60, 67),
  receive-queue drops, collapses and retransmission timeouts, no
  `alloc_fail` and every ring alive after. fio returned 0 with 0 MB/s.
  Section 5d has why.
- The sixteen-connection copy cells, 1977 and 2058 MB/s, are half of
  what the network allowed with the same buffers.
- No sixteen-connection loan result exists yet on this card, and no
  NVMe/TCP loan at a budget with room. Both need pages the region does
  not have at 4 x 256: smaller rings, or a larger region on the next
  boot (one boot has one region size).

## 5d. Why sixteen connections stall on a budget of one WRITE (2026-10-07)

`step-on-n16`, `FORCE_BUDGET=250` (one WRITE of 127 in flight, 120 in
reserve), held buffer 128 KiB, local NVMe unless said.

| invocation | trace | runs | result |
|---|---|---|---|
| after `step-copy-n16` (NVMe/TCP) or `step-on` | off | 4 | all stalled: 28 to 43 WRITEs admitted in 120 s |
| alone after the warm-up | `credit.bt` | 5 | all ran: 311 to 320 MB/s, about 36000 admitted, 72000 denied |
| after `step-on` | `credit.bt` | 1 | ran: 359 MB/s |

The trace hides it, so it is a matter of timing; whether the cell
before it matters is not known (the cell alone and untraced was not
run). The traced runs have the same drops and timeouts in small
numbers (zero-window drops 0 to 6, timeouts 0 to 4).

Caught untraced, with a watcher that captured both ends only once
admission had been flat for 20 s (`xeu-step-4x256-prp-b250-stall.bt/`):

- **Budget:** `inflight 1`, `avail 3`, `reserve 120`, `waiters 15`,
  unchanged for 40 s at a time. The accounting is consistent: it waits
  for the one WRITE in flight.
- **That WRITE's connection:** the server's socket is empty with a
  window of 15360 open. The client has 2 segments unacknowledged, 400
  KB not sent, `cwnd 1`, and its retransmission timer backed off nine
  times to 103 s.
- **The fifteen waiting:** each server socket holds one to three and a
  half frames (65792 to 229888 of `rb131072`). On the client thirteen
  are in zero-window probing backed off eight or nine times, timers 27
  to 62 s; two are in retransmission backoff.
- **The wire:** 8 packets in 40 s, all zero-window probes and their
  `win 0` answers. No ring stopped, no `alloc_fail`, 468 pages free.
- Server, this boot: `TCPZeroWindowDrop` 493, `TCPRcvQDrop` 151,
  `TCPTimeouts` 99.

The reading: a frame is charged 64 KiB, so a 128 KiB buffer is full at
two frames and a connection denied credit shuts its window at once.
With one WRITE admitted and sixteen connections, each waits many turns
and its sender's timer doubles each time. Segments that arrive with
the buffer over its limit are dropped, which puts that sender into
retransmission backoff (this step is from the counters and the
unacknowledged segments, not from a capture of a drop). When such a
connection is admitted its sender sleeps for up to 100 s, the WRITE
holds the whole budget until then, and the others back off further.

It is the three together: a buffer of two frames, a budget of one
WRITE, many connections. Section 5e takes each away in turn.

## 5e. The held buffer and the budget, varied (2026-10-07 and 08)

One sequential queue, untraced, every cell 120 s and one run
(`/var/tmp/zc-run-logs/xeu-queue.log`, directories `xeu-q-*`). No
`alloc_fail`, no overrun, no tail drop on any ring and no ring stopped
in any of it; `paused` was 0 but for three cells (1, 10, 10). The
rings went to 4 x 128 for the budgets beyond one WRITE, which do not
fit at 4 x 256, and were put back after. At 4 x 128 the region has
about 1160 pages free at idle: 512 posted and about 375 kept by the
pools.

**`step-on-n16` alone after the warm-up, untraced, budget 250, 128 KiB
buffer, 4 x 256, local NVMe:** three runs, all stalled (38, 37 and 27
WRITEs admitted). The cell before it is not needed; it is the trace
that hid the stall.

**Sixteen connections, loans** (`step-on-n16`), MB/s:

| rings | storage | buffer | budget 250 | budget 630 | budget 880 |
|---|---|---|---|---|---|
| 4 x 256 | local NVMe | 128 KiB | 0 (stall) | | |
| 4 x 256 | local NVMe | 256 KiB | 576 | | |
| 4 x 128 | local NVMe | 128 KiB | 0 (stall) | 1241 | 1395 |
| 4 x 128 | local NVMe | 256 KiB | | 1796 | 1985 |
| 4 x 128 | local NVMe | 512 KiB | | 1989 | 1979 |
| 4 x 128 | NVMe/TCP | 128 KiB | 0 (stall) | 1270 | 1539 |
| 4 x 128 | NVMe/TCP | 256 KiB | | 1779 | 1939 |
| 4 x 128 | NVMe/TCP | 512 KiB | | 2044 | 2309 |

**Sixteen connections, copies** (`step-copy-n16`, budget 0): 2286 at
256 KiB on 4 x 256 (local NVMe); 2090 (local NVMe) and 2225 (NVMe/TCP)
at 128 KiB on 4 x 128; 1977 and 2058 at 128 KiB on 4 x 256 (5c). No
copy was run at 512 KiB.

**One connection, loans** (`step-on`), MB/s, 4 x 128 unless said:

| storage | buffer | budget 250 | budget 630 | budget 880 |
|---|---|---|---|---|
| local NVMe | 256 KiB | 568 (649 at 4 x 256) | 869, 889 | 803, 878 |
| local NVMe | 512 KiB | | 1536 | 1444 |
| NVMe/TCP | 256 KiB | 551 | 798, 796 | 856, 810 |
| NVMe/TCP | 512 KiB | | 1406 | 1420 |

What it shows:

- **The stall needs both a two-frame buffer and a budget of one
  WRITE.** A 256 KiB buffer at a budget of 250 runs (576 MB/s, no
  zero-window drop, no timeout); a budget of 630 or 880 at 128 KiB
  runs (a few zero-window drops and 5 to 9 timeouts per cell, against
  58 to 99 and 21 to 25 in a stalled one).
- **Sixteen connections reach their copy only with both.** At a budget
  of 880 and a 512 KiB buffer the loans ran at 1979 (local NVMe) and
  2309 (NVMe/TCP); the copies at smaller buffers ran at 2090 to 2286.
  Whether a loan beats its copy at the same buffer is not shown.
- **One connection follows the held buffer**, as the baseline of 5c
  said it would: about 800 to 890 MB/s at 256 KiB whatever the budget
  beyond one WRITE, 1400 to 1540 at 512 KiB.
- **The denials stay high where it runs well** (260000 to 350000 a
  cell at sixteen connections): admission is being used all the time,
  and it holds.
- **What it costs in pages.** The best cells peak at about 1500 to
  1535 pages in use of 2048 (`used_pages`), with 512 posted. That is
  three times the whole L3.
- Three larger-buffer runs at 4 x 256 were skipped for want of pages
  (404 free at idle): 512 KiB on the local NVMe, 256 and 512 KiB on
  NVMe/TCP.

Next: the page size (Mike, 2026-10-07: 16 KiB and 4 KiB are open for
this platform); a copy cell at 512 KiB and repeats of the cells above.

## 5f. Cache misses in the copy cells at two ring sizes (2026-10-08)

`perf stat -a` for 90 s inside each copy step cell on the local NVMe,
at 4 x 256 and at 4 x 64 (`xeu-llc.log`, `xeu-llc-4x*-prp.*.perf`),
less what the idle host counted at the same rate (8.5 million
`l3d_cache_lmiss_rd` and 2.6 million `l2d_cache_refill` a second,
unexplained; the other port has another client's connections). A
megabyte is 16384 cache lines.

| rings | cell | MB/s | pages in use | L2 refills per MB | L3 long misses per MB |
|---|---|---|---|---|---|
| 4 x 256 | step-copy | 769 | 1112 | 16400 | 41100 |
| 4 x 64 | step-copy | 767 | 338 | 14500 | 35300 |
| 4 x 256 | step-copy-n16 | 2067 | 1120 | 25300 | 59500 |
| 4 x 64 | step-copy-n16 | 1633 | 337 | 21900 | 51300 |

- Going from 1112 pages in use to 338 took 12 to 14% off the misses
  per megabyte, no more. One connection's rate did not change; sixteen
  connections lost a fifth of theirs, with 207 tail drops at 4 x 64.
- About one L2 refill per cache line of payload at either size: the
  first read of received data is not served from L2 whether the rings
  are 70 MiB or 21 MiB of pages.
- `l3d_cache_rd` reads 0 on this CPU, so there is no miss ratio, and
  the long-miss count is above the L2 refill count, so it is not
  simply "L2 misses that left the L3". Treat the last column as a
  trend only.
- This neither shows nor rules out that frame data is confined to a
  seventh of the cache's sets at 64 KiB. At 4 x 64 the frames in the
  rings are about 2.3 MiB, which would fit even in a seventh of the
  L3, and the misses still did not fall away; so something other than
  the rings' size sets most of them. Whether data the card has just
  written is in the cache at all when the CPU first reads it is the
  question this raises, and it is one for the on-die receive design,
  not answered here.

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
