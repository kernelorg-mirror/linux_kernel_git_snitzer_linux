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

4. **Taking a port down** to bind the region and shrink the rings:
   yes for `enp161s0f0` (2026-10-07). `enp164s0f0` is never touched,
   which also rules out reloading `xeu`.
5. **The page size.** Open for this platform (2026-10-07), and on
   2026-10-08 Mike chose to pivot to 16 KiB and repeat this testing
   there. The 64 KiB work stops at the "64K" section below.

Still open:

6. **The share of the L3 on this card.** Half (256 pages) is the share
   picked for veth. At 64 KiB it does not fit: finding 7 of the "64K"
   section. To be decided again at 16 KiB.

## 5. Work items

In order. Items 1 to 3 need no decision and no change to the card.
As of 2026-10-08, at 64 KiB: items 1 to 4 are done (5b); item 5 gave
an upper bound and not a measure (5b); items 6 and 7 were run as the
step cells and their variations (5c, 5e, 5g) and not as the plan's
pairs; item 8 was not run as written, since the share does not fit;
item 9 is this file and the artifact, with the handoff, `README.md` and
`RESULTS.md` still to be brought to it.

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

## 5a. Bring-up after the reboot of 2026-10-07 (work item 4)

Mike said yes to `enp161s0f0` (rings and region) and rebooted tardis1
for it. `enp164s0f0` is left alone: 192.168.0.105's connections are on
it. These are the steps as planned; they were run that day, and 5b has
what differed (the region's name in step 4, and that `ethtool -G` in
step 3 works only with the port up).

