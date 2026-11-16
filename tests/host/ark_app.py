#!/usr/bin/env python3
"""ark_app.py <p-kernel> <workdir> <port> - drives one hosted node for
tests/host/run_ark_app.sh (ark-entry.md §7.3 A7: who may call galaxy).
Prints "[ark-app] <check> PASS|FAIL" lines and a few "[ark-app] note" lines.

The checks (fixed before the code, ark-entry.md §7.2 6 and §7.6):
  A7-file      PKERNEL_PFS_DIR/galaxy.token is 64 hex, mode 0600, and a new
               boot writes a different one
  A7-public    /, /galaxy.json, /modules.json, /manifesto, /vocab, /langs
               answer 200 with no pass
  A7-no-token  every guarded route (all POSTs, the GETs that carry the person,
               and an unknown path) answers 401 with no pass
  A7-bad-token a pass one byte off, a short pass, and a pass in the wrong
               place (?t= on a plain GET) all answer 401
  A7-good-token the same routes answer with the pass in X-Galaxy-Token
               (200; /nope 404), /events?t=<pass> 200 and /ws?t=<pass> 101
  A7-origin    Origin: http://evil.example with the right pass -> 403 and the
               profile is not changed
  A7-host      Host: evil.example:<port> with the right pass -> 403 and the
               profile is not changed; Host: localhost:<port> is fine
  A7-options   OPTIONS is refused (400 or 405) or the socket closes
  A7-nowrite   the pass-less POSTs wrote nothing: the profile seq and handle
               are the same, and `mind provenance` has the word taught WITH
               the pass but not the one sent without it
"""
import os, re, socket, stat, subprocess, sys, time

pk, work, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
HOSTH = "127.0.0.1:%d" % port
pfs = os.path.join(work, "pfs")
os.makedirs(pfs, exist_ok=True)
env = dict(os.environ, PKERNEL_NODE_ID="1", PKERNEL_PFS_DIR=pfs,
           PKERNEL_GALAXY_PORT=str(port))
logpath = os.path.join(work, "node.log")


def res(name, ok, why=""):
    print("[ark-app] %s %s%s" % (name, "PASS" if ok else "FAIL",
                                 ("  (" + why + ")") if (why and not ok) else ""), flush=True)


def raw(method, path, body=None, headers=None, first_line_only=False, timeout=120):
    """One request on a fresh socket. Returns (status:int|None, headers:str, body:bytes).
    status None = the node closed without a status line."""
    h = {"Host": HOSTH}
    if headers:
        h.update(headers)
    b = body.encode() if isinstance(body, str) else (body or b"")
    if method == "POST":
        h.setdefault("Content-Type", "application/x-www-form-urlencoded")
        h["Content-Length"] = str(len(b))
    req = "%s %s HTTP/1.1\r\n" % (method, path)
    for k, v in h.items():
        if v is None:
            continue
        req += "%s: %s\r\n" % (k, v)
    req += "Connection: close\r\n\r\n"
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    data = b""
    try:
        s.sendall(req.encode() + b)
        while True:
            try:
                c = s.recv(65536)
            except socket.timeout:
                break
            if not c:
                break
            data += c
            if first_line_only and b"\r\n\r\n" in data:
                break
    except (ConnectionError, OSError):
        pass
    finally:
        s.close()
    m = re.match(rb"HTTP/1\.[01] (\d{3})", data)
    if not m:
        return None, "", data
    he = data.find(b"\r\n\r\n")
    return int(m.group(1)), data[:he].decode("latin-1"), data[he + 4:] if he >= 0 else b""


