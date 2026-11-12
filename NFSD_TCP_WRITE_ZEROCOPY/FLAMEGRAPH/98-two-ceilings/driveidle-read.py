#!/usr/bin/python3 -I
# driveidle-read.py RUN: RUN's driveidle.out as one line a data drive: the
# share of the traced time at each queue depth (0 is idle), requests a
# second and GB/s; then all eight together.
import os, re, sys
L = '/var/tmp/zc-run-logs'; d = f'{L}/{sys.argv[1]}'
at = {}; issued = {}; bytes_ = {}; traced = 0
for l in open(f'{d}/driveidle.out'):
    m = re.match(r'@at\[(\d+), (\d+)\]: (\d+)', l)
    if m: at.setdefault(int(m.group(1)), {})[int(m.group(2))] = int(m.group(3))
    m = re.match(r'@issued\[(\d+)\]: (\d+)', l)
    if m: issued[int(m.group(1))] = int(m.group(2))
    m = re.match(r'@bytes\[(\d+)\]: (\d+)', l)
    if m: bytes_[int(m.group(1))] = int(m.group(2))
    m = re.match(r'traced_us (\d+)', l)
    if m: traced = int(m.group(1))
names = {}
for n in os.listdir('/sys/block'):
    try: maj, mi = open(f'/sys/block/{n}/dev').read().split(':'); names[(int(maj) << 20) | int(mi)] = n
    except Exception: pass
tot = {q: 0 for q in range(5)}; n = 0
print(f"{sys.argv[1]}: traced {traced / 1e6:.2f} s; time at a queue depth of 0 (idle) / 1 / 2 / 3 / 4+:")
for dev in sorted(at):
    name = names.get(dev, str(dev))
    if not name.startswith('nvme') or name == 'nvme8n1': continue
    t = sum(at[dev].values()) or 1
    sh = [100.0 * at[dev].get(q, 0) / t for q in range(5)]
    for q in range(5): tot[q] += at[dev].get(q, 0)
    n += 1
    print(f"  {name}: {sh[0]:5.1f}% {sh[1]:5.1f}% {sh[2]:5.1f}% {sh[3]:5.1f}% {sh[4]:5.1f}% | {issued.get(dev, 0) / (traced / 1e6):8.0f} requests/s, {bytes_.get(dev, 0) / (traced / 1e6) / 1e9:5.2f} GB/s")
T = sum(tot.values()) or 1
print(f"  all {n}: " + " ".join(f"{100.0 * tot[q] / T:5.1f}%" for q in range(5)) + f" | {sum(v for k, v in bytes_.items() if names.get(k, '').startswith('nvme') and names.get(k) != 'nvme8n1') / (traced / 1e6) / 1e9:.2f} GB/s")