The region is made at the first pool that matches, once per boot, and
`page_pool` is built in, so its parameters can be written after boot.
`xeu` is loaded from `/etc/modules-load.d`, so its pools exist by then;
they must be made again with the parameters set.

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
   (the port's PCI function, not `enp161s0f0`: see 5b) and
   `echo 2048 > /sys/module/page_pool/parameters/rx_region_pages`.
   2048 pages (128 MiB) is for measuring, not the bound: room for rings
   up to 8 x 128 and a budget of several 120-page WRITEs. One boot has
   one region size.
5. `ip link set enp161s0f0 up`; then `page_pool_rx_region` must show
   `dev 0000:a1:00.0`, `pages 2048`, `alloc_fail 0`, and `free` lower by
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

## 64K. Findings at 64 KiB pages (tardis1, build 458, 2026-10-07 and 08)

Everything found on the `xeu` port at `PAGE_SIZE` 64 KiB, in one
place. The detail and the tables are in 5b (bring-up), 5c (baselines,
ring stall, step cells), 5d (the sixteen-connection stall), 5e (the
held buffer and the budget varied), 5g (CPU per GB) and 5f (cache
misses) below. Every figure is from 4 rings and a 2048-page (128 MiB)
region; most are one run. The work at 64 KiB stops here: on 2026-10-08
Mike chose to move tardis1 to 16 KiB pages and repeat it there.

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
    the collapse** (5e): 576 MB/s with a 256 KiB buffer alone, 1241 to
    1539 with a budget of four to six WRITEs alone, 1779 to 2309 with
    both.
12. **At the same 512 KiB buffer the loan and its copy are level**
    (5e, 5g): within about 10% either way at one and at sixteen
    connections on both storages, on the first run. The second run
    agrees but for the sixteen-connection loan on the local NVMe,
    which ran at 569 MB/s where it had run at 1979.
13. **No saving in server CPU shows for the loan** (5g): the same
    kernel time per GB as its copy on NVMe/TCP, about a quarter more
    on the local NVMe, where a loan goes through the page cache. The
    NVMe/TCP stand-in on the same host costs more CPU than the NFS
    path, so the measure is blunt.
14. **None of the working settings fits in half of the L3.** The best
    cells peak at 1500 of the 2048 pages with rings of 4 x 128.
15. **Whether rings of 4 x 128 drop under the NFS cells is not known**
    (5e: the counter was read after it had been zeroed). iperf3 at
    41 Gbit/s on one stream dropped 0.1% at that size (5b), and
    sixteen-connection copies lost a fifth of their rate with 207 tail
    drops at 4 x 64 (5f).
16. **Shrinking the rings barely moves the cache misses** of a copy
    cell: 12 to 14% fewer per megabyte from 4 x 256 to 4 x 64, about
    one L2 refill per line of payload at either (5f).
17. **The driver has a patch for finding 2** in `kernel-rpmtree`,
    and it works (5c): loaded on this boot, four dry regions of four
    left every ring receiving.
18. **Not obtained:** what one WRITE really holds; repeats (most
    figures are one run, and finding 12 shows what that is worth); a
    profile by function.

**For 16 KiB.** A frame still has a page to itself, so findings 1 to 5
carry over and 6 to 8 change by arithmetic: a 1 MiB WRITE is 117 pages
of 16 KiB, 1.8 MiB; the L3 is 2048 pages; a 128 KiB buffer is eight
frames. Whether the stall of finding 10, the level rates of finding 12
and the misses of finding 16 change with it is what the same cells are
to show. It needs a 16 KiB build of the kernel and of `kmod-xsight`
(only 4 KiB and 64 KiB are installed here; `kernel-rpmtree` builds a
third with `--arm64-16k`), and a reboot, which is Mike's to time.
Mike has built the 16 KiB kernel; whether its kmod is 2.0.1-4, with
the driver patch, is the first thing to check on that boot.

## 16K. Findings at 16 KiB pages (tardis1, build 459, 2026-10-08)

The same hunt on the `+16k` kernel
(`7.1.13-21.hs.459.loanpages_pool.el9.aarch64+16k`), from
`xeu-rig/README.md`. A 6144-page (96 MiB) region bound to
`0000:a1:00.0`, 4 rings. The L3 is 2048 pages; half is 1024. Output is
`/var/tmp/zc-run-logs/xeu-16k-*`. **Every cell here is one run** until
the repeats are in.

How the rig stood:

- **The driver has the refill retry.** `kmod-16k-xsight` 2.0.1-4 was
  built for the running kernel with `xeu-rig/build-kmod.sh`, installed
  (`rpm -U` removed release 3) and `xeu` reloaded before any region
  was bound; `xeu_rx_refill_timer` is in `/proc/kallsyms` and
  `rx_ip_align` is 2.
- **Only one port is up.** Mike tore down every other mount of tardis1
  (192.168.0.105 had sixteen connections on `enp164s0f0`) and had
  `enp164s0f0` taken down at 10:12:57 for the day, so that nothing
  else is on the card. It is to come back up once the sizing with one
  port is settled.
- **The geometry is as the arithmetic said**: factor 190 at MTU 9000,
  a record cost of 123 and a reserve of 130 pages, and `sweep.sh`
  charges 126 pages for a WRITE of 1 MiB. A 128 KiB buffer is 9 pages,
  256 KiB 17.
- **A budget of n WRITEs admits n.** `xeu-rig/inflight.sh` sampled
  `net.sunrpc.region_budget_stats` four times a second through the
  sweep: budgets of 260, 386, 638 and 1142 pages showed at most 1, 2,
  4 and 8 in flight, with up to 15 waiting.

### Ring size under a plain network load

`xeu-rig/ringsize.sh 64 128 256`, 20 s a cell, the Python sender and
receiver (`xeu-16k-ringsize.tsv`; its `retrans` column is a running
total on the client, which `ringsize.sh` no longer reports that way):

| rings | streams | receive buffer | MB/s | tail drops | region pages in use at peak |
|---|---|---|---|---|---|
| 4 x 64 | 1 | autotuned | 5823 | 142515 | 572 |
| 4 x 64 | 16 | autotuned | 28234 | 1210105 | 995 |
| 4 x 64 | 1 | 256 KiB | 3647 | 0 | 502 |
| 4 x 64 | 16 | 128 KiB | 12122 | 16860 | 483 |
| 4 x 128 | 1 | autotuned | 8066 | 22502 | 1442 |
| 4 x 128 | 16 | autotuned | 47359 | 473170 | 5134 |
| 4 x 128 | 1 | 256 KiB | 3674 | 0 | 1050 |
| 4 x 128 | 16 | 128 KiB | 11945 | 0 | 959 |
| 4 x 256 | 1 | autotuned | 7272 | 0 | 1359 |
| 4 x 256 | 16 | autotuned | 49483 | 8125 | 5889 |
| 4 x 256 | 1 | 256 KiB | 3631 | 0 | 2084 |
| 4 x 256 | 16 | 128 KiB | 11877 | 0 | 1988 |

- With the buffer held, as every NFS cell holds it, **the smallest
  ring that does not drop is 4 x 128**, for one connection and for
  sixteen. 4 x 64 drops 0.06% of sixteen connections' frames.
- Sixteen autotuned streams carry about 48 GB/s (380 Gbit/s). The
  interface's `rx_bytes` gave 48448 MB/s where the receiver counted
  47817 (`xeu-16k-ratecheck.out`), so the rate is the wire's and not
  the receiver's arithmetic. They drop at every ring size and use
  nearly the whole region.
- Held buffers carry three to four times what they did at 64 KiB
  (3.6 GB/s to one connection at 256 KiB against 0.9; 11.9 GB/s to
  sixteen at 128 KiB against 3.9): a frame is charged a 16 KiB page.
- `enp164s0f0` went down during the 4 x 128, one stream, autotuned
  cell. Its rate lies between its neighbours' and it was not rerun.

### The core: rings of 4 x 128, three buffers, both storages

`plans/16k-1-core.plan` into `xeu-16k-core`, 120 s `step` cells, 1 MiB
WRITEs. MB/s, with the copy of the same buffer and storage on the
`copy` line. "Native" is `/hs/nvme0` (needs whole pages); NVMe/TCP is
the loopback target on a file in `/hs/nvme1` (takes arbitrary pieces).
Pages are the most of the region in use in the loan cells.

| buffer | budget, WRITEs | native, 1 conn | native, 16 conn | NVMe/TCP, 1 conn | NVMe/TCP, 16 conn | pages | of the L3 | ring tail drops |
|---|---|---|---|---|---|---|---|---|
| 128 KiB | copy | 1375 | 2250 | 1197 | 2308 | 880 to 895 | 43% | 0 |
| 128 KiB | 1 | 594 | 894 | 709 | 756 | 1089 to 1123 | 54% | 0 |
| 128 KiB | 2 | 1056 | 1687 | 1196 | 1457 | 1240 | 60% | 0 |
| 128 KiB | 4 | | 1264 | | 1939 | 1459 | 71% | 0 |
| 128 KiB | 8 | | 1651 | | 2212 | 1881 to 1904 | 92% | 0 |
| 256 KiB | copy | 1712 | 1978 | 1994 | 2151 | 871 to 893 | 43% | 0 |
| 256 KiB | 1 | 793 | 1197 | 766 | 953 | 1210 | 59% | 0 |
| 256 KiB | 2 | 1385 | 1627 | 1153 | 1690 | 1305 to 1318 | 64% | 0 |
| 256 KiB | 4 | | 2223 | | 2175 | 1491 to 1499 | 73% | 0 |
| 256 KiB | 8 | | 1532 | | 2187 | 1913 to 1935 | 94% | 11 native |
| 512 KiB | copy | 1942 | 1946 | 1942 | 2377 | 889 to 907 | 44% | 0 |
| 512 KiB | 1 | 865 | 1369 | 823 | 1051 | 1365 to 1370 | 67% | 0 |
| 512 KiB | 2 | 1680 | 1922 | 1410 | 1899 | 1441 to 1472 | 71% | 5 native, 19 NVMe/TCP |
| 512 KiB | 4 | | 2251 | | 2342 | 1613 | 78% | 422 native, 2576 NVMe/TCP |
| 512 KiB | 8 | | 1625 | | 2069 | 1959 to 1986 | 96% | 574 native, 47 NVMe/TCP |

No cell stalled, paused, overran or ran the region dry (`alloc_fail`
0 throughout, `escaped` 0); one retransmit timeout in all, in the
512 KiB, four WRITEs, NVMe/TCP cell.

1. **Sixteen connections reach their copy at a budget of four WRITEs
   with a 256 KiB buffer, on both storages**: 2223 against 1978 native,
   2175 against 2151 on NVMe/TCP, with no ring drops, at about 1500
   pages, 73% of the L3. The same budget at 512 KiB is level too (2251
   against 1946, 2342 against 2377) but costs 120 pages more and is
   the first to drop frames at rings of 4 x 128.
2. **Nothing that is level fits in half of the L3.** The copy alone
   holds about 880 pages (the rings' 512 and what the pools keep), so
   there are under 150 pages of half the L3 left, and a WRITE is
   charged 126. Every cell fits in the whole L3.
3. **Budgets of one and two WRITEs leave the loan behind** at every
   buffer: 33% to 70% of the copy at one, 58% to 100% at two (the
   100% is one connection on NVMe/TCP at 128 KiB, 1196 against 1197).
   `denied` there is about twice `admitted`: it counts refusals to
   admit, after which the connection waits, and not WRITEs that were
   copied. Every WRITE of a loan cell is admitted in the end
   (`admitted` is the cell's count of WRITEs).
4. **A budget of eight buys nothing over four** on NVMe/TCP at 256 KiB
   (2187 against 2175) for 420 more pages, and on the native drive the
   cell at eight was slower than at four at 256 and 512 KiB (1532,
   1625). At 128 KiB the native cells are not in order either (1687,
   1264, 1651 at two, four and eight). One run each: these are the
   cells to repeat before anything is read into them.
5. **A 128 KiB buffer is worse than 256 KiB and saves no pages**: no
   sixteen-connection loan reaches its copy at 128 KiB (the nearest,
   2212 against 2308, is on NVMe/TCP at eight WRITEs and 92% of the
   L3), and the pages in use are within 40 of the 256 KiB cells'. The
   footprint follows the rings and the budget.
6. **The stall of 64 KiB (finding 10 there) is gone.** Sixteen
   connections on a 128 KiB buffer and a budget of one WRITE ran at
   894 MB/s native and 756 on NVMe/TCP, untraced, with no retransmit
   timeout. A 128 KiB buffer is eight frames here where it was two.
7. **How each was written is as at 64 KiB**: every copy direct on both
   storages, every admitted loan direct on NVMe/TCP, every admitted
   loan on the native drive through the page cache (dontcache).
   Nothing keeps a loan off the storage that needs whole pages: the
   transport loans by the region and the budget alone, and
   `nfs_dio_split()` then finds the loaned vector misaligned for a
   device with a `virt_boundary_mask` (`NFS_DIO_MEM_MISALIGNED`) and
   issues the WRITE buffered with `IOCB_DONTCACHE`. So on the native
   drive the loan trades the receive copy for a copy into the page
   cache.
8. **Server kernel CPU per GB, sixteen connections** (`summarize.py`,
   system and softirq time over 90 s, less idle): on NVMe/TCP the loan
   is at or a little under its copy at the budgets that are level
   (0.96 against 0.97 at 256 KiB and four WRITEs, 0.87 against 0.90 at
   512 KiB); on the native drive it is level at 256 KiB (0.48 against
   0.49) and above at 512 KiB (0.45 against 0.38). Softirq time alone
   is lower for every loan on NVMe/TCP at 256 KiB (0.155 against 0.172
   CPU-seconds per GB at four WRITEs). Single runs; the differences
   are small.

### The rings: 4 x 64 and 4 x 256 against 4 x 128, 256 KiB buffer

`plans/16k-2-rings.plan` into `xeu-16k-rings`, then
`plans/16k-4-ring64.plan` into `xeu-16k-ring64` (budgets of three, four
and six WRITEs at 4 x 64, and the copies a second time). MB/s; one run
each but for the 4 x 64 copies, which are two. Pages are the most in
use in the cell; drops are the ring's over the 90 s window; timeouts
are the client's retransmit timeouts.

| rings | budget, WRITEs | storage | 1 conn | 16 conn | pages, 16 conn | of the L3 | tail drops, 16 conn | timeouts | paused |
|---|---|---|---|---|---|---|---|---|---|
| 4 x 64 | copy | native | 1701, 1687 | 2225, 2079 | 382 to 404 | 20% | 0, 46 | 0 | 0 |
| 4 x 64 | copy | NVMe/TCP | 1727, 1749 | 2300, 2166 | 382 to 412 | 20% | 199, 0 | 0 | 0 |
| 4 x 64 | 2 | native | 1225 | 1771 | 891 | 43% | 1176 | 0 | 15 |
| 4 x 64 | 2 | NVMe/TCP | 1133 | 1745 | 906 | 44% | 1123 | 1 | 70 |
| 4 x 64 | 3 | native | 1556 | 1894 | 984 | 48% | 891 | 2 | 1 |
| 4 x 64 | 3 | NVMe/TCP | 1767 | 2170 | 1000 | 49% | 2793 | 2 | 236 |
| 4 x 64 | 4 | native | 2050 | 1620 | 1102 | 54% | 4801 | 15 | 29 |
| 4 x 64 | 4 | NVMe/TCP | 2016 | 2139 | 1063 | 52% | 1761 | 0 | 15 |
| 4 x 64 | 6 | native | | 1745 | 1304 | 64% | 6340 | 24 | 32 |
| 4 x 64 | 6 | NVMe/TCP | | 2235 | 1286 | 63% | 10649 | 42 | 17 |
| 4 x 64 | 8 | native | | 2266 | 1504 | 73% | 4692 | 31 | 4 |
| 4 x 64 | 8 | NVMe/TCP | | 2029 | 1501 | 73% | 7679 | 89 | 6 |
| 4 x 256 | copy | native | 1527 | 1987 | 1152 | 56% | 0 | 0 | 0 |
| 4 x 256 | copy | NVMe/TCP | 1734 | 2359 | 1908 | 93% | 0 | 0 | 0 |
| 4 x 256 | 2 | native | 1223 | 1837 | 2032 | 99% | 0 | 0 | 0 |
| 4 x 256 | 2 | NVMe/TCP | 1142 | 1743 | 2145 | 105% | 0 | 0 | 0 |
| 4 x 256 | 4 | native | | 1219 | 2365 | 115% | 0 | 0 | 0 |
| 4 x 256 | 4 | NVMe/TCP | | 2130 | 2298 | 112% | 0 | 0 | 0 |
| 4 x 256 | 8 | native | | 1452 | 2737 | 134% | 0 | 0 | 0 |
| 4 x 256 | 8 | NVMe/TCP | | 2014 | 2648 | 129% | 0 | 0 | 0 |

`alloc_fail` 0 and `escaped` 0 throughout. `inflight.sh` had budgets of
512, 638 and 890 pages at 3, 4 and 6 in flight.

9. **Rings of 4 x 256 are out.** Every loan cell holds the whole L3 or
   more, no loan is faster than at 4 x 128, and 4 x 128 did not drop
   either.
10. **Rings of 4 x 64 take 400 to 500 pages off every cell.** The copy
    holds about 390 pages where it holds 880 at 4 x 128: the pools
    keep fewer as well as the rings posting 256 fewer.
11. **Every sixteen-connection loan at 4 x 64 drops frames at the
    ring**, 900 to 10600 in 90 s, more as the budget grows, with
    retransmit timeouts from a budget of four up; the one-connection
    cells drop none. 4801 drops is about 0.03% of the frames of that
    cell. **Mike, 2026-10-08: drops of that order are acceptable where
    the rate is level with the copy.** That relaxes the "no drop" of
    the handoff, and it is on that reading that 4 x 64 is a candidate.
12. **A budget of three WRITEs at 4 x 64 is the first setting inside
    half of the L3 that is near level**: at about 1000 pages (49%),
    NVMe/TCP ran at 2170 with sixteen connections against copies of
    2300 and 2166, and at 1767 with one against 1727 and 1749; the
    native drive at 1894 against 2225 and 2079, and 1556 against 1701
    and 1687.
13. **One connection at a budget of four WRITEs is ahead of its copy**
    on both storages at 4 x 64: 2050 against 1701 native, 2016 against
    1727 on NVMe/TCP, at about 860 pages (42%).
14. **The native drive's sixteen-connection loans are not in order by
    budget at any ring size** (4 x 64: 1771, 1894, 1620, 1745, 2266 at
    two, three, four, six and eight; 4 x 128: 1627, 2223, 1532;
    4 x 256: 1837, 1219, 1452), where NVMe/TCP's rise and level off.
    The copies of one setting differ by 7% between two runs (2225,
    2079). Repeats are needed before any native cell is called level
    or not.
15. **Server kernel CPU per GB on NVMe/TCP is under the copy's in 7 of
    the 9 sixteen-connection cells** at budgets of two, four and eight
    across the three ring sizes (for example 0.94 against 1.06 at
    4 x 256 and four WRITEs); the two that are not are both at eight.
    On the native drive the loan costs the same or more.

### The shortlist, three runs

`plans/16k-5-shortlist.plan` twice more (`xeu-16k-short-a`, `-b`), with
the runs above: `xeu-rig/summarize.py` over the five outdirs. 256 KiB
buffer; median MB/s and the range of three runs (the 4 x 64 copies are
four; the one-connection loans at 4 x 128 are two and are given whole).

| setting | storage | 16 conn, loan | 16 conn, copy | loan of copy | 1 conn, loan | 1 conn, copy | pages | of the L3 |
|---|---|---|---|---|---|---|---|---|
| 4 x 128, 4 WRITEs | native | 2158 (1866 to 2223) | 2232 (1978 to 2331) | 97% | 1602, 1715 | 1982 (1712 to 2029) | 1491 to 1530 | 74% |
| 4 x 128, 4 WRITEs | NVMe/TCP | 2331 (2175 to 2353) | 2139 (2132 to 2151) | 109% | 2021, 1590 | 1994 (1716 to 2000) | 1496 to 1530 | 74% |
| 4 x 64, 4 WRITEs | native | 2009 (1620 to 2200) | 2254 (2079 to 2312) | 89% | 1971 (1787 to 2050) | 1694 (1649 to 1715) | 1090 to 1102 | 54% |
| 4 x 64, 4 WRITEs | NVMe/TCP | 2164 (2139 to 2181) | 2286 (2166 to 2318) | 95% | 1757 (1635 to 2016) | 1745 (1727 to 1934) | 1053 to 1092 | 53% |
| 4 x 64, 3 WRITEs | native | 1894 (1863 to 2079) | 2254 (2079 to 2312) | 84% | 1657 (1556 to 1784) | 1694 (1649 to 1715) | 968 to 990 | 48% |
| 4 x 64, 3 WRITEs | NVMe/TCP | 2165 (1986 to 2170) | 2286 (2166 to 2318) | 95% | 1614 (1591 to 1767) | 1745 (1727 to 1934) | 970 to 1003 | 49% |

Ring tail drops and the client's retransmit timeouts in the
sixteen-connection loan cells, run by run:

| setting | storage | tail drops | timeouts |
|---|---|---|---|
| 4 x 128, 4 WRITEs | both | 0, 0, 0 | 0, 0, 0 |
| 4 x 64, 4 WRITEs | native | 4801, 4399, 486 | 15, 4, 2 |
| 4 x 64, 4 WRITEs | NVMe/TCP | 1761, 55357, 6009 | 0, 191, 6 |
| 4 x 64, 3 WRITEs | native | 891, 12000, 27211 | 2, 11, 46 |
| 4 x 64, 3 WRITEs | NVMe/TCP | 2793, 4476, 20243 | 2, 2, 35 |

16. **Rings of 4 x 128, a 256 KiB buffer and a budget of four WRITEs
    hold over three runs**: sixteen connections' loans at 97% of their
    copies on the native drive and 109% on NVMe/TCP, with no ring drop
    and no timeout in any run, at 74% of the L3. One connection is
    behind its copy there on the native drive (1602 and 1715 against
    1982).
17. **Rings of 4 x 64 fall short of level at sixteen connections**:
    95% of the copy on NVMe/TCP at three and at four WRITEs, 84% and
    89% on the native drive. The pages in use repeat to within 40.
18. **The drops at 4 x 64 do not stay near 0.03%.** One setting's
    three runs differ thirtyfold; the worst are about 0.15% of the
    frames (three WRITEs, native) and 0.25% (four WRITEs, NVMe/TCP,
    with 191 timeouts). The rate held through each: that run made
    2181 MB/s.
19. **The native drive's scatter was noise**: the four-WRITE cell at
    4 x 64 that ran at 1620 ran at 2200 and 2009. A copy's own three
    runs span 15% (1978 to 2331 at 4 x 128).
20. **What a copy cell holds depends on what ran before it**: about
    600 pages at 4 x 128 straight after a ring change, 880 after loan
    cells have left pages in the pools. The loan cells' peaks do not
    move with it.

### What the storage takes, and what limits the cells

`xeu-rig/nvme-baseline.sh prp` and `sgl`
(`xeu-16k-nvme-baseline/`): fio on tardis1 itself, 1 MiB direct writes
at iodepth 16, on a scratch directory in `/hs/nvme0` and on an
NVMe/TCP loopback target made as the harness makes its own (a 4 GiB
file in `/hs/nvme1`); all removed after.

| job | native | NVMe/TCP |
|---|---|---|
| writes alone, one job, 120 s | 11207 MB/s | 4353 and 4440 MB/s (two runs) |
| writes alone, four jobs, 60 s | 11201 MB/s | 11210 MB/s |
| the step cell's own job, 120 s | 3034 MB/s | 1392 and 1389 MB/s |

21. **Neither storage limits a cell, and the drive does not slow as it
    is written**: 11.2 GB/s in each 10 s of 120 s on the native drive
    (1.3 TB). NVMe/TCP is limited by the one queue a job uses (4.4
    GB/s), not by its drive (11.2 GB/s with four jobs).
22. **The step cell's job limits itself, and so limits every cell
    above.** It is one fio thread that reads back and checksums each
    block it writes (`verify_backlog`): fio reports as many bytes read
    as written, and on tardis1 that thread is at 97% of a CPU at 3.0
    GB/s. In the cells it runs on the client, where it is at 93% to
    95% of a CPU in the best of them (78% user and 17% system in the
    4 x 128, four WRITEs, NVMe/TCP loan at 2353 MB/s; 76% and 17% in
    the native copy at 2232). So:
    - the MB/s of every table above is the write half of a load that
      is half reads;
    - the 2.1 to 2.35 GB/s at which the good cells of every ring size,
      buffer and storage agree is the client's fio thread;
    - "level with its copy" means both carry all that this load
      offers. A loan that is behind (budgets of one and two WRITEs, a
      128 KiB buffer, 4 x 64 on the native drive) is behind; a loan
      that is level may or may not be faster than its copy, and the
      budget that suffices here may not under more load.
    The footprints, drops and timeouts do not depend on this.
23. **`STEP_JOBS=<n>`** (`run-budget.sh`) makes the step cell n writers
    on a file each with no verify, which lifts that limit and gives up
    the check of the data. `plans/16k-6-noverify.plan` is the shortlist
    for it.

### The cells with the load uncapped: four writers, no verify

`STEP_JOBS=4 ./sweep.sh xeu-16k-nv4 plans/16k-6-noverify.plan`. Rings
of 4 x 128, 256 KiB buffer; one run each; MB/s. The client's fio
threads are at 2% to 6% of a CPU each.

| budget, WRITEs | native, 1 conn | native, 16 conn | NVMe/TCP, 1 conn | NVMe/TCP, 16 conn | pages | of the L3 |
|---|---|---|---|---|---|---|
| copy | 2767 | 4067 | 2575 | 8170 | 895 to 901 | 44% |
| 2 | | 1438 | | 1879 | 1390 | 68% |
| 4 | 2082 | 3054 | 2611 | 3119 | 1608 to 1612 | 79% |
| 8 | | 3442 | | 4271 | 2045 to 2051 | 100% |

No timeout, pause or `alloc_fail`; 113 and 77 ring drops in the two
fastest NVMe/TCP cells.

24. **With the load uncapped, the budget is what limits sixteen
    connections' loans.** On NVMe/TCP the loan runs at 1879, 3119 and
    4271 MB/s at two, four and eight WRITEs where its copy, which
    nothing admits or refuses, runs at 8170. Finding 16 ("four WRITEs
    are level") was the verifying job's ceiling and does not hold
    here. One connection is level at four WRITEs on NVMe/TCP (2611
    against 2575).
25. **On the native drive the loan is behind its copy at every
    budget** (35%, 75% and 85% at two, four and eight WRITEs), and
    the native copy is half of NVMe/TCP's (4067 against 8170) though
    both are written direct and the drive takes 11.2 GB/s. Why is not
    known.
26. **Where the loan is written direct it costs less server CPU**:
    0.59 to 0.67 kernel CPU-seconds per GB for the sixteen-connection
    loans on NVMe/TCP against 0.99 for their copy. One run each, at
    different rates.

## Hero. Writes from the client as fast as they will go (Mike, 2026-10-08)

**This is the primary focus from the evening of 2026-10-08, until every
win is wrung out and NFS writes from the remote client are near line
rate.** It is not the sizing hunt: the footprint is allowed to be
several times the L3. The sizing results above stand as they are.

What there is to reach for, all measured at 16 KiB on this boot:

| limit | rate |
|---|---|
| the wire: 16 TCP streams to a receiver that discards, buffers autotuned | 48 GB/s (380 Gbit/s) |
| the wire, 16 streams, buffers held at 128 KiB | 11.9 GB/s |
| one native drive, fio on tardis1 | 11.2 GB/s |
| the NVMe/TCP loopback target, one queue / four | 4.4 / 11.2 GB/s |
| NFS, best so far: copy, 16 connections, four writers, NVMe/TCP, 256 KiB held | 8.2 GB/s |
| NFS, the same on the native drive | 4.1 GB/s |

The brakes known to be on, in the order the ladder takes them off:

1. **The load.** The verifying step job is one thread; four writers
   without verify took the copy from 2.2 to 4.1 and 8.2 GB/s. More
   writers: `STEP_JOBS`.
2. **The held receive buffer.** Holding it took the wire from 48 to
   12 GB/s. `FORCE_RCVBUF=0` (a buffer of 0 in a plan) leaves it
   autotuned.
3. **The rings.** 4 x 128 was chosen for its footprint; 4 x 256 is the
   size that did not drop under autotuned streams.
4. **The budget.** A loan's rate at sixteen connections follows it
   (finding 24). Sixteen WRITEs admit every connection at once.
5. **One drive.** 11.2 GB/s. Past that needs more than one drive under
   the export: an MD stripe of four to six (Mike raised it; it means
   remaking drives, which the rig's rule forbids until he says) or
   several exports, one per drive (a harness change).

Not limits now: nfsd has 128 threads on 64 CPUs; the client's fio
threads idle with the verify off.

**The ladder** is `xeu-rig/plans/16k-7-hero.plan`, sixteen connections,
one thing changed per line:

    STEP_JOBS=16 NULLB_MIB=16384 ./sweep.sh $L/xeu-16k-hero plans/16k-7-hero.plan

| rung | rings | buffer | budget | storage | what it adds |
|---|---|---|---|---|---|
| 1 | 4 x 128 | 256 KiB | copy | native | sixteen writers |
| 2 | 4 x 128 | autotuned | copy | native | the buffer let go |
| 3 | 4 x 256 | autotuned | copy | native | the larger rings |
| 4 | 4 x 256 | autotuned | copy | NVMe/TCP | the other storage |
| 5 | 4 x 256 | autotuned | 16 WRITEs | NVMe/TCP | the loan, written direct |
| 6 | 4 x 256 | 1 MiB | 16 WRITEs | NVMe/TCP | the loan with the buffer held |
| 7 | 4 x 256 | autotuned | 16 WRITEs | native | the loan through the page cache |

Read each rung against the one before it, and read `alloc_fail`: with
autotuned buffers sixteen plain streams used 5900 of the region's 6144
pages, so the region may run dry (the driver's refill retry is what
makes that survivable; say if it happened). NVMe/TCP is a target on
the same host, so its cells also pay the target's CPU.

Where to look next, as written before the ladder ran (what it and the
runs after it found is "Hero, the first evening" below):

- If rung 1 is no faster than four writers, the limit is on the
  server or the wire and not the load: profile it (`PROFILE=1`).
- Why the native copy is half of NVMe/TCP's (finding 25). The native
  export is a directory of a file system that is 68% full and the
  files are made by the cell; `PREALLOC=1` or a file system of its own
  would say whether allocation is the cost.
- More than sixteen connections: `nconnect` stops at 16, so more
  needs more mounts or more clients.
- A larger region and budget than this boot's 6144 pages
  (`rx_region_pages` is set once a boot, by `bringup.sh`).
- The second port, `enp164s0f0`, which Mike had taken down on
  2026-10-08 until one port is optimized.
- Past one drive: rung 5 of the brakes above.

### Hero, the first evening (2026-10-08)

All at sixteen connections with sixteen writers and no verify
(`STEP_JOBS=16`), one run each, MB/s. Loans are at a budget of sixteen
WRITEs (2150 pages). Output `/var/tmp/zc-run-logs/xeu-16k-hero`,
`-profile`, `-sync-*`, `-t512-r256`, `-async`, `-pagecache`.

**Where it stands.**

| | durable (the export sync) | not durable (export async, a diagnostic) |
|---|---|---|
| copy, native | 6704 | 10601 |
| copy, NVMe/TCP | 8339 | 11300 |
| loan, NVMe/TCP (written direct) | 7657 | 11212 |
| loan, native (through the page cache) | 6838 | 30858 |
| copy, native, through the page cache (`MODE=1`) | | 25182 |

The best of each over the runs below. One drive takes 11.2 GB/s; the
wire carried 48 GB/s.

**The ladder** (`plans/16k-7-hero.plan`, 128 nfsd threads, 1 MiB blocks):

| rung | rings | buffer | cell | MB/s | pages | tail drops | timeouts | `alloc_fail` |
|---|---|---|---|---|---|---|---|---|
| 1 | 4 x 128 | 256 KiB | copy, native | 4755 | 782 | 2 | 0 | 0 |
| 2 | 4 x 128 | autotuned | copy, native | 5407 | 6144 | 494980 | 380 | 4180 |
| 3 | 4 x 256 | autotuned | copy, native | 5351 | 6144 | 474672 | 2849 | 67822 |
| 4 | 4 x 256 | autotuned | copy, NVMe/TCP | 7979 | 6144 | 159917 | 1305 | 28392 |
| 5 | 4 x 256 | autotuned | loan, NVMe/TCP | 2485 | 6144 | 590349 | 6239 | 44519 |
| 6 | 4 x 256 | 1 MiB | loan, NVMe/TCP | 4713 | 4341 | 20 | 0 | 0 |
| 7 | 4 x 256 | autotuned | loan, native | 2462 | 6144 | 663349 | 6611 | 53743 |

H1. **An autotuned receive buffer is a dead end with this region.** The
    sockets' queues take every page of it (6144 of 6144), the card
    drops half a million frames, and it gains a copy 14% at most. A
    loan cell with no page left loans nothing (`admitted` 0, rungs 5
    and 7) and collapses under timeouts. Hold the buffer.
H2. **The driver's refill retry works under it**: five runs emptied the
    region (`alloc_fail` 198656 in all) and each time every ring came
    back by itself.
H3. **More writers alone add little** (4067 with four, 4755 with
    sixteen): the load was not the native copy's limit.

**Where nfsd's threads are** (`xeu-rig/nfsd-stacks.sh`: every nfsd
thread's kernel stack once a second for 20 s; copies at 4 x 128,
256 KiB, 128 threads; `plans/16k-8-profile.plan`):

| the thread is | native (5595 MB/s) | NVMe/TCP (7330 MB/s) |
|---|---|---|
| waiting for its direct write (`blk_io_schedule`) | 37% | 25% |
| in the fsync after it, on the XFS log (`xlog_wait_on_iclog`, `xlog_wait`) | 44% | 49% |
| in the fsync after it, on the device's cache flush (`blkdev_issue_flush`) | 15% | 20% |
| idle | 2% | 1% |
| receiving (`tcp_read_sock`) | under 1% | under 1% |

H4. **Every WRITE is followed by an fsync, and the threads spend 60%
    to 70% of their time in it**: `iomap_dio_complete` to
    `vfs_fsync_range` to `xfs_file_fsync`, a log force and a cache
    flush. No thread is idle, so the rate is the thread count over the
    time of one WRITE. The CPUs are 40% idle.
H5. **The native drive is the slower because its flush is**: 4800
    flushes a second at 14 ms each, against 5700 at 2 ms on NVMe/TCP
    (`iostat`). The WD_BLACK SN8100 has a volatile write cache
    (`write_cache` is `write back`). The local fio that took 11.2 GB/s
    issued none. This is the gap of finding 25.
H6. Why each WRITE is stable was not traced. The likely cause: the job
    writes 1 MiB blocks with `direct=1` over a mount with a `wsize` of
    1 MiB, so a block is one WRITE, and the Linux client sends a direct
    write that fits one request as FILE_SYNC. `io_cache_write` is 2
    (direct) in the cells, which sets no floor of its own.
H7. The flat profile's largest item is the copy: `__memmove` in nfsd,
    17% of all cycles in the native copy.

**Threads and block size** (`plans/16k-9-sync.plan`, rings of 4 x 128;
copies hold 256 KiB, loans 1 MiB):

| cell | 128 threads, 1 MiB | 512 threads, 1 MiB | 128 threads, 8 MiB | 512 threads, 8 MiB |
|---|---|---|---|---|
| copy, native | 4755, 5595 | 6063, 6102 | 6277 | 6704 |
| copy, NVMe/TCP | 7330 | 8339, 7840 | 4734 | 6867 |
| loan, NVMe/TCP | | 6242 | 7401 | 7657 |
| loan, native | | 4126 | 5134 | 6838 |

H8. **512 nfsd threads and 8 MiB blocks each help the loans and the
    native copy, and add up** (`/proc/fs/nfsd/threads`; `STEP_BS=8M`
    sends a block as eight UNSTABLE WRITEs and a COMMIT). Sixteen
    writers at iodepth 16 keep 256 WRITEs in flight at 1 MiB, so more
    than 256 threads cannot be busy there.
H9. **The NVMe/TCP copy is slower with 8 MiB blocks** (4734 and 6867
    against 7330 to 8339). Not explained.
H10. **A 1 MiB buffer wants rings of 4 x 256.** At 4 x 128 the loans
    drop 42000 to 483000 frames; at 4 x 256 with 512 threads and 1 MiB
    blocks (`xeu-16k-t512-r256`) the NVMe/TCP loan runs at 6235 with
    2332 drops and the native at 4770 with 128, about 4400 pages in
    use. Their copies at the same rings and buffer: 7295 and 4983, so
    the loans are at 85% and 96%. A 1 MiB buffer does not help a copy
    (7295 against 7840, 4983 against 6102 at 256 KiB).

**With no WRITE made durable** (`EXPORT_SYNC=async`, 512 threads, 1 MiB
blocks; `plans/16k-10-async.plan`). A diagnostic: nfsd answers before
the data is safe, which no server that keeps its promise does.

| cell | rings, buffer | MB/s | written | tail drops | kernel CPU-s per GB |
|---|---|---|---|---|---|
| copy, native | 4 x 128, 256 KiB | 9381 | direct | 0 | 0.49 |
| copy, NVMe/TCP | 4 x 128, 256 KiB | 10954 | direct | 3690 | 0.75 |
| copy, native | 4 x 256, 1 MiB | 10601 | direct | 0 | 0.39 |
| copy, NVMe/TCP | 4 x 256, 1 MiB | 11300 | direct | 0 | 0.70 |
| loan, NVMe/TCP | 4 x 256, 1 MiB | 11212 | direct | 0 | 0.52 |
| loan, native | 4 x 256, 1 MiB | 30858 | page cache | 395 | 0.52 |

H11. **The fsync of each WRITE costs 30% to 50%**: the copies go from
    6102 and 7840 to 9381 and 10954 with nothing else changed.
H12. **Without it, all that is written direct stops at one drive**,
    10.6 to 11.3 GB/s against the 11.2 the drive takes.
H13. **At that ceiling the NVMe/TCP loan is level with its copy on a
    quarter less server CPU** (11212 against 11300; 0.52 against 0.70
    kernel CPU-seconds per GB), same rings and buffer.
H14. **The native loan's 30.9 GB/s (247 Gbit/s) is not what the drive
    took.** It goes through the page cache, and with nothing forcing
    it out the cell overwrites the same 8 GiB of files in memory; the
    drive was writing 10.1 GB/s the while (`iostat`). It is what the
    wire and the receive path carry with the storage out of the way.

**The receive path alone: copy against loan, both through the page
cache** (`EXPORT_SYNC=async MODE=1`, `plans/16k-11-pagecache.plan`,
rings of 4 x 256, 512 threads):

| buffer | copy | loan | loan of copy | kernel CPU-s per GB, copy / loan | CPUs idle, copy / loan |
|---|---|---|---|---|---|
| 1 MiB | 24915 | 30552 | 123% | 1.19 / 0.56 | 50% / 70% |
| 256 KiB | 25182 | 21226 | 84% | 0.85 / 0.50 | |

H15. **With the storage out of the way and a 1 MiB buffer, the loan
    carries 23% more than the copy on less than half the server CPU**
    (30.6 against 24.9 GB/s; 0.56 against 1.19). With a 256 KiB buffer
    the loan is the slower (21.2 against 25.2), still on less CPU.
H16. **At 25 to 31 GB/s the server is not the limit**: 95% to 97% of
    the 512 nfsd threads are idle and half or more of the CPUs. The
    client has 11 to 14 CPUs busy and its fio jobs are at 16% to 23% of
    a CPU each. What limits there is not found.

**What stands between this and line rate, in order:**

1. **One drive** (11.2 GB/s). Every durable figure and every direct one
   is under it. More drives under the export is Mike's to decide: an MD
   stripe remakes drives.
2. **The fsync of each WRITE** on drives whose flush is slow (H4, H5,
   H11). The ways round it: a workload that sends UNSTABLE WRITEs and
   COMMITs (larger blocks, or buffered writers); drives with power-loss
   protection; or, to see it only, telling the kernel a drive's cache
   is write-through, which Mike must decide since `/hs/nvme0` holds
   1.3 TB of other data.
3. **nfsd threads**: 128 are all busy under a durable load.
4. **Above 30 GB/s, something not yet found** (H16): the client, its
   sixteen connections, or TCP with held buffers.

### The wall near 25 GB/s is the receive copy (2026-10-08, late)

Mike, 2026-10-08: about half of line rate is the norm for writes on
this hardware, so 25 GB/s is where it has always stood; what is wanted
is a way through it. This is the observation that points at one.

**The copy stops at 25 to 26 GB/s whatever is under it, and the loan
goes past on less than half the CPU.** With the storage out of the way
(one drive, page cache, export async) the copy ran at 24.9, 25.2, 26.0
and 26.3 GB/s across two buffers and sixteen or thirty-two writers,
and the loan at 30.6 and 30.9 on 0.56 kernel CPU-seconds per GB
against the copy's 1.19 (H15). With six drives under it and every
WRITE durable the copy is at the same place, 24.7 GB/s, and the loan
at 32.4. So the wall is not storage, not nfsd's threads (95% idle
there) and not the number of writers. It is something the copy loads
and the loan does not, per connection or per receive CPU.

`xeu-rig/where.sh` during the native copy on the stripe (8 MiB blocks,
rings of 4 x 256, 1 MiB buffer, about 28 GB/s on the wire over the
10 s; `xeu-16k-where-stripe-prp-copy.*`) says what:

| what | measured |
|---|---|
| server cycles in `__memmove` | 49.5% of all |
| server cycles in `rwsem_spin_on_owner` | 11.0% |
| receive per ring | 10.2, 8.6, 5.6 and 3.5 GB/s |
| the two busiest ring CPUs (0 and 2) | 89% and 90% busy, 70% to 75% of it softirq |
| every one of the client's 16 connections | 49% to 57% of its busy time waiting on the server's window (`rwnd_limited`, `ss -ti`) |
| the server's CPUs overall | 40% idle |
| the client's CPUs overall | 71% idle; two at 100% |

H17. **Half of the server's cycles are the copy, and the client waits
    on the server.** Each connection is received by one nfsd thread at
    a time, which copies the record out; while it copies, the window
    stays shut. Sixteen connections each about half-limited by the
    window is the 25 GB/s.
H18. **Four rings are not enough and are not evenly fed.** Sixteen
    connections hash 10.2 : 8.6 : 5.6 : 3.5 across them, and the two
    CPUs that take the most are at 90%. The port was put at four rings
    for its footprint (`bringup.sh`); it comes up with eight.
H19. `rwsem_spin_on_owner` at 11% is a lock nfsd's threads spin on in
    the write. Which lock was not traced.

### Correction, 2026-10-09: on the stripe nothing is written direct

Found on the morning of 9 October, when Mike asked whether a loan is not
written with O_DIRECT in the end. **Every native cell on `/dev/md0`,
copy and loan alike, was written through the page cache
(`IOCB_DONTCACHE`), and none direct**: `wr_direct` is 0 in every one
(`summary.tsv`; the best copy 2923544 WRITEs dontcache, the best loan
3402632). On the single drive a copy was written direct (H12, 16K
finding 7); it was taken to be so on the stripe and not looked at.

Why: `md0` reports a `dma_alignment` of 511 where its NVMe members
report 3 (`/sys/block/*/queue/dma_alignment`), so nfsd's memory
alignment for direct I/O there is 512 bytes and not 4
(`xfs_io -c 'statx -m 0x2000'`: `dioattr.mem = 0x200`). A copied WRITE's
data starts part-way into its first page, after the RPC header, at an
offset that is a multiple of 4 and not of 512, and `nfs_dio_split()`
issues the WRITE buffered (`NFS_DIO_MEM_MISALIGNED`). Seen with the
tracepoint on the RAM disk, which asks for 512 too: 6311 of 6311
`nfsd_write_dio_split` events `mem_align=512 ... disposition=mem_misaligned
dontcache=1`, with `mem_offset` 300, 1920, 6976. A stripe made of 32
null_blk devices (2 each) reported 511 as well: it is the stacking of
the limits, not these drives.

So, for everything from "Six drives" on:

| on the stripe | what it was |
|---|---|
| copy, native | **two copies**: out of the receive pages, then into the page cache; written back, not direct |
| loan, native | **one copy**: into the page cache; written back |
| copy and loan, NVMe/TCP | written direct, through the target on the same host |

C1. **The 30.1 against 25.4 GB/s of N1 is one copy against two, both
    through the page cache.** It stands as measured. It is not a
    zero-copy write against a direct one, and "half of the server's
    cycles are the copy" (H17) was two copies.
C2. **The only direct writes on the stripe are NVMe/TCP's**: the copy
    24.8 GB/s and the loan 20.2 to 21.1 (flushes sent or not, 8 MiB
    writes).
C3. **An MD or device-mapper stripe takes away direct I/O from nfsd's
    copy as well as from a loan**, on drives that would take either
    direct by themselves. A loan needs 4-byte alignment (`rx_ip_align=2`)
    and a copy's first piece has no more. Nothing in the tree says so.
    The limit is read-only at runtime; a stacked device taking its
    members' alignment is a change to the block layer.
    (Made, and booted as build 461 on 9 October: see "Build 461"
    below. A copy on the stripe is written direct there.)
C4. The trim that the stripe needs (N1), the cost of the COMMITs and a
    stage that does not pass 30 GB/s whatever the budget, the rings,
    the files or the connections (N6, N7, N9 to N11, and 64 files with
    22 WRITEs: 30.9) are all of a path that puts every byte through
    the page cache and writeback.

Storage tried for a loan written direct with no hop in front of it, and
why each will not do: a memory-backed null_blk has no boundary and an
alignment of 2 but copies under one lock (6.0 GB/s for copy and loan,
`xeu-16k-nullb`); a stripe of 32 of them reports 511; brd (44 GB/s to
fio) asks for 512, so copy and loan both go through the page cache
there (19.6 and 21.8 GB/s, `xeu-16k-brd`). On this host only NVMe/TCP
takes a loan direct.

### Six drives: `/dev/md0` (2026-10-08, evening)

Mike had `nvme2` to `nvme7` striped: MD raid0, a 1 MiB chunk (the size
of a WRITE and of fio's block, so an aligned WRITE is one request to
one drive; `max_sectors_kb` is 1024), a 6 MiB stripe, XFS with
`sunit` and `swidth` to match, mounted at `/hs/stripe` and not in
`/etc/fstab`. Their 1.3 TB each is gone; their lines in `/etc/fstab`
and `/etc/exports` are commented out with `#stripe 2026-10-08-stripe#`
and the files as they were are `/etc/fstab.2026-10-08-stripe` and
`/etc/exports.2026-10-08-stripe`. `mdadm` was installed for it. The
stripe needs whole pages as its drives do (`virt_boundary_mask` 4095),
and it asks for 512-byte memory alignment where they ask for 4: **see
the correction above, nothing native here was written direct.**
The drives' volatile write cache is on and the kernel sends flushes
(`write back`) in every run below.

fio on tardis1 (`PRP_BASE=/hs/stripe JOBS=16 nvme-baseline.sh prp`):
27.9 GB/s to one writer and **67.3 GB/s to sixteen**, six times one
drive and more than the wire carried.

Sixteen connections, sixteen writers, 512 nfsd threads, the export
sync, MB/s (`PRP_DIR=/hs/stripe/zcbudget
SGL_BACKING=/hs/stripe/zcbudget-nvmet.img`; `xeu-16k-stripe-sync`,
`-sync-bs8m`):

| cell | rings, buffer | 1 MiB blocks | 8 MiB blocks | one drive, best |
|---|---|---|---|---|
| copy, native | 4 x 128, 256 KiB | to rerun | 24702 | 6704 |
| copy, NVMe/TCP | 4 x 128, 256 KiB | 20325 | 24763 | 8339 |
| copy, native | 4 x 256, 1 MiB | 22305 | | 4983 |
| copy, NVMe/TCP | 4 x 256, 1 MiB | 19730 | 22496 | 7295 |
| loan, NVMe/TCP, 16 WRITEs | 4 x 256, 1 MiB | 10647 | 20182 | 7657 |
| loan, native, 16 WRITEs | 4 x 256, 1 MiB | to rerun | **32370** | 6838 |

H20. **Durable copies reach 24.7 GB/s on the stripe** (198 Gbit/s;
    the native one through the page cache, the correction above),
    where one drive gave 6.7 to 8.3, and stop at the copy's wall.
H21. **The native loan, durable, runs at 32.4 GB/s** (259 Gbit/s, two
    thirds of what the wire carried) with 8 MiB blocks: 31% over its
    copy. It is written through the page cache and each 8 MiB block is
    committed. 4780 ring drops, no timeout, 4229 pages in use.
H22. **The NVMe/TCP loan is the slow one** (10.6 and 20.2 GB/s): it is
    written direct to a target on the same host, one more TCP hop
    through the same CPUs. It says little about a drive with SGLs.
H24. **A copy costs less CPU with little of the region in use, and
    goes no faster.** Server kernel CPU-seconds per GB on the stripe
    with 8 MiB blocks: the native copy 0.80 at rings of 4 x 128 and a
    256 KiB buffer (770 pages in use, 38% of the L3) and 1.32 at 4 x 256
    and 1 MiB (2055 pages, all of the L3), at 24.7 and 24.8 GB/s;
    NVMe/TCP's 1.01 and 1.47. The diagnostic of H15 had the same
    (0.85 and 1.19). One run each, and the rings and the buffer differ
    as well as the pages in use; cache misses were not counted. The
    native loan beside them: 0.53 at 32.4 GB/s with 4229 pages in use.
H23. Two native cells of the 1 MiB run failed in the harness, which
    asked `/dev/md0` for an NVMe controller; fixed, and they are to be
    rerun.

### Hero, the night of 8 to 9 October: what holds the loan

Mike, 2026-10-08: avoid the copy; find what holds the loan back. All on
the stripe's native storage (a loan is written through the page cache
there, **and so is a copy: the correction below "The wall near 25
GB/s"**), sixteen connections, sixteen writers, 512 nfsd threads, 8 MiB
writes unless said. Output `/var/tmp/zc-run-logs/xeu-16k-*`; the chain
is `xeu-16k-night-driver.sh` and the drivers after it.

**The result that stands, three runs each** (`xeu-16k-best-trim-a`,
`-b`, `-c`: the stripe trimmed before every cell, flushes sent, nothing
traced):

| | run a | run b | run c | median |
|---|---|---|---|---|
| copy, rings of 4 x 128, 256 KiB buffer | 25539 | 25360 | 25142 | 25360 |
| loan, 16 WRITEs, rings of 4 x 256, 1 MiB buffer | 29727 | 30054 | 30357 | 30054 |

N1. **The loan carries 30.1 GB/s of durable writes against the copy's
    25.4, 18% more, and it repeats** (one copy against two, both
    through the page cache: C1) (29.7 to 30.4; 25.1 to 25.5). The
    loan holds about 4200 pages of the region, the copy 770. The
    32.4 GB/s of H21 was one run and was not seen again.

**The stripe must be trimmed.** Its file system is not mounted with
discard and every cell deletes its files. Untrimmed after about 150 TB
the same cells gave the copy 22.6 to 23.3 and the loan 23.6 to 24.8,
level; one `fstrim` (12 s) later, 25.4 and 28.7. fio on tardis1 did not
show it (66.9 GB/s before the trim, 67.1 after, 60 s). `TRIM=/hs/stripe`
makes `sweep.sh` trim before every line.

N2. **A loan that looked bistable was the drives.** Through the evening
    the loan ran at 30 to 32 GB/s in four cells and 22.8 to 26.0 in
    seven, and the cells with `where.sh` and `anat.bt` inside them ran
    low, which was put down to the tracing. It was not: traced cells on
    the trimmed stripe ran at 29.4 to 30.6. Figures from 22:00 to
    00:45 on 8 to 9 October (`xeu-16k-anat-*`, `-budget`, `-mounts*`,
    `-best-noflush-*`, `-best-flush-*`) compare within a run and not
    with others.

**What a loaned WRITE's time is made of** (`anat.bt` and `where.sh`
inside the cell, `capture-sweep.sh`; the loan at 16 WRITEs, 4 x 256,
1 MiB):

| | measured |
|---|---|
| admissions that had first been refused | 98% to 100% |
| WRITEs in flight at a refusal | 16 in 87%, 15 in 10% |
| a WRITE holds its credit for | 0.25 to 1 ms |
| a refused connection waits | 0.25 to 0.5 ms |
| the client's connections waiting on the server's window | 92% to 96% of their busy time |
| server CPUs idle | a third or more |
| `__memmove` (the copy into the page cache) | 28% of cycles |

N3. **At sixteen WRITEs the budget is full all the time**: a
    connection has one WRITE in flight, and must receive it, write it
    and give its credit back before its next is let in. The client
    waits on the server's window while it does.
N4. **A WRITE's hold grows with how many are in flight** (mostly 0.25
    to 0.5 ms at 8 and 12 WRITEs, 0.5 to 1 ms at 22 and 24), so the
    rate does not follow the budget: halving it to 8 cost 17%.
N5. **A larger buffer shortens the hold**: with 256 KiB most holds are
    0.5 to 1 ms, with 1 MiB they are spread from 0.25 ms, because less
    of the record has still to cross the wire once it is admitted.

**The budget and the buffer, rings that do not drop** (4 x 256,
`xeu-16k-budget256`, traced, the stripe trimmed at the start):

| buffer | WRITEs | MB/s | client waiting on the window | the busiest ring | its CPU in softirq |
|---|---|---|---|---|---|
| 1 MiB | 12 | 26655 | 96% | 14.2 GB/s | 80% |
| 1 MiB | 16 | 29383 | 96% | 14.6 GB/s | 81% |
| 1 MiB | 18 | 29426 | | | |
| 1 MiB | 20 | 29918 | | | |
| 1 MiB | 22 | 30569 | 41% | 14.9 GB/s | 88% |
| 1.5 MiB | 16 | 30022 | 96% | 14.9 GB/s | 80% |

N6. **Past sixteen WRITEs the budget buys 4%.** At 22 the client no
    longer waits on the server and the rate is the same.
N7. **One ring takes half of the traffic and its CPU is at 80% to 88%
    in softirq.** Sixteen connections do not hash evenly over four
    rings (`ethtool -S`: for example 7.2, 9.5, 14.6 and 4.7 GB/s). That
    is the loan's next limit on this boot, ahead of the budget.
N8. **Rings of 4 x 128 cannot carry a 1 MiB buffer**: 0.3 to 2.1
    million frames dropped a cell and hundreds of timeouts
    (`xeu-16k-budget`), which spoils the larger budgets there.

**Tried and of no use:**

N9. **More connections** (`STEP_MOUNTS`, a second and a fourth address
    of the server on the port, 32 and 64 connections;
    `xeu-16k-mounts*`): the copy 18.3 and 24.1 GB/s at sixteen, 12.8
    and 19.7 at 32, 21.1 and 18.8 at 64; the loan 27.1 at sixteen, 15.0
    at 32; ring drops and timeouts up with the count (3285 timeouts in
    one 64-connection copy). One budget shared by more connections
    leaves more of them waiting.
N10. **Eight rings of 256** for the copy (25.0 and 23.9 against 25.5),
    and the loan does not fit: eight rings of 256 leave 3184 pages
    free.
N11. **More files** (64 writers at depth 4 for 16 at depth 16): the
    copy 25.8, the loan 31.1, as before. The lock nfsd spins on
    (`rwsem_spin_on_owner`, 5% to 11% of cycles) is not what limits.
N12. **Not sending the flush** (the kernel told `write through` on the
    members and `md0`, the drives' cache on: as a drive with power-loss
    protection): with 1 MiB writes the copy gained 16% to 22% (27.1
    GB/s) and the loans 78% to 130% (18.7 and 19.0); with 8 MiB writes
    nothing moved. It matters to a client that syncs every megabyte.
N13. **The client's write size decides the native loan**: 8.1 GB/s
    with 1 MiB writes and flushes sent, 18.7 with none sent, 30 with
    8 MiB writes. A 1 MiB direct write is one FILE_SYNC WRITE, which
    nfsd maps to `IOCB_DSYNC | IOCB_SYNC` (`nfs3proc.c`), an fsync of
    page-cache data and of the inode after every WRITE.

**Files that grow** (`STEP_SIZE=256G`, so no write is an overwrite;
checked on the server; `xeu-16k-stripe-grow*`, before the trim):

N14. **Growing files do not collapse on the stripe.** With 8 MiB
    writes the copy ran at 26.3 and 23.3 GB/s and the loan at 26.0;
    with 1 MiB writes the copy at 22.7 and 20.7 (27.1 on files that
    have their size) and the loan at 17.1. XFS's wait for direct
    writes in progress, 95 ms a write on one drive at 64 KiB, costs
    15% to 20% here at most.

**The region.** It cannot be resized on this kernel
(`net/core/page_pool.c`: one static region, made with the first pool,
never freed). "page_pool: Give the fixed receive region back when it
is resized" at the tip gives it back once no pool is
bound and the size asked for has changed; built, not booted. N6 and N7
say a larger region would let the larger buffer and budget be had
together but that the receive side is the nearer limit.

**The drives.** About 230 TB were written on 8 October and 9 October up
to 01:00, most of it to the six striped drives: each went from about 75
to about 95 TB written in its life, 3% to 4% of its rated endurance
used, no media error (`xeu-16k-smart-*.txt`).

### The last runs on build 459 (9 October, 01:00 to 06:20)

All the native loan on the trimmed stripe (through the page cache, as
the correction says), sixteen connections, 8 MiB writes, 512 nfsd
threads, flushes sent, unless said. Each is a thing that might have
held the loan at 30 GB/s and did not.

**The budget, untraced** (`xeu-16k-budget256-trim`, rings of 4 x 256):
29.8, 29.7, 30.8 and 30.5 GB/s at 16, 18, 20 and 22 WRITEs with a
1 MiB buffer; 30.7 at 16 with 1.5 MiB; 27.7 at 12; the copy at the same
rings and buffer 23.4.

**The wire at each held buffer** (`wirebuf.sh`, sixteen plain TCP
streams to a receiver that discards, rings of 4 x 256;
`xeu-16k-wirebuf.tsv`):

| the server's receive buffer | MB/s | ring tail drops | region pages in use |
|---|---|---|---|
| 256 KiB | 24267 | 0 | 1803 |
| 512 KiB | 45031 | 0 | 1835 |
| 1 MiB | 49314 | 24 | 2080 |
| 2 MiB | 49487 | 87 | 2974 |
| 4 MiB | 48514 | 900047 | 3700 |
| not held | 49203 | 455433 | 4794 |

One stream: 6364 MB/s at 1 MiB, 7369 not held.

L1. **With a 1 MiB buffer held the wire is full (49 GB/s) and loses
    nothing.** A loan at 30 is not at a ceiling of its transport. A
    256 KiB buffer is one: 24.3 GB/s, which is where the copy cells
    that hold 256 KiB stand (25.4).

**Rings** (`xeu-16k-r8x128`, `-rps`, `-deeprings`; traced):

| rings | cell | MB/s | ring tail drops | the busiest ring | its CPU in softirq |
|---|---|---|---|---|---|
| 8 x 128 | loan, 16 WRITEs, 1 MiB | 30430 | 383421 | 7.9 GB/s | 61% |
| 8 x 128 | loan, 22 WRITEs, 1 MiB | 31849 | 618070 | 8.1 GB/s | 65% |
| 8 x 128 | copy, 256 KiB / 1 MiB | 23481 / 25288 | 0 / 235292 | | |
| 8 x 256 | loan, 16 WRITEs, 512 KiB | 26280 | 0 | | |
| 4 x 256, RPS to CPUs 8 to 63 | copy / loan | 23189 / 30372 | 0 / 120151 | | |
| 4 x 512 | copy, 1 MiB | 21361 | 0 | | |
| 4 x 1024 | copy, 1 MiB / 512 KiB | 21392 / 21505 | 0 / 0 | | |
| 4 x 512 | loan, 16 WRITEs, 768 KiB | 29365 | 0 | 10.9 GB/s | 72% |
| 4 x 256 | loan, 16 WRITEs, 1 MiB | 30277 | 6 | 9.2 GB/s (all four even) | 63% |

L2. **Eight rings halve what the busiest ring takes and add 4%.** RPS
    adds nothing.
L3. **Rings deep enough to drop nothing change nothing**: the copy at
    4 x 512 and 4 x 1024 dropped no frame and ran at 21.4 GB/s; the
    loan at 4 x 512 dropped none and ran at 29.4. The frames dropped
    in other cells are not what limits either.
L4. **The client's congestion window near 60 segments is not loss.**
    It is the same in the cells that dropped nothing: a sender held by
    the receiver's window does not grow it. `rwnd_limited` is 97% in
    those cells.
L5. What this boot's 6144 pages hold: the pools keep about 970 pages
    on top of what the rings post, so 4 x 256 leaves about 4180 free
    after a load, 4 x 512 3120 and 4 x 1024 1072. A loan at 16 WRITEs
    and 1 MiB needs 3254.

**Files** (`xeu-16k-files64`): 64 writers at depth 4 with 22 WRITEs,
30.9 GB/s; with 16, 29.4. The file's lock is not it either.

**Storage with nothing slow under it** (`xeu-16k-nullb`, `-brd`; in
the correction above): a memory-backed null_blk 6.0, 5.8 and 5.6 GB/s
for the copies and the loan, all written direct, under its one lock;
brd 19.6 and 17.6 for the copies and 21.8, 21.8 and 18.8 for the loan
at 16, 22 and 8 WRITEs, all through the page cache.

L6. **What is left.** The loan stays at 30 GB/s with the client waiting
    on the server 97% of the time, the budget full, the server's CPUs
    mostly idle and nothing lost on the wire. Every such cell puts
    every byte through the page cache and writeback, which the next
    boot's raid0 alignment lets the copy, at least, avoid. With the
    export async the same loan ran at 36.0 (one run, 8 October).

The drives at 02:40 on 9 October: the six striped ones at 103 to 112 TB
written in their lives and 4% of their endurance used; the cells after
that added a few TB each.

### Build 461: the copy on the stripe is written direct (9 October, 12:40 to 13:10)

Mike built `7.1.13-21.hs.461.loanpages_pool.el9.aarch64+16k` from the
tip, with "block: Let a stacking device take its members' memory
alignment" in it, and rebooted. `kmod-16k-xsight` 2.0.1-4 came with the
build and carries the refill retry; it was checked and not rebuilt.

`/sys/block/md0/queue/dma_alignment` is **3**, as each of its six
drives reports, where build 459 said 511. The array assembled by
itself as `/dev/md0`. The region this boot is 32768 pages
(`./bringup.sh 32768 256`), bound with no trouble.

The best copy and the best loan of N1 again (`plans/16k-17-best.plan`),
the same load: sixteen connections, 8 MiB writes, 512 nfsd threads,
flushes sent, the stripe trimmed before every cell, 120 s cells, three
runs (`xeu-16k-461-best1` to `-best3`):

| cell | run 1 | run 2 | run 3 | build 459, three runs | how it was written |
|---|---|---|---|---|---|
| copy, 4 x 128, 256 KiB | 28070 | 27693 | 27314 | 25.4 GB/s (25.1 to 25.5) | `wr_direct` 3213288, 3170112, 3126648; none dontcache, none buffered |
| loan, 16 WRITEs, 4 x 256, 1 MiB | 30352 | 30326 | 30157 | 30.1 GB/s (29.7 to 30.4) | `wr_dontcache` 3474328, 3471328, 3452344; none direct |

MB/s. Over the 90 s window of each cell, the server's CPU-seconds
(system, softirq) and what that is per GB written:

| cell | system | softirq | CPU-seconds per GB | ring tail drops | RTOs | region pages in use at peak |
|---|---|---|---|---|---|---|
| copy | 867, 868, 867 | 347, 333, 337 | 0.48, 0.48, 0.49 | 0, 1144, 2 | 0, 0, 0 | 775, 770, 774 |
| loan | 1258, 1208, 1223 | 257, 251, 251 | 0.55, 0.53, 0.54 | 143483, 318903, 165369 | 35, 98, 60 | 4154, 4180, 4091 |

No cell ran the region dry (`alloc_fail` 0 throughout; the least free
was 28588 of 32768).

B1. **The stacking change does what it was written for: nfsd's copy on
    an MD stripe is written direct.** Every WRITE of the three copy
    cells, 9.5 million, went direct and none through the page cache.
    C3 of the correction no longer holds on this kernel for a copy.
B2. **Written direct, the copy carries 27.7 GB/s (27.3 to 28.1) where
    it carried 25.4 through the page cache: 9% more.** The three runs
    fall by 0.4 GB/s each in the order they were run; three runs do not
    say whether that is the drives or chance.
B3. **The loan is where it was: 30.3 GB/s (30.2 to 30.4) against 30.1.**
    It still goes through the page cache, as it must on drives that
    need whole pages; the alignment was never what kept it from a
    direct write here.
B4. **So the comparison the night was thought to be making, a direct
    copy against a loan through the page cache, is 27.7 against 30.3:
    the loan carries 9% more, not the 19% of N1.** N1's gap was one
    copy against two.
B5. **The direct copy now costs the server less CPU for each GB than
    the loan does: 0.48 CPU-seconds against 0.54** (system and softirq,
    all three runs each way). The loan's one copy into the page cache
    and its writeback cost more than the copy cell's one copy out of
    the receive pages. On this storage the loan's gain is rate, and its
    cost is CPU, 4100 region pages against 770, and frames dropped at
    the ring.
B6. **A copy holding a 256 KiB buffer ran at 27.3 to 28.1 GB/s, above
    the 24.3 that sixteen plain streams carried at that buffer
    (L1).** L1 read 24.3 as the ceiling the copy cells stood at; on
    this boot they pass it. `wirebuf.sh` has not been run on build 461,
    so whether the wire's figure moved or the plain streams never were
    the copy's ceiling is not known.

Not run on this boot yet: step 4 of the handoff (deeper rings, larger
buffers and budgets of 16 to 64 WRITEs in the room 32768 pages give),
and the test of "page_pool: Give the fixed receive region back when it
is resized" (the region has been set once and not resized). Mike asked
for a pause after these three runs. `smartctl` is not installed on
this boot's root, so the drives' wear was not read; the three runs
wrote about 18 TB to the stripe, 3 TB a drive.

### Build 461, traced: the rings, the pools, the locks and nfsd's outliers (9 October, 13:45 to 14:10)

After a call with Xsight's engineers (Mike's transcript,
`~/snitm/kernel/xsight_optimization_meeting_transcript.txt`). What they
said that bears on this rig: a buffer must be reused while it is still
in cache, so what is freed last should be allocated first (LIFO, as
DPDK does); one burst of jitter can push the buffers out of cache into
a slower state that does not recover, so try a load paced at the
source and raised in steps; flows reach the rings by the hash of
their five-tuples, so balance needs many more flows than rings and is
read from `ethtool -S` as deltas; more rings is more memory posted.

