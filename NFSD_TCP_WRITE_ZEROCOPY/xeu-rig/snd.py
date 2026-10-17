import socket, sys, threading, time
n, secs = int(sys.argv[1]), float(sys.argv[2])
# the server's address on the port and the client's, tardis1's port a by default
srv = sys.argv[3] if len(sys.argv) > 3 else "192.168.1.211"
src = sys.argv[4] if len(sys.argv) > 4 else "192.168.1.106"
buf = bytes(1 << 20)
def tx():
    s = socket.create_connection((srv, 5202), source_address=(src, 0))
    end = time.time() + secs
    while time.time() < end: s.sendall(buf)
    s.close()
ts = [threading.Thread(target=tx) for _ in range(n)]
[t.start() for t in ts]; [t.join() for t in ts]
