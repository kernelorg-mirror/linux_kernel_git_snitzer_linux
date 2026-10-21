# Xsight's replies: what to work through on tardis1 (from 20 October)

The handoff for a tardis1 session that follows up Xsight's answers to
Mike's note. It adds to "20 October: tardis1's boot into the v7.3-7
kernel" in `README.md`, which comes first: do its steps 1 to 6 on the
new kernel (build 491), then these. Nothing here changes the kernel series.

Where things are:

- **What we asked and what they said, item by item**:
  `../xsight-email-checklist.md`. It quotes both replies under the
  items they touch, gives each a status and our reading, and ends with
  the points for our reply. This file is the work behind those points.
- **The note as sent**: `../xsight-uncounted-loss-email.txt` (as of
  `ecca41eba8bb`).
- **Our xeu patches** (Patch0 to Patch7 of the kmod-xsight 2.0.1 spec,
  Release 5 to 7; the FIFO counters are Release 7, kernel-rpmtree
  `7a06a06`): in `kernel-rpmtree`, the `xsight-WIP3` branch and after
  (not `main`); `build-kmod.sh` builds them. The rx_ip_align one is also in
  `../xeu-SGL-compat-receive-placement/`.
- **Rules that hold**: `settled.sh` before every run and its audit
  after; reloading `xeu` and anything that takes a port down while
  the client has it mounted is Mike's; the client is shared (check
  `uptime`, no `logperf` or `fio`, before a battery). **Nothing is sent
  to Xsight but by Mike**: the session drafts, he sends.

## What they answered, in short

The two replies (Ian Wigmore's, from the dev team, and Don Wallwork's
through him), weighed on 20 October; the checklist has the detail.

- **Taken**: the RX FIFO counters (xeu will add them), the refill
  retry (a real bug; they will fix it by returning the NAPI budget),
  receive buffers smaller than a page (as page frags), the RSS table
  (not said for the key and hash function).
- **Answered in part**:
  - **The FIFO**: a non-zero drop count is a rate mismatch; the FIFO
    is the port's whole 2 MiB, nothing more to give. New: **xeu drives
    only two XDMA engines per port** (an IOB interrupt restriction),
    less DMA capacity than their DPDK driver; and Don suggests the XDMA
    engines may stall on memory bandwidth. No figure for what two
    engines sustain.
  - **The hash**: over a 13-byte input, IPv4 source, destination,
    source port, destination port, then the 8-bit protocol. Standard
    RSS takes 12 bytes, no protocol byte, which likely explains why
    none of our field orders matched. Not said: which key bytes, the
    byte order.
  - **IRQ placement**: declined as "the way most drivers implement
    it". Fair for spreading across ports (mlx5, bnxt and i40e do not
    either), not for the hint: those set only the hint
    (`irq_update_affinity_hint()`), so a placement made by hand
    survives the port being opened again; xeu's
    `irq_set_affinity_hint()` resets it on every open.
- **Not yet**: pause frames and interrupt deferral, "we will
  investigate" (they also say xeu supports deferral, which the hang
  contradicts).
- **Asked of us**: should `rx_refill_thresh` vary by use case?
- **Rejected on a misreading: rx_ip_align.** They answered it as the
  CPU's alignment of the IP header (`NET_IP_ALIGN` is 0 on arm64 on
  purpose). It is the storage DMA's alignment of the payload: at
  2 mod 4 no loaned page can go to NVMe as direct I/O. Our patch's
  subject led with the header, which invited the answer.
- Ian's covering note puts the two-port limit in "E1's DRAM
  bandwidth"; it is the server's DRAM.
- Don also advised 8 rings and rings of 512 or 1024 for bursts. We
  measured both already (eight rings three times, no gain, the rings'
  CPUs 58 to 70% idle; two ports best at rings of 128); his rule of
  thumb is for packets under 512 bytes, ours are 7792.

## The work, in order

