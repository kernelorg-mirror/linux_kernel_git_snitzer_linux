import socket, sys, threading, time
rcvbuf, n, secs = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
addr = sys.argv[4] if len(sys.argv) > 4 else "192.168.1.211"	# the server's address on the port
ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
if rcvbuf: ls.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf // 2)
ls.bind((addr, 5202)); ls.listen(64)
tot = [0] * n
def rx(i, c):
    b = bytearray(1 << 20)
    while True:
        r = c.recv_into(b)
        if not r: break
        tot[i] += r
cs = []
for i in range(n):
    c, _ = ls.accept(); cs.append(c)
print("sk_rcvbuf", cs[0].getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF), flush=True)
t0 = time.time()
ts = [threading.Thread(target=rx, args=(i, c)) for i, c in enumerate(cs)]
[t.start() for t in ts]; [t.join() for t in ts]
dt = time.time() - t0
print("rcvbuf %d conns %d: %.0f MB/s (%.2f Gbit/s) over %.1f s" % (rcvbuf, n, sum(tot) / dt / 1e6, sum(tot) * 8 / dt / 1e9, dt))
