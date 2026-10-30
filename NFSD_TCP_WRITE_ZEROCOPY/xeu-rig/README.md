# tardis1 on build 504, third boot (29 to 30 October, night): two drives stall on their own, six drives give stack 64 its answer — read this first

The power cycle found all eight data drives with no rescan, in bus
order (`nvme0n1` to `nvme7n1` on buses 01 to 71, the Micron
`nvme8n1`), all `write through`. Port b answered. Logs, the run scripts
and `results.tsv` are in `$L/t1-504/`, this boot's checks in `boot3/`.

**Steps 1 and 2 passed** (`boot3/`): sunrpc `666175BED7A343E376C1742`
(the module's own), xeu `CA0931345E46C929B11D01F`, `arm_cmn` loaded;
after `./bringup.sh 32768 256`, `./port-setup.sh enp164s0f0 56` and
`settled.sh apply`, the two `mapped` lines and "the settled setting".
A slot's life on both ports: each line goes and comes back,
`escaped` 0, no WARNING. KUnit 7, 52, 7 and 10, no failure, no skip
(`run-kunit.sh` refuses to run under an active nfs-server: stop it
first; the script starts it again). Both ports answered after each
step. No WARNING in dmesg all session.

**Step 3, the drives on their own** (`probes.sh`, `drv-D1` to `drv-D3`,
each drive's `sec.tsv`): all eight at 7.5 GB/s with an fsync every
8 MiB, 75 s, about 560 GB a drive. `drvchar.py`'s `PAUSE` ran for the
first time and works.

| drive | D1, no pause | D2, every drive stopped at 12 s for 8 s | D3, as D2 with no fstrim just before |
|---|---|---|---|
| `/hs/nvme0` (bus 01) | stops from 6 s (about 45 GB) | the same | the same |
| `/hs/nvme4` (bus 41) | one stop of about 10 s at 315 GB | at 390 GB | at 503 GB, still stopped at the end |
| the other six | 7.5 GB/s throughout | 7.5, and caught up after the pause | the same |

- `/hs/nvme0` stops every 10 to 20 s: 0.12 GB/s at 100% busy for 2 to
  7 s, then 11.8 GB/s until it has caught up. That begins far inside
  the fast region. Boot 2 saw it stop for about 2 s at a time too; this
  boot had no rescan, so the rescan was not what was wrong.
- `/hs/nvme4` goes to 0.28 GB/s at 100% busy for about 10 s once a
  probe, earlier than 12 October's 625 GB.
- The 8 s stall of every drive at once did no harm to the other six,
  and the fstrim just before made no difference.
- Not heat or wear: 22 to 32 C composite, 44 to 63 C sensor 1;
  `critical_warning` 0, `media_errors` 0, 16 to 21% used, firmware
  `830ZRR02` on all. `/hs/nvme0` holds 29 GB (`hke.git`, `thayne`)
  against 14 on the others.

By the plan that stops step 5. **Mike: run on six drives**, without
`/hs/nvme0` and `/hs/nvme4`: `FS_A="/hs/nvme1 /hs/nvme2 /hs/nvme3"
FS_B="/hs/nvme5 /hs/nvme6 /hs/nvme7"` (`twoport.sh` takes them as they
are; `JOBS` 16 gives 5 writers a filesystem, 15 a port). Each drive then
takes about 10 GB/s, not 7.5.

**The runs** (each after an fstrim of all eight and 120 s of rest,
`DEFER=0/0 RCVBUF=393216 ENTRIES=128`, `DRIVE_GB=450`; scripts
`run-6d-*.sh`, logs `run-6d-*.log`). At `WARM=10 RUNTIME=30` the six
drives passed 450 GB and stepped down (`6d-short-A-1`: five of six at
33 to 36 s, 43.30 GB/s), so the round was stopped and run at **`WARM=8
RUNTIME=20`**, about 340 to 430 GB a drive:

| run | stack | GB/s | wire a + b in the window | WRITEs/s | ring / nfsd kcycles a WRITE | peak / mean in use | client retransmits | drives |
|---|---|---|---|---|---|---|---|---|
| `6d-s20-A-1` | 0 | 58.15 | 28.98 + 29.66 | 55.5k | 96.7 / 1088.6 | 1296 / 1296 | 16 | none stepped down |
| `6d-s20-s64-1` | 64 | 57.68 | 29.84 + 29.19 | 55.7k | 97.6 / 1065.1 | 1180 / 954 | 16 | none stepped down |
| `6d-s20-A-2` | 0 | 58.10 | 29.31 + 29.27 | 55.4k | 98.5 / 1099.4 | 1196 / 1193 | 19 | none stepped down |
| `6d-s20-s64-2` | 64 | 25.84 | 10.20 + 10.44 | 18.7k | 98.7 / 690.1 | 1194 / 1133 | 891 | does not count: `/hs/nvme2` and `/hs/nvme3` stalled at 11 s |
| `6d-s20-s64-2r` | 64 | 58.42 | 29.39 + 29.54 | 55.6k | 96.1 / 1050.8 | 1018 / 952 | 13 | none stepped down |
| `6d-s20-d2-1` | 0, `DEFER=2/10000` | 53.34 | 27.44 + 27.46 | 51.8k | 93.6 / 1037.9 | 1247 / 1247 | 20 | does not count: `/hs/nvme6` stepped down at 12 s |
| `6d-s20-d2-2` | 0, `DEFER=2/10000` | 51.29 | 26.39 + 25.29 | 48.8k | 93.9 / 981.1 | 1280 / 1115 | 114 | does not count: `/hs/nvme5` stepped down at 20 s |
| `6d-s20-d2-1r` | 0, `DEFER=2/10000` | 58.35 | 30.14 + 28.74 | 55.6k | 92.1 / 1002.0 | 1201 / 1185 | 12 | none stepped down |
| `6d-s20-d2-2r` | 0, `DEFER=2/10000` | 52.09 | 25.62 + 24.36 | 47.0k | 95.7 / 932.2 | 1191 / 1154 | 6 | does not count: `/hs/nvme5` stepped down at 14 s |

In every run: `alloc_fail` 0, `escaped` 0, `free_unmapped` 31616 and
both slots `free 0` after, both budgets' `admitted` unchanged (3096
and 2), no new WARNING; the host was left at the settled setting,
stack 0. These are six drives and 15 writers a port, so the rates do
not compare with 503's or boot 2's; the settings compare with each
other.

**Stack 64 against 0, with the mapping: no cost, and fewer pages.** Two
runs of each that count, all at the same rate (57.7 to 58.4 GB/s,
55.4k to 55.7k WRITEs/s):
- nfsd 1065.1 and 1050.8k cycles a WRITE at 64, against 1088.6 and
  1099.4 at 0: 3.3% fewer, both runs at 64 below both at 0. The rings
  are the same within noise (96.9k against 97.6k).
- The region's mean in use 954 and 952 at 64, against 1296 and 1193:
  about 23% fewer. The peak too: 1180 and 1018, against 1296 and 1196.
  On 503, without the mapping, stack 64 cost 3 to 7% more nfsd cycles;
  with it, it holds fewer of the region's pages, not more.
- **`settled.sh` holds the stack at 64 since the morning of 30 October
  (Mike, `9fe6781919d1`).** `settled.sh apply` opened both ports again
  for it and ended in "the settled setting"; the runs below are at 64.

**Deferral 2/10000 on both ports: 6% fewer ring cycles a WRITE, at
stack 0 and at 64.** At stack 0, `d2-1r` beside the stack-0 runs: the
same rate (58.35 GB/s, 55.6k WRITEs/s), the rings 92.1k cycles a WRITE
against 96.7 and 98.5 (6% fewer; 503's one run showed 7%), nfsd 1002.0k
against 1088.6 and 1099.4 (8% fewer), the region's mean in use about the
same (1185). Its partner did not count in two tries. Then at stack 64,
after it was settled (`run-6d-s20-s64-d2*.sh`), against `s64-1` and
`s64-2r`:

| run | stack | GB/s | wire a + b in the window | WRITEs/s | ring / nfsd kcycles a WRITE | peak / mean in use | client retransmits | drives |
|---|---|---|---|---|---|---|---|---|
| `6d-s20-s64-d2-1` | 64, `DEFER=2/10000` | 58.36 | 29.42 + 29.44 | 55.6k | 90.4 / 1019.6 | 1009 / 943 | 11 | none stepped down |
| `6d-s20-s64-d2-2` | 64, `DEFER=2/10000` | 49.47 | 26.79 + 26.49 | 50.3k | 94.1 / 1006.4 | 1205 / 1007 | 15 | does not count: `/hs/nvme6` and `/hs/nvme7` stepped down at 12 and 25 s |
| `6d-s20-s64-d2-2r` | 64, `DEFER=2/10000` | 58.38 | 29.65 + 29.27 | 55.7k | 91.5 / 1038.6 | 1018 / 945 | 13 | none stepped down |

The two that count: the same rate (58.4 GB/s, 55.6k to 55.7k WRITEs/s),
the rings 90.4 and 91.5k cycles a WRITE against 97.6 and 96.1 at 0/0
(6% fewer), nfsd 1019.6 and 1038.6k against 1065.1 and 1050.8 (3%
fewer), the region's mean in use the same (943 and 945 against 954 and
952). Both pass the plan's test for `DEFER[b]` to become 2/10000 in
`settled.sh`; that is Mike's call, and port a is already at 2/10000.

**The xeu-off run** (`6d-xeuoff-1`, `rx_prefetch`, `rx_gro_frags` and
`irq_msix_mask` at N, `WARM=10 RUNTIME=30`, stack 0) does not count:
`/hs/nvme5` and `/hs/nvme7` stepped down at 14 and 17 s, 39.37 GB/s.
Its rings took 114.1k cycles a WRITE, against 96 to 98k with the three
at Y. It says nothing yet about xeu and the stalls; all three were put
back to Y.

**The drives are still what decides a run.** Six of the twelve runs
on six drives were decided by drives: `6d-short-A-1` by its length,
the other five by a stall. `/hs/nvme5` (bus 51) stepped down in four of
them (`6d-xeuoff-1`, `6d-short-A-1`, `d2-2`, `d2-2r`). In `d2-2r` it went alone to 0.75 GB/s at 100% busy for
8 s while its port's other two wrote at 11 GB/s; in `s64-2`,
`/hs/nvme2` and `/hs/nvme3` went to 0.75 GB/s at 100% busy at 11 s and
every other drive, on both ports, fell to 1.4 to 3 GB/s at 10 to 30%
busy, starved of WRITEs. A drive that stalls on one port stalls the
other port's too (nfsd's one pool is the likely path; not measured).
`/hs/nvme5` held 7.5 GB/s in all three probes; at about 10 GB/s it does
not.

**What is open:**
- The drives: `/hs/nvme0`, `/hs/nvme4` and now `/hs/nvme5` stall on
  their own, all on firmware `830ZRR02`, none hot or worn. Whether that
  is the drives, their slots or the firmware is not known.
- Step 4 (the `STALLED` and `low in the warm-up` lines in `drives.txt`)
  is still to be written; `drives-sec.tsv` beside `wire-sec.tsv` shows
  the stalls today by hand.
- `DEFER[b]` 2/10000 in `settled.sh` (Mike's call; the runs for it are
  in); xeu's three parameters off wants a run that counts.

# tardis1 on build 504, second boot (29 October, late): four drives found only by a rescan, the runs decided by drives, power cycle again

The hard power reset brought port b back, but the boot found only four
of the eight data drives. The copy runs on that boot were decided by
drives stepping down, the four rescanned ones first. Mike is power
cycling again. Logs are in `$L/t1-504/`: this boot's checks in `boot2/`,
the first boot's stopped `two-copy-A-1` in `boot1/`.

