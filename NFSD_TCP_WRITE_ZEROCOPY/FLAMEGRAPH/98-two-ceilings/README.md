# Round 98: the two ceilings, and where the drives' 12% goes

**Mike, 12 November: "ultimately I want to achieve 81GB/s, I don't care
_how_ that is realized."**  Rounds 94 to 97 had worked the footprint;
this round turns to the rate, and asks what bounds it.

## The two ceilings, measured with no NFS

- **The drives: 81.58 GB/s, 10.20 a drive, whatever they are fed.**
  Local fio, all eight at once, 1 MiB direct writes: 81.58 at 2, 4 and
  8 outstanding a drive; 81.58 again as random 1 MiB writes, as 24
  writers a drive in order, and as 3 ([orderprobe.sh](orderprobe.sh),
  [orderprobe.out](orderprobe.out)).  Exactly 10.20 on every drive at
  every point: a cap in the drive, reached however it is fed.  The
  loaned WRITE's layout (34 SGL segments a 256 KiB request, round 94's
  `sglwrite`) costs 1% on eight drives at once, as on one
  ([ceilprobe.sh](ceilprobe.sh), [ceilprobe.out](ceilprobe.out)).  So
  81 GB/s is the drives' ceiling itself, with nothing above it.
- **Memory: about 148 GB/s.**  [membw.c](membw.c), 48 threads on
  nfsd's CPUs: write 147 to 149 GB/s, read 148.  The mesh's
  `hnf_mc_reqs` counter reads 146 GB/s for a 148 GB/s read, so round
  56's 2.18 G requests/s at the 73.27 GB/s run (rings of 256) was about
  140 GB/s: 94% of memory, which is why that rate was flat from 512 to
  640 KiB.  The settled setting moves 1.70 to 1.74 bytes through memory
  a payload byte (`t2-r522..r524`): 121 to 125 GB/s at 72, 82%.
  **Saturating memory does throttle the drives:** flat local writes on
  eight drives fall from 78.8 to 50.7 GB/s beside a CPU memory load
  that fills the rest of the bus, but lose nothing beside 61 GB/s of it
  (about 140 in all).

So the rate is bounded by the drives' 81.6 times the share of the time
they are fed, and by 148 over the bytes moved a payload byte: at 1.74,
85; at 1.9, 78.

## Where the 12% goes: the drives are idle 10% of the time

[run522.sh](run522.sh) traced the block layer's issues and completions
(`driveidle.bt`), and read every drive at 4 or more requests 98% of the
time while delivering 8.6 to 9.2 GB/s: wrong, as `/sys/block`'s 90 to
93% busy said (a requeued request is issued twice).
[run524.sh](run524.sh) reads the depth exactly instead, from the
driver's tag maps (`/sys/block/nvmeNn1/inflight`,
[inflight.py](inflight.py), every ms for 12 s of the window):

| run | load | GB/s | a drive: idle / 1 to 3 / 4+ | mean depth | a drive GB/s | memory |
|---|---|---|---|---|---|---|
| `t2-r524` | two ports, settled (16 jobs, 8 MiB blocks, iodepth 16) | 71.75 | **10.2% / 11.2% / 78.5%** | 11.5 | 8.90 to 9.02 | 125 GB/s, 1.74 a byte |
| `t2-r525` | port a alone on nvme0-3 (its one-port setting) | 40.36 | 1.0% / 1.2% / 97.8% | 48 | **10.08 to 10.10** | 112 GB/s, 2.77 a byte |
| `t2-r525` | local flat fio on nvme4-7 at the same time | 40.86 | | | **10.21 to 10.22** | |

- **On two ports every drive is idle a tenth of the time and shallow
  another tenth**, and delivers 88% of its cap: 10.20 x 0.88 = 8.98.
  The 12% is gaps in the feed, not the drive's service under NFS.
- **NFS runs a drive at its cap when the feed is deep:** one port's
  16 writers on four drives hold them at 48 deep and 10.1 GB/s.
- **The NIC's traffic does not slow drives through the fabric:** the
  four drives it was not writing to held 10.21 locally beside it.

A drive's feed on two ports is four writers' 8 MiB blocks, each
arriving as 32 requests at once over twelve connections, then nothing
from that writer until its next block: a mean of 11.5 queued with
10% of the time empty.  **The lever is a deeper, smoother feed a
drive, which is the client's load shape**: more writers a drive, or
smaller blocks at a deeper iodepth.  The cost is the second ceiling:
more in flight holds more pages and moves more bytes through memory,
and 80 GB/s at 1.74 a byte is 139 GB/s, 94% of memory.

## More writers a drive do not fill the gaps (t2-r526 to r528)

[run526.out](run526.out), the same two-port setting with the client's
load reshaped (`JOBS`, `BS`, `IODEPTH` to twoport.sh):

| run | client load | GB/s | a drive idle / 1 to 3 / 4+ | mean depth | held median | memory | loans denied | client CPUs |
|---|---|---|---|---|---|---|---|---|
| `t2-r524` | 16 jobs, 8 MiB, iodepth 16 (settled) | 71.75 | 10.2% / 11.2% / 78.5% | 11.5 | 56 MiB | 125 GB/s, 1.74 | 2.9% | 70% |
| `t2-r526` | 32 jobs, 8 MiB, iodepth 16 | 71.66 | 10.5% / 10.7% / 78.8% | 12.7 | 59 | 126, 1.77 | 2.8% | 70% |
| `t2-r527` | 32 jobs, 1 MiB, iodepth 64 | 67.64 | 13.8% / 12.5% / 73.7% | 8.9 | 75 | 132, 1.94 | 9.4% | 56% |
| `t2-r528` | 64 jobs, 1 MiB, iodepth 32 | 67.17 | 14.6% / 12.4% / 73.0% | 9.4 | 80 | 131, 1.96 | 16% | 57% |

