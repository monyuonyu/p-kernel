#!/usr/bin/env python3
"""ark_entry_dom.py <p-kernel> <workdir> <port> — ark-entry.md 7.3 A8 and 7.2 4:
the page's own first screens, driven in headless Chromium against a real hosted
node (the real galaxy.html as built into the binary, the real /ark routes).

The order the page must walk (7.2 4): the manifesto -> consent -> the key backup ->
the heir -> the first record. Checks (fixed before the screens were written):
  A8-url     opened as /#t=<pass>: the page moved the pass to sessionStorage and
             took it out of the URL (history.replaceState)
  A8-ask     (added 2026-11-21 after the first form) the key step opens on the
             explanation: no POST /ark/key/backup and no key in the DOM until "show"
  A8-order   after consent the key screen is the only step shown; the heir and the
             record steps are not shown until "written down"; the record step not
             until the heir is set or skipped. The page's POSTs come in this order:
             /profile, /ark/key/backup, /ark/key/backup/done, /ark/heir, /ark/rec
  A8-key     the 64 hex on the key screen are ark.key on disk; a second
             POST /ark/key/backup (with the pass, from outside) is 410
  A8-clear   after "written down" the 64 hex are gone from the DOM (added after the
             screens were written, with its negative nc-keep-key)
  A8-header  every request the page made to a guarded route carried
             X-Galaxy-Token = the pass, and none was answered 401
  A8-heir    GET /ark/heir (with the pass) is the 64 hex typed on the heir step
  A8-rec     GET /ark/rec.json has one record; GET /ark/rec/<no> is the UTF-8 of what
             was typed, byte for byte (multibyte, trailing blanks, blank lines)
  A8-done    after the record the overlay is closed; a reload (no #t=, the pass from
             sessionStorage) shows neither the intro nor the entry screens
Second node (a fresh store), the "come back" path:
  A8-later   (added with A8-ask) "later" on the key step closes the entry with no
             key POST; a reload opens the key step again
  A8-resume  the key was shown, the page reloaded before "written down": the entry
             opens on the key step again, says it was shown once (#aekeygone, the
             route is 410) and does not show the key; "written down" goes on
  A8-skip    "skip" on the heir step: GET /ark/heir is null, the record still writes
Prints "[ark-dom] <check> PASS|FAIL". Exit 0 only if every check ran and passed.
The browser: PK_CHROMIUM = a chrome-headless-shell / chromium binary, else
Playwright's own download (CI: `python3 -m playwright install chromium`).
"""
import hashlib, json, os, re, socket, subprocess, sys, time

from playwright.sync_api import sync_playwright

pk, work, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
HOSTH = "127.0.0.1:%d" % port
BASE = "http://%s" % HOSTH
HEIR = "ab" * 31 + "cd"
REC_TEXT = "朝の散歩、café   \n\n二行目のあと空白  \nend\t"
RESULTS = {}


def res(name, ok, why=""):
    RESULTS[name] = bool(ok)
    print("[ark-dom] %s %s%s" % (name, "PASS" if ok else "FAIL",
                                 ("  (" + why + ")") if (why and not ok) else ""), flush=True)


def raw(method, path, body=b"", tok=None):
    h = "Host: %s\r\n" % HOSTH
    if tok:
        h += "X-Galaxy-Token: %s\r\n" % tok
    if method == "POST":
        h += "Content-Type: text/plain\r\nContent-Length: %d\r\n" % len(body)
    s = socket.create_connection(("127.0.0.1", port), timeout=30)
    data = b""
    try:
        s.sendall(("%s %s HTTP/1.1\r\n%sConnection: close\r\n\r\n" % (method, path, h)).encode() + body)
        while True:
            c = s.recv(65536)
            if not c:
                break
            data += c
    except OSError:
        pass
    finally:
        s.close()
    m = re.match(rb"HTTP/1\.[01] (\d{3})", data)
    he = data.find(b"\r\n\r\n")
    return (int(m.group(1)) if m else None), (data[he + 4:] if he >= 0 else b"")


