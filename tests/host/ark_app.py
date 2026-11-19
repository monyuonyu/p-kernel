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
    hd = re.search(r'"handle_len":(\d+)', txt)   # "owner" = 5, "evil" = 4
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


def shell_wait(n, line, pat, timeout=60):
    """send a shell line, wait for one more match of pat than before; the last match"""
    before = len(re.findall(pat, node_log()))
    n.shell(line)
    t_end = time.time() + timeout
    while time.time() < t_end:
        ms = re.findall(pat, node_log())
        if len(ms) > before:
            time.sleep(0.3)
            return re.findall(pat, node_log())[before]
        time.sleep(0.3)
    return None


def rec_ls(n):
    """(nrec, sealed id or '', {no: (seq, vis, len)}) from `ark rec ls`"""
    m = shell_wait(n, "ark rec ls", r"\[ark-rec\] index nrec=(\d+)[^\r\n]*?(?: sealed=([0-9a-f]{64}))?\r?\n")
    if m is None:
        return None, "", {}
    L = node_log()
    tail = L[L.rfind("[ark-rec] index nrec="):]
    rows = {}
    for no, seq, vis, ln in re.findall(r"\[ark-rec\]   no=(\d+) seq=(\d+) vis=(\w+) len=(\d+)", tail):
        rows[int(no)] = (int(seq), vis, int(ln))
    return int(m[0]), m[1], rows


def rec_count(n):
    return rec_ls(n)[0]


# part 2 (ark-entry.md 7.3 A1-A6): a body with multibyte text, CRLF, trailing
# spaces and a NUL, kept byte for byte
BODY1 = "朝の散歩、café \r\nline with trailing spaces   \r\n".encode() + b"\x00end\t \n"
# A5: words of counting and ranking that must not be in the record list or the page
RANK_WORDS = ["likes", "like_count", "rank", "ranking", "score", "views", "followers",
              "popular", "いいね", "順位", "人気"]

def consent_and_backup(H):
    """consent (the manifesto) and the key backup written down: the record routes open"""
    _st, hd, _b = raw("GET", "/manifesto")
    m = re.search(r"X-Manifesto-Id: ([0-9a-f]{64})", hd)
    c, _h, _b = raw("POST", "/profile", "ack=1&handle=owner&mid=%s" % (m.group(1) if m else ""), headers=H)
    b, _h, _b = raw("POST", "/ark/key/backup", "", headers=H)
    d, _h, _b = raw("POST", "/ark/key/backup/done", "", headers=H)
    return c, b, d


def lock_part():
    """A9-lock (ark-entry.md 7.10 1): the shell's `ark rec|heir|key` and the record routes
    take the same flag. Built with -DARK_REC_TEST_HOLD and PKERNEL_ARK_REC_HOLD_MS, the
    side that is inside waits HOLD ms holding it, so the other side surely comes then.
      D1 the shell is inside (`ark rec new`): POST /ark/rec answers 503 at once, writes
         nothing (the shell's record is the only one)
      D2 the page is inside (POST /ark/rec): the shell's `ark rec ls` says busy at once,
         the POST then answers 200 (two records)"""
    import threading
    hold = int(env.get("PKERNEL_ARK_REC_HOLD_MS", "0"))
    n = Node("lock")
    up = n.up()
    H = {"X-Galaxy-Token": read_token()}
    c, b, d = consent_and_backup(H)
    print("[ark-app] note lock: up=%s consent=%s backup=%s done=%s hold=%d" % (up, c, b, d, hold), flush=True)
    # D1
    before = len(re.findall(r"\[ark-rec\] new no=", node_log()))
    n.shell("ark rec new 0 from the shell")
    time.sleep(0.8)
    t = time.time()
    s1, _h, _b = raw("POST", "/ark/rec", b"from the page while the shell is inside", headers=H)
    e1 = time.time() - t
    t_end = time.time() + 30
    while time.time() < t_end and len(re.findall(r"\[ark-rec\] new no=", node_log())) <= before:
        time.sleep(0.2)
    n1 = rec_count(n)
    d1 = s1 == 503 and e1 < hold / 1000.0 - 1.0 and n1 == 1
    # D2
    out = {}
    def post():
        out["s"], _h2, _b2 = raw("POST", "/ark/rec", b"from the page, the shell comes", headers=H)
    th = threading.Thread(target=post)
    th.start()
    time.sleep(0.8)
    busy = shell_wait(n, "ark rec ls", r"\[ark\] busy[^\r\n]*|\[ark-rec\] index nrec=\d+", timeout=hold / 1000.0 + 30)
    th.join(timeout=60)
    n2 = rec_count(n)
    d2 = busy is not None and busy.startswith("[ark] busy") and out.get("s") == 200 and n2 == 2
    n.stop()
    res("A9-lock", d1 and d2, "D1 post=%s in %.1f s nrec=%s / D2 shell=%r post=%s nrec=%s"
        % (s1, e1, n1, busy, out.get("s"), n2))


