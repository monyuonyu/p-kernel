#!/usr/bin/env python3
"""galaxy_slots.py <p-kernel> <workdir> <port> - drives one hosted node for
tests/host/run_galaxy_slots.sh (galaxy has GALAXY_MAX_CLIENTS = 4 slots; a
held SSE stream must not keep a slot from the page). Prints
"[gx-slots] <check> PASS|FAIL ..." lines."""
import os, socket, subprocess, sys, time, urllib.request, urllib.error

pk, work, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
base = "http://127.0.0.1:%d" % port
log = open(os.path.join(work, "node.log"), "wb")
env = dict(os.environ, PKERNEL_NODE_ID="1", PKERNEL_PFS_DIR=os.path.join(work, "pfs"),
           PKERNEL_GALAXY_PORT=str(port))
os.makedirs(env["PKERNEL_PFS_DIR"], exist_ok=True)
p = subprocess.Popen([pk], stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, env=env)


def res(name, ok, why=""):
    print("[gx-slots] %s %s %s" % (name, "PASS" if ok else "FAIL", why), flush=True)


def tok():
    try:
        return open(os.path.join(env["PKERNEL_PFS_DIR"], "galaxy.token")).read().strip()
    except OSError:
        return ""


def get_once(path):
    try:
        with urllib.request.urlopen(base + path, timeout=3) as r:
            r.read()
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except (urllib.error.URLError, ConnectionError, OSError):
        return None


def get_within(path, secs):
    """seconds until a GET answers 200, or None"""
    t0 = time.time()
    while time.time() - t0 < secs:
        if get_once(path) == 200:
            return time.time() - t0
        time.sleep(0.2)
    return None


def sse_open():
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(("GET /events HTTP/1.0\r\nHost: 127.0.0.1:%d\r\nX-Galaxy-Token: %s\r\n\r\n"
               % (port, tok())).encode())
    got = b""
    t = time.time() + 5
    try:
        while time.time() < t and b"galaxy stream open" not in got:
            b = s.recv(4096)
            if not b:
                break
            got += b
    except OSError:
        pass
    if b"galaxy stream open" not in got:
        s.close()
        return None
    return s


def eof_within(s, secs):
    """True if the server closed s within secs (data is read and dropped)"""
    t = time.time() + secs
    s.settimeout(0.3)
    while time.time() < t:
        try:
            b = s.recv(4096)
            if not b:
                return True
        except socket.timeout:
            continue
        except OSError:
            return True
    return False


try:
    up = get_within("/manifesto", 120)
    res("S0-up", up is not None)
    # S1: three streams held, a fourth opened and closed by the browser (a quick
    # reload): its slot comes back at once, not when a keepalive write fails (15 s
    # and more), and the GET does not cost a held stream (S1-keep; written after the
    # first run of the fix: with S1 as four closed streams, the oldest-gives-way rule
    # alone also let the GET in, and the falsifier NO_READ stayed green)
    ss = []
    for _ in range(4):
        ss.append(sse_open())
        time.sleep(0.3)
    res("S0-open", all(ss), "(%d of 4 streams open)" % sum(1 for x in ss if x))
    if ss[3]:
        ss[3].close()
    time.sleep(0.5)
    w = get_within("/manifesto", 5)
    res("S1-closed", w is not None, "(%s)" % ("answered in %.1f s" % w if w is not None else "no answer in 5 s"))
    res("S1-keep", bool(ss[0]) and not eof_within(ss[0], 1), "(the oldest held stream stays)")
    for x in ss[:3]:
        if x:
            x.close()
    time.sleep(1)
    # S2: four streams held open: the page still gets in, the oldest stream gives way
    ss = []
    for _ in range(4):
        ss.append(sse_open())
        time.sleep(0.3)
    res("S2-open", all(ss), "(%d of 4 streams open)" % sum(1 for x in ss if x))
    w = get_within("/manifesto", 5)
    res("S2-held", w is not None, "(%s)" % ("answered in %.1f s" % w if w is not None else "no answer in 5 s"))
    if all(ss):
        res("S2-oldest", eof_within(ss[0], 2), "(the first stream is closed)")
        res("S2-newest", not eof_within(ss[3], 1), "(the last stream stays)")
    else:
        res("S2-oldest", False, "(not all open)")
        res("S2-newest", False, "(not all open)")
    for x in ss:
        if x:
            x.close()
finally:
    try:
        p.stdin.write(b"exit\n"); p.stdin.flush()
    except OSError:
        pass
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait()