Two traced runs of `plans/16k-17-best.plan` (`xeu-16k-461-trace1`,
`-trace2`) and one traced copy with 64 connections
(`xeu-16k-461-copy-m4`), by `xeu-rig/trace-sweep.sh`: `where.sh`, the
pools' counters, `perf lock contention`, a profile with call graphs
and `nfsdlat.bt`. Traced, the copy ran at 27.7 and 27.7 GB/s and the
loan at 29.8 and 29.5 (27.7 and 30.3 untraced).

**The rings.** Sixteen flows on four rings fall as the source ports
happen to hash, anew at each mount:

| cell | ring 0 | 1 | 2 | 3 | tail drops |
|---|---|---|---|---|---|
| copy, 16 connections (trace1) | 25% | 6% | 31% | 37% | 0 in 10 s |
| loan, 16 connections (trace1) | 26% | 14% | 19% | 42% | 17480 in 10 s |
| loan, the last untraced cell, whole cell | 14% | 19% | 41% | 26% | 206600, every one on ring 2 |
| copy, 64 connections | 28% | 27% | 20% | 25% | 23915 in 10 s |

`xeu` gives no way to steer: `ethtool -x`, `-n rx-flow-hash` and
ntuple filters are not supported. (Corrected from the driver source,
9 October evening: ntuple takes exactly one rule, whose target can
only be a VF, so it cannot steer to a ring either. The SDK in the
module has an RSS redirection table and key that `xeu` does not
expose. See "rings, how the flows fall" under "Since then".) The Rx interrupts are on CPUs 0, 2,
4 and 6 and the Tx ones on 1, 3, 5 and 7; nfsd's threads are held to
CPUs 8 to 55 (`taskset -pc` of one), so nfsd is already off the
rings' CPUs. `irqbalance` is running.

