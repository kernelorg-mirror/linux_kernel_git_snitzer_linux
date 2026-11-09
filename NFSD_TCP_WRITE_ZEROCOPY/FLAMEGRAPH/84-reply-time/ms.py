#!/usr/bin/python3
# ms.py BEFORE AFTER: the client's NFS WRITEs between two copies of
# /proc/self/mountstats, summed over every mount: ops, and per op the
# mean queue, RTT and execute times (ms).
import sys
def w(path):
    t = [0] * 8
    for line in open(path):
        f = line.split()
        if f and f[0] == "WRITE:":
            for i in range(8):
                t[i] += int(f[1 + i])
    return t
a, b = w(sys.argv[1]), w(sys.argv[2])
d = [y - x for x, y in zip(a, b)]
ops = d[0] or 1
print(f"WRITE ops {d[0]} trans {d[1]} timeouts {d[2]} MB sent {d[3] / 1e6:.0f}"
      f" | per op ms: queue {d[5] / ops:.3f} rtt {d[6] / ops:.3f}"
      f" execute {d[7] / ops:.3f}")
