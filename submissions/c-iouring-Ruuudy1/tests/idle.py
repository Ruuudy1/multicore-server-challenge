#!/usr/bin/env python3
# Opens N keep-alive connections (from several 127.0.0.x source addresses, since one address has only ~64k
# ports), sends one request on each, holds them all idle, checks a sample still answers, and reports the
# server's resident memory.
#   python3 tests/idle.py <server pid> <port> [connections, default 100000]
import resource
import selectors
import socket
import sys
import time

pid, port = int(sys.argv[1]), int(sys.argv[2])
n = int(sys.argv[3]) if len(sys.argv) > 3 else 100000
resource.setrlimit(resource.RLIMIT_NOFILE, (n + 1000, resource.getrlimit(resource.RLIMIT_NOFILE)[1]))
REQ = b"GET /api/tags HTTP/1.1\r\nHost: x\r\n\r\n"


def rss_mb(p):
    with open(f"/proc/{p}/status") as f:
        for line in f:
            if line.startswith("VmRSS"):
                return int(line.split()[1]) / 1024
    return 0.0


def drain(socks, timeout=60):
    """Reads one full /api/tags response from every socket (Content-Length framed)."""
    sel = selectors.DefaultSelector()
    pending = {}
    for s in socks:
        s.setblocking(False)
        sel.register(s, selectors.EVENT_READ)
        pending[s] = b""
    done, deadline = 0, time.time() + timeout
    while pending and time.time() < deadline:
        for key, _ in sel.select(timeout=1):
            s = key.fileobj
            try:
                chunk = s.recv(65536)
            except BlockingIOError:
                continue
            buf = pending[s] + chunk
            if b"\r\n\r\n" in buf:
                head, body = buf.split(b"\r\n\r\n", 1)
                cl = int([l for l in head.split(b"\r\n") if l.lower().startswith(b"content-length")][0].split(b":")[1])
                if len(body) >= cl and head.startswith(b"HTTP/1.1 200"):
                    sel.unregister(s)
                    del pending[s]
                    done += 1
                    continue
            pending[s] = buf
    sel.close()
    return done


before = rss_mb(pid)
socks = []
t0 = time.time()
for i in range(n):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.bind((f"127.0.0.{2 + i // 50000}", 0))
    s.connect(("127.0.0.1", port))
    socks.append(s)
    if len(socks) % 20000 == 0:
        print(f"{len(socks)} connected, server RSS {rss_mb(pid):.1f} MB", flush=True)
print(f"{n} connections in {time.time() - t0:.1f}s")
for s in socks:
    s.sendall(REQ)
ok = drain(socks)
print(f"{ok}/{n} answered; server RSS {before:.1f} MB before, {rss_mb(pid):.1f} MB with {n} connections")
time.sleep(5)
sample = socks[:: max(1, n // 2000)]
for s in sample:
    s.setblocking(True)
    s.sendall(REQ)
ok2 = drain(sample)
print(f"after idling: {ok2}/{len(sample)} sampled connections answered; server RSS {rss_mb(pid):.1f} MB")
for s in socks:
    s.close()
sys.exit(0 if ok == n and ok2 == len(sample) and rss_mb(pid) < 100 else 1)
