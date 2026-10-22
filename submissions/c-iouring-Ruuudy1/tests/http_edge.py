#!/usr/bin/env python3
# HTTP edge cases the conformance suite doesn't reach: pipelining, partial writes, chunked bodies,
# Expect: 100-continue, HEAD, HTTP/1.0, Connection: close, oversized and malformed requests.
#   python3 tests/http_edge.py [host] [port]
import json
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 3110
fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails += 1


def connect():
    s = socket.create_connection((HOST, PORT), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def read_responses(s, count, timeout=10):
    """Reads count responses (Content-Length framed). Returns [(status, headers, body)]."""
    s.settimeout(timeout)
    buf, out = b"", []
    while len(out) < count:
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                return out
            buf += chunk
        head, rest = buf.split(b"\r\n\r\n", 1)
        lines = head.decode().split("\r\n")
        status = int(lines[0].split()[1])
        headers = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:]}
        n = int(headers.get("content-length", "0"))
        while len(rest) < n:
            chunk = s.recv(65536)
            if not chunk:
                break
            rest += chunk
        out.append((status, headers, rest[:n]))
        buf = rest[n:]
    return out


def closed(s):
    s.settimeout(3)
    try:
        return s.recv(1) == b""
    except (ConnectionResetError, socket.timeout):
        return True


# pipelined requests answered in order
s = connect()
s.sendall(b"GET /api/tags HTTP/1.1\r\nHost: x\r\n\r\nGET /api/nope HTTP/1.1\r\nHost: x\r\n\r\nGET /api/articles?limit=1 HTTP/1.1\r\nHost: x\r\n\r\n")
r = read_responses(s, 3)
check([x[0] for x in r] == [200, 404, 200], f"pipelined: statuses {[x[0] for x in r]}")
check(len(r) == 3 and "articles" in json.loads(r[2][2]), "pipelined: third response is the list")

# a request split across many small writes
s.sendall(b"GET /api/ar")
time.sleep(0.05)
s.sendall(b"ticles?limit=2 HTTP/1.1\r\nHo")
time.sleep(0.05)
s.sendall(b"st: x\r\n\r\n")
r = read_responses(s, 1)
check(r and r[0][0] == 200 and len(json.loads(r[0][2])["articles"]) == 2, "split request")

# a body that arrives after the headers
body = b'{"user":{"email":"nobody@example.com","password":"x"}}'
s.sendall(b"POST /api/users/login HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n" % len(body))
time.sleep(0.05)
s.sendall(body)
r = read_responses(s, 1)
check(r and r[0][0] == 401, f"late body: {r[0][0] if r else None}")

# chunked body
chunks = b"%x\r\n%s\r\n0\r\n\r\n" % (len(body), body)
s.sendall(b"POST /api/users/login HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n" + chunks)
r = read_responses(s, 1)
check(r and r[0][0] == 401, f"chunked body: {r[0][0] if r else None}")

# Expect: 100-continue
s.sendall(b"POST /api/users/login HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: %d\r\n\r\n" % len(body))
s.settimeout(5)
interim = s.recv(64)
check(interim.startswith(b"HTTP/1.1 100 Continue\r\n\r\n"), f"100 Continue: {interim[:30]!r}")
rest = interim[len(b"HTTP/1.1 100 Continue\r\n\r\n"):]
s.sendall(body)
r = read_responses(s, 1)
check(r and r[0][0] == 401, "body after 100 Continue")

# HEAD: headers only, same Content-Length
s.sendall(b"HEAD /api/tags HTTP/1.1\r\nHost: x\r\n\r\nGET /api/tags HTTP/1.1\r\nHost: x\r\n\r\n")
s.settimeout(5)
data = b""
while data.count(b"HTTP/1.1") < 2 or not data.endswith(b"]}"):
    chunk = s.recv(65536)
    if not chunk:
        break
    data += chunk
head1, rest = data.split(b"\r\n\r\n", 1)
check(head1.startswith(b"HTTP/1.1 200") and rest.startswith(b"HTTP/1.1 200"), "HEAD has no body, next response follows")
s.close()

# Connection: close
s = connect()
s.sendall(b"GET /api/tags HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
r = read_responses(s, 1)
check(r and r[0][0] == 200 and r[0][1].get("connection") == "close", "Connection: close header")
check(closed(s), "closed after Connection: close")

# HTTP/1.0 without and with keep-alive
s = connect()
s.sendall(b"GET /api/tags HTTP/1.0\r\n\r\n")
r = read_responses(s, 1)
check(r and r[0][0] == 200 and closed(s), "HTTP/1.0 closes")
s = connect()
s.sendall(b"GET /api/tags HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
r = read_responses(s, 1)
check(r and r[0][1].get("connection") == "keep-alive", "HTTP/1.0 keep-alive")
s.sendall(b"GET /api/tags HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
check(len(read_responses(s, 1)) == 1, "HTTP/1.0 keep-alive second request")
s.close()

# malformed and oversized requests
for raw, want, label in [
    (b"GARBAGE\r\n\r\n", 400, "malformed request line"),
    (b"GET / HTTP/1.1\r\nX: " + b"a" * 20000 + b"\r\n\r\n", 431, "headers over 16 KB"),
    (b"POST /api/users HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n", 413, "body over the limit"),
    (b"POST /api/users HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n", 501, "unsupported transfer encoding"),
    (b"GET /api/profiles/%zz HTTP/1.1\r\n\r\n", 400, "bad percent escape in the path"),
]:
    s = connect()
    s.sendall(raw)
    r = read_responses(s, 1)
    check(r and r[0][0] == want and closed(s), f"{label}: {r[0][0] if r else None}")

# routing details
s = connect()
for path, want in [("/api/tags/", 404), ("/api//tags", 404), ("/api/%61rticles?limit=1", 200), ("/api/articles/feed", 401),
                   ("/api/tags?x=1", 200), ("/api/profiles/no_such_user_zz", 404), ("/api/articles?limit=1&limit=500", 200)]:
    s.sendall(b"GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path.encode())
    r = read_responses(s, 1)
    check(r and r[0][0] == want, f"GET {path}: {r[0][0] if r else None}")
s.sendall(b"PATCH /api/user HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
r = read_responses(s, 1)
check(r and r[0][0] == 404, "PATCH -> 404")
s.close()

# many idle connections stay open
conns = [connect() for _ in range(200)]
time.sleep(1)
for c in conns[:20]:
    c.sendall(b"GET /api/tags HTTP/1.1\r\nHost: x\r\n\r\n")
check(all(read_responses(c, 1)[0][0] == 200 for c in conns[:20]), "idle connections still served")
for c in conns:
    c.close()

print(f"{fails} failures")
sys.exit(1 if fails else 0)
