# The xeu rig at 16 KiB: where it stands (2026-10-09, morning)

**Read this section first; the handoff below it was written before the
16 KiB boot and is kept for the rig's rules and its scripts.** The
results are in `../XEU_CONVERGENCE.md`: "16K" (sizing), "Hero" and what
follows it (the fast runs), and **"Correction, 2026-10-09"**, which
changes how every native figure from the stripe is read.

## The state of tardis1

- Kernel `7.1.13-21.hs.459.loanpages_pool.el9.aarch64+16k`, `xeu` with
  the refill retry (kmod 2.0.1-4), `rx_ip_align` 2.
- `enp164s0f0` is **down** (Mike, 2026-10-08) and every other mount of
  tardis1 was torn down. Both wait for him.
- A 6144-page region is bound to `0000:a1:00.0`. It cannot be resized
  on this kernel. The port was left at 4 rings of 256.
- **`nvme2` to `nvme7` are an MD raid0, `/dev/md0`, XFS at
  `/hs/stripe`** (1 MiB chunk; not in `/etc/fstab`; made with
  `mdadm --create /dev/md0 --level=0 --chunk=1024 --raid-devices=6`).
  What they held is gone. Their lines in `/etc/fstab` and `/etc/exports`
  are commented out with `#stripe 2026-10-08-stripe#`; the files as they
  were are `/etc/fstab.2026-10-08-stripe` and
  `/etc/exports.2026-10-08-stripe`. `mdadm` was installed. `nvme0` and
  `nvme1` are as they were.
- The drives took about 20 TB each on the night of 8 to 9 October
  (3% to 4% of their endurance used; `xeu-16k-smart-*.txt` in the run
  logs). **Trim the stripe before a run** (`TRIM=/hs/stripe` to
  `sweep.sh`): untrimmed it gives up a fifth.
- nfsd is back at 128 threads; the drives' and `md0`'s `write_cache`
  are back at `write back`; `null_blk` is loaded and unused.

## What was found, in one place

1. Sizing (8 October): rings of 4 x 128, a 256 KiB buffer and a budget
   of four WRITEs are the smallest setting that kept up, at 73% of the
   L3, under a load that the client's verifying fio thread held to
   2.2 GB/s. Use `STEP_JOBS` for anything about rate.
2. On the stripe, durable, three runs each: **the loan 30.1 GB/s, the
   copy 25.4** (sixteen connections, 8 MiB writes, 512 nfsd threads;
   the loan at 16 WRITEs, 4 x 256, 1 MiB; the copy at 4 x 128,
   256 KiB).
3. **Neither was written direct.** `md0` asks for 512-byte memory
   alignment where its drives ask for 4, so nfsd's copy goes through
   the page cache there as a loan does: two copies against one.
4. The wire carries 49 GB/s to sixteen plain streams with a 1 MiB
   buffer held. Nothing on the network side holds the loan at 30: not
   the budget past 16, the rings' depth or number, dropped frames,
   connections or files.

## The next boot

Three things are waiting for a kernel build, all at the tip and all
built but **not booted**:

| commit | what |
|---|---|
| `374836fe6449` | page_pool: the region is given back when its size changes with no pool bound |
| `9a621e18dbce` | md/raid0: the array takes its members' memory alignment |
| (no commit) | a larger region: `./bringup.sh 32768 256` |

On that boot, in order:

1. `cat /sys/block/md0/queue/dma_alignment` is 3 (it is 511 without
   the raid0 change). If the array does not assemble by itself:
   `mdadm --assemble --scan`, then `mount -o noatime /dev/md0 /hs/stripe`.
2. `./bringup.sh 32768 256`, then `fstrim /hs/stripe`.
3. The copy on the stripe, and that it is written direct now
   (`wr_direct` in `summary.tsv`):
   `PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./sweep.sh $L/<outdir> plans/16k-17-best.plan`
   with nfsd at 512 threads (`echo 512 > /proc/fs/nfsd/threads`).
   That is the comparison the night was thought to be making: a direct
   copy against a loan through the page cache.
4. With the room the larger region gives: rings of 4 x 1024 or more
   with a 1 MiB or 2 MiB buffer and budgets of 16 to 64
   (`plans/16k-15-budget.plan` and `16k-20-deeprings.plan` are the
   shapes; none of their larger lines fitted in 6144 pages).
5. To test the region patch: with the port down,
   `echo 16384 > /sys/module/page_pool/parameters/rx_region_pages`, port
   up, and `/sys/kernel/debug/page_pool_rx_region` shows the new size
   and the kernel log the old region freed.

