#!/usr/bin/env python3
"""Edge cases for the write pulse: pipelining order, read-your-writes, half-close, abrupt close,
bursts of writes in one pulse, Expect: 100-continue, split requests, errors on held requests.
usage: pulse_edge.py PORT   (a server must be listening on 127.0.0.1:PORT; needs ../../seed/tokens.json)"""
import json, os, socket, struct, sys, time

PORT = int(sys.argv[1])
TOK = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../seed/tokens.json")))
fails = 0

def check(name, cond, detail=""):
    global fails
    print(("  ok   " if cond else "  FAIL ") + name + ("" if cond else "  " + str(detail)[:300]))
    if not cond: fails += 1

def conn():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s

def read_responses(s, n):
    """Read n complete HTTP responses; returns list of (status, body)."""
    buf = b""; out = []
    while len(out) < n:
        while True:
            i = buf.find(b"\r\n\r\n")
            if i >= 0:
                head = buf[:i].decode("latin1")
                status = int(head.split(" ")[1])
                cl = 0
                for line in head.split("\r\n")[1:]:
                    if line.lower().startswith("content-length:"): cl = int(line.split(":")[1])
                if status == 100:
                    buf = buf[i + 4:]; out.append((100, b"")); break
                if len(buf) >= i + 4 + cl:
                    out.append((status, buf[i + 4:i + 4 + cl])); buf = buf[i + 4 + cl:]; break
            d = s.recv(65536)
            if not d: return out, buf
            buf += d
    return out, buf

def req(method, path, tok=None, body=None, extra=""):
    h = "%s %s HTTP/1.1\r\nHost: x\r\n" % (method, path)
    if tok: h += "Authorization: Bearer %s\r\n" % tok
    if body is not None: h += "Content-Type: application/json\r\nContent-Length: %d\r\n" % len(body)
    return (h + extra + "\r\n").encode() + (body or b"")

def post_body(text): return json.dumps({"body": text}).encode()

t0 = TOK[0]["token"]; t1 = TOK[1]["token"]

print("-- pipelining and read-your-writes")
s = conn()
s.sendall(req("POST", "/posts", t0, post_body("edge one")))
(r,), _ = read_responses(s, 1)
check("create -> 201", r[0] == 201, r)
pid = json.loads(r[1])["post"]["id"]
s.sendall(req("POST", "/posts/%d/like" % pid, t0) + req("GET", "/posts/%d" % pid))
rs, _ = read_responses(s, 2)
check("pipelined like + get: order and statuses", [x[0] for x in rs] == [201, 200], rs)
check("pipelined get sees the like", json.loads(rs[1][1])["post"]["like_count"] == 1, rs[1])
s.sendall(req("GET", "/feed") + req("POST", "/posts", t0, post_body("edge two")) + req("GET", "/feed") + req("GET", "/health"))
rs, _ = read_responses(s, 4)
check("pipelined get/post/get/health: statuses in order", [x[0] for x in rs] == [200, 201, 200, 200], [x[0] for x in rs])
newid = json.loads(rs[1][1])["post"]["id"]
check("feed before the post does not have it", json.loads(rs[0][1])["posts"][0]["id"] != newid)
check("feed after the post starts with it", json.loads(rs[2][1])["posts"][0]["id"] == newid)
# many writes pipelined on one connection: each waits for its own pulse, order kept
s.sendall(b"".join(req("POST", "/posts", t0, post_body("pipe %d" % i)) for i in range(12)))
rs, _ = read_responses(s, 12)
ids = [json.loads(b)["post"]["id"] for st, b in rs if st == 201]
check("12 pipelined posts -> 12 x 201, ids ascending", len(ids) == 12 and ids == sorted(ids) and len(set(ids)) == 12, rs[:2])
check("bodies in request order", [json.loads(b)["post"]["body"] for st, b in rs] == ["pipe %d" % i for i in range(12)])
s.close()

print("-- errors on held requests")
s = conn()
s.sendall(req("POST", "/posts", "garbage.token.here", post_body("x")) + req("POST", "/posts/abc/like") +
          req("POST", "/posts/abc/like", t0) + req("POST", "/posts/999999999/like", t0) + req("POST", "/posts", t0, b"{nope"))
rs, _ = read_responses(s, 5)
check("401, 401, 400, 404, 400 in order", [x[0] for x in rs] == [401, 401, 400, 404, 400], [x[0] for x in rs])
s.close()