Twice the writers change nothing; 1 MiB blocks lose 6%, hold 20 MiB
more and copy a tenth of the records (the budget's denials).  **So the
idle tenth is not the client's block shape, and the reading above
has the arrow backwards:** a drive fed at 88% of its capacity is idle
12% of the time by arithmetic, whatever the arrivals look like.  The
idle time is the consequence of 72 GB/s arriving, not its cause.
What sets the 72 is still open: one port alone drives its four drives
to their cap, so on two ports a shared resource holds the inflow, and
the shared resources are the client (70% busy on 48 CPUs at 72, 48%
at 40, spread evenly across its cores), the server's memory (125 of
148 GB/s), and the server's receive path and threads (17% busy).
The client's connections are `rwnd_limited` 98% of the time in every
run.  The loan budget of 64 denies 2.5 to 2.9% of loans (128 denied
none) at no rate seen; not the lever either.

## Neither the pipe nor the server's ACK moves it (t2-r529, r530)

[run529.out](run529.out): rings of 256 (round 56's 73.27 setting,
no drops) with the mark at 2 (the server's ACK 18 us sooner, round
97), at 512 and 640 KiB: **71.48 and 72.18 GB/s**, no drop, memory at
133 to 134 GB/s (1.86 a byte, 90%), the drives idle 10%.  So on two
ports the rate holds at 72 against every server-side knob there is:
the window, the rings, the mark, the budget, the client's writers.

## The wall is the client's copy, a connection at a time (r531 to r533)

[run531.out](run531.out), [run533.out](run533.out).  `t2-r531` is the
settled load with a `perf` profile of the whole client for 6 s of the
window (`CLIPERF=1`; `$L/t2-r531/cli-perf.txt`, and the reads below
from the client's `/tmp/zc-cli.data`):

| of the client's 48 CPUs | |
|---|---|
| `cpu_idle_poll` (the idle loop) | 28% |
| **`__pi_memcpy` in `tcp_sendmsg_locked`, from `xprt_transmit` in rpciod's workers** | **18.4%** (18.0 of it that one chain) |
| fio's `get_io_u` | 8.4% |
| `__list_del_entry_valid_or_report` (`CONFIG_DEBUG_LIST=y` on the client's kernel) | 6.4%, 2.6 of it in `nfs_scan_commit_list` |
| `tcp_sendmsg_locked` itself | 2.9% |

**The client copies every payload byte into TCP**
(`xprt_send_pagedata()` in `net/sunrpc/socklib.c` sends the pages as
a bvec with no `MSG_SPLICE_PAGES`, so `tcp_sendmsg_locked()` copies
them; the server side's `svc_tcp_sendmsg()` splices, the client's never
did), **in the transmit of one RPC at a time a connection**
(`xprt_transmit()` holds the transport while rpciod's worker sends).
A 1 MiB WRITE is one worker copying 1 MiB through TCP under the
socket lock, about 360 us of system time a WRITE at this rate (55% of
48 CPUs in the kernel, 72k WRITEs a second): about **3 GB/s a
connection, and 24
connections are 72 GB/s.**  The connections read `rwnd_limited` 98%
of the time because the window stays full while the ACK that would
open it waits in the client's softirq queue behind the copies.  One
port's 16 connections at 2.5 GB/s each sit under the cap, which is
why one port reaches its drives' 10.1 GB/s and two ports do not.

The cross-check is more connections.  `t2-r532`, 16 a port on rings
of 128: **73.37 GB/s**, 2,250 ring drops and 2,397 retransmits, the
drives idle 8.9%.  `t2-r533`, 16 a port on rings of 256:
**71.80 GB/s**, no ring drop, 31 retransmits, the drives idle 10%,
held 70 MiB, **memory at 138 GB/s (1.93 a byte, 93% of 148)**, the
32 connections `rwnd_limited` 99.6%.  So 32 connections on rings of
256 buy nothing: by then the server's memory is the wall, as it was
for round 56's 73.27 at the same rings.

## Where this leaves 81

Three walls meet within 2% of each other at this setting, and each
one hides the others:

1. **The client's copy, a connection at a time:** about 3 GB/s a
   connection, 72 GB/s on 24 connections.  More connections lift it
   (73.4 on 32), but then
2. **the rings of 128 drop frames** under the burstier arrivals
   (2,250 drops, 2,397 retransmits), and on rings of 256, which do
   not drop,
3. **memory is at 93%** (1.93 bytes a payload byte at 72 GB/s), as
   round 56 found: the larger rings hold 16 MiB of posted buffers and
   the loaned pages miss the L3 more.

The drives, at 81.6 GB/s, are above all three.  The order to take
them in: the client first, since nothing on the server can move the
rate while 24 connections copy (`MSG_SPLICE_PAGES` for the page
array in `xprt_send_pagedata()`, as `svc_tcp_sendmsg()` does on the
server, head and tail still copied; and a client kernel without
`CONFIG_DEBUG_LIST`).  Then, with the client's cap gone, the server's
two: at rings of 128 and 1.74 bytes a byte, 81 GB/s is 141 GB/s of
memory, 95%; so the bytes a payload byte have to come down toward
1.6, which is the footprint work of rounds 94 to 97 by its real name
(a page that reaches the drive while still in the L3 costs no memory
read), and the rings' drops at the higher rate have to be measured
again.  Realistically that is 76 to 78 GB/s with the drives at 95%,
not 81.

Nothing on the host was changed by this round: the settled setting
is in force (mark 6, 512 KiB, rings of 128, 12 connections, budget
64, window 0).