Each item says why, what to run, and what the result decides. Write
each up in this file under its item (date, build, logs under
`$L/xsight-<item>/`), as docs commits at the tip of
`…-v7.3-7`, with the grouping check.

### X1. The hash: compute it (an hour, no reload)

**Why.** Closes item 3c of the checklist, or makes the follow-up
question exact; and a computed hash lets `rss-place.py` place a flow
before it connects instead of after its first packets.

**Run.** With Toeplitz selected (`ethtool -X enp161s0f0 hfunc
toeplitz`), record the key (`ethtool -x enp161s0f0 > $L/xsight-x1/key.txt`).
Then one connection at a time, each from a known client port, so that
each hash belongs to a known tuple: `iperf3 -s -B 192.168.1.211 -D`
here, and from the client `iperf3 -c 192.168.1.211 -P 1 -t 5 --cport
<port>` for eight or so ports, with `timeout -s INT 5 bpftrace
xeu-rx.bt` beside each (load it at idle first); the client's address
from `ss -tn` here. Write one line per connection, "CLIENT_IP
192.168.1.211 CLIENT_PORT 5201 HASH" (the hash as `@flow` prints it),
and run

    ./toeplitz-check.py $L/xsight-x1/key.txt $L/xsight-x1/flows.txt

It tries the 13-byte and 12-byte inputs, the key from each starting
byte, each field's byte order and the hash's, and prints what matches
every flow (`./toeplitz-check.py --selftest` checks the hash itself
against Microsoft's published vector). Then check what `rss-place.py`
assumes: that bits 13 and up of the hash pick the table entry.

**Decides.** A match: item 3c answered, and the input recorded here.
No match: the question back to them names the 13-byte input we tried
and the key offsets and byte orders it failed in.

**Done, 20 October, build 491: no match; the table bits confirmed.**
Logs in `$L/xsight-x1/`.

- **Selecting Toeplitz.** `ethtool -X enp161s0f0 hfunc toeplitz`
  fails with `EINVAL`, as `../XEU_CONVERGENCE.md` recorded on 7.1:
  the port reports no function while Spooky is set, so the CLI's
  request changes nothing and the core refuses it before `xeu` is
  called. `ynl` sets it (the tree's, `--no-schema`: tardis1 has no
  `jsonschema`):

      python3 tools/net/ynl/pyynl/cli.py --no-schema \
          --spec Documentation/netlink/specs/ethtool.yaml \
          --do rss-set --json '{"header": {"dev-name": "enp161s0f0"}, "hfunc": 1}'

  `ethtool -x` then shows `toeplitz: on`. Port a stays on Toeplitz
  until `xeu` is reloaded; `settled.sh` does not audit the function.
- **The key**: the 40-byte Microsoft key, `6d:5a:56:da:…:01:fa`, then
  28 zero bytes. `toeplitz-check.py` reads every hex digit in its
  KEY file, so give it the key line alone (`key-line.txt`), not
  `ethtool -x`'s whole output: the table's numbers break it.
- **Ten flows**, one iperf3 connection at a time from 192.168.1.106
  to 192.168.1.211:5201, client ports 40001 to 40008, 40101 and 41000,
  each a single flow of 13 to 29 GB in `xeu-rx.bt`'s `@flow`
  (`flows.txt`). The first captures of 40004 and 41000 recorded no
  flow on port a although iperf3 ran; repeated with a 15 s trace
  (`rx2-*.txt`) they did. Not explained. The same eight ports under
  Spooky are in `spooky/`.
- **`toeplitz-check.py`: no combination matches every flow**
  (`check.txt`): neither the 13-byte input (addresses, ports, the
  protocol byte) nor the 12-byte one, with the key from any of its 68
  starting bytes, each field in either byte order, the hash as
  computed or byte-swapped. So the question back names exactly that.
- **`rss-place.py`'s assumption holds**: for all ten flows the table
  entry `(hash >> 13) & 127` names the ring the flow was on, and 13 is
  the only shift that does (the low seven bits do not). A flow can be
  placed from its first packet's hash, as now; not before it connects.

### X2. The FIFO without NFS: is it the card's DMA or the host (an hour)

**Why.** Don gave two causes for FIFO drops beside bursts: xeu's two
XDMA engines, and the engines stalling on memory. Plain TCP separates
them from NFS: it has no copy into the page cache and no drives.

**Run.** The xeu that comes with the new kernel has the FIFO counters
(Mike, 20 October): `echo Y > /sys/module/xeu/parameters/rx_port_stats`
(default N), then `ethtool -S enp161s0f0 | grep port_rx_fifo_tc0` lists
`drop_packets` and `lines`. One port at the one-port setting: plain TCP with the receivers
discarding (`iperf2port.sh`, as in 5b of `../XEU_CONVERGENCE.md`;
49.8 GB/s before), then copying into a small buffer, each three times,
the FIFO's drops and fill sampled as in "15 October, late: the note
to Xsight" (`$L/b474xs/runs.sh`). Then the same with
`copybench` (built from `copybench.c` here) loading memory beside it on
CPUs the rings and receivers do not use, at two or three levels of
memory traffic (`copybench stp <threads> <first cpu> <seconds>`;
`memcost-log.sh` measures the traffic, `../LOAN-PERFORMANCE.md` "The
measure" says how).

**Decides.**
- Drops with the receivers discarding, at about 6 Mpps: the card's
  side (the XDMA engines or bursts the FIFO cannot hold), with no host
  copy involved. That is the case for asking what two engines
  sustain, and whether the IOB restriction can be lifted.
- No drops discarding, drops once memory is loaded, rising with the
  load: Don's memory stall; ask what latency the engines tolerate.
- Neither: the drops come with NFS's own burstiness (as the paced
  runs suggest), and the reply says so.

**Done, 20 October, build 491: the second case, memory.** Driver
`$L/xsight-x2/x2.sh` (and `rerun.sh`), log `x2.log`, `rerun.log`.
Port a alone, `iperf2port.sh a 16 20` at the settled setting, the
FIFO sampled once a second, `memcost-log.sh` beside each run;
`copybench stp <n> 56 34` on port b's ring CPUs (56-63, idle with only
port a in use). Requests to memory as `memcost-read.py` gives them,
1e9/s.