print("-- half-close and abrupt close")
s = conn()
s.sendall(req("POST", "/posts", t0, post_body("half close")))
s.shutdown(socket.SHUT_WR)
rs, rest = read_responses(s, 1)
check("POST then FIN: still answered 201", len(rs) == 1 and rs[0][0] == 201, rs)
hc = json.loads(rs[0][1])["post"]["id"] if rs else 0
try: tail = s.recv(10)
except Exception as e: tail = b"err"
check("server closes after answering", tail == b"")
s.close()
for i in range(50):
    s = conn()
    s.sendall(req("POST", "/posts", t1, post_body("rst %d" % i)))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()
time.sleep(0.3)
s = conn(); s.sendall(req("GET", "/health")); rs, _ = read_responses(s, 1)
check("server healthy after 50 POST+RST", rs and rs[0][0] == 200, rs)
s.sendall(req("GET", "/posts/%d" % hc)); rs, _ = read_responses(s, 1)
check("half-closed post is readable", rs[0][0] == 200 and json.loads(rs[0][1])["post"]["body"] == "half close")
s.close()

print("-- a burst of writes from many connections (one pulse, one commit)")
N = 300
socks = [conn() for _ in range(N)]
for i, s in enumerate(socks): s.sendall(req("POST", "/posts", TOK[i]["token"], post_body("burst %d" % i)))
got = {}
for i, s in enumerate(socks):
    rs, _ = read_responses(s, 1)
    if rs and rs[0][0] == 201:
        p = json.loads(rs[0][1])["post"]; got[i] = p
check("300 posts -> 300 x 201", len(got) == N, len(got))
check("ids distinct", len({p["id"] for p in got.values()}) == N)
check("each answer carries its own body and author", all(p["body"] == "burst %d" % i and p["author"] == TOK[i]["username"] for i, p in got.items()))
# likes from all of them on one post, then the count
target = got[0]["id"]
for i, s in enumerate(socks): s.sendall(req("POST", "/posts/%d/like" % target, TOK[i]["token"]))
st = [read_responses(s, 1)[0][0][0] for s in socks]
check("300 likes -> 300 x 201", st.count(201) == N, (st.count(201), st.count(200)))
for i, s in enumerate(socks): s.sendall(req("POST", "/posts/%d/like" % target, TOK[i]["token"]))
st = [read_responses(s, 1)[0][0][0] for s in socks]
check("repeat likes -> 300 x 200", st.count(200) == N, (st.count(201), st.count(200)))
socks[0].sendall(req("GET", "/posts/%d" % target)); rs, _ = read_responses(socks[0], 1)
check("like_count is 300", json.loads(rs[0][1])["post"]["like_count"] == N, rs[0][1][:200])
for s in socks: s.close()

print("-- Expect: 100-continue, split requests, HTTP/1.0")
s = conn()
b = post_body("expect")
s.sendall(req("POST", "/posts", t0, None, "Content-Type: application/json\r\nContent-Length: %d\r\nExpect: 100-continue\r\n" % len(b)))
rs, _ = read_responses(s, 1)
check("100 Continue", rs and rs[0][0] == 100, rs)
s.sendall(b); rs, _ = read_responses(s, 1)
check("then 201", rs and rs[0][0] == 201, rs)
full = req("POST", "/posts", t0, post_body("split request"))
for cut in (10, 60, len(full) - 5):
    s.sendall(full[:cut]); time.sleep(0.03); s.sendall(full[cut:])
    rs, _ = read_responses(s, 1)
    check("request split at byte %d -> 201" % cut, rs and rs[0][0] == 201, rs)
s.close()
s = conn()
s.sendall(("POST /posts HTTP/1.0\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n" % (t0, len(b))).encode() + b)
rs, _ = read_responses(s, 1)
check("HTTP/1.0 POST -> 201 then close", rs and rs[0][0] == 201 and s.recv(10) == b"", rs)
s.close()

print("-- a slow reader behind a write (the write must not block other connections)")
a = conn(); b2 = conn()
a.sendall(req("POST", "/posts", t0, post_body("a")))
t = time.time(); b2.sendall(req("GET", "/feed")); rs, _ = read_responses(b2, 1)
check("read on another connection answered promptly (%.1f ms)" % ((time.time() - t) * 1000), rs[0][0] == 200 and time.time() - t < 0.2)
rs, _ = read_responses(a, 1); check("and the write is answered", rs[0][0] == 201)
a.close(); b2.close()

print("edge: failed=%d" % fails)
sys.exit(1 if fails else 0)