A loan written direct with no hop in front of it has no storage on this
host: these drives need whole pages, a memory-backed null_blk copies
under one lock (6 GB/s) and brd asks for 512-byte alignment. NVMe/TCP
on the same host is the only one, at about 20 GB/s.

## Scripts added on 8 and 9 October

`sweep.sh` takes `TRIM=`, `PRP_DIR=`, `SGL_BACKING=`, `PRP_STORAGE=`;
`run-budget.sh` takes `STEP_JOBS=`, `STEP_BS=`, `STEP_SIZE=`,
`STEP_IODEPTH=`, `STEP_MOUNTS=`, `EXPORT_SYNC=`. `capture-sweep.sh` runs
`where.sh` (both ends' CPUs, the rings, a profile, the client's view of
each connection) and `anat.bt` (what a loaned WRITE waits for) inside
every cell. `nfsd-stacks.sh` says where nfsd's threads are;
`wirebuf.sh` what the wire carries at each held buffer;
`nvme-baseline.sh` what the storage takes; `napi.bt` whether the
receive poll is held off (written, not yet used). **A sweep started
detached is not waited on by anything: start the next one from a driver
script, or it will sit idle** (it did, for two and a half hours).

---

# The xeu rig: 16 KiB handoff (written 2026-10-08 on the 64 KiB boot)

**A session on tardis1 after the reboot into the 16 KiB kernel starts
here.** Mike built the 16 KiB kernel himself and is installing it; this
file and the scripts beside it are everything the last session knew
that the tree did not.

## The job, and its priority (Mike, 2026-10-08)

Find the best sizing of the receive region against the L3 at 16 KiB, on
the real hardware: the `xeu` port `enp161s0f0`, the client
192.168.1.106, the real NVMe as it is (PRP, needs whole pages) and
behind an NVMe/TCP target (takes arbitrary pieces). **The full battery
of sizing runs is the top priority, and solid 16 KiB results are wanted
the same day.** Everything else (docs still owed, repeats of 64 KiB)
waits.

"Sizing" is four numbers and what they cost: the ring size, the held
receive buffer, the budget (in WRITEs of 1 MiB admitted at once), and
so the region pages in use at peak. The answer wanted is the smallest
footprint, against half of the L3 and the whole of it, at which sixteen
connections' loans run level with their copies with no stall and no
drop, on each storage.

## Rules that do not change (XEU_CONVERGENCE.md section 0)

- Load goes only to the harness's own mount of tardis1 on the client.
  Never to the client's other mounts, never to tardis1's other exports.
- Only `/hs/nvme0` and `/hs/nvme1`, as files. No drive is reformatted.
- Only `enp161s0f0`. **`enp164s0f0` carries another client's
  (192.168.0.105) connections and is never touched, so `xeu` is never
  reloaded.** A reboot is Mike's to do.
- Commit at the tip of `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`; no
  regrouping (AGENTS.md).

## First ten minutes on the 16 KiB boot

1. `getconf PAGESIZE` is 16384 and `uname -r` ends `+16k`.
2. `cat /sys/module/xeu/parameters/rx_ip_align` is 2
   (`/etc/modprobe.d` has `options xeu rx_ip_align=2`).
