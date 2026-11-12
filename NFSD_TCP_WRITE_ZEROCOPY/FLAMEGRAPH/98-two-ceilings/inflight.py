#!/usr/bin/python3 -I
# inflight.py <out> <seconds> [interval_ms]: the eight data drives' queue
# depth, read from /sys/block/nvmeNn1/inflight (reads + writes in the
# driver, exact, from the tag maps) every interval (1 ms), and the bytes
# written from /sys/block/nvmeNn1/stat at the start and end.  Prints one
# line a drive: the share of samples at a depth of 0, 1, 2, 3 and 4 or
# more, the mean depth, and GB/s over the span; then all eight.
import sys, time
out, secs = sys.argv[1], float(sys.argv[2])
step = (float(sys.argv[3]) if len(sys.argv) > 3 else 1.0) / 1000
drives = [f'nvme{i}n1' for i in range(8)]
fds = {d: open(f'/sys/block/{d}/inflight') for d in drives}
def sectors(d):
    return int(open(f'/sys/block/{d}/stat').read().split()[6])
s0 = {d: sectors(d) for d in drives}; t0 = time.monotonic()
hist = {d: [0] * 5 for d in drives}; tot = {d: 0 for d in drives}; n = 0
end = t0 + secs
while time.monotonic() < end:
    for d in drives:
        f = fds[d]; f.seek(0); r, w = f.read().split()
        q = int(r) + int(w); hist[d][min(q, 4)] += 1; tot[d] += q
    n += 1
    time.sleep(step)
t1 = time.monotonic(); s1 = {d: sectors(d) for d in drives}
with open(out, 'w') as o:
    allh = [0] * 5; allgb = 0
    o.write(f"inflight: {n} samples over {t1 - t0:.2f} s; share at a depth of 0 / 1 / 2 / 3 / 4+, mean depth, GB/s\n")
    for d in drives:
        gb = (s1[d] - s0[d]) * 512 / (t1 - t0) / 1e9; allgb += gb
        for i in range(5): allh[i] += hist[d][i]
        o.write(f"  {d}: " + " ".join(f"{100.0 * hist[d][i] / n:5.1f}%" for i in range(5)) + f"  mean {tot[d] / n:5.2f}  {gb:5.2f} GB/s\n")
    o.write("  all 8: " + " ".join(f"{100.0 * allh[i] / (8 * n):5.1f}%" for i in range(5)) + f"  {allgb:.2f} GB/s\n")
