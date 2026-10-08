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
3. **Is the driver's retry patch in?** `grep xeu_rx_refill_timer
   /proc/kallsyms`, and `rpm -q kmod-16k-xsight` (2.0.1-4 carries it).
   The patch is `kernel-rpmtree` commit `c405aa5` (not pushed), built
   and never loaded: this boot may be its first run. Without it a ring
   that runs empty stays empty until the port is taken down and up
   (5c); the scripts here bounce the port when the region runs dry and
   the patch is absent. **If it is in, the first dry region is the
   test of it: check every ring still receives (`ethtool -S`,
   `ring_N_rx_packets`) and write down what happened.**
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

- `common.sh`, `bringup.sh`, `ringsize.sh`, `sweep.sh`, `summarize.py`,
  `plans/`: this directory. `rcv.py` and `snd.py` are `ringsize.sh`'s
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