| run | GB/s | FIFO drops | ring drops | FIFO fill, max lines | copybench | requests to memory |
|---|---|---|---|---|---|---|
| discard, three runs | 49.71 to 49.82 | **0** | 118,610 to 286,679 | 1 | none | 0.01 |
| copy, three runs | 49.24 to 49.85 | 4,471 to 16,000 | 101,069 to 405,061 | 4,084 to 14,078 | none | 0.27 to 0.32 |
| discard | 49.56 | **0** | 561,044 | 1 | 2 threads, 27.4 GB/s | 0.92 |
| copy | 46.18 | 100,156 | 185,582 | 11,921 | 2 threads, 26.4 GB/s | 1.03 |
| discard | 49.65 | **0** | 422,937 | 1 | 4 threads, 47.3 GB/s | 1.58 |
| copy | 46.49 (and 47.03) | 100,999 (and 218,713) | 140,121 | 16,229 | 4 threads, 45.1 GB/s | 1.60 |
| discard | **45.22, 45.15** | **520,618, 599,969** | 303,409, 272,890 | 9,924, 15,410 | 8 threads, 64.7 and 64.8 GB/s | 2.27, 2.28 |
| copy | 40.36 | 302,880 | 115,886 | 15,034 | 8 threads, 62.4 GB/s | 2.12 |

- **Discarding, at line rate and about 6 Mpps, the FIFO never drops
  and never fills** (one line at most), with or without memory loaded
  up to four copybench threads. So the two XDMA engines carry a port at
  line rate when memory is free: it is not the card's side.
