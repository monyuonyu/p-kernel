#!/usr/bin/env python3
"""stop_galaxy.py <p-kernel> <workdir> <port> - drives one hosted node for
tests/host/run_stop_galaxy.sh (ROADMAP 0-14 S2 stage 2b: galaxy POST /teach
obeys the stop). Prints "[stop-galaxy] <check> PASS|FAIL" lines."""
import os, subprocess, sys, time, urllib.request, urllib.error

pk, work, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
base = "http://127.0.0.1:%d" % port
log = open(os.path.join(work, "node.log"), "wb")
env = dict(os.environ, PKERNEL_NODE_ID="1", PKERNEL_PFS_DIR=os.path.join(work, "pfs"),
           PKERNEL_GALAXY_PORT=str(port))
os.makedirs(env["PKERNEL_PFS_DIR"], exist_ok=True)
p = subprocess.Popen([pk], stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, env=env)


def res(name, ok):
    print("[stop-galaxy] %s %s" % (name, "PASS" if ok else "FAIL"), flush=True)


def gx_token():
    # ark-entry.md 7.2 6: galaxy wants this boot's pass on POSTs and person GETs
    try:
        return open(os.path.join(env["PKERNEL_PFS_DIR"], "galaxy.token")).read().strip()
    except OSError:
        return ""


def http(path, body=None):
    req = urllib.request.Request(base + path, data=body.encode() if body is not None else None,
                                 headers={"X-Galaxy-Token": gx_token()})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, dict(r.headers), r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read().decode("utf-8", "replace")


def shell(line):
    p.stdin.write((line + "\n").encode()); p.stdin.flush()


def node_log():
    log.flush()
    return open(os.path.join(work, "node.log"), "rb").read().decode("utf-8", "replace")


def wait_log(pat, n_before, secs):
    t = time.time() + secs
    while time.time() < t:
        if node_log().count(pat) > n_before:
            return True
        time.sleep(0.5)
    return False


try:
    mid = None
    for _ in range(120):
        try:
            st, hd, _b = http("/manifesto")
            mid = hd.get("X-Manifesto-Id")
            break
        except (urllib.error.URLError, ConnectionError, OSError):
            time.sleep(1)
    res("G0-up", mid is not None)
    st, _h, b = http("/profile", "ack=1&mid=%s" % mid)
    res("G1-consent", st == 200)
    # G2: not stopped -> taught
    st, _h, b = http("/teach", "k=sky&v=blue")
    res("G2-teach", st == 200 and '"ok":true' in b)
    # G3: stopped at the console -> 403 refused:stopped, nothing taught
    shell("stop now")
    up = wait_log("[stop] stopped 1", 0, 30)
    st, _h, b3 = http("/teach", "k=sea&v=green")
    res("G3-refused", up and st == 403 and '"refused":"stopped"' in b3)
    # G4: resumed -> taught again
    shell("stop resume")
    wait_log("[stop] stopped 0", 0, 30)
    st, _h, b = http("/teach", "k=ink&v=fresh")
    res("G4-teach", st == 200 and '"ok":true' in b)
    shell("mind provenance")
    wait_log("[mind]   \"ink\"", 0, 60)
    time.sleep(1)
    L = node_log()
    res("G5-facts", '[mind]   "sky" -> ' in L and '[mind]   "ink" -> ' in L and '[mind]   "sea" -> ' not in L)
    print("[stop-galaxy] G3 reply: %s" % b3.strip()[:120], flush=True)
    if len(sys.argv) > 6:
        # the signed arm (audit-59's limit): a signed stop, not the console's
        # own, must refuse POST /teach too; a local resume does not undo it
        spk, scs, scr = sys.argv[4], sys.argv[5], sys.argv[6]

        def paste(cmd):
            for i in range(0, len(cmd), 64):
                shell("stop in " + cmd[i:i + 64])
            shell("stop apply")

        shell("stop key " + spk)
        wait_log("keys 1", 0, 30)
        paste(scs)
        up = wait_log("[stop] stopped 1 local 0 signed 1 epoch 5", 0, 30)
        st, _h, b6 = http("/teach", "k=leaf&v=green")
        res("G6-signed-refused", up and st == 403 and '"refused":"stopped"' in b6)
        n = node_log().count("[stop] stopped 1 local 0 signed 1")
        shell("stop resume")
        held = wait_log("[stop] stopped 1 local 0 signed 1", n, 30)
        st, _h, b7 = http("/teach", "k=leaf&v=green")
        res("G7-local-resume-keeps", held and st == 403 and '"refused":"stopped"' in b7)
        paste(scr)
        down = wait_log("[stop] stopped 0 local 0 signed 0 epoch 6", 0, 30)
        st, _h, b = http("/teach", "k=fire&v=warm")
        res("G8-signed-resume", down and st == 200 and '"ok":true' in b)
        n = node_log().count('[mind]   "fire"')
        shell("mind provenance")
        wait_log('[mind]   "fire"', n, 60)
        time.sleep(1)
        L = node_log()
        res("G9-facts", '[mind]   "fire" -> ' in L and '[mind]   "leaf" -> ' not in L)
        print("[stop-galaxy] G6 reply: %s" % b6.strip()[:120], flush=True)
finally:
    try:
        shell("exit")
        p.wait(timeout=20)
    except Exception:
        p.kill()
    log.close()
