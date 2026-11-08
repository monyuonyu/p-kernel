"""b5_step7_check.py - the judge of B5 step 7 (research/b5-step7-design.md).

Reads the logs written by run_b5_step7.sh (student_adopt_test.c, stages s7a
s7b s7y s7c). Each arm prints one line

  [b5s7] arm=<name> last=<bpb> kept=<bpb> at <pass> (gate <nats>) ... took=<n>/<m> ...

kept is node A's model at its best pass on the gate window. Lines fixed in the
design (section 4) before the code and before any run on the seeds 131..138:

  R1  trust4-garb2 <= alone-A - 0.08                          on >= 7 seeds
  R2  trust4-flip2 <= alone-A - 0.05                          on >= 7 seeds
  R3  trust4-garb1 <= trim4-garb1 + 0.03                      on >= 7 seeds
  R4  trust4 <= trim4 + 0.03                                  on >= 7 seeds
  R5  trust4-garb2, trust4-flip2, trust4-near2 <= alone-A + 0.02   every seed
  R6  trust4-yori1, trust4-yori2 <= alone-A + 0.02                 every seed
  NC1 the build -DB5S7_TS_FROM_GJ (step 6's TS) turns R3 or R4 red
  NC2 the build -DB5S7_PREV_CLAIMED (the near poison claims its previous and
      jumps every round) turns R6 red
The gj build runs s7a, the claimed build s7y; alone-A of an NC check comes
from the plain log of the seed. near1 (trim and trust) is printed as a
reading, not a line.

  b5_step7_check.py [--seeds "131 ... 138"] <dir>   exit 0 = R1-R6 green, NC1 NC2 red
  b5_step7_check.py --self-test
"""
import os
import re
import shutil
import sys
import tempfile

D1 = 0.08
D2 = 0.05
D34 = 0.03
D5 = 0.02
D6 = 0.02
NEED = 7
SEEDS = " ".join(str(s) for s in range(131, 139))
ARM = re.compile(r"^\[b5s7\] arm=(\S+) last=([0-9.]+) kept=([0-9.]+) at (\d+) \(gate ([0-9.]+)\)")
PARTS = {"a": ["alone-A", "trim4", "trust4", "trim4-garb1", "trust4-garb1"],
         "b": ["trust4-garb2", "trust4-flip2", "trust4-near2"],
         "y": ["trust4-yori1", "trust4-yori2"],
         "c": ["trim4-near1", "trust4-near1"]}


def read(path, needed):
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
        elif line == "[b5s7] done":
            done = True
    if not done or any(a not in arms for a in needed):
        return None
    return arms


def read_plain(d, s):
    out = {}
    for p, needed in PARTS.items():
        a = read(os.path.join(d, "plain-%s-%s.log" % (s, p)), needed)
        if a is None:
            return None
        out.update(a)
    return out


def read_nc(d, b, s, part, plain):
    """the part's arms of build b, alone-A from the plain log; None if either is missing"""
    a = read(os.path.join(d, "%s-%s.log" % (b, s)), PARTS[part])
    if a is None or plain is None:
        return None
    a = dict(a)
    a["alone-A"] = plain["alone-A"]
    return a


def count(seeds, test, label, lines, need):
    ok, n = 0, 0
    for s in sorted(seeds, key=int):
        a = seeds[s]
        if a is None:
            lines.append("[b5s7] %s seed %s: log missing or short" % (label, s))
            continue
        n += 1
        g, text = test(a)
        ok += g
        lines.append("[b5s7] %s seed %s: %s -> %s" % (label, s, text, "ok" if g else "RED"))
    return n == len(seeds) and ok >= need


def r1(a):
    d = a["trust4-garb2"] - a["alone-A"]
    return d <= -D1 + 1e-9, "trust-garb2 %.4f alone-A %.4f (%+.4f)" % (a["trust4-garb2"], a["alone-A"], d)