class Node:
    def __init__(self, tag):
        self.dir = os.path.join(work, tag)
        self.pfs = os.path.join(self.dir, "pfs")
        os.makedirs(self.pfs, exist_ok=True)
        env = dict(os.environ, PKERNEL_NODE_ID="1", PKERNEL_PFS_DIR=self.pfs,
                   PKERNEL_GALAXY_PORT=str(port))
        self.log = open(os.path.join(self.dir, "node.log"), "ab")
        self.p = subprocess.Popen([pk], stdin=subprocess.PIPE, stdout=self.log,
                                  stderr=subprocess.STDOUT, env=env)

    def up(self):
        for _ in range(120):
            try:
                if raw("GET", "/manifesto")[0] == 200:
                    return True
            except OSError:
                pass
            time.sleep(1)
        return False

    def token(self):
        try:
            return open(os.path.join(self.pfs, "galaxy.token")).read().strip()
        except OSError:
            return ""

    def keyhex(self):
        try:
            return open(os.path.join(self.pfs, "ark.key"), "rb").read().hex()
        except OSError:
            return ""

    def stop(self):
        try:
            self.p.stdin.write(b"exit\n")
            self.p.stdin.flush()
            self.p.wait(timeout=30)
        except Exception:
            self.p.kill()
            self.p.wait()
        self.log.close()


def shown(pg, sel):
    try:
        return pg.is_visible(sel)
    except Exception:
        return False


def wait_shown(pg, sel, timeout=15):
    t_end = time.time() + timeout
    while time.time() < t_end:
        if shown(pg, sel):
            return True
        time.sleep(0.1)
    return False


def wait_post(pg, posts, path, timeout=15):
    """the status of the page's POST to path. pg.wait_for_timeout, not time.sleep: the
    sync API hands out the response events only while it is inside a call"""
    t_end = time.time() + timeout
    while time.time() < t_end:
        for p, st in posts:
            if p == path and st is not None:
                return st
        pg.wait_for_timeout(100)
    return None


STEPS = ("#aestep-key", "#aestep-heir", "#aestep-rec")


def only(pg, sel):
    """sel is shown and the other two steps are not"""
    return shown(pg, sel) and not any(shown(pg, s) for s in STEPS if s != sel)


def open_page(browser, tok, reqs, posts):
    ctx = browser.new_context(viewport={"width": 411, "height": 800})
    pg = ctx.new_page()
    pg.on("console", lambda m: print("[ark-dom] note console: %s" % m.text[:200], flush=True)
          if m.type in ("error", "warning") else None)

    def on_resp(r):
        rq = r.request
        path = re.sub(r"^https?://[^/]+", "", rq.url).split("?")[0].split("#")[0]
        reqs.append((rq.method, path, rq.headers.get("x-galaxy-token", ""), r.status))
        if rq.method == "POST":
            posts.append((path, r.status))
    pg.on("response", on_resp)
    pg.goto(BASE + "/" + ("#t=" + tok if tok else ""), wait_until="load")
    return ctx, pg


def consent(pg):
    """the first run: the pictures, skip to the consent page, agree, consent only"""
    if not wait_shown(pg, "#introskip", 20):
        return False
    pg.click("#introskip")
    if not wait_shown(pg, "#arkagree", 10):
        return False
    pg.click("#arkagree")
    if not wait_shown(pg, "#arkskip", 10):
        return False
    pg.click("#arkskip")
    return True


GUARDED_PREFIX = ("/self.json", "/profile", "/console.txt", "/log.txt", "/ark/", "/teach", "/ask", "/events")


