"""b5_step6_check.py - the judge of B5 step 6 (research/b5-step6-design.md).

Reads the logs written by run_b5_step6.sh (student_adopt_test.c, stages s6a
s6b s6c s6r1). Each arm prints one line

  [b5s6] arm=<name> last=<bpb> kept=<bpb> at <pass> (gate <nats>) ... took=<n>/<m> ...

kept is node A's model at its best pass on the gate window; the ruler
(b1_held.txt) never chooses. Four nodes, each a quarter. Lines fixed in the
design (section 4) before the code and before any run on the seeds 123..130:

  R1  trust4-garb2 <= alone-A - 0.08                          on >= 7 seeds
  R2  trust4-flip2 <= alone-A - 0.05                          on >= 7 seeds
  R3  trust4-garb1 <= trim4-garb1 + 0.03                      on >= 7 seeds
  R4  trust4 <= trim4 + 0.03                                  on >= 7 seeds
  R5  trust4-garb2, trust4-flip2, trust4-near2 <= alone-A + 0.02   every seed
  NC1 the build -DB5S6_TRUST_FLAT (every TS 1) turns R1 red
  NC2 the build -DB5S6_NO_NORM (no scaling to |g0|) turns R1 red
The NC builds run trust4-garb2 only; alone-A (no merge, the same in every
build) comes from the plain log of the seed. near1 (trim and trust) is printed
as a reading, not a line.

  b5_step6_check.py [--seeds "123 ... 130"] <dir>   exit 0 = R1-R5 green, NC1 NC2 red
  b5_step6_check.py --self-test
"""
import os
import re
import shutil
import sys
import tempfile

D1 = 0.08      # R1: half the smallest gap on the seen seeds 91 92 (0.160)
D2 = 0.05      # R2: just under half of 0.118
D34 = 0.03     # R3, R4
D5 = 0.02      # R5
NEED = 7
SEEDS = " ".join(str(s) for s in range(123, 131))
ARM = re.compile(r"^\[b5s6\] arm=(\S+) last=([0-9.]+) kept=([0-9.]+) at (\d+) \(gate ([0-9.]+)\)")
PARTS = {"a": ["alone-A", "trim4", "trust4", "trim4-garb1", "trust4-garb1"],
         "b": ["trust4-garb2", "trust4-flip2", "trust4-near2"],
         "c": ["trim4-near1", "trust4-near1"]}
NC_ARMS = ["trust4-garb2"]


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
        elif line == "[b5s6] done":
            done = True
    if not done or any(a not in arms for a in needed):
        return None
    return arms


def read_plain(d, s):
    """the three plain logs of seed s merged, or None if any is missing or short"""
    out = {}
    for p, needed in PARTS.items():
        a = read(os.path.join(d, "plain-%s-%s.log" % (s, p)), needed)
        if a is None:
            return None
        out.update(a)
    return out


def read_nc(d, b, s, plain):
    """trust4-garb2 of build b with alone-A of the plain log; None if either is missing"""
    a = read(os.path.join(d, "%s-%s.log" % (b, s)), NC_ARMS)
    if a is None or plain is None:
        return None
    return {"alone-A": plain["alone-A"], "trust4-garb2": a["trust4-garb2"]}


def count(seeds, test, label, lines, need):
    ok, n = 0, 0
    for s in sorted(seeds, key=int):
        a = seeds[s]
        if a is None:
            lines.append("[b5s6] %s seed %s: log missing or short" % (label, s))
            continue
        n += 1
        g, text = test(a)
        ok += g
        lines.append("[b5s6] %s seed %s: %s -> %s" % (label, s, text, "ok" if g else "RED"))
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


def near1(plain, lines):
    for s in sorted(plain, key=int):
        a = plain[s]
        if a is not None:
            lines.append("[b5s6] reading near1 seed %s: trust %.4f trim %.4f (%+.4f)"
                         % (s, a["trust4-near1"], a["trim4-near1"], a["trust4-near1"] - a["trim4-near1"]))