def r2(a):
    d = a["trust4-flip2"] - a["alone-A"]
    return d <= -D2 + 1e-9, "trust-flip2 %.4f alone-A %.4f (%+.4f)" % (a["trust4-flip2"], a["alone-A"], d)


def r3(a):
    d = a["trust4-garb1"] - a["trim4-garb1"]
    return d <= D34 + 1e-9, "trust-garb1 %.4f trim-garb1 %.4f (%+.4f)" % (a["trust4-garb1"], a["trim4-garb1"], d)


def r4(a):
    d = a["trust4"] - a["trim4"]
    return d <= D34 + 1e-9, "trust %.4f trim %.4f (%+.4f)" % (a["trust4"], a["trim4"], d)


def r5(a):
    worst = max(a["trust4-garb2"], a["trust4-flip2"], a["trust4-near2"])
    d = worst - a["alone-A"]
    return d <= D5 + 1e-9, "worst of garb2/flip2/near2 %.4f alone-A %.4f (%+.4f)" % (worst, a["alone-A"], d)


def r6(a):
    worst = max(a["trust4-yori1"], a["trust4-yori2"])
    d = worst - a["alone-A"]
    return d <= D6 + 1e-9, "worst of yori1/yori2 %.4f alone-A %.4f (%+.4f)" % (worst, a["alone-A"], d)