3. **Is the driver's retry patch in?** `grep -c xeu_rx_refill_timer
   /proc/kallsyms` (1 is yes) and `rpm -q kmod-16k-xsight` (release 4
   carries it). The patch is `kernel-rpmtree` commit `f61ea4c` (not
   pushed); it was tested at 64 KiB on 2026-10-08 and works
   (`XEU_CONVERGENCE.md` 5c). **Mike expects it to be missing from the
   16 KiB kmod and wants release 4 rebuilt against the 16 KiB kernel by
   the same method (2026-10-08). Do this before `bringup.sh`:** with no
   region bound yet, the reload needs no care about the rings.

   1. `./build-kmod.sh` builds `kmod-16k-xsight` 2.0.1-4 for the
      running kernel from the `kernel-rpmtree` spec and patches, into
      `/var/tmp/zc-run-logs/kmod-xsight-16k-build`, and checks the
      result: the vermagic must be the running kernel's and "retry
      patch in xeu.ko" must be 1. It installs and loads nothing. About
      two minutes. It needs `kernel-16k-devel` and
      `kernel-16k-modules-core` of the running kernel; if either is
      missing, say so to Mike.
   2. Installing it and reloading `xeu` takes `enp164s0f0` down for a
      few seconds and drops the other client's connections, so **it
      needs Mike's leave, and the session's permissions may refuse it
      even then** (they did at 64 KiB until he gave the commands
      himself). Give him these to approve or to run, each as its own
      prompt starting with `!`:

          ! rpm -Uvh /var/tmp/zc-run-logs/kmod-xsight-16k-build/RPMS/aarch64/kmod-16k-xsight-2.0.1_*-4.el9.aarch64.rpm
          ! rmmod xeu && modprobe xeu

      `rpm -U` removes every other installed `kmod-16k-xsight`, since
      they share a name (at 64 KiB it took the kmods of builds 454 and
      455 with it). If a region is already bound, first
      `echo none > /sys/module/page_pool/parameters/rx_region_dev`: a
      fresh load gives the port 8 rings of 1024.
   3. Afterwards: `grep -c xeu_rx_refill_timer /proc/kallsyms` is 1,
      `rx_ip_align` is 2, both ports have their addresses, and
      192.168.0.105's connections are back
      (`ss -tn state established '( sport = :2049 )'`).

   Without the patch a ring that runs empty stays empty until the port
   is taken down and up; the scripts here bounce the port when the
   region runs dry and the patch is absent, so the sweep can run
   either way, but say which it was.
4. Nothing has made a region: `/sys/kernel/debug/page_pool_rx_region`
   does not exist. **Run nothing on veth this boot** (the veth harness
   would make the region for `rxsrv`).
5. `./bringup.sh 6144 128`: a 6144-page (96 MiB) region bound to the
   port's PCI device and rings of 4 x 128. One boot has one region
   size; 6144 has room for the widest line of the plans (rings of
   4 x 256, sixteen WRITEs, 512 KiB buffers). The region is only where
   the pages come from: what is measured against the L3 is the pages
   in use at peak (`used_pages`), which follows the rings, the buffers
   and the budget and not the region's size.

## Then, in this order

Everything long is started detached and waited on by PID:

    setsid nohup ./sweep.sh <outdir> <plan> > <log> 2>&1 < /dev/null &
    tail --pid=<pid> -f /dev/null      # as a background command

1. **`./ringsize.sh 64 128 256`** (about 6 minutes): a plain network
   load at each ring size, buffers free and held. It says the smallest
   ring that does not tail-drop. At 64 KiB that was 256 for one stream
   left to tune itself and 128 for sixteen.
2. **`./sweep.sh $L/xeu-16k-smoke plans/smoke.plan`** (3 minutes), and
   read its `geometry.txt`. **The harness's one-frame-per-page geometry
   has never run at 16 KiB.** Expected by arithmetic: 1 MiB is 64
   pages of payload and about 118 frames, so a factor near 185 and a
   charge near 120 pages a WRITE; a 128 KiB buffer is 9 pages. If the
   factor is far from that, stop and look (`FRAME_PAGE` in
   `run-budget.sh`). Then check that a budget "of n WRITEs" admits n:
   during a sixteen-connection loan cell,
   `sysctl net.sunrpc.region_budget_stats` should show `inflight`
   reaching n and no more. `sweep.sh` works the budget out as
   n x CHARGE + reserve + 4; if `inflight` tops out below n, CHARGE is
   low (trace one cell with `credit.bt`: `ADMIT ... credit=` is what
   the kernel charges).
3. **`plans/16k-1-core.plan`** (1 h 50 min; the lines are in order of
   priority and a stopped sweep resumes). Rings of 4 x 128, both
   storages, buffers of 256, 128 and 512 KiB, budgets of 1, 2, 4 and 8
   WRITEs, with each buffer's copies.
4. **`./summarize.py <outdir>`** prints the sizing table: copy and loan
   at one and sixteen connections, the pages in use and their share of
   the L3, CPU per GB, and a note where a cell stalled, dropped, paused
   or ran the region dry.
5. **`plans/16k-2-rings.plan`** (70 min) at the buffer the core showed
   best (edit the plan's buffer first), then **`plans/16k-3-wide.plan`**
   (30 min).
6. **Repeats of the shortlist.** A sweep skips lines listed in its
   `done` file, so a repeat is the same plan into a new outdir;
   `summarize.py` takes several outdirs and shows the median and range.
   `PERF=1` adds cycles and instructions per cell; `PROFILE=1` a 30 s
   flat profile by function per cell (not both).
7. Record as it comes in a "16K" section of `XEU_CONVERGENCE.md` shaped
   like its "64K" one, and tell Mike the table.

## What 64 KiB showed, to set 16 KiB against

The account is `XEU_CONVERGENCE.md` section "64K" (18 numbered
findings). In short, at 64 KiB on this card:

- a 1 MiB WRITE was about 117 pages, 7.3 MiB; rings that did not drop
  were twice the L3; no working setting fitted in half of the L3 (the
  best cells peaked at 1500 pages, 94 MiB);
- sixteen connections on a 128 KiB buffer (two frames) and a budget of
  one WRITE stalled, 9 untraced runs of 9, through TCP backoff on the
  client; a 256 KiB buffer or a budget of four WRITEs ended it;
- with a 512 KiB buffer and a budget of six WRITEs the loan and its
  copy were level, about 1.4 to 1.5 GB/s at one connection and 2.0 to
  2.3 at sixteen; one cell did not repeat (1979, then 569);
- no saving in server CPU showed for the loan (0.84 against 0.84
  kernel CPU-seconds per GB on NVMe/TCP, 0.35 against 0.45 on the local
  NVMe);
- baselines: the drive 10.4 GiB/s, the wire 7.0 GB/s to one connection
  left to tune itself, 3.9 GB/s to sixteen at 128 KiB, 0.9 to 1.2 GB/s
  to one at 256 KiB.

At 16 KiB a frame still has a page to itself, but the page is a
quarter the size: a WRITE is about 1.8 MiB of region, the L3 is 2048
pages, and a 128 KiB buffer is eight frames. Whether the stall, the
level rates and the misses change with that is what the plans show.

## What went wrong before, so it does not again

- **`pgrep -f` and `pkill -f` match the shell that runs them.** A queue
  of jobs each waiting for "no `run-budget.sh` process" waited on
  itself all night, and a `pkill -f` killed its own shell. Wait on a
  PID (`tail --pid`), kill by PID.
- **Ring counters are zeroed by a ring change and by down/up.** Read
  `rx_hw_tail_drop_packets` before, not after. `sweep.sh` reads it
  inside each cell.
- **The harness's derived budget is wrong on this card.** It reads the
  parked pages too early; the pools then keep several hundred more. A
  derived budget ran the region dry and stopped a ring. Always
  `FORCE_BUDGET` (which `sweep.sh` does) and let the guard skip what
  does not fit.
- **`cells.tsv`'s `drops` column is the interface's, not the ring's**,
  and reads about 125 for every cell whatever the load.
- **The `seq-sized` cells are too short to compare rates** (512 MiB,
  under a second). Use the 120 s `step` cells.
- **Attaching `credit.bt` hides the sixteen-connection stall.** Trace
  to read the accounting, never to decide whether a cell stalls.
- **One run is not a result.** The same cell ran at 1979 and at 569.
- **The region is named by the PCI device** (`0000:a1:00.0`), and
  `ethtool -G` works only with the port up, `-L` only with it down.
- **`fio` returns 0 from a stalled cell.** Read the rate and
  `admitted`.

## Where things are

- `common.sh`, `build-kmod.sh`, `bringup.sh`, `ringsize.sh`, `sweep.sh`,
  `summarize.py`, `plans/`: this directory. `rcv.py` and `snd.py` are `ringsize.sh`'s
  sender and receiver (only the server's receive buffer is set).
  `credit.bt` is the admission trace.
- `../run-budget.sh`: the harness (`NETDEV=`, `CLIENT=`, `STORAGE=`,
  `EXPORT_DIR=`, `BACKING=`, `FORCE_BUDGET=`, `FORCE_RCVBUF=`,
  `CELLS=`).
- Run output: `/var/tmp/zc-run-logs/` (`xeu-*`; it is on the root file
  system and survives the reboot). The 64 KiB runs are `xeu-q-*`,
  `xeu-step-*`, `xeu-cpu-*`, `xeu-llc-*`, `xeu-copy-*`.
- The client: `ssh -i /root/.ssh/id_ed25519_snitzer2 root@192.168.1.106`
  (x86_64, 4 KiB pages, 48 CPUs; it has other work, so its idle CPU is
  not zero).
- The artifact: "NFSD Receive Region Budget",
  <https://claude.ai/artifact/3UQkMVoWW8p9A5ZASPnm3g>. Its source is
  `../artifacts/region-budget.html` (also
  `/var/tmp/zc-run-logs/artifact/region-budget.html`); publish to the
  same URL. Its 64 KiB section opens with the summary of the xeu runs
  (`id="xeu64"`); a 16 KiB one belongs at the top of the 16 KiB section
  when there are results, and the page's lede names the rigs.
- Still owed, after the 16 KiB results: `transplant/tardis1-build457-handoff.md`,
  `transplant/README.md` and `RESULTS.md` say nothing of the xeu runs.
