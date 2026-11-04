"""b5_step4_check.py - the judge of B5 step 4 (research/b5-step4-design.md).

Reads <build>-<seed>.log written by the step-4 runner (not yet written; the
implementation round adds it). Each arm prints one line

  [b5s4] arm=<name> last=<bpb> kept=<bpb> at <pass> (gate <nats>) ... took=<n>/<m> app=<k>

where kept is node A's model at the pass with the lowest loss on the gate
window (the fixture's held side), the stop of every arm; the ruler
(b1_held.txt) is never used to choose. Lines fixed in the design before the
code and before any run on the judgement seeds 99..106:

  K1  apprentice kept <= shrink-app kept - DK  on at least NEED of the seeds
      (the peer's knowledge, beyond one node shrunk x0.5 through the same gate)
  K2  apprentice kept <= min(alone-A kept, alone-B kept) - D1  on every seed
  K3  poison-app kept <= alone-A kept + 0.02  on every seed
  NC  the build -DB5S4_PEER_SAME (the honest peer trains on A's own half,
      shifted by SEQ/2: no new bytes) must turn K1 red

  b5_step4_check.py [--seeds "99 ... 106"] <dir>   exit 0 = K1-K3 green, NC red
  b5_step4_check.py --self-test
"""
import os
import re
import sys

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


def judge(plain, same, dk, d1):
    p1, p2, p3, lines = gates(plain, dk, d1)
    s1, _, _, ls = gates(same, dk, d1)
    lines += ["  (B5S4_PEER_SAME) " + x for x in ls]
    nc = not s1 and all(v is not None for v in same.values())
    res = [("K1", p1, "apprentice <= shrink-app - %.3f on %d+ seeds" % (dk, NEED)),
           ("K2", p2, "every seed: apprentice <= better alone - %.3f" % d1),
           ("K3", p3, "every seed: poison-app <= alone-A + 0.02"),
           ("NC-same", nc, "a peer with no new bytes turns K1 red")]
    for gid, ok, what in res:
        lines.append("[b5s4] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    ok = all(r[1] for r in res)
    lines.append("[result] %s b5-step4" % ("PASS" if ok else "FAIL"))
    return ok, lines


def self_test():
    dk, d1 = 0.10, 0.30
    def A(app, shrink=4.0, alone=4.6, poison=4.5):
        return {"alone-A": alone, "alone-B": alone + 0.1, "full": 3.5, "shrink-app": shrink,
                "apprentice": app, "poison-app": poison}
    S = [str(s) for s in range(99, 107)]
    good = {s: A(3.8) for s in S}
    same = {s: A(4.0) for s in S}
    def one(d, s, v):
        x = dict(d); x[s] = v; return x
    cases = [
        ("green", (good, same), True),
        ("K1 one short ok", (one(good, "99", A(3.95)), same), True),
        ("K1 two short", (one(one(good, "99", A(3.95)), "100", A(3.95)), same), False),
        ("K1 at the line", ({s: A(3.9) for s in S}, same), True),
        ("K2 one seed", (one(good, "103", A(3.8, shrink=4.0, alone=4.05)), same), False),
        ("K3 poison worse", (one(good, "104", A(3.8, poison=4.65)), same), False),
        ("NC green", (good, good), False),
        ("NC missing", (good, one(same, "99", None)), False),
        ("missing seed", (one(good, "106", None), same), False),
    ]
    bad = 0
    for name, (p, s), want in cases:
        got, _ = judge(p, s, dk, d1)
        print("[b5s4-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want
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
    ok, lines = judge(get("plain"), get("same"), DK, D1)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
