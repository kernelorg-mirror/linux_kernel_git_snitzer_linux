import socket, sys, threading, time
n, secs = int(sys.argv[1]), float(sys.argv[2])
buf = bytes(1 << 20)
def tx():
    s = socket.create_connection(("192.168.1.211", 5202), source_address=("192.168.1.106", 0))
    end = time.time() + secs
    while time.time() < end: s.sendall(buf)
    s.close()
ts = [threading.Thread(target=tx) for _ in range(n)]
[t.start() for t in ts]; [t.join() for t in ts]
