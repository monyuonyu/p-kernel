"""b5_step4_check.py - the judge of B5 step 4 (research/b5-step4-design.md).

Reads <build>-<seed>.log written by run_b5_step4.sh (student_adopt_test.c,
stage s4). Each arm prints one line

  [b5s4] arm=<name> last=<bpb> kept=<bpb> at <pass> (gate <nats>) ... took=<n>/<m> app=<k>

where kept is node A's model at the pass with the lowest loss on the gate
window (the fixture's held side), the stop of every arm; the ruler
(b1_held.txt) is never used to choose. Lines fixed in the design before the
code and before any run on the judgement seeds 99..106:

  K1  apprentice kept <= shrink-app kept - DK  on at least NEED of the seeds
      (the peer's knowledge, beyond one node shrunk x0.5 through the same gate)
  K2  apprentice kept <= min(alone-A kept, alone-B kept) - D1  on every seed
  K3  poison-app kept <= alone-A kept + 0.02  on every seed
  K4  plain apprentice <= same apprentice - DK  on at least NEED of the seeds
      (paired: the honest peer beats a peer with no new bytes; audit-130)
  NC  the build -DB5S4_PEER_SAME (the honest peer trains on A's own half,
      shifted by SEQ/2: no new bytes) must turn K1 red

  b5_step4_check.py [--seeds "99 ... 106"] <dir>   exit 0 = K1-K4 green, NC red
  b5_step4_check.py --self-test
"""
import os
import re
import shutil
import sys
import tempfile

DK = 0.04      # fixed in the design (section 3) from exploration seeds 91..95, before the code
D1 = 0.10
NEED = 7
ARM = re.compile(r"^\[b5s4\] arm=(\S+) last=([0-9.]+) kept=([0-9.]+) at (\d+) \(gate ([0-9.]+)\)")
NEEDED = ["alone-A", "alone-B", "full", "shrink-app", "apprentice", "poison-app"]


def read(path):
    """{arm: kept} of a finished log, or None (missing, short, an arm twice)"""
    if not os.path.exists(path):
        return None
    arms, done = {}, False
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        m = ARM.match(line)
        if m:
            if m.group(1) in arms:
                return None
            arms[m.group(1)] = float(m.group(3))
        elif line == "[b5s4] done":
            done = True
    if not done or any(a not in arms for a in NEEDED):
        return None
    return arms


def gates(seeds, dk, d1):
    """seeds: {seed: arms or None} -> (k1, k2, k3, lines); a missing seed is a fail"""
    lines, k2, k3, ok1, n = [], True, True, 0, 0
    for s in sorted(seeds, key=int):
        a = seeds[s]
        if a is None:
            lines.append("[b5s4] seed %s: log missing or short" % s)
            k2 = k3 = False
            continue
        n += 1
        alone = min(a["alone-A"], a["alone-B"])
        g1 = a["apprentice"] <= a["shrink-app"] - dk + 1e-9
        g2 = a["apprentice"] <= alone - d1 + 1e-9
        g3 = a["poison-app"] <= a["alone-A"] + 0.02 + 1e-9
        ok1 += g1
        k2 &= g2
        k3 &= g3
        lines.append("[b5s4] seed %s: apprentice %.4f shrink %.4f (%+.4f) alone %.4f (%+.4f) poison %.4f full %.4f"
                     " -> K1 %s K2 %s K3 %s"
                     % (s, a["apprentice"], a["shrink-app"], a["apprentice"] - a["shrink-app"], alone,
                        a["apprentice"] - alone, a["poison-app"], a["full"],
                        "ok" if g1 else "RED", "ok" if g2 else "RED", "ok" if g3 else "RED"))
    k1 = n == len(seeds) and ok1 >= min(NEED, len(seeds))
    return k1, k2, k3, lines