**After the power cycle, before anything else:**
1. `lspci | grep -c Non-Volatile` is 9, and `lsblk` shows `nvme8n1` as
   the Micron root drive and `nvme0n1` to `nvme7n1` as the WD_BLACKs.
   **If a drive is missing again, stop and tell Mike:** a drive that is
   late to answer at a cold start is a fault of its own (firmware, or
   the firmware's wait for slow devices), not something to work around
   with a rescan.
2. Port b answers: `ping -I enp164s0f0 192.168.0.106`.
3. Steps 1 and 2 of the plan below again.
4. **Step 3, the drives on their own, before any NFS run:**
   `$L/t1-504/probes.sh` runs D1, D2 and D3 as the plan has them, about
   12 minutes in all. If the drives hold their fast region (as on 12
   October: about 625 GB), the rescan is what was wrong with this boot.
5. Then `$L/t1-504/run-xeuoff.sh` (below), then a round at the short
   length (`$L/t1-504/run-short.sh`: A, s64, A, s64 at `WARM=10
   RUNTIME=30`), and only once the probes are clean.

**The drives.** `nvme`s on root ports 00, 10, 20 and 30 (buses 01 to
31) were missing at boot, although each port's link was up at 32 GT/s
x4. `echo 1 > /sys/bus/pci/rescan` found all four. The kernel names
them in the order it finds them, so the names moved: the root drive was
`nvme4n1`, the drives on buses 41 to 71 `nvme0n1` to `nvme3n1`, and the
rescanned ones `nvme5n1` to `nvme8n1`. They were mounted by bus, which
is the usual order (01 is `/hs/nvme0` and holds `hke.git` and `thayne`,
as the usual `nvme0` does):

| `/hs/` | nvme0 | nvme1 | nvme2 | nvme3 | nvme4 | nvme5 | nvme6 | nvme7 |
|---|---|---|---|---|---|---|---|---|
| bus | 01 | 11 | 21 | 31 | 41 | 51 | 61 | 71 |
| device on this boot | nvme5n1 | nvme6n1 | nvme7n1 | nvme8n1 | nvme0n1 | nvme1n1 | nvme2n1 | nvme3n1 |

`twoport.sh` and `settled.sh` find the drives through their mounts and
were right. `drvchar.py` took drive `n` to be `/dev/nvme<n>n1` and now
reads through the mount too. `drives.txt` and `drives-sec.tsv` name the
kernel's devices, so on this boot `nvme5` there is `/hs/nvme0`.

**Steps 1 and 2 passed again** (`boot2/`): the mapping's two `mapped`
lines; the settled setting after the bring-up, the slot test and
KUnit; a slot's life on both ports with `escaped` 0 and no WARNING;
KUnit 7, 52, 7 and 10; port b answered after each.
(`dmesg -c` was run once before the slot test by mistake, so the boot's
first 10 minutes of the kernel log are gone. The probe lines had been
read, and the WARNING counts from then on are whole.)

**The runs** (`run.log`, `run-r.log`): `run.sh` as the plan has it
(`DEFER=0/0 RCVBUF=393216 ENTRIES=128`, `WARM=15 RUNTIME=45`, each after
an fstrim and 120 s of rest), then A-1 once more at `WARM=10 RUNTIME=30`
(`run-r.sh`). The region, the slots and the budgets held in every run:
`alloc_fail` 0, `escaped` 0, `free_unmapped` 31616 and both slots
`free 0` after each, nothing admitted for loans, no new WARNING.

| run | stack | GB/s | wire a + b in the window | WRITEs/s | ring / nfsd kcycles a WRITE | peak / mean in use | client retransmits | drives |
|---|---|---|---|---|---|---|---|---|
| `two-copy-A-1` | 0 | 60.48 | 30.54 + 30.10 | 57.4k | 96.5 / 1068.9 | 1200 / 1200 | 10 | `/hs/nvme6` stepped down at 44 s (about 450 GB from the trim) |
| `two-copy-s64-1` | 64 | 61.39 | 31.18 + 30.59 | 58.3k | 94.8 / 1005.2 | 1217 / 961 | 7 | none stepped down; up to 576 GB |
| `two-copy-A-2` | 0 | 59.22 | 27.95 + 28.38 | 53.1k | 94.2 / 991.8 | 1280 / 1277 | 119 | stalls at 5 to 9 and 11 to 14 s; `/hs/nvme4` stepped down at 36 s (about 345 GB) |
| `two-copy-s64-2` | 64 | 35.67 | 10.51 + 10.37 | 19.4k | 106.4 / 710.0 | 1221 / 1002 | 440 | collapse at 33 s; six stepped down |
| `two-copy-A-1r` (30 s) | 0 | 60.82 | 31.00 + 30.89 | 58.5k | 96.1 / 1043.7 | 1120 / 1104 | 9 | `/hs/nvme0` stepped down at 32 s (about 350 GB) |

Only `s64-1` counts. There is no answer from this boot on stack 0
against 64; `s64-1` beside `A-1r` (both at about 58.4k WRITEs/s) has
4% fewer nfsd cycles a WRITE and 13% fewer pages in use on average,
the opposite of 503's cost, which is what the mapping is for. It wants
the round run again on drives that hold.

**How the stalls start** (`drives-sec.tsv` beside `wire-sec.tsv`):
- In `A-2` at 5 s and `s64-2` at 33 s, `/hs/nvme1` to `/hs/nvme3` (three
  of the four rescanned drives) went in the same second to 0.8 GB/s at
  100% busy. In that second every other drive, on both ports, fell to
  about 1 GB/s at 5 to 20% busy: idle, starved of WRITEs. The wire then
  fell on both ports (to 4 GB/s for 10 s in `s64-2`) and the client
  retransmitted. The likely path is nfsd's one pool filling with WRITEs
  held by the slow drives, so that the other port's drives starve too;
  that has not been measured.
- Then other drives went to the same 0.75 to 0.8 GB/s at 100% busy:
  `/hs/nvme0` (rescanned) and `/hs/nvme5`, `/hs/nvme7`. `/hs/nvme0` also
  stopped for about 2 s at a time (0.12 GB/s, 100% busy) in `A-1`,
  `s64-1` and `A-2`, too short for the step-down rule.
- Every step-down came at 345 to 450 GB from the trim (the placement
  load's 130 GB or so counted), well inside the 625 GB of 12 October,
  and the shorter run did not avoid it. The drives were at 23 to 30 C
  (composite) and 44 to 57 C (sensor 1): not heat.
- `drives.txt`'s `STEPPED DOWN` misses most of this: it looks at one
  drive at a time and only after the warm-up. Step 4's `STALLED` line is
  still to be written.
- Separately, the first 10 s or so of every run (placement and the
  start of the warm-up) has the client retransmitting 3,000 to 15,000
  segments a second, with `TCPOFOQueue` in the thousands. There is no
  per-second record from earlier builds to tell whether that is new.

**Is it xeu?** Builds 503 and 504 are tardis1's first with kmod-xsight
2.0.1-13 and `rx_prefetch`, `rx_gro_frags` and `irq_msix_mask` at Y
(also `skb_defer_max` 0 and NAPI's deferral), and 503 is where the
stalls began. But the stalls traced here start with drives at 100% busy,
which the network cannot cause, and the wire falls after them. Port b's
loss of receive on the first boot and the retransmits at the start of a
run are still open to it. `run-xeuoff.sh` is one `A` run at the short
length with all three parameters at N (`EXP_XEU_*`; `settled.sh apply`
reopens the ports for `irq_msix_mask`, and the script's end puts them
back to Y).

**Changed in the rig:** `drvchar.py` reads a drive through its mount,
and has `PAUSE=<second>:<seconds>` for step 3: it stops each drive's
fio by its process group and starts it again, and leaves the paused
seconds and the two after them out of the step-down test. fio's rate is
an average from its start, so after a pause the drives write flat out
until they have caught up, as a client does with its backlog. It has
not yet run against the drives. `twoport.sh`'s `DRIVE_GB` (600) is
unchanged; the runs above passed 450.

# tardis1 on build 504 (29 October, night): port b receives nothing, power cycle

Build 504 (`7.3.0-7.hs.504.loanpages_pool`) is the first kernel on
tardis1 with `0c375e06bf64`. Steps 1 and 2 of the plan below passed;
the copy runs of step 5 never started, because port b stopped
receiving. Mike is giving the host a hard power reset to get the port back.

**What passed** (`$L/t1-504/`):
- sunrpc's srcversion is the module's own; xeu is the build's
  `kmod-16k-xsight-…504…-13`, srcversion `CA0931345E46C929B11D01F`.
- After `./bringup.sh 32768 256` and `./port-setup.sh enp164s0f0 56`,
  the region has two `mapped` lines, `0000:a1:00.0` and `0000:a4:00.0`,
  each `dir 2 pools 4`; `settled.sh apply` ends in "the settled setting".
- A slot's life (`slot-life.txt`): each port down takes its line away
  and gives its pages to `free_unmapped`, up brings it back, `escaped`
  0, no WARNING.
- KUnit (`kunit.log`): 7, 52, 7 and 10, no failure, no skip, as on 503.

**What failed.** `run.sh` (A-1, s64-1, A-2, s64-2: stack 0 and 64 in
turns, with `wiresample.sh`'s `wire-sec.tsv` and a `client-sec.tsv`
beside each run) stopped at its first run: `b: no answer from
192.168.0.106 in 20 s`. Port b (`enp164s0f0`, 192.168.0.211) is up
with a link, and sends: its ARP requests for .106 show in tcpdump and
`tx_packets` counts them. It receives nothing: `rx_packets` 0,
`rx_dropped` 10, `port_rx_fifo_tc0_packets` stuck at 1804,
`port_rx_non_prom_drop_packets` 1. The client's `ens6np0` is up at
400 Gb/s and cannot ping .211 either. Port a works. There is no IOMMU
on the ports (no `iommu_group`) and dmesg has no fault. A reopen by
`./port-setup.sh enp164s0f0 56` did not bring it back.

This is the card's known failure (Mike): port b stops receiving from
time to time, for no known cause, and only a hard power reset brings
it back; a reboot or a reopen does not. **After the power cycle, check
that port b answers** (`ping -I enp164s0f0 192.168.0.106`) after the
bring-up and again after the slot test, so that a lost port is caught
before a run.

**Then** steps 1 and 2 again, then `$L/t1-504/run.sh` (Mike: test
`page_pool.rx_region_stack` 64 first; the stack-0 runs alternate with
it for the comparison). Steps 3 and 4 (the drives on their own, the
STALLED line) were put after it.

# tardis1's next boot: the per-device mapping, the drives and the stalls (plan of 29 October, evening) — read this first

Mike is installing a kernel built from `0c375e06bf64` or later, the
region's per-device mapping, on tardis1. Build 503, the kernel before
it, lacked that commit (next section). This is the plan for the first
session on the new kernel, in order. Nothing in it has run.

**Why the plan changed: the drives' rule did not keep 503's runs
clean.** The rule is fstrim, rest, then `WARM=15 RUNTIME=45` to stay
inside a drive's 625 GB fast region ("What a drive takes",
`../XEU_CONVERGENCE.md`). Three of 503's five runs were decided by drops
in what the drives wrote, and `drives-sec.tsv` shows the rule is not
what failed:
- **Every drop began far inside the fast region**, 1 to 350 GB into a
  run, after 120 s of rest and `twoport.sh`'s own 30.
- **Several drops hit all eight drives in the same second, on both
  ports**: `s64-1` at 11 to 15 s, down to 8 GB/s in all; `s64-2` at 34
  to 44 s, to 5 to 8, with 265 client retransmits. The ports have
  their own drives and filesystems, so the stream stopped upstream of
  them: in the client, the wire or the server.
- **A drive that stepped down mostly did so just after such a stall.**
  The 12 October probe saw pauses of 0.3 to 3 s do no harm inside the
  fast region; these stalls were 5 to 10 s.
- **`twoport.sh` judges only what fio counted**, after the warm-up, so
  `A-1`'s drop at 9 to 16 s read "no drive stepped down".

The two questions are whether the drives still give back their fast
region after a trim and a rest, and where the stalls start. Steps 3 and
4 answer them, before the copy runs of step 5.

## 1. The first ten minutes

As in "The boot into the kernel built from the tip" further down, items
1 to 6, with these changes:
- `uname -r` is the new build, not `…hs.503…`, and
  `cat /sys/module/sunrpc/srcversion` equals `modinfo -F srcversion sunrpc`.
- **xeu comes with the build** (item 1 of "tardis1: copy mode on both
  ports" below): `rpm -qa | grep kmod-xsight` names the new kernel, and
  `modinfo -F version xeu` is 2.0.1-13. `/sys/module/xeu/parameters/` has
  `rx_gro_frags`, `rx_prefetch` and `irq_msix_mask`, and the srcversion
  is `CA0931345E46C929B11D01F` (round 45's source, as on 503). Only if
  the package is missing: `./build-kmod.sh`, `rpm -U`, and reload `xeu`
  before any region is bound, with no NFS client connected
  (`ss -Htn state established '( sport = :2049 )'`).
- `modprobe arm_cmn` if it is not loaded.
- Mount the drives:
  `for n in 0 1 2 3 4 5 6 7; do mount -o noatime /dev/nvme${n}n1 /hs/nvme$n; done`
- `./bringup.sh 32768 256`, `./port-setup.sh enp164s0f0 56`, then
  `./settled.sh apply`, which must end in "the settled setting".

## 2. The per-device mapping, then KUnit

Items 3 to 5 of "tardis1: copy mode on both ports" below:
- **The mapping is there.** `cat /sys/kernel/debug/page_pool_rx_region`
  shows `free_unmapped` and two `mapped <pci> dir <d> pools 4 free <n>`
  lines, one for `0000:a1:00.0` and one for `0000:a4:00.0`. If there is
  no `mapped` line, the kernel lacks `0c375e06bf64`: stop and tell Mike.
- **A slot's life.** With no client connected:
  - `ip link set enp164s0f0 down`: its line goes, `free_unmapped` grows,
    `escaped` stays 0, dmesg has no WARNING.
  - `ip link set enp164s0f0 up`: the line comes back.
  - Then the same for `enp161s0f0`, then `./settled.sh apply`.
- **KUnit** with nfs-server stopped (`../run-kunit.sh`). On 503 the
  counts were 7, 52, 7 and 10. Record the counts; there must be no
  failure and no skip.

## 3. The drives on their own (local, no NFS)

This answers whether the drives still give back their fast region after
a trim and a rest, as 12 October found. It runs with no export and no
client.

**Change first: `drvchar.py` gains `PAUSE=<second>:<seconds>`.** At
that second of the clock it stops every drive's fio (SIGSTOP), and
starts them again (SIGCONT) after the length given. That is a stall
like 503's, on every drive at once. Its sampling and summary are as
before. This is a docs commit once it has run.

**Three probes**, all eight drives at 7.5 GB/s each (what a two-port
run writes to them) with an fsync every 8 MiB, `STOP=0`, 75 s
(about 560 GB a drive, inside the fast region). Each probe starts with
an fstrim of all eight, 120 s of rest, then an fstrim of each drive
just before, as `twoport.sh` does:

| probe | command | expected, if the drives are as on 12 October |
|---|---|---|
| D1, no pause | `STOP=0 ./drvchar.py $L/t1-<build>/drv-D1 75 0:7.5:fsync=8 1:7.5:fsync=8 … 7:7.5:fsync=8` | 7.5 GB/s throughout, no step down |
| D2, an 8 s stall at 12 s (about 90 GB in) | `PAUSE=12:8` with the same line | 7.5 again after the stall, no step down |
| D3, as D2 with no fstrim just before (only the first, then 150 s of rest) | `PAUSE=12:8` | as D2 |

- If D1 or D2 steps down, the drives have changed since 12 October. The
  rule for a run has to change first; tell Mike before step 5.
- If D2 steps down and D3 does not, the fstrim just before the load is
  to blame. `twoport.sh` then drops its fstrim, or rests after it.
- If all three hold, the drives are not the problem, and the stalls of
  step 4 are.

The drives' cache must read `write through` (the audit holds it).

## 4. Where a stall starts: the rig's two changes

Both are `twoport.sh` changes, committed as docs once a run has shown
them working (the first run of step 5 does).

1. **Per second, beside `drives-sec.tsv`, what the drives cannot show.**
   For the same seconds, from the same moment:
   - `wire-sec.tsv` on the server, from a new `wiresample.py`: each
     port's receive GB/s (`/sys/class/net/<dev>/statistics/rx_bytes`)
     and the deltas of `TcpExt` `TCPOFOQueue`, `TCPRcvQDrop`,
     `PruneCalled`, `TCPBacklogDrop`, `TCPZeroWindowDrop` and
     `TCPToZeroWindowAdv` (`/proc/net/netstat`).
   - `client-sec.tsv` from a loop on the client over the management
     link: `Tcp` `RetransSegs` and `TcpExt` `TCPTimeouts` and
     `TCPLossProbes` each second, with its epoch time, so that it can
     be lined up.
2. **The summary tells a stall from a drive.** Over every sampled
   second, the warm-up included, `drives.txt` gains:
   - `STALLED: <from> to <to> s, the drives down to <n> GB/s, the wire
     down to <n> GB/s, client retransmits <n>`, for each run of seconds
     in which all eight drives together wrote under half the run's
     median;
   - `low in the warm-up: …`, for a drive stepping down before fio
     counted, in the same terms as the existing line.

   The existing `STEPPED DOWN` line stays as it is, so runs compare with
   503's.

A stall in which the wire falls with the drives started before the
server's disks: then look at the client's retransmits and timeouts and
the server's receive counters. A stall in which the wire holds and the
drives fall started in the server, after the socket.

## 5. The copy runs: the stack at 64 with the mapping

The same command line as on 503 (`DEFER=0/0 RCVBUF=393216 ENTRIES=128`,
`WARM=15 RUNTIME=45`, four drives a port). Each run follows an fstrim
of all eight and 120 s of rest, and checks: the settled setting, the
region at 32768 pages, `admitted` unchanged and no new WARNING. Take
the scripts in `$L/t1-503/` (`run.sh`, `run-s64.sh`, `run-d2.sh`,
`chain.sh`) as the start, in `$L/t1-<build>/`.

**In turns, not in pairs**, so that a drift in the drives or the host
over the session does not fall on one setting (on 503 every run after
the first pair was decided by the drives):

    A-1  s64-1  A-2  s64-2      (DEFER 0/0; stack 0, then 64 with EXP_RX_REGION_STACK=64)
    d2-1  d2-2                  (DEFER=2/10000 on both ports, stack 0)

**A run counts only if it has no `STALLED` line and no step down.** A
run that does not count is run once more at the end of its round. If
the repeat does not count either, stop and look at its per-second
files before going on.

**For each run, record:** each port's GB/s and the wire in the window;
WRITEs/s; `./cost.sh` (cycles a WRITE, ring and nfsd); the region's
peak and mean in use (every 0.2 s for 10 s); **each `mapped` slot's
`free` and `free_unmapped` after the run**; `alloc_fail` and
`escaped`; client retransmits; the step-down, stall and warm-up lines.

**What should show:**
- **At stack 0, no regression against 503:** about 62 GB/s on the wire
  in the window (61.91 and 61.94 on 503) at 58.5k WRITEs/s.
- **At stack 64, no cost now.** On 503, without the mapping, `s64-1`
  had 1% less on the wire and 3 to 7% more nfsd cycles a WRITE. With
  pages returned to their slot still mapped, both should be within
  noise of stack 0, with the region's mean in use lower (995 to 1087
  on 503, 1113 to 1279 at stack 0).
- **Deferral 2/10000 on both ports:** the rings' cycles a WRITE 7% lower
  (88.3k against 94 to 95k on 503's one run) at the same rate. If both
  runs count and show that, `settled.sh`'s `DEFER[b]` can become
  2/10000 (Mike's call).

## 6. Then

- Write the results up at the top of this README, and point `AGENTS.md`
  at them.
- Leave the host at the settled setting (`settled.sh apply`, stack 0,
  `EXP_RX_REGION_STACK` unset).
- What is still open afterwards: whatever steps 3 and 4 found about the
  stalls, `DEFER[b]`, and the regroup when Mike asks.

# tardis1 on build 503 (29 October, evening): copy on two ports, no regression

tardis1 booted `7.3.0-7.hs.503.loanpages_pool.el9.aarch64+16k`. **It is
not the kernel the section below is for.** It was built at 14:00 MDT
(20:00 UTC) on 29 October; `0c375e06bf64`, the region's per-device
mapping, was committed at 15:49 MDT, and the debugfs file has no `mapped`
and no `free_unmapped` line. It does
carry every other code commit since build 494: `rx_region_stack`,
`drc_max_entries`, the wake changes, nfsd's reused credentials. Mike
chose to run copy mode on it anyway. Items 2 and 4 of "tardis1: copy mode
on both ports" (the slots and a slot's life) could not be checked, and
still wait for a kernel built from `0c375e06bf64` or later.

Logs, the run scripts (`run.sh`, `run-s64.sh`, `run-d2.sh`, `chain.sh`)
and `results.tsv` are in `$L/t1-503/`.

**The first ten minutes:**
- sunrpc `666175BED7A343E376C1742`, the same from `modinfo`.
- nfsd `3F0C341E86E266D5FDF2ADF`.
- xeu `CA0931345E46C929B11D01F` (kmod-xsight 2.0.1-13, already
  installed for 503; no rebuild needed).
- `arm_cmn` loaded at boot. No NFS client connected.
- `./bringup.sh 32768 256` and `./port-setup.sh enp164s0f0 56`: region
  32768 pages on `0000:a1:00.0,0000:a4:00.0`, `alloc_fail` 0, `escaped` 0.
- `settled.sh apply` ended in "the settled setting".

**KUnit, from the package:** `sunrpc-xdr-bvec` 7, `sunrpc-svcsock-rx` **52**
(50 on 494), `nfsd-receive-bvec` 7, `nfsd4-receive-bvec` 10. No failure,
no skip.

**`twoport.sh`'s `DEFER=` did not hold (fixed).** Since `41194d60010d`
`settled.sh place` writes each port's settled deferral, and `twoport.sh`
ran the placement after setting the run's `DEFER`, so it put the settled
value back. The first `DEFER=0/0` run failed the two-port audit
(`enp161s0f0 NAPI deferral 2/10000 (want 0/0)`); `DEFER=2/10000` would
have failed on port b the same way. `67b042f0993e` sets the run's
deferral after the placement. It then held on both ports: `defer_a=0/0`
and `defer_b=2/10000` each passed the audit.

## The runs (18:16 to 18:41 MDT)

Copy mode on both ports: `RCVBUF=393216 ENTRIES=128`, `twoport.sh`'s
defaults (`WARM=15 RUNTIME=45`, four drives a port). Each run came after
an fstrim of all eight drives and 120 s of rest. The region's free pages
were sampled every 0.2 s for 10 s, from 10 s into the timed load.

| run | setting | total (fio) | a / b | wire in the window, a + b | WRITEs/s | cycles a WRITE, ring / nfsd | rings a / b, nfsd busy | region peak, mean in use (min to max) | retrans | drives |
|---|---|---|---|---|---|---|---|---|---|---|
| `two-copy-A-1` | DEFER 0/0, stack 0 (256) | 57.09 | 28.93 / 28.16 | 31.49 + 30.42 = 61.91 | 58.6k | 95.3k / 1030.6k | 25 / 23, 56% | 1280, 1279 (1264 to 1280) | 13 | none stepped down |
| `two-copy-A-2` | DEFER 0/0, stack 0 (256) | **61.32** | 31.15 / 30.17 | 31.49 + 30.45 = 61.94 | 58.5k | 94.2k / 988.0k | 23 / 23, 53% | 1168, 1113 (1104 to 1168) | 12 | none stepped down; nvme1 and nvme3 past the fast region |
| `two-copy-s64-1` | DEFER 0/0, stack 64 | 54.88 | 27.79 / 27.10 | 31.02 + 30.31 = 61.33 | 57.8k | 96.4k / 1062.4k | 24 / 24, 56% | 1193, 1087 (940 to 1164) | 22 | nvme4 and nvme7 at 21 s |
| `two-copy-s64-2` | DEFER 0/0, stack 64 | 40.87 | 20.72 / 20.16 | 18.72 + 18.94 = 37.66 | 34.7k | 95.4k / 932.3k | 11 / 11, 27% | 1198, 995 (939 to 1177) | 265 | seven of eight, from 19 s |
| `two-copy-d2-1` | DEFER 2/10000 on both, stack 0 | 48.52 | 23.84 / 24.69 | 29.02 + 29.18 = 58.20 | 55.5k | 88.3k / 957.5k | 17 / 15, 48% | 1280, 1152 (flat) | 38 | nvme0 at 23 s, nvme7 at 22 s, nvme5 and nvme6 at 48 to 51 s |
| build 494 `two-384k-1` | DEFER 0/0 | 58.47 | 29.78 / 28.69 | 30.06 + 29.03 = 59.09 | | | | | | none stepped down |
| build 494 `two-384k-2` | DEFER 0/0 | 57.89 | 29.14 / 28.74 | 29.58 + 29.16 = 58.74 | | | | | | nvme7 at 14 s |

In every run `admitted` stayed at 3096 and 2 (nothing loaned), with
`alloc_fail` 0, `escaped` 0 and 31616 pages free after it. No WARNING
appeared in dmesg. The second `DEFER=2/10000` run was not run: Mike
stopped the testing at about 18:40. The host was left at the settled setting
(stack 0, port a 2/10000, port b 0/0), with the drives mounted,
nfs-server running and the region bound.

**What they show:**
- **No regression against build 494; the wire is higher.** Both 0/0 runs
  carried about 62 GB/s on the wire in the window (61.91, 61.94), against
  494's 58.74 to 59.09, at the same 58.5k WRITEs a second in both.
  - `A-2`'s 61.32 is the highest fio total on two ports recorded on
    tardis1. Before it: 61.02 once with `fq` on the client, 60.41 at the
    setting.
  - Its twin `A-1` gave 57.09 with the same wire in the window, so the
    gap is outside the window, not the steady rate. Quote the wire, or
    more runs, not 61.32 alone.
  - 494 ran `WARM=10 RUNTIME=30`; these ran 15 and 45.
- **Stack 64 bought little region on two ports, and the drives decided
  both runs.**
  - The mean in use fell from 1113 to 1279 pages to 995 to 1087. On
    tardis2, one port, stack 64 took it from 1132 to 774 (round 45,
    run111).
  - In `s64-1`, the run the drives spoiled least, the wire was 1% lower
    and nfsd's cycles a WRITE 3 to 7% higher. That is the round trip
    through the region that 503 still pays without the per-device
    mapping.
  - `s64-2` collapsed in the window (37.66 GB/s on the wire, 265
    retransmits), with seven drives stepped down.
- **Deferral 2/10000 on both ports, one run:** the rings' cycles a
  WRITE were 7% lower (88.3k against 94 to 95k), and the rings were 15
  to 17% busy against 23 to 25%, as round 44 found on port a. The rate
  is not a measurement: four drives stepped down.
- **Drives stepped down in the three runs after the 0/0 pair, writing
  less than the pair did.** They wrote 437 to 563 GB each, where the 0/0
  runs wrote 499 to 601 with none stepping down. The retransmits rose
  with them (22, 265, 38 against 12 to 13). One possible reading: stalls
  on the receive side pause the writes, and a pause of 0.3 s steps a
  drive down ("Keep a run inside the drives' fast region",
  `AGENTS.md`). That is not shown. It may also be the drives' state
  after a series of runs, since every one of these came after the 0/0
  pair.

**Still open:**
- A kernel built from `0c375e06bf64` or later, for items 2 to 4 of the
  section below and stack 64 with the per-device mapping.
- The second `DEFER=2/10000` run, and a pair whose drives hold, before
  `settled.sh` moves port b to 2/10000.
- Why the drives stepped down from the third run on.

# 29 October: the kernel with the region's per-device mapping — tardis1 copy on two ports, tardis2 loan on one

Mike pushed the tip on 29 October evening (`effa3133c00a`, its last code
commit `0c375e06bf64`) and started a GitLab build from it. That kernel
goes on both servers:

- **tardis1:** copy mode on both ports. It is the first run of the
  region's per-device mapping with two devices sharing the region, and
  of everything FLAMEGRAPH kept since build 494.
- **tardis2:** loan mode on port a, FLAMEGRAPH round 45's step 4.

Nothing in this section has run yet.

**What the kernel carries that has never run anywhere:**
`0c375e06bf64` "page_pool: keep a fixed receive region page mapped for its
device". The account is [`../FLAMEGRAPH/45-region-holders/`](../FLAMEGRAPH/45-region-holders/README.md),
steps 3 and 4.

- **Why:** a region page that went back to the region was unmapped,
  its DMA index erased from the pool's `dma_mapped` xarray, and mapped
  and indexed again by the next ring to take it. That cost about 0.74k
  nfsd and 0.58k ring cycles a page, most of it the xarray, whose nodes
  bounce between nfsd's 48 CPUs and the ring's. It is why a shallow
  pool stack (`rx_region_stack` 64, a third less of the region in use)
  cost loan mode 3 to 3.5% (round 45, run111 and run112).
- **What it does:**
  - Each device that maps region pages (per DMA direction, up to 8) gets a
    slot: a free list of pages still mapped for it. The region keeps an
    owner byte per page.
  - A pool returns its pages to its slot's list still mapped. It takes
    pages in this order:
    1. its own slot's list, with a sync and nothing else;
    2. the unmapped pages, mapped with no xarray index;
    3. another device's list, each page unmapped for that device under
       the region's lock.
  - When a device's last pool is destroyed (port down, xeu unload),
    `page_pool_scrub()` has the region unmap every page still mapped
    for that device, free or still out, and the slot gives up its
    device reference.
  - A pool that does not map, or finds every slot taken, keeps the old
    round trip.
- **debugfs:** `/sys/kernel/debug/page_pool_rx_region` gains
  `free_unmapped` and one `mapped <pci> dir <d> pools <n> free <n>`
  line per slot. That line is the first proof the kernel has it.
- **Built:** with W=1 on `net/core/` (build 499's `.config`), and
  checkpatch `--strict` clean. sparse is not on tardis2, so it has not
  been run.

**Also new on tardis1 since build 494:** every code commit FLAMEGRAPH
kept on tardis2 (`git log --oneline 77f82c816493..0c375e06bf64 -- . ':!NFSD_TCP_WRITE_ZEROCOPY' ':!AGENTS.md'`).

- **Those copy mode runs through:**
  - the wake changes (`2fc9cd2e839b`, `eff5f16d4f30`, `0f0ed4159a12`,
    `6c6dc284fed0`);
  - nfsd's reused credentials (`916c11a2d1fc`) and its reply cache size
    (`635e44bf37bf`);
  - page_pool's stack depth (`dc23eac70f34`), `rx_region`'s own line
    (`085734569d9d`), and the per-device mapping.
- **What tardis2 measured but tardis1 has not run**, all held by
  `settled.sh`:
  - `drc_max_entries` 4096;
  - `skb_defer_max` 0;
  - xeu's `rx_prefetch` Y, `rx_gro_frags` Y and `irq_msix_mask` Y;
  - **NAPI's deferral, 2/10000 on port a and 0/0 on port b**
    (FLAMEGRAPH round 44, one port).

  `rodata=noalias` is held on tardis2 only.

## tardis1: copy mode on both ports

**What to find out:**
1. **No regression.** The two-port copy runs are to match build 494's
   58.47 and 57.89 GB/s (`$L/t1-tipkernel/`, `two-384k-1` and `-2`).
2. **The per-device mapping with two devices.** There are two slots,
   each port's pages are mapped for its own function, and each slot
   goes and comes back cleanly with its port.
3. **The region's working set in copy mode on two ports**, at the
   settled stack (0, the ring's 256) against 64. On tardis2, one port,
   64 took copy mode's mean in use from 1132 to 774 pages with no cost
   (round 45, run111).

**The first ten minutes**, after "The boot into the kernel built from the
tip" further down, items 1 to 6 (the kernel, its own sunrpc, the
packages, `arm_cmn`, the drives, the bring-up):

1. **xeu comes with the build.** The GitLab build makes kmod-xsight
   2.0.1-13 (kernel-rpmtree `b3cd3d5`) beside the kernel; install it
   with the kernel, so no `./build-kmod.sh`. Check that the loaded xeu
   is that one: `modinfo -F version xeu`, and `rx_gro_frags`,
   `rx_prefetch` and `irq_msix_mask` in
   `/sys/module/xeu/parameters/`, which `settled.sh` wants. Then, with
   no NFS client connected (`ss -Htn state established '( sport = :2049 )'`),
   `./bringup.sh 32768 256` and `./port-setup.sh enp164s0f0 56`.
2. **`./settled.sh apply`** must end in "the settled setting". The
   receive region devices are both ports' PCI functions.
3. **The region:** `cat /sys/kernel/debug/page_pool_rx_region`.
   - Two `mapped` lines, one per port, each with `pools 4`.
   - `escaped 0`, `alloc_fail 0`.
   - No `mapped` line at all means the kernel does not have
     `0c375e06bf64`: stop there.
4. **A slot's life**, with no client connected:
   - `ip link set enp164s0f0 down`: its `mapped` line goes,
     `free_unmapped` takes its pages back, `escaped` stays 0, and dmesg
     has no WARNING.
   - `ip link set enp164s0f0 up`: the line comes back.
   - Then the same for port a.
   - Then `./settled.sh apply` again, since an open undoes part of it.
5. **KUnit** from the package (`../run-kunit.sh`). On 494 the counts were
   7, 50, 7 and 10, and FLAMEGRAPH added cases since, so record the new
   counts. No failure and no skip.

**The runs**, each after an fstrim of all eight drives and 120 s of rest,
with `twoport.sh`'s defaults (`WARM=15 RUNTIME=45`, four drives a port).
No `WRITES` means budget 0, which is copy mode. Read `drives.txt`
before believing a figure.

1. **As build 494 ran, both ports at deferral 0/0** (`DEFER` sets both
   for the run), twice:

       DEFER=0/0 RCVBUF=393216 ENTRIES=128 ./twoport.sh $L/t1-<build>/two-copy-A-1 ab

2. **The stack at 64**, twice. `export EXP_RX_REGION_STACK=64; ./settled.sh
   apply` reopens both ports for it. Keep it exported through the runs,
   since twoport.sh's audit reads it; the twoport.sh line is the same.
   Then `unset EXP_RX_REGION_STACK; ./settled.sh apply`.
3. **Optional: both ports at deferral 2/10000** (`DEFER=2/10000`), at
   the settled stack, twice. It has only been measured on one port, and
   `settled.sh` holds port b at 0/0 until a run like this one shows the
   same.

**For each run, record:**
- each port's GB/s, and the wire in the window;
- the region's peak (`summary.txt`, "region pages in use at most");
- the mean in use: the debugfs `free` every 0.2 s for 10 s, as
  `../FLAMEGRAPH/45-region-holders/run111.sh` does;
- each slot's `free` after the run;
- `./cost.sh RUNDIR` for the rings' and nfsd's cycles a WRITE;
- `admitted`, which must not move in copy mode.

## tardis2: loan mode on port a (FLAMEGRAPH round 45, step 4)

**After the boot:**
- `rodata=noalias` is in `/etc/kernel/cmdline`, so it carries over.
- The drives, `nfs-server` and `./bringup.sh 32768 256`, as in "tardis2
  on 27 October" below.
- The kernel's own sunrpc and nfsd are now the tip's, so `updates/` is
  empty for the new kernel.
- xeu: the build's kmod-xsight 2.0.1-13, installed with the kernel.
  Round 45's kept xeu, `CA0931345E46C929B11D01F`, is the same source,
  so check that its srcversion matches.
- Then `./settled.sh apply` and the region checks of tardis1's items 3
  and 4, with the one `mapped 0000:a1:00.0` line.

**Then:**
1. Update the srcversions that `../FLAMEGRAPH/45-region-holders/run111.sh`
   and `run112.sh` pin (`XK`, `N`, `K`).
2. Run run111 (stack 256 against 64, loan64 and copy) and run112 (the
   profile).

**What should show:**
- B's +3.9k ring and +10.5k nfsd cycles a WRITE should fall to about
  the region's lock, a few hundred cycles a page.
- A itself should come in under run111's A (130.2k and 296.4k), since
  it too sent about 3 pages a WRITE round.
- In run112's profile, `__xa_cmpxchg` and `page_pool_register_dma_index`
  should be gone from the round trip.

# tardis2 on 27 October: build 499 with `rodata=noalias` — read this first

Since 23 October the work on tardis2 is the loan's CPU cost a WRITE,
a round at a time, in [`../FLAMEGRAPH/`](../FLAMEGRAPH/README.md): its
README has the rules (loan mode only, no copy runs) and every round's
outcome. This section is where tardis2 stands after round 30.

**What tardis2 runs** (the current state, which round 30's B measured):
- **The kernel:** build 499 (`7.3.0-7.hs.499.el10.aarch64+16k`), booted
  with `rodata=noalias`. `/etc/kernel/cmdline` carries it into each new
  kernel's boot entry, and `settled.sh` audits a boot without it BAD.
- **The modules** (since 28 October): the tip's sunrpc in `updates/`
  (FLAMEGRAPH round 36's build, `E80412A536ADD1E86B16F5A`), the
  kernel's own nfsd (`3F0C341E86E266D5FDF2ADF`), and xeu in `updates/`
  (round 35's, `63631721F8B149913C0DC19`, kmod-xsight 2.0.1-12). The
  initrd carries this sunrpc and xeu, so a boot loads them.
- **The settled setting:** `settled.sh` holds `rx_prefetch` Y,
  `irq_msix_mask` Y, `svc_tcp_rx_want_eighths` 6 and `drc_max_entries`
  4096. The server port's qdisc stays `fq_codel` under `mq` (round 31's
  `noqueue` was not kept).

**After a boot:** `for n in 0 1 2 3 4 5 6 7; do mount -o noatime
/dev/nvme${n}n1 /hs/nvme$n; done`, `systemctl start nfs-server`,
`./bringup.sh 32768 256`, then `./settled.sh apply`, which must end in
"the settled setting". Check that no NFS client is connected first.
The loan runs use port a onto the four DPRP5s (`/hs/nvme4` to `7`).

**Round 30's result** ([`../FLAMEGRAPH/30-tlb-look/`](../FLAMEGRAPH/30-tlb-look/README.md)):
- **What it found:** `rodata=full`, arm64's default, maps the linear map
  in 16 KiB pages on a CPU without BBML3, and the Neoverse N2 has none.
  `rodata=noalias` maps it in 32 MiB blocks.
- **The A/B:** across a reboot, both sides on the tip's sunrpc, four
  untraced loan64 runs a side and one with `perf stat` on the ring CPUs
  and on nfsd's (`../FLAMEGRAPH/30-tlb-look/run64.sh`; the runs are
  `$L/t2-r64-full/` and `$L/t2-r64-noalias/`).
- **What a loaned WRITE costs:**

  | per loaned WRITE | `rodata=full` | `rodata=noalias` | B − A |
  |---|---|---|---|
  | nfsd, µs | 100.50 ± 2.56 | 91.67 ± 1.43 | −8.83 ± 1.47 (−8.8%) |
  | rings, µs | 47.55 ± 0.56 | 44.60 ± 0.99 | −2.95 ± 0.57 (−6.2%) |
  | page-table walks, nfsd's CPUs · rings | 108 · 61 | 2.1 · 0.29 | |

- **Unchanged:** the rate (40.4 to 40.5 GB/s, drive-limited), the
  region's peak and latency at iodepth 1 (p50 0.420 ms).
- **Kept (Mike, 27 October).** It costs the read-only protection of the
  kernel's aliases in the linear map, a security trade-off still to be
  weighed for Hammerspace's product. tardis1 does not have it.
- **Measuring:** `cost.sh RUNDIR...` gives the ring and nfsd µs a WRITE
  of `twoport.sh` runs.

**Where it stands:** a loaned WRITE costs the rings 44.5 µs and nfsd
90.3, against 92.6 and 184.1 when the FLAMEGRAPH rounds began (the
settled side of round 32, four runs). Round 32, more transmit queues,
was not kept. The next rounds are FLAMEGRAPH README's "The queue".

# Build 494 on tardis2: no regression on the DPRP5s (23 October)

tardis2 booted `7.3.0-7.hs.494.loanpage_pool.el10.aarch64+16k`, the same
tree as tardis1's build 494 below. Logs, the driver (`run1.sh`) and
`results.tsv` are in `$L/t2-tipkernel/` on tardis2.

**The first checks passed:** the kernel's own sunrpc (srcversion
`C78A9E1FDAC3084874BA63F`, as on tardis1), nothing in `updates/`,
`page_pool_rx_region_key` and `svc_tcp_rx_loans` in kallsyms, the 494
`modules-internal` and `devel` packages, `arm_cmn` loaded at boot. No
NFS client was connected. The drives as on 21 October (nvme0 to nvme3
the WD_BLACKs, nvme4 to nvme7 the DPRP5s). `bringup.sh 32768 256`:
`alloc_fail` 0. `settled.sh apply` ended in "the settled setting" before
and after the suites and the runs.

- **tardis2's `/etc/modprobe.d/sunrpc.conf` no longer sets
  `svc_tcp_rx_loan_pages=Y`.** The boot logged "unknown parameter
  ... ignored". The file before is `/root/sunrpc.conf.pre-494-2026-10-23`.
- **Another user's VM runs off `/hs/nvme0`** (qemu-kvm with an
  `ofe-6.0.0` image from `hke.git`, started about 9 minutes after boot).
  It was left alone, and the runs used only the DPRP5s. It sat idle during them.
  Because of it, nvme0 has two mounts (the boot's, `relatime`, and
  `noatime` on top).

**KUnit, from the package:** `sunrpc-xdr-bvec` 7, `sunrpc-svcsock-rx`
50, `nfsd-receive-bvec` 7, `nfsd4-receive-bvec` 10. No failure, no
skip. (`run-kunit.sh` put the server back stopped, as it was found, so
its last check failed. The fix was `systemctl start nfs-server` and
`settled.sh apply`.)

**Copy and loan at 64 WRITEs, two rounds** (port a onto the four
DPRP5s, `WARM=10 RUNTIME=30`, each run after an fstrim and 120 s of
rest, `memcost-log.sh` beside each, 750 KiB, every window about
349 KiB), against 491's `run17.sh` and `run19.sh` at 750 KiB, a WRITE
at a time:

| run | GB/s | woken | context switches | nfsd CPUs busy | FIFO drops, ring drops, client retransmits | bytes through memory a byte | 491 at 750 KiB |
|---|---|---|---|---|---|---|---|
| copy-1 | 40.81 | 9.91 | 12.78 | 25% | 0, 0, 5 | 2.82 | 40.80 to 40.81, woken 10.06 to 10.52, 26 to 27%, 2.99 |
| copy-2 | 40.81 | 11.38 | 14.18 | 27% | 0, 0, 2 | 2.91 | |
| loan64-1 | 40.48 | 14.39 | 15.75 | 13% | 0, 0, 6 | 1.92 | 40.30 to 40.41, woken 14.32 to 15.90, 13 to 14%, 1.89 |
| loan64-2 | 40.08 | 18.08 | 18.26 | 15% | 0, 0, 11 | 1.84 | |

- **No regression.** Every run is the drives' (10.0 to 10.2 GB/s each,
  99 to 100% busy, none stepped down), and every WRITE went direct.
- **Copy mode loaned nothing:** both budgets' `admitted` stayed at the
  suites' 2 and 3096 across each copy run.
- **The loan path admits:** 1,542,640 and 1,528,448 records with 0
  overrun. The region in use was at most 6,497 and 6,513 pages.
- **Loan runs spread from run to run in wakes a WRITE** (`run2.sh` to
  `run4.sh`). loan64-1 and loan64-2 are the two ends of the spread:

  | run | GB/s | refused a loaned WRITE | woken a WRITE | rings of a busy | unread on a socket (`ss`, mean) |
  |---|---|---|---|---|---|
  | loan64-1, the first after the load | 40.48 | 0.96 | 14.39 | 41% | 188 KiB |
  | loan64-2 | 40.08 | 0.17 | 18.08 | 57% | 29 KiB |
  | loan64-3 | 40.03 | 0.15 | 18.27 | 53% | 34 KiB |
  | loan64-4 | 40.15 | 0.20 | 17.81 | 50% | 21 KiB |
  | loan56-1 (budget 7190) | 39.87 | 0.25 | 17.75 | 53% | 9 KiB |
  | loan64-5, the first after a fresh sunrpc load | 40.35 | 0.62 | 16.47 | 49% | 97 KiB |
  | loan64-6 | 40.34 | 0.56 | 16.59 | 51% | 55 KiB |
  | 491, 750 KiB (three runs) | 40.30 to 40.41 | 0.69 to 0.97 | 14.32 to 15.90 | 40 to 42% | |

  loan60-1 is not valid (15,475 ring drops, 16,029 client retransmits).
  Where records wait for credit, sockets stay deep and a
  record takes few visits. Where they do not, sockets drain, records
  take more visits, and wakes follow visits (21 October, "The receive
  window"). The ring CPUs carry the extra wakes, since `svc_data_ready()`
  wakes from softirq. What was ruled out:
  - **Another user's podman build** (`bootc-image-builder`, 03:54:14
    to 03:58:35) ran beside loan64-2. loan64-3 and loan64-4 ran with no
    load beside them and look the same.
  - **The budget binding at 64 WRITEs**: at 56 sockets stay shallow and
    refusals low.
  - **The code since 491's sunrpc** (`f84c0ee96abf`, `fb352b960445`,
    `428a1051e6cb`): reading the diffs, none changes admission, the
    charge or the learning while a budget is above 0.
  - **The first loan run after a load** (`run4.sh`: `SUNRPC_KO=none
    ./swap-sunrpc.sh`, every counter and factor at 0, then loan64
    twice): loan64-5 and loan64-6 match each other and sit between
    loan64-1 and loan64-2. A run's place in the spread does not follow
    from what the budget has learned. Both learned the same factors,
    `cost_factor` 182 to 194 (sampled each second, `<tag>-bstats.txt`;
    it moves between 168 and 205 within a run) and `hold_factor` 107.
  - **The flows' placement**: every run got 4 flows on each ring
    (`place-a.txt`), and the expected shares do not follow the wakes
    (loan64-3, at 26/26/24/24%, woke 18.3; 491's `mb-loan64-750k`, at
    25/26/25/24%, woke 15.9).

- **What the spread follows: how busy the four ring CPUs are** (0, 2,
  4 and 6, the mean of their softirq in twoport.sh's window). Every loan
  run of both builds is on one curve, and so is the copy:

  | run | rings' softirq | woken a WRITE | refused a loaned WRITE | GB/s |
  |---|---|---|---|---|
  | copy-1, copy-2 (494) | 66%, 72% | 9.91, 11.38 | - | 40.81 |
  | rb-loan64-1m-1, -2 (491, 1 MiB) | 71% | 10.48 to 10.80 | 1.08 | 40.42 to 40.48 |
  | rb-loan64-750k-2 (491) | 76% | 14.32 | 0.97 | 40.41 |
  | loan64-1 (494) | 76% | 14.39 | 0.96 | 40.48 |
  | mb-loan64-750k, rb-loan64-750k-1 (491) | 79% | 15.33 to 15.90 | 0.69 to 0.83 | 40.30 to 40.39 |
  | loan64-5, -6 (494) | 89 to 90% | 16.47 to 16.59 | 0.56 to 0.62 | 40.34 to 40.35 |
  | loan56-1, loan64-2 to -4 (494) | 92 to 94% | 17.75 to 18.27 | 0.15 to 0.25 | 39.87 to 40.15 |
  | tr-loan64-1 to -5 (494, `pair.bt` beside) | 97 to 98% | 17.83 to 18.19 | 0.03 | 38.46 to 38.64 |

  **494 is not off 491's curve**: loan64-1 sits among 491's runs. 491's
  three runs at 750 KiB happened to land low, and 494's eight landed
  across the curve. Why a run lands where it does is open.

- **The traced pair could not be had** (`run5.sh`, `pair.bt`: groups.bt
  and stacks.bt in one). The probes run in the rings' softirq and put
  every run at the saturated end: five runs at 17.8 to 18.2 wakes, none
  at the low end, at 38.5 GB/s with the drives at 96% (no longer quite
  the drives'). What the saturated end does, from `tr-loan64-1-pair.txt`,
  a WRITE at a time: 19.0 skbs queued, `svc_data_ready()` 19.2, threads
  woken 17.96, visits 14.82 of which 13.69 short, credit waits 0.03.
  Nearly every skb wakes a thread, which takes what has come and gives
  the transport back, and the sockets hold mostly 64 to 128 KiB when
  data arrives. At the low end more records wait for credit. A transport
  that waits for credit is not woken for data, so its socket fills
  (188 KiB in loan64-1) and its record is read in fewer visits.
  The two feed each other. A wake is paid in the rings' softirq: loan64-3
  over loan64-1 has some 150,000 more wakes a second for 18 more points
  of four CPUs, about 5 us a wake. Busier rings leave shallower sockets
  and so more wakes.

- **The lighter trace leaves the rings where an untraced run has them,
  and still gives no pair** (`run6.sh`, `light.bt`, both in
  `$L/t2-tipkernel/`). Every probe fires in an nfsd thread: the visit's
  entry, the visit and short-read tracepoints, the credit's admissions
  and waits, and nfsd's WRITEs. Wakes come from `pool_stats` as in an
  untraced run, and no probe fired on CPUs 0 to 7. Six runs landed in
  the middle of the curve, where loan64-5 and -6 are, none at either
  end:

  | run | rings' softirq | woken a WRITE | refused a loaned WRITE | credit waits a WRITE | visits a WRITE | GB/s |
  |---|---|---|---|---|---|---|
  | lt-loan64-1 | 89.6% | 16.00 | 0.48 | 0.47 | 13.40 | 40.29 |
  | lt-loan64-4 | 89.5% | 16.03 | 0.44 | 0.36 | 13.66 | 40.17 |
  | lt-loan64-3 | 89.6% | 16.20 | 0.43 | 0.38 | 13.57 | 40.28 |
  | lt-loan64-6 | 87.9% | 16.27 | 0.27 | 0.24 | 13.72 | 40.19 |
  | lt-loan64-5 | 88.9% | 16.60 | 0.26 | 0.25 | 13.97 | 40.19 |
  | lt-loan64-2 | 90.6% | 17.08 | 0.09 | 0.09 | 14.29 | 39.90 |

  **Within this band, wakes follow the credit, not the rings.** The six
  runs' softirq lies within 3 points and does not order them, but credit
  waits do, without exception. Fewer waits give more visits, as the
  bullet above has it. lt-loan64-2, with almost no waits, is nearly at
  the saturated end's wakes at 90.6%. So the rings may be where the cost
  shows rather than what sets it. What sets how often a run waits for
  credit is still open.

- **The empty reads `light.bt` showed were the probe's, not nfsd's.**
  `light.bt` put a kprobe and a kretprobe on `svc_tcp_recvfrom()`. Its
  entry fired about twice per call (56.5M against 28.2M
  `svcsock_tcp_recv`), and the extra half read `sk_rmem_alloc` as 0,
  most likely a failed read. The kretprobe recorded nothing, here and
  in `pair.bt`. `probe.bt` (`run7.sh`, pr-loan64-1, 15 s, 40.21 GB/s,
  rings at 88.2%) counts the function with a kprobe, an fentry and an
  fexit together, and all three agree with the tracepoint: 18,435,645
  calls, every one from `svc_recv()`. Per WRITE:

  | measure | per WRITE |
  |---|---|
  | calls (visits) | 13.88 |
  | that return with no record (short) | 12.76 |
  | that find the socket empty at entry | 0.57 (4.1% of calls) |
  | threads woken (`pool_stats`, twoport.sh's window) | 15.63 |

  A visit that finds nothing is rare. What the wakes buy is the short
  visits: a thread takes the 32 to 64 KiB that has come and gives the
  transport back. That is 12 to 13 handoffs for a 1 MiB record. About
  1.7 wakes a WRITE more than calls suggests some woken threads find no
  transport at all, but the wakes are counted over twoport.sh's window
  and the calls over the whole trace, so that gap is an estimate. To
  trace a visit's length, use an fexit, not a kretprobe.

- **Threads that find no connection, and what a counted wake is**
  (`run8.sh` with `probe2.bt`, `run9.sh` with `probe3.bt`, one loan run
  each at 39.96 and 40.16 GB/s). Every probe fires in an nfsd thread,
  except a `sched_switch` filtered to nfsd going to sleep inside
  `svc_thread_wait_for_work()`. `pool_stats` and nfsd's WRITEs are read
  when the trace attaches and before it stops, so all of it is over one
  span. The WRITEs from `/proc/net/rpc/nfsd` match the trace's exactly.
  From p3-loan64-1, per WRITE:

  | measure | per WRITE |
  |---|---|
  | threads woken (`pool_stats`) | 17.81 |
  | transports enqueued (`pool_stats`) = dequeued (tracepoint) | 14.43 |
  | passes through `svc_recv()` | 17.61 |
  | ... that slept first | 4.60 |
  | ... that did not sleep | 13.01 |
  | times nfsd went to sleep (`sched_switch`) | 6.04 |
  | passes that found no transport | 3.18 (18% of passes) |
  | ... without sleeping | 2.18 |
  | ... after sleeping | 1.01 |

  `probe2.bt` gave the same with a 2 us wait standing in for a sleep:
  14.12 dequeued, 17.99 passes, 3.87 (21.5%) empty, 2.76 of them
  without sleeping. Every dequeued transport was read: 59 of
  26,948,604 found no slot.

  - **A counted wake is often not a sleep ended.** `threads-woken` is
    3 times the sleeps the wakee probes saw, and twice nfsd's voluntary
    context switches. Why is the next bullet: about half the counted
    wakes are of a thread whose wake is already in flight. (Written
    first as wakes landing on threads before they sleep; that is only
    0.9 a WRITE.)
  - **Most threads that find nothing never slept.**
    `svc_thread_should_sleep()` saw a transport on the pool's queue,
    so the thread skipped sleeping, and another thread dequeued it
    first: 2.18 a WRITE. Only 1.01 a WRITE (one sleep in six) was a
    thread woken from sleep for a transport that another had taken.
    Either costs about a microsecond of nfsd's time, not the rings'.
  - **A transport waits little in the queue**: under 8 us from enqueue to
    dequeue for 96% (`qtime_us`).

  So the wakes that find no connection are not where the rings' time
  goes.

- **What a wake costs the rings, and why half the counted wakes do
  nothing** (`run10.sh`, `run11.sh`, `run13.sh`, `run14.sh`;
  `probe4.bt`, `probe5.bt`, `probe7.bt`). `svc_pool_wake_idle_thread()`
  is timed from entry to exit, by where it runs: the ring CPUs (0 to 7,
  the softirq of `svc_data_ready()`) or nfsd's (8 to 55). Each call is
  classed by the target's state at `wake_up_process()`:
  - off its runqueue and asleep: a sleep ended;
  - off its runqueue in `TASK_WAKING`: another waker's wake is in
    flight;
  - still on its runqueue;
  - no call: the first idle thread was running, or there was none.

  These probes run on the rings and push a run to the saturated end
  (rings at 96 to 98%, 36.8 to 37.3 GB/s), so the times are an upper
  bound. Two runs without them anchor the counts:
  - an untraced run, cs-loan64-1 (40.21 GB/s, rings at 90.4%): 17.64
    counted wakes a WRITE, 8.35 voluntary context switches of nfsd's
    threads (from `/proc/<pid>/status`);
  - the light run p6-loan64-1, whose only probe on the rings is one
    count (39.99 GB/s, rings at 93.3%).

  From p7-loan64-1, per WRITE (1,746,392 WRITEs; the three classes that
  wake add up to `pool_stats`' 32,136,582, 18.40 a WRITE):

  | the wake | from the rings | from nfsd | time a call, rings / nfsd |
  |---|---|---|---|
  | ends a sleep | 3.50 | 4.52 | 1.64 / 1.96 us |
  | of a thread already being woken | 3.18 | 6.30 | 1.18 / 1.35 us |
  | of a thread still on its runqueue | 0.42 | 0.48 | 1.72 / 1.94 us |
  | no wake made (counted by neither) | 1.80 | 3.31 | 0.67 / 0.76 us |

  - **The wakes that end a sleep match the sleeps:** 14.00M of them
    against 14,524,666 voluntary context switches of nfsd's threads
    over the same span. It is 8.0 a WRITE here, and 8.22 to 8.35 in
    p5-loan64-2 and the untraced run, at rings from 90 to 97%. How
    often nfsd sleeps does not move with the curve.
  - **9.5 counted wakes a WRITE are duplicates.** `try_to_wake_up()`
    puts a sleeping thread in `TASK_WAKING` while the wake goes to the
    thread's CPU. `svc_pool_wake_idle_thread()` takes "not running" as
    idle. The woken thread stays first on `sp_idle_threads` until it
    runs and takes itself off. So every enqueue and every
    `svc_thread_wake_next()` in that window picks the same thread,
    calls `wake_up_process()` on it and counts a wake. Each costs a
    `pi_lock` on a task that another CPU is waking, and wakes nobody.
    Two thirds of the duplicates come from nfsd's own
    `svc_thread_wake_next()` and `svc_xprt_received()`, a third from
    the rings. The untraced run's 17.64 counted against 8.35 switches
    says it is as large there.
  - **Only half of all wakes come from the rings:** 8.9 counted a
    WRITE, of which 3.5 end a sleep.
  - **What the rings pay:** 20.0 s of the rings' time over the trace,
    11.4 us a WRITE, against the 109.7 us a WRITE the four ring CPUs
    spend in softirq (96.2% of four CPUs at 35,065 WRITEs a second).
    That is 10% with the probes' cost in it. The "no wake" class,
    0.67 us for a function that reads one pointer, is mostly the probe
    pair, and taking it off each call leaves about 4%, of which the
    duplicates are a quarter. **So wakes are not where most of the
    rings' time goes, and the "about 5 us a wake" in the bullet above
    does not hold.** That figure divided the extra ring time between
    runs by counted wakes. A ring wake that ends a sleep costs under 1
    us net, and what makes the rings busier at the high end is the
    rest of the softirq.

  The duplicates are nfsd's to fix, not the loan's. **Fixed at the tip
  in `2fc9cd2e839b` "SUNRPC: Wake an idle service thread only once"
  (Mike asked for it).** An `RQ_WAKING` bit, taken by
  `test_and_set_bit()` before `wake_up_process()`, lets one caller wake
  the thread. The others treat it as running. The thread clears the bit
  when it goes idle and each time it goes back to sleep still on the
  list. The bit is last in the `RQ_` enum, so nfsd's values do not move
  and the fix goes in with sunrpc.ko alone. `build-sunrpc.sh` builds it
  for build 494, clean, with no export changed.

  **It has run** (Mike asked for the reload). `swap-sunrpc.sh` put it
  in place (srcversion `82ACAC76B5F85A2645B58A4`, the log in
  `$L/t2-tipkernel/reload2.txt`) and started every service again by
  itself, the first real test of its fix. The four suites pass, 7, 50,
  7 and 10, with no failure and no skip (`kunit2.txt`). Its own final
  check failed only because nfs-server had been stopped for it, as on
  the first run that day; `settled.sh` is at the settled setting.
  `run15.sh`, fx-loan64-1, untraced, against cs-loan64-1 before the
  fix:

  | run | threads woken a WRITE | nfsd's voluntary switches a WRITE | rings' softirq | refused a loaned WRITE | GB/s |
  |---|---|---|---|---|---|
  | cs-loan64-1 (before) | 17.64 | 8.35 | 90.4% | 0.31 | 40.21 |
  | fx-loan64-1 (fix) | 6.68 | 8.24 | 88.1% | 0.57 | 40.37 |

  Counted wakes fall 2.6 times, to 6.7 a WRITE, about the 6.1 to 6.5
  sleeps in `svc_thread_wait_for_work()` the wakee probes saw. The rest
  of nfsd's voluntary switches are its other sleeps, which are not
  woken through the pool. How often nfsd sleeps is the same, as it
  should be: the fix removes calls that woke nobody, not wakes. The run
  is drive-limited with no drive stepped down. The rings are 2.3 points
  lower, but one run lands anywhere from 88 to 94% on this curve, and
  this one refused as often as loan64-5 and -6 at 89 to 90%. So the
  rings' gain, about 1% on the estimate above, is not shown by one run.

  **The A/B against the kernel's own sunrpc** (`run16.sh`: fx-loan64-2
  to -6, the fix; `run17.sh`: ab1-A to ab6-B, alternating A, the
  kernel's own, srcversion `C78A9E1FDAC3084874BA63F`, and B, the fix,
  `fix-sunrpc.ko`, `82ACAC76B5F85A2645B58A4`). Each A/B run comes after
  a reload by `swap-sunrpc.sh`, a check of the loaded srcversion and
  `settled.sh apply`, then a trim and 120 s of rest. All are untraced
  loan runs at 64 WRITEs, `WARM=10 RUNTIME=30`, drive-limited at 40.0 to
  40.4 GB/s, and no drive stepped down. Wakes and sleeps from
  `pool_stats` and `/proc/<pid>/status` around each run:

  | run | ring us a WRITE | rings' softirq | refused a loaned WRITE | woken a WRITE | nfsd's voluntary switches a WRITE |
  |---|---|---|---|---|---|
  | ab1-A | 93.4 | 89.0% | 0.20 | 17.96 | 8.50 |
  | ab2-B | 91.1 | 87.5% | 0.70 | 6.55 | 8.14 |
  | ab3-A | 94.2 | 90.4% | 0.48 | 17.30 | 8.34 |
  | ab4-B | 90.3 | 86.9% | 0.65 | 6.61 | 8.21 |
  | ab5-A | 92.3 | 88.8% | 0.68 | 16.87 | 8.27 |
  | ab6-B | 93.3 | 89.2% | 0.38 | 6.73 | 8.27 |
  | fx-loan64-1 to -6 (B) | 88.3 to 93.2 | 85.0 to 88.9% | 0.18 to 0.92 | 6.38 to 6.90 | 7.98 to 8.45 |

  **The wakes are settled:** every A run counts 16.9 to 18.0 wakes a
  WRITE and every B run 6.4 to 6.9, with nfsd sleeping as often either
  way, 8.0 to 8.5 times a WRITE.

  **The rings gain 1 to 2.5%, about 1.2 to 2.3 us a WRITE.** That is
  the size of the scatter between runs, so it shows only on the curve.
  The fit is ring us a WRITE against refusals a loaned WRITE, with one
  slope for both and a term for the fix:
  - the interleaved A/B alone (6 runs): -1.23 us (standard error 0.96),
    -1.3%;
  - all 18 untraced runs on this boot without loan64-1 (A: loan64-2 to
    -6, cs-loan64-1, ab1, ab3, ab5; B: fx-loan64-1 to -6, ab2, ab4,
    ab6): -2.32 us (0.62), -2.5%;
  - the same in points of the rings' softirq: -1.26 (0.94) and -2.20
    (0.59).

  loan64-1 is left out because it is the odd run of 23 October, the
  first after the boot, with the rings at 76%. Its 78.8 us at 0.96
  refused lies far off the line the others make. With it in, the fit is
  that one point's: -0.37 us (1.26). The other B run that refused
  nearly as often, fx-loan64-4 at 0.92, took 88.3 us, so loan64-1's low
  cost is not where every run goes at that rate. The throughput does
  not move, as the drives decide it. The fix stays a bookkeeping and
  efficiency change (no wake was ever lost), with a small saving on the
  rings, about what the probe estimate gave.

  Not yet done: a test of the loop itself, by giving the rings room
  (more rings, or the dropped `svc_tcp_rx_keep`, which cut wakes). The
  rings are the settled setting's, so it is Mike's to decide.
- At the end the hold factors are 107 (default) and 108 (loans).

# Build 494: the kernel from the tip, no regression (22 October, evening) — read this first

tardis1 booted `7.3.0-7.hs.494.loanpage_pool.el9.aarch64+16k`, built
from the tip on GitLab. The tree has no code commit after
`77f82c816493`, so it carries every commit the section below lists as
"built only"; they have now run. Logs are in `$L/t1-tipkernel/`, and
the figures are in its `results.tsv`.

**The first ten minutes passed as written below.** The kernel's own
sunrpc (srcversion `C78A9E1FDAC3084874BA63F`, the same from `modinfo`),
no `svc_tcp_rx_loan_pages`, `page_pool_rx_region_key` and
`svc_tcp_rx_loans` in kallsyms, the 494 `modules-internal` and `devel`
packages, `arm_cmn` already loaded at boot. No NFS client was connected
before the bring-up. Region 32768 pages, `alloc_fail` 0. `settled.sh
apply` ended in "the settled setting" before and after the KUnit suites.

**KUnit, from the package:** `sunrpc-xdr-bvec` 7, `sunrpc-svcsock-rx`
50, `nfsd-receive-bvec` 7, `nfsd4-receive-bvec` 10. No failure, no
skip. The region key's tests pass, so the fake region holds the key on.

**`run24.sh`, 19:58 to 20:21** (each run after an fstrim of all eight
drives and 120 s of rest):

| run | total GB/s | wire in the window | bytes through memory a byte | drives |
|---|---|---|---|---|
| `one-750k-1` | 40.13 | 46.37 | 1.34 | six stepped down, nvme7 at 46 s, the rest at 60 to 63 s |
| `two-384k-1` | 58.47 (29.78 + 28.69) | 30.06 + 29.03 | — | none stepped down |
| `one-750k-2` | 43.44 | 45.80 | 1.36 | seven stepped down, four at 27 to 33 s |
| `two-384k-2` | 57.89 (29.14 + 28.74) | 29.58 + 29.16 | — | nvme7 at 14 s |
| `loan64-1` | 38.67 | 39.01 | 3.51 | none stepped down |

- **Copy mode on two ports matches 491:** 58.47 and 57.89 against
  57.68.
- **Copy mode on one port:** both fio totals are below 491's 45.08 to
  45.20, but most drives stepped down in each run. The wire in the
  10 s window, 45.80 to 46.37 GB/s, is at or above 491's 45.32 to
  45.50. nfsd's WRITEs a second (43.3k to 43.9k against 42.1k to
  43.0k) and the rings' CPU (44% against 42 to 44%) are as on 491.
  Memory is 1.34 to 1.36 bytes a byte, against 1.34 to 1.35. 491's
  `one-750k-4` had the same shape: 40.94, with wire 44.57.
  **Not a regression, but not yet a clean one-port figure.** The
  drives decided both runs. To get that figure, one port needs a run
  inside the drives' fast region (`WARM=10 RUNTIME=25`), not
  `RUNTIME=60` onto eight drives.
- **Copy mode loaned nothing:** both budgets' `admitted` stayed at
  3096 and 2 across every copy run. Those counts are what the svcsock
  suite leaves behind; 491's 6192 and 4 were two passes of it.
- **The loan path (items 2 and 3) runs:** budget 8198 pages (64
  WRITEs). In the run 2,765,248 records were admitted, with 0 denied,
  0 paused and 0 overrun (3,329,504 in the budget's stats across the
  warm-up and the run).
  Every WRITE was issued `dontcache` (direct 0, buffered 0), as
  expected on the WD_BLACKs, which need whole pages. FIFO drops were
  349 and client retransmits 364, against 0 to 16 and 7 to 51 in
  copy mode. The rings were 54% busy and nfsd 59%. That checks that
  loans work; it is not a measurement.
- **At the end** `settled.sh apply` gave "the settled setting". The
  default budget's `hold_factor` is 107.

**Still open:** the same three items as in the list below (the A/B of
"What a server that never loans pays" on an ordinary NIC, the rig
scripts on `../loan-budget.sh`, the regroup when Mike asks). Also a
one-port copy run inside the drives' fast region, if a clean figure
against 491 is wanted.

# The boot into the kernel built from the tip (the handoff before build 494)

Written on 22 October 2026, before Mike pushed the branch, built a kernel
from it on GitLab and installed it on tardis1. Everything here is for the
first session on that kernel. The section after this one, "22 October,
afternoon", is the handoff before it: what was run on build 491 with the
tip's sunrpc swapped in.

**What the kernel carries that has never run** (the code commits since
build 491; the docs commits are between them). The first five ran on
build 491 as a swapped-in sunrpc; the rest have only been built:

| commit | what | run? |
|---|---|---|
| `f84c0ee96abf` | `svc_tcp_rx_loan_pages` removed: a budget above 0 is what loans, the lookup behind the static key `svc_tcp_rx_loans` | KUnit and copy mode on 491 |
| `d8697f698379`, `69ae778de1e0`, `af1bf767005c` | the KUnit interface and tests for it | KUnit on 491, 7/7 and 50/50 |
| `ec0b3c51f93c` | page_pool: `page_pool_rx_region_key`, on while the region exists | built only |
| `75ab5ec32723` | sunrpc: no skb walk for region pages without a region | built only |
| `78a1538d27bd` | KUnit: the fake region holds the key on | built only |
| `c8c25e8b6b76` | page_pool: the region's and stack's branches behind the key | built only |
| `428a1051e6cb` | sunrpc: the budget's `lock` on its own cache line | built only |
| `fb352b960445` | sunrpc: one budget lookup at a loaned record's first region skb | built only |
| `979433fb2055` | KUnit interface: the copy check and admission by transport | built only |

The "built only" ones were compiled at each commit with W=1,
`CONFIG_WERROR=y` and sparse, with and without `CONFIG_KUNIT`
(`$L/build-regionkey`, `$L/build-nokunit`). Why each exists is "What a
server that never loans pays" in `../UPSTREAM_DEFENSE.md`, items 1, 2,
3, 8 and 9.

**The first ten minutes.**

1. **The kernel.** `uname -r` is the new build, not
   `7.3.0-7.hs.491.loanpages_pool`.
2. **Its own sunrpc, with no swap.** `updates/` is per kernel, so the new
   one has none; build 491's `updates/sunrpc.ko` (the tip's sunrpc
   before the region key, `807985F…`) stays with 491.
   - `cat /sys/module/sunrpc/srcversion` must equal
     `modinfo -F srcversion sunrpc`.
   - `/etc/dracut.conf.d/zc-no-nfs.conf` and
     `/usr/lib/dracut/modules.d/99zc-no-nfs/` keep the NFS stack out of
     the initrd that the kernel's install builds. If the srcversion is
     not the kernel's, the initrd loaded sunrpc: see "22 October:
     tardis1's reboot for its management link" below.
3. **It is a kernel from the tip.**
   - There is no `/sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages`.
   - `grep -w page_pool_rx_region_key /proc/kallsyms` finds it
     (build 491 has none), and so does
     `grep -w svc_tcp_rx_loans /proc/kallsyms`.
4. **The packages beside it**, for `run-kunit.sh` and `build-sunrpc.sh`:
   the new build's `kernel-16k-modules-internal` (the KUnit modules) and
   `kernel-16k-devel`. On 491 both were installed.
5. **`modprobe arm_cmn`.** It was not loaded at 491's boot, and without it
   `memcost-log.sh` counts nothing.
6. **The bring-up**, as before:
   - `for n in 0 1 2 3 4 5 6 7; do mount -o noatime /dev/nvme${n}n1 /hs/nvme$n; done`
     (nvme8 is the root);
   - `./bringup.sh 32768 256`, then `./port-setup.sh enp164s0f0 56`;
   - `systemctl start nfs-server` (if it is not running), then
     `./settled.sh apply`, which must end in "the settled setting".
     `svc_tcp_rx_loan_pages` shows as "not in this kernel", not as BAD.
7. **The KUnit suites, from the package now**: `../run-kunit.sh` with
   nfs-server stopped. It puts the server and the settled setting back.
   - Expected: `sunrpc-xdr-bvec` 7, `sunrpc-svcsock-rx` 50,
     `nfsd-receive-bvec` 7, `nfsd4-receive-bvec` 10. No failure, and on
     16 KiB pages no skip. On 491 they were 7, 48, 7 and 10; the svcsock
     suite gained two cases with the tip.
   - The region key's tests are in `sunrpc-svcsock-rx`: the fake region
     must hold the key on, or every region-page test sees none.
   - Then `./settled.sh apply` again.

**Then the runs: `$L/t1-tipkernel/run24.sh`** (about 35 min; in the
background, `> $L/t1-tipkernel/run24.log 2>&1`). It refuses to start
unless steps 2, 3 and 5 hold and the host is settled.
- **Copy mode, two rounds**, the same shapes as `run22.sh` and
  `run23.sh` on 491:
  - one port onto all eight drives at 750 KiB, with memcost, and the
    budgets' `admitted` unchanged across each run;
  - two ports at 384 KiB.
- **Then one loan run**: one port, `WRITES=64`. It must admit records:
  items 2 and 3 change the loan path, and nothing has run them yet.
- Against 491: one port 45.08 to 45.20 GB/s at 1.34 to 1.35 bytes
  through memory a byte, two ports 57.68. Read every run's `drives.txt`
  before its figure. nvme7 stepped down in each run of 491's first two
  rounds and wrote the least each time.
- The loan run has no figure on this boot to compare with, so it is a
  check that loans still work, not a measurement. tardis1's WD_BLACKs need
  whole pages: expect loaned WRITEs to go dontcache where a piece does not
  fill a page.

**Still open, not for this boot:**
- **The A/B of "What a server that never loans pays"**: `v7.3-7` against
  the tip at upstream's defaults, on an ordinary NIC with no region. It
  gives items 5 to 8 their numbers. Not tardis1, whose card has a region.
- **The rig scripts on `../loan-budget.sh`** have not run on a test host.
- **The regroup**, when Mike asks.

# 22 October, afternoon: the loan switch removed, copy mode as before

The tip's sunrpc on tardis1, build 491, after the reboot that followed
the svcsock KUnit crash (`$L/sunrpc-491-loankey-tip`, srcversion
`807985F30E385B5AF1149B9`). The commits at the tip since the section
below are:

- `f84c0ee96abf` (code): removes `svc_tcp_rx_loan_pages`, so a budget
  above 0 is what loans. With every budget at 0, the default and the
  rig's setting at rest, a record goes straight to the copy behind the
  static key `svc_tcp_rx_loans`.
- `d8697f698379` (KUnit interface) and `69ae778de1e0` and
  `af1bf767005c` (KUnit tests).
- `bfa4d5f0d3bc` (`settled.sh`: the switch is "not in this kernel").

The crash: the suite's shared fixture had no `xpt_net`, and
classification now looks the budget up through it. What a server that
never loans pays is now a task, in `../UPSTREAM_DEFENSE.md`.

**The bring-up.** sunrpc was not loaded after the crash, and `updates/`
held the build from before it. `swap-sunrpc.sh` installed the tip's
build. The suites were run by hand from the build directory:
`sunrpc-xdr-bvec` 7/7 and `sunrpc-svcsock-rx` 50/50
(`$L/sunrpc-491-loankey-tip2/kunit.txt`). Then the eight drives were
mounted, and `./bringup.sh 32768 256`, `./port-setup.sh enp164s0f0 56`
and `./settled.sh apply` were run, ending in "the settled setting". On
this boot `arm_cmn` must be loaded by hand (`modprobe arm_cmn`) before
`memcost-log.sh` can count anything.

`run-kunit.sh` will fail on this sunrpc. It loads the suites installed
with the kernel, built against the old sunrpc: 14 of the exported
functions the suites use changed version, and one they use is gone.
Until the suites are installed beside this sunrpc, run them from
`$L/sunrpc-491-loankey-tip2/sunrpc/` by hand.

**Copy mode, against the same day's runs on `sunrpc-491-rehold`**
(`$L/t1-rcvbuf`, the first section below). The same shapes, each after
an fstrim of all eight drives and 120 s of rest. The drivers are
`$L/t1-loankey/run22.sh` (rounds 1 and 2, without `arm_cmn`, so no
memory figures) and `run23.sh` (one port, rounds 3 and 4, with
`arm_cmn`, and both budgets' `admitted` read before and after each
run):

| run | GB/s | drives | FIFO drops, ring drops, client retransmits | nfsd CPUs busy | bytes through memory for each byte of payload |
|---|---|---|---|---|---|
| one-750k-1 | 45.20 | nvme7 stepped down at 60 s | 28, 0, 29 | 23% | - |
| one-750k-2 | 44.83 | drive-limited, nvme7 at 42 s | 0, 0, 7 | 23% | - |
| one-750k-3 | 45.08 | drive-limited, nvme4 and nvme6 at 38 s | 16, 0, 19 | 24% | 1.35 |
| one-750k-4 | 40.94 (wire 44.57) | five stepped down at 27 to 28 s | 29, 0, 32 | 22% | 1.34 |
| **reference**, one port | 43.86, 41.41 (drive-limited) | | 0, 0, 7 and 6 | 24%, 16% | 1.32, 1.27 |
| two-384k-1 | 39.56: **not valid**, nvme4 to nvme6 stepped down at 17 to 18 s | | 0, 29, 120 | 41% | - |
| two-384k-2 | 57.68 (29.17, 28.51) | nvme7 at 30 s, the run's end | 0, 27, 46 | 58% | - |
| **reference**, two ports | 57.06, 55.67 | | 0, 21 and 46, 26 and 52 | 56%, 59% | - |

- **No regression.** One port carries 45 GB/s, as before or a little
  above. Two ports carry 57.68 against 57.06 and 55.67. Losses, nfsd's
  CPU and the windows (357,000 on one port, 182,784 on two; hold
  factor 108) are as before. The memory traffic is 1.34 to 1.35 bytes
  a byte against 1.27 to 1.32: the reference's 1.27 was a drive-limited
  run at a lower wire rate. The region in use is the same, 27 to
  28 MiB, and so are the cache fills, 2.26 a line.
- **Nothing was loaned.** `admitted` stayed at 6,192 (initial
  namespace) and 4 (default) through rounds 3 and 4. Those counts are
  the KUnit suites', which ran on this module. With every budget at 0
  the key is off, so the credit path cannot be reached.
- **The drives decided most of these runs.** Read `drives.txt` before
  any figure. nvme7 stepped down in each of rounds 1 and 2 and wrote
  the least each time (335 to 442 GB, against 400 to 510); in rounds 3
  and 4 it did not stand out. Not looked into.

# 22 October: 750 KiB on one port, 384 KiB on two

tardis1's cross-check of tardis2's receive buffer result (step 1 of the
handover below), after the reboot. Build 491, the tip's sunrpc
(`$L/sunrpc-491-rehold`, srcversion `3F8D843450D4E5907693FC2`: the credit
wake gating and `de65c0117021`), the settled setting, copy mode, each run
after an fstrim of all eight drives and 120 s of rest
(`$L/t1-rcvbuf/run20b.sh`, its log `run20b.log`, the runs in
`results.tsv`).

**The one-port setting is now 750 KiB** (Mike, 22 October):
`svc_tcp_rx_rcvbuf` 768,000, in `settled.sh`'s values at rest and in
tardis1's `/etc/modprobe.d/sunrpc.conf` (the file before is
`/root/sunrpc.conf.pre-750k-2026-10-22`). Two ports keep 384 KiB
(393,216), which `twoport.sh` sets for a run on both. tardis2 runs one
port only, so the one-port setting is its setting too: its
`/etc/modprobe.d/sunrpc.conf` says 768,000 as well from 22 October
(beside `svc_tcp_rx_loan_pages=Y`, which it keeps; the file before is
`/root/sunrpc.conf.pre-750k-2026-10-22` there). Its live value stays
1 MiB until sunrpc is next loaded or `settled.sh apply` is run there
from this branch. The
settled figures before (47.39 GB/s on one port, 60.41 on two) were at
1 MiB and 384 KiB.

**One port onto all eight drives** (`RUNTIME=60`, `memcost-log.sh`
beside each), 1 MiB and 750 KiB by turns, two rounds:

| run | buffer | window (`rcv_ssthresh`) | GB/s | FIFO drops, ring drops, client retransmits | woken, context switches a WRITE | nfsd CPUs busy | region in use, at most | bytes through memory for each byte of payload | cache fills a line |
|---|---|---|---|---|---|---|---|---|---|
| one-1m-1 | 1 MiB | 487,424 (13), 483,328 (3) | 43.81 | 3,942, 3, 3,952 | 12.79, 14.79 | 23% | 2,064 pages | 1.34 | 2.25 |
| one-750k-1 | 750 KiB | 357,000 | 43.86 | 0, 0, 7 | 14.11, 15.28 | 24% | 1,792 | 1.32 | 2.26 |
| one-1m-2 | 1 MiB | 487,424 | 42.89 | 2,421, 0, 2,429 | 12.03, 14.74 | 23% | 2,077 | 1.37 | 2.27 |
| one-750k-2 | 750 KiB | 357,000 | 41.41 | 0, 0, 6 | 9.86, 12.48 | 16% | 1,808 | 1.27 | 2.26 |

`one-750k-2` was limited by a drive (one of the eight wrote 1.6 GB/s, the
busiest was 98% busy; the wire carried 34.70 GB/s in the window): its
rate, wakes and CPU are the drives', not the buffer's. No drive stepped
down in the other three.

- **The same rate, without the losses**, here where the storage keeps up
  with the wire, as on tardis2 where it does not: 43.86 against 42.89
  and 43.81 GB/s, and no FIFO or ring drops with 6 or 7 retransmits,
  against 2,400 to 3,900 at 1 MiB.
- **Wakes**: about 13% more threads woken and 3% more context switches a
  WRITE (tardis2: 40% and 11 to 15%), nfsd's CPU the same.
- **Memory**: about 4 MiB less of the region in use (1,792 pages against
  2,064 to 2,077), but the traffic barely moves: 1.32 bytes through
  memory a byte against 1.34 and 1.37, and the same cache fills a line.
  tardis1's copy is already near the floor, so the window's share of the
  32 MiB system cache buys little here; tardis2's copy, at 3.19, gained
  6%.

**Two ports, four drives each** (`WARM=10 RUNTIME=30`), 384 KiB, 750 KiB
and 1 MiB by turns; stopped after the second 384 KiB run (Mike), since
two ports were settled at 384 KiB on 14 October and the question was
one port's:

| run | buffer | window | GB/s (a, b) | FIFO drops a + b | ring drops a + b | client retransmits | woken, context switches a WRITE | nfsd CPUs busy | region in use, at most |
|---|---|---|---|---|---|---|---|---|---|
| two-384k-1 | 384 KiB | 182,784 | 57.06 (28.74, 28.32) | 0 | 21 | 26 | 16.67, 11.18 | 56% | 1,296 pages |
| two-750k-1 | 750 KiB | 357,000 | 55.49 (27.41, 28.08) | 1,738 | 145,735 | 154,829 | 12.40, 11.13 | 58% | 1,646 |
| two-1m-1 | 1 MiB | 487,424 | 54.09 (26.99, 27.10) | 38,797 | 182,443 | 233,171 | 12.51, 11.38 | 58% | 1,710 |
| two-384k-2 | 384 KiB | 182,784 (30), 178,857 (2) | 55.67 (27.85, 27.82) | 0 | 46 | 52 | 16.63, 10.93 | 59% | 1,328 |

- **On two ports a larger window loses**: the rings run out of buffers
  (ring drops) and the client retransmits as many, more with each step
  up, and the rate falls 3% at 750 KiB and 5% at 1 MiB. 384 KiB stays
  the two-port setting; one buffer for both is not to be had.
- **Below the settled figure**: both 384 KiB runs, 57.06 and 55.67 GB/s,
  are under the 60.41 measured on 13 October (an earlier kernel, and
  `RUNTIME=45` against 30 here). Not looked into: a two-port run of its
  own on this kernel, at the settled length, is to say whether the
  figure still holds.
- No memcost was taken on two ports. `memcost-log.sh` and
  `memcost-read.py` take two ports now (`a,b`; `cdb19b22d270`), and
  `$L/t1-rcvbuf/run21.sh` is the pair written for it, never run.

**What is left of the handover below**: steps 1 and 2 are done (this
section); keep is Mike's to decide; then the regroup.

# 22 October: tardis1's reboot for its management link

Written on 22 October, before Mike rebooted tardis1 to try to clear its
management link, for the session after. The work since the Xsight
handover is in the next section ("tardis2's four DPRP5s"); this is only
where it stands.

**Why the reboot.** tardis1's `xemi0` (`xemi` 1.0.2, 1 Gb/s, the
management link the client and tardis2 are reached over) drops about
40% of the frames it receives, at idle: 1,178,171 dropped of 2,297,769
by 14:05, with `ring_0_rx_rx_eop_ec_err_smdc_se` (980,207), `..._runt`
(143,182) and `..._pcs_dec` (54,826) still climbing. tardis2's
`enaxsl1i0` dropped 455 since its boot and none while watched. ssh from
tardis1 to the client times out now and then, which stopped the
cross-check below twice before its first run. First, after the boot:
`ip -s link show xemi0` twice, 30 s apart, and `ethtool -S xemi0 |
grep -E 'smdc_se|runt|pcs_dec'`. If they still climb, the cable, the
switch port or the PHY is next; the cross-check is not worth running
over it.

**The branch.** `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY` in the linux
clone, nine commits on `origin/…-v7.3-7` (`eb1ca139fcb5`), **not
pushed**, all at the tip to be regrouped when this round of testing
ends:

- code: `6745782e0794` (wake a region credit waiter only when it can be
  admitted) and `de65c0117021` (a transport held again takes its new
  factor's window; at the regroup its TCP part moves into the TCP
  helpers, `tcp_sock_set_scaling_ratio_max()` and
  `tcp_sock_set_window_clamp()`, as its message says);
- KUnit: `9f922b3ce97b` and `407550bba4ea`, one for each;
- docs: `0ecb9764c066`, `478ab27f1cbd`, `c42e910d1151`, `e0c1729bab83`
  and this handover.

**tardis1 after the boot.** Its sunrpc is the tip's, from
`/lib/modules/$(uname -r)/updates/sunrpc.ko` (`$L/sunrpc-491-rehold`,
srcversion `3F8D843450D4E5907693FC2`): check
`cat /sys/module/sunrpc/srcversion`. **The boot on 22 October did not
keep it**: it came up on `2CC4824FD68A32C33B9C5F4`, the kernel's own.
systemd-boot reads its initrd from the ESP (`/efi/rocky/$(uname
-r)/initrd`, not `/boot`), and dracut had copied Hammerspace's
`/etc/modules-load.d/nfs.conf` into it with every module it names, so
the initrd loaded sunrpc before the real root and `updates/` was never
reached. `swap-sunrpc.sh` put the tip's back (no client was connected).
Since then `/etc/dracut.conf.d/zc-no-nfs.conf` (`omit_drivers` for the
NFS stack) and the dracut module
`/usr/lib/dracut/modules.d/99zc-no-nfs/` (drops the initrd's copy of
`nfs.conf`) keep the NFS stack out of the initrd; the root is local XFS
and needs none of it. The initrd was rebuilt with `dracut -f
/efi/rocky/$K/initrd $K`, and the one before is beside it as
`initrd.pre-zc-no-nfs`. After the next boot the srcversion should read
`3F8D84…` without a swap; if not, boot from that copy by renaming it
back. The same day `nfsd4_bvec_kunit` was taken out of `nfs.conf`
(Mike), so a boot no longer runs that suite; the file before is
`/root/nfs.conf.pre-zc-2026-10-22`. Then as on 20 October: the eight
drives mounted, `./bringup.sh 32768 256`, `./port-setup.sh enp164s0f0
56`, `systemctl start nfs-server`, `./settled.sh apply` ending in "the
settled setting". The sunrpc KUnit suites of that build passed on
tardis1 at 21:46 on 21 October (`$L/t1-rcvbuf/kunit.txt`).

**Then, in this order** (1 and 2 done on 22 October: the section above):

1. **tardis1's cross-check of 750 KiB**: `$L/t1-rcvbuf/run20b.sh >
   $L/t1-rcvbuf/run20b.log 2>&1` in the background, about 1 h 50 min:
   one port onto all eight drives, `RUNTIME=60`, `svc_tcp_rx_rcvbuf`
   1 MiB and 750 KiB by turns with `memcost-log.sh` beside each; then
   two ports, 384 KiB (two ports' settled value), 750 KiB and 1 MiB by
   turns; two rounds of each. It stops at the first failure.
   `one-1m-1` there now is the failed first try (the client did not
   answer): run20b.sh writes over it. The client is shared with
   tardis2: nothing may run there meanwhile.
2. **Write it up and commit**, then **settle the default**: on tardis2
   750 KiB lost nothing measured and ended the losses (next section,
   "The receive buffer"). To make it `svc_tcp_rx_rcvbuf`'s default it
   should also hold where storage keeps up with the wire (tardis1's
   eight drives, run20b's one-port runs), on two ports, with fewer
   connections than 16 (a window a round trip is all one connection
   can carry) and over a longer round trip. In copy mode it also
   leaves about 4 MiB more of the 32 MiB system cache free and takes
   6% less memory traffic a byte; for loans the budget sets that.
3. **Keep and the linger**: done on tardis2 (next section, "Keep and the
   linger again"). Neither changes the rate or saves nfsd's CPU; keep
   saves wakes at no cost, the linger costs CPU spinning and context
   switches sleeping. Keep is Mike's to decide; the linger stays
   dropped.
4. **The regroup** of the commits, then `../transplant/check-grouping.sh`.

**tardis2** (10.20.12.213, the client's ssh key) runs the linger and
keep build with both off, the same as the tip's sunrpc (next section,
at its end); nothing is running there.

# tardis2's four DPRP5s: loan against copy at 40 GB/s (21 October)

Build 491 (`7.3.0-7.hs.491.loanpages_pool.el10.aarch64+16k`), xeu at
rx_ip_align 2, the settled setting, port a. Where this differs from the
17 October section below, it is right.

**The drives changed.** Mike replaced the two DP800s and two of the six
WD_BLACKs with four **DapuStor DPRP5 15.36 TB** drives (PCIe 5 x4, SGLs,
dword alignment, `virt_boundary_mask` 0, 256 KiB transfers of up to 65
segments, 4 KiB logical blocks, no volatile write cache). The kernel's
names changed with them: **nvme0 to nvme3 are the WD_BLACKs, nvme4 to
nvme7 the DPRP5s, nvme8 the boot drive** (the Micron). Each DPRP5 got
its own XFS (`mkfs.xfs -f`, default options; they held only an empty
GPT), and `common.sh` now gives tardis2 eight data drives
(`0ecb9764c066`). After a boot: `for n in 0 1 2 3 4 5 6 7; do mount -o
noatime /dev/nvme${n}n1 /hs/nvme$n; done`, `./bringup.sh 32768 256`,
`systemctl start nfs-server` (`settled.sh` refuses an nfsd with no
threads), then `./settled.sh apply`. Every run below is `WARM=10 RUNTIME=30` after
an fstrim of its drives and 120 s of rest, one run per row unless it
says otherwise; drivers and logs in `/var/tmp/zc-run-logs/t2-dprp5/` on
tardis2 (`run.sh` to `run19.sh`).

**One port onto four drives of each kind.** The window is each
connection's receive window, 340 KiB on the first run of a mode after a
sunrpc load and 476 KiB after it (see "The receive window" below):

| port a | window | GB/s | each drive | nfsd CPUs busy | FIFO drops, ring drops, client retransmits | region in use at most |
|---|---|---|---|---|---|---|
| loan, 64 WRITEs, the DPRP5s | 340 KiB | 40.43 | 10.0 to 10.1, 99% busy | 14% | 0, 0, 5 | 6,437 pages |
| loan, 64 WRITEs, the DPRP5s | 476 KiB | 40.48 | 10.1, 100% busy | 12% | 0, 77, 91 | 6,316 |
| copy, the DPRP5s | 340 KiB | 40.81 | 10.2, 100% busy | 27% | 0, 0, 0 | 1,216 |
| copy, the DPRP5s | 476 KiB | 40.79 | 10.2, 99% busy | 29% | 534, 6, 567 | 1,424 |
| copy, the WD_BLACKs | 340 KiB | 44.82 | 11.2 to 11.3, 97% busy | 24% | 11, 0, 17 | 1,168 |

All five are the drives'. Every WRITE, loaned or copied, went to its
drive direct, none to dontcache: the DPRP5s take loaned pieces as the
DP800s did, and SGLs decide whether a loaned page can go direct, not the
rate. On the same drives and window the loan carries the copy's rate
with half nfsd's CPU. Losses come with the 476 KiB window: none at
340 KiB in either mode; at 476 KiB the loan's are ring drops and the
copy's FIFO drops, the copy adding its memory traffic to the deeper
window's bursts (as X2: the FIFO drops when memory is busy). The
WD_BLACKs' copy wrote 622 to 623 GB a drive, at the edge of their fast
region (twoport.sh: "PAST THE FAST REGION"), with none stepping down.

**The loan wakes more nfsd threads a WRITE than the copy, on the same
drives**: 11.1 to 12.0 against 6.3 to 7.7, at the same receive window
(476 KiB; see "The receive window" below, which also explains the first
pair's 16.0), context switches 14.4 to 14.8 a WRITE against 10.9 to
11.7, for half the CPU. A kernel stack at every
`svc_pool_thread_wake` (`stacks.bt`, one traced pair, about 2.4 million
records each; `s-dprp5-{loan64,copy}-stacks.txt`), a record at a time:

| what woke the thread | loan | copy |
|---|---|---|
| a thread that took work saw more queued (`svc_recv`, `svc_thread_wake_next`) | 3.48 | 2.27 |
| data arrived on a transport no thread held (`svc_data_ready` from `tcp_data_queue`) | 2.86 | 1.45 |
| a thread gave the transport back after a short visit with data waiting (`svc_tcp_recvfrom`'s `err_noclose`) | 2.31 | 2.65 |
| a thread gave the transport back after a whole record (`svc_tcp_recvfrom`'s complete path) | 0.78 | 0.78 |
| the credit chain after an admission (`svc_tcp_rx_credit_admit`) | 0.64 | - |
| the credit chain after a release, as the reply is sent (`svc_tcp_sendto` ... `svc_tcp_rx_credit_release`) | 0.39 | - |
| all | 10.50 | 7.17 |

With the drives the limit, the budget is full: each loaned WRITE was
refused credit 1.65 times on average (2.55 to 2.72 million refusals for
1.43 to 1.54 million admissions a run). The refusals show in what each
visit took (`svcsock_tcp_recv`'s length): 0.81 visits a record took
only a record's first 32 to 63 bytes and were refused (a new record
behind waiters, `head != svsk`, or short of its 126-page cost), and
0.82 a record took nothing (0.05 in copy mode), mostly waiters the
credit chain woke too early. `svc_tcp_rx_credit_wake_locked()` woke the
oldest waiter whenever `avail + reserve > 0`, while admission needs
`avail` to cover the record's whole cost while anything is in flight.
**`6745782e0794`** (code) makes the wake wait for that, and
**`9f922b3ce97b`** (KUnit) tests it; measured below. The
first-bytes visit is not addressed: a record is only known to be a loan
once its first region skb is read, and not waking for data while others
wait would hold small RPCs behind the loans. The larger "woke another"
and "data arrived" counts follow transports being parked and resumed;
the chain from that to each wake was not traced. (`origins.bt`, which
tried the split with per-CPU flags, lost kretprobes and put every wake
on `svc_data_ready`: not a result. Nor is `t-wd-copy`, which ran after
run3.sh was stopped, without its rest.)

**The region against the 32 MiB system cache.** `memcost-log.sh` beside
each run, `memcost-read.py` after; the region in use is at the middle
of the counted stretch, 16 KiB pages:

| port a onto the DPRP5s | GB/s | each drive | region in use | bytes through memory for each byte of payload | to memory | nfsd CPUs busy |
|---|---|---|---|---|---|---|
| loan, 16 WRITEs (2150 pages) | 34.04 | 8.5 | 38 MiB | 0.71 | 24 GB/s | 8% |
| loan, 24 (3158) | 37.58 | 9.4 | 44 MiB | 1.08 | 41 GB/s | 9% |
| loan, 32 (4166) | 38.98 | 9.7 | 51 MiB | 1.37 | 54 GB/s | 11% |
| loan, 48 (6182) | 40.08 | 10.0 | 68 MiB | 1.74 | 70 GB/s | 11% |
| loan, 64 (8198) | 40.48 | 10.1 | 84 MiB | 1.92 | 78 GB/s | 12% |
| copy | 40.79 | 10.2 | 22 MiB | 3.12 | 127 GB/s | 28% |

- **Here the budget sets the rate.** A loaned WRITE holds its pages
  until the drive has them, so the budget is the depth queued at the
  drives; fewer WRITEs in flight, slower drives (88% busy at 16 WRITEs,
  95 to 100% from 24, so "busy" is not the drives' limit). On the DP800s (2.9 GB/s a drive) 16 to 128 WRITEs gave the
  same rate.
- **What the loan holds is the data in flight**, about the drives'
  rate times their latency: some 22 MiB of the region is in use in copy
  mode too (the rings), so the loans hold about 16, 22, 29, 46 and
  62 MiB. The copy holds the same data in flight in nfsd's pages, which
  the CPU writes and memory takes: 3.12 bytes a byte here, against
  tardis1's 2.2 (not looked into).
- **No budget keeps the rate inside the cache**: traffic and rate rise
  together, with no knee. At 64 WRITEs nearly every loaned line goes to
  memory before the drive reads it (1.92: written back, then read), and
  still 38% under the copy. 48 WRITEs gives 99% of the rate for 1.74;
  24 gives 93% for 1.08.
- Client retransmits: 89, 27, 472, 117 and 91 at 16 to 64 WRITEs; the
  472 came with 461 ring drops in that run.

**The receive window: why runs differ in wakes.** Runs of one mode on
the same drives woke threads at two levels (copy about 10 or about 6.5 a
WRITE, loan about 15 or about 11), with the same rate and nfsd CPU. Each
run's `ss-server.txt` shows why: every connection's window
(`rcv_ssthresh`, `rcv_wnd`) was either **340 KiB** (348,160) or
**476 KiB** (483,328 to 487,424). `svc_tcp_rx_hold_buffer()` holds a
connection's receive buffer to `svc_tcp_rx_rcvbuf` (1 MiB) and clamps its
window from a hold factor, the memory its skbs take for each 100 bytes of
payload. A freshly loaded sunrpc has measured none and uses
`SVC_TCP_RX_CREDIT_FACTOR_UNLEARNED` (150): 340 KiB. Connections measure
it as they read (107 on this card) and store it in their budget
(`svc_tcp_rx_budget_default_stats` for copy mode, `..._stats` for
loans), where it stays while the module is loaded; every later
connection is held at 107 from the start: 476 KiB. So the first run of
each mode after a sunrpc load (boot or swap) is at 340 KiB and every
later one at 476 KiB; restarting nfs-server alone changes nothing
(`run15.sh`). A connection held at 150 measures 107 and is held again
(`svc_tcp_rx_hold_stale()`), but before `de65c0117021` its window
stayed at 340 KiB to the end: **early connections never picked up the
learned value mid-life**. Raising the clamp lifts `rcv_ssthresh` only to
the current window, and TCP raises its ratio to a higher ceiling only
when it next measures a segment of a new size. `de65c0117021` lifts both
on a re-hold (`407550bba4ea` tests it); with it, on a fresh load, the
first runs of each mode had 20 holds at 150 then 16 at 107, and every
connection ended at 476 KiB (`run16.sh`, `rh-*`). From then on a mode's
window no longer depends on when its connections started.

- **What the window changes**: with 476 KiB, more of a record waits in
  the socket when a thread gets to it (about 205 KiB at each
  `svc_data_ready()`, against mostly 64 to 128 KiB), so a record takes
  fewer visits (6.2 to 6.4 against 8.3 in copy mode), and wakes follow
  visits (1.13 a visit in every run; `groups.bt`, `gr-copy-{1,2,3}`).
  The 16 connections can also have 7.4 MiB in flight against 5.4, and
  the 476 KiB runs carry the FIFO and ring drops and client retransmits
  (tens to hundreds in the window, against 0 to 8). Rate and nfsd CPU
  are the same. Not the losses: second by second (`persec2.bt`,
  `run14.sh`), a run opens with a burst of losses during which wakes are
  11 to 20 a record and sockets shallow, then settles; reordering is
  rare (at most one out-of-order arrival in a hundred records).
- **Is the factor right?** TCP's own count agrees: memory over unread
  bytes (`skmem r` over `Recv-Q` in `ss-server.txt`, sockets with 64 KiB
  or more unread) has a median of 1.061 in copy mode (224 sockets, 99th
  percentile 1.92) and 1.142 in loan mode (121, 99th 2.05), so the loan
  budget's learned 107 is about 7% low there (not looked into). The
  window is half of what the buffer holds at the factor, so a socket can
  cost up to about 2.1 bytes a byte before its buffer is full; TCP pruned
  or collapsed nothing all day (`nstat`: `PruneCalled`, `RcvPruned`,
  `OfoPruned`, `TCPRcvCollapsed`, `TCPRcvQDrop` all 0). Both budgets
  learned 107 each time they started from nothing.
- **Before `de65c0117021`, compare runs only at the same window**, read
  from `ss-server.txt`: the first run of a mode after a sunrpc load was
  at 340 KiB. Comparisons on tardis1 that spanned a sunrpc load may have
  met the same split; not checked.

**The receive buffer: 750 KiB against 1 MiB.** If the losses come with
the 476 KiB window, a smaller buffer should give the 340 KiB window on
purpose, at the learned factor. `svc_tcp_rx_rcvbuf` 768,000 (750 KiB)
gives about 349 KiB (`rcv_ssthresh` 357,000). On the gating and
`de65c0117021`, by turns with the settled 1 MiB (`run17.sh`, two rounds;
`run19.sh` with `memcost-log.sh` beside it), a WRITE at a time:

| port a onto the DPRP5s | buffer | GB/s | woken | context switches | nfsd CPUs busy | FIFO drops, ring drops, client retransmits | 99.9th percentile completion (round 1) |
|---|---|---|---|---|---|---|---|
| copy | 1 MiB | 40.79 to 40.80 | 6.25 to 7.67 | 10.91 to 12.25 | 28 to 29% | 94 to 443, 22 to 105, 118 to 674 | 86.5 ms |
| copy | 750 KiB | 40.80 to 40.81 | 10.06 to 10.52 | 13.29 to 13.60 | 26 to 27% | 0, 0, 2 to 7 | 89.7 ms |
| loan, 64 WRITEs | 1 MiB | 40.42 to 40.48 | 10.48 to 10.80 | 14.05 to 14.23 | 12% | 0, 22 to 40, 25 to 44 | 97.0 ms |
| loan, 64 WRITEs | 750 KiB | 40.30 to 40.41 | 14.32 to 15.90 | 15.64 to 16.35 | 13 to 14% | 0, 0, 5 to 9 | 97.0 ms |

| port a onto the DPRP5s | buffer | region in use, mid-run | bytes through memory for each byte of payload |
|---|---|---|---|
| copy | 1 MiB | 22 MiB | 3.19 |
| copy | 750 KiB | 18 MiB | 2.99 |
| loan, 64 WRITEs | 1 MiB (`m-dprp5-loan64`, the stock sunrpc) | 84 MiB | 1.92 |
| loan, 64 WRITEs | 750 KiB | 76 MiB | 1.89 |

- **750 KiB has no losses**: no FIFO or ring drops in any of its six
  runs and 2 to 9 retransmits, against tens to hundreds at 1 MiB. Rate
  and the tail of fio's completions (the drives' queues set them) are
  the same.
- **It costs wakes**: about 40% more threads woken and 11 to 15% more
  context switches a WRITE, some 120,000 more wakes a second, a quarter
  to half a CPU of scheduling at a few microseconds each: below what
  nfsd's CPU shows (within 2 points either way, 26 to 29% copy, 12 to
  14% loan).
- **The system cache**: in copy mode about 4 MiB less receive memory is
  in use, room in the 32 MiB cache, and memory takes 6% less for each
  byte; in loan mode the budget, not the window, sets the memory, and
  nothing changes.
- **On tardis2, 750 KiB is the better setting**: nothing measured is
  worse, the losses go, and the copy's memory traffic falls. As a
  default, `svc_tcp_rx_rcvbuf` also bounds what one connection can carry
  (a window a round trip), so it is still to be checked where storage
  keeps up with the wire, on two ports, and with fewer connections.

**The credit wake gating on tardis2.** `6745782e0794` and `9f922b3ce97b`
built on tardis2 (`/var/tmp/zc-run-logs/sunrpc-491-wakeneed/`; gcc
14.3.1 against the kernel's 14.4.1; 14 exports change version, all
KUnit-only, so nfsd loads against it unchanged), swapped in with
`swap-sunrpc.sh`. `sunrpc-xdr-bvec` 7/7 and `sunrpc-svcsock-rx` 49/49
with the new `svcsock_rx_credit_wake_need_test`, run by hand from the
built modules (`run-kunit.sh` loads the installed ones). Loans at 64
WRITEs onto the DPRP5s (`run9.sh`, and `run10.sh`'s keep N runs, the
same code), against the installed sunrpc, a WRITE at a time:

| sunrpc | window | GB/s | refused a loaned WRITE | woken | context switches | nfsd CPUs busy |
|---|---|---|---|---|---|---|
| installed | 476 KiB | 40.45 to 40.48 | 1.51 to 1.75 | 11.10 to 11.96 | 14.35 to 14.81 | 12 to 16% |
| gating | 476 KiB | 40.14 to 40.45 | 0.95 to 1.04 | 10.80 to 11.56 | 14.19 to 14.59 | 12 to 14% |
| installed | 340 KiB | 40.43 | 1.22 | 16.0 | 16.24 | 14% |
| gating | 340 KiB | 40.50 | 0.92 | 14.81, 15.11 | 15.81, 16.09 | 14% |

The traced run (`stacks.bt`, `g-dprp5-loan64-s`) against the installed
one, a record at a time: visits that took nothing 0.33 against 0.82,
credit chain wakes 0.61 against 1.03 (0.14 after an admission against
0.64), all wakes 10.14 against 10.50. A small saving, about 0.4 a WRITE,
with the rate and CPU unchanged; kept in the series.

**`svc_tcp_rx_keep` again, on top of the gating.** The dropped 0006,
rebased (its `svcsock.c` hunk had the linger's context) and built in a
worktree of its own on tardis2 (`/var/tmp/keep-tree`,
`/var/tmp/zc-run-logs/sunrpc-491-wakeneed-keep`; not on the branch).
Keep N and Y by turns on one module load (`run10.sh`), and keep N's
copy from `run12.sh` on the same load, all at 476 KiB:

| run | keep | woken a WRITE | context switches a WRITE | nfsd CPUs busy |
|---|---|---|---|---|
| loan 64 | N | 10.86 | 14.23 | 12% |
| | Y | 8.34, 8.54 | 12.75, 12.99 | 11 to 12% |
| copy | N (gr-copy-2, -3) | 7.37, 7.67 | 12.66, 12.84 | 29 to 30% |
| | Y | 4.86 | 10.65 | 28% |

About a fifth to a third fewer wakes and a tenth to a sixth fewer
context switches, nfsd CPU and rate unchanged: as on tardis1 on 15
October, nothing that meets the bar for taking it back. (`k-loan64-N1`,
at 340 KiB, 15.11, is left out; its fio total, 31.84 GB/s, has a stall
outside the window.)

**Keep and the linger again, at 750 KiB and on `de65c0117021`.** With
every window now from the learned factor, the runs above at two windows
no longer have to be told apart, so both dropped features were run once
more on one module load: the dropped 0005 (`svc_tcp_rx_linger_us`: a
thread that has read part of a record waits on the socket for the rest
instead of giving the transport back) rebased onto the tip, keeping both
its trace events and the tip's, with 0006 on it as above, and a third
form of the linger new here, `svc_tcp_rx_linger_sleep`: instead of
spinning, the thread sleeps on the socket's wait queue
(`schedule_hrtimeout()`) until data arrives, the transport closes or
the linger's time is up. Built in `/var/tmp/keep-tree` on tardis2 (not
committed; the diff is `$L/sunrpc-491-lingerkeep/lingerkeep.patch`),
KUnit 7/7 and 50/50 from the build. Five settings by turns, copy and
loan at 64 WRITEs, two rounds, every run at `svc_tcp_rx_rcvbuf` 768,000
(`run18.sh 768000`, `lk-*`), a WRITE at a time:

| port a onto the DPRP5s | setting | GB/s | woken | context switches | nfsd CPUs busy |
|---|---|---|---|---|---|
| copy | neither | 40.81, 40.82 | 10.36, 10.66 | 13.73, 13.74 | 26, 27% |
| | keep | 40.75, 40.81 | 6.72, 6.73 | 11.85, 11.96 | 24, 26% |
| | linger 50 us, spinning | 40.79, 40.80 | 2.40, 2.48 | 9.57, 9.66 | 33, 33% |
| | linger 50 us, sleeping | 40.80, 40.80 | 2.36, 2.51 | 15.43, 15.76 | 25, 26% |
| | keep and the sleeping linger | 40.81, 40.81 | 2.48, 2.54 | 16.02, 16.89 | 24, 25% |
| loan, 64 WRITEs | neither | 40.40, 40.42 | 14.90, 15.11 | 15.83, 15.88 | 13, 14% |
| | keep | 40.34, 40.40 | 12.30, 12.70 | 14.79, 15.01 | 13, 13% |
| | linger 50 us, spinning | 40.41, 40.48 | 3.85, 4.05 | 9.56, 9.81 | 21, 23% |
| | linger 50 us, sleeping | 40.30, 40.44 | 4.00, 4.02 | 25.67, 27.00 | 12, 13% |
| | keep and the sleeping linger | 40.32, 40.39 | 3.84, 3.89 | 25.86, 26.06 | 12, 12% |

Every window was about 349 KiB (`rcv_ssthresh` 350,106 to 357,000); no
FIFO or ring drops and 0 to 11 client retransmits in every run but
`lk-copy-keepsleep-1` (342 ring drops, 349 retransmits; its second
round had none).

- **None changes the rate.**
- **Keep**: a third fewer wakes in copy mode and a sixth fewer for
  loans, an eighth and a twentieth fewer context switches, nfsd's CPU
  the same or a point or two lower. The same as at 476 KiB above.
- **The spinning linger** cuts context switches by 30 to 40%, and wakes
  by three quarters, for 6 to 9 more points of nfsd's CPU: the cost it
  was dropped for on 15 October, measured again.
- **The sleeping linger is not a cheaper spin.** The pool's wakes fall
  as with the spin, since the transport is not given back, but the
  thread now sleeps and is woken by the socket's own callback, which
  "threads woken" (`svc_pool_thread_wake`) does not count: context
  switches rise by 15% in copy mode and 65% for loans, more than the
  wakes it saved, for no CPU saved (within a point). Keep on top of it
  adds nothing.
- So, by 15 October's bar (rate or nfsd's CPU), neither feature earns
  its place back; keep is the only one with nothing against it, and
  whether fewer wakes alone are worth its code is Mike's to decide.

tardis2 is left running the linger and keep build (`updates/sunrpc.ko`
from `$L/sunrpc-491-lingerkeep`, srcversion `8BB73DEC24E9BBB107B00FC`)
with keep `N`, the linger 0 and `svc_tcp_rx_linger_sleep` `N`, so it
runs as the tip's sunrpc does (`$L/sunrpc-491-rehold`, srcversion
`3F8D843450D4E5907693FC2`: the gating and `de65c0117021`). To go back
to the tip's: `./swap-sunrpc.sh $L/sunrpc-491-rehold`. To go back to the
installed sunrpc: `rm /lib/modules/$(uname -r)/updates/sunrpc.ko`,
`depmod -a`, `SUNRPC_KO=none ./swap-sunrpc.sh`, `systemctl start
nfs-server`, `./settled.sh apply`.

# tardis2: the second server, with SGL drives (17 October)

tardis2 (`tardis2.perf.hammer.space`) is tardis1's twin: the same E1
card (`enp161s0f0`, `enp164s0f0`), 64 Neoverse-N2 CPUs, a 16 KiB
kernel (build 481), and the same client (plsm121c-06, reached at
10.20.12.106 with `/root/.ssh/id_ed25519_snitzer2`). What differs:

- **The drives** (until 21 October; now four WD_BLACKs and four
  DPRP5s, nvme0 to nvme7, in the section above). Six data drives, each
  with its own XFS on `/hs/nvme0` to `/hs/nvme5`: nvme0 to nvme3 are WD_BLACK SN8100 2 TB (no SGLs,
  `virt_boundary_mask` 4095, 1 MiB transfers), and **nvme4 and nvme5
  are DapuStor DP800 1.9 TB with SGLs** (dword alignment,
  `virt_boundary_mask` 0, 256 KiB transfers of up to 65 segments, no
  volatile write cache and no autonomous power states). nvme6 is the
  boot drive and is never a data drive: `common.sh`'s `data_drives`
  refuses one that holds a system filesystem, and the udev rule matches
  by model. The `nvme` driver builds an SGL only for an average segment
  of `sgl_threshold` (32768) or more.
- **The addresses.** Port a is 192.168.1.213 (`common.sh` picks it by
  host); management is `enaxsl1i0` (driver `xemi`) at 10.20.12.213.
- **Port b is not cabled**: `PORTS=a` in `common.sh`'s host block, and
  `settled.sh` audits and places port a only.
- **No tuned and none of Hammerspace's services.** The sysctls are in
  `/etc/sysctl.d/90-zc-settled.conf` (made from `settled.sh`'s table),
  nfsd's 512 threads in `/etc/nfs.conf` (pinned by `settled.sh place`),
  its 1 MiB `max_block_size` in the drop-in
  `nfs-server.service.d/zc-max-block-size.conf`, irqbalance's
  `--banmod=xeu` and banned CPUs 8-55 in `/etc/sysconfig/irqbalance`,
  xeu's and sunrpc's settings in `/etc/modprobe.d/{xeu,sunrpc}.conf`,
  port a's MTU 7792 in NetworkManager ("Wired connection 2"), and the
  WD_BLACKs' write-through in `/etc/udev/rules.d/99-zc-write-through.rules`.
- `perf` (7.3, as the kernel) was installed by Mike: Hammerspace's
  package needs a libLLVM no repo has, and Rocky's is 6.12. `sparse` is
  in no enabled repo.
- **The scripts follow the host** through `common.sh`: each port's
  addresses (`SRVP`, `CLIP`), its default filesystems (`PFS`; FS_A and
  FS_B still override), the default ports (`DEFWHICH`: `a` here, `ab` on
  tardis1), the data drives (`data_drives`, which `drvsample.py` and
  `nfsd-stacks.sh` take too), and `which_ok`, which refuses port b here
  (and `rwbase.sh`'s two-port runs). `rcv.py` and `snd.py` take the
  addresses as arguments; `stall-watch-client.sh <secs> 213` watches for
  tardis2.
- **The client is shared with tardis1.** A run unmounts the client's
  `/mnt/rxr-*` and kills its fio, iperf3 and senders, which would end the
  other server's run, so `twoport.sh`, `iperf2port.sh`, `ringsize.sh` and
  `wirebuf.sh` first call `client_free`: it refuses while the client has
  NFS mounted from another server or one of those tools running.
- What was on nvme0 (thayne's `hke.git` and `ofe-*` files) is in
  `/root/nvme0-backup-2026-10-17`; the root filesystem has about 8 GB
  free after it.

After a boot: `for n in 0 1 2 3 4 5; do mount -o noatime /dev/nvme${n}n1
/hs/nvme$n; done`, then `./bringup.sh 32768 256`, then `./settled.sh
apply`, which ended in "the settled setting" on 17 October.

**The first runs** (17 October, copy mode at the one-port setting, all
six drives, `WARM=5 RUNTIME=15`, so short and only a check of the rig):
44.12 and 46.35 GB/s on port a (`/var/tmp/zc-run-logs/tardis2-first-twoport*`),
against tardis1's 47.39 on eight drives. The DP800s write at most about
2.9 GB/s each and are 100% busy at it, the WD_BLACKs about 9.5 at 80%:
with the writers divided evenly they wrote 103 GB each to the WD_BLACKs'
350, and twoport.sh marks them DRIVE-LIMITED and, in the second run,
"stepped down" at 9 s (its test was made for drives that fall from
11 GB/s, so for the DP800s it may say only that they are at their rate).

**The first loan onto real SGL drives** (17 October, port a onto the two
DP800s alone, `FS_A="/hs/nvme4 /hs/nvme5" WARM=10 RUNTIME=25`; twoport.sh's
`WRITES=<n>` sets a loan budget as sweep.sh's cells do, and every run now
counts how nfsd issued its WRITEs):

| | loan, 16 WRITEs (2150 pages) | 32 (4166) | 64 (8198) | 128 (16262) | copy |
|---|---|---|---|---|---|
| fio, GB/s | 5.73 | 5.76 | 5.77 | 5.76 | 5.77 |
| WRITE segments direct / dontcache / buffered | 190976 / 0 / 0 | 191816 / 0 / 0 | 192184 / 0 / 0 | 192272 / 0 / 0 | 192352 / 0 / 0 |
| loans admitted / denied / paused | 190976 / 393378 / 0 | 191816 / 405536 / 6 | 192184 / 405934 / 5 | 192272 / 401979 / 2 | - |
| threads woken, context switches a second | 44810, 107628 | 35480, 90975 | 35504, 91485 | 35483, 91542 | 2833, 25627 |
| client retransmits in the 10 s window | 875 | 311 | 61 | 65 | 101 |
| region pages in use at most | 3515 | 5425 | 8958 | 15836 | 1568 |

Every WRITE was loaned and every loan was written direct at each
budget: the DP800s take the loaned pieces as they are, with nothing
falling back to dontcache, which nvme-loop and the WD_BLACKs
(`virt_boundary_mask` 4095) cannot do. All four runs are the drives'
(2.9 GB/s each, 94 to 100% busy), so they say nothing of rate or CPU.

A denied record is not copied: `svc_tcp_rx_credit_admit()` puts its
socket in the budget's FIFO and it is admitted when credit returns;
`denied` counts each refusal. It stays at 2.1 a WRITE from 16 WRITEs
to 128, and the region's pages in use follow the budget up (3515, 5425,
8958, 15836, half the region at 128), so the budget is full at every size: with storage slower than
the wire and the client keeping 2 GiB in flight, the WRITEs wait at
the budget whatever its size, and more budget only holds more of them
in memory. What a larger budget did change is the client's
retransmits (875, 311, 61, against the copy's 101; 65 at 128, so
nothing more past 64) and, from 16 to 32 only, the wakeups; from 32 to
128 the loan still woke twelve times the
threads of the copy, the cost of the wait and wake through the FIFO.
Logs in `/var/tmp/zc-run-logs/tardis2-sgl-{loan16,loan32,loan64,loan128,copy}`.

**A loan over drives of both kinds** (17 October, port a onto all six,
`WRITES=64`, `WARM=10 RUNTIME=25`). The loan is decided as the record
arrives, before nfsd knows its file, so every WRITE is loaned and each
drive takes it as it can: the DP800s direct, the WD_BLACKs, whose
`virt_boundary_mask` the loaned pieces do not meet, as one buffered
dontcache write (`NFS_DIO_MEM_MISALIGNED` in `nfs_dio_split()`), a copy
into the page cache. That is not copy mode for them, and no setting
gives loan to some exports and copy to others.

| | loan 64, six drives | copy, six drives | loan 64, the four WD_BLACKs | copy, the four WD_BLACKs |
|---|---|---|---|---|
| fio, GB/s | **19.72** | 44.93 | 38.30 | 45.53 |
| WRITE segments direct / dontcache | 191408 / 466624 | 1503240 / 0 | 0 / 1277064 | 1518144 / 0 |
| loans admitted / denied | 658032 / 1252655 | - | 1277064 / 2476 | - |
| GB/s a drive, WD_BLACK / DP800 | 3.5 / 2.9 | 9.9 / 2.9 | 9.5 (86% busy) / - | 11.3 (99% busy) / - |
| nfsd CPUs busy (8-55) | 9% | 24% | 59% | 26% |
| ring drops, client retransmits | 19, 21 | 3, 3654 | 24324, 26307 | 4, 4711 |

Mixed, the loan carries less than half the copy, and the reason is the
budget, not dontcache: the WD_BLACKs alone take loaned WRITEs as
dontcache at 9.5 GB/s each, but beside the DP800s they write 3.5. The
budget and its FIFO are one for every connection; a loan bound for a
DP800 holds its credit until the slow drive has it, and the FIFO puts
every record after it behind it, so all six drives go at about the
DP800s' pace (178 GB to each WD_BLACK, 146 to each DP800). Dontcache is
not free either: on the WD_BLACKs alone it took 59% of nfsd's CPUs to
the copy's 24% over six, with ring drops and retransmits the copy does
not have. On the same four drives the copy carries 45.53 GB/s, the
drives' 11.3 each (566 GB, inside the fast region): where storage needs
whole pages, a loan that ends as dontcache costs 16% of the rate and
more than twice nfsd's CPU against copying on receipt. A loan for some storage beside a copy for the rest would need
the choice made per export, or a budget that one slow device cannot
fill for all. Logs in `/var/tmp/zc-run-logs/tardis2-{six-loan64,six-copy,wd-loan64,wd-copy}`.

# 20 October: tardis1's boot into the v7.3-7 kernel — read this first

Written on 20 October, after Mike pushed the series rebased onto
`v7.3-7` as a **new** branch,
`hs-gitlab/kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-v7.3-7` at
`5a0e2aeaf190` (491 commits), and started a pipeline build of it, for
the session after he reboots tardis1 into that build. Where it differs
from the sections after it, it is right. The rebase itself is told in
`../README.md`, "Branch state (2026-10-20)".

**The branch to work on is `…-v7.3-7`.** The old name,
`kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`, is still `26c5cdcd549a` on
`v7.3-3` on hs-gitlab (build 479 came from it). Fetch, and
commit at the tip of `…-v7.3-7`; `../transplant/check-grouping.sh
hs-gitlab/kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-v7.3-7` before
handing back. The remote's name is the tree's: on tardis1 hs-gitlab is
`origin` (gitlab refused root's key there on 20 October), and
`check-grouping.sh v7.3-7` takes the tag, which the `snitzer` remote
carries. The `….pre-rebase-v7.3-7` branch is only in the tree the
rebase was done in; on tardis1 the same commit is
`origin/kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`.

## What changed since build 479

- **The base.** `v7.3-7` is on `v7.3` itself (479's `v7.3-3` was on
  rc7), and carries the NFSD DIRECT changes made since: the per-outcome
  `nfsd_write_dio_*` and `nfsd_read_dio_*` tracepoints that replaced
  `nfsd_write_dio_split`; a synchronous direct-mode WRITE persisted
  once, after all of its segments; and the shared boundary page of a
  split WRITE dropped only once both WRITEs have written it, with the
  mm and iomap side of that (`WB_DONTCACHE_DIRTY`,
  `folio_mark_dropbehind()`, a partly covered page kept until an
  uncached write completes it). The rig runs NFSD DIRECT
  (`io_cache_write` 2), so a split WRITE's buffered ends go through
  that new code: a change in rate against 479 belongs to the base
  first.
- **The series' code is 479's** but for two things. "nfs_common:
  require interior payload gaps to be logical-block aligned" now
  reports a payload refused for an interior gap off an `offset_align`
  multiple as its own outcome, `nfsd_write_dio_gap_misaligned`; until
  now it was counted as `mem_misaligned`. And "NFSD: Release a WRITE's
  receive loan once the data is in the page cache" sits on the base's
  new `nfsd_vfs_write()` (the stable flags passed by value), its own
  lines unchanged. `git range-diff` shows nothing else but context.
- **A new KUnit case**, `nfsd_bvec_dio_disposition_test` in
  `nfsd-receive-bvec`: never run anywhere (the host the rebase was
  done on has no KVM).
- `../run-sgl-nfsd-cmp.sh` and `../run-sgl-localio-ab.sh` count the
  per-outcome events; no script in this directory used the old one.

Recognise the kernel: `uname -r` names `7.3.0` with no `rc7` and a
build after 481; `ls /sys/kernel/tracing/events/nfsd | grep
write_dio` lists `direct`, `gap_misaligned`, `mem_misaligned`,
`no_alignment`, `no_middle` and `too_small`, and no `split`; `modinfo
-p sunrpc` lists the same parameters as 479's (`svc_tcp_spare_bytes_max`,
none of the dropped ones).

## In this order

1. **The boot**, as for 479: the drives mounted, `./bringup.sh 32768
   256`, `./port-setup.sh enp164s0f0 56`, `./settled.sh apply` ending
   in "the settled setting" (loans `Y`, one pool of 512 threads,
   `svc_tcp_spare_bytes_max` 67108864, `io_cache_write` 2). Never get
   past a BAD item by hand.
2. **The KUnit suites from the RPM**, `../run-kunit.sh` with
   nfs-server stopped, into `$L/kunit-<build>.log`: **7, 48, 7 and 10
   cases** (479: 7, 48, 6 and 10; the one more is the new disposition
   case), no failure, no skip. The first run of the series' suites on
   `v7.3-7`.
3. **`dmesg`**: no WARNING, BUG or "svc: TCP receive window clamp
   failed", after the boot and again after the runs.
4. **Copy mode against 479**, the 479 battery (`$L/b479/battery.sh`
   and its `-slow` copies) into `$L/b<build>/`, at what 479 found
   reads on this host: trim the eight drives first (`fstrim -v
   /hs/nvmeN`, about 1 s each), `WARM=10 RUNTIME=30 REST=120`, and the
   client idle (`uptime`; no `logperf` or `fio` on 10.20.12.106).
   479's clean runs: two ports **56.86 to 58.58** GB/s (58.04, 58.58,
   58.27, 56.86), one port a 45.88 and 47.42, one port b 46.34 to
   47.80; 2.19 to 2.28 and 1.43 to 1.50 bytes through memory for each
   byte. Read `drives.txt` of every run and leave out a run a drive
   stepped down in. Even after a trim, the third two-port run in a row
   had drives step down in all three trimmed sets on 479 (abt-3,
   abs-3, abh-3): count on two clean runs a trim.
5. **The outcomes, counted**: with `bpftrace -e
   'tracepoint:nfsd:nfsd_write_dio_* { @[probe] = count(); }'` beside
   one copy run of step 4 and one loan run, each loaded at idle first.
   The loan run is a `prp` cell through `sweep.sh` (`256 1048576 8 prp
   step-on-n16`, onto nvme0's export), with the same environment as the
   `16k-42` runs on 473 and 474: every cell of `16k-42` is `sgl`, onto
   the NVMe/TCP target, which takes arbitrary pieces, so its loans are
   written direct and show neither refusal. One `sgl` cell beside it is
   the control. `$L/b491/battery.sh` runs both. Copy
   mode receives each record into contiguous pages, so it should show
   no `gap_misaligned`. A loaned WRITE's fragments end where its
   frames did, and these drives are PRP-only (`virt_boundary_mask`
   4095), so its gaps count: a loan that is refused direct I/O is now
   `mem_misaligned` only for a fragment off the memory alignment (where
   the frame landed in its buffer) and `gap_misaligned` for one whose
   gap falls off an `offset_align` multiple. Earlier builds counted
   both as `mem_misaligned`; how a loan run divides between the two is
   the new information. Record the counts, do not predict them.
6. **The bound**: `drgn spare-count.py 300 0.5` beside one two-port
   run; at most about 2,080 pages, 0 once the clients are gone.

After these, the follow-up of Xsight's replies:
`XSIGHT-FOLLOWUP.md` here, items X1 to X7 (what they answered and our
reading of it are in `../xsight-email-checklist.md`).

Write it up as "Build <N>: the first v7.3-7 kernel" after this
section, as the 479 section was: what each step showed, the table
against 479, the runs in `$L/b<N>/` and their lines in
`$L/b<N>/results-twoport-<N>.tsv`. Then the README's branch state if
anything there changes; docs commits only, and the grouping check.

## Build 491: the first v7.3-7 kernel (20 and 21 October)

Build 491 (`7.3.0-7.hs.491.loanpages_pool.el9.aarch64+16k`) on
tardis1: `uname -r` names 7.3.0 and no rc, the `write_dio` events are
the six of the section above and no `split`, and `modinfo -p sunrpc`
has `svc_tcp_spare_bytes_max` and none of the dropped parameters. The
battery is `$L/b491/battery.sh` (written before the boot) and
`battery-from4.sh` (its copy without step 2); logs in `$L/b491/`.

- **Step 1, the boot: as written.** The eight drives, `bringup.sh 32768
  256`, `port-setup.sh enp164s0f0 56`, `settled.sh apply`: the settled
  setting, loans `Y`, one pool of 512 threads on 8-55, spare 67108864,
  `io_cache_write` 2 (`$L/b491/boot.log`).
- **Step 2, KUnit: one case failed, a test the rebase did not adapt.**
  7, 48, 7 and 10 cases, but `nfsd_bvec_dio_segments_test` failed in
  its seven DONTCACHE cases with a buffered segment: base flags 0x1000002
  against 0x2 (`$L/kunit-491.log`). v7.3-7's "nfs_common: share direct
  I/O write split and boundary-page helpers" issues a buffered
  DONTCACHE segment with `IOCB_DONTCACHE_DEFER` (1 << 24), and the test
  expected only `IOCB_DSYNC`. Fixed by `b783bf8ec202` (Mike's ask), the
  bit expected on exactly those segments. With that module built
  against the kernel's -devel and loaded in place of the installed one:
  7, 48, 7 and 10, no failure, no skip (`$L/kunit-491-fix.log`). The
  installed 491 RPM still carries the old test.
- **Step 3, `dmesg`**: no WARNING, BUG or window clamp failure after
  the boot or the runs. One WRITE timeout on nvme8 (the root drive) at
  852 s, before bringup, outside the rig.
- **Step 4, copy mode: ab-1 only** (Mike stopped the battery after it):
  48.14 GB/s, a 23.24 and b 24.90, with nvme0, nvme3 and nvme7 stepping
  down at 25 to 31 s, so not comparable with 479's clean 56.86 to
  58.58. No FIFO or ring drops in the window; every segment written
  direct (1,937,744); the outcome counter saw only
  `nfsd_write_dio_direct` (2,814,984), as copy mode should. The clean
  two-port runs of X5 (`XSIGHT-FOLLOWUP.md`) are the better reading of
  491: 57.48 and 56.83 GB/s at `rx_refill_thresh` 32, 58.13 at 8, all
  in 479's range.
- **Steps 5 and 6: not run** (the loan outcomes and the spare-page
  bound), at Mike's word.
- **The devel package came in damaged**: `kernel-16k-devel` for 491
  had 17,915 of its 21,820 files at 0 bytes (installed 20:55, before
  the reboot), so no module could be built against it. It was
  reinstalled from dnf's cache (`rpm -K` digests OK). That reinstall
  was made with `rpm -Uvh --replacepkgs`, which **removed 479's and
  474's `kernel-16k-devel`**; Mike chose not to restore them. Use
  `rpm -i --replacepkgs` for a kernel package.
- **xeu is Release 8 now** (X4: a ring interrupt keeps its affinity
  across a close and open; kernel-rpmtree `787354a`, pushed), installed
  with `rpm -ivh --force` and Release 7 dropped from the rpm database
  only, so the 462 and 463 kmods stay.
- **The client is slow to answer ssh right after a run** (three runs
  of X5 stopped on it); the rig's ssh now waits 30 s to connect and
  ends a dead connection after 30 s.

# 15 October: the boot into the reworked tree

Written late on 14 October, after Mike pushed
`kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY` at `32cf5556bd2e` (459
commits on `v7.3-3`) and started a pipeline build of it, for the
session after he reboots tardis1 into that build. Where it differs
from the sections after it, it is right.

## What the kernel is

The series reworked for upstream that night: the code group reviewed,
folded, split and given upstream messages, 38 code commits, then 44
KUnit interface and test commits, then the docs. The account is steps
3 to 5 of `../CODE-SERIES.md`; `AGENTS.md`'s branch entry has the
summary. Build 473 was built from the tree before it. The code differs
from 473's in six places only (`git diff
kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-code.pre-fixes
kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-code.73rebase8`, 6 files):

- **TCP receive page loans are off by default**
  (`svc_tcp_rx_loan_pages`, Y on 473). `settled.sh` sets Y and its
  audit holds the rig to Y, so the rig loans as before; a kernel booted
  without it applied copies every record.
- An NFSv4 COMPOUND with a tag keeps its loan until the reply is
  encoded (the tag is copied from the received pages then).
- The reply cache checksums a call's first bytes from a buffer on the
  stack, not from the reply buffer.
- `pc_xdr_bvec` is a bool, `svc_proc_lookup()` is exported only for
  KUnit, an unused include is gone, and the KUnit-only
  `svc_tcp_rx_fixed_factor()` lives in the KUnit interface.
- `svc_tcp_rx_copy_unlocked` was already gone (see "Copy mode on 7.3"):
  records are copied under the socket lock unless
  `svc_tcp_rx_copy_threads` is 1 or more. On one port the two carried
  the same on 473 (45.81 GB/s locked, 45.40 unlocked, six runs
  alternating on seven drives, `$L/b473/ab4.log`); two ports were not
  settled.

**The branch has moved on since that build** (later on 14 October,
`.73rebase9`): two calls into TCP now go through TCP's own interfaces,
and that build has neither.

- The region budget clamps a held transport's window with a new
  `tcp_sock_set_window_clamp()` (added in the tcp commit beside
  `tcp_sock_set_scaling_ratio_max()`) instead of the `TCP_WINDOW_CLAMP`
  socket option. The same `tcp_set_window_clamp()` under the same lock,
  so no change in behaviour; but it is built into vmlinux, so it needs
  the next full kernel build to run.
- `svc_tcp_rx_lowat` sets the mark through the socket's
  `->set_rcvlowat` (TCP's `tcp_set_rcvlowat()`, as `SO_RCVLOWAT` does)
  instead of writing `sk_rcvlowat`, and leaves TLS transports alone by
  `XPT_TLS_SESSION`. Only with `svc_tcp_rx_lowat` set does it change
  anything: TCP caps the mark at half the receive buffer and wakes the
  transport at once if enough is queued.

Recognise it: `uname -r` names a build after 473, and
`modinfo -p sunrpc | grep loan_pages` says "(default: N)".

## First, in this order

1. `getconf PAGESIZE` is 16384. Mount the data drives (not in
   `/etc/fstab`): `for n in 0 1 2 3 4 5 6 7; do mount -o noatime
   /dev/nvme${n}n1 /hs/nvme$n; done`; `./drive-cache.sh` shows `write
   through` on all eight.
2. `./settled.sh apply` ends in "the settled setting"; never get past a
   BAD item by hand. Then `cat
   /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages` is `Y`.
3. **The four KUnit suites, never yet run on the reworked tree**:
   `../run-kunit.sh` with nfs-server stopped (it puts the server and
   the settled setting back and fails if the host is not at it). It
   needs the suites installed for this kernel (the RPM's
   modules-internal package). On 473 they ran 7, 46, 6 and 10 cases
   with no failure and no skip; the tests did not change in the rework,
   so the same is expected. Both the tip with KUnit and without it were
   built clean with `CONFIG_WERROR=y` on 14 October
   (`/var/tmp/zc-run-logs/walk-73rebase/kbuild-tip-result.txt`), but
   nothing was run.
4. **No regression against build 473**, at the setting and with the
   commands under "Where it stands" and "The loan on 7.3":
   two ports (`RCVBUF=393216 ENTRIES=128 LINGER=0 LOWAT=0 ./twoport.sh
   <out> ab`; 473: 59.27 and 59.36), one port on eight drives (473:
   45.98 to 47.76), and the three loan plans through `sweep.sh` (473's
   table below). Same code as 473 but for the six changes, so the same
   figures are expected.
5. **The `->set_rcvlowat` change, without a reboot**: confined to
   sunrpc, so `./build-sunrpc.sh` builds `sunrpc.ko` from the branch
   against this kernel's `kernel-devel` and `./swap-sunrpc.sh` puts it
   in (nfsd stopped, mounts down: Mike's to do, as for the locked and
   unlocked comparison on 14 October). Build it with the window clamp
   call put back to the socket option (a local edit, not committed),
   or the module will not load: this kernel has no
   `tcp_sock_set_window_clamp()`. Compare with this kernel's own sunrpc
   with `svc_tcp_rx_lowat=131072 svc_tcp_rx_keep=1`, the setting that
   halved thread wakeups on 13 October.

**The drives first.** On 14 October they were tired: after 13 minutes
idle nvme2 stepped down at 341 GB (a fresh drive takes about 625) and
nvme3 had been stepping down first in every run; the one-port
comparison that night ran on seven drives without nvme3 (`FS_A` of
the other seven). After a reboot they have rested; still read
`drives.txt` of every run, and keep runs short (the comparisons that
night used `WARM=10 RUNTIME=30 REST=120`, and nvme3 still stepped down
once at 34 s on two ports).

## Build 474: steps 1 to 4 done, no regression (15 October)

Build 474 (`7.3.0-0.rc7.3.hs.474.loanpages_pool.el9.aarch64+16k`) is
the reworked tree: `modinfo` says "(default: N)" for
`svc_tcp_rx_loan_pages`. It has neither of the two `.73rebase9`
changes. The runs are in `$L` (`/var/tmp/zc-run-logs`); the driver is
`$L/b474/battery.sh`, which runs 473's `resume.sh` steps 3 and 4
unchanged but for the build number, and its log is
`$L/b474/battery.log`. Every audit came back at the settled setting,
and the host was left at it.

- **Steps 1 and 2.** On a fresh boot `settled.sh apply` refuses at
  once ("enp161s0f0 does not have 4 rings"). The ports come first:
  `./bringup.sh 32768 256` and `./port-setup.sh enp164s0f0 56`, as
  "The rules" says, then `./settled.sh apply` ends in "the settled
  setting" with `svc_tcp_rx_loan_pages` at Y.
- **Step 3: the KUnit suites pass on the reworked tree**, 7, 46, 6
  and 10 cases with no failure and no skip, as on 473
  (`$L/kunit-474.log`). `run-kunit.sh` put the server and the setting
  back.
- **Step 4: copy mode**, 10:38 to 11:01 by tardis1's clock, with
  `memcost-log.sh` beside each run (`$L/b474-<tag>`, their lines in
  `$L/b474/results-twoport-474.tsv`, memory samples
  `$L/b474/copymem-<tag>`):

  | | build 474: GB/s | bytes through memory for each byte of payload | drives | build 473 |
  |---|---|---|---|---|
  | two ports, `ab-1` to `-3` | **59.76**, 59.56, 59.18 (ports 29.3 + 30.5, 30.4 + 29.1, 29.6 + 29.6) | 2.19, 2.17, 2.23 | nvme2 stepped down at 43 s in `ab-1` | 59.27, 59.36; 2.25, 2.23 |
  | one port a, eight drives, `a8-1` to `-3` | 41.62, 45.02, **47.62** | 1.38, 1.44, 1.39 | three stepped down in each of `a8-1` and `a8-2`; none in `a8-3` | 46.77, 45.98, 47.59 |
  | one port b, eight drives, `b8-1` to `-3` | **47.18**, 46.17, 46.00 | 1.42, 1.42, 1.42 | none in `b8-1`; three in `b8-2`, nvme2 in `b8-3` | 47.76, 47.54, 47.06 |

  Two ports carry what 473 carried, at a little less memory for each
  byte; still about 1% under 464's mean of 60.24. One port: the two
  runs in which no drive stepped down (47.62, 47.18) are inside 473's
  range, and in the others the wire carried 46.8 to 48.0 GB/s in the
  read window, so the drives decide those totals, not the kernel.
- **The drives are still tired after the reboot.** nvme2 or nvme3
  stepped down in five of the nine copy runs, nvme3 as early as 28 s
  (`a8-1`, the first one-port run after the three two-port runs): its
  drives wrote 453 to 520 GB each at 2.3 to 5.0 GB/s, where 473's
  `a8-1` wrote 523 each at 5.8 to 5.9 with none stepping down. Rest
  between runs was the harness's default (`REST=30`). Rest alone did
  not restore them; a trim of the data drives, or runs on seven drives
  without nvme3 as on 14 October, is the next thing to try.
- **Step 4: the loan**, the three plans through `sweep.sh` exactly as
  on 473 (`CELL_BOUNCE=1` for all three), 11:01 to 12:01. Runs
  `$L/xeu-16k-474-loanmem-1` to `-3`, memory samples
  `$L/loanmem-474-<n>`, readings `$L/loanmem-474-<n>.read.txt`:

  | rings, held buffer | WRITEs let in | build 474: GB/s | bytes through memory for each byte of payload | ring drops | build 473: GB/s, bytes, drops |
  |---|---|---|---|---|---|
  | 256, 1 MiB | 16 | 11.74 | 3.92 | 0 | 11.74, 4.01, 0 |
  | 256, 1 MiB | 8 | 11.74 | 2.91 | 0 | 11.72, 2.99, 0 |
  | 256, 1 MiB | 4 | 10.55 | 2.04 | 0 | 10.65, 2.37, 0 |
  | 256, 1 MiB | copy | 10.40 | 5.73 | 8 | 10.44, 5.97, 0 |
  | 128, 384 KiB | 8 | 11.73 | 2.04 | 0 | 11.73, 2.14, 1 |
  | 128, 384 KiB | 4 | 9.08 | 1.01 | 0 | 8.45, 1.11, 0 |
  | 128, 384 KiB | copy | 10.39 | 5.20 | 0 | 10.49, 5.11, 5 |
  | 128, 256 KiB | 8 | 11.70, 11.72 | 1.78, 1.78 | 0, 0 | 11.66, 1.86, 0; 11.70, 1.78, 0 |
  | 128, 256 KiB | 6 | 10.51 | 1.25 | 0 | 9.90, 1.42, 0 |
  | 128, 256 KiB | 4 | 7.65 | 0.77 | 0 | 7.44, 1.01, 0 |
  | 128, 256 KiB | copy | 10.43 | 5.09 | 0 | 10.41, 4.96, 0 |
  | 128, 192 KiB | 8 | 11.60 | 1.63 | 0 | 11.50, 1.72, 0 |
  | 128, 128 KiB | 8 | 11.31 | 1.42 | 0 | 10.74, 1.68, 0 |
  | 64, 256 KiB | 8 | 11.71 | 1.32 | 12,374 | 11.63, 1.42, 13,509 |
  | 64, 192 KiB | 8 | 11.51 | 1.20 | 2,243 | 11.46, 1.38, 3,216 |
  | 64, 128 KiB | 8 | 10.63 | 1.14 | 1 | 10.48, 1.24, 1 |
  | 64, 128 KiB | 6 | 8.34 | 0.68 | 0 | 8.24, 0.79, 0 |
  | 64, 128 KiB | copy | 10.47 | 4.72 | 0 | 10.48, 4.72, 0 |

  The loan on 474 is 473's: the same rates within about 1% in most
  cells, three cells better (rings of 128 with 384 KiB and 4 WRITEs,
  256 KiB and 6, 128 KiB and 8), and its cost in memory level or
  lower everywhere, to a floor of 1.14 (1.24 on 473). Every loaned
  WRITE was written direct (`wr_direct`, none buffered or
  `wr_dontcache`) and none held more than it was charged (`overrun`
  0). One run of each cell on each build; the differences are not
  looked into.
- **Step 5 is ready for Mike.** `sunrpc.ko` from the branch (sunrpc
  at `67a5625f68bb`), with the window clamp put back to the
  `TCP_WINDOW_CLAMP` socket option for the build only (reverted
  afterwards, nothing committed), is at
  `$L/sunrpc-474-lowat/sunrpc/sunrpc.ko`; all 326 of the installed
  module's exports match, so nfsd loads against it. To put it in:
  `./swap-sunrpc.sh $L/sunrpc-474-lowat`, then compare it with this
  kernel's own sunrpc at `svc_tcp_rx_lowat=131072
  svc_tcp_rx_keep=1`. `build-sunrpc.sh` complains that it cannot copy
  `include/trace/events/sunrpc_base.h`, which 7.3 does not have; the
  build is not affected.

## 15 October, afternoon: the tip's sunrpc on build 474, and the series cut

The branch moved on that afternoon: six commits from Mike (the
spare-page bound, its KUnit interface and tests, a sparse fix), then
several thread pools per NUMA node dropped and the bound folded into
the page swap it fixes (`../CODE-SERIES.md`, step 6, "What was done").
Everything below ran on build 474 with sunrpc built from the branch
(`build-sunrpc.sh`, the window clamp put back to the socket option for
the build only, since 474 has no `tcp_sock_set_window_clamp()`) and
swapped in (`swap-sunrpc.sh`, its own KUnit modules installed beside
it in `updates/`). The drivers and logs are in `$L/b474lw/`,
`$L/b474sp/` and `$L/b474cov/`.

**The drives are tired**: in these runs one or more stepped down in
14 of 19 copy runs, as early as 18 s into a 30 s run after 120 s of
rest. Only the runs no drive stepped down in are read for rate below.

### `->set_rcvlowat` (step 5): it does what the write to `sk_rcvlowat` did

Two ports at the two-port setting, by turns, `$L/b474lw/ab.log`:

| mark | sunrpc | runs no drive stepped down in | GB/s | threads woken a WRITE |
|---|---|---|---|---|
| 131072 | 474's own | old-1, old-5 | 56.53, 54.87 | 7.67, 7.69 |
| 131072 | `->set_rcvlowat` | new-3 | 57.09 | 7.79 |
| 0 | the tip's (with `->set_rcvlowat` and the bound) | `$L/b474sp`, four runs | 59.41 to 59.81 | 10.0 to 12.8 |

The mark set through TCP's interface halves wakeups as before and
costs the rate it cost on 13 October; with the mark off the call is
never made (the mark stays 1). Nothing changes in the pinned setting:
`svc_tcp_rx_lowat=0`. The other three runs with the mark on, and both
of the new module with it off in this A/B, had drives stepping down.

### The spare-page bound: it holds, costs no rate, and saves nfsd's CPU

`$L/b474sp/queue.sh`, log `queue.log`. The rig learned the knob first:
`settled.sh` holds `svc_tcp_spare_bytes_max` at 64 MiB (reported, not
failed, on a sunrpc without it), `twoport.sh` and `sweep.sh` take
`SPARE=`, and `spare-count.py` reads the count of kept pages from the
live kernel with drgn.

- **KUnit on the tip's sunrpc: 7, 48, 6 and 10 cases**, no failure or
  skip, `svcsock_rx_spare_none_test` and `svcsock_rx_spare_kept_test`
  among them. The bound adds a field to `struct svc_sock`, so the 14
  exports that take one change version; all are exported only for the
  KUnit suites and imported only by `svcsock_kunit`, so the swap is
  safe with the suite built beside it (`build-sunrpc.sh`'s "with
  another version" line is not 0 for this build, and that is why).
- **The bound held in every load**: at the default two ports kept at
  most 2,080 pages (32.5 MiB), one port 1,040 and the loan cell 1,035,
  all under 64 MiB, so the default never bites here; at 4 MiB the count
  stopped at exactly 256 pages, three times; at 0 nothing was kept; with
  the bound changed every 2 s under load (0, 1 MiB, 4 MiB, 64 MiB)
  nothing was left over. After every run, the clients gone, the count
  was 0, and no WARNING was raised.
- **Rate**: two ports, the runs no drive stepped down in: 59.57 (0),
  59.49 and 59.81 (4 MiB), 59.41 (474's own, every page kept). The loan
  cell: 11.47, 11.62 and 11.84 GB/s at the default, 0 and 4 MiB, no
  overrun, every loaned WRITE direct.
- **nfsd's CPU** (`nfsd_busy`, the same runs): 60% at 0, 58 to 59% at
  4 MiB, 49 to 51% on 474's own. The page swap saves about 10 points of
  nfsd's CPUs and the bound keeps that; the rate is the memory's on two
  ports either way.

### What runs in practice: the call counts

`$L/b474cov/coverage.txt`: calls to 91 functions the code commits add
or change, over one port, two ports and a loan cell. The reading is
step 6 of `../CODE-SERIES.md`: copy mode runs none of the loan's own
functions; some of the loan's edges run in no load; "Release a
WRITE's receive loan once the data is in the page cache" runs nowhere
here, since nfsd writes direct.

### Several thread pools per NUMA node: dropped

Mike, 15 October: Chuck Lever has called it a dead end. Out of the
branch; the patch and what dropping it took are in
`../patches/dropped/`. **tardis1 runs one pool from 15:45 that day**:
`pools_per_node=4` is out of `/etc/modprobe.d/sunrpc.conf` and
`settled.sh` holds 512 threads in one pool. Every figure before that
was measured with four.

## 15 October, evening: keep and the linger, each on and off

Commits 35, "SUNRPC: Optionally avoid waking a thread after a short TCP
receive" (`svc_tcp_rx_keep`, Y at the settled setting), and 33,
"SUNRPC: Optionally wait for the rest of a partially received TCP
record" (`svc_tcp_rx_linger_us`, 50 on one port, 0 on two), had been
pinned without a measure of their own (keep together with the
low-water mark on 12 October). Build 474 with its own sunrpc,
everything else at the settled setting, runs of 10 s + 30 s after
120 s of rest, the settings by turns (`$L/b474kl/ab.sh`, results
`$L/b474kl/results-474kl.tsv`; `twoport.sh` gained `KEEP=` for it).

**Two ports, keep Y against N** (8 runs; a drive stepped down in 5):

| keep | GB/s, no drive slowed | GB/s, a drive slowed | woken a WRITE | context switches a WRITE | nfsd busy |
|---|---|---|---|---|---|
| Y | 58.22, 57.35 | 55.18, 57.86 | 14.0, 11.7, 11.9, 11.9 | 10.8 to 11.2 | 55 to 57% |
| N | 58.16 | 57.01, 57.33, 50.54 | 16.7, 16.8, 16.6 | 11.1 to 11.4 | 56 to 59% |

(The 50.54 run had two drives slow and a quarter fewer WRITEs; it is
left out of the per-WRITE figures.) Keep wakes about a quarter fewer
threads, but the context switches a WRITE are within 3% and nfsd's
CPU within a point or two, and the rate shows no difference.

**One port on all eight drives** (9 runs; no drive stepped down):

| setting | GB/s | woken a WRITE | context switches a WRITE | nfsd busy |
|---|---|---|---|---|
| linger 50, keep Y (settled) | 46.29, 47.69, 47.57 | 2.5 to 2.6 | 10.4 to 10.9 | 31 to 32% |
| linger 0, keep Y | 46.58, 47.07, 46.12 | 7.7 to 8.1 | 12.7 to 13.0 | 24 to 25% |
| linger 50, keep N | 47.43, 47.77, 47.77 | 2.5 | 10.7 to 10.9 | 31 to 32% |

Keep changes nothing on one port: with the linger a thread already
waits for the rest of a WRITE. Without the linger the wakeups
triple, and with it nfsd's CPU is about 7 points busier (some 3 of its
48 CPUs, busy-waiting); the rate at 0 was the lowest of its round twice
in three, about 1% under, which three runs a setting do not establish.

**Both are dropped from the series (Mike, that evening)**, their patches
in `../patches/dropped/` (0005, 0006). The settled setting is now linger
0 and keep N where the kernel still has them (build 474), and
`/etc/modprobe.d/sunrpc.conf` sets neither (the file before is
`/root/sunrpc.conf.pre-drop-linger-keep`). Every one-port figure before
was measured at a 50 us linger, every figure with keep Y.

## 15 October, late: the note to Xsight checked against today's setting

The draft `../xsight-uncounted-loss-email.txt` (11 and 12 October) was
re-read claim by claim and rewritten against the settled setting on
build 474 (`$L/b474xs/`, scripts `runs.sh`, `runs2.sh`, `defer.sh`).
One port on all eight drives, 10 s + 30 s after 120 s of rest, the
port's FIFO fill and drops, the rings' frames and TCP's segments
sampled once a second beside each run:

| cell | GB/s | FIFO drops in the window | ring drops | retransmits | frames a segment (GRO) | FIFO fill: median, 90th, max of 16384 |
|---|---|---|---|---|---|---|
| settled, `fq_codel` (`codel-1`, `-3`) | 46.16, 46.45 | 4,334, 4,717 | 0, 5 | 4,341, 4,761 | 5.94, 5.99 | 0, 700 to 1,000, 10,915 and 14,269 |
| client paced, `fq maxrate 23gbit` a flow (`fq23-2`, `-4`) | 36.95, 37.82 | 0, 0 | 0, 0 | 9, 6 | 7.20, 7.20 | 0, 1, 1 |

- **The FIFO still drops at today's setting, about one frame in
  100,000**, two orders of magnitude under the 11 October figures,
  which were at the 2 MiB and `copy_threads` settings. Bursty as
  before: fill 0 in more than half the readings.
- **Paced, nothing is lost anywhere and the rate is 37 to 38 GB/s**,
  the client's connections limited by the receive window 86 to 91% of
  the time (`client-ss.txt`). The 11 October paced run also tail-dropped
  42 thousand frames at the rings; today none. GRO merges **more**
  frames a segment when paced (7.2 against 6.0) with the rings' CPUs
  equally busy (47% against 46%), so the note's suspicion that paced
  traffic merges worse is wrong, and its question on coalescing was
  dropped with it.
- **Deferring interrupts stops the port, reproduced** (`defer.sh`,
  `$L/b474xs-defer`): `napi_defer_hard_irqs=2` and `gro_flush_timeout=
  50000` set under load, the rings' frames fell from 3 million a second
  to a handful within 2 s, the client was unreachable over the port,
  writing 0 to both did not revive it, down and up did; nothing logged.
  The client's fio hung on its mount until the port came back.
- `codel-1` ran with the server's tx pause still on, `codel-3` with it
  off (the setting that was found and fixed at 22:11, "settled.sh
  holds pause off at both ends"); no difference between them.

What the rewrite kept, cut and why is in the note's own history
(`git log -- ../xsight-uncounted-loss-email.txt`). Every question left
in it rests on one of these measurements, the 11 October accounting, or
the driver's source.

## The next kernel: what is still to be tested

Build 479 is that kernel, and steps 1, 2, 4 and 5 are done on it
(16 October): see "Build 479" after this list.

For the session that boots the first kernel built from the branch
after 15 October (the folded tip, or later). Recognise it:
`modinfo -p sunrpc` lists `svc_tcp_spare_bytes_max` and no
`pools_per_node`, and `grep -w tcp_sock_set_window_clamp
/proc/kallsyms` finds it. A kernel built after the second drop that
day also lists no `svc_tcp_rx_lowat` and no `svc_tcp_rx_copy_threads`
(both were 0 on the rig; `settled.sh` reports them as "not in this
kernel", and `/etc/modprobe.d/sunrpc.conf` no longer sets the mark, the
file before being `/root/sunrpc.conf.pre-drop-lowat`); the kernel built
from the push before it still has both, at 0. A kernel built after the
drop of the linger and keep has no `svc_tcp_rx_linger_us` and no `svc_tcp_rx_keep`;
before it they are held at 0 and N.

1. **The boot**, as under "First, in this order" above: the drives
   mounted, `./bringup.sh 32768 256`, `./port-setup.sh enp164s0f0 56`,
   `./settled.sh apply` ending in "the settled setting" (one pool of
   512 threads, `svc_tcp_spare_bytes_max` 67108864), loans `Y`.
2. **The KUnit suites from the RPM**, `../run-kunit.sh`: 7, 48, 6 and
   10 cases, no failure, no skip. On 474 the 48 ran only from a swapped
   module.
3. **`tcp_sock_set_window_clamp()`, the one change no swap could
   try**: it is in vmlinux. The held buffer's clamp now goes through
   it rather than the `TCP_WINDOW_CLAMP` socket option, under the same
   lock to the same `tcp_set_window_clamp()`, so nothing is expected
   to change. Check it: no "svc: TCP receive window clamp failed" in
   `dmesg` after a run, and the `svcsock_tcp_rx_hold` trace event
   giving the same clamp for each held transport as on 474 (a 384 KiB
   buffer on two ports, 1 MiB on one); then the rates below.
4. **The new baseline, one pool and everything built in**: two ports
   at the two-port setting and one port on eight drives, three runs
   each, with `memcost-log.sh` beside them, as the build 474 battery
   (`$L/b474/battery.sh`, step 4a). 474's figures with four pools:
   59.18 to 59.76 on two ports, 46.0 to 47.6 on one, about 2.2 and 1.4
   bytes through memory for each byte. These runs are also the
   comparison with four pools; a difference is the pools' or the
   build's, not to be put right by bringing the pools back.
5. **The bound built in**: `drgn spare-count.py 300 0.5` beside one
   two-port run; at most about 2,080 pages, 0 once the clients are
   gone.
6. **The loan plans**, `16k-42` to `16k-44` through `sweep.sh`, as on
   473 and 474, if the copy runs show anything moved.

Read `drives.txt` of every run. If the drives step down as they did on
15 October, trim them, or run without nvme2 and nvme3, before reading a
rate.

The A/Bs once proposed here for `svc_tcp_rx_keep` and the 50 us linger
were run on the evening of 15 October, and both were dropped from the
series ("15 October, evening: keep and the linger, each on and off").

## Build 479: the next kernel, one pool and everything built in (16 October)

Build 479 (`7.3.0-0.rc7.3.hs.479.loanpages_pool.el9.aarch64+16k`) is
the first kernel built from the branch after 15 October: `modinfo -p
sunrpc` lists `svc_tcp_spare_bytes_max` and none of `pools_per_node`,
`svc_tcp_rx_lowat`, `svc_tcp_rx_copy_threads`, `svc_tcp_rx_linger_us`
and `svc_tcp_rx_keep`, and `tcp_sock_set_window_clamp` is in
`/proc/kallsyms`. The runs are in `$L/b479/` (drivers `battery.sh`,
`after.sh`, `trim-ab.sh`, `trim-one.sh` and their `-slow` copies, each
with its `.log`) and `$L/b479-<tag>`, their lines in
`$L/b479/results-twoport-479.tsv`. Every audit came back at the
settled setting, and the host was left at it.

- **Step 1, the boot**: as written; `settled.sh apply` ended in "the
  settled setting" with loans `Y`, one pool of 512 threads and
  `svc_tcp_spare_bytes_max` 67108864.
- **Step 2: the KUnit suites from the RPM pass**, 7, 48, 6 and 10
  cases with no failure and no skip (`$L/kunit-479.log`). The 48 now
  run from the installed modules.
- **Step 3, in part**: no "svc: TCP receive window clamp failed" and
  no WARNING or BUG in `dmesg` after all the runs. The
  `svcsock_tcp_rx_hold` clamps were not compared with 474's.
- **Step 5: the bound holds built in.** `spare-count.py 300 0.5` beside
  a two-port run: at most 2,080 pages against a bound of 4,096 (64 MiB),
  0 before the run and 0 once the clients were gone, as on 474.
- **Step 6, the loan plans: not run.**

**Step 4: copy mode.** The battery's commands from 474 (two ports at
the two-port setting, one port a and b on all eight drives,
`memcost-log.sh` beside each) were run three ways: with the harness's
rest (`REST=30`), with all eight drives trimmed first, and trimmed with
`WARM=10 RUNTIME=30 REST=120`. Only the third gave runs no drive
stepped down in. Memory is given as in the 474 table: bytes through
memory for each byte of payload, the two-port figure halved from the
log's.

| | GB/s, no drive stepped down | GB/s, a drive stepped down | bytes through memory | build 474 |
|---|---|---|---|---|
| two ports | 58.04 (rest 30); **58.58** (rest 120) | 50.09, 51.87; trimmed 57.54, 57.86, 44.21; rest 120 46.14, 51.58 | 2.19, 2.23 (no drive stepped down) | 59.18 to 59.76; 2.17 to 2.23 |
| one port a, eight drives | **45.88, 47.42** (rest 120) | 46.51, 45.33, 46.81; trimmed 43.55, 46.77, 45.82; rest 120 40.77 | 1.43 to 1.47 | 47.62 clean; 1.38 to 1.44 |
| one port b, eight drives | **47.79, 46.34, 47.80** (rest 120) | 47.13, 45.88, 45.87; trimmed 44.66, 44.69, 46.26 | 1.46 to 1.50 | 47.18 clean; 1.42 |

- **One port has not regressed**: five clean runs at 45.88 to 47.80
  GB/s, the wire at 46.3 to 48.3 in the read window, 474's range.
- **Two ports: one clean run of 58.58**, 1 to 2% under 474's 59.18 to
  59.76; so was 58.04 with the shorter rest. One run each; the 1 to 2%
  is not established.
- **The memory for each byte is about 3% higher than 474's** on one
  port (1.43 to 1.50 against 1.38 to 1.44); on two ports 2.23 against
  2.17 to 2.23. Not looked into.
- **The drives**: a drive stepped down in every run but one with
  `REST=30`, trimmed or not, as early as 19 s; trimmed, abt-2's
  drives wrote 491 to 548 GB with one stepping down at 58 s, and after
  30 s of rest nvme0 and nvme1 stepped down at 19 s in abt-3. With
  `REST=120` and 30 s runs, five of six one-port runs and one of three
  two-port runs were clean. A trim takes about 1 s a drive (`fstrim -v
  /hs/nvmeN`). For a rate on this host, use `WARM=10 RUNTIME=30
  REST=120` after a trim.
- **abs-2 stalled on port a, not on its drives**: port a 12.14 GB/s
  and port b 34.00; port a's drives 38% busy, its wire 10.5 GB/s in
  the window, the 99th percentile of a write 4.8 s (121 ms on b), 11
  ring drops and 168 client retransmits. Once; not traced.
- **The client is shared.** At 13:51 another user's sweep
  (`/root/lp_sweep.sh`, `logperf` on the client's two ScaleFlux drives)
  was running on it, and the audit's ssh to 10.20.12.106 timed out, so
  the run refused (`$L/b479/trim-ab.refused.log`). Check the client
  is idle (`uptime`, no `logperf` or `fio`) before a battery.

**The harness after the tardis2 changes** (`b63b4018afd1`: the scripts
follow the host, `client_free`, the WRITE segment counts, `WRITES=`),
re-checked on 479 on tardis1, `$L/b479/harness-ab.sh` and its `.log`:
the drives trimmed, then three two-port runs at `WARM=10 RUNTIME=30
REST=120`, then the KUnit suites.

- **The harness works here**: `client_free` passed before each run,
  every audit came back at the run's setting, and after each run the
  loan budget was back at 0 and the nfsd tracepoint histograms were
  gone. Every WRITE segment was written direct (abh-1 2,221,424,
  abh-2 2,213,096; none dontcache or buffered).
- **Two ports**: 58.27 and 56.86 GB/s with no drive stepped down
  (ports 29.58 + 28.69 and 28.26 + 28.60; 2.23 and 2.28 bytes through
  memory for each byte), then 42.92 in abh-3 with three of port b's
  drives stepping down at 30 to 34 s. With 58.04 and 58.58 before,
  479 has four clean two-port runs, 56.86 to 58.58: about 2% under
  474's 59.18 to 59.76, steadily, though not established.
- **KUnit**: 7, 48, 6 and 10 cases, no failure, no skip
  (`$L/kunit-479-harness.log`); the server and the settled setting
  put back.

## Still Mike's to decide

Dropping the stacking device's DMA alignment (the loan harness's
dm-delay needs it), and the page_pool region's future upstream. Do not
act on these unasked, nor on anything step 6 of `../CODE-SERIES.md`
lists: it is a survey, not a decision. On 15 October, on Mike's word,
six were dropped: several thread pools per NUMA node, the copy outside
the socket lock, the SO_RCVLOWAT mark, veth's `rx_ip_align`, the
linger and keep (`../patches/dropped/`).
Ronaldo's author address is settled: his twelve commits carry
`ronaldo@slipstream-io.com` since 15 October.

## Left over from 14 October

- The walk and the builds ran in worktrees on nvme3, removed again
  that night: nvme3 holds only the harness's `zcbudget` and was
  trimmed. What is kept of them is in
  `/var/tmp/zc-run-logs/walk-73rebase/`: `walk.sh` (to walk again, give
  it a tree and its paths), its configuration, and every walk's
  summary (`run15-73rebase9.summary.txt` the last, 38 of 38 clean,
  and `kbuild-tip-result.txt` for the tip with and without KUnit).
- The rework's working worktree was in a session scratch directory
  under `/tmp`; if a reboot removed it, `git worktree prune`. Its
  branches stay: `…-code.73rebase1` to `.73rebase9` are the stages
  worth keeping; `…-code` and the `…-code.pre-*`, `.restructure*`,
  `.splits*`, `.msgs`, `.comments`, `.tcpif`, `.bvecmove-1`,
  `-reorder-1*`, `-reorder-2` and `-tcpif` branches are working copies
  Mike may delete.
- `sparse` v0.6.5-rc1 from git master is in `/usr/local/bin` (EPEL's
  0.6.4 fails the kernel's checker probe).

# 13 October, evening

Written at the end of 13 October on tardis1 (build 464, 16 KiB
pages) as the handoff, brought up to date late that night after the
loan measurements and the loan page, and on 14 October for the
rebase onto `v7.3-3`. Everything below this section is dated
history and says so; where it differs from this section, this
section is right.

## Where it stands

Copy mode, NFS WRITEs from the one client, durable, every run inside
the drives' fast region:

| | best run | runs of the setting | setting |
|---|---|---|---|
| two ports | **60.41 GB/s** | 59.96 to 60.41 in seven | `RCVBUF=393216 ENTRIES=128 LINGER=0 LOWAT=0 ./twoport.sh <out> ab` |
| one port, on all eight drives | **47.39 GB/s** | 46.5 to 47.4 in four | `FS_A="/hs/nvme0 ... /hs/nvme7" RUNTIME=60 LOWAT=0 ./twoport.sh <out> a` (`FS_B=`, `b` for the other) |

- Two ports: 384 KiB held buffer, four rings of 128 a port, no
  linger. One port: the values in `/etc/modprobe.d/sunrpc.conf` (1 MiB,
  linger 50) and four rings of 256. Both: `svc_tcp_rx_keep=1`,
  `svc_tcp_rx_lowat=0`, 8192-byte receive buffers at an MTU of 7792,
  512 nfsd threads on CPUs 8-55, the client untouched.
- **The setting, the CPU layout included, is `settled.sh`'s, and
  every run is audited against it (Mike, 14 October: "lock it down
  hard").** nfsd on CPUs 8-55 and nothing of the ports' there; port
  a's Rx-i on CPU 2i and Tx-i on 2i+1, port b's on 56+2i and 57+2i,
  each port's async interrupt with its Tx-0 (1 and 57); transmit
  steering (XPS) port a's tx-i to CPU 8+i and port b's to 32+i, the
  map every figure here was measured with; no RPS, NAPI not threaded,
  irqbalance kept off nfsd's CPUs and the card. `./settled.sh audit
  rest` checks it all, and the module settings, live; `./settled.sh
  apply` puts it back. `twoport.sh`, `sweep.sh` (each cell, under
  load), `iperf2port.sh` and `memcost-cal.sh` refuse to run off it,
  and `run-kunit.sh` restores it. Its header says what undoes each
  part and why XPS is held where it is.
- **!!! `irq_tuning.service` and `rps_xps_tuning.service` are MASKED
  (Mike, 14 October) and are TO BE RECONCILED with this setting. !!!**
  Hammerspace's E1 profile puts the rings on nfsd's CPUs (port a on
  8-31, port b on 32-55) with its own XPS map, and re-ran on every
  reload of `xeu`. Kept, with how to put them back, in
  `/root/hs-tuning-units-masked-2026-10-14/`; the card's MaxReadReq
  of 512, which `irq_tuning` set, `settled.sh` sets now. See
  `AGENTS.md`.
- 61.02 GB/s was carried once on two ports with `fq` on the client,
  which is no longer part of the setting and is worth nothing
  measurable.
- **The limit on two ports is memory.** The DIMMs are fully
  populated (Mike: four at 5200 MT/s, 166 GB/s in theory). A copy
  sends each byte through memory twice: the measured limit is a bare
  benchmark's 62 to 68 GB/s, 60.1 in frame-sized pieces. NFS on two
  ports is at that: 2.17 bytes through memory for each byte of
  payload, about 131 GB/s of the DIMMs' 166 at 60.4.
  Nothing that adds parallelism around the copy moves it.
- One port is at 95% of what its wire carries with no NFS (49.8).

## Copy mode on 7.3: build 473 (14 October)

The battery's regression check (step 3 under "Next"), on build 473
(`7.3.0-0.rc7.3.hs.473.loanpages_pool.el9.aarch64+16k`, the series on
`v7.3-3`), 12:41 to 13:04 by tardis1's clock. Each run at the setting
under "Where it stands", audited by `settled.sh` before it changed
anything and again just before its load, the client on `fq_codel` as
for build 464's comparison runs, and `memcost-log.sh` beside each. No
drive stepped down in any of them. The runs are `$L/b473-<tag>`, their
lines in `$L/b473/results-twoport-473.tsv`, the memory samples
`$L/b473/copymem-<tag>`.

| | build 473 | GB/s | bytes through memory for each byte of payload | build 464 |
|---|---|---|---|---|
| two ports | `ab-1`, `ab-2` | **59.27, 59.36** (ports 28.2 + 31.1, 28.7 + 30.7) | 2.25, 2.23 | 59.96 to 60.41 in eight runs on `fq_codel`; 2.17 (`cmn-two-1`, 60.38) |
| two ports | `ab-3` | 47.45, port b silent from second 25 (the eighth event, `../SILENT-PORT.md`) | | |
| one port a, eight drives | `a8-1` to `-3` | 46.77, 45.98, 47.59 | 1.44, 1.42, 1.46 | 46.5 to 47.4 (both ports' runs) |
| one port b, eight drives | `b8-1` to `-3` | **47.76**, 47.54, 47.06 | 1.48, 1.47, 1.46 | the same |

- **One port shows no regression on 7.3.** Its six runs (45.98 to
  47.76) cover 464's range (46.5 to 47.4), and `b8-1`'s 47.76 GB/s,
  at 1.48 bytes through memory for each byte, is the best one-port
  run on either build. It is the newest entry on the region budget
  page.
- **Two ports are about 1.5% under 464**, 59.27 and 59.36 against a
  mean of 60.24, with about 3% more memory traffic for each byte (2.25
  and 2.23 against 2.17). Two runs; not yet looked into. The figures
  divide the mesh's requests by the run's whole payload, as 464's 2.17
  does: `memcost-read.py` reads one port's payload, so its figure for
  a two-port run is that port's, and halving it is right only when the
  ports are even.
- What differs from 464's runs besides the kernel: the async
  interrupts on CPUs 1 and 57 (on 464 they sat on nfsd's 8 and 9),
  which fired no times, and nothing else `settled.sh` holds.
- **These runs copied with the socket unlocked**
  (`svc_tcp_rx_copy_unlocked=Y`, the default on 464 and 473). The knob
  is gone from the code after build 473 (Mike, 14 October; "SUNRPC:
  Copy with the socket unlocked only when records are claimed"): a
  kernel built from the tip copies under the lock unless
  `svc_tcp_rx_copy_threads` is 1 or more. On 464 the two carried the
  same (43.6 and 43.5 against 43.6, one port), so the next build's
  figures should match these; `settled.sh` reports the knob as not in
  that kernel rather than failing on it.

## The loan on 7.3: build 473 (14 October)

The battery's step 4 on build 473: `plans/16k-42-loan-memcost.plan`,
`16k-43-loan-memcost-small.plan` and `16k-44-loan-memcost-floor.plan`
through `sweep.sh` on the stand-in, as written out in
`../LOAN-PERFORMANCE.md`, 13:04 to 14:03 by tardis1's clock, with
`CELL_BOUNCE=1` for all three (on build 464 for the third only, so the
first two plans' footprints are not compared). Every cell was audited
once its load was running, against its plan line, and passed. The runs
are `$L/xeu-16k-473-loanmem-1` to `-3`, the memory samples
`$L/loanmem-473-1` to `-3` and their readings `$L/loanmem-473-<n>.read.txt`.

| rings, held buffer | WRITEs let in | build 473: GB/s | bytes through memory for each byte of payload | ring drops | build 464: GB/s, bytes, drops |
|---|---|---|---|---|---|
| 256, 1 MiB | 16 | 11.74 | 4.01 | 0 | 11.74, 4.07, 359 |
| 256, 1 MiB | 8 | 11.72 | 2.99 | 0 | 11.74, 2.99, 269 |
| 256, 1 MiB | 4 | 10.65 | 2.37 | 0 | 10.59, 2.28, 297 |
| 256, 1 MiB | copy | 10.44 | 5.97 | 0 | 10.35, 5.75, 6,203 |
| 128, 384 KiB | 8 | 11.73 | 2.14 | 1 | 11.73, 2.00, 646 |
| 128, 384 KiB | 4 | 8.45 | 1.11 | 0 | 8.40, 0.93, 207 |
| 128, 384 KiB | copy | 10.49 | 5.11 | 5 | 10.41, 5.02, 17,966 |
| 128, 256 KiB | 8 | 11.66, 11.70 | 1.86, 1.78 | 0, 0 | 11.72, 1.86, 203; 11.71, 1.89, 221 |
| 128, 256 KiB | 6 | 9.90 | 1.42 | 0 | 10.28, 1.39, 162 |
| 128, 256 KiB | 4 | 7.44 | 1.01 | 0 | 7.45, 0.80, 120 |
| 128, 256 KiB | copy | 10.41 | 4.96 | 0 | 10.41, 4.90, 171 |
| 128, 192 KiB | 8 | 11.50 | 1.72 | 0 | 11.55, 1.70, 154 |
| 128, 128 KiB | 8 | 10.74 | 1.68 | 0 | 10.81, 1.62, 121 |
| 64, 256 KiB | 8 | 11.63 | 1.42 | 13,509 | 11.60, 1.36, 64,803 |
| 64, 192 KiB | 8 | 11.46 | 1.38 | 3,216 | 11.43, 1.34, 19,809 |
| 64, 128 KiB | 8 | 10.48 | 1.24 | 1 | 10.74, 1.32, 7,463 |
| 64, 128 KiB | 6 | 8.24 | 0.79 | 0 | 8.12, 0.77, 865 |
| 64, 128 KiB | copy | 10.48 | 4.72 | 0 | 10.47, 4.71, 7,898 |

- **The loan on 7.3 is build 464's, cell for cell**: the same rates,
  its cost in memory falling with what it holds to a floor of 1.24
  (1.32 on 464), and its copy at 4.7 to 6.0 whatever it holds. The
  edges are where they were: fewer than 8 WRITEs let in costs rate, a
  128 KiB held buffer about 8%, and rings of 64 with a larger held
  buffer drop frames by the thousand. The best setting that loses
  nothing is still rings of 128, a 192 to 256 KiB held buffer and 8
  WRITEs let in, at 1.7 to 1.9.
- **Every loaned WRITE was written direct**, 9.70 million in the 15
  loan cells (`wr_direct`, none buffered or `wr_dontcache`), **and none
  held more than it was charged** (`overrun` 0).
- **Fewer frames dropped at the rings** in most cells than on 464,
  none over 5 at rings of 128. One run of each cell on each build; not
  looked into.
- On the loan page, "NFSD Loan Performance", from 14 October.

## Reads and mixes: the 7.1.13 baseline (build 464, 14 October)

Taken before the boot into 7.3 so that it can be laid beside it:
`./rwbase.sh <build>` runs the same loads (about 25 minutes a round,
two rounds), each through `twoport.sh`, whose `RW=`, `RWMIX=` and
`RWTSV=` are new for it. A run that reads first writes its files
whole (16 GiB a port) and rests 20 s. nfsd serves READs direct
(`io_cache_read` 2), so they come from the drives, not the server's
page cache. 16 writers a port at a queue of 16, 512 MiB files.
Both rounds' lines are in `results-rwbase-464.tsv`; each run is
`$L/rwbase-464-saved/rwbase-<tag>-<round>`, except round 1's 8 MiB
READ, 1 MiB random READ and 4 KiB random READ, which build 473's
first round overwrote (their lines in the table and the `.tsv` are
all that is left of them). `rwbase.sh` now names each run
`$L/rwbase-<build>-<tag>-<round>`.

**A restart of nfs-server puts nfsd on all 64 CPUs.**
`hammerspace-nfsd-tuning` puts nfsd's threads on CPUs 8-55 at boot,
and a stop and start of nfs-server (`run-kunit.sh` needs one) brings
up 512 threads on every CPU, among the ports' ring CPUs. `systemctl
restart hammerspace-nfsd-tuning` puts them back, starting nfs-server
if it is stopped; `run-kunit.sh` now does that itself, and
`settled.sh` refuses a run until it is so.

Two ports at the two-port setting, four drives a port:

| load | round 1 | round 2 | of which READ / WRITE | completion, mean |
|---|---|---|---|---|
| READ, 8 MiB sequential | 52.72 GB/s | 53.71 | | 81 ms |
| READ, 1 MiB random | 49.08 | 38.61, port a silent | | 13 ms |
| READ, 4 KiB random | 1.16 M IOPS (4.76 GB/s) | 1.16 M (4.75) | | 0.44 ms |
| 70/30, 8 MiB sequential | 55.68 | 56.03 | 38.9 / 16.8 and 39.2 / 16.9 | 80 / 89 ms |
| 70/30, 1 MiB random | 54.72 | 54.69 | 38.3 / 16.4 both | 9.5 / 10.6 ms |
| 70/30, 4 KiB random | 944 k IOPS (3.87 GB/s) | 951 k (3.89) | 661 k / 283 k and 666 k / 285 k | 0.49 / 0.67 ms |
| WRITE, 8 MiB sequential | 54.90 | 57.99 | | 83 ms |

One port over all eight drives, at the one-port setting:

| load | round 1 | round 2 | of which READ / WRITE |
|---|---|---|---|
| READ, 8 MiB sequential | 37.51 GB/s | 36.90 | |
| 70/30, 8 MiB sequential | 31.59 | 36.38 | 22.1 / 9.5 and 25.4 / 10.9 |

- **Large reads are not limited by the drives.** On two ports each
  drive reads 6.5 to 6.8 GB/s at 61 to 67% busy. On one port it is
  4.6 at 50%, and the port sends 36.8 GB/s, well short of the 47.4
  it takes in WRITEs. Not looked into.
- **4 KiB is limited by nfsd's CPUs**: CPUs 8-55 are 97 to 98% busy
  in every 4 KiB run. The drives read 0.6 GB/s each and show 99%
  busy, but for an NVMe drive that only means something was always
  outstanding; the script's "DRIVE-LIMITED" mark there is not the
  limit.
- **The two WRITE runs** (54.90 and 57.99) are below the 60.0 to
  60.4 of 13 October's seven runs at the same setting. They ran
  after the mixes, with the usual 30 s of rest; every drive stayed
  inside its fast region and none stepped down. Not looked into.
- **The one-port 70/30 mix varies** by 15% between its two rounds,
  for no reason seen in the drives (46 to 50% busy).
- **Port a went silent** in round 2's 1 MiB random READs, from 22 s
  into what fio counted to the end: the 1 MiB figure to lay beside
  is round 1's. This is the sixth event, the first on a read load
  and the first with the client on `fq_codel`; see
  `../SILENT-PORT.md`.
- The mixes drop a few frames at the card (up to 84 at the FIFO and
  128 at the rings in a window) and the client retransmits 100 to
  360 segments in one; the READ-only loads drop none.

## Next: the same battery on the v7.3-3 rebase (Mike, 13 October)

**The series is rebased onto `v7.3-3`** (`kernel-7.3/main`, after
7.3-rc7), on the branch `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`,
done on 14 October, and tardis1 booted build 473 from it that
morning (`7.3.0-0.rc7.3.hs.473.loanpages_pool.el9.aarch64+16k`).
Its KUnit suites pass: 7, 46, 6 and 10, the same as on 7.1.13.

**Every run on build 473 so far is discarded and is to be done again
(Mike, 14 October)**, kept in `$L/b473-discarded/` with a README.
None of it was at the settled setting:
- the KUnit run restarted nfs-server, which put nfsd's 512 threads
  on all 64 CPUs, among the ports' ring CPUs, until 10:28 (the ring
  CPUs spent 13 to 18% in nfsd, and the 1 MiB random READs carried
  44.1 GB/s against 464's 49.1);
- the svcsock suite left `svc_tcp_rx_rcvbuf` at 0 (fixed in the
  suite, which now puts back what it changes; the installed one does
  not, and `run-kunit.sh` writes the setting back after it), so
  every one-port run and every loan cell after it held no buffer;
- each port's async interrupt sat on one of nfsd's CPUs, and the
  transmit steering was not checked by anything.
`settled.sh` (under "Where it stands") was written for it: every
script that runs a load now audits the host against the settled
setting and refuses to run off it.

Run the KUnit suites with `run-kunit.sh` (it leaves the host at the
settled setting and says so), then "The battery" at the end of
`../LOAN-PERFORMANCE.md` from its first step, and lay each figure
beside build 464's, which are in that table:

1. `./memcost-cal.sh $L/memcost-cal-<build>`: the card and the drives
   by themselves (about 6 min, no NFS).
2. `./rwbase.sh <build>`: the READs and mixes above, beside build
   464's (`results-rwbase-464.tsv`).
3. Copy mode on two ports, three runs, and on one port over all
   eight drives, three for each port: the command lines under
   "Where it stands" above. **This is the check for a regression**:
   60.0 to 60.4 GB/s on two ports and 46.5 to 47.4 on one on build
   464. `memcost-log.sh` beside a run and `memcost-read.py` after it
   give its bytes through memory for each byte of payload (2.17 on
   two ports). **Done on build 473**: "Copy mode on 7.3" above. One
   port shows no regression; two ports are about 1.5% under.
4. The loan and its copy on the stand-in:
   `plans/16k-42-loan-memcost.plan`, `16k-43-loan-memcost-small.plan`
   and `16k-44-loan-memcost-floor.plan` through `sweep.sh` with
   `CELL_BOUNCE=1` and `memcost-log.sh` beside each, as written out
   in that document. Take port b down and up once first (`ip link
   set enp164s0f0 down; ip link set enp164s0f0 up`, then
   `./settled.sh place b`). A sweep now ends with port a's rings back
   at 256 and its layout in place, and writes `audit-end.txt`.
   **Done on build 473**: "The loan on 7.3" above. The loan is
   build 464's, cell for cell.
5. **The silent port** (Mike, 14 October): chase it on this kernel,
   after the four above. See "Set aside" below and
   `../SILENT-PORT.md`. **7.3 shows it**: the seventh event, on port
   b in round 2's 70/30 8 MiB mix on build 473 (12:17:50, 21 to 23
   s, the same signature as the sixth); see "The seventh" there.

Before any of it on a new boot: the "After a boot" line under the
rules, `getconf PAGESIZE` (16384), and `./settled.sh apply` ending in
"the settled setting" (it checks `xeu`'s settings, the MTU, the
module settings and the CPU layout; a BAD item names what is off). The
mesh's counter is `arm_cmn/hnf_mc_reqs/`: check that the new kernel
still has it (`ls
/sys/bus/event_source/devices/arm_cmn_0/events/hnf_mc_reqs`, and
`perf stat -a -e arm_cmn/hnf_mc_reqs/ -- sleep 1` reading a few
million idle), since every memory figure rests on it.

## What a loaned byte costs in memory: done on build 464 (13 October, evening)

The account is `../LOAN-PERFORMANCE.md`, and the page is "NFSD Loan
Performance", https://claude.ai/artifact/1hAkQK1xDiqPr1efdv4ZDb
(source `../artifacts/loan-performance.html`).

- **The measure is bytes through memory for each byte of payload**
  (the mesh's requests to the memory controller, 64 bytes each,
  over the payload carried). Copy mode on two ports is 2.17.
- **The card and the drives move data through the cache.** One port
  receiving 49.6 GB/s uses 0.03; the drives writing data still in
  the cache use 0.09, and 0.58 for data that has left it. So a
  loaned byte uses about 0.1 if it is still in the cache when a
  drive takes it and about 1.6 if not.
- **A loan is not bounded by memory as a copy is.** A copy's limit
  is quoted only as measured, the bare benchmark's 62 to 68 GB/s;
  no figure derived from the DIMMs' 166 (Mike).
- **On the stand-in the loan's use falls with its footprint**: 4.07
  at about 37 MiB, 2.99 at about 24, 1.89 at 23 with rings of 128,
  and a floor near 1.3 at 17 to 18 MiB with rings of 64, all at
  11.4 to 11.7 GB/s, where its copy uses 5.75 to 4.71. The floor is
  inside the cache and is the stand-in's own. Every loaned WRITE
  was written direct and none was over its charge. Fewer than 8
  WRITEs let in costs rate (a tenth at 6, a third at 4), a 128 KiB
  held buffer costs 7%, and rings of 64 drop tens of thousands of
  frames; the best setting that loses nothing is rings of 128, a
  192 to 256 KiB held buffer and 8 WRITEs let in.
- **A footprint** is what a load takes from the region beyond what
  was there idle plus its own port's posted rings. `CELL_BOUNCE=1`
  to `sweep.sh` empties the port's pools before each cell so that
  it reads true; the region's pages less its free ones is not it.
- "Dialing in loan mode" in that document is what to do when the
  SGL drives are here.
- **The stand-in** is NVMe/TCP on this host in front of a file on
  `/hs/nvme0`. It passes the data through TCP to itself, so its
  figures are not an SGL drive's; the difference between a loan and
  its copy is what to read. It leaves `nvmet`, `nvmet_tcp` and
  `nvme_tcp` loaded with nothing configured.
- The mesh counts two of its events at a time. Ask for two
  (`hnf_mc_reqs`, `hnf_cache_fill`), as the scripts do; with more,
  each is counted part of the time and scaled.

Not done, and not asked for: the loan on two ports, which `sweep.sh`
does not do. What a loan uses and carries in front of a drive that
takes the pages itself waits for the SGL drives.

**Writing about it:** the measure is "bytes through memory for each
byte of payload", and a footprint is as above. Mike asked for the
pages to be rewritten when a figure or a term changed, without
dwelling on what they said before; the loan page and
`../LOAN-PERFORMANCE.md` are written that way.

## The rules

- **A run stays inside the drives' fast region**: about 625 GB a
  drive from its last idle of 20 s. `twoport.sh` defaults to
  `WARM=15 RUNTIME=45` after `REST=30`; a traced run
  (`PROFILE=1`, `RUNTIME=55`) wants `WARM=10`. Read `drives.txt` of
  every run (what each drive wrote, and `no drive stepped down`)
  before its figure.
- **The drives are `write through`** (Mike: "never forget this").
  `./drive-cache.sh` prints it; `twoport.sh` applies it.
- **Nothing connected before a port is reconfigured or `xeu`
  reloaded**: `ss -tn state established '( sport = :2049 )'`.
- **The client is reached at 10.20.12.106** (`CLIMGMT`), not over a
  port under test.
- **Copy mode is not optimized at the loan's expense** (Mike): one
  frame to a buffer, pages returned last in first out, all inside
  the region.
- After a boot: `mount -o noatime /dev/nvmeNn1 /hs/nvmeN` for 0 to
  7, `./bringup.sh 32768 256` (the region and port a), `./port-setup.sh
  enp164s0f0 56` (port b), then `./settled.sh apply`, which has to
  end in "the settled setting".
- **Every run is audited against `settled.sh` and refuses to run off
  it** (Mike, 14 October). Do not get past a BAD item by hand or by
  a variable: put the host back with `./settled.sh apply` (or what
  the item names), and change the setting itself only in
  `settled.sh`, after a run that shows why.

## Decided on 13 October: do not undo from older text

| what | now | why |
|---|---|---|
| `svc_tcp_rx_lowat` | 0, in `modprobe.d` | 131072 halves wakeups on two ports and costs 2 to 3 GB/s; about 1 on one port |
| `fq` on the client | not set; the client keeps `fq_codel` | level with it over 19 runs; the 2 GB/s of 11 October was on flushing drives and runs past the fast region |
| ssh to the client | the management address | the harness's traffic was on port a |
| rings a port | four of 128 on two ports | eight of 64 are level in four layouts, never better |
| `svc_tcp_rx_copy_threads` | 0 | level on four rings, 3 GB/s down on eight, thousands of ring drops |
| the copy's last 4% | left alone | every way to it costs the loan |
| MD raid0 | not used, not compared | Mike |

## Set aside, with what would reopen it

**The silent port**: a port carrying nothing for about 25 s while
the other carries on, five times by 13 October, on either port, all
with `fq`. Not the drives, the card or the server's software: the
client's transmit side for the port stops for about 1.4 s and `fq`'s
pacing then holds it at a trickle. **It showed again on 14 October**
(port a, in the READ baseline above), with the client on `fq_codel`
and lasting the 24 s to the end of the run, so pacing is not what
holds it. **It is to be chased on the 7.3 kernel (Mike, 14
October)**, after the battery under "Next": `../SILENT-PORT.md` has
the account and what to do, and `$L/silent-driver.sh` runs with both
ends sampled.

Noted and not looked into: port a alone drops 33 to 1,087 frames at
the card's FIFO in a one-port run and port b none; port a's link is
optical with 1.5 million bits a second corrected by FEC at the
client, port b's is 2 m of copper with none.

## Where each account is

| document | what |
|---|---|
| `../XEU_CONVERGENCE.md` | the dated accounts; 13 October's are "What a drive takes", "The 128 KiB mark off and on", "Eight receive rings again" and "The client's queueing again" |
| `../XEU-4-VS-8-RING.md` | where interrupts and CPUs sit with four rings and eight, the eight-ring layouts, copy threads |
| `../TRACING-NFSD-AND-XEU-MEMCPY-TIME.md` | the receive path traced at 60 GB/s, the memory's limit, where the copy's 4% goes and why it is left |
| `../SILENT-PORT.md` | the silent port |
| `../TARDIS1_COPY_PERFORMANCE.md` | the copy benchmark of 12 October |
| `../LOAN-PERFORMANCE.md` | what a loaned byte costs in memory: the counter, the card and the drives alone, the loan on the stand-in, and the battery for the next kernel |
| `../LOST-TIME-CHASING-SPENT-DRIVES.md` | what 12 October's slow drives skewed; closed |
| `results-twoport-464.tsv` | a line for every two-port and one-port run |
| the artifacts | "NFSD Receive Region Budget", https://claude.ai/artifact/3UQkMVoWW8p9A5ZASPnm3g, source `../artifacts/region-budget.html` (its `droids.gif` is published beside it and is not in the tree): copy mode. "NFSD Loan Performance", https://claude.ai/artifact/1hAkQK1xDiqPr1efdv4ZDb, source `../artifacts/loan-performance.html`: the loan. Both are open to anyone with the link (Mike). To change one from another session, read it first and publish to its link |

Tools new on 13 October, all in this directory: `drvchar.py` (a
drive's fast region), `stall-sample.py` and `stall-read.py` (both
ends of both ports five times a second), `copy-adjacency.bt` and
`ring-flow-runs.bt` (where the copy's pieces come from, and a
connection's runs of frames on a ring), `copybench.c` with `wr`,
`rd` and `RUN=`, `memcost-cal.sh`, `memcost-log.sh` and
`memcost-read.py` (bytes through memory for each byte of payload);
`twoport.sh` gained `LOWAT=`, `TXN=`, `NFSD_CPUS=`, `QDISC=fqnp` and
a `PROFILE=1` that reports by group of CPUs; `../run-budget.sh`
gained `STEP_SECS=` and `sweep.sh` `CELL_REST=`.

The host as left: no NFS connections, nothing exported, both client
ports on `fq_codel`, the drives `write through`, the live
`svc_tcp_rx_lowat` 0, budget 0, four rings of 256 on each port (both
taken down and up during the floor run, their interrupts placed
again), the stand-in's modules loaded and idle.

Build 464 was built from
`origin/kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY-7.1.13-21.hs.464.loanpages_pool.el9.aarch64+16k`;
at the end of 13 October that held all of that day's work. On 14
October the series was regrouped (code, KUnit interface, KUnit tests,
docs) and `origin/kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY` force-pushed
at `ea18d80f8bda`; the same series was then rebased onto `v7.3-3` as
`kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`, where work goes on.

# 12 October, 22:30, with 13 October's notes as they were added (history: the section above supersedes it)

Its figures for two ports (58.5), for one port on four drives (42.2)
and "two `sunrpc` settings pinned" are of 12 October; the command
lines below that pass `QDISC=fq` are of then.


**The drives are characterised (12 October, 21:07 to 22:30 by
tardis1's clock), and a run has to stay inside their fast region.**
A drive takes about 625 GB at full rate from its last idle of 20 s.
Past that it keeps up only while the writes do not stop: after any
pause, 0.3 s is enough, it is at 4 GB/s for 25 to 55 s. Rest was
never the matter (20 s is all a drive needs); the length of the run
was: 120 s on two ports is 1.28 TB a drive. "What a drive takes" in
`../XEU_CONVERGENCE.md`; `./drvchar.py` is the local probe.

- `twoport.sh` now defaults to `WARM=15 RUNTIME=45` and waits
  `REST=30` first: two ports stay inside the region. **One port on
  its own four drives needs `WARM=10 RUNTIME=25`; better, give it
  all eight** (`FS_A="/hs/nvme0 ... /hs/nvme7"`, `RUNTIME=60`).
- **Read `drives.txt` of every run**: what each drive wrote (marked
  past 600 GB) and `no drive stepped down`. `drives-sec.tsv` is
  every drive each second; `./drvsum.sh` summarises it.
- **Rerun that way, 60 s apart, no drive stepping down in fifteen
  runs: two ports 57.0, 58.0, 58.2 GB/s; one port on eight drives
  45.2 to 46.8 (93 to 94% of plain TCP); one port on its four 44.4
  to 45.2, the drives at 100%.** `$L/rested-driver.sh` ran them.
- **The 128 KiB mark is unpinned (13 October):
  `svc_tcp_rx_lowat=0` in `/etc/modprobe.d/sunrpc.conf`.** Off, two
  ports carry 60.0 and 60.7 GB/s and one port 46.5 to 47.4; at
  131072, 57.9 and 57.3, and 45.6 to 46.3. `LOWAT=` in `twoport.sh`
  sets it for a run. The account is "The 128 KiB mark off and on" in
  `../XEU_CONVERGENCE.md`. The artifact was brought up to date the
  same morning.
- **Eight receive rings were run again (13 October) and are level
  with four**: 8 x 64 carries 59.5 to 60.5 GB/s against 59.6 to 60.7
  for 4 x 128, with the Rx CPUs a third less busy. `xeu` takes only
  powers of two for a ring's depth. Four of 128 stays. "Eight
  receive rings again" in `../XEU_CONVERGENCE.md`.
- **`fq` on the client is unpinned (Mike, 13 October): the two-port
  setting leaves the client on its default `fq_codel`,** so nothing
  on the client is changed. By turns inside the drives' fast region:
  `fq` 59.4 to 60.7 (seven runs), `fq nopacing` 59.9 to 60.3,
  `fq_codel` 60.0 to 60.4. The 2 GB/s of 11 October was with the
  drives flushing and runs past the fast region. Do not pass
  `QDISC=fq`. "The client's queueing again" in
  `../XEU_CONVERGENCE.md`.
- **The scripts reach the client over the management network (13
  October): `CLIMGMT`, 10.20.12.106, in `common.sh`.** Every ssh and
  scp to the client goes there and not over a port under test.
  `CLIIP` and `twoport.sh`'s `CLI[]` stay the client's addresses on
  the ports, for what is exported to, mounted and pinged. The first
  two-port run that way, on the client's default queueing:
  `mgmt-two-1`, 60.32 GB/s.
- **The silent port** (a port carrying nothing for 25 s in about one
  two-port run in ten, five times by 13 October) is set aside again
  (Mike): chase it if it shows again. All five were with `fq`, whose
  pacing is what holds the port at a trickle once it has begun; none
  showed in 19 runs of the comparison. `../SILENT-PORT.md`, with
  `stall-sample.py` and `stall-read.py` to catch one at both ends.
- Nothing is left from `../LOST-TIME-CHASING-SPENT-DRIVES.md`
  (Mike, 13 October): raid0 is not compared again, and the silent
  port only if it shows again. The work now is two ports with more
  parallelism: `../XEU-4-VS-8-RING.md`. Each is now a comparison on a level
  floor. Where this section says a figure was "limited by the
  drives" or gives a range of two-port totals, that was this.

**There is no MD any more (Mike, 12 October evening).** Each data
drive has its own XFS on `/hs/nvme0` to `/hs/nvme7` (not in
`/etc/fstab`: `mount -o noatime /dev/nvmeNn1 /hs/nvmeN` after a
boot). `twoport.sh` exports and mounts a port's four and divides its
writers among them. All eight drives were formatted (`nvme format`)
that evening and are 4 KiB. Where this section says stripe, read the
port's four drives. Two ports that way: 52.6, 50.9, 50.4 GB/s, no
gain over the stripes; single drives go slow for a run (4.4 GB/s at
98% busy), a different one each time. Also tried that evening and
not gains: eight receive rings on CPUs of their own, nfsd's threads
split by port.

**The drives are run as enterprise storage (Mike: "never forget
this").** Linux sees `write through` on the eight data drives and
both stripes, so no cache flush is sent; the drives' caches stay on
and the export stays `sync`. Check with `./drive-cache.sh` before
any run (`twoport.sh` and `sweep.sh` run it; a udev rule sets it at
boot, with the drives' autonomous power states off). Before this,
every COMMIT (one per 8 MiB write of the job) flushed all four
drives of a stripe and held one port at 36 GB/s.

**Two ports: 58.5 GB/s at best** (seven runs with no flush sent,
45.0 to 58.5), same setting as before (61.0 on 13 October with the
mark off; `QDISC=fq` was part of it until that day and is not now):

    RCVBUF=393216 ENTRIES=128 LINGER=0 LOWAT=0 ./twoport.sh <out> ab

**One port on four drives: 42.2 at best, and limited by the drives
in every run** (six runs, 18.1 to 42.2), each of them past the
drives' fast region (above). Read `drives.txt` in a run's
directory, or `drvbusy_a`/`drvbusy_b` in the TSV, before believing a
one-port figure. `twoport.sh` warms up for 15 s (`WARM`; 40 when
these were run) before fio counts. The best one port has carried is
now 46.8, on all eight drives (above).

**The copy is at about 90% of what the machine can copy** (62 to
68 GB/s memory to memory, reached with 8 CPUs). A copy that does
not fetch its destination gains nothing: `copybench.c`, and
`../TARDIS1_COPY_PERFORMANCE.md`. Next: what sets that ceiling
(memory channels populated?), and the loan. The 12% of cycles in
wakeups and context switches has not been traced.

**Two `sunrpc` settings pinned on 12 October, both in
`/etc/modprobe.d/sunrpc.conf`:** `svc_tcp_rx_keep=1` (a thread that
finds a WRITE short does not wake another for data already there)
and `svc_tcp_rx_lowat=131072` (a thread is woken when 128 KiB of a
short WRITE has arrived, not for each piece). Together they take
thread wakeups on two ports from 1.15 million a second to 0.40
million. **The second was taken off on 13 October: it costs 2 to 3
GB/s on two ports and about 1 on one.** Splitting nfsd's threads by port was tried
and is not a gain. All in `../XEU_CONVERGENCE.md` from "nfsd's
threads split by port".

The client is on 7.3.0-0.rc7.2.hs.469, which changed little. The
account is "No cache flush to the drives, and two ports at 58.5
GB/s" in `../XEU_CONVERGENCE.md`.

# On build 464: start here (2026-10-11, 10:00)

## 1. The baseline is done: 43.0 and 42.5 GB/s

Build 464 carries the tip and is level with 463 (42.5 and 42.7) at the
pinned copy setting: `rx_buf_size=8192`, MTU 7792, rings of 4 x 256,
`rx_refill_thresh=8`, a 1 MiB held buffer, 16 connections, `PLACE=1`,
plan `plans/16k-35-rxbuf8192.plan`, run by
`$L/xeu-16k-464-baseline-driver.sh` (`RUNS="5 6"` for two more). The
account is "Build 464: the baseline" in `../XEU_CONVERGENCE.md`.

**The svcsock KUnit suite used to leave the factor fixed at 150** in
the loaded `sunrpc`, and every window after it was 348 KiB where it
should be 487 (41.6 and 40.4 GB/s, the first two runs of this boot).
It is fixed at the tip, and the fixed `svcsock_kunit.ko` is installed
in 464's `updates/`, so `../run-kunit.sh` on this build is safe to
measure after. It was not seen before 10 October because the harness
wrote `svc_tcp_rx_credit_factor` for every cell, which put back
whatever the suite had left; the parameter's removal took that away.
The check, if in doubt: `rcv_ssthresh` in a run's `ss-server.txt` is
487424 or 483497 here.

**The state of tardis1 this boot:** `sunrpc` built from the tip in
464's `updates/` (it has the hold tracepoint; the packaged one is
beside it), `pools_per_node` 4, nfsd at 512 threads, a
32768-page region bound, rings of 4 x 256, `/dev/md0` on `/hs/stripe`,
`kernel.panic` 10, nfsd's threads on CPUs 8-55. About 52 TB written
to the stripe. The hand-built
modules in `/lib/modules/7.1.13-21.hs.463…/updates/` are still there
and belong to 463 only.

## 2. TODO, next: what limits the drain (Mike, 2026-10-11)

The saved traces of the 42.7 GB/s run
(`$L/xeu-16k-463-tipcheck-2`) say the limit is how fast the server
drains each connection: the client's sixteen connections were
`rwnd_limited` for 94% to 98% of their busy time. The copy is 25% of
server cycles. What else is there, before it and after it:

**Step A is done (2026-10-11): "What limits the drain, step A" in
`../XEU_CONVERGENCE.md`.** Use `perf report --no-inline` on that file;
it is quick.

- `arch_counter_get_cntpct` is the linger's wait loop, not the
  tracing. The cycles are a thread waiting, and a cheaper clock would
  free nothing.
- A connection copies about two fifths of the time and waits in the
  linger on an empty socket about one fifth. The copy's own speed is
  not all that holds the drain: the window and the round trip to
  refill it are in it.
- The copy is done with the socket locked (trace and code). Arrivals
  go to the backlog and a reply waits for the lock under the send
  mutex.

**Step B is done (2026-10-11): "What limits the drain, step B" in
`../XEU_CONVERGENCE.md`.**

- Eight rings of 128 on 8192-byte buffers: 42.2 and 41.9, no better
  than four (43.0 and 42.5), with the rings' CPUs 58% idle, fifty
  times the ring drops and a third more system time. The rings' CPUs
  are not the limit.
- **Do not set `napi_defer_hard_irqs` or `gro_flush_timeout` on the
  port: it stops receiving** and only a down and up brings it back.
  For Xsight.
- `svcsock_tcp_rx_hold` (in the hand-built `sunrpc` now loaded) shows
  what each transport is held to.

**The copy is made outside the socket lock (2026-10-11)**, at the tip
and in the loaded module: 43.6 and 43.5 GB/s against 43.6 with
`svc_tcp_rx_copy_unlocked=N` on the same module, so no gain by itself.
It is the first step towards two threads on one connection. "The copy
outside the socket lock" in `../XEU_CONVERGENCE.md`. **Pin every nfsd
thread to 8-55 after `echo 512`:** with that the baseline is 43.5,
not 42.5 to 43.0.

**Two threads on one connection (2026-10-11)**:
`svc_tcp_rx_copy_threads` 1 or 2, at the tip and in the loaded module,
default 0. 43.6 to 44.0 GB/s against 44.2: no gain, though the client
is held by the window less (85% of the time against 96%). Retransmits
rise four to ten times. "Two threads on one connection" in
`../XEU_CONVERGENCE.md`. **Everything tried on the server's drain
today lands between 43.5 and 44.6**; the client has not been looked at
since. Two runs of ten had about 360 RTOs and 34 to 35 GB/s, not
explained.

**A larger held buffer with claiming (2026-10-11)**:
`plans/16k-40-claim-1536k.plan`, `16k-41-claim-2m.plan`. Claiming
gains nothing at 1.5 MiB or 2 MiB (43.9 to 44.1 against 44.3 to 44.6
without). The client is held by the window 30% to 59% of the time
instead of 96% and the rate does not move; ring drops and retransmits
rise a hundred times. **The window is not the limit; look at the
client next.** "A larger held buffer, with and without claiming" in
`../XEU_CONVERGENCE.md`. The pinned setting stays at 1 MiB.

**The client looked at (2026-10-11)**: `client-look.sh` (run on the
client during a cell: counters 20 s apart and a 10 s profile). **The
client puts 355 to 363 Gbit/s on the wire, 92% to 94% of the 48.4 GB/s
iperf3 put on it; about 3.5 GB/s is all that is left.** It is not the
limit: every socket has megabytes unsent, a WRITE waits 11.6 ms for
its connection, a third of its CPUs are idle and no thread is near a
whole CPU. It copies what it sends (16% of its cycles; the RPC
client's send does not splice pages). "The client" in
`../XEU_CONVERGENCE.md`.

**fq on the client (2026-10-11)**: tried at 1 MiB, 1.5 MiB and 2 MiB
and put back. Ring drops and retransmits fall to a tenth to two
fifths; the rate is 42.0 to 42.2 at 1 MiB, 43.2 to 43.6 at 1.5 MiB,
44.2 to 44.4 at 2 MiB, so no gain. A paced sender waits on the window
more. "fq on the client" in `../XEU_CONVERGENCE.md`. **44.2 to 44.6
GB/s is the ceiling by every route tried: window, loss, or pacing.**
The client's `mq` has no handle: `tc qdisc replace dev ens3np0 root
handle 1: mq` first, and `tc qdisc del dev ens3np0 root` puts
`fq_codel` back.

**More than two threads (2026-10-11)**: `svc_tcp_rx_copy_threads` 3, 4
and 8 give 43.7 to 44.3, as 2 did. **The runs with hundreds of RTOs
(34 to 40 GB/s) have all been with 0: three of nine at 1 MiB, none of
twelve with claiming.** Not settled, and what happens in such a run
is the open question on the server. "More than two threads" in
`../XEU_CONVERGENCE.md`.

**0 and 2 by turns, six each (2026-10-11, evening)**: 43.9 against
44.2 GB/s on average, inside the spread; no RTO episode in either, or
in fourteen watched runs before. **Keep `svc_tcp_rx_copy_threads` at
0 on one port.** Its retransmits are explained: **the card loses
frames ahead of its rings and counts them nowhere.** One cell of each
accounted for packet by packet: the client's PHY sent 234,702 more
than the rings forwarded or dropped with 2 threads, 5,153 with 0, and
the client retransmitted 240,733 and 10,492. Read a setting's loss
from the client's retransmits, not from the ring drops. For Xsight:
where is a frame dropped ahead of `fwd`, `tail_drop` and `pp_drop`?
The note to Xsight is drafted, not sent:
`../xsight-uncounted-loss-email.txt`, this first and the smaller
questions after it (deferral, moderation, the Toeplitz hash, a filter
to a ring). The SDK has `xsl_rm_eu_rx_port_counters_get()` with
per-priority `tc_drop_packets`, which `xeu` never calls. The older
`../xsight-steering-email.txt` predates the RSS table patch and is
superseded by it.

**The uncounted loss is the port's RX FIFO (2026-10-11, 20:30).**
kmod-xsight 2.0.1-7 (kernel-rpmtree `7a06a06`) is installed and
loaded: with `echo Y > /sys/module/xeu/parameters/rx_port_stats`
(default N), `ethtool -S` lists `port_rx_fifo_tc<N>_drop_packets`
and the FIFO's fill. Accounted packet by packet, the FIFO dropped
13,038 frames in a cell with 0 threads and 234,332 with 2, which is
what was missing (11,445 and 233,507). By the SDK's sizing it is
2 MiB for this port, 268 frames, 46 microseconds at 44 GB/s, and
that is all there is: there is nothing to resize it to. The link is
clean (`xsllink get --fec-hist port=0`: `NCCW: 0`).
**The ceiling near 44 GB/s is that FIFO overflowing in bursts, with
flow control off.** Next: `ethtool -A` on both ends so that it
pauses the sender, and whether it can be made larger (for Xsight).
"The uncounted loss is the port's RX FIFO" in
`../XEU_CONVERGENCE.md`. **Flow control was tried and does not
reach the wire**: with `ethtool -A enp161s0f0 tx on` (and `rx on`
on the client) the client received no pause frame in four cells and
the FIFO went on dropping. **`fq` on the client with the FIFO
counted (22:00)**: its drops fall to an eightieth at 1 MiB (167) and
the rate is 42.2; with every flow capped (`fq maxrate 23gbit`) the
FIFO is never used and the rate is 38.7 with the rings dropping, so
the bursts are not what holds the rate ("fq on the client again" in
`../XEU_CONVERGENCE.md`). `rx on tx on` with the port remade stops
the rings dropping and the FIFO drops it all. For Xsight. The FIFO
is 2 MiB, measured (16327 lines of 16384 at its fullest). After a reload of `xeu`, `bringup.sh`
refuses because the region still shows: run its steps by hand (that
section says which). `rpm -ivh --force` and `rpm -e --justdb` of the
old release install a kmod without removing other kernels' kmods.
"0 and 2 threads by turns" in `../XEU_CONVERGENCE.md`. Do not kprobe
every received segment (`gro-order.bt`): it makes the rings drop.

**The tuning service sets 512 threads and pins them all** (a drop-in,
`hammerspace-nfsd-tuning.service.d/threads.conf`). No more `echo 512`
and `taskset` by hand; a restart of the service does both. It still
leaves `io_cache_write` at 4, which no cell has measured.

**The pinned module settings are in `/etc/modprobe.d` (2026-10-11,
19:00)**: `xeu.conf` gained `rx_refill_thresh=8` (no more setting it
by hand after a load), and a new `sunrpc.conf` has `pools_per_node=4
svc_tcp_rx_rcvbuf=1048576 svc_tcp_rx_linger_us=50`. `sunrpc` was
reloaded from it (`SUNRPC_KO=none ./swap-sunrpc.sh`, no clients
connected) and came up with all three, 512 threads in pools of 128,
all on 8-55. They hold for every port and every NFS connection.
`run-budget.sh` now puts the linger back as it found it after a run,
where it wrote 0. Not in a file yet: the port's MTU of 7792, its
rings and region.

**Two stripes of four drives (2026-10-11, 22:40), for runs on both
ports**: `/dev/md0` (`nvme0n1` to `nvme3n1`) on `/hs/stripe` and
`/dev/md1` (`nvme4n1` to `nvme7n1`) on `/hs/stripe1`, raid0 with a
1 MiB chunk, XFS, `zcbudget` in each. `/hs/nvme0` and `/hs/nvme1`
were wiped for it (Mike) and are out of `/etc/fstab` and
`/etc/exports`. After a boot: `mdadm --assemble --scan`, then `mount
-o noatime` each. Locally each takes 41 to 44 GB/s, alone or
together, **which is about what one port carries: run the pinned
setting again on four drives before comparing two ports with one.**
"Two stripes of four drives" in `../XEU_CONVERGENCE.md`.

**Two ports, the best setting found (2026-10-12, 01:35): 54 GB/s**
(seven runs, 48.1 to 54.2, middle 54.0), with the two ports' receive
memory in 21 MiB of the 32 MiB L3:

    RCVBUF=393216 ENTRIES=128 QDISC=fq LINGER=0 ./twoport.sh <out> ab

384 KiB held buffer, rings of 4 x 128, linger 0, `fq` on the client
(`fq_codel`: 52). **`fq` is no longer part of it (13 October): the
client keeps its default `fq_codel`, which carries the same.** The one-port setting on two ports is 43 (48 MiB).
**It is not the one-port setting: one port alone with it carries 31,
against 36 with 1 MiB and 4 x 256.** Why two ports did not add up:
with receivers that discard without copying, plain TCP carries 99.4
GB/s on the two ports (`MODE=trunc ./iperf2port.sh ab`); with a copy,
86.5; and a memory-to-memory copy on this machine tops out near 62
GB/s. The copy is what the ports share. Every run is in
`results-twoport-464.tsv`; the account is "Two ports: why they did
not add up" in `../XEU_CONVERGENCE.md`. Open: port b runs 2 to 4 GB/s
under port a when both are loaded, and what the smaller memory saves
is not explained by the cache counters.

**Two ports (2026-10-11, 23:40): 36 GB/s each alone on four drives,
and 43.3 together (21.6 each), with each port's FIFO dropping 870
thousand frames in 70 s.** No gain over one port on six drives, and
what the shared limit is is not known: iperf3 to both ports at once
and a profile are next. `./port-setup.sh enp164s0f0 56` gives the
second port the pinned settings and puts it in the region (this boot
only), `./ring-irqs.sh <port> <cpu>` puts its rings' interrupts on
56-63 (the driver puts every port's rings on 0-7, so two ports would
share them; redo after a down and up or an `ethtool -G`), `./twoport.sh <out> a|b|ab` runs either port or both.
"Two ports" in `../XEU_CONVERGENCE.md`.

**`io_cache_write` 2 is pinned (2026-10-11, 19:20)**: one cell with 4,
which the tuning service's profile set, carried 28.6 GB/s against 44:
nfsd syncs every 1 MiB WRITE where the client's COMMIT syncs 8 MiB.
The service's drop-in now passes `--io-cache-write 2`, so a restart
or a boot leaves 2. "nfsd's write mode" in `../XEU_CONVERGENCE.md`.
The bundle's own `e1` profile (`HS_IO_CACHE_WRITE=4` in
`/usr/local/sbin/hs-tune-common.sh`) is unchanged: the fix is this
host's drop-in only. Changing the default there is for later (Mike,
2026-10-11); do not edit the profile until he asks.

**Noted.**

- 12% of lingers time out (287,671 of 2.37 million), 50 us of a
  thread each and a hand-off of the connection.
- A quarter of direct writes take over 1 ms (147 thousand of 619
  thousand; most take 130 to 500 us) and 52 thousand take 4 to 8 ms.
  XFS's inode lock and log are in the lock table. With 512 threads and
  the client waiting on the window, this is not the limit now; it
  would matter once the drain is faster.

# The xeu rig at 16 KiB: where it stands (2026-10-10, 12:50, on build 463)

**Build 463 is running, and both new `xeu` features work on the
card.** The account is "Build 463: the RSS table and `rx_buf_size` on
the card" in `../XEU_CONVERGENCE.md`. In short:

- **The RSS table** (`rss_indir_size=128`, now in
  `/etc/modprobe.d/xeu.conf`): `ethtool -X` moves flows between
  rings, and bits 13 to 19 of a flow's hash pick its table entry, so a
  flow is placed by setting that entry (`ynl` `rss-set`, with
  `--no-schema`; `rss-balance.py` builds the request
  from `xeu-rx.bt`'s output).
- **Two bugs in the RSS patch were fixed** and the kmod rebuilt and
  loaded (`xeu` srcversion `9B0770879836A992C43D41D`, still Release
  6, from `$L/kmod-xsight-16k-463-r6fix`): setting the key had
  stopped the hashing, and the firmware's own table could not be
  read. **A kmod built before kernel-rpmtree `b32b6c9` has both
  bugs: do not use `ethtool -X hkey` on it.**
- **`rx_buf_size=5440`** receives a frame in two buffers and three
  fifths of the region pages (1360 to 1376 against 2168 to 2376), and
  dropped no frames where the whole page dropped 3557 and 127510. The
  rate does not separate the sizes in two runs each (41.0 and 36.6
  GB/s against 41.4 and 38.3).
- The `ethtool` 6.15 command cannot set the hash function on this
  port; `ynl` can. A flow's hash cannot yet be computed from its
  ports.

**The state of tardis1** (it does not outlive a reboot, except the
modprobe file and the installed kmod): `xeu` loaded with
`rx_buf_size=5440`, a 32768-page region bound, rings of 4 x 512,
`rx_refill_thresh` 8, `pools_per_node` 4, nfsd at 512 threads,
`/dev/md0` on `/hs/stripe`, `kernel.panic` 10. This boot wrote
about 18 TB to the stripe.

**`rx_buf_size=5440` is settled for now (Mike, 2026-10-10): no more
runs against the whole page.** What it gives is enough. The next work
is to settle the other settings around it: ring depth, the held
buffer, `rx_refill_thresh`, the linger, the pools. It is pinned:
`/etc/modprobe.d/xeu.conf` has `rx_ip_align=2 rss_indir_size=128
rx_buf_size=5440`, and `xeu` was reloaded with it (rings of 4 x 512,
the region bound again, `rx_refill_thresh` 8). The artifact
was republished on 2026-10-10 with a first section on build 463 and
the call with Xsight (`id="xeu463"`, `id="call"`).

**`PLACE=1` places the flows (2026-10-10, 14:25):** the harness sets
the RSS table so each ring carries a quarter, in place of
`BALANCE=35`. Proved under NFS in one cell: 23/21/36/20% as the flows
fell, 25.0/25.0/25.4/24.6% in the cell, 39.9 GB/s. Use it for every
cell from here ("Flows placed on the rings under NFS" in
`../XEU_CONVERGENCE.md`).

**Ring depth and the held buffer (2026-10-10, 16:05):** on 5440-byte
buffers, placed, two runs each: **4 x 512 with a 1 MiB buffer carries
41.8 and 40.7 GB/s with no drops, in 28.5 to 30 MiB**; 685 KiB on the
same rings 39.4 and 39.6; rings of 4 x 256 drop 105 to 880 thousand
frames. The rings were left at 4 x 256 by the last cell: `ethtool -G
enp161s0f0 rx 512` before anything else. The account is "Ring depth
and the held buffer on 5440-byte buffers: the results" in
`../XEU_CONVERGENCE.md`.

**Eight rings did not help (2026-10-10, 17:10):** 39.6 and 39.3 GB/s
at 8 x 256 with the 1 MiB buffer against 4 x 512's 41.8 and 40.7; the
rings' CPUs went from 9% idle to 60% to 70% idle and the region in use
went past the L3 (32 to 33 MiB). With the 685 KiB buffer eight rings carry 39.8 and 39.8, four
rings' rate. The rings' CPUs are not the limit. **The port is back at
4 rings of 512 entries and nfsd's threads on CPUs 8-55.** A 1 MiB
WRITE takes about 360 us to receive on its connection, one thread at
a time, so sixteen connections top out near 46 GB/s: 32 connections
(`STEP_MOUNTS=2`) were tried and the client fell over: 12 GB/s with
the client's CPUs 80% in system time and the server 88% idle. Mike
stopped the queue. **Do not run two mounts from this client again
without looking at the client first** ("Thirty-two connections" in
`../XEU_CONVERGENCE.md`; it also records a page pool that has not
finished shutting down since the 8-to-4 ring change).

**The client's kernel changes (Mike, 2026-10-10, about 18:40):** he
is rebooting 192.168.1.106 from `6.12.76-2.hs.212.el8` into
`v7.1.13-21.hs.460.el9`, which is built on the same `v7.1.13-21` as
tardis1's kernel. Every figure before that was taken with the 6.12
client. On the new client kernel, first run `plans/16k-32-best.plan`
once as its baseline (41.8, 40.7 and 41.1 GB/s on the old one), then
`./conns-watch.sh 2 <outdir>` to see whether two mounts still
collapse. `kernel-rpmtree` was copied to the client at
`/root/snitm/git/kernel-rpmtree`.

**The best setting is now 8192-byte buffers with the MTU at 7792
(2026-10-10, 21:15): 43.2 and 43.3 GB/s** in 24 to 26 MiB, against
5440's 41 to 42 in 27 to 30. One buffer to a frame, so it is also the
setting a loan should try first. `plans/16k-35-rxbuf8192.plan`, run
with `HW_ANY_MTU=1`; **Both are pinned (Mike, 2026-10-10, 21:40):**
`/etc/modprobe.d/xeu.conf` has `rx_ip_align=2 rss_indir_size=128
rx_buf_size=8192`, and NetworkManager's `enp161s0f0` profile has MTU
7792 (`nmcli con mod enp161s0f0 802-3-ethernet.mtu 7792`). The region,
the four rings of 256 and `rx_refill_thresh` 8 are still set by hand
after a boot or a reload. `rx_buf_size` is the driver's, so the other
port, `enp164s0f0`, has 8192-byte buffers too; its MTU is still 9000,
where a frame takes two of them.

**The client keeps MTU 9000, and that is safe.** Each end of a TCP
connection advertises its MSS from its own MTU in the SYN, and a
sender never sends more than the peer's. The server advertises 7752
(7792 less 40), so the client's segments fit a buffer whatever its
own MTU is. Read on the client during a run (`ss -tin dst
192.168.1.211`): `mss:7740` (7752 less 12 of TCP timestamps),
`advmss:8948`, `pmtu:9000`. It is per connection and settled before
data moves, so nothing is fragmented and no path MTU discovery is
involved; the server's own frames are at most 7792, which a
9000-byte port takes; the client's traffic to other hosts is
untouched. **It covers TCP only**: `ping -s 8500` from the client to
the server gets no answer (with or without DF), where `ping -M do -s
7764` does. NFS and its mount here are over TCP. Anything that sends
the server a single non-TCP packet over 7792 bytes will lose it. The account is "One
buffer to a frame" in `../XEU_CONVERGENCE.md`, which also says that
`where.sh`'s per-ring GB/s were a tenth too high before this.

**The loan is written direct on the 8192-byte buffers (2026-10-10,
21:25):** on NVMe/TCP, all 2.5 million loaned WRITEs direct, 20.9
GB/s against its copy's 25.4 at 0.61 CPU-seconds per GB against 1.43,
and no crash. It is held by its budget, charged as if a frame had a
whole page: set the factor for two frames to a page before the next
loan run ("The loan on 8192-byte buffers is written direct" in
`../XEU_CONVERGENCE.md`).

**`svc_tcp_rx_credit_factor` is gone (2026-10-11, 00:20).** A
transport's window is held by what its skbs are measured to occupy
(`hold_factor` in the budget's statistics: 107 here) and opened to
half of what its held buffer holds; a budget learns a loan's charge.
The copy at the pinned setting: 42.5 and 42.7 GB/s, against 42.4 and
42.0 the same night on the code before with its factor at 190. **The
rig is about 1 GB/s under its 43.3 of 21:00 whichever code runs**, not
explained. The sunrpc of the tip is loaded from `updates/`
(`954D242F33480A3301D26E3`). "The factor is gone" in
`../XEU_CONVERGENCE.md` has the six commits and the five batches.

**The kernel learns a loan's charge now (2026-10-10, 22:30).**
"SUNRPC: Learn a loaned record's charge from the records":
`svc_tcp_rx_credit_factor` 0, the default, has each budget keep the
most pages a record held. The sunrpc built from the tip is loaded
from `updates/` (`7ED891B5AFE050EE947DD94`). A loan cell is run with
`KERNEL_CREDIT_FACTOR=0`; the harness still writes its own factor for
copy cells ("The charge for a loan" in `../XEU_CONVERGENCE.md`).

**Where the search for the rest stands (2026-10-10, 19:45).** The
copy carries 41 to 42 GB/s of the wire's 48 at the best setting
(`plans/16k-32-best.plan`), on the 6.12 client and on the 7.1.13-21
one. Tried since and not a gain: eight rings (the rings' CPUs were
not the limit), a load paced at the source and stepped up, and 32
connections from two mounts (37.0 GB/s at 1 MiB a connection, 35.7 at
512 KiB with the footprint held). A 1 MiB WRITE takes about 350 us to
receive on its connection, of which the copy is about 150. A 1.5 MiB
held buffer gave 41.4: less waiting and a dearer copy (38 MiB in use,
`memmove` 38%), which cancel. Not tried: a shorter or longer linger,
`rx_refill_thresh`, one pool against four; none is expected to move
the rate.

**The loan written direct waits for drives, not for the card (Mike,
2026-10-10).** Sample NVMe drives that support SGLs are coming to
tardis1. NVMe/TCP was used as the stand-in so that everything a loan
needs for storage that takes arbitrary pieces is in place when they
arrive. What the card will not do is header-data split or payload
aggregation: Hammerspace raised header-data split with coalescing
with Xsight, who showed no interest in delivering it, and
`../xeu-hds-experiment/` is not being pursued. So on the drives
tardis1 has today, which need whole pages, a loan goes through the
page cache; on the SGL drives it can be written direct if every piece
is on a 4-byte boundary, which is the TODO below. For the copy past
41 to 42 GB/s what is left is how one connection's WRITE is received
(one thread at a time, about 350 us for 1 MiB).

**Not done from the list below:** 5440 at the old
setting of 256 entries and a 1 MiB buffer; and everything in step 3
(the loan cell that crashed tardis1, the linger's values, the pools,
4 x 128, the region resize test). Step 0's crash log cannot be read:
the journal holds only the running boot.

**From build 462, still standing:** the whole stack carried 38.0 GB/s
at the median of three traced runs there (36.7 to 38.3; "Build 462:
the whole stack of changes" in `../XEU_CONVERGENCE.md`). **Step 6 of
"The boot after build 461", what each change is worth, waits (Mike,
2026-10-09): do not start it unasked.** Step 7 has not been run.

## The boot into the next kernel: the new xeu features first (planned 2026-10-09, late)

**Mike is building a kernel from the tip of this tree (`…-WIP3`, to be
pushed to hs-gitlab) with kmod-xsight 2.0.1-6 (kernel-rpmtree
`xsight-WIP3`, 8c58863).** On that boot, test the two new `xeu`
features first: where flows land on the rings (the RSS table) and
receive buffers smaller than a page (`rx_buf_size`). Mike wants to
see both working. Everything else in this file comes after.

What that kernel has, so none of it needs building or swapping:
- the sunrpc fixes, built in: the stack-overflow fix, its KUnit test,
  the `BUILD_BUG_ON`, and the linger tracepoint;
- kmod-xsight 2.0.1-6, with `rss_indir_size` and `rx_buf_size`
  beside `rx_refill_thresh` and `rx_ip_align`.

The `updates/sunrpc.ko` of build 462 is under that kernel's own
`/lib/modules`, so it does not load on the new one.

**Before the reboot (Mike):** add `rss_indir_size=128` to the
`options xeu` line in `/etc/modprobe.d` that has `rx_ip_align=2`. Then
the first boot has the table, and the RSS tests need no reload of
`xeu`.

### 0. First checks (minutes)

- `uname -r` is the new kernel and `getconf PAGESIZE` is 16384.
- `rpm -q kmod-16k-xsight` is 2.0.1-6.
- `modinfo xeu | grep -E 'parm: *(rss_indir_size|rx_buf_size)'`
  shows both.
- `cat /sys/module/xeu/parameters/{rx_ip_align,rss_indir_size,rx_buf_size,rx_refill_thresh}`
  reads 2, 128, 0 and 32.
- `ls /sys/kernel/tracing/events/sunrpc/svcsock_tcp_rx_linger` exists.
- `sysctl kernel.panic=10`, so a panic reboots and does not hang.
- The crash: `journalctl --list-boots`, then `journalctl -k -b <the
  boot that crashed>`. It is a stack-protector panic, or a bad PC,
  under `svc_tcp_rx_release_refs` or `page_pool_put_netmem_bulk`, if
  the crash section below has the cause right.
- `../run-kunit.sh`: svcsock 44/44 with
  `svcsock_rx_page_pool_bulk_release_test`. Before any load, with
  nfs-server stopped; afterwards restore nfsd as "How the module was
  put in" below says.
- `mount -o noatime /dev/md0 /hs/stripe`, `./bringup.sh 32768 256`,
  `echo 512 > /proc/fs/nfsd/threads`. **Run nothing on veth.**

### 1. Ring placement (the RSS table): about an hour, no reload

The tools:
- `./rings.sh 5`: each ring's GB/s, share, frames, and the tail drops.
- `timeout -s INT 5 bpftrace xeu-rx.bt`: each flow's hash (the card's)
  and the NAPI, which is the ring, it landed on; the buffers' truesize
  and frags. It fires on every frame, so the rate while it runs is a
  traced rate.

A load to watch: 16 TCP streams to the port. Run `iperf3 -s -B
192.168.1.211 -D` here, and `iperf3 -c 192.168.1.211 -P 16 -t 60`
on the client, as in 5b of `../XEU_CONVERGENCE.md`. Then the harness's
copy cell, for NFS.

1. `ethtool -x enp161s0f0` before setting anything: the firmware's own
   table. If it fails, `dmesg` names the first entry that is not one of
   the port's rings. Either way, record what it shows. It answers the
   question of how big the hardware's table is.
2. Under load: `rings.sh` and `xeu-rx.bt` give the baseline, the
   shares as they fall and which flow is on which ring.
3. `ethtool -X enp161s0f0 weight 1 0 0 0`. **Everything should move to
   ring 0** (`rings.sh`, under load). That shows the table is used.
   - If only part moves, the hardware indexes fewer entries than
     were set. Reload with a smaller `rss_indir_size` (64, 16, 4).
     The reload is Mike's.
   - Then `weight 0 1 0 0`, and `equal 4` to spread it again.
4. `ethtool -X enp161s0f0 hfunc toeplitz`; `ethtool -x` then shows it
   with the key. In `xeu-rx.bt` the flows' hashes change. Then check
   whether **the hash's low bits pick the entry**: for each flow in
   `@flow`, take hash mod 128, look that entry up in `ethtool -x`, and
   see whether it names the ring the flow is on. If it does for every
   flow:
   - a flow's ring can be chosen;
   - Toeplitz over the 4-tuple with the shown key gives the hash, so
     a flow's entry can be computed from its ports;
   - this is one of the questions to Xsight, answered.
5. If 4 holds, balance by giving each flow's entry a ring of your
   choosing. The `ethtool` CLI (6.11) sets only weights, not single
   entries. 7.1's netlink `rss-set` takes the whole table as an
   `indir` array, and the in-tree `ynl` client sends it. Nothing new
   is needed:

       cd <the kernel tree>
       tools/net/ynl/pyynl/cli.py --spec Documentation/netlink/specs/ethtool.yaml \
           --do rss-set --json '{"header": {"dev-name": "enp161s0f0"}, "indir": [0, 1, 2, 3, ...]}'

   The array is `rss_indir_size` (128) ring numbers, one for each
   entry. To move one flow, read the table with `ethtool -x` (or
   `--do rss-get` with the same header), change that flow's entry,
   and send the whole table back. Then check with `rings.sh` and
   `xeu-rx.bt` that the flow moved and nothing else did. `ynl` needs
   Python 3 with PyYAML on tardis1, which has not been checked. With
   sixteen flows on four rings the aim is four flows to a ring.

Record it all as a new section of `../XEU_CONVERGENCE.md`.

### 2. `rx_buf_size`: one reload, then the A/B

1. Reload `xeu` with pieces of a page. This is Mike's: it drops
   `enp164s0f0` for a few seconds. **Watch this reload:** it may be
   the first time a pool bound to the region is destroyed on a kernel
   with the LIFO stack. Destroying the pool drains the stack in
   `page_pool_empty_ring()` ("page_pool: Recycle a region-bound pool's
   pages last in, first out"). On build 462, `xeu` was reloaded for
   kmod release 5, but the docs do not say whether a region was bound
   then. Have `kernel.panic=10` set (step 0). Afterwards, check `dmesg`
   for warnings from `page_pool` and that `alloc_fail` is still 0.

       echo none > /sys/module/page_pool/parameters/rx_region_dev
       rmmod xeu && modprobe xeu rss_indir_size=128 rx_buf_size=5440
       ./bringup.sh 32768 256

2. `dmesg | grep rx_buf_size` should show: "5376-byte RX buffers, 3 to a
   page, up to 2 to a 9000-byte MTU frame".
3. **See it work.** Under load, `xeu-rx.bt` should show `@truesize`
   10752 with 1 frag for full frames, where a whole page showed 16384
   with 0. Then compare the region's `used_pages` for the same copy
   cell against the control. Expect about 2/3.
4. Then the A/B in "Receive buffers smaller than a page" below:
   - 5376, twice, now;
   - 4096, twice, after another reload;
   - the default, twice, as this boot's control, after a reload
     with `rx_buf_size` left out.

Read every run's ring shares before comparing rates, or run with
`BALANCE=35`.

### 3. Then, in this order

1. The loan cell again, which crashed tardis1, with and without the
   linger. Reading its `rx_pp_recycle_ring_full` and `alloc_slow`
   decides the page-pool items.
2. "Is the linger worth its CPU in copy mode" (50, 25, 20 and 0 us,
   with `linger.bt`).
3. "Does `pools_per_node` earn its place" (64 connections; each pool
   change restarts nfsd, which is Mike's).
4. The 4 x 128 cell ("The footprint against the L3").
5. **The region resize test.** "page_pool: Give the fixed receive
   region back when it is resized" has never run: it was queued after
   the loan cell when tardis1 crashed. It takes the port down, so it
   comes after the loads above. With the port down, write the new size
   (`echo 16384 > /sys/module/page_pool/parameters/rx_region_pages`),
   then bring the port up. `/sys/kernel/debug/page_pool_rx_region`
   should show the new size, and the kernel log should show the old
   region freed. Then `./bringup.sh 32768 256` puts the size back for
   any later run.

## tardis1 crashed in the loan cell (2026-10-09, evening)

(On the next kernel, which has the fix built in, step 1 of "On the
boot after the power cycle" below does not apply. The section above
has that boot's order.)

After the three runs above, the session started the loan cell, traced,
twice: once with no linger and once with `svc_tcp_rx_linger_us` 50. It
meant to run the region resize test next. **tardis1 crashed and did
not reboot itself; it was unreachable until a hard power cycle.** The
console was not read, and it is not yet known which of those runs was
going when it crashed.

**The likely cause is a stack overflow in the batched return** ("SUNRPC:
Return a loaned record's page-pool pages in batches", `bdd837db04f1`).
`svc_tcp_rx_release_refs()` handed `page_pool_put_netmem_bulk()` up to
64 pages at a time. That function collects the pages whose last
reference it drops into an on-stack `bulk[XDP_BULK_QUEUE_SIZE]` (16
entries) and does not check the bound. A loaned 1 MiB WRITE at 16 KiB
is 64 or more pages of one pool, and nfsd usually holds their last
references, so nearly every loaned WRITE wrote up to 48 entries past
the end of that array on an nfsd thread's stack. Only a loaned record
takes this path: every copy-mode run of build 462 had the patch loaded
and never ran it. The loan cell was the first run that did.

Three commits at the tip:

| commit | what |
|---|---|
| "SUNRPC: Keep page-pool return batches within the bulk API's bound" | the batch is `XDP_BULK_QUEUE_SIZE`; `Fixes: bdd837db04f1` |
| "SUNRPC: test releasing more page-pool pages than one bulk batch in the svcsock KUnit suite" | `svcsock_rx_page_pool_bulk_release_test`: 65 pages of one pool, held only by the record, all back in the pool's ring after the release |
| "SUNRPC: Refuse to build a page-pool return batch the bulk API cannot take" | a `BUILD_BUG_ON` in `svc_tcp_rx_release_refs()`: a batch over `XDP_BULK_QUEUE_SIZE` fails the build (checked: 64 does) |

`svcsock_kunit` passed 43/43 on the buggy module because no test
dropped a last reference inside a batch, so nothing was collected. The
new test passes on the fixed code. **Never run it against a sunrpc
from before the fix.** It is expected to overflow the stack there and
can take down whichever host runs it.

**On the boot after the power cycle:**

1. **The sunrpc with the bug is still installed.** It is
   `/lib/modules/$(uname -r)/updates/sunrpc.ko` (srcversion
   `988D31FBE6BF6DC88282CB6`) and loads again on boot. Copy mode does
   not reach the bug; a loan does. Before any loan cell, run
   `./build-sunrpc.sh` from the tip and `./swap-sunrpc.sh`, then
   `../run-kunit.sh`: svcsock should be 44/44.
2. Read the crash from the boot before: `journalctl -k -b -1` if the
   journal is persistent, or else the BMC console. A
   `__stack_chk_fail` panic, or a bad PC, under
   `svc_tcp_rx_release_refs` or `page_pool_put_netmem_bulk` confirms
   the cause. Anything else means it is still open: the linger, and
   the region resize ("page_pool: Give the fixed receive region back
   when it is resized"), are then the suspects.
3. `sysctl kernel.panic=10` before any load, so that a panic reboots
   the box instead of leaving it hung.
4. Then the loan cell's two runs again (no linger, linger 50), and the
   region resize test only after them.

The state below is from before the crash. A reboot resets everything
in it except the installed modules.

## Is the linger worth its CPU in copy mode: 50, 25, 20 and 0 us

What build 462 says so far (the whole stack, sixteen connections):

| linger | runs | GB/s | CPU-seconds per GB | calls per WRITE |
|---|---|---|---|---|
| 50 us | 3 | 38.3, 38.0, 36.7 | 0.52, 0.52, 0.51 | 2.1, 2.1, 2.3 |
| 0 (`xeu-16k-462-minus-linger`) | 1 | 37.2 | 0.43 | 6.9 |

The linger does its job: a WRITE is received in about 2 calls where it
took 7. But in copy mode it has not shown a gain in rate (37.2 is inside
the spread of the three runs at 50), and it costs about a fifth more
server CPU for each GB (0.52 against 0.43). Much of what it was meant
to save between threads, the page swap already saves: a record that
changes hands no longer frees and allocates its pages. And the copy is
held by the L3 and by how the flows fall onto the rings, not by the
server's CPU (38 GB/s at 0.52 is about 20 of the 64 CPUs). So in copy
mode the linger's worth is open. A shorter wait could keep the calls
near 2 at less CPU, or show that the linger is not needed in copy mode.
For the loan it is a separate question: a loaned record that changes
hands carries its loan table with it.

- **The cell:** `plans/16k-26-copybest.plan` (rings of 4 x 256, 1 MiB
  held buffer), the rest of the stack as on build 462, with
  `RX_LINGER_US` at 50, 25, 20 and 0. The linger is set for each run,
  so nothing needs a build or an nfsd restart and no client is dropped.
  50 is run again as this boot's control, and 0 is the cell that
  answers whether the linger is needed at all.
- **The order:** after the fixed sunrpc is in place. 50, 25, 20, 0, and
  then all four again. Eight runs of about 3 TB is 24 TB on the stripe:
  say so to Mike before the queue, and trim before each run.

      RX_LINGER_US=<us> PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/xeu-16k-<build>-linger<us>-<k> plans/16k-26-copybest.plan

- **What decides it:** `@calls_per_write` and `@write_in_calls_us` in
  `handoff-*.txt`, CPU-seconds per GB, `arch_counter_get_cntpct`'s share
  of the profile, and the rate. The best value is the shortest linger
  that keeps calls near 2, if the rate holds. If 0 runs as fast as the
  others at less CPU, the linger is off for copy mode.
- **How long each wait lasts, which picks the value directly.** "SUNRPC:
  Trace how long a thread lingers for the rest of a TCP record" adds
  the tracepoint `sunrpc:svcsock_tcp_rx_linger`. It fires once per
  wait, with the time waited, the limit, how much of the fragment had
  arrived, and why the wait ended: data, timeout, close or resched.
  `linger.bt` makes histograms of it by outcome, and `trace-sweep.sh`
  runs it for 8 s in every cell, into `linger-*.txt`. The cell's
  tracers now end at about 94 s of its 120. Read the waits that ended
  on data: the limit wants to sit just above most of them. Every
  timeout is a wait spent for nothing. The tracepoint is in sunrpc, so
  it comes with the module build of the tip. Neither the tracepoint
  nor `linger.bt` has run yet. With an older sunrpc, `linger.bt`
  fails to attach and leaves only its error in the file.

## TODO: 4-byte placement with `rx_buf_size` (work item; Mike, 2026-10-10)

**The most a loan can hope for on the E1 is 4-byte placement**, so
that a loaned WRITE goes direct to storage that takes arbitrary pieces
(NVMe with SGLs, NVMe/TCP). Header-data split and payload aggregation
are not coming from Xsight (above). **It matters on tardis1: sample
NVMe drives with SGLs are coming, and this has to be settled before
they are used for a loan.** The drives there today need whole pages
whatever the placement.

**`rx_buf_size=5440` breaks the placement `rx_ip_align=2` gave, and
it has to be revisited before any loan is run on these buffers.** The
driver says so at every open: "MTU 9000 spans 2 RX buffers; payload
after the first buffer is not 4-byte aligned".

- Every buffer is posted with the same headroom, 64 + 2 = 66 bytes,
  because any descriptor can be the first of a frame.
- In a frame's first buffer the payload starts at 66 + 54 = 120 (132
  with TCP timestamps): on a 4-byte boundary, which is what
  `rx_ip_align=2` is for.
- The rest of the frame starts at byte 66 of the next buffer: 2 past a
  4-byte boundary. So with two buffers to a 9000-byte frame, about
  half of every loaned WRITE's pieces fail the 4-byte gate. Only the
  offset fails: the lengths are multiples of 4 (5000 and 3960 bytes of
  payload in the two buffers; 4988 and 3960 with TCP timestamps,
  which both ends have on), and the three buffers of a page start at
  0, 5440 and 10880.
- With `rx_ip_align=0` it is the other way about (118 and 64): one of
  the two is off whichever is chosen, as long as both buffers are
  posted alike.

To work out, none of it started:

1. **Read, 2026-10-10: one misplaced piece sends the whole WRITE
   through the page cache.** `nfs_dio_split()`
   (`fs/nfs_common/nfs_dio.c`) walks every fragment of the payload in
   `nfs_dio_iter_aligned_and_splittable()` and returns at the first
   whose offset or length is not a multiple of the file's
   `dio_mem_align` (`(off | len) & (mem_align - 1)`). The split is then
   `NFS_DIO_MEM_MISALIGNED`: the payload is issued as one buffered
   segment, with `IOCB_DONTCACHE` since
   `nfsd_direct_misaligned_dontcache` is on by default, and
   `nfsd_write_dio_split` traces it. There is no partial path: the
   split divides a WRITE by file offset (a prefix, a direct middle, a
   suffix), never by which pieces of memory are aligned. Every loaned
   WRITE larger than one buffer has a continuation piece, so **on
   5440-byte buffers every loaned WRITE would be buffered**, not half
   of them.
2. Whether the driver can post a frame's later buffers at a headroom
   that is a multiple of 4. It does not know at posting time which
   buffer will be first; a short frame (an ACK, the tail of a burst)
   takes one buffer and shifts the order.
3. **The ask of Xsight, already mentioned to them (Mike,
   2026-10-10): that every buffer of a frame allows 4-byte
   placement.** It is the change that would help most, and a far
   smaller one than header-data split. One way to put it: the driver
   posts every buffer at a headroom that is a multiple of 4, and the
   card skips 2 bytes at the start of a frame only, so the IP header
   and the payload are placed in the first buffer and the rest of the
   frame starts on a boundary in the next. Today the 2 bytes are in
   the address the driver posts, so every buffer has them.
4. **The fallback that needs nothing, and is proven: 16 KiB pages
   with a whole page to a frame** (`rx_buf_size=0`), where
   `rx_ip_align=2` places every piece. Loans went direct that way on
   NVMe/TCP, the SGL drives' stand-in. It bloats the L3: 34 to 37 MiB
   of receive memory for the copy against 21 at 5440, and a loan
   holds more. So until 3 or 5 it is the loan on whole pages or the
   copy on 5440-byte buffers, by the storage.

5. **A frame that fits in one buffer** (thought of on reading the
   gate; run in copy mode the same evening: 43.2 and 43.3 GB/s, the
   best yet, and every frame one buffer. Run as a loan on NVMe/TCP the
   same night: every loaned WRITE was written direct). The misplacement is of a frame's second buffer, so
   it goes away if no frame has one. A buffer holds its size less 386
   bytes (66 of headroom, 320 of `skb_shared_info`): 5054 at 5440, and
   7806 at 8192, which is two buffers to a 16 KiB page. With
   `rx_buf_size=8192` and the server's MTU at 7792 (the client's can
   stay 9000: the server's MSS sets the segment size) every frame is
   one buffer placed by `rx_ip_align=2`. And it is denser than today:
   8192 bytes of buffer for 7752 of payload is 1.06 a byte, against
   10880 for 8960 (1.21) at 5440 and 16384 for 8960 (1.83) with whole
   pages. The price is 16% more frames for the same data. Worth a
   copy-mode cell for the footprint alone, before any loan.

Copy mode is not affected: nfsd copies the pieces out and its own
buffer is what is written.

## A known gap in the page swap: pages a socket keeps (work item)

Since the page swap (`47f13c685c5b5`), a connection keeps the pages a
thread gave it in exchange for a parked record. That is up to the
largest record that changed hands on it, about 1 MiB for nfsd, and it
keeps them until it closes. 16 MiB for this rig, about 16 GiB for a
thousand nconnect=16 clients. They are freed at close, so nothing
leaks, but nothing bounded the total.

**Bounded on 15 October**, on `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`,
in three commits: code, KUnit interface, KUnit tests. The pages kept
across all sockets are held to `svc_tcp_spare_bytes_max` (64 MiB;
0 keeps none), and past it a restore frees as it did before the swap.
The suite gains two tests (svcsock 46), which have not run. The
details, why freeing on a quiet connection would not have worked, and
what to read on tardis1 are under "sunrpc, the pages a socket keeps
after the page swap" in `../XEU_CONVERGENCE.md`'s "Since then".

## The page-pool work items: what was done (2026-10-09, evening)

T1 and T2 in `../XEU_CONVERGENCE.md` suggested three things for a pool
bound to the region. "Since then", after that document's list of
suggestions, has the account.

| item | state |
|---|---|
| a LIFO in place of the pool's FIFO ring | done: "page_pool: Recycle a region-bound pool's pages last in, first out", in build 462; run only in copy mode, with other changes, never alone |
| a much shorter ring (`xeu`'s `pool_size`) | not done; the LIFO was chosen instead. `rx_refill_thresh` is another knob |
| the DMA mapping kept while a page is in the region | not done; an overflowing page is still unmapped, put on the region's list and mapped again |

**What decides the rest is the loan cell, rerun on the fixed sunrpc**
(step 4 of the crash section). Read `rx_pp_recycle_ring_full` and
`alloc_slow` in `pp-*.txt` against T2's 4.06 million and 254 thousand
in 10 s. Near zero means the LIFO and the rest of the stack have dealt
with it. Still near 9% of returns means keeping the mapping on region
pages is the next change. It is in `net/core` and needs a kernel
build.

## Receive buffers smaller than a page: `rx_buf_size`, and its A/B

The work item, with the driver's code as it is, the change and the
sizes, is "Receive buffers smaller than a page" in
`../XEU_CONVERGENCE.md`'s "Since then". In short:
- `xeu` gets a load-time `rx_buf_size`;
- each receive descriptor takes one piece of a page-pool page, built
  in place as today;
- at 5440 bytes (5376 once rounded down to the page pool's 128-byte
  alignment) a 9018-byte frame takes 2 pieces, and a 16 KiB page holds
  3, so 2/3 of a page per frame where today it is a whole one.

**Written, built, not run** (9 October, evening):
`kernel-rpmtree/xsight/xeu-cut-RX-buffers-smaller-than-a-page-with-rx_buf_size.patch`,
Patch6 of `xsight.spec`, Release 6 (`kernel-rpmtree` 8c58863, on its
`xsight-WIP3` branch). The spec's `%prep` applies Patch0 to Patch6 in
order. Every object of the module that this host's 4 KiB config builds
compiles clean (`xeu_ipsec.c` and `xeu_psp.c` need config this host
lacks), with no new W=1 warnings. Besides the pieces, it counts the
whole frame (MTU plus Ethernet and VLAN headers) when it tells the
firmware how many buffers a frame may take, where the MTU alone could
come out one short. The open logs the size in use. To build: `build-
kmod.sh` (Release 6 has both patches); a size is set with `modprobe
xeu rx_buf_size=5440`.
Lowering the MTU tests nothing: the driver posts a whole page whatever
the frame. 9000 is the driver's maximum MTU, so it cannot be raised
either.

**The A/B.** The cell is `plans/16k-26-copybest.plan`'s copy cell, on
the whole stack with the fixed sunrpc, `BALANCE` on. Each
`rx_buf_size` is a reload of `xeu`. That takes `enp164s0f0` down for
a few seconds, so each reload is Mike's.

| `rx_buf_size` | ring entries | frames the ring holds | held receive buffer | the same data window |
|---|---|---|---|---|
| 0 (a page, the control) | 256 | 256 | 1 MiB | yes |
| 5440 (5376) | 512 | 256 | 672 KiB | yes |
| 4096 | 1024 | about 341 | 768 KiB | yes; more frames posted |

Every row holds the same frames and the same data in the receive
buffer. So what changes is the memory per frame, and what that does
to the rate. 672 and 768 KiB are 1 MiB scaled by each size's truesize
per frame (10752 and 12288 bytes against 16384). The ring size has to
be a power of two, so 4096 cannot be held at 256 frames.

The order: load 5440 and run the cell twice; load 4096, twice; load
the default, twice, as this boot's control. Six runs at about 3 TB
each is 18 TB on the stripe; trim before each. After that, if 5440
wins, run it once at the old setting (256 entries, 1 MiB) as well.
That is half the frames posted and more data in the buffer, to see
what the denser buffers allow and not only what they cost.

**What decides it:**
- the rate, at equal frames and equal data window;
- `used_pages`, expected near 2/3 of the control's at 5440;
- `memmove`'s share of cycles, and calls per WRITE (`handoff.bt`);
- CPU-seconds per GB, which carries the cost of twice the
  descriptors and of smaller GRO skbs;
- ring drops and `rx_pp_*`.

If 5440 is not faster and `memmove` is no smaller, the page per frame
is not what holds the copy, and the default stays. The size of a GRO
skb is not measured by anything here. If the CPU per GB rises, that
is the first thing to instrument.

## How the flows fall on the rings: three work items

The largest source of spread between runs of one cell is which ring
each flow hashes to. Build 462's slow run had one ring at 19.9 GB/s
and 440 thousand drops. **`xeu` supports no steering**: `ethtool -x`
and `-X` and `-n rx-flow-hash` were checked on build 461 and are not
there, and its one ntuple rule can only target a VF. The SDK built
into the module does have an RSS redirection table and key
(`xsl_rm_reta_set()`, `xsl_rm_rss_hash_set()`), which `xeu` does not
expose ("`xeu` gives no way to steer" and "rings, how the flows
fall" in `../XEU_CONVERGENCE.md`). So no `ethtool` rule pins flows
here yet. The
work items, all three also in that document's "Since then" list:

1. **More flows than rings.** At 64 connections the rings came out
   even (28, 27, 20 and 25%), where sixteen gave 25, 6, 31 and 37%.
   The 64-connection test below will show it again.
2. **Mount again until the rings are even: `BALANCE=<percent>`** to
   `run-budget.sh`, and so to `sweep.sh` and `trace-sweep.sh`. It
   applies to the hardware client's step cells with `STEP_JOBS`.
   Before the cell is measured, the harness:
   - mounts and runs the cell's own writers for `BALANCE_SECS`
     (default 4), on files of their own, deleted after;
   - reads each ring's `rx_bytes` from `ethtool -S`;
   - if the busiest ring took more than `<percent>`, unmounts and
     mounts again. Each mount gets new client source ports, so new
     hashes.

   It tries at most `BALANCE_TRIES` mounts (default 6). If none comes
   within the limit, the cell runs on the last mount and says so.
   Each try is a line of `<cell>/balance.txt`: the busiest ring's
   share, every ring's share, and the client's source ports. A try
   whose ports are the same as the one before is marked, since then
   remounting changed nothing. The tracers' 35 s count from the
   cell's start, after the balancing, as before. `geometry.txt` names
   the setting.

   For four rings `BALANCE=35` is a guess to start from: an even fall
   is 25%. Build 462's busiest ring carried 19.9 GB/s in the slow run
   and 14.5 in the fast one; the shares were not read. It does not make the
   flows even. It stops one bad fall from deciding a cell. **Not run
   yet:** that the client's ports change with each mount, and how
   many tries a balanced fall takes, are the first things to read.
3. **Steering in `xeu`**: a kmod change. **The first of it is
   written** (9 October, evening; built, not run):
   `kernel-rpmtree/xsight/xeu-expose-the-RSS-redirection-table-hash-key-and-function-to-ethtool.patch`,
   Patch5 of `xsight.spec`, Release 6 (`kernel-rpmtree` 799acf5,
   on its `xsight-WIP3` branch). It wires `ethtool -x`/`-X`
   to the SDK's existing calls:
   - the redirection table, sized by the new load-time
     `rss_indir_size` (default 0 means no table, as before; at most
     255);
   - the 68-byte key;
   - the hash function, of which only Toeplitz can be set. Spooky,
     the firmware's default, shows as unknown.

   It also adds `get_rx_ring_count`, without which 7.1's `ethtool` core
   refuses any table. What `ethtool -X` set is programmed again after
   every reconfiguration of the port.

   It applies at `-p0` after Patch0 to Patch4, and its four files
   compile with no new warnings (also at W=1) against the 7.1.13 tree.
   The whole module could not be built here: the vendor's
   `xeu_ipsec.c` needs XFRM offload, which this host's 4 KiB config
   lacks.

   **To try it on tardis1** (`build-kmod.sh` builds Release 6;
   loading it reloads `xeu`, so it is Mike's):

       modprobe xeu rss_indir_size=128     # after rmmod xeu
       ethtool -x enp161s0f0               # the firmware's table
       ethtool -X enp161s0f0 hfunc toeplitz
       ethtool -X enp161s0f0 weight 1 0 0 0   # everything to ring 0
       ethtool -X enp161s0f0 equal 4

   Read the first `ethtool -x`. If it fails, the warning in `dmesg`
   names the first entry that is not one of the port's rings: that is
   the end of the firmware's own table, or its handles are not the
   rings'. After `weight 1 0 0 0`, a short load should land on ring 0
   alone (`ethtool -S` ring counters as deltas). If it does not, the
   hardware indexes fewer entries than were set, so try sizes down to
   the ring count. The rest is the questions in
   `../xsight-steering-email.txt`.

   Still to ask for, in order:
   - the RSS indirection table and key (`ethtool -x`/`-X`). It is the
     cheapest, works with standard tools, and with the key readable it
     places a flow by moving its table entry;
   - ntuple filters (`ethtool -N ... action <ring>`), for two hot flows
     in one entry;
   - interrupt coalescing (`ethtool -C`). It is not steering, but it is
     missing too, and `enable_irq` was 6.1% of samples.

   aRFS and RFS would not help: sunrpc reads with `tcp_read_sock()`,
   which does not record a flow's CPU. The reasons, and the questions
   for Xsight (is there a table, how large, Toeplitz and its key,
   filters, moderation), are under "rings, how the flows fall" in
   `../XEU_CONVERGENCE.md`'s "Since then".

Without `BALANCE`, read the busiest and least busy ring of every run
(`where-*.txt`) before comparing rates. Set aside a run whose busiest
ring is far over a quarter, or say so with its figure.

## Does `pools_per_node` earn its place: 64 connections, 1 pool against 4

"SUNRPC: Let a NUMA node have several thread pools" was written for
T6 in `../XEU_CONVERGENCE.md`. With 64 connections and the one pool,
512 threads waited 5.25 s in 10 s on its queue of ready connections
(`__lwq_dequeue`). The build 462 runs had four pools but only sixteen
connections, and at sixteen that lock is not near the top of the list
on either build (S6). So nothing yet says the patch does its job. The
test is the load that showed the problem, run with 1 pool and with 4.

- **The cell:** the copy at its best setting, rings of 4 x 256 and a
  1 MiB held buffer (`plans/16k-26-copybest.plan`), with
  `STEP_MOUNTS=4`. That is four addresses of the server on
  `enp161s0f0`, each with nconnect 16, so 64 connections; the harness
  adds the addresses for the cell and removes them after. The rest of
  the stack is as for build 462: linger 50, `rx_refill_thresh` 8,
  deferred frees at 128. T10's 64-connection copy (21.1 GB/s) was on
  rings of 4 x 128 with a 256 KiB buffer and dropped 190 thousand
  frames, so it is not the figure to compare with. The comparison is
  between the two runs here.
- **The order:** after the sunrpc from the tip is in place (the crash
  section above). Changing the pool count is
  `systemctl stop nfs-server`, `echo <n> >
  /sys/module/sunrpc/parameters/pools_per_node`,
  `systemctl start nfs-server`, `echo 512 > /proc/fs/nfsd/threads`,
  then check `cat /proc/fs/nfsd/pool_threads` (n pools). The value is
  read only when nfsd's service is created. **Stopping nfsd drops
  every client, including the other one on `enp164s0f0`, so each
  change is Mike's.** Run 4 pools, then 1, then 4, then 1. At about
  3 TB a run that is 12 TB on the stripe; trim before each run
  (`TRIM=`).

      STEP_MOUNTS=4 RX_LINGER_US=50 PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/xeu-16k-<build>-m4-pools<n>-<k> plans/16k-26-copybest.plan

- **What answers it:**
  - In `perf lock contention`, `__lwq_dequeue`'s contended count and
    time waited with 1 pool against 4. 4 should cut it by about the
    number of pools. If it does not, the patch is not doing its job.
  - `/proc/fs/nfsd/pool_stats`, saved by hand before and after the
    sweep (the plan is one cell; the harness does not save it). Each pool's
    packets and wakeups show whether the 64 connections spread over
    the four pools. The pool is picked by a hash of the transport, so
    expect an uneven split, not an even one.
  - Then the rate, CPU-seconds per GB, drops and RTOs.
  - Judged as one change of many (see "How to judge a change here"):
    if the lock's wait time goes away and the rate does not move, the
    patch still did its job, and something else is the limit.

The state of tardis1 (it does not outlive a reboot, except where said):

- Kernel `7.1.13-21.hs.462.el9.aarch64+16k`. **sunrpc is the module
  built from the tip, in `/lib/modules/$(uname -r)/updates/sunrpc.ko`**
  (srcversion `988D31FBE6BF6DC88282CB6`; the kernel's own
  `sunrpc.ko.xz` is where it was). `svcsock_kunit.ko` and
  `xdr_kunit.ko` built with it are beside it. They stay across a
  reboot of this kernel; `pools_per_node` does not, being given to
  `modprobe` by `swap-sunrpc.sh` and in no file of `/etc/modprobe.d`.
- `pools_per_node` 4 (four pools of 128 threads; chosen by the
  session, Mike has not said 4 or 1), `svc_tcp_rx_linger_us` 0 until a
  run sets it, nfsd at 512 threads.
- `kmod-16k-xsight` 2.0.1-5 for build 462 is installed and loaded
  (the kmods of builds 459 and 461 went with the upgrade);
  `rx_refill_thresh` is **8**, `rx_ip_align` 2.
- A 32768-page region is bound to `0000:a1:00.0`, rings of 4 x 256,
  `alloc_fail` 0. `/dev/md0` is mounted at `/hs/stripe` by hand.
- This boot wrote about 12 TB to the stripe. No client other than the
  harness's was connected at any check.

How the module was put in: `./build-sunrpc.sh` (it now gives the
module BTF, without which `nfsdlat.bt` and `handoff.bt` do not
attach), then `POOLS_PER_NODE=4 ./swap-sunrpc.sh`, which stops the NFS
services, unloads sunrpc and what depends on it, installs and starts
them again. `../run-kunit.sh` wants nfs-server stopped and leaves it
so: afterwards `systemctl restart hs-nfsd-blksize; systemctl start
nfs-server; systemctl restart hammerspace-nfsd-tuning` and set the
threads. The module build warns that the compiler is not the one the
kernel was built with.

What is left open, by what the runs showed: how the sixteen flows fall
on the four rings (the one slow run of three had a ring at 19.9 GB/s
and 440 thousand drops); the linger's busy wait, 14% to 17% of cycles;
`memmove`, still a quarter of them; a 9000-byte frame with 16 KiB to
itself (now a work item: "Receive buffers smaller than a page"
above). The driver's `rx_refill_thresh` patch is Release 5 in
`kernel-rpmtree` (3bc8f82); Release 6 adds the RSS and `rx_buf_size`
patches (799acf5, 8c58863). Mike pushed them to the `xsight-WIP3`
branch of the kernel-rpmtree remote; `main` there does not have them.

**The footprint against the L3 is the next lever for the copy.** Build
462 used 2052 to 2291 region pages, 32 to 36 MiB, which is the whole
L3. On build 461 the copy's time was set by how much of the region was
in use: a 1 MiB WRITE's calls took 224 us with 24 MiB in use, 269 with
32 and 357 with 48, and `memmove` rose from 24% of cycles to 38%
("The copy with a larger held buffer", F2). With `memmove` still 25%
to 27% of cycles, whatever keeps the pages in use under 32 MiB should
show directly in the copy's time. First, rings of 4 x 128 on the
whole stack, against 4 x 256. On build 461 that ring size used
24 MiB and ran at 35.6 GB/s, but dropped 1.5 million frames. On build
462 the drops at 4 x 256 fell from 250 thousand to about 3 thousand,
so what to read is whether 4 x 128 now holds without dropping. The
cell is `plans/16k-27-copy128.plan`, run the same way as
`16k-26-copybest.plan`, two or three runs of each: how the flows fell
onto the rings moved build 462's runs by 4% on its own.

**The linger's 14% to 17% of cycles in `arch_counter_get_cntpct` is
the busy wait itself, not an extra cost of reading the clock.** The
wait loop in `svc_tcp_rx_linger()` calls `ktime_get_ns()` on every
pass, to check the deadline. On arm64 that reads the counter as
`isb; mrs cntpct_el0` followed by `arch_counter_enforce_ordering()`: a
pipeline flush and a dependency on the value. The rest of a pass is
two bit tests, `need_resched()` and a `yield`, so nearly every sample
taken while a thread lingers lands in the counter read. A session
suggested reading the clock only every N passes to win that back
(2026-10-09, in chat only). That would not work: the samples would
move to the bit test and the `yield`, and the core would stay held
until data came or the time ran out. What costs CPU is how long a
thread lingers, which is why CPU per GB went from 0.49 to 0.52.
Three ways to reduce it, none written or run:

- **A shorter linger.** A piece is about 13 us in coming, and 50 was a
  first guess. Planned: "Is the linger worth its CPU in copy mode"
  below.
- **Sleeping instead of spinning.** The thread sleeps until
  `svc_data_ready()` wakes it, with a timer bounding the wait. One
  thread still receives the record and no core is held, at the cost of
  a wakeup's latency for each piece.
- **`WFE` instead of `yield`** (`smp_cond_load_relaxed()` on the
  flags word). The core idles more cheaply until the flag's cache
  line changes, but it is still held for that thread.

## Before the reboot into build 462 (2026-10-09, 17:30)

**Read this section first; the handoff below it was written before the
16 KiB boot and is kept for the rig's rules and its scripts.** The
results are in `../XEU_CONVERGENCE.md`: "16K" (sizing), "Hero" and what
follows it (the fast runs), **"Correction, 2026-10-09"**, which
changes how every native figure from the stripe is read, "The last
runs on build 459", and **"Build 461"**, the first runs on the kernel
with the stacking-alignment change.

**From 14:30 on 9 October Mike's direction is copy mode. Start at "The
boot after build 461" just below.** Steps 4 and 5 of build 461's list,
further down, were not started and are not the priority.

## The boot after build 461: start here

Mike built a kernel from `origin/…-WIP3` (its tip is "AGENTS.md: a
commit message carries no disclaimer about the work's state") and is
installing it. **It has three of the five code patches written on
9 October; the other two are sunrpc's alone and can be built as a
module** (the table in "The patches" below).

What 9 October found, in the order it matters for copy mode
(`../XEU_CONVERGENCE.md`, from "Build 461" to "The copy with a larger
held buffer"):

1. On build 461 nfsd's copy on the stripe is written direct.
2. **The harness had deferred skb frees off in every run before 15:30
   on 9 October**; they are on now (`SKB_DEFER_MAX`, default 128).
3. At a 256 KiB held buffer a 1 MiB WRITE is received in 17 calls, a
   GRO skb at a time, by a different thread nine times in ten, and
   its connection is copied from for 44% of the time.
4. **With a 1 MiB held buffer the copy carries 35.6 GB/s on rings of
   4 x 128 and 35.9 on 4 x 256** (27.7 at 256 KiB; the loan 30.3).
5. With the waiting gone the copy's own speed is the limit and it
   falls as the region in use passes the 32 MiB L3: rings of 4 x 512
   drop nothing and carry 32.8. Whatever has a page reused sooner or
   fewer pages idle is worth rate.

**A change is one of many** (Mike; the paragraph "How to judge a
change here" below). Keep what does its job and measure them
together.

On the new kernel, in order. **Everything goes in first and the whole
is measured (Mike, 2026-10-09): the two sunrpc patches and the
driver's parameter are loaded before any cell is run, not after the
kernel's three have been measured alone.** All three of those are
off until set (`svc_tcp_rx_linger_us` 0, `pools_per_node` 1,
`rx_refill_thresh` 32), so the kernel's three patches by themselves
are still one run away, with nothing to unload. And straight after a
boot is the cheapest time for both swaps: no region is bound, and the
clients have to be dropped once and not twice.

0. `cat /sys/block/md0/queue/dma_alignment` is 3 and `enp161s0f0` has
   its address. **Run nothing on veth.**
1. **The sunrpc module.** `./build-sunrpc.sh` builds `sunrpc.ko` from
   this tree for the running kernel (it needs that kernel's `-devel`;
   a minute) and says whether nfsd can load against it as it is: no
   export missing and none with another version. It builds
   `svcsock_kunit.ko` beside it. **Putting it in place stops nfsd,
   unmounts every NFS mount and drops every client, the other client
   on `enp164s0f0` too: ask Mike, and keep the installed
   `sunrpc.ko` beside the new one.** If `pools_per_node` is wanted
   for the first runs, set it before nfsd starts again
   (`echo 4 > /sys/module/sunrpc/parameters/pools_per_node`;
   `cat /proc/fs/nfsd/pool_threads` then shows four pools).
2. **The driver.** The refill threshold is Patch4 of
   `kernel-rpmtree/xsight/xsight.spec`, uncommitted ("The driver's
   refill threshold is a module parameter" below). If Mike's build
   did not carry it, `T=$L/kmod-xsight-16k-build-r5 ./build-kmod.sh`
   builds release 5 for the running kernel; installing it and
   reloading `xeu` takes `enp164s0f0` down for a few seconds and is
   Mike's (the commands are in the handoff further down, with 5 for
   4). Then: `grep -c xeu_rx_refill_timer /proc/kallsyms` is 1,
   `rx_ip_align` is 2, `rx_refill_thresh` is 32, both ports have their
   addresses.
3. `mount -o noatime /dev/md0 /hs/stripe` (`mdadm --assemble --scan`
   first if the array is not there), `./bringup.sh 32768 256`,
   `echo 512 > /proc/fs/nfsd/threads`.
4. The KUnit suites, `svcsock_kunit` above all: the page swap changed
   `svc_tcp_save_pages()`, `svc_tcp_restore_pages()` and
   `svc_tcp_clear_pages()` (`../TESTING.md` has how they are run).
   Before any load.
5. **The whole stack**, the copy at its best setting, traced, three
   times (about 3 TB each; the stripe is trimmed before each by
   `TRIM=`):

       echo 8 > /sys/module/xeu/parameters/rx_refill_thresh
       RX_LINGER_US=50 PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/xeu-16k-<build>-stack1 plans/16k-26-copybest.plan

   **Load every tracer at idle first** (`timeout -s INT 3 bpftrace
   nfsdlat.bt`, the same for `handoff.bt`; `handoff.bt` has
   `XPT_QTIME_OFFSET`, which is of the kernel, and says how to read
   it): a cell was thrown away on a tracer that had not loaded.
   Against build 461's one traced run of that cell: 35.9 GB/s, 0.49
   server CPU-seconds per GB, 2054 region pages, 6.4 calls for a
   WRITE, 269 us in them, `memmove` 30% of cycles, 250564 frames
   dropped. What shows each change is doing its job: in `pp-*.txt`,
   `rx_pp_recycle_ring_full` near zero and `rx_pp_alloc_refill`
   taking what `rx_pp_recycle_cached` does not (the stack);
   `__folio_batch_release` and `alloc_pages_bulk` gone from the
   profile (the page swap); in `handoff-*.txt`, calls for a WRITE
   near 1 and the next call by the same thread (the linger, which is
   a busy wait: read the CPU for each GB with it); `__lwq_dequeue`
   down the contended list (the pools). The values 50 and 8 are first
   guesses: a piece of a WRITE is about 13 us in coming.
6. Then what each is worth, by taking one away at a time from the
   whole: `RX_LINGER_US=0`, `rx_refill_thresh` 32, `SKB_DEFER_MAX=0`;
   `pools_per_node` needs nfsd restarted.
7. Not run on any kernel: a load paced at the source (`STEP_RATE`,
   below); 64 connections at a 1 MiB buffer (planned: "Does
   `pools_per_node` earn its place" above); a 2 MiB buffer; rings of
   4 x 128 against 4 x 256 (why it matters now: "The footprint
   against the L3" at the top).

`smartctl` is not installed, so the drives' wear is not read; 9 October
wrote about 90 TB to the stripe, 15 TB a drive.

**Mike rebuilt the kernel from the tip on 9 October and rebooted into
it (build 461). A session has no memory of the days before it:
everything known is in these files and in
`/var/tmp/zc-run-logs/` (which is on the root file system and
survives).** Commits here end with a `Signed-off-by:` of the
configured git identity (`git commit -s`) and nothing else: no
`Assisted-by:` since 2026-10-23, never `Co-Authored-By` (AGENTS.md,
"Commit trailers"). Mike corrected that once already.

## The state of tardis1 now (build 461, 2026-10-09 13:30)

- Kernel `7.1.13-21.hs.461.loanpages_pool.el9.aarch64+16k`;
  `kmod-16k-xsight` 2.0.1-4 for it is installed and loaded (retry
  symbol present, `rx_ip_align` 2). It was checked, not rebuilt.
- A **32768-page** region is bound to `0000:a1:00.0`; the port is at
  4 rings of 256. `alloc_fail` 0.
- `/dev/md0` assembled by itself and is mounted at `/hs/stripe`
  (`mount -o noatime`, by hand: it is still not in `/etc/fstab`).
  Its `dma_alignment` is 3.
- nfsd is at **512** threads (the boot's own count is 128).
- `enp164s0f0` came up with the boot and has its address. It is not
  ours and was not touched. Nothing has run on veth.
- `smartctl` is not installed on this root, so the drives' wear was
  not read. This boot has written about 18 TB to the stripe.

## The state of tardis1 before the reboot (build 459)

A reboot undoes some of this: no region is bound, the port is back at
its 8 rings of 1024, nfsd at its own thread count, `enp164s0f0` is
probably up again (leave it; it is not ours), `/hs/stripe` is not
mounted (it is not in `/etc/fstab`) and the array may come up as
`/dev/md127` and not `/dev/md0`.

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
5. **Build 461: the copy on the stripe is written direct, and carries
   27.7 GB/s (27.3 to 28.1) against the loan's 30.3 (30.2 to 30.4),
   three runs each.** The loan still goes through the page cache. The
   direct copy costs the server less CPU per GB than the loan (0.48
   against 0.54 CPU-seconds) and holds 770 region pages against 4100.
6. **Traced on build 461** (after the call with Xsight; "Build 461,
   traced" in `../XEU_CONVERGENCE.md`): the pools reuse pages through a
   FIFO and not last in, first out; the rings take 6% to 42% each of
   sixteen flows; a loan through the page cache queues on each file's
   inode lock; the outliers in nfsd are COMMITs; 64 connections on
   rings of 128 made the copy slower (21.1 GB/s). Nothing was changed
   in the kernel for it. `trace-sweep.sh` and `nfsdlat.bt` are the
   tools.

## The patches (2026-10-09)

Five code patches at the tip, by subject. The first three are in the
kernel Mike built from `…-WIP3`; the last two are not:

| commit | what | which mode it is for |
|---|---|---|
| "SUNRPC: Trade pages with the socket when a partial record changes hands" | nfsd no longer frees and allocates a WRITE's pages each time a part-received record passes between threads (T3) | copy |
| "page_pool: Recycle a region-bound pool's pages last in, first out" | a stack in place of the FIFO ring for pools bound to the region (T1) | both |
| "SUNRPC: Return a loaned record's page-pool pages in batches" | the pool's lock is taken for each batch and not each page (T4) | loan |
| "SUNRPC: Let a NUMA node have several thread pools" | `pools_per_node`: several queues of ready connections where 512 threads shared one (T6) | both |

| "SUNRPC: Let a thread wait for the rest of a partly received TCP record" | `svc_tcp_rx_linger_us`: a WRITE is received by one thread, which waits for the next piece and does not park the record (W3) | both |

The last two were written after Mike's build of `…-WIP3` began and are
not in it. The linger is off at 0; `RX_LINGER_US=50` to `sweep.sh`
sets it for a run (a piece is about 13 us in coming), and
`handoff.bt` shows whether it took: calls for each WRITE near 1 and
the next call by the same thread. It is a busy wait, so read the
server's CPU with it. `pools_per_node` does nothing until it is set, and that is read
when nfsd starts: `options sunrpc pools_per_node=4` in
`/etc/modprobe.d` before a boot, or
`echo 4 > /sys/module/sunrpc/parameters/pools_per_node` and a restart
of nfsd, which drops every client's connections and so is Mike's to
do. `cat /proc/fs/nfsd/pool_threads` then shows the pools.

`net/core/page_pool.c` is built in, so they need a kernel build and a
reboot. `svcsock_kunit` covers the save and restore of pages; run it
on the new kernel.

**The testing is of copy mode (Mike, 2026-10-09).** The aim is the
shortest time from a frame landing to its buffer being back in the
pool, by making the copy and the direct write as efficient as they
will go; that path is also what a loan to a drive that takes
arbitrary pieces uses. Each iteration is the copy cell alone,
`plans/16k-23-copy.plan`, which halves what an iteration writes to
the stripe (about 3 TB):

    PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/<outdir> plans/16k-23-copy.plan

The figures to beat, build 461 with sixteen connections and 512 nfsd
threads: **35.9 GB/s with a 1 MiB held buffer on rings of 4 x 256**
(`plans/16k-25-copybuf-rings.plan`, its first line; 0.49 server
CPU-seconds per GB, 2054 region pages, 250 thousand frames dropped),
and at the old setting of 256 KiB and 4 x 128, 27.7 GB/s (27.3 to
28.1 untraced, 27.7 traced), 0.48 server
CPU-seconds per GB, the client `rwnd_limited` 98.5%, `memmove` 16% of
cycles, page frees and allocations about 5%. What to read on the new
kernel: `rx_pp_alloc_refill` against `rx_pp_alloc_empty` in
`pp-*.txt` (the stack in use), `__folio_batch_release` and
`alloc_pages_bulk` gone from the profile, and the rate.

**The driver's refill threshold is a module parameter in
`kernel-rpmtree`, uncommitted (2026-10-09).** `xsight/xeu-make-the-RX-refill-threshold-a-module-parameter.patch`
is Patch4 of `xsight.spec`, whose Release is 5: `rx_refill_thresh`,
default 32 (as the driver is), the empty slots at which a poll refills
the ring, writable while the port is up. It is not committed there
because that repository has no git identity configured for this user
and its AGENTS.md forbids taking one from the history; the spec has no
changelog entry for the same reason. `./build-kmod.sh` built it
against build 461 into `$L/kmod-xsight-16k-build-r5` (the parameter is
in `modinfo`); nothing was installed or loaded. A new kernel needs the
kmod built against it, and loading it reloads `xeu`, which is Mike's
to do. After that
`echo 8 > /sys/module/xeu/parameters/rx_refill_thresh` needs no
reload.

**How to judge a change here (Mike, 2026-10-09): as one of many, not
by whether it alone moves the rate.** A combination of changes can be
what unlocks the rate, and one stage's limit hides the gain of a fix
ahead of it. So a change that does what it is meant to (the counters,
the lock contention, the order of reuse, the cycles in the profile)
is kept and added to the others, and the whole is measured; "no gain
alone" is a result to record and not a reason to drop it or to leave
the next one untried.

**The harness turns deferred skb frees off** (`net.core.skb_defer_max=0`
in `run-budget.sh`, since 4 October, undocumented until 9 October), so
every figure from this rig before 15:30 on 9 October was taken that
way. **The harness's default is now 128, on (Mike);** `SKB_DEFER_MAX=0`
repeats the old behaviour, and a run whose `geometry.txt` does not name
the setting was made with 0. On, half of the copy cell's pages come back
by the pool's lockless cache and the rate is the same (27.3 and
28.0 GB/s): "The harness had deferred skb frees off" in
`../XEU_CONVERGENCE.md`. That is one change alone. The page-pool patch
above makes the other half of the pages, those that overflow the
32-page cache, last in, first out as well.

**How a WRITE is received in copy mode** (`handoff.bt`; "How a WRITE
is received" in `../XEU_CONVERGENCE.md`): 17 calls of
`svc_tcp_recvfrom()` for each 1 MiB WRITE, 89% of them by another
thread than the last, and the connection copied from for 44% of the
time; 38% is waiting for data behind a 256 KiB buffer and 18% is
waiting for a thread.

**The copy's best setting so far, build 461: a 1 MiB held buffer on
rings of 4 x 128 or 4 x 256, 35.6 and 35.9 GB/s** (one traced run
each; "The copy with a larger held buffer" in `../XEU_CONVERGENCE.md`).
Rings of 4 x 512 drop nothing and run at 32.8: once the waiting is
gone the copy's own speed is the limit, and it falls as the region in
use passes the 32 MiB L3 (a 1 MiB WRITE's copy takes 224 us with
24 MiB in use and 357 us with 48). Whatever has pages reused sooner
or fewer of them idle is worth rate from here.

Copy-mode things that need no build and are not run: fewer nfsd
threads (idle threads are woken last in, first out already, so this
may change nothing); 64 connections on rings deeper than 128; a load
paced at the source, for which the harness now has `STEP_RATE`:

    STEP_RATE="24000 27000 30000 33000" PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $L/<outdir> plans/16k-23-copy.plan

One figure (MB/s, all writers together) paces the whole cell; several
are steps of equal length within it, the same writers on the same
files, each step starting as the one before ends. The log and
`<cell>/ramp.tsv` have each step's rate against what it was offered,
and the cell's MBps is their mean. `where.sh` and the tracers sample
from 35 s in, so with four steps they see the second and third.

## This boot (build 461): steps 0 to 3 are done, 4 and 5 wait for Mike

Step 0 passed with no rebuild; step 1's `dma_alignment` is 3; step 2's
region is bound and the stripe trimmed; step 3 ran three times
(`$L/xeu-16k-461-best1` to `-best3`, driver
`$L/xeu-16k-461-best-driver.sh`) and is written up as "Build 461" in
`../XEU_CONVERGENCE.md`. **Steps 4 and 5 have not been started.** For
step 5, the region has been set once this boot and never resized.
`setsid` forks when started from a shell that leads its process group,
so the PID `$!` gives is not the sweep's: find it with `pgrep -xf` of
the exact command line, then `tail --pid`.

Three things were waiting for this kernel build:

| commit, by its subject | what |
|---|---|
| "page_pool: Give the fixed receive region back when it is resized" | the region is given back when its size changes with no pool bound |
| "block: Let a stacking device take its members' memory alignment" | a stacking device (every MD level, device-mapper) takes its members' memory alignment |
| (no commit) | a larger region: `./bringup.sh 32768 256` |

The steps, in order:

0. **The driver: check it, do not rebuild it.** Mike folded the Xsight
   changes he knows of (the refill retry among them) into
   `kernel-rpmtree`, so the kmod built with the new kernel should carry
   them (Mike, 2026-10-09). Check and go on:
   `grep -c xeu_rx_refill_timer /proc/kallsyms` is 1,
   `cat /sys/module/xeu/parameters/rx_ip_align` is 2, `rpm -q
   kmod-16k-xsight` names the running kernel, and `enp161s0f0` has its
   address. Only if one of those fails is the handoff's
   `./build-kmod.sh` and reload wanted, and then say so to Mike first
   (the reload takes `enp164s0f0` down for a few seconds).
   **Run nothing on veth.**
1. `cat /sys/block/md0/queue/dma_alignment` is 3 (it is 511 without
   the stacking change). If the array does not assemble by itself:
   `mdadm --assemble --scan`, then `mount -o noatime /dev/md0 /hs/stripe`.
2. `./bringup.sh 32768 256`, then `fstrim /hs/stripe`.
3. The copy on the stripe, and that it is written direct now
   (`wr_direct` in `summary.tsv`):
   `PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./sweep.sh $L/<outdir> plans/16k-17-best.plan`
   with nfsd at 512 threads (`echo 512 > /proc/fs/nfsd/threads`).
   That is the comparison the night was thought to be making: a direct
   copy against a loan through the page cache.
   Every cell of 120 s writes about 3 TB to the stripe, and the drives
   are consumer ones at 4% of their endurance: say so to Mike before a
   long queue. Start a queue from a driver script that waits on the
   one before it (`tail --pid`), and wait on the driver.
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
- (2026-10-11: `/hs/nvme0` and `/hs/nvme1` are gone, see "Two
  stripes" at the top.) Until then: only `/hs/nvme0` and `/hs/nvme1`,
  as files; no drive is reformatted.
- (2026-10-11, Mike: this rule is lifted. Both ports are ours, for
  runs on two ports at once. Check that no NFS client is connected
  before reconfiguring one.) Until then:
  only `enp161s0f0`. **`enp164s0f0` carries another client's
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