def judge(plain, gj, claimed):
    """-> (ok, lines, {gate id: bool})"""
    lines = []
    need = min(NEED, len(plain))
    g1 = count(plain, r1, "R1", lines, need)
    g2 = count(plain, r2, "R2", lines, need)
    g3 = count(plain, r3, "R3", lines, need)
    g4 = count(plain, r4, "R4", lines, need)
    g5 = count(plain, r5, "R5", lines, len(plain))
    g6 = count(plain, r6, "R6", lines, len(plain))
    n13 = count(gj, r3, "NC1 (B5S7_TS_FROM_GJ) R3", lines, min(NEED, len(gj)))
    n14 = count(gj, r4, "NC1 (B5S7_TS_FROM_GJ) R4", lines, min(NEED, len(gj)))
    n26 = count(claimed, r6, "NC2 (B5S7_PREV_CLAIMED) R6", lines, len(claimed))
    nc1 = not (n13 and n14) and all(v is not None for v in gj.values())
    nc2 = not n26 and all(v is not None for v in claimed.values())
    for s in sorted(plain, key=int):
        a = plain[s]
        if a is not None:
            lines.append("[b5s7] reading near1 seed %s: trust %.4f trim %.4f (%+.4f)"
                         % (s, a["trust4-near1"], a["trim4-near1"], a["trust4-near1"] - a["trim4-near1"]))
    res = [("R1", g1, "trust4-garb2 <= alone-A - %.2f on %d+ seeds" % (D1, NEED)),
           ("R2", g2, "trust4-flip2 <= alone-A - %.2f on %d+ seeds" % (D2, NEED)),
           ("R3", g3, "trust4-garb1 <= trim4-garb1 + %.2f on %d+ seeds" % (D34, NEED)),
           ("R4", g4, "trust4 <= trim4 + %.2f on %d+ seeds" % (D34, NEED)),
           ("R5", g5, "every seed: trust4 garb2/flip2/near2 <= alone-A + %.2f" % D5),
           ("R6", g6, "every seed: trust4 yori1/yori2 <= alone-A + %.2f" % D6),
           ("NC1", nc1, "-DB5S7_TS_FROM_GJ turns R3 or R4 red"),
           ("NC2", nc2, "-DB5S7_PREV_CLAIMED turns R6 red")]
    for gid, ok, what in res:
        lines.append("[b5s7] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    ok = all(r[1] for r in res)
    lines.append("[result] %s b5-step7" % ("PASS" if ok else "FAIL"))
    return ok, lines, {r[0]: r[1] for r in res}


def _log(arms, done=True, extra=()):
    out = ["[b5s7] seed 0 lr 0.001 build plain"]
    for name, kept in arms.items():
        out.append("[b5s7] arm=%s last=%.4f kept=%.4f at 9 (gate 2.5000) peek=%.4f at 8 lastgate=2.6 took=1/8 app=1"
                   % (name, kept + 0.5, kept, kept))
    out += list(extra)
    if done:
        out.append("[b5s7] done")
    return "\n".join(out) + "\n"


def self_test():
    bad = 0

    def check(name, got, want):
        nonlocal bad
        print("[b5s7-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want

    check("constants D1 0.08 D2 0.05 D34 0.03 D5 0.02 D6 0.02 NEED 7 seeds 131-138",
          (D1, D2, D34, D5, D6, NEED, SEEDS), (0.08, 0.05, 0.03, 0.02, 0.02, 7, "131 132 133 134 135 136 137 138"))
    S = SEEDS.split()

    def P(alone=3.95, trim=3.62, trust=3.59, tg1=3.66, sg1=3.65, sg2=3.80, sf2=3.82, sn2=3.89,
          y1=3.90, y2=3.92, tn1=3.76, sn1=3.90):
        return {"alone-A": alone, "trim4": trim, "trust4": trust, "trim4-garb1": tg1, "trust4-garb1": sg1,
                "trust4-garb2": sg2, "trust4-flip2": sf2, "trust4-near2": sn2,
                "trust4-yori1": y1, "trust4-yori2": y2, "trim4-near1": tn1, "trust4-near1": sn1}
    good = {s: P() for s in S}

    def G(trim=3.62, trust=3.59, tg1=3.66, sg1=3.65):
        return {"alone-A": 3.95, "trim4": trim, "trust4": trust, "trim4-garb1": tg1, "trust4-garb1": sg1}
    GJr = {s: G(trust=3.70) for s in S}                  # NC1 red (R4 red)
    GJg = {s: G() for s in S}                            # NC1 green
    CLr = {s: {"alone-A": 3.95, "trust4-yori1": 4.10, "trust4-yori2": 3.92} for s in S}   # R6 red
    CLg = {s: {"alone-A": 3.95, "trust4-yori1": 3.90, "trust4-yori2": 3.92} for s in S}   # R6 green

    def one(d, s, v):
        x = dict(d)
        x[s] = v
        return x

    def two(d, a, b, v):
        return one(one(d, a, v), b, v)
    allg = {g: True for g in ("R1", "R2", "R3", "R4", "R5", "R6", "NC1", "NC2")}
    cases = [
        ("green", (good, GJr, CLr), True, allg),
        ("R1 one short ok", (one(good, "131", P(sg2=3.90)), GJr, CLr), True, {"R1": True}),
        ("R1 two short", (two(good, "131", "132", P(sg2=3.90)), GJr, CLr), False, {"R1": False, "R2": True}),
        ("R1 at the line", ({s: P(sg2=3.87) for s in S}, GJr, CLr), True, {"R1": True}),
        ("R2 two short", (two(good, "133", "134", P(sf2=3.91)), GJr, CLr), False, {"R2": False}),
        ("R3 two short", (two(good, "133", "134", P(sg1=3.70)), GJr, CLr), False, {"R3": False, "R4": True}),
        ("R4 two short", (two(good, "135", "136", P(trust=3.66)), GJr, CLr), False, {"R4": False, "R3": True}),
        ("R4 at the line", ({s: P(trust=3.65) for s in S}, GJr, CLr), True, {"R4": True}),
        ("R5 near2 one seed", (one(good, "137", P(sn2=3.975)), GJr, CLr), False, {"R5": False, "R6": True}),
        ("R6 yori1 one seed", (one(good, "137", P(y1=3.975)), GJr, CLr), False, {"R6": False, "R5": True}),
        ("R6 yori2 one seed", (one(good, "138", P(y2=3.98)), GJr, CLr), False, {"R6": False}),
        ("R6 at the line", ({s: P(y1=3.97, y2=3.97) for s in S}, GJr, CLr), True, {"R6": True}),
        ("R6 ignores near1", ({s: P(sn1=4.5) for s in S}, GJr, CLr), True, {"R6": True}),
        ("NC1 green", (good, GJg, CLr), False, {"NC1": False, "NC2": True}),
        ("NC1 red by R3 only", (good, {s: G(sg1=3.72) for s in S}, CLr), True, {"NC1": True}),
        ("NC1 missing", (good, one(GJr, "131", None), CLr), False, {"NC1": False}),
        ("NC2 green", (good, GJr, CLg), False, {"NC2": False, "NC1": True}),
        ("NC2 one seed red is red", (good, GJr, one(CLg, "138", CLr["138"])), True, {"NC2": True}),
        ("NC2 missing", (good, GJr, one(CLr, "131", None)), False, {"NC2": False}),
        ("missing seed", (one(good, "138", None), GJr, CLr), False,
         {"R1": False, "R2": False, "R3": False, "R4": False, "R5": False, "R6": False}),
    ]
    for name, (p, g, c), want, gw in cases:
        got, _, r = judge(p, g, c)
        check(name, got, want)
        for gid, w in gw.items():
            check("%s: %s" % (name, gid), r[gid], w)

    d = tempfile.mkdtemp()
    try:
        def put(name, text):
            p = os.path.join(d, name)
            open(p, "w").write(text)
            return p
        arms = P()
        pa = {k: arms[k] for k in PARTS["a"]}
        check("read: kept, not last", read(put("k.log", _log(pa)), PARTS["a"]), pa)
        check("read: no done -> None", read(put("nd.log", _log(pa, done=False)), PARTS["a"]), None)
        dup = _log(pa, extra=["[b5s7] arm=trim4 last=9.0 kept=3.0000 at 1 (gate 2.0)"])
        check("read: an arm twice -> None", read(put("dup.log", dup), PARTS["a"]), None)
        check("read: a step 6 log -> None", read(put("s6.log", _log(pa).replace("[b5s7]", "[b5s6]")), PARTS["a"]), None)

        def write_all(gj_trust=3.70, cl_y1=4.10):
            for s in S:
                for p, names in PARTS.items():
                    put("plain-%s-%s.log" % (s, p), _log({k: arms[k] for k in names}))
                ga = {k: arms[k] for k in PARTS["a"]}
                ga["trust4"] = gj_trust
                put("gj-%s.log" % s, _log(ga))
                put("claimed-%s.log" % s, _log({"trust4-yori1": cl_y1, "trust4-yori2": 3.92}))

        def run_main():
            null = open(os.devnull, "w")
            old, sys.stdout = sys.stdout, null
            try:
                return main(["x", d])
            finally:
                sys.stdout = old
                null.close()
        write_all()
        check("main: directory PASS", run_main(), 0)
        check("read_plain merges the four parts", read_plain(d, "131"), arms)
        os.remove(os.path.join(d, "plain-132-y.log"))
        check("main: a plain part missing -> rc 1", run_main(), 1)
        write_all(gj_trust=3.59)
        check("main: NC1 all green -> rc 1", run_main(), 1)
        write_all(cl_y1=3.90)
        check("main: NC2 all green -> rc 1", run_main(), 1)
    finally:
        shutil.rmtree(d)
    return bad


def main(argv):
    if argv[1:] == ["--self-test"]:
        return 1 if self_test() else 0
    seeds, d, i = SEEDS, None, 1
    while i < len(argv):
        if argv[i] == "--seeds":
            seeds = argv[i + 1]
            i += 2
        else:
            d = argv[i]
            i += 1
    if d is None:
        print(__doc__)
        return 2
    sl = seeds.split()
    plain = {s: read_plain(d, s) for s in sl}
    gj = {s: read_nc(d, "gj", s, "a", plain[s]) for s in sl}
    claimed = {s: read_nc(d, "claimed", s, "y", plain[s]) for s in sl}
    ok, lines, _ = judge(plain, gj, claimed)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