def paired(plain, same, dk):
    """K4: plain apprentice <= same apprentice - dk on NEED+ seeds; a seed missing in either is a fail"""
    lines, ok, n = [], 0, 0
    for s in sorted(plain, key=int):
        p, q = plain[s], same.get(s)
        if p is None or q is None:
            lines.append("[b5s4] K4 seed %s: missing" % s)
            continue
        n += 1
        g = p["apprentice"] <= q["apprentice"] - dk + 1e-9
        ok += g
        lines.append("[b5s4] K4 seed %s: honest %.4f same-half %.4f (%+.4f) -> %s"
                     % (s, p["apprentice"], q["apprentice"], p["apprentice"] - q["apprentice"],
                        "ok" if g else "RED"))
    return n == len(plain) and ok >= min(NEED, len(plain)), lines


def judge(plain, same, dk, d1):
    """-> (ok, lines, {gate id: bool})"""
    p1, p2, p3, lines = gates(plain, dk, d1)
    s1, _, _, ls = gates(same, dk, d1)
    lines += ["  (B5S4_PEER_SAME) " + x for x in ls]
    k4, l4 = paired(plain, same, dk)
    lines += l4
    nc = not s1 and all(v is not None for v in same.values())
    res = [("K1", p1, "apprentice <= shrink-app - %.3f on %d+ seeds" % (dk, NEED)),
           ("K2", p2, "every seed: apprentice <= better alone - %.3f" % d1),
           ("K3", p3, "every seed: poison-app <= alone-A + 0.02"),
           ("K4", k4, "honest apprentice <= same-half apprentice - %.3f on %d+ seeds" % (dk, NEED)),
           ("NC-same", nc, "a peer with no new bytes turns K1 red")]
    for gid, ok, what in res:
        lines.append("[b5s4] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    ok = all(r[1] for r in res)
    lines.append("[result] %s b5-step4" % ("PASS" if ok else "FAIL"))
    return ok, lines, {r[0]: r[1] for r in res}


def _log(arms, done=True, last=None, extra=()):
    """a log in the runner's format; last defaults to kept + 0.5 (so last != kept)"""
    out = ["[b5s4] seed 0 lr 0.001"]
    for name, kept in arms.items():
        lv = kept + 0.5 if last is None else last
        out.append("[b5s4] arm=%s last=%.4f kept=%.4f at 9 (gate 2.5000) peek=%.4f at 8 took=1/8 app=1"
                   % (name, lv, kept, kept))
    out += list(extra)
    if done:
        out.append("[b5s4] done")
    return "\n".join(out) + "\n"


def self_test():
    bad = 0
    def check(name, got, want):
        nonlocal bad
        print("[b5s4-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want

    # the constants of the design (section 3), not to be moved after the seeds
    check("constants DK 0.04 D1 0.10 NEED 7", (DK, D1, NEED), (0.04, 0.10, 7))

    dk, d1 = 0.10, 0.30
    def A(app, shrink=4.0, alone=4.6, poison=4.5, aloneB=None):
        return {"alone-A": alone, "alone-B": alone + 0.1 if aloneB is None else aloneB, "full": 3.5,
                "shrink-app": shrink, "apprentice": app, "poison-app": poison}
    S = [str(s) for s in range(99, 107)]
    good = {s: A(3.8) for s in S}
    same = {s: A(4.0) for s in S}
    def one(d, s, v):
        x = dict(d); x[s] = v; return x
    # (name, (plain, same), overall, {gate: want} checked one by one)
    cases = [
        ("green", (good, same), True, {"K1": True, "K2": True, "K3": True, "K4": True, "NC-same": True}),
        ("K1 one short ok", (one(good, "99", A(3.95)), same), True, {"K1": True}),
        ("K1 two short", (one(one(good, "99", A(3.95)), "100", A(3.95)), same), False, {"K1": False}),
        ("K1 at the line", ({s: A(3.9) for s in S}, same), True, {"K1": True, "K4": True}),
        # K2 alone: poison stays under alone-A + 0.02 so K3 stays green
        ("K2 one seed", (one(good, "103", A(3.8, shrink=4.0, alone=4.05, poison=4.0)), same), False,
         {"K2": False, "K3": True, "K1": True}),
        # the better alone is B: max(alone) would pass it
        ("K2 better alone is B", (one(good, "101", A(3.8, alone=4.6, aloneB=3.95)), same), False,
         {"K2": False, "K3": True}),
        ("K3 poison worse", (one(good, "104", A(3.8, poison=4.65)), same), False, {"K3": False, "K2": True}),
        ("NC green", (good, good), False, {"NC-same": False}),
        ("NC missing", (good, one(same, "99", None)), False, {"NC-same": False, "K4": False}),
        ("missing seed", (one(good, "106", None), same), False, {"K1": False, "K2": False, "K3": False,
                                                                  "K4": False}),
        # K4: same build's apprentice is red on K1 (NC fine) but close to honest
        ("K4 two short", (good, one(one(same, "99", A(3.85)), "100", A(3.85))), False,
         {"K4": False, "NC-same": True, "K1": True}),
        ("K4 one short ok", (good, one(same, "99", A(3.85))), True, {"K4": True}),
        ("K4 at the line", (good, {s: A(3.9, shrink=3.9) for s in S}), True, {"K4": True, "NC-same": True}),
    ]
    for name, (p, s), want, gw in cases:
        got, _, g = judge(p, s, dk, d1)
        check(name, got, want)
        for gid, w in gw.items():
            check("%s: %s" % (name, gid), g[gid], w)

    # read(): through real files
    d = tempfile.mkdtemp()
    try:
        arms = {"alone-A": 4.0, "alone-B": 4.05, "full": 3.8, "shrink-app": 3.9, "apprentice": 3.84,
                "poison-app": 4.0}
        def put(name, text):
            p = os.path.join(d, name); open(p, "w").write(text); return p
        r = read(put("k.log", _log(arms)))
        check("read: kept, not last", r, arms)
        check("read: no done -> None", read(put("nd.log", _log(arms, done=False))), None)
        dup = _log(arms, extra=["[b5s4] arm=apprentice last=9.0 kept=3.0000 at 1 (gate 2.0)"])
        check("read: an arm twice -> None", read(put("dup.log", dup)), None)
        short = dict(arms); del short["poison-app"]
        check("read: an arm missing -> None", read(put("sh.log", _log(short))), None)
        check("read: no file -> None", read(os.path.join(d, "nofile.log")), None)

        # main(): constants and which build is which, on a directory of logs
        # plain: app - shrink = -0.06 (K1 needs -0.04; -0.10 would be red), app - alone = -0.16
        # same : app = shrink (K1 red = NC ok), plain - same = -0.06 (K4 ok)
        same_arms = dict(arms); same_arms["apprentice"] = 3.90
        for s in S:
            put("plain-%s.log" % s, _log(arms))
            put("same-%s.log" % s, _log(same_arms))
        def run_main():
            null = open(os.devnull, "w")
            old, sys.stdout = sys.stdout, null
            try:
                return main(["x", d])
            finally:
                sys.stdout = old
                null.close()
        check("main: plain/same directory PASS", run_main(), 0)
        put("plain-102.log", _log(same_arms))      # one plain seed with no peer's knowledge: K1 still 7/8
        check("main: one plain seed like same, still PASS", run_main(), 0)
        put("plain-103.log", _log(same_arms))      # two: K1 6/8 -> red, exit 1
        check("main: two plain seeds like same, FAIL rc 1", run_main(), 1)
    finally:
        shutil.rmtree(d)
    return bad


def main(argv):
    if argv[1:] == ["--self-test"]:
        return 1 if self_test() else 0
    seeds, d, i = " ".join(str(s) for s in range(99, 107)), None, 1
    while i < len(argv):
        if argv[i] == "--seeds":
            seeds = argv[i + 1]; i += 2
        else:
            d = argv[i]; i += 1
    if d is None or DK is None or D1 is None:
        print(__doc__)
        return 2
    sl = seeds.split()
    get = lambda b: {s: read(os.path.join(d, "%s-%s.log" % (b, s))) for s in sl}
    ok, lines, _ = judge(get("plain"), get("same"), DK, D1)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