def main():
    exe = os.environ.get("PK_CHROMIUM") or None
    with sync_playwright() as p:
        browser = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()

        # ---- node 1: the whole first run ----
        n = Node("n1")
        if not n.up():
            res("A8-up", False, "galaxy did not answer")
            n.stop(); browser.close(); return
        tok = n.token()
        reqs, posts = [], []
        ctx, pg = open_page(browser, tok, reqs, posts)
        time.sleep(1.0)
        url = pg.evaluate("location.href")
        ss = pg.evaluate("sessionStorage.getItem('gxt')")
        res("A8-url", ("t=" not in url) and ss == tok, "url=%s ss=%s" % (url[-80:], (ss or "")[:8]))

        ok = consent(pg)
        key_only = ok and wait_shown(pg, "#aestep-key", 15) and only(pg, "#aestep-key")
        key_txt = ""
        if key_only:
            # A8-ask (added 2026-11-21 after the first form: the key is fetched only
            # on "show the key"): the step is open, no key POST yet, no key in the DOM
            pg.wait_for_timeout(1500)
            pre = (pg.text_content("#aekey") or "").strip()
            nb = len([1 for pth, _st in posts if pth == "/ark/key/backup"])
            res("A8-ask", shown(pg, "#aekeyshow") and nb == 0 and not re.search(r"[0-9a-f]{64}", pre),
                "show=%s backup-posts=%d key=%r" % (shown(pg, "#aekeyshow"), nb, pre[:12]))
            pg.click("#aekeyshow")
        else:
            res("A8-ask", False, "the key step did not open")
        if key_only:
            t_end = time.time() + 10
            while time.time() < t_end:
                key_txt = (pg.text_content("#aekey") or "").strip()
                if re.fullmatch(r"[0-9a-f]{64}", key_txt):
                    break
                time.sleep(0.1)
        o1 = key_only
        if key_only:
            pg.click("#aekeydone")
        heir_only = key_only and wait_shown(pg, "#aestep-heir", 10) and only(pg, "#aestep-heir")
        left = pg.evaluate("(document.getElementById('aekey')||{}).textContent||''") if heir_only else "?"
        res("A8-clear", heir_only and not re.search(r"[0-9a-f]{64}", left),
            "heir=%s key still in the DOM=%r" % (heir_only, left[:12]))
        if heir_only:
            pg.fill("#aeheir", HEIR)
            pg.click("#aeheirsave")
        rec_only = heir_only and wait_shown(pg, "#aestep-rec", 10) and only(pg, "#aestep-rec")
        if rec_only:
            pg.fill("#aerectext", REC_TEXT)
            pg.click("#aerecsave")
            wait_post(pg, posts, "/ark/rec")
        closed = False
        t_end = time.time() + 10
        while rec_only and time.time() < t_end:
            if not pg.evaluate("!!(document.getElementById('arkentry')&&document.getElementById('arkentry').classList.contains('open'))"):
                closed = True
                break
            time.sleep(0.1)
        seq = [pth for pth, st in posts]
        want = ["/profile", "/ark/key/backup", "/ark/key/backup/done", "/ark/heir", "/ark/rec"]
        res("A8-order", o1 and heir_only and rec_only and seq == want,
            "consent=%s key=%s heir=%s rec=%s posts=%s" % (ok, o1, heir_only, rec_only, seq))

        kfile = n.keyhex()
        again, _b = raw("POST", "/ark/key/backup", b"", tok)
        res("A8-key", key_txt != "" and key_txt == kfile and again == 410,
            "shown=%s file=%s again=%s" % (key_txt[:8], kfile[:8], again))

        guarded = [r for r in reqs if r[1].startswith(GUARDED_PREFIX)]
        bad = [r for r in guarded if r[2] != tok or r[3] == 401]
        res("A8-header", len(guarded) >= 5 and not bad,
            "guarded=%d bad=%s" % (len(guarded), [(m, pth, st) for m, pth, _t, st in bad][:6]))

        hs, hb = raw("GET", "/ark/heir", b"", tok)
        res("A8-heir", hs == 200 and ('"heir":"%s"' % HEIR).encode() in hb, "st=%s %r" % (hs, hb[:90]))

        ls, lb = raw("GET", "/ark/rec.json", b"", tok)
        nos = [int(x) for x in re.findall(rb'"no":(\d+)', lb)]
        back = b""
        if len(nos) == 1:
            _g, back = raw("GET", "/ark/rec/%d" % nos[0], b"", tok)
        want_b = REC_TEXT.encode("utf-8")
        res("A8-rec", ls == 200 and len(nos) == 1 and back == want_b,
            "list=%s nos=%s sha back=%s want=%s" % (ls, nos, hashlib.sha256(back).hexdigest()[:8],
                                                   hashlib.sha256(want_b).hexdigest()[:8]))

        pg.goto(BASE + "/", wait_until="load")
        time.sleep(2.0)
        intro_open = pg.evaluate("document.getElementById('intro').classList.contains('open')")
        ent_open = pg.evaluate("!!(document.getElementById('arkentry')&&document.getElementById('arkentry').classList.contains('open'))")
        n401 = len([r for r in reqs if r[3] == 401 and r[1] != "/favicon.ico"])  # 7.7: the browser's own
        res("A8-done", closed and not intro_open and not ent_open and n401 == 0,
            "closed=%s intro=%s entry=%s 401s=%d" % (closed, intro_open, ent_open, n401))
        ctx.close()
        n.stop()

        # ---- node 2: reload after the key was shown, then skip the heir ----
        n = Node("n2")
        if not n.up():
            res("A8-up2", False, "galaxy did not answer")
            n.stop(); browser.close(); return
        tok = n.token()
        reqs, posts = [], []
        ctx, pg = open_page(browser, tok, reqs, posts)
        ok = consent(pg)
        # A8-later (added with A8-ask): "later" closes the entry without fetching the
        # key; a reload opens the key step again
        kl = ok and wait_shown(pg, "#aestep-key", 15) and wait_shown(pg, "#aekeylater", 5)
        if kl:
            pg.click("#aekeylater")
        closed_l = False
        t_end = time.time() + 10
        while kl and time.time() < t_end:
            if not pg.evaluate("document.getElementById('arkentry').classList.contains('open')"):
                closed_l = True
                break
            pg.wait_for_timeout(100)
        nb_l = len([1 for pth, _st in posts if pth == "/ark/key/backup"])
        pg.goto(BASE + "/", wait_until="load")
        back_l = wait_shown(pg, "#aestep-key", 15) and wait_shown(pg, "#aekeyshow", 5)
        res("A8-later", bool(closed_l) and nb_l == 0 and back_l,
            "later=%s closed=%s backup-posts=%d again=%s" % (kl, closed_l, nb_l, back_l))
        ks = back_l
        if ks:
            pg.click("#aekeyshow")
        kp = wait_post(pg, posts, "/ark/key/backup") if ks else None
        k1 = bool(ks) and kp == 200
        print("[ark-dom] note n2 before reload: consent=%s keystep=%s backup=%s posts=%s"
              % (ok, ks, kp, posts), flush=True)
        pg.goto(BASE + "/", wait_until="load")          # the pass is in sessionStorage now
        k2 = wait_shown(pg, "#aestep-key", 15) and only(pg, "#aestep-key")
        st_rj, _b = raw("GET", "/ark/rec.json", b"", tok)
        print("[ark-dom] note n2 after reload: entry=%r steps=%r rec.json=%s url=%s" % (
            pg.evaluate("document.getElementById('arkentry').className"),
            pg.evaluate("[...document.querySelectorAll('#arkentry .aestep')].map(e=>e.id+':'+e.className)"),
            st_rj, pg.evaluate("location.href")), flush=True)
        if k2 and wait_shown(pg, "#aekeyshow", 5):
            pg.click("#aekeyshow")
        gone = k2 and wait_shown(pg, "#aekeygone", 10)
        txt2 = (pg.text_content("#aekey") or "").strip() if k2 else ""
        intro2 = pg.evaluate("document.getElementById('intro').classList.contains('open')")
        if k2:
            pg.click("#aekeydone")
        h2 = k2 and wait_shown(pg, "#aestep-heir", 10) and only(pg, "#aestep-heir")
        res("A8-resume", k1 and k2 and gone and not re.search(r"[0-9a-f]{64}", txt2) and not intro2 and h2,
            "first=%s again=%s gone=%s keytext=%r intro=%s heir=%s" % (k1, k2, gone, txt2[:16], intro2, h2))
        if h2:
            pg.click("#aeheirskip")
        r2 = h2 and wait_shown(pg, "#aestep-rec", 10) and only(pg, "#aestep-rec")
        if r2:
            pg.fill("#aerectext", "skip the heir")
            pg.click("#aerecsave")
            wait_post(pg, posts, "/ark/rec")
        hs, hb = raw("GET", "/ark/heir", b"", tok)
        ls, lb = raw("GET", "/ark/rec.json", b"", tok)
        nheir = [pth for pth, _st in posts if pth == "/ark/heir"]
        res("A8-skip", r2 and hs == 200 and b'"heir":null' in hb and len(re.findall(rb'"no":', lb)) == 1
            and not nheir, "rec=%s heir=%r list=%r heirposts=%d" % (r2, hb[:40], lb[:60], len(nheir)))
        ctx.close()
        n.stop()
        browser.close()


main()
ALLC = ["A8-url", "A8-ask", "A8-order", "A8-key", "A8-clear", "A8-header", "A8-heir", "A8-rec", "A8-done",
        "A8-later", "A8-resume", "A8-skip"]
if all(RESULTS.get(c) for c in ALLC):
    print("[ark-dom] ALL PASS")
    sys.exit(0)
print("[ark-dom] RED: %s" % " ".join(c for c in ALLC if not RESULTS.get(c)))
sys.exit(1)