def judge(plain, flat, nonorm):
    """-> (ok, lines, {gate id: bool})"""
    lines = []
    need = min(NEED, len(plain))
    g1 = count(plain, r1, "R1", lines, need)
    g2 = count(plain, r2, "R2", lines, need)
    g3 = count(plain, r3, "R3", lines, need)
    g4 = count(plain, r4, "R4", lines, need)
    g5 = count(plain, r5, "R5", lines, len(plain))
    n1 = count(flat, r1, "NC1 (B5S6_TRUST_FLAT) R1", lines, min(NEED, len(flat)))
    n2 = count(nonorm, r1, "NC2 (B5S6_NO_NORM) R1", lines, min(NEED, len(nonorm)))
    nc1 = not n1 and all(v is not None for v in flat.values())
    nc2 = not n2 and all(v is not None for v in nonorm.values())
    near1(plain, lines)
    res = [("R1", g1, "trust4-garb2 <= alone-A - %.2f on %d+ seeds" % (D1, NEED)),
           ("R2", g2, "trust4-flip2 <= alone-A - %.2f on %d+ seeds" % (D2, NEED)),
           ("R3", g3, "trust4-garb1 <= trim4-garb1 + %.2f on %d+ seeds" % (D34, NEED)),
           ("R4", g4, "trust4 <= trim4 + %.2f on %d+ seeds" % (D34, NEED)),
           ("R5", g5, "every seed: trust4 garb2/flip2/near2 <= alone-A + %.2f" % D5),
           ("NC1", nc1, "-DB5S6_TRUST_FLAT turns R1 red"),
           ("NC2", nc2, "-DB5S6_NO_NORM turns R1 red")]
    for gid, ok, what in res:
        lines.append("[b5s6] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    ok = all(r[1] for r in res)
    # test-holes H14 (as H13 in step 7): a run on other than the 8 judgement
    # seeds is a reading: PARTIAL, never PASS (the gate lines stay).
    if sorted(plain, key=int) != sorted(SEEDS.split(), key=int):
        lines.append("[result] PARTIAL b5-step6 (%d of the %d judgement seeds: a reading, not the judgement)"
                     % (len([s for s in plain if s in SEEDS.split()]), len(SEEDS.split())))
        return False, lines, {r[0]: r[1] for r in res}
    lines.append("[result] %s b5-step6" % ("PASS" if ok else "FAIL"))
    return ok, lines, {r[0]: r[1] for r in res}


def _log(arms, done=True, extra=()):
    out = ["[b5s6] seed 0 lr 0.001 build plain"]
    for name, kept in arms.items():
        out.append("[b5s6] arm=%s last=%.4f kept=%.4f at 9 (gate 2.5000) peek=%.4f at 8 lastgate=2.6 took=1/8 app=1"
                   % (name, kept + 0.5, kept, kept))
    out += list(extra)
    if done:
        out.append("[b5s6] done")
    return "\n".join(out) + "\n"


def self_test():
    bad = 0

    def check(name, got, want):
        nonlocal bad
        print("[b5s6-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want

    check("constants D1 0.08 D2 0.05 D34 0.03 D5 0.02 NEED 7 seeds 123-130",
          (D1, D2, D34, D5, NEED, SEEDS), (0.08, 0.05, 0.03, 0.02, 7, "123 124 125 126 127 128 129 130"))
    S = SEEDS.split()

    def P(alone=3.95, trim=3.62, trust=3.59, tg1=3.66, sg1=3.65, sg2=3.80, sf2=3.82, sn2=3.89, tn1=3.76, sn1=3.90):
        return {"alone-A": alone, "trim4": trim, "trust4": trust, "trim4-garb1": tg1, "trust4-garb1": sg1,
                "trust4-garb2": sg2, "trust4-flip2": sf2, "trust4-near2": sn2, "trim4-near1": tn1, "trust4-near1": sn1}
    good = {s: P() for s in S}
    NCr = {s: {"alone-A": 3.95, "trust4-garb2": 3.95} for s in S}     # R1 red
    NCg = {s: {"alone-A": 3.95, "trust4-garb2": 3.80} for s in S}     # R1 green

    def one(d, s, v):
        x = dict(d)
        x[s] = v
        return x

    def two(d, a, b, v):
        return one(one(d, a, v), b, v)
    allg = {g: True for g in ("R1", "R2", "R3", "R4", "R5", "NC1", "NC2")}
    cases = [
        ("green", (good, NCr, NCr), True, allg),
        ("R1 one short ok", (one(good, "123", P(sg2=3.90)), NCr, NCr), True, {"R1": True}),
        ("R1 two short", (two(good, "123", "124", P(sg2=3.90)), NCr, NCr), False, {"R1": False, "R2": True}),
        ("R1 at the line", ({s: P(sg2=3.87) for s in S}, NCr, NCr), True, {"R1": True}),
        ("R1 0.005 over, two", (two(good, "123", "124", P(sg2=3.875)), NCr, NCr), False, {"R1": False}),
        ("R2 two short", (two(good, "125", "126", P(sf2=3.91)), NCr, NCr), False, {"R2": False, "R1": True}),
        ("R2 at the line", ({s: P(sf2=3.90) for s in S}, NCr, NCr), True, {"R2": True}),
        ("R3 two short", (two(good, "125", "126", P(sg1=3.70)), NCr, NCr), False, {"R3": False, "R4": True}),
        ("R3 at the line", ({s: P(sg1=3.69) for s in S}, NCr, NCr), True, {"R3": True}),
        ("R4 two short", (two(good, "127", "128", P(trust=3.66)), NCr, NCr), False, {"R4": False, "R3": True}),
        ("R4 at the line", ({s: P(trust=3.65) for s in S}, NCr, NCr), True, {"R4": True}),
        ("R5 near2 one seed", (one(good, "129", P(sn2=3.975)), NCr, NCr), False, {"R5": False, "R1": True}),
        ("R5 flip2 one seed", (one(good, "129", P(sf2=3.98)), NCr, NCr), False, {"R5": False}),
        ("R5 at the line", ({s: P(sn2=3.97) for s in S}, NCr, NCr), True, {"R5": True}),
        ("R5 ignores near1", ({s: P(sn1=4.5) for s in S}, NCr, NCr), True, {"R5": True}),
        ("NC1 green", (good, NCg, NCr), False, {"NC1": False, "NC2": True}),
        ("NC1 missing", (good, one(NCr, "123", None), NCr), False, {"NC1": False}),
        ("NC2 green", (good, NCr, NCg), False, {"NC2": False, "NC1": True}),
        ("NC2 one seed green, still red", (good, NCr, one(NCr, "130", NCg["130"])), True, {"NC2": True}),
        ("missing seed", (one(good, "130", None), NCr, NCr), False,
         {"R1": False, "R2": False, "R3": False, "R4": False, "R5": False}),
    ]
    for name, (p, f, n), want, gw in cases:
        got, _, r = judge(p, f, n)
        check(name, got, want)
        for gid, w in gw.items():
            check("%s: %s" % (name, gid), r[gid], w)
    # H14: a good run on 1 or 7 seeds, or on 8 with one not a judgement seed, is PARTIAL
    for name, sub in (("1 seed", S[:1]), ("7 seeds", S[:7]), ("8 seeds, one swapped", S[:7] + ["999"])):
        got, ls, r = judge({s: P() for s in sub}, {s: NCr[S[0]] for s in sub}, {s: NCr[S[0]] for s in sub})
        check("H14 %s: not PASS" % name, got, False)
        check("H14 %s: the gates still green" % name, all(r.values()), True)
        check("H14 %s: result PARTIAL" % name, ls[-1].startswith("[result] PARTIAL"), True)

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
        dup = _log(pa, extra=["[b5s6] arm=trim4 last=9.0 kept=3.0000 at 1 (gate 2.0)"])
        check("read: an arm twice -> None", read(put("dup.log", dup), PARTS["a"]), None)
        check("read: a step 5 log -> None", read(put("s5.log", _log(pa).replace("[b5s6]", "[b5s5]")), PARTS["a"]), None)
        check("read: no file -> None", read(os.path.join(d, "nofile.log"), PARTS["a"]), None)

        def write_all(sg2_flat=3.95, sg2_nonorm=3.95):
            for s in S:
                for p, names in PARTS.items():
                    put("plain-%s-%s.log" % (s, p), _log({k: arms[k] for k in names}))
                put("flat-%s.log" % s, _log({"trust4-garb2": sg2_flat}))
                put("nonorm-%s.log" % s, _log({"trust4-garb2": sg2_nonorm}))

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
        check("read_plain merges the three parts", read_plain(d, "123"), arms)
        check("read_nc takes alone-A from plain", read_nc(d, "flat", "123", arms), {"alone-A": 3.95, "trust4-garb2": 3.95})
        os.remove(os.path.join(d, "plain-124-c.log"))
        check("main: a plain part missing -> rc 1", run_main(), 1)
        write_all(sg2_nonorm=3.80)
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
    flat = {s: read_nc(d, "flat", s, plain[s]) for s in sl}
    nonorm = {s: read_nc(d, "nonorm", s, plain[s]) for s in sl}
    ok, lines, _ = judge(plain, flat, nonorm)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