- **With the memory loaded harder, the FIFO drops with no host copy at
  all**: eight copybench threads (65 GB/s of copying, 2.3e9 requests a
  second) and the receivers discarding gave 520,618 and 599,969 FIFO
  drops and 45 GB/s, twice. Copying, the drops start with the copy
  alone and rise with the load. That is Don's case: the engines stall
  on memory. The question back is what latency the engines tolerate,
  and how much a FIFO of 2 MiB covers at 50 GB/s (about 40 us).
- **The rings tail-drop in every run** (101 to 561 thousand in 20 s),
  the FIFO dropping or not: a separate loss, on the host's side of the
  DMA, not asked about here.
- The first copy-cb4 (47.03 GB/s, 218,713 drops) ran beside a module
  build (X4's), so it was repeated: 46.49 and 100,999.
- `settled.sh apply` at the end of `x2.sh` found the client briefly
  unreachable; the audit after was the settled setting.

### X3. rx_ip_align: the evidence for the reply (tardis2, then tardis1)

**Why.** The rejection rests on a misreading; the reply needs the
payload case in numbers, not argument.

**Run.** The new kernel counts each loaned WRITE's outcome
(`nfsd_write_dio_direct`, `_mem_misaligned`, `_gap_misaligned`;
`bpftrace -e 'tracepoint:nfsd:nfsd_write_dio_* { @[probe] = count(); }'`).
Two loan runs at the same setting, `xeu` loaded with `rx_ip_align=0`
and with `2` (a reload: Mike's), counting the outcomes and the rate.
**tardis2 is the host for this**: its two DP800s take SGLs (dword
alignment, no virtual boundary), so there a loaned page at 4-byte
alignment can go straight to the drive and rx_ip_align is the whole
difference. Run it on tardis2's nvme4 and nvme5 alone. On tardis1 only
a `prp` cell shows anything (`256 1048576 8 prp step-on-n16` onto
nvme0's export, as step 5 of the boot section): there a loaned WRITE's
gaps refuse it anyway, so the A/B shows `mem_misaligned` moving to
`gap_misaligned`, not to `direct`. tardis1's `sgl` cells, onto the
NVMe/TCP target, take arbitrary pieces and go direct at either
setting, so they cannot show it.

**Decides.** The expected result, `mem_misaligned` all at 0 and mostly
`direct` at 2, is the reply's evidence. Draft the paragraph: the payload
lands at 2 mod 4 (`NET_SKB_PAD` 64 + 54 bytes of headers); NVMe's DMA
needs 4-byte alignment; so every loaned WRITE is copied; `NET_IP_ALIGN`
is about the CPU's header loads and rightly 0 on arm64, which is not
this. And ask the question behind the patch: **can the E1 shift only a
frame's first buffer (a receive pad, or header split)**, so that a
frame's continuation buffers are aligned too? A driver cannot do that
alone (see `../xeu-hds-experiment/` for what was tried in software).

### X4. The affinity hint: a patch and its test (Mike reloads)

**Why.** Their answer was the script; the script's placement does not
survive a down and up or `ethtool -G`, because xeu sets the affinity on
every open. The fix is one call, as mainline drivers do it.

**Run.** A seventh xeu patch: in `xeu_setup_rings_irq()`, set the
affinity once when the vectors are first set up and the hint with
`irq_update_affinity_hint()` on each open (or leave the affinity to the
admin entirely, as i40e does). Build with `build-kmod.sh`; Mike
reloads. Then: place port b's interrupts by hand, `ip link set
enp164s0f0 down; ... up`, `ethtool -G enp164s0f0 rx 128`, and read
`/proc/irq/<n>/smp_affinity_list` after each: the placement must
stay. `settled.sh audit` should no longer need to put it back.

**Decides.** Attach it to the reply, with the before and after.

**Done, 20 October, build 491: the placement holds.** Logs in
`$L/xsight-x4/`. The patch is Patch8 of `xsight.spec`, Release 8
(`kernel-rpmtree` `xsight-affinity` branch, not pushed).

- **Before**, Release 7 (`before.txt`): port b's IRQs, placed on
  56-63 by `settled.sh`, went back to the driver's 0-7 after a down and
  up and after `ethtool -G rx 128`, under the same IRQ numbers. **The
  async ring's IRQ went from 57 to 0-63 as well, though xeu gives it no
  hint**: closing frees the IRQs and disables MSI-X, and that resets the
  affinity whatever the hint. So `irq_update_affinity_hint()` alone, the
  one call this item proposed, would not have kept a placement.
- **The patch** records each IRQ's affinity (rx, tx and async) when
  `xeu_close()` frees it, sets it again when `xeu_open()` requests it,
  and sets the driver's spread only as the hint.
- **After**, Release 8 (`after.txt`): every IRQ of port b, the async
  one included, stayed on 56-63 through a down and up, `ethtool -G rx
  128` and `-G rx 256`; the hints still name 0-7. Then plain TCP into
  port b (`MODE=trunc iperf2port.sh b 16 20`): 49.53 GB/s, its ring
  CPUs 50% busy and port a's 1% (`after-load-b.txt`).
- **The reload** (`reload.txt`): `rpm -ivh --force` of Release 8 and
  `rpm -e --justdb` of Release 7, so the 462 and 463 kmods stay; `rmmod
  xeu; modprobe xeu`; `port-setup.sh` for each port (`bringup.sh`
  refuses while the region exists); `settled.sh apply`. On 7.3 the
  region outlives the pools, so it was left bound and the reloaded
  ports found it again (same `phys_base`, 32768 pages, none escaped);
  do not unbind it first, as the 7.1 note in `../XEU_CONVERGENCE.md`
  did. The reload left `min_free` 0 and `alloc_fail` 4 (0 before): the
  region ran dry once while the ports came up, likely the two ports'
  default eight rings before `port-setup.sh` cut them to four. Not
  checked.
- Not tested: `ethtool -L`, which goes through the same close and open.

### X5. rx_refill_thresh: our answer to their question (two runs)

**Why.** They asked whether the threshold should vary by use case. A
module parameter will not go upstream; what will is a value the
driver derives, if one exists.

**Run.** First gather what set it: `rx_refill_thresh` 8 has been part
of the setting since 10 October (`README.md` "15 October" sections;
`../XEU_CONVERGENCE.md`, the 8 x 64 rings at threshold 2 among them).
Then on the new kernel, one port and two ports at the settled setting,
the driver's default against 8, three runs each, the rings' tail drops
and refills beside the rate.

**Decides.** If 8 matters only with small rings, or with buffers
smaller than a page, the answer is a threshold derived from the ring
size (and buffer size), not a parameter; if it makes no difference on
this kernel, say so and drop the patch.

### X6. Interrupt deferral and pause: a reproducer they can run

**Why.** Both are "we will investigate". Make that cheap for them.

**Run.** Write `defer-repro.sh` from `$L/b474xs/defer.sh`: the port
under plain TCP load (no NFS), the two settings written, then each
second for 10 s the rings' frame counts, `/proc/interrupts` for the
port, and the NAPI state (`ynl --family netdev --dump napi-get` if the
tool is here); then the writes of 0, and the down and up. Record one
run. For pause, the commands of the note's 2b in one script, with the
FIFO fill and the client's pause counters sampled.

**Decides.** Nothing on our side; attach them to the reply.

### X7. The reply

Once X1 to X5 have results: draft `../xsight-reply-2.txt` from the
checklist's "For the reply to them", in the note's style (results
first, then each point with its evidence, short questions). Leading
it: rx_ip_align restated around the payload (X3), and the DRAM being
the server's. Mike reviews and sends.