**The pools reuse pages first in, first out.** Over 10 s:

| | copy | loan | copy, 64 connections |
|---|---|---|---|
| pages received into | 41.1 M | 43.9 M | 32.6 M |
| returned to the pool's ring (`recycle_ring`) | 41.08 M | 39.86 M | 32.32 M |
| returned to the lockless cache (`recycle_cached`) | 10868 | 4179 | 254438 |
| ring full, so back to the region (`recycle_ring_full`) | 8878 | 4.06 M | 41632 |
| allocations that went to the region (`alloc_slow`) | 555 | 253861 | 2603 |

(T1 is of the harness's setting and not of nfsd: see "The harness had
deferred skb frees off" below.)

T1. **Nearly every page goes back through the pool's `ptr_ring`,
    which is a FIFO as long as the receive ring** (`xeu` sets
    `pool_size` to the ring's size). The lockless cache, which is the
    LIFO, is used only by the pool's own NAPI CPU, and the pages are
    freed by nfsd on other CPUs. So a page freed now is reused after
    every page ahead of it in that ring and then every buffer posted
    ahead of it: the opposite of what Xsight described. The region's
    own free list is a stack, but only what overflows reaches it.
T2. **In the loan cell 9% of the returns overflow.** Each such page is
    unmapped for DMA (an xarray erase under its lock), put on the
    region's list under the region's one lock with interrupts off, and
    mapped again when a pool takes it back, 16 at a time: 400 thousand
    pages a second each way.
T3. **nfsd's copy buffers are not reused either.** In the copy cell
    the profile has nfsd releasing a batch of pages after each request
    (`__folio_batch_release` from `svc_recv`) and `alloc_pages_bulk`
    refilling: about 5% of the cycles, and the copy's destination is
    whatever the page allocator hands out. Why the pages are released
    is not read yet.

**Locks**, from `perf lock contention -a -b` over 10 s:

| lock, by caller | copy | loan | copy, 64 connections |
|---|---|---|---|
| the pool ring's producer lock (`page_pool_put_unrefed_netmem`), taken for each page | 88 k contended, 0.13 s waited | 568 k, 0.96 s | 178 k, 0.36 s |
| the queue of ready connections of nfsd's one thread pool (`__lwq_dequeue`) | 129 k, 0.77 s | 13 k, 0.09 s | 351 k, **5.25 s** |
| XFS inode lock (`xfs_ilock`) | 1.7 k, 0.01 s | 16 k read and 0.8 k write, 0.26 s by caller; **by lock 7.6, 3.1 and 2.8 s on three inodes** | 4.8 k, 0.31 s |
| a connection's send mutex (`svc_tcp_sendto`) | 3.8 k, 2.95 s, **one of 2.84 s** | 2.9 k, 0.04 s | not in the top ten |
| the region's lock (`page_pool_rx_region_put`) | not in the top thirty | 6.9 k, 0.013 s | not in the top ten |
| DMA index of a released page (`page_pool_release_dma_index`) | none | 45 k, 0.09 s | none |

T4. **The region's lock is not contended to speak of** (13 ms waited
    in 10 s), though it is taken with interrupts off 800 thousand
    times a second in the loan cell. What nfsd's threads do contend
    on is the pool ring's producer lock, once for each page: a loaned
    WRITE's pages are released one at a time (`skb_page_unref`).
    Releasing a WRITE's pages in one batch
    (`page_pool_put_netmem_bulk`) would take it once.
T5. **The loan through the page cache queues on each file's inode
    lock.** A buffered write takes it exclusively: 6.9% of all cycles
    are `rwsem_spin_on_owner` under `down_write`, and single waits
    reached seconds. A direct write takes it shared. That, and the
    copy into the page cache (28% of cycles), is the path a loan has
    on drives that need whole pages.
T6. **With 64 connections the wake-up of nfsd's threads becomes the
    contended lock**: 5.25 s waited in 10 s on the queue of ready
    connections of the one pool (`pool_mode` is pernode on a host with
    one node, so all 512 threads share it). Idle threads are woken
    last in, first out already.

**nfsd's time**, from `nfsdlat.bt` over 15 s (the mount is NFSv3):

| | copy, direct | loan, page cache |
|---|---|---|
| a WRITE in nfsd (`nfsd_write`) | 128 to 512 us for 87%; **4.4% take 2 to 8 ms** | 128 to 512 us for 96%; none over 4 ms |
| a COMMIT | 0.5 to 2 ms for 57%; **24% take 4 to 32 ms** | 0.5 to 2 ms for 52%; **33% take 4 to 64 ms** |
| RPCs over 10 ms | 4963 (4147 of them COMMITs) | 7879 (6575 COMMITs) |
| sending a reply | 4 to 32 us; 10 over 0.5 ms | 8 to 128 us; 350 over 0.5 ms |

T7. **The outliers are COMMITs**, which is these drives' cache flush;
    the direct WRITE has a second, smaller tail of 2 to 8 ms that the
    buffered one does not.
T8. **The copy is slow for a copy**: 16.2% of 64 CPUs' cycles are
    `memmove` in the receive copy at 27.7 GB/s, 2.7 GB/s for each CPU
    doing it; in the loan cell 28% at 29.5, 1.65 GB/s each, into the
    page cache. Both read what the card has just written and write to
    pages not lately used. This is the figure the Xsight advice is
    about, and T1 and T3 are where reuse is lost.
T9. **The copy is held by the server's receive**: the client's
    connections are `rwnd_limited` 98.5% of the time with sixteen
    connections (85 to 97% in the loan cell).
T10. **Sixty-four connections did not help the copy: 21.1 GB/s.** The
    rings were more even and the client waited on the window less
    (77%), but rings of 128 with 256 KiB buffers dropped 190306
    frames and there were 1296 RTOs. It is not a fair test of more
    connections: it wants deeper rings, which the 32768-page region
    has room for.
T11. `enable_irq()` called from `xeu_rx_poll` at the end of each poll
    accounts for 6.1% of the samples in the copy cell (time with
    interrupts off is charged to where they come back on). The rings'
    CPUs go idle between polls: they are not saturated.

Not run: a load paced at the source and raised in steps (the harness
has no rate setting yet); the copy with more connections on deeper
rings.

What each finding suggests (none of it done when this was written;
what came of each, as of the evening of 9 October, is in "Since
then" after the list):

- **sunrpc:** release a loaned WRITE's pages in one batch (T4).
- **page_pool:** for a pool bound to the region, a LIFO in place of
  the FIFO ring, or a much shorter ring so that pages overflow to the
  region's stack, with the DMA mapping kept while a page is in the
  region (T1, T2). `xeu`'s `pool_size` is the same knob from the
  driver's side and needs a kmod rebuild and a reload.
- **nfsd:** keep and reuse a thread's copy buffers when the write was
  direct (T3); a thread pool per CPU or several pools, so 512 threads
  do not share one queue of ready connections (T6).
- **storage:** on drives that need whole pages, copy and write direct;
  the loan's inode-lock queue and second-rate copy are the page
  cache's (T5, B5). A loan wants a drive that takes it direct.

**Since then (9 October, evening).** Every change below has run only in
copy mode, in build 462's three runs and all together. None has been
measured alone; that is step 6 of the README and waits for Mike.

- **sunrpc, batch release (T4): written, and it had a bug.** "SUNRPC:
  Return a loaned record's page-pool pages in batches"
  (`bdd837db04f1`) handed `page_pool_put_netmem_bulk()` 64 pages where
  its on-stack array takes 16. A loaned record was the first thing to
  run it, and tardis1 crashed in that loan cell ("tardis1 crashed in
  the loan cell" below). It is fixed at the tip, with a KUnit test and
  a `BUILD_BUG_ON`, and has not run since.
- **page_pool, LIFO (T1): done.** "page_pool: Recycle a region-bound
  pool's pages last in, first out" (`e2aa12b004e65`, in build 462)
  gives a pool bound to the region a stack in place of its ring, as
  large as the ring and under the ring's producer lock. In build 462's
  copy runs `rx_pp_recycle_ring_full` fell from 17278 to 178 and 111
  in 10 s, and `rx_pp_alloc_empty` from 1077 to 11 and 7. Neither can
  be put down to the stack alone: `rx_refill_thresh` 8 and deferred
  frees changed in the same runs, and a stack the ring's size fills no
  less often than the ring. `memmove` went from 30% to 27% of cycles,
  the only sign that reuse is warmer, and that is not isolated either.
  **The loan, where T2 had 9% of returns overflow, has not been
  measured with it**: the one loan run on build 462 is the one that
  crashed.
- **page_pool, a much shorter ring: not done.** The LIFO was chosen
  instead. `xeu` still sets `pool_size` to the ring's size, and no
  patch in `kernel-rpmtree/xsight/` changes that. Release 5's
  `rx_refill_thresh` is a different knob: how empty the receive ring
  gets before a poll refills it.
- **page_pool, the DMA mapping kept while a page is in the region (T2):
  not done.** A page that overflows its pool still goes through
  `page_pool_return_netmem()`: unmapped (`dma_unmap_page_attrs()`, and
  the xarray erase in `page_pool_release_dma_index()`), put on the
  region's list under the region's lock, and mapped again when a pool
  takes it back. In copy mode on build 462 the overflow is near zero,
  so there is little to gain there. Whether it is wanted depends on the
  loan cell, rerun on the fixed sunrpc: read `rx_pp_recycle_ring_full`
  and `alloc_slow` against T2's 4.06 million and 254 thousand in 10 s.
  If the overflow is still near 9%, keep the mapping on region pages,
  since the region is bound to one device. That is a `net/core` change
  and needs a kernel build.
- **nfsd, reuse a thread's copy buffers (T3): done, in sunrpc.** The
  cause was not the direct write. A part-received record parked on the
  socket and picked up by another thread had its pages freed and
  allocated again each time. "SUNRPC: Trade pages with the socket when
  a partial record changes hands" (`47f13c685c5b5`) exchanges them
  instead. In build 462 `__folio_batch_release`, `alloc_pages_bulk` and
  `free_unref_folios` are gone from the profile (S3). A socket now
  keeps up to the pages of its largest parked record until it closes;
  nothing bounds that yet.
- **sunrpc, the pages a socket keeps after the page swap: new, not
  done.** Since "SUNRPC: Trade pages with the socket when a partial
  record changes hands" (`47f13c685c5b5`), when a thread takes a parked
  record back (`svc_tcp_restore_pages()`), it swaps its own pages into
  the socket's `sk_pages[]` where it used to free them. They stay there
  for the next thread that parks a record on that socket.
  - **How much:** as many pages as the largest record that changed
    hands on the connection, so at most `svc_serv_maxpages()` (the
    largest RPC message plus 3 pages). For nfsd's 1 MiB that is about
    1 MiB a connection, at any page size.
  - **For how long:** until the connection closes.
    `svc_tcp_clear_pages()`, at close, walks every slot and frees them,
    so nothing leaks (checked 9 October against its early return,
    which is taken only on an invariant violation, with a warning).
  - **What they are:** ordinary page-allocator pages, the threads'
    `rq_pages`, not region pages. No shrinker reaches them.
  - **What changed:** before the patch a connection between records
    held none. Now every connection that ever received a large WRITE
    in more than one call keeps about 1 MiB. That is 16 MiB for this
    rig's sixteen connections. With nconnect=16 from a thousand
    clients it is about 16 GiB that comes back only as clients
    disconnect. The total has no bound.
  - **Ways to bound it, none written:**
    1. Free the kept pages when the connection goes quiet: a read
       that finds no data and no record under way. That keeps the
       swap inside a burst of WRITEs, which is where it pays.
    2. A cap on pages kept across all sockets, as a module parameter.
       Over the cap a restore frees, as before the patch.
    3. A shrinker over the kept pages.

    1 does most of it, and 2 is a cheap backstop. The linger makes a
    record change hands less often (2 calls for a WRITE where it was
    6), so with it on there is less for the swap to save, but the
    pages are kept all the same once a record has changed hands.
- **nfsd, several thread pools (T6): written, not shown to work.** "SUNRPC:
  Let a NUMA node have several thread pools" (`72766c5ac54ae`) adds
  `pools_per_node`; build 462 ran with 4. T6's contention was at 64
  connections and build 462 ran sixteen, where `__lwq_dequeue` is not
  near the top of the lock list (S6). The test is in the README: "Does
  `pools_per_node` earn its place".
- **storage (T5, B5):** unchanged. Build 461's MD stacking change has
  the copy written direct on the stripe; the loan still goes through
  the page cache.
- **Receive buffers smaller than a page: `xeu` `rx_buf_size` (W2,
  F2): new, not written. The README has the A/B ("Receive buffers
  smaller than a page").** Today `xeu` posts one whole page-pool page
  per receive descriptor, so at 16 KiB pages a 9018-byte frame has a
  16 KiB page to itself, and the ~7 KiB after it is never used.
  Details:
  - **What the driver does now** (stock 2.0.1, `xeu_drv.c`):
    `xeu_rx_refill()` takes a page with `page_pool_alloc_pages()`
    and posts `dst_addr = dma + XEU_RX_PAGE_HEADROOM` with
    `len = XEU_RX_PAGE_DATA_SIZE`, which is `PAGE_SIZE` less the
    headroom and `skb_shared_info`. `xeu_rx_poll()` builds the skb
    on the first buffer with `napi_build_skb(page, PAGE_SIZE)`. If a
    frame spans several descriptors (`num_desc`), it adds the rest
    as frags with `skb_add_rx_frag(..., PAGE_SIZE)`.
    `max_rx_buf_per_pkt` is `DIV_ROUND_UP(XEU_MAX_MTU,
    XEU_RX_PAGE_DATA_SIZE)`, and is passed to the firmware in
    `xsl_rm_rss_configure()`. The hardware takes any address and
    length per descriptor (`PROJECT.md`, "Xsight E1 RX
    capabilities").
  - **The change.** Add a load-time parameter `rx_buf_size` in bytes
    (0, the default, means `PAGE_SIZE`, which is today's behaviour).
    Each descriptor gets one `rx_buf_size` piece of a page-pool page
    (`page_pool_dev_alloc_frag()`), at that piece's offset. Each
    piece keeps its own headroom and `skb_shared_info` room, because
    any descriptor can be the first of a frame, so the skb is still
    built in place. Concretely:
    - `xeu_sw_rx_desc` records the offset;
    - the refill posts `dma + offset + headroom`;
    - the poll uses `napi_build_skb(addr + offset, rx_buf_size)` and
      gives frags the offset and a truesize of `rx_buf_size`;
    - the data size becomes a per-device value;
    - `max_rx_buf_per_pkt` is computed from it, so the firmware
      allows the extra buffers per frame.

    Page-pool fragment references are what our loan, region and
    recycle paths already count.
  - **Which sizes are worth it.** Each piece holds `rx_buf_size`
    less about 66 bytes of headroom (64 plus `rx_ip_align`'s 2) and
    about 320 of `skb_shared_info`. A 9018-byte frame then needs:

    | `rx_buf_size` | data per piece | pieces per frame | pieces per 16 KiB page | page per frame |
    |---|---|---|---|---|
    | 16384 (today) | ~16000 | 1 | 1 | 1 |
    | 8192 | ~7800 | 2 | 2 | 1, no gain |
    | **5440** (5376 on arm64) | ~4990 | **2** | **3** | **2/3** |
    | 4096 | ~3710 | 3 | 4 | 3/4 |

    Any size from about 4900 to 5461 gives 2 pieces per frame and 3
    per page. The page pool rounds a fragment up to the DMA cache
    alignment (128 bytes on arm64), so the driver rounds the size
    down to it: 5440 is 5376, still 2 and 3. **Written, built, not
    run:** `kernel-rpmtree/xsight/xeu-cut-RX-buffers-smaller-than-a-page-with-rx_buf_size.patch`
    (Patch6, Release 6, `kernel-rpmtree` 8c58863; the README has the
    details and the A/B).
    The ring index is a mask, so a ring's size stays a power of two.
  - **What it can and cannot buy (correcting an earlier claim).** The
    unused 7 KiB of a page is never written or read, so it takes no
    cache. "A 1 MiB WRITE is 1.8 MiB of region" counts pages, not
    cache lines, and F2's curve cannot simply be read at 25% fewer
    pages. What denser buffers do buy:
    - truesize: a held receive buffer carries 83% data at 5440,
      against 55% at a page per frame (W2's 256 KiB held 140 KiB);
    - posted and region memory per frame, about 2/3: the region and
      a loan's charge are counted in pages;
    - possibly cache sets. Every frame now sits at the same offset
      of its page, so the frame bytes cover only part of each page's
      cache sets. Spread over three offsets, they use more of the L3.
      That is a guess, and the A/B would show it.

    What it costs:
    - twice the descriptors per frame;
    - each frame brings two frags to GRO in place of one, so a GRO
      skb holds about half as many frames. That means more skbs for
      each WRITE, and more per-skb work in TCP and in
      `svc_tcp_recv_actor()`.
  - **Things in the same code.** These all touch the descriptor and
    poll paths that the change rewrites, and must be carried with
    it:
    - `rx_ip_align` (the headroom, per piece);
    - the refill retry, and `rx_refill_thresh`, which both count
      descriptors, so the threshold wants scaling;
    - the paired-posting experiment in `xeu-hds-experiment/`, which
      is the same code with a different aim; the two are exclusive.

    The harness's loan geometry (`FRAME_PAGE`, the charge per WRITE)
    assumes a frame per page. It needs changing before a loan cell
    runs at another `rx_buf_size`. Copy cells, at budget 0, do not
    use it.
- **rings, how the flows fall (F5, S5): new, none of it done.** Which
  ring takes a flow is the hash of its five-tuple, and with sixteen
  flows on four rings that alone moved build 462's runs by 4% (one
  ring at 19.9 GB/s, 440 thousand drops). Steering on this card was
  checked on build 461 and is not there: `xeu` supports none of
  `ethtool -x`/`-X`, `-n rx-flow-hash` or ntuple filters ("`xeu`
  gives no way to steer", above). Do not propose them again.
  **What the driver source shows** (stock 2.0.1, read 9 October
  evening):
  - ntuple takes exactly one rule, at location 0, whose target can
    only be a VF (`xeu_set_rxnfc()`), so it cannot place a flow on a
    ring.
  - The Xsight SDK built into the module (`xsl_rm_eunit_lib.o`) has
    the RSS controls that `xeu` does not wire to `ethtool`:
    `xsl_rm_reta_set()`/`_get()`, a redirection table of up to 256
    ring entries; `xsl_rm_rss_hash_set()`/`_get()`, a 68-byte seed
    and the packet types hashed; and `xsl_rm_rss_hash_type_set()`,
    Spooky (the default, `PP_RSS_DEFAULT_HASH`), Toeplitz or
    symmetric Toeplitz.
  - `xeu` calls only `xsl_rm_rss_configure()`, with its list of rings
    and `max_rx_buf_per_pkt`.
  - There are also flow-table RSS groups (`xsl_rm_ft_rss_group_set()`,
    4 groups for each vNIC).

  So option 3's first ask, `get_rxfh`/`set_rxfh`, may be a small
  `xeu` change on calls that already exist. The draft to Xsight
  (`xsight-steering-email.txt`) asks how they behave. **Written the
  same evening, built, not run:**
  `kernel-rpmtree/xsight/xeu-expose-the-RSS-redirection-table-hash-key-and-function-to-ethtool.patch`
  (Patch5, Release 6, `kernel-rpmtree` 799acf5). The README's item 3 under "How the
  flows fall on the rings" has what it does and how to try it.
  Three ways remain:
  1. **More flows than rings.** At 64 connections the rings took 28,
     27, 20 and 25% against 25, 6, 31 and 37% at sixteen. This is the
     only one measured. The 64-connection `pools_per_node` test in the
     README will show it again.
  2. **Mount again until the rings are even (the harness, no build).**
     Each mount gives its connections new source ports, so new hashes.
     Before a cell's measured part, the harness reads each ring's
     packets from `ethtool -S` over a few seconds and remounts if one
     ring is far over a quarter. This does not balance the flows; it
     keeps a bad fall from deciding a result. Written as
     `BALANCE=<percent>` to `run-budget.sh` (the README has how to
     use it); not run yet.
  3. **Steering in the driver**: a kmod change, and questions for
     Xsight. What to ask for, in order:
     1. **The RSS indirection table and key, `ethtool -x`/`-X`
        (`get_rxfh`/`set_rxfh`).** It comes first. It is cheap
        wherever the hardware already spreads flows by a hash through
        a table, and it works with the standard tools. It lets rings
        be weighted. With the key readable, a client's 4-tuple names
        a flow's table entry, so moving a hot entry to an idle ring
        places that flow with no remounting. A harness, or a daemon
        on a production server, could balance deterministically on
        that alone. Its limit is two hot flows in one entry.
     2. **ntuple filters, `ethtool -N flow-type tcp4 ... action
        <ring>`.** These place one flow exactly, which covers the case
        the table cannot. The limits are the hardware's filter table
        and the rules: for NFS they are written after each mount,
        since the client's ports change. aRFS is built on them.
     3. **Interrupt coalescing, `ethtool -C`.** It is not steering, but
        it is also missing ("no interrupt coalescing controls" in
        section 1). `enable_irq` from `xeu_rx_poll` was 6.1% of the
        samples (T11), and coalescing is the usual knob for that cost.

     Not worth asking for now:
     - **aRFS, and RFS in software.** RFS learns a flow's CPU from
       `sock_rps_record_flow()`. TCP calls it from `recvmsg`, splice
       and zerocopy receive (`net/ipv4/af_inet.c`, `net/ipv4/tcp.c`),
       but not from `tcp_read_sock()`, which is how sunrpc reads. And
       any nfsd thread serves any connection, so there is no CPU to
       steer to. It would need sunrpc changes before it could pay.
     - **Setting `rx-flow-hash`.** That only chooses the hash's fields,
       and the ports are evidently in it. A read-only `-n
       rx-flow-hash` would confirm it.

     **Questions for Xsight:**
     - Does the hardware have an RSS indirection table, and how many
       entries?
     - Is the hash Toeplitz, and can the key be read and set?
     - Is there a flow-filter (ntuple) table, how many entries, and
       can it match a TCP 4-tuple to a ring?
     - Can interrupt moderation be set, per ring?
     - If the table exists, can `xeu` expose `get_rxfh`/`set_rxfh`?

     This order is from the driver notes in section 1 and the traces
     here, not from Xsight's hardware documentation, which has not
     been read.

### The harness had deferred skb frees off; with them on the copy is no faster (9 October, 14:30 to 15:10)

**`run-budget.sh` has set `net.core.skb_defer_max=0` for every cell
since it was written on 4 October, with no comment and no word of it
in any document.** With deferral on (the kernel's default is 128) a
TCP receiver's consumed skb is handed back to the CPU that received it
and freed there, where its pages may go into the page pool's lockless
cache, which is last in, first out. With it off the skb is freed where
it was consumed: for nfsd, on CPUs 8 to 55, through the pool's locked
ring. So:

- **every NFS figure from this rig, at 64 KiB and at 16 KiB, copy and
  loan, was taken with deferred frees off.** They compare with each
  other; none is the kernel's default behaviour.
- **T1 above is what the harness's setting does, not what nfsd does.**
- Two cells run "at 1024 and 4096" (`xeu-16k-461-defer1024`,
  `-defer4096`: 29.9 and 25.7 GB/s) tested nothing, the harness having
  put the setting back to 0; they are two more samples of the ordinary
  copy cell. Four more cells went on finding that out
  (`xeu-16k-461-defer-why` to `-why4`: 27.2, 27.4, a light one, 30.1).

How it was found: a plain TCP receiver outside the harness
(`wirebuf.sh`) had 93% of its pages put back on the ring's own CPU; in
an NFS cell 4.69 million of 5.04 million calls of
`skb_attempt_defer_free()` by nfsd took the immediate-free branch and
none reached the deferral code.

The harness now takes `SKB_DEFER_MAX=`, and from 15:30 on 9 October
**its default is 128, deferral on** (Mike). `sweep.sh` writes the value
in `geometry.txt`; a run whose `geometry.txt` does not name it was made
with 0. That goes for every user of `run-budget.sh`, the test plan on
veth included: to repeat an earlier figure, pass `SKB_DEFER_MAX=0`. The direct copy with it on,
one traced cell each (`xeu-16k-461-deferon128`, `-deferon32`), against
the same cell with it off:

| over 10 s | off (trace1) | 128 | 32 |
|---|---|---|---|
| MB/s, the cell | 27713 | 27289 | 27970 |
| pages into the lockless cache (`recycle_cached`) | 10868 | 21.07 M | 21.13 M |
| cache full, so to the ring (`recycle_cache_full`) | 0 | 18.64 M | 18.67 M |
| pages into the ring (`recycle_ring`) | 41.08 M | 20.19 M | 20.21 M |
| refills of the alloc cache from the ring | 2.58 M | 1.26 M | 1.26 M |
| the pool ring's producer lock, contended | 88 k | not in the top seven | not in the top seven |
| server CPU-seconds per GB | 0.48 | 0.50 | 0.49 |
| softirq CPU-seconds in 90 s | 347 | 359 | 356 |
| `memmove`, share of cycles | 16.8% | 16.9% | 17.2% |
| client `rwnd_limited` | 98.5% | 97.8% | 98.5% |

D1. **Deferral does what it says: half of the pages now come back
    through the lockless cache, on the ring's own CPU, and the pool
    ring's lock drops out of the contended list.** The other half
    overflow the cache, which holds 32 pages at this page size, into
    the ring, from the same CPU.
D2. **The copy is no faster and no cheaper for it: 27.3 and 28.0 GB/s
    against 27.7.** Ten copy cells with deferral off today ran from
    25.7 to 30.1. The order in which the region's pages are reused is
    not what holds the copy.
D3. That is deferral alone, and with it only half of the pages take
    the last-in, first-out route. The page-pool stack ("page_pool:
    Recycle a region-bound pool's pages last in, first out") does the
    same for the half that overflow. Neither is judged by its own
    effect on the rate: they are two of the changes to be measured
    together (Mike, 2026-10-09). The copy itself was unchanged at 17%
    of cycles in these two cells.
D4. The freeing moves to the rings' CPUs, the busiest of which is at
    88% to 92% in softirq in these cells; it did not cost rate here.

### How a WRITE is received in copy mode: seventeen calls, and the connection idle for half of it (9 October, 16:30)

`xeu-rig/handoff.bt` inside the direct copy cell (sixteen connections,
256 KiB held buffer, rings of 4 x 128, deferred frees on, build 461),
10 s each: `xeu-16k-461-handoff` (27.5 GB/s) and `-handoff3`
(27.7 GB/s); the figures are of the second, the first agrees.
(`-handoff2` ran with a tracer that had not loaded and measured
nothing.)

A record that has not all arrived is parked on the socket and the
thread goes back to the pool; the next data to arrive queues the
connection again and a thread is woken for it.

| for each 1 MiB WRITE | |
|---|---|
| calls of `svc_tcp_recvfrom()` | **17.3** (most take 15 to 20) |
| data read by a call | about 60 KiB, one GRO skb |
| the next call is by another thread | **89%** |
| from the first call to the last | 552 us |
| of that, inside the calls (the copy) | 242 us, **44%** |
| of that, a queued connection waiting for a thread | about 100 us, 18% (6 us a time; half are under 6 us, 1.7% over 16 us) |
| of that, nothing queued: no data has arrived | about 210 us, 38% (13 us a time) |

W1. **A connection is being copied from for 44% of the time it takes
    to receive a WRITE.** While it is, the copy runs at 4.3 GB/s
    (1 MiB in 242 us); over the whole WRITE the connection carries
    1.9 GB/s. The 2.7 GB/s for each CPU of T8 had the waits in it.
W2. **Most of the idle time is the connection waiting for data, with a
    client that is waiting for the window 98% of the time.** Each call
    empties the socket, the window opens, the client sends, and the
    data is about 13 us in coming: the receive is stop and wait, a
    GRO skb at a time. A 256 KiB receive buffer holds about 140 KiB of
    data when each 9000-byte frame has a 16 KiB page to itself. The
    plain wire showed the same thing: 24 GB/s at 256 KiB, 45 at
    512 KiB, 49 at 1 MiB (L1).
W3. **A third of the idle time is the hand-off**: 6 us from the
    connection being queued to a thread reading it, sixteen times a
    WRITE, and nine times in ten a different thread on a different
    CPU, which then takes the record's pages from the socket.

What this adds to the list of changes, none of it run or written:

- a larger held buffer for copy mode (512 KiB, 1 MiB) on build 461:
  the copy at 1 MiB was run only on build 459, through the page cache;
  the region has the room now;
- the thread that finds a record incomplete staying with the
  connection for a few microseconds before it parks the record, or a
  receive low-water mark, so that a WRITE is one thread's and a few
  calls;
- what a frame's page costs the receive buffer's accounting (16 KiB
  charged for 9000 bytes held).

### The copy with a larger held buffer: 35.9 GB/s, and the copy slows as the region in use leaves the cache (9 October, 16:50 to 17:05)

The direct copy, sixteen connections, 8 MiB writes, 512 nfsd threads,
deferred frees on, build 461, one traced cell each
(`xeu-16k-461-copybuf`, `-copybuf-rings`; `plans/16k-24-copybuf.plan`,
`16k-25-copybuf-rings.plan`). The first column is the cell of W1.

| held buffer, rings | 256 KiB, 4 x 128 | 512 KiB, 4 x 128 | 1 MiB, 4 x 128 | 1 MiB, 4 x 256 | 1 MiB, 4 x 512 |
|---|---|---|---|---|---|
| MB/s | 27658 | 31840 | **35627** | **35925** | 32795 |
| region pages in use at peak | 936 | 1086 | 1534 | 2054 | 3072 |
| the same in MiB (the L3 is 32) | 15 | 17 | 24 | 32 | 48 |
| calls of `svc_tcp_recvfrom()` for a WRITE | 17.3 | 11.2 | 8.1 | 6.4 | 5.7 |
| inside those calls, us for a 1 MiB WRITE | 242 | 217 | 224 | 269 | **357** |
| the connection copied from, share of the WRITE's receipt | 44% | 47% | 56% | 69% | 83% |
| `memmove`, share of all cycles | 17% | 21% | 24% | 30% | **38%** |
| server CPU-seconds per GB | 0.49 | 0.43 | 0.44 | 0.49 | 0.60 |
| ring tail drops, the cell | 0 | 843059 | 1506917 | 250564 | 0 |
| client RTOs | 0 | 573 | 391 | 30 | 0 |
| client `rwnd_limited`, median | 98.5% | 58.5% | 40.5% | 97.9% | 98.6% |
| the busiest ring, GB/s, and its CPU in softirq | 12.4, 92% | 16.1, 92% | 14.2, 86% | 18.0, 86% | 12.5, 80% |

Every WRITE of every cell was written direct.

F1. **A 1 MiB held buffer takes the direct copy from 27.7 to 35.6 and
    35.9 GB/s**, the most this rig has written durably, and past the
    loan's 30.3. The stop and wait of W2 goes: a WRITE is received in
    6 to 8 calls where it took 17.
F2. **With the waiting gone, what holds the copy is the copy, and the
    copy is as fast as the region in use is small.** A 1 MiB WRITE's
    calls take 217 to 242 us with 15 to 24 MiB of the region in use,
    269 us with 32 MiB, which is all of the L3, and 357 us with
    48 MiB. `memmove` goes from 24% of cycles to 38% for less data
    moved. This is what Xsight's engineers described, measured on
    this path: the buffers have to come round while the cache still
    holds them.
F3. **So rings of 512 drop nothing and are slower (32.8), and rings
    of 128 drop 1.5 million frames and are as fast as rings of 256.**
    The frames dropped cost less than the cache lost.
F4. The 256 KiB cell holds little (15 MiB) and is slow for another
    reason, the wait for data (W2). The two wants pull apart: a buffer
    deep enough not to wait, and a region in use small enough to stay
    in cache.
F5. At 4 x 256 the busiest ring took 18.0 GB/s of 44 and another 2.9:
    the sixteen flows fell badly. Its CPU was at 86% in softirq.

One run each, traced. What follows from F2, for the stack of changes:
anything that has a page reused sooner or leaves fewer pages idle is
now worth rate and not only order. That is the page-pool stack and the
page swap in the kernel being built; fewer buffers posted and idle
(4 x 256 is 1024 pages, half of the L3, waiting to be written); the
driver posting a freed page sooner than every 32; and a 9000-byte
frame not having 16 KiB to itself.

### Build 462: the whole stack of changes (9 October, 18:10 to 19:00)

Mike built `7.1.13-21.hs.462` from `…-WIP3` and rebooted into it. It
has the page swap, the page-pool stack and the batched return. The two
sunrpc patches written after that build began (`pools_per_node`,
`svc_tcp_rx_linger_us`) were built as a module from the tip and put in
place of the kernel's (`xeu-rig/build-sunrpc.sh`, `swap-sunrpc.sh`),
with `pools_per_node=4`; `kmod-16k-xsight` release 5 was built for the
kernel, installed and `xeu` reloaded, which gives `rx_refill_thresh`.
No client was connected for any of it. The four KUnit suites pass on
the new sunrpc (xdr 7/7, svcsock 43/43, nfsd 6/6, nfsd4 10/10).

The cell is the copy's best of build 461, sixteen connections, 8 MiB
writes, 512 nfsd threads, rings of 4 x 256, a 1 MiB held buffer
(`plans/16k-26-copybest.plan`), with everything on: a linger of 50 us,
a refill threshold of 8, deferred skb frees at 128, four pools.
Traced, three runs (`xeu-16k-462-stack1` to `-stack3`), against build
461's one traced run of the same cell:

| | build 461 | run 1 | run 2 | run 3 |
|---|---|---|---|---|
| GB/s | 35.9 | 38.3 | 38.0 | 36.7 |
| server CPU-seconds for each GB | 0.49 | 0.52 | 0.52 | 0.51 |
| region pages in use at peak | 2054 | 2052 | 2106 | 2291 |
| calls of `svc_tcp_recvfrom()` for a WRITE | 6.4 | 2.1 | 2.1 | 2.3 |
| us in those calls for a WRITE | 269 | 351 | 354 | 345 |
| `memmove`, of cycles | 30% | 27% | 27% | 25% |
| `arch_counter_get_cntpct`, of cycles | 0.4% | 14.6% | 13.6% | 17.3% |
| frames dropped at the ring's tail | 250564 | 3442 | 3257 | 440528 |
| client RTOs | 30 | 1 | 2 | 139 |
| `rx_pp_recycle_ring_full`, 10 s | 17278 | 178 | 111 | 5360 |
| `rx_pp_alloc_empty`, 10 s | 1077 | 11 | 7 | 335 |
| the busiest ring and the least busy, GB/s | 18.0, 2.9 | 14.5, 8.7 | | 19.9, 6.2 |

Every WRITE was written direct and the region failed no allocation.

S1. **The stack carries 38.0 GB/s at the median (36.7 to 38.3), 6%
    over build 461's 35.9**, which was one run.
S2. **The linger does what it is for and is paid for in CPU.** A WRITE
    is received in 2 calls where it took 6.4. The wait is a busy one
    and reads the clock: `arch_counter_get_cntpct` is 14% to 17% of
    all cycles, and the time in the calls for a WRITE went up, since
    the waiting is now inside them. The CPU for each GB is up by 5%
    with the rate up by 6%.
S3. **The page swap shows**: `free_unref_folios` (0.6% on build 461)
    is out of the profile and neither `__folio_batch_release` nor
    `alloc_pages_bulk` is in it.
S4. **The pools' rings almost never fill and the pools almost never
    run empty** in runs 1 and 2.
S5. **Two runs of three dropped nearly nothing (3 thousand frames
    against 250 thousand), and the third dropped 440 thousand and was
    the slowest.** What differs in the third is how the sixteen flows
    fell on the rings: one ring took 19.9 GB/s. The flows' fall is the
    variable left that nothing here sets.
S6. `__lwq_dequeue` is in the head of the lock contention list on
    neither build, so that list does not say what the pools did.
    `svc_tcp_sendto`'s mutex was waited on for 5.4 s in 10 s against
    19.1 s.

**What each change is worth was not measured (Mike, 2026-10-09: wait
on that).** A queue that took one away at a time was started and
stopped. Its first cell had finished: no linger, the rest on, one
traced run (`xeu-16k-462-minus-linger`): 37.2 GB/s, 0.43 server
CPU-seconds for each GB, 6.9 calls for a WRITE, 257573 frames dropped,
20 RTOs. One run, and within the spread of the three above.

### tardis1 crashed in the loan cell (9 October, evening)

After the three runs, the loan cell was started, traced, twice: once
with no linger and once with a linger of 50 us. A test of the region
resize was to follow. **tardis1 crashed and did not reboot itself; it
was unreachable until a hard power cycle.** The console has not been
read, and which run was going is not known.

The likely cause is in "SUNRPC: Return a loaned record's page-pool
pages in batches" (`bdd837db04f1`). `svc_tcp_rx_release_refs()` passed
`page_pool_put_netmem_bulk()` up to 64 pages. That function collects
the pages whose last reference it drops into an on-stack
`bulk[XDP_BULK_QUEUE_SIZE]`, which has 16 entries, and does not check
the bound. A loaned 1 MiB WRITE at 16 KiB is 64 or more pages of one
pool, mostly with nfsd holding the last reference. So nearly every
loaned WRITE wrote up to 48 entries past that array on an nfsd
thread's stack. Only a loaned record takes the path: the copy runs
above had the patch loaded and never ran it. `svcsock_kunit` passed
43/43 because no test dropped a last reference inside a batch.

At the tip: "SUNRPC: Keep page-pool return batches within the bulk
API's bound" (the batch is `XDP_BULK_QUEUE_SIZE`), "SUNRPC: test
releasing more page-pool pages than one bulk batch in the svcsock
KUnit suite", and "SUNRPC: Refuse to build a page-pool return batch the
bulk API cannot take" (a `BUILD_BUG_ON`). Built with W=1 and checked by
checkpatch; nothing has run. The README's section on the crash has the
next boot's order. The sunrpc with the bug is still installed in
`updates/` and loads on boot.

### Build 463: the RSS table and `rx_buf_size` on the card (10 October, 12:00 to 12:50)

Build 463 (`7.1.13-21.hs.463.loanpages_pool`) has the sunrpc fixes built
in and kmod-xsight 2.0.1-6. The KUnit suites pass (svcsock 44/44 with
`svcsock_rx_page_pool_bulk_release_test`). The journal holds only the
running boot, so the crash of 9 October was not read. `xeu` was loaded
without `rss_indir_size`; the session added it to
`/etc/modprobe.d/xeu.conf` and reloaded `xeu` (Mike's leave, given for
this boot). The runs are `$L/xeu-16k-463-*`.

**The RSS table works, and a flow's ring can be chosen.** Sixteen
`iperf3` streams to the port, eight rings, `rss_indir_size=128`:

- `ethtool -X weight` moved every flow to ring 0, then to ring 1, then
  to rings 6 and 7, and `equal 8` spread them again. All 128 entries
  are used.
- **Bits 13 to 19 of a flow's hash pick its table entry.** The hash is
  the one `xeu` puts in the skb (`xeu-rx.bt` shows it). An even table
  showed bits 13 to 15 naming the ring for all sixteen flows, and a
  table in blocks of eight showed bits 16 to 18.
- So a flow is placed by reading its hash and setting its entry.
  `xeu-rig/rss-balance.py` builds the `rss-set` request from
  `xeu-rx.bt`'s output. It took the rings from 1, 4, 1, 3, 3, 2, 0 and
  2 flows to 2, 2, 2, 2, 2, 2, 3 and 1 (two flows shared an entry).
- `ynl` needs `--no-schema` on tardis1 (no `jsonschema`); PyYAML was
  installed for it.

**Two bugs in the RSS patch, found here and fixed** (kernel-rpmtree,
folded into the Release 6 commits, which Mike force-pushes):

1. **Setting the key stopped the hashing.** The patch read the packet
   types back with `xsl_rm_rss_hash_get()` and wrote them with the key.
   The firmware's reply says only that a type is hashed, not on which
   fields, so the write left none: every packet hashed to 0 and landed
   on ring 0, and putting the old key back did not undo it. Only a
   reload of `xeu` did. The fix sends the key with no packet type
   marked valid, which the firmware takes as the seed alone.
2. **The firmware's own table could not be read.**
   `xsl_rm_reta_get()` returns handles whose id is the ring's number
   across all DMA engines (64 to an engine), so a ring of engine 2
   read back as `0x2010080` where its handle is `0x2010000`. `ethtool
   -x` failed with `EIO` until a table had been set, and so did a
   netlink `rss-set` of the hash function alone. The fix takes either
   form as the ring.

Both fixes ran on the card: `ethtool -x` shows the firmware's table
(rings 0 to 7 in turn), `ethtool -X hkey` changes every flow's hash
with the flows still spread, and `rss-set` with `"hfunc": 1` works
before any table is set.

**Still open in the RSS support:**

- `ethtool -X hfunc toeplitz` from the `ethtool` 6.15 command fails
  with `EINVAL`. The command sends no hash function in its request
  while the port reports none (Spooky has no ethtool name), and the
  core refuses a request that changes nothing; `xeu` is not called.
  `ynl` sets it: `--do rss-set --json '{"header": {"dev-name":
  "enp161s0f0"}, "hfunc": 1}'`.
- There is no way back from Toeplitz to Spooky without reloading `xeu`.
- **A flow's hash cannot be computed from its ports.** With Toeplitz
  set every hash changes, and with a key that repeats every 16 bits
  the hashes repeat every 16 bits, as Toeplitz would. But they are not
  the standard function over the addresses and ports with the key
  shown, in any field order tried. A question for Xsight. Placing
  flows does not need it.

**`rx_buf_size=5440` works.** The open logs "5440-byte RX buffers, 3 to
a page, up to 2 to a 9000-byte MTU frame": the size is not rounded to
5376, since the DMA cache alignment on this host is 64 bytes. A full
frame reaches GRO with a truesize of 11136 and one frag, where a whole
page gives 16640 and none. The driver also warns that with two buffers
to a frame the payload after the first is not 4-byte aligned; that
does not matter to the copy, and will to a loan written direct.
Reloading `xeu` with a region bound (unbound first with `echo none >
rx_region_dev`) gave every page back and logged nothing: the first
drain of the LIFO stack at a pool's destruction.

**The A/B: the copy cell at three buffer sizes, two traced runs each.**
The whole stack as on build 462 (linger 50, `rx_refill_thresh` 8, four
pools, 512 threads, deferred frees on), `BALANCE=35`, sixteen
connections, the stripe trimmed before each. Each row holds the same
frames on a ring where it can, and the held buffer is 1 MiB scaled by
a frame's truesize.

| `rx_buf_size` | rings | held buffer | GB/s | region pages | ring drops | retransmits | CPU-s per GB | `memmove` |
|---|---|---|---|---|---|---|---|---|
| 5440 | 4 x 512 | 685 KiB | 41.0, 36.6 | 1376, 1360 | 0, 0 | 17, 3507 | 0.49, 0.54 | 23.8%, 23.9% |
| 4096 | 4 x 1024 | 772 KiB | 35.8, 37.4 | 1808, 1968 | 0, 0 | 3277, 4230 | 0.57, 0.54 | 27.2%, 27.1% |
| 0 (a page) | 4 x 256 | 1 MiB | 41.4, 38.3 | 2376, 2168 | 3557, 127510 | 27016, 157375 | 0.48, 0.52 | 27.1%, 27.1% |

- **The footprint is what was predicted.** 5440 holds 1360 to 1376
  region pages, 21 MiB, against the whole page's 2168 to 2376: about
  three fifths, and under the 32 MiB L3. 4096 posts a third more
  frames and holds 1808 to 1968.
- **The rate does not separate the sizes.** Two runs of one size
  differ by 2 to 4 GB/s, as much as the sizes differ. 5440 and the
  whole page each have a run at 41 and a run at 37 to 38; 4096 has
  neither run above 37.4.
- **The pieces dropped nothing.** The whole page lost 3557 and 127510
  frames at the ring tail and retransmitted 27 and 157 thousand
  segments; neither smaller size dropped a frame. (4096 has more
  frames posted; 5440 has the same 256.)
- `memmove` is 24% of cycles at 5440 and 27% at the others. A WRITE is
  received in 2.1 to 2.3 calls in every run.
- `rx_pp_recycle_ring_full` is 0 with the pieces and 52 and 1434 with
  whole pages; `alloc_fail` stayed 0 throughout.

With the fall of the flows what it is, more runs of 5440 against the
whole page are needed to say whether the smaller footprint is worth
rate. What the two runs each do say: 5440 costs nothing, holds three
fifths of the memory and ends the tail drops at 256 frames a ring. The
whole-page runs are also this boot's figure for the stack on build
463: 41.4 and 38.3 GB/s against build 462's 38.3, 38.0 and 36.7.

After the A/B `xeu` is loaded with `rx_buf_size=0`, rings of 4 x 256,
and the region bound; `/etc/modprobe.d/xeu.conf` has `rx_ip_align=2
rss_indir_size=128`.

### Flows placed on the rings under NFS: `PLACE=1` (10 October, 14:25)

`rx_buf_size=5440` is settled for now (Mike) and pinned in
`/etc/modprobe.d/xeu.conf`. The harness can now place the flows where
it only rejected a bad fall (`BALANCE`). **`PLACE=1`** to
`run-budget.sh`, and so to `sweep.sh` and `trace-sweep.sh`: before a
step cell is measured it mounts, starts a short load, reads each flow's
hash and bytes, and points each flow's RSS table entry at a ring so
that the rings carry the same (`xeu-rig/rss-place.py`: the busiest
entry to the ring with the least so far). The mount is kept for the
cell. `<cell>/place.txt` has every flow's hash, entry and ring and the
rings' shares before and after.

One traced run of the 5440 copy cell proved it
(`$L/xeu-16k-463-place-1`):

| | ring 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| as the sixteen flows fell at the mount | 23% | 21% | 36% | 20% |
| after the table was set, in the short load | 25% | 25% | 25% | 25% |
| 10 s of the cell itself, from 35 s in | 25.0% | 25.0% | 25.4% | 24.6% |

Four flows to a ring, no two sharing an entry. The cell carried
**39.9 GB/s** with 1376 region pages, no ring drops, no RTO, 0.50
CPU-seconds per GB and `memmove` at 24%. That is one run; with
`BALANCE=35` the same cell gave 41.0 and 36.6. Whether placing ends
the spread between runs needs repeats. In this run the harness lost
`place.txt` (it was made before the cell's directory went to the
client and was fetched back empty); the file was rebuilt from the
saved flows and the console, and the order is fixed.

A client that reconnects during a cell gets a new source port and so
a new entry: the cell does not place again.

### What ring depth and the held buffer decide (10 October, with `plans/16k-29-depth-buf.plan` running)

The main thing is how to spend the L3: the 5440-byte buffers left
about 11 MiB of the 32 MiB cache unused, and these two settings decide
what that headroom buys. Each one trades memory in the cache against a
different kind of waiting or loss.

**Ring depth** (128 or 256 frames a ring) sets:

- **Memory posted to the card.** Every posted buffer is cache
  footprint whether or not a frame lands in it.
- **Tolerance for a late CPU.** A ring is served by one CPU; if it is
  late, a shallow ring overflows and the frame is dropped, which the
  client pays for in retransmits.
- **The page pool's size.** `xeu` sizes each ring's pool to the ring,
  so depth also changes how pages recycle.

**Held receive buffer** (685 KiB or 1 MiB per connection) sets:

- **How much unread data can sit in receive pages** per connection,
  which is the other part of the footprint.
- **The window the client sees.** Too small and the client stalls
  waiting on the server, which is what held the copy at 27.7 GB/s on
  9 October.
- **Whether a whole 1 MiB WRITE fits**, which affects how many reads
  it takes to receive one.
- **Where back pressure lands**: the point at which the server makes
  the client wait rather than taking more into memory.

So across the four cells, what to read beyond the rate:

- region pages in use at peak, against the 32 MiB cache;
- time to copy one WRITE and `memmove`'s share of cycles, which show
  whether the data is still in cache when it is copied;
- ring drops and retransmits;
- reads per WRITE and how long the client waits on the window;
- server CPU per GB.

Two things come along as side effects of the runs themselves:

- **Run-to-run spread with placement.** These are the first repeats
  with every flow placed, so they show how much of the earlier 4 GB/s
  spread placement removes.
- **Whether the scaled buffer was the right call.** In the A/B the
  held buffer was shrunk to 685 KiB to keep the data window the same
  as the whole-page control. The 1 MiB cells show whether that was
  holding the rate back.

What these runs do not vary is the linger, the refill threshold and
the pool count. Those stay fixed so the four cells differ only in
depth and buffer.

The cells, each on `rx_buf_size=5440`, traced, sixteen connections,
`PLACE=1`, the plan run twice (`$L/xeu-16k-463-depthbuf-1` and `-2`):

| ring entries | frames a ring holds | held receive buffer |
|---|---|---|
| 4 x 512 | 256 | 685 KiB (1 MiB of data at this buffer size) |
| 4 x 512 | 256 | 1 MiB |
| 4 x 256 | 128 | 685 KiB |
| 4 x 256 | 128 | 1 MiB |

### Ring depth and the held buffer on 5440-byte buffers: the results (10 October, 15:20 to 16:05)

`$L/xeu-16k-463-depthbuf-1` and `-2`: the plan run twice, traced,
`PLACE=1`, the rest of the stack as before.

| rings (frames) | held buffer | GB/s | region pages | ring drops | retransmits | RTOs | CPU-s per GB | `memmove` | `arch_counter_get_cntpct` | calls per WRITE |
|---|---|---|---|---|---|---|---|---|---|---|
| 4 x 512 (256) | 685 KiB | 39.4, 39.6 | 1536, 1456 | 0, 0 | 4546, 3902 | 0, 0 | 0.49, 0.50 | 23.7%, 23.6% | 19.0%, 18.8% | 2.3, 2.3 |
| 4 x 512 (256) | 1 MiB | **41.8, 40.7** | 1904, 1824 | 0, 0 | 4765, 5001 | 0, 1 | 0.50, 0.49 | 30.1%, 28.5% | 11.7%, 12.8% | 2.0, 2.1 |
| 4 x 256 (128) | 685 KiB | 40.5, 40.3 | 1056, 1126 | 106001, 105372 | 146140, 149346 | 18, 23 | 0.48, 0.48 | 21.7%, 21.8% | 20.1%, 20.3% | 2.3, 2.4 |
| 4 x 256 (128) | 1 MiB | 40.9, 40.8 | 1451, 1499 | 884434, 875254 | 1204311, 1192019 | 200, 203 | 0.47, 0.46 | 24.7%, 23.9% | 16.1%, 16.5% | 2.2, 2.3 |

1. **4 x 512 with a 1 MiB buffer is the best: 41.8 and 40.7 GB/s, no
   drops.** About 1.7 GB/s over the 685 KiB buffer on the same rings.
   The linger's busy wait falls from 19% of cycles to 12%, since more
   of a WRITE has arrived when a thread reads; `memmove` rises from
   24% to 29%, with 28.5 to 30 MiB of region in use against 23 to
   24. It spends nearly all of the L3.
2. **Rings of 128 frames drop.** 105 thousand frames a run at 685 KiB
   and 880 thousand at 1 MiB, with 18 to 200 RTOs on the client. The
   rate holds at 40.3 to 40.9 through TCP's resends, in as little as
   16.5 MiB. Not a setting to keep.
3. **Placed runs repeat.** The two runs of a setting differ by 0.1 to
   1.1 GB/s. 4 x 512 at 685 KiB has three placed runs now, 39.4, 39.6
   and 39.9, where `BALANCE=35` gave 41.0 and 36.6.
4. **The placement.** Before the table was set the flows fell with up
   to 57% on a ring and as little as 0% (the table of the cell before
   is still in force at the mount, so this is not the card's own
   fall). In seven cells of eight the rings then carried 24% to 26%
   each through the cell. In `depthbuf-2`'s first cell they carried
   31/19/25/25%: `rss-place.py` balances by the bytes of a 3 s sample
   and put 5, 3, 4 and 4 flows on the rings. Balancing by flow count
   when the flows are symmetric, or sampling longer, would avoid it.
5. `place.txt` was kept in every cell (the fix of the first run's
   loss). `rx_pp_recycle_ring_full` 0 and `alloc_fail` 0 throughout.
6. 4 x 512 at 685 KiB peaked at 1456 to 1536 pages here and at 1360
   to 1376 in the three runs before; not looked into.

Next, not started: a buffer past 1 MiB on 4 x 512 (does it gain, or
does the region leave the cache), then the linger, `rx_refill_thresh`
and the pools.

### Eight rings: the rings' CPUs were not the limit (10 October, 16:50 to 17:10)

In the 41.8 GB/s cell the four rings' CPUs (0, 2, 4, 6) were 91% busy,
about 80% of it softirq, and the client's connections waited on the
server's window 95% to 99% of the time. So eight rings were tried, to
halve each ring CPU's load: `plans/16k-30-rings8.plan`,
`$L/xeu-16k-463-rings8-1` and `-2`, 5440-byte buffers, a 1 MiB held
buffer, `PLACE=1`, traced. With eight rings, rings 4 to 7 interrupt
on CPUs 8, 10, 12 and 14 (Tx on 9, 11, 13, 15), which nfsd's 8-55
includes, so **nfsd's threads were moved to CPUs 16-55 for these
cells** (`taskset`, 40 CPUs for 48).

| rings (frames a ring) | GB/s | region pages | ring drops | RTOs | CPU-s per GB | `memmove` | calls per WRITE |
|---|---|---|---|---|---|---|---|
| 4 x 512 (256), the cell before | 41.8, 40.7 | 1904, 1824 | 0, 0 | 0, 1 | 0.50, 0.49 | 30.1%, 28.5% | 2.0, 2.1 |
| 8 x 256 (128) | 39.6, 39.3 | 2085, 2048 | 0, 0 | 34, 6 | 0.63, 0.62 | 28.8%, 27.5% | 1.8, 1.9 |
| 8 x 512 (256) | 36.0 | 2848 | 0 | 0 | 0.69 | 34.3% | 1.6 |

1. **No gain: 39.6 and 39.3 GB/s against 41.8 and 40.7.** The rings'
   CPUs were relieved as meant: each of the eight was 60% to 70% idle,
   where the four had been 9% idle. So their load was not what held
   the rate.
2. **The footprint grew past the L3 and the copy got dearer.** Eight
   rings of 128 frames post the memory four of 256 do, but the region
   in use peaked at 2048 to 2085 pages (32 to 33 MiB) against 1824 to
   1904. Server CPU per GB rose from 0.50 to 0.62. At 8 x 512 the
   region reached 2848 pages (44.5 MiB), `memmove` 34% of cycles, and
   the rate fell to 36.0. It is the copy against the cache again
   ("The copy with a larger held buffer", F2).
3. **Eight shallow rings do not drop.** 128 frames a ring dropped 880
   thousand frames a run on four rings and none on eight, with each
   ring carrying an eighth. The 34 and 6 RTOs are not explained.
4. Placement put two flows on each of the eight rings in all three
   cells; in the cells the rings carried 12% to 13% each.
5. Not separated: nfsd on 40 CPUs and not 48. Its CPUs showed 55% to
   58% system time and about 9% idle (the rest I/O wait) against 30%
   and 26% on four rings. The four-ring cell has not been run with
   nfsd on 16-55.

After these cells the port was at 8 rings of 256 entries and nfsd's
threads on CPUs 16-55 (both put back after the follow-up below). The
best setting is 4 x 512: `ip link set
enp161s0f0 down; ethtool -L enp161s0f0 combined 4; ip link set
enp161s0f0 up; ethtool -G enp161s0f0 rx 512`, and `systemctl restart
hammerspace-nfsd-tuning` (or `taskset -pc 8-55` on each nfsd thread)
puts nfsd back.

**The follow-up, eight rings of 128 frames with the 685 KiB buffer
(17:25 to 17:40, `$L/xeu-16k-463-rings8-685k-1` and `-2`): 39.8 and
39.8 GB/s**, which is four rings' rate with that buffer (39.4, 39.6,
39.9). The rings' CPUs were 54% to 67% idle. So the rings' CPUs are
not the limit at either buffer. The footprint did not come back under
the L3 as meant: 2046 and 2074 pages, the same as with 1 MiB, so on
eight rings it is not the held buffers that set it (eight pools, each
with its own cache and stack, are the likely cause; not looked into).
`memmove` 22.5% and 22.8%, CPU 0.61 CPU-seconds per GB, no drops, 3
and 1 RTOs.

**What the rate is made of, from `handoff.bt` in the four-ring cells.**
A 1 MiB WRITE takes 356 to 365 us from its first `svc_tcp_recvfrom()`
to its last with the 1 MiB buffer and 378 to 379 us with 685 KiB. At
41.8 GB/s each of the sixteen connections completes a WRITE every
401 us, so a connection is inside one WRITE's receive for about nine
tenths of the time and the gap between WRITEs is about 45 us. One
connection is received by one thread at a time, so **sixteen
connections cannot carry much more than 16 x 1 MiB / 360 us = 46.6
GB/s, and carry 41.8**. What is left is in the time to receive one
WRITE on one connection, or in the number of connections. The copy
itself is about 150 us of the 360 (0.50 CPU-seconds per GB at 30%
`memmove`); the rest is waiting for the WRITE's data to arrive and
the per-call work. Not run: 32 connections (`STEP_MOUNTS=2`) on this
stack.

After the follow-up the port was put back to 4 rings of 512 entries
(`ethtool -X equal 4` first: the ring count cannot be lowered while
the table names a ring that would go) and nfsd's threads to CPUs 8-55.

### Thirty-two connections: the client fell over, and the queue was stopped (10 October, 17:15 to 17:30)

`plans/16k-31-conns32.plan`, `STEP_MOUNTS=2` (two mounts of nconnect
16 to two addresses of the server), sixteen writers as before, rings
of 4 x 512, `PLACE=1`, traced. The first pass ran; **Mike stopped the
second pass in its first cell (the client was clearly unhappy)**.
`$L/xeu-16k-463-conns32-1` has the two cells; `-2` is a stopped cell
and is not a result.

| held buffer | GB/s | region pages | ring drops | retransmits | RTOs |
|---|---|---|---|---|---|
| 1 MiB | 12.2 | 2580 | 26995 | 64633 | 28 |
| 685 KiB | 12.1 | 2532 | 32 | 4448 | 9 |

**This is not a figure for the server.** In the 1 MiB cell the
client's CPUs were 80% in system time and 15% idle (41% and 46% in the
one-mount cells) and the server 88% idle. The client's connections
waited on the server's window 48% to 71% of the time, against 95% to
99% with one mount. A 1 MiB WRITE took 1.86 ms from its first read to
its last, of which 0.27 ms was reading: the data was not arriving.
What the client was doing is not known: it has no `perf`.

Not ours, on the client (192.168.1.106) at the time: an NFS mount of
192.168.1.161 not responding, with eighteen `find` blocked on it and
a load average over 100, and two `grep` at 100% of a CPU each for
weeks. Whether the client collapses with two mounts on a quiet day
was not separated from that.

**It puts T10 in question** ("Sixty-four connections did not help the
copy: 21.1 GB/s", build 461, four mounts): that was read as the
server's doing and the client's CPUs were not looked at.

Placement worked with 32 flows over two mounts: 10/7/7/8 and 9/7/8/8
flows a ring (balanced by bytes), the busiest ring 29% and 26% in the
check.

Stopping it: the driver, `trace-sweep.sh` and `sweep.sh` were killed
by PID and `run-budget.sh` sent TERM, whose exit trap took the export
and the second address down; `fio` was killed on the client, which
had no mount left; the cell's files were removed from the export.

**Left over from the ring change of 17:14 (8 rings to 4): one page
pool does not finish shutting down.** `page_pool_release_retry()
stalled pool shutdown: id 454, 24 inflight`, once a minute since. 24
pages of a pool of rings 4 to 7 are still out. One guess, not shown:
skbs on the deferred-free lists of CPUs 8 to 14, which are flushed by
those CPUs' own receive softirq, and no ring interrupts there any
more. A loopback ping from every CPU was tried to flush them.

### The client rebooted; the best cell again, and a load paced at the source (10 October, 18:00 to 18:15)

Mike rebooted the client (192.168.1.106): idle, none of its own NFS
mounts, MTU 9000, its sysctls at their defaults.

**The best cell repeats: 41.1 GB/s** (`plans/16k-32-best.plan`: rings
of 4 x 512, a 1 MiB held buffer, `PLACE=1`, traced;
`$L/xeu-16k-463-best-rebooted-client`). No ring drops, 0.49
CPU-seconds per GB, `memmove` 29.6%, rings 24/25/25/26% in the cell,
359 us for a WRITE's receive. The client 42% system and 47% idle, as
before its reboot. 25148 retransmits and 15 RTOs with no ring drop,
against about 5000 and 0 to 1 before: not explained.

**`used_pages` is a high-water mark across cells, not one cell's
footprint.** This cell reported 2644 pages, and the region showed that
many in use at idle before it started: the pools keep the pages they
have recycled (the stack is as large as the ring), and the 32
connection cells had left 2580. A figure taken after a larger cell is
inflated. The figures of the depth and buffer runs were taken in
rising order within a pass, but the second pass followed the first.
Before a cell whose footprint matters, empty the pools: take the port
down and up (or change the ring size and back).

**Pacing at the source gains nothing** (`STEP_RATE="36000 39000 42000
45000"`, the same cell, 30 s a step without stopping;
`$L/xeu-16k-463-paced-1`):

| offered, GB/s | carried, GB/s |
|---|---|
| 36.0 | 36.0 |
| 39.0 | 39.0 |
| 42.0 | 41.6 |
| 45.0 | 40.8 |

A load the server can take, raised in steps, tops out where the load
offered all at once does (41.1, 41.8, 40.7). So there is no lower
state here that a flat-out start falls into and a paced one avoids,
which was Xsight's suggestion to check. 35 ring drops, no RTO. The
placement put 4, 4, 3 and 5 flows on the rings (two shared an entry)
and the rings carried 24/24/22/30% in the check.

### Thirty-two connections on the rebooted client: the same collapse (10 October, 18:20)

One cell of `plans/16k-32-best.plan` with `STEP_MOUNTS=2`, run by
`xeu-rig/conns-watch.sh`, which reads the port and the client's CPUs
30 s into the cell and stops it under 25 GB/s
(`$L/xeu-16k-463-conns32-fresh`). **It stopped it: the port carried
12.4 GB/s and the client's CPUs were 81% in system time and 15%
idle.** The client had been rebooted an hour before and was idle, and
the one-mount cell just before this carried 41.1 with the client 47%
idle. So two mounts of nconnect 16 are what this client cannot drive,
whatever else was wrong with it earlier, and T10's four mounts on
build 461 (21.1 GB/s) most likely had the same cause.

What the client's kernel is doing with 39 of its 48 CPUs is not
known: it has neither `perf` nor `bpftrace` (kernel 6.12.76-2.hs.212,
`mlx5_core`, 48 queues). That is the next thing to find out, and
installing either is Mike's. Until then more than sixteen connections
from this client says nothing about the server.

The placement put 8, 9, 7 and 8 of the 32 flows on the rings. The
stop left the cell's second address on the port (the cell removes it
itself and was killed first); `conns-watch.sh` now takes it off.

### The client on 7.1.13-21: the baseline, and 32 connections that no longer collapse (10 October, 19:15 to 19:30)

Mike rebooted the client into `7.1.13-21.hs.460.el9.x86_64` (it was
`6.12.76-2.hs.212.el8`). It has `perf` now. The port was taken down
and up first, to empty the pools, so `used_pages` below is each
cell's own high-water mark, the second on top of the first.

| cell (rings of 4 x 512, 1 MiB buffer, `PLACE=1`, traced) | GB/s | region pages | ring drops | retransmits | RTOs | CPU-s per GB | client sys / idle | client waits on the window |
|---|---|---|---|---|---|---|---|---|
| 16 connections (`xeu-16k-463-best-client71-1`) | 41.6 | 1754 | 213 | 5926 | 4 | 0.50 | 50% / 38% | 95% to 99% |
| 32 connections, two mounts (`xeu-16k-463-conns32-client71`) | 37.0 | 2725 | 43916 | 253141 | 50 | 0.51 | 70% / 19% | 73% to 86% |

1. **The baseline is the old client's: 41.6 GB/s** against 41.8, 40.7
   and 41.1, with the same CPU per GB, `memmove` share (29%) and time
   to receive a WRITE (350 us). The client spends 50% of its CPUs in
   the kernel for it where the 6.12 kernel spent 42%.
2. **Two mounts no longer collapse on this client kernel**: 37.6 GB/s
   on the wire at 30 s where 6.12 gave 12.4 twice. So that was the
   6.12 client's NFS or TCP, not its state that day.
3. **Thirty-two connections carry less than sixteen: 37.0 GB/s.** The
   region in use reached 2725 pages, 42.6 MiB, well past the L3:
   twice the connections with a 1 MiB buffer each. The rings dropped
   44 thousand frames, the client retransmitted 253 thousand segments
   and had 50 RTOs, and its CPUs were 19% idle. A WRITE took 671 us
   from first read to last where sixteen connections take 350, with
   the same 361 us of it inside the calls: each connection gets data
   half as fast.
   **With half the held buffer (512 KiB, `plans/16k-33-conns32-512k.plan`,
   `$L/xeu-16k-463-conns32-512k`, the pools emptied first): 35.7
   GB/s.** The region in use was 1754 pages, the sixteen-connection
   cell's, with no ring drop and no RTO, so the footprint and the
   loss are out of it and it is still 6 GB/s under sixteen
   connections. The client waits on the server's window again (91% to
   95%) with its CPUs 69% in the kernel and 21% idle. On the server
   the linger's busy wait is 24% of cycles (13% at sixteen) and
   `memmove` 20%: a WRITE takes 712 us from first read to last, 377
   of it in the calls, at 2.9 calls for a WRITE. More connections
   from this client do not carry more, with the footprint held or
   not. Sixty-four were not run.
4. `rss-place.py` counted flows before bytes for the first time in
   the second cell and put 8 on each ring; the check showed
   25/26/25/25%. In the cell itself the rings carried 21/21/25/33%,
   so with two mounts the flows do not carry the same for long. The
   first cell was the last with the old script: 4/4/5/3 flows,
   25/25/30/20%.

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

  Every loan was written direct. All but one WRITE was denied once
  before it was admitted: the budget the harness derived was 343,
  where it was 462 for the PRP pair, because `parked` had risen to
  1636, and at 127 a WRITE and a reserve of 120 that is one WRITE in
  flight and sometimes two. `fio_rc` 0, no retransmits, no tail drops,
  `alloc_fail` unchanged. The backing file was removed; the `nvmet`
  and `nvme-tcp` modules stay loaded.

- **These cells are too short to compare rates.** The loan cell ran
  at half its copy, and the budget looked like the cause. It is not:
  the same cell with `FORCE_BUDGET=` (`xeu-geometry-4x256-sgl-b600`,
  `-b900`):

  | budget | MB/s | denied | used_pages | its warm-up, MB/s |
  |---|---|---|---|---|
  | 343 (derived) | 619 | 511 | 1640 | 664 |
  | 600 | 847 | 86 | 1861 | 860 |
  | 900 | 498 | 0 | 1731 | 658 |

  With no WRITE denied the cell was the slowest of the three. Each
  cell's rate follows its own warm-up, which is the same copy job run
  moments before and itself ranged from 658 to 860 here and 1118 to
  1342 on the PRP runs. A cell writes 512 MiB, under a second at these
  rates, so no `seq-sized` figure in this section is a measurement of
  copy against loan; the 120 s step cells of 5c and 5e are. What these
  runs do show: every loan written direct, no pause or overrun, no
  `alloc_fail`, no tail drops, at budgets beyond the 408 pages the
  region had free.

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

A patch that retries is in `kernel-rpmtree` as `Patch3` of
`xsight/xsight.spec` (`xeu-retry-an-RX-refill-that-came-up-short.patch`,
kmod-xsight 2.0.1-4, commit `f61ea4c`, not pushed): a timer per ring
runs the poll again while the ring is short.

**It was tested on this boot on 2026-10-08, and it works.** The 64k
package was built on tardis1 from that spec (`rpmbuild` on the host;
`/var/tmp/zc-run-logs/kmod-xsight-64k-build`), installed, and `xeu`
reloaded with Mike's leave (that takes `enp164s0f0` down for some
seconds). Then, at 4 x 256 on the 2048-page region:

| check | unpatched | patched |
|---|---|---|
| sixteen connections, buffers left to grow, 20 s: region dry (`min_free` 0) | a ring dead after, 1 of 1; 5 connections hung | 4 runs of 4: senders done in 20 s, every ring receiving |
| 60 new connections to port 2049 after each | 2 of 12 timed out | 0 of 60 failed, 4 times |
| the next load, sixteen connections at 128 KiB | (after a down and up) 3878 MB/s | 3936 MB/s on all four rings, no tail drop |
| port brought up with 4 x 1024 to post from 2048 pages | not tried | the first fill retried every tick until the rings were made smaller |
| `step-copy-n16` | 1718 to 2067 MB/s | 1811 MB/s |
| "Failed to allocate Rx page" in the log | 13000 lines from one run | about 100 over four runs |
| "Unbalanced enable for IRQ" or any warning | | none |

`rpm -U` of the new package removed the kmod-64k-xsight packages of
builds 454 and 455 with release 3 of 458, since they share a name:
those two kernels have no xsight modules installed now.

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
- A sixteen-connection loan that runs, and an NVMe/TCP loan at a
  budget with room, need pages the region does not have at 4 x 256.
  Section 5e has them at 4 x 128.

## 5d. Why sixteen connections stall on a budget of one WRITE (2026-10-07)

`step-on-n16`, `FORCE_BUDGET=250` (one WRITE of 127 in flight, 120 in
reserve), held buffer 128 KiB, local NVMe unless said.

| invocation | trace | runs | result |
|---|---|---|---|
| after `step-copy-n16` (NVMe/TCP) or `step-on` | off | 4 | all stalled: 28 to 43 WRITEs admitted in 120 s |
| alone after the warm-up (5e) | off | 3 | all stalled: 27 to 38 admitted |
| after two other cells, 4 x 128, each storage (5e) | off | 2 | both stalled: 29 admitted |
| alone after the warm-up | `credit.bt` | 5 | all ran: 311 to 320 MB/s, about 36000 admitted, 72000 denied |
| after `step-on` | `credit.bt` | 1 | ran: 359 MB/s |

Untraced it stalls every time, 9 runs of 9, whatever ran before it,
on either storage and at either ring size. The trace hides it, so it
is a matter of timing. The traced runs have the same drops and
timeouts in small numbers (zero-window drops 0 to 6, timeouts 0 to 4).

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
`alloc_fail`, no overrun and no ring stopped in any of it; `paused`
was 0 but for three cells (1, 10, 10). **Whether a ring tail-dropped
is not known:** the queue read `rx_hw_tail_drop_packets` after putting
the rings back, and a ring change zeroes it. What stands in for it is
the client's retransmissions: 0 to 100 in every copy cell and every
one-connection cell, 228 to 5642 in the sixteen-connection loan cells
that ran, nearly all of them tail loss probes. The
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
at 128 KiB on 4 x 128; 1977 and 2058 at 128 KiB on 4 x 256 (5c).

**Copy against loan at the same 512 KiB buffer**, 4 x 128, MB/s
(`xeu-copy512.log`, `xeu-copy-4x128-*-rb524288`; the loans are the
512 KiB rows of the tables here, at budgets 630 and 880):

| storage | connections | copy | loan, budget 630 | loan, budget 880 |
|---|---|---|---|---|
| local NVMe | 1 | 1341 | 1536 | 1444 |
| local NVMe | 16 | 1936 | 1989 | 1979 |
| NVMe/TCP | 1 | 1548 | 1406 | 1420 |
| NVMe/TCP | 16 | 2074 | 2044 | 2309 |

At the same buffer the loan and its copy are level: within about 10%
of each other and in both directions, on one run of each. Section 5g
has a second run of the 880 column with the CPU counted.

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
  2309 (NVMe/TCP), against 1936 and 2074 for the copies at that
  buffer.
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

Two repeats of all of the above were queued and cancelled before they
started, on the decision to move to 16 KiB (section 4): every figure
here is one run.

## 5g. CPU per GB written, copy against loan (2026-10-08)

The four step cells on each storage at a 512 KiB held buffer, 4 x 128,
loans at a budget of 880, one run (`xeu-cpu.log`,
`xeu-cpu-4x128-*-b880-rb524288`, `xeu-cpu-*.perf`,
`xeu-cpu-client.samples`). CPU time is the whole host's from
`/proc/stat` over 90 s inside each cell, less the idle rate, over the
gigabytes the cell wrote in that time. The server's figure is kernel
time only (system and softirq): one window had 25 s of user time from
something else on the host. The client's is all busy time less the
lowest rate seen over any 30 s, 2.1 CPU-seconds a second, on a
machine with other work; it is coarse.

| storage | cell | conns | MB/s | server kernel CPU-s per GB | of which softirq | cycles per MB | client CPU-s per GB |
|---|---|---|---|---|---|---|---|
| local NVMe | copy | 1 | 1097 | 0.35 | 0.10 | 1.68 M | 1.0 |
| local NVMe | loan | 1 | 1467 | 0.45 | 0.18 | 1.26 M | 0.8 |
| local NVMe | copy | 16 | 1718 | 0.39 | 0.11 | 1.57 M | 1.2 |
| local NVMe | loan | 16 | 569 | 0.45 | 0.08 | 1.46 M | 1.1 |
| NVMe/TCP | copy | 1 | 1556 | 0.84 | 0.20 | 2.29 M | 0.9 |
| NVMe/TCP | loan | 1 | 1736 | 0.83 | 0.22 | 2.24 M | 0.9 |
| NVMe/TCP | copy | 16 | 2327 | 0.84 | 0.17 | 2.54 M | 1.0 |
| NVMe/TCP | loan | 16 | 2056 | 0.85 | 0.18 | 2.51 M | 1.1 |

- **No saving in server CPU shows for the loan.** On NVMe/TCP, where
  the loan is written direct and nothing is copied by nfsd, the kernel
  time per GB is the same as the copy's to within 2%, and the cycles
  per MB are 1 to 2% lower. On the local NVMe the loan costs about a
  quarter more kernel time at one connection (0.45 against 0.35): there
  it goes through the page cache, which is a copy of its own.
- **The NVMe/TCP figures are mostly the stand-in.** The target is
  `nvmet-tcp` on the same host over loopback, and the NVMe/TCP cells
  cost twice the kernel time of the local NVMe's for the same NFS work.
  A saved copy in nfsd is a small part of that, and this measure may
  not be fine enough to show it. A profile by function would say; one
  was set up for the repeats and cancelled with them. One second of
  profile taken at 64 KiB during a copy cell had `nfsd` in
  `__memmove` at 6% of all samples.
- **The client's CPU does not differ** between copy and loan, as
  expected: about 1 CPU-second per GB.
- **A second run of each cell, and they do not all agree with the
  first** (5e, MB/s, first / this): local NVMe copy 1341 / 1097 and
  1936 / 1718, loan 1444 / 1467 and 1979 / 569; NVMe/TCP copy
  1548 / 1556 and 2074 / 2327, loan 1420 / 1736 and 2309 / 2056. The
  sixteen-connection loan on the local NVMe ran at 569 here, steady
  for the whole cell, with 51649 retransmissions that were all
  spurious tail loss probes (as many DSACKs came back), no timeout and
  no zero-window drop; its first run was 1979. So that cell has two
  speeds and one run does not say which is usual.

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