open(logpath, "wb").close()
if os.environ.get("ARK_APP_PART") == "lock":
    lock_part()
    sys.exit(0)
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

    # A1-gate (part 2): before the consent a record is refused, nothing written
    g403, _h, _b = raw("POST", "/ark/rec", b"before the consent", headers=H)
    nrec_pre = rec_count(n)

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
        if p == "/teach":
            b = "k=ink&v=fresh"   # not "sea": A7-nowrite looks for sea (sent without the pass)
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
    res("A7-nowrite", hd1 == "5" and seq1 == seq0 and has_sky and not has_sea,
        "handle_len=%s seq %s->%s sky=%s sea=%s" % (hd1, seq0, seq1, has_sky, has_sea))

    # ---- part 2: the record routes (ark-entry.md 7.2 1-4, 7.3 A1-A6) ----
    res("A1-gate", g403 == 403 and nrec_pre == 0, "before consent=%s nrec=%s" % (g403, nrec_pre))
    # A4: after the consent, before "written down", the record route is 409
    c409, _h, _b = raw("POST", "/ark/rec", b"before the backup", headers=H)
    n409 = rec_count(n)
    b1, _h, key1 = raw("POST", "/ark/key/backup", "", headers=H)
    b2, _h, _b2 = raw("POST", "/ark/key/backup", "", headers=H)
    bg, _h, bgb = raw("GET", "/ark/key/backup", headers=H)
    try:
        kfile = open(os.path.join(pfs, "ark.key"), "rb").read().hex()
    except OSError:
        kfile = ""
    key1 = key1.decode("latin-1").strip()
    bd, _h, _b = raw("POST", "/ark/key/backup/done", "", headers=H)
    print("[ark-app] note backup: 409=%s nrec=%s first=%s second=%s get=%s done=%s key=%s file=%s"
          % (c409, n409, b1, b2, bg, bd, key1[:8], kfile[:8]), flush=True)

    # A1: a record, read back byte for byte
    _n0, sealed0, _r = rec_ls(n)
    p1, _h, j1 = raw("POST", "/ark/rec", BODY1, headers=dict(H, **{"Content-Type": "application/octet-stream"}))
    m = re.search(rb'"no":(\d+),"seq":(\d+)', j1)
    no1 = int(m.group(1)) if m else -1
    g1, _h, back = raw("GET", "/ark/rec/%d" % no1, headers=H)
    res("A1", p1 == 200 and m is not None and int(m.group(2)) == 1 and g1 == 200 and back == BODY1,
        "post=%s %r get=%s same=%s" % (p1, j1[:40], g1, back == BODY1))
    # A2: the shell sees it, the sealed index id moved
    nr2, sealed1, rows = rec_ls(n)
    res("A2", nr2 == 1 and rows.get(no1, (0,))[0] == 1 and rows.get(no1, (0, 0, 0))[2] == len(BODY1)
        and sealed1 != "" and sealed1 != sealed0,
        "nrec=%s rows=%s sealed %s->%s" % (nr2, rows, sealed0[:8], sealed1[:8]))
    # A3: vis and learn add versions; the first version is self / no
    st0, _h, js0 = raw("GET", "/ark/rec.json", headers=H)
    v1, _h, jv = raw("POST", "/ark/rec/%d/vis" % no1, "public", headers=H)
    l1, _h, jl = raw("POST", "/ark/rec/%d/learn" % no1, "yes", headers=H)
    s2, _h, js2 = raw("GET", "/ark/rec.json", headers=H)
    t0 = js0.decode("utf-8", "replace"); t2 = js2.decode("utf-8", "replace")
    ok3 = (st0 == 200 and '"vis":"self"' in t0 and '"learn":"no"' in t0 and '"seq":1' in t0
           and v1 == 200 and b'"seq":2' in jv and l1 == 200 and b'"seq":3' in jl
           and s2 == 200 and '"vis":"public"' in t2 and '"learn":"yes"' in t2 and '"seq":3' in t2)
    g3, _h, back3 = raw("GET", "/ark/rec/%d" % no1, headers=H)
    res("A3", ok3 and back3 == BODY1, "json0=%r vis=%s %r learn=%s %r json2=%r same=%s"
        % (t0[:120], v1, jv[:30], l1, jl[:30], t2[:120], back3 == BODY1))
    # A5: no counting / ranking words in the list or the page
    _s, _h, page = raw("GET", "/")
    # whole words (the page has "WebViews" in a CSS comment: not a count of views)
    pg = page.decode("utf-8", "replace").lower()
    hits = [w for w in RANK_WORDS
            if re.search(r"(?<![a-z_])%s(?![a-z_])" % re.escape(w), t2.lower())
            or re.search(r"(?<![a-z_])%s(?![a-z_])" % re.escape(w), pg)]
    res("A5", s2 == 200 and not hits, "words=%s" % hits)
    # A6: 16 KB is the limit of one POST; over it is 413 and nothing written
    big_ok, _h, _b = raw("POST", "/ark/rec", b"x" * 16384, headers=H)
    nr6a = rec_count(n)
    big_no, _h, _b = raw("POST", "/ark/rec", b"y" * 16385, headers=H)
    nr6b = rec_count(n)
    res("A6", big_ok == 200 and big_no == 413 and nr6a == 2 and nr6b == 2,
        "16384=%s 16385=%s nrec %s->%s" % (big_ok, big_no, nr6a, nr6b))
    # AH (7.2 1, 4): the heir's public key by HTTP: set, read back, clear, refuse a bad one
    HEIR = "ab" * 32
    h1, _h, _b = raw("POST", "/ark/heir", HEIR, headers=H)
    h2, _h, hb2 = raw("GET", "/ark/heir", headers=H)
    h3, _h, _b = raw("POST", "/ark/heir", "zz", headers=H)
    h4, _h, hb4 = raw("GET", "/ark/heir", headers=H)
    h5, _h, _b = raw("POST", "/ark/heir", "clear", headers=H)
    h6, _h, hb6 = raw("GET", "/ark/heir", headers=H)
    res("AH", h1 == 200 and h2 == 200 and ('"heir":"%s"' % HEIR).encode() in hb2 and h3 == 400
        and h4 == 200 and ('"heir":"%s"' % HEIR).encode() in hb4 and h5 == 200 and h6 == 200
        and b'"heir":null' in hb6,
        "set=%s get=%s %r bad=%s after-bad=%r clear=%s get=%s %r" % (h1, h2, hb2[:90], h3, hb4[:30], h5, h6, hb6[:40]))
    # A4 (this boot): 409 before "written down" wrote nothing; the key once, as the file
    a4 = (c409 == 409 and n409 == 0 and b1 == 200 and re.fullmatch(r"[0-9a-f]{64}", key1 or "")
          and key1 == kfile and b2 == 410 and bg in (404, 405) and bd == 200)
    n.stop()

    # boot 3: the key is not shown again after a restart
    n = Node("3")
    n.up()
    b3, _h, _b = raw("POST", "/ark/key/backup", "", headers={"X-Galaxy-Token": read_token()})
    r3, _h, back4 = raw("GET", "/ark/rec/%d" % no1, headers={"X-Galaxy-Token": read_token()})
    _n3, sealed3, _r3 = rec_ls(n)
    n.stop()
    res("A4", a4 and b3 == 410 and r3 == 200 and back4 == BODY1,
        "409=%s nrec=%s first=%s second=%s get=%s done=%s same-as-file=%s restart=%s"
        % (c409, n409, b1, b2, bg, bd, key1 == kfile, b3))

    # A4-import: the written-down key on an empty node opens the sealed copies;
    # a node that has ark.key refuses the import and keeps its bytes
    pfs2 = os.path.join(work, "pfs2")
    os.makedirs(pfs2, exist_ok=True)
    for f in os.listdir(pfs):        # the blocks only (as the net would bring them)
        if re.fullmatch(r"[0-9a-f]{64}(\.org)?", f):
            with open(os.path.join(pfs, f), "rb") as a, open(os.path.join(pfs2, f), "wb") as b:
                b.write(a.read())
    env2 = dict(env, PKERNEL_PFS_DIR=pfs2, PKERNEL_GALAXY="0")
    env_keep = env
    env = env2
    n = Node("import")
    time.sleep(3)
    imp = shell_wait(n, "ark key import " + (key1 or "0" * 64), r"\[ark-key\] import[^\r\n]*")
    shell_wait(n, "ark rec restore " + (sealed3 or "0" * 64), r"\[ark-rec\] restore[^\r\n]*", timeout=120)
    sm = shell_wait(n, "ark rec sum %d" % max(no1, 0), r"\[ark-rec\] sum no=\d+ seq=\d+ len=\d+ sha256=([0-9a-f]{64})")
    k2 = open(os.path.join(pfs2, "ark.key"), "rb").read() if os.path.exists(os.path.join(pfs2, "ark.key")) else b""
    other = "1" * 64
    ref = shell_wait(n, "ark key import " + other, r"\[ark-key\] import[^\r\n]*")
    k3 = open(os.path.join(pfs2, "ark.key"), "rb").read() if os.path.exists(os.path.join(pfs2, "ark.key")) else b""
    n.stop()
    env = env_keep
    import hashlib
    res("A4-import", imp is not None and "written" in imp and k2.hex() == key1
        and sm == hashlib.sha256(BODY1).hexdigest() and ref is not None and "refused" in ref and k3 == k2,
        "import=%r sum=%s refuse=%r kept=%s" % (imp, (sm or "")[:8], ref, k3 == k2))

    # A1-gate2 (ark-entry.md 7.10 2): the key written down (an import sets the marks)
    # but no consent on this node: POST /ark/rec is 403 and writes nothing. A1-gate
    # only saw the node before the backup, where a missing consent gate shows as 409.
    pfs3 = os.path.join(work, "pfs3")
    os.makedirs(pfs3, exist_ok=True)
    env = dict(env_keep, PKERNEL_PFS_DIR=pfs3)
    pfs_keep, pfs = pfs, pfs3
    n = Node("gate2")
    n.up()
    imp3 = shell_wait(n, "ark key import " + "2" * 64, r"\[ark-key\] import[^\r\n]*")
    marks = os.path.exists(os.path.join(pfs3, "ark.key.done"))
    g2, _h, gb2 = raw("POST", "/ark/rec", b"after the backup, before the consent",
                      headers={"X-Galaxy-Token": read_token()})
    nr3 = rec_count(n)
    n.stop()
    env, pfs = env_keep, pfs_keep
    res("A1-gate2", imp3 is not None and "written" in imp3 and marks and g2 == 403 and nr3 == 0,
        "import=%r done-mark=%s post=%s %r nrec=%s" % (imp3, marks, g2, gb2[:40], nr3))
finally:
    pass
