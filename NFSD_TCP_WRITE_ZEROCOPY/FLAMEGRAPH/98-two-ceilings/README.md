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