class Node:
    def __init__(self, tag):
        self.log = open(logpath, "ab")
        self.log.write(("\n===== boot %s =====\n" % tag).encode())
        self.log.flush()
        self.p = subprocess.Popen([pk], stdin=subprocess.PIPE, stdout=self.log,
                                  stderr=subprocess.STDOUT, env=env)

    def up(self):
        for _ in range(120):
            try:
                st, _h, _b = raw("GET", "/manifesto", timeout=5)
                if st == 200:
                    return True
            except OSError:
                pass
            time.sleep(1)
        return False

    def shell(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()

    def stop(self):
        try:
            self.shell("exit")
            self.p.wait(timeout=30)
        except Exception:
            self.p.kill()
            self.p.wait()
        self.log.close()


def node_log():
    return open(logpath, "rb").read().decode("utf-8", "replace")


def read_token():
    try:
        return open(os.path.join(pfs, "galaxy.token"), "r").read().strip()
    except OSError:
        return ""


def token_file_ok():
    fp = os.path.join(pfs, "galaxy.token")
    try:
        mode = stat.S_IMODE(os.stat(fp).st_mode)
    except OSError:
        return False, "no galaxy.token"
    t = read_token()
    if not re.fullmatch(r"[0-9a-f]{64}", t):
        return False, "not 64 hex: %r" % t[:80]
    if mode != 0o600:
        return False, "mode %o" % mode
    return True, ""


def profile(tok):
    st, _h, b = raw("GET", "/profile.json", headers={"X-Galaxy-Token": tok})
    txt = b.decode("utf-8", "replace")
    seq = re.search(r'"seq":(\d+)', txt)
    hd = re.search(r'"handle":"([^"]*)"', txt)
    return st, (seq.group(1) if seq else None), (hd.group(1) if hd else None), txt


# the guarded routes: (method, path, body). Each must be 401 without the pass.
GUARDED = [
    ("GET", "/self.json", None), ("GET", "/profile.json", None),
    ("GET", "/console.txt", None), ("GET", "/log.txt", None),
    ("GET", "/events", None), ("GET", "/ws", None),
    ("GET", "/nope", None), ("GET", "/ark/rec.json", None),
    ("POST", "/teach", "k=sea&v=green"), ("POST", "/ask", "k=sky"),
    ("POST", "/profile", "ack=1&handle=evil"), ("POST", "/nope", "x=1"),
]
PUBLIC = ["/", "/galaxy.json", "/modules.json", "/manifesto", "/vocab", "/langs"]
WS_HDR = {"Upgrade": "websocket", "Connection": "Upgrade",
          "Sec-WebSocket-Key": "dGhlIHNhbXBsZSBub25jZQ==", "Sec-WebSocket-Version": "13"}


def stream_status(path, tok_in_query, extra=None):
    q = ("?t=" + tok_in_query) if tok_in_query is not None else ""
    h = dict(extra or {})
    if path == "/ws":
        h.update(WS_HDR)
    st, _h, _b = raw("GET", path + q, headers=h, first_line_only=True, timeout=10)
    return st


open(logpath, "wb").close()
tok1 = ""
try:
    # ---- boot 1: the file, then shut down to see a new pass at boot 2 ----
    n = Node("1")
    up = n.up()
    print("[ark-app] note boot1 up=%s" % up, flush=True)
    tok1 = read_token()
    n.stop()

    n = Node("2")
    up = n.up()
    tok = read_token()
    ok, why = token_file_ok()
    res("A7-file", up and ok and tok != tok1, why or ("same pass at boot 2" if tok == tok1 else ""))
    H = {"X-Galaxy-Token": tok}

    # consent (with the pass) so /teach would be heard
    st, hd, _b = raw("GET", "/manifesto")
    mid = re.search(r"X-Manifesto-Id: ([0-9a-f]{64})", hd)
    mid = mid.group(1) if mid else ""
    st, _h, _b = raw("POST", "/profile", "ack=1&mid=%s&handle=owner" % mid, headers=H)
    print("[ark-app] note consent with the pass: %s" % st, flush=True)
    st, seq0, hd0, _t = profile(tok)
    print("[ark-app] note profile before: %s seq=%s handle=%s" % (st, seq0, hd0), flush=True)

    # A7-public
    bad = []
    for p in PUBLIC:
        st, _h, _b = raw("GET", p)
        if st != 200:
            bad.append("%s=%s" % (p, st))
    res("A7-public", not bad, " ".join(bad))

    # A7-no-token
    bad = []
    for m, p, b in GUARDED:
        if p in ("/events", "/ws"):
            st = stream_status(p, None)
        else:
            if p == "/profile":
                b = "ack=1&mid=%s&handle=evil" % mid
            st, _h, _b = raw(m, p, b)
        if st != 401:
            bad.append("%s %s=%s" % (m, p, st))
    res("A7-no-token", not bad, " ".join(bad))

    # A7-bad-token: one byte off, short, and ?t= on a plain GET
    bad = []
    if len(tok) == 64:
        off = tok[:-1] + ("0" if tok[-1] != "0" else "1")
        short = tok[:63]
    else:
        off, short = "0" * 64, "0" * 63
    for name, hh in (("off", {"X-Galaxy-Token": off}), ("short", {"X-Galaxy-Token": short})):
        for m, p, b in (("GET", "/self.json", None), ("POST", "/teach", "k=sea&v=green"),
                        ("POST", "/profile", "ack=1&mid=%s&handle=evil" % mid)):
            st, _h, _b = raw(m, p, b, headers=hh)
            if st != 401:
                bad.append("%s %s %s=%s" % (name, m, p, st))
    st, _h, _b = raw("GET", "/self.json?t=" + tok)
    if st != 401:
        bad.append("query-on-get=%s" % st)
    for p in ("/events", "/ws"):
        st = stream_status(p, off)
        if st != 401:
            bad.append("off %s=%s" % (p, st))
    res("A7-bad-token", not bad, " ".join(bad))
    st, seq1, hd1, _t = profile(tok)     # read now: A7-good-token writes "owner" again

    # A7-good-token
    bad = []
    for m, p, b in GUARDED:
        if p in ("/events", "/ws", "/ark/rec.json"):
            continue
        if p == "/profile":
            b = "ack=1&mid=%s&handle=owner" % mid
        st, _h, _b = raw(m, p, b, headers=H)
        want = 404 if p == "/nope" else 200
        if st != want:
            bad.append("%s %s=%s" % (m, p, st))
    st = stream_status("/events", tok)
    if st != 200:
        bad.append("/events?t=%s" % st)
    st = stream_status("/ws", tok)
    if st != 101:
        bad.append("/ws?t=%s" % st)
    st, _h, _b = raw("GET", "/self.json", headers=dict(H, Host="localhost:%d" % port))
    if st != 200:
        bad.append("host-localhost=%s" % st)
    st, _h, _b = raw("GET", "/self.json", headers=dict(H, Origin="http://127.0.0.1:%d" % port))
    if st != 200:
        bad.append("origin-self=%s" % st)
    st, _h, _b = raw("POST", "/teach", "k=sky&v=blue", headers=H)
    if st != 200:
        bad.append("teach-sky=%s" % st)
    res("A7-good-token", bad == [], " ".join(bad))

    # A7-origin / A7-host: right pass, wrong caller -> 403, nothing written
    st, seqa, hda, _t = profile(tok)
    o1, _h, _b = raw("POST", "/profile", "ack=1&mid=%s&handle=origin" % mid,
                     headers=dict(H, Origin="http://evil.example"))
    o2, _h, _b = raw("GET", "/self.json", headers=dict(H, Origin="http://evil.example"))
    o3, _h, _b = raw("GET", "/galaxy.json", headers={"Origin": "http://127.0.0.1:%d.evil.example" % port})
    st, seqb, hdb, _t = profile(tok)
    res("A7-origin", o1 == 403 and o2 == 403 and o3 == 403 and seqb == seqa and hdb == hda,
        "post=%s get=%s public=%s seq %s->%s handle %s->%s" % (o1, o2, o3, seqa, seqb, hda, hdb))
    h1, _h, _b = raw("POST", "/profile", "ack=1&mid=%s&handle=host" % mid,
                     headers=dict(H, Host="evil.example:%d" % port))
    h2, _h, _b = raw("GET", "/galaxy.json", headers={"Host": "evil.example:%d" % port})
    h3, _h, _b = raw("GET", "/self.json", headers=dict(H, Host="127.0.0.1:%d" % (port + 1)))
    st, seqc, hdc, _t = profile(tok)
    res("A7-host", h1 == 403 and h2 == 403 and h3 == 403 and seqc == seqb and hdc == hdb,
        "post=%s public=%s port=%s seq %s->%s handle %s->%s" % (h1, h2, h3, seqb, seqc, hdb, hdc))

    # A7-options
    st, _h, _b = raw("OPTIONS", "/teach", headers={"Origin": "http://evil.example",
                                                   "Access-Control-Request-Method": "POST",
                                                   "Access-Control-Request-Headers": "x-galaxy-token"},
                     timeout=10)
    res("A7-options", st in (None, 400, 405), "OPTIONS=%s" % st)

    # A7-nowrite: the pass-less POSTs of A7-no-token / bad-token changed nothing
    n.shell("mind provenance")
    t_end = time.time() + 60
    L = ""
    while time.time() < t_end:
        L = node_log()
        if '[mind]   "sky" -> ' in L:
            break
        time.sleep(0.5)
    time.sleep(1)
    L = node_log()
    has_sky = '[mind]   "sky" -> ' in L
    has_sea = '[mind]   "sea" -> ' in L
    res("A7-nowrite", hd1 == "owner" and seq1 == seq0 and has_sky and not has_sea,
        "handle=%s seq %s->%s sky=%s sea=%s" % (hd1, seq0, seq1, has_sky, has_sea))
    n.stop()
finally:
    pass
