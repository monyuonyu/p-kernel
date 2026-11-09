"""b5_step9_check.py - the judge of B5 step 9 (research/b5-step9-design.md).

Reads the logs written by run_b5_step9.sh (student_adopt_test.c, stages s9a
s9b s9y s9c s9d = step 8's arms, s9p = pair2 (the two poisons are sent to
nobody), s9r1 s9r2 = garb2 and near2 with the poisons' noise seed shifted by
7919 x rep, s9n = near2 of the NC1 build). Each arm prints one line

  [b5s9] arm=<name> last=<bpb> kept=<bpb> at <pass> (gate <nats>) ...

kept is node A's model at its best pass on the gate window. Lines fixed in the
design (section 3) before the code and before any run on the seeds 149..156:

  R1-R6 R8 R9  step 8's lines (b5_step8_check.py), on the step 9 arms (rep 0)
  R7'  median(near2 rep 0, 1, 2) <= pair2 + 0.03                on >= 7 seeds
  NC1  the build -DB5S8_NO_ECHO turns R7' red (its near2 rep 0 alone, pair2
       from the plain log of the seed)
Readings (not lines): step 8's R7 (near2 - garb2), garb2 - pair2 (median and
range over rep 0 1 2), the range of near2.

  b5_step9_check.py [--seeds "149 ... 156"] <dir>   exit 0 = R1-R9 (R7') green, NC1 red
  b5_step9_check.py --self-test
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
D7P = 0.03
D8 = 0.03
D9 = 0.02
NEED = 7
SEEDS = " ".join(str(s) for s in range(149, 157))
ARM = re.compile(r"^\[b5s9\] arm=(\S+) last=([0-9.]+) kept=([0-9.]+) at (\d+) \(gate ([0-9.]+)\)")
NEARS = ["trust4-near2", "trust4-near2-r1", "trust4-near2-r2"]
GARBS = ["trust4-garb2", "trust4-garb2-r1", "trust4-garb2-r2"]
PARTS = {"a": ["alone-A", "trim4", "trust4", "trim4-garb1", "trust4-garb1"],
         "b": ["trust4-garb2", "trust4-flip2", "trust4-near2"],
         "y": ["trust4-yori1", "trust4-yori2"],
         "c": ["trim4-near1", "trust4-near1"],
         "d": ["trust4-ada2"],
         "p": ["trust4-pair2"],
         "r1": ["trust4-garb2-r1", "trust4-near2-r1"],
         "r2": ["trust4-garb2-r2", "trust4-near2-r2"]}
NC_PART = ["trust4-near2"]


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
        elif line == "[b5s9] done":
            done = True
    if not done or any(a not in arms for a in needed):
        return None
    return arms


def read_parts(d, s):
    """{arm: kept} of every finished plain part of seed s (possibly empty)"""
    out = {}
    for p, needed in PARTS.items():
        a = read(os.path.join(d, "plain-%s-%s.log" % (s, p)), needed)
        if a is not None:
            out.update(a)
    return out


def read_plain(d, s):
    a = read_parts(d, s)
    return a if all(k in a for ks in PARTS.values() for k in ks) else None


def read_nc(d, s, plain):
    """NC1's near2 with pair2 from the plain arms; None if either is missing"""
    a = read(os.path.join(d, "noecho-%s.log" % s), NC_PART)
    if a is None or plain is None or "trust4-pair2" not in plain:
        return None
    return {"trust4-near2": a["trust4-near2"], "trust4-pair2": plain["trust4-pair2"]}


def median(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2.0


def count(seeds, test, label, lines, need):
    ok, n = 0, 0
    for s in sorted(seeds, key=int):
        a = seeds[s]
        if a is None:
            lines.append("[b5s9] %s seed %s: log missing or short" % (label, s))
            continue
        n += 1
        g, text = test(a)
        ok += g
        lines.append("[b5s9] %s seed %s: %s -> %s" % (label, s, text, "ok" if g else "RED"))
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


def r7p(a):
    """R7': the median of the near2 runs present (3 in plain, 1 in NC1) against pair2"""
    nv = [a[k] for k in NEARS if k in a]
    m = median(nv)
    d = m - a["trust4-pair2"]
    return d <= D7P + 1e-9, "near2 median %.4f of %d pair2 %.4f (%+.4f)" % (m, len(nv), a["trust4-pair2"], d)


def r8(a):
    d = a["trust4-near1"] - a["trim4-near1"]
    return d <= D8 + 1e-9, "trust-near1 %.4f trim-near1 %.4f (%+.4f)" % (a["trust4-near1"], a["trim4-near1"], d)


def r9(a):
    d = a["trust4-ada2"] - a["alone-A"]
    return d <= D9 + 1e-9, "trust-ada2 %.4f alone-A %.4f (%+.4f)" % (a["trust4-ada2"], a["alone-A"], d)


def judge(plain, noecho):
    """-> (ok, lines, {gate id: bool})"""
    lines = []
    need = min(NEED, len(plain))
    g1 = count(plain, r1, "R1", lines, need)
    g2 = count(plain, r2, "R2", lines, need)
    g3 = count(plain, r3, "R3", lines, need)
    g4 = count(plain, r4, "R4", lines, need)
    g5 = count(plain, r5, "R5", lines, len(plain))
    g6 = count(plain, r6, "R6", lines, len(plain))
    g7 = count(plain, r7p, "R7'", lines, need)
    g8 = count(plain, r8, "R8", lines, need)
    g9 = count(plain, r9, "R9", lines, len(plain))
    n17 = count(noecho, r7p, "NC1 (B5S8_NO_ECHO) R7'", lines, min(NEED, len(noecho)))
    nc1 = not n17 and all(v is not None for v in noecho.values())
    res = [("R1", g1, "trust4-garb2 <= alone-A - %.2f on %d+ seeds" % (D1, NEED)),
           ("R2", g2, "trust4-flip2 <= alone-A - %.2f on %d+ seeds" % (D2, NEED)),
           ("R3", g3, "trust4-garb1 <= trim4-garb1 + %.2f on %d+ seeds" % (D34, NEED)),
           ("R4", g4, "trust4 <= trim4 + %.2f on %d+ seeds" % (D34, NEED)),
           ("R5", g5, "every seed: trust4 garb2/flip2/near2 <= alone-A + %.2f" % D5),
           ("R6", g6, "every seed: trust4 yori1/yori2 <= alone-A + %.2f" % D6),
           ("R7'", g7, "median(near2 rep 0 1 2) <= trust4-pair2 + %.2f on %d+ seeds" % (D7P, NEED)),
           ("R8", g8, "trust4-near1 <= trim4-near1 + %.2f on %d+ seeds" % (D8, NEED)),
           ("R9", g9, "every seed: trust4-ada2 <= alone-A + %.2f" % D9),
           ("NC1", nc1, "-DB5S8_NO_ECHO turns R7' red")]
    for gid, ok, what in res:
        lines.append("[b5s9] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    ok = all(r[1] for r in res)
    # H13 (audit-158): only the 8 fixed seeds are the judgement
    full = sorted(plain, key=int) == sorted(SEEDS.split(), key=int)
    if not full:
        lines.append("[result] PARTIAL b5-step9 (%d of the %d judgement seeds: a reading, not the judgement)"
                     % (len([s for s in plain if s in SEEDS.split()]), len(SEEDS.split())))
        return False, lines, {r[0]: r[1] for r in res}
    lines.append("[result] %s b5-step9" % ("PASS" if ok else "FAIL"))
    return ok, lines, {r[0]: r[1] for r in res}


def readings(d, sl):
    """per seed, whatever parts are finished: R7' so far, step 8's R7, garb2 - pair2, the near2 range"""
    out = []
    for s in sl:
        a = read_parts(d, s)
        bits = []
        nv = [a[k] for k in NEARS if k in a]
        gv = [a[k] for k in GARBS if k in a]
        if "trust4-pair2" in a and nv:
            bits.append("near2 median(%d) - pair2 %+.4f" % (len(nv), median(nv) - a["trust4-pair2"]))
        if "trust4-near2" in a and "trust4-garb2" in a:
            bits.append("step 8 R7 near2 - garb2 %+.4f" % (a["trust4-near2"] - a["trust4-garb2"]))
        if "trust4-pair2" in a and gv:
            dv = [g - a["trust4-pair2"] for g in gv]
            bits.append("garb2 - pair2 median(%d) %+.4f range %+.4f..%+.4f" % (len(dv), median(dv), min(dv), max(dv)))
        if len(nv) > 1:
            bits.append("near2 range %.4f" % (max(nv) - min(nv)))
        nc = read(os.path.join(d, "noecho-%s.log" % s), NC_PART)
        if nc is not None and "trust4-pair2" in a:
            bits.append("NC1 near2 - pair2 %+.4f" % (nc["trust4-near2"] - a["trust4-pair2"]))
        out.append("[b5s9] reading seed %s: %s" % (s, "; ".join(bits) if bits else "nothing finished"))
    return out


def _log(arms, done=True, extra=()):
    out = ["[b5s9] seed 0 lr 0.001 build plain"]
    for name, kept in arms.items():
        out.append("[b5s9] arm=%s last=%.4f kept=%.4f at 9 (gate 2.5000) peek=%.4f at 8 lastgate=2.6 took=1/8 app=1"
                   % (name, kept + 0.5, kept, kept))
    out += list(extra)
    if done:
        out.append("[b5s9] done")
    return "\n".join(out) + "\n"


def self_test():
    bad = 0

    def check(name, got, want):
        nonlocal bad
        print("[b5s9-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want

    check("constants D1 0.08 D2 0.05 D34 0.03 D5 0.02 D6 0.02 D7P 0.03 D8 0.03 D9 0.02 NEED 7 seeds 149-156",
          (D1, D2, D34, D5, D6, D7P, D8, D9, NEED, SEEDS),
          (0.08, 0.05, 0.03, 0.02, 0.02, 0.03, 0.03, 0.02, 7, "149 150 151 152 153 154 155 156"))
    S = SEEDS.split()

    def P(alone=3.95, trim=3.62, trust=3.59, tg1=3.66, sg1=3.65, sg2=3.75, sf2=3.82, sn2=3.73,
          y1=3.90, y2=3.92, tn1=3.76, sn1=3.64, ad2=3.87, pr=3.72, n1=3.74, n2=3.72, g1=3.76, g2=3.74):
        return {"alone-A": alone, "trim4": trim, "trust4": trust, "trim4-garb1": tg1, "trust4-garb1": sg1,
                "trust4-garb2": sg2, "trust4-flip2": sf2, "trust4-near2": sn2,
                "trust4-yori1": y1, "trust4-yori2": y2, "trim4-near1": tn1, "trust4-near1": sn1,
                "trust4-ada2": ad2, "trust4-pair2": pr, "trust4-near2-r1": n1, "trust4-near2-r2": n2,
                "trust4-garb2-r1": g1, "trust4-garb2-r2": g2}
    good = {s: P() for s in S}

    def NB(sn2=3.87, pr=3.72):
        return {"trust4-near2": sn2, "trust4-pair2": pr}
    NEr = {s: NB() for s in S}                       # NC1 red (+0.15)
    NEg = {s: NB(sn2=3.74) for s in S}               # NC1 green (+0.02)

    def one(d, s, v):
        x = dict(d)
        x[s] = v
        return x

    def two(d, a, b, v):
        return one(one(d, a, v), b, v)
    allg = {g: True for g in ("R1", "R2", "R3", "R4", "R5", "R6", "R7'", "R8", "R9", "NC1")}
    cases = [
        ("green", (good, NEr), True, allg),
        ("R1 two short", (two(good, S[0], S[1], P(sg2=3.90)), NEr), False, {"R1": False, "R7'": True}),
        ("R2 two short", (two(good, S[2], S[3], P(sf2=3.91)), NEr), False, {"R2": False}),
        ("R3 two short", (two(good, S[2], S[3], P(sg1=3.70)), NEr), False, {"R3": False, "R4": True}),
        ("R4 two short", (two(good, S[4], S[5], P(trust=3.66)), NEr), False, {"R4": False, "R3": True}),
        ("R5 near2 one seed", (one(good, S[6], P(sn2=3.975, n1=3.70, n2=3.70)), NEr), False, {"R5": False, "R7'": True}),
        ("R6 yori1 one seed", (one(good, S[6], P(y1=3.975)), NEr), False, {"R6": False, "R5": True}),
        ("R7' one short ok", (one(good, S[0], P(sn2=3.80, n1=3.80)), NEr), True, {"R7'": True}),
        ("R7' two short", (two(good, S[0], S[1], P(sn2=3.80, n1=3.80)), NEr), False, {"R7'": False, "R5": True}),
        ("R7' at the line (median 3.75 = pair2 + 0.03)", ({s: P(sn2=3.75, n1=3.75, n2=3.70) for s in S}, NEr), True,
         {"R7'": True}),
        ("R7' just over the line", ({s: P(sn2=3.7505, n1=3.7505, n2=3.70) for s in S}, NEr), False, {"R7'": False}),
        ("R7' is the median: one bad rep is not red", ({s: P(sn2=3.98, n1=3.72, n2=3.73) for s in S}, NEr), False,
         {"R7'": True, "R5": False}),
        ("R7' is the median: two bad reps are red", ({s: P(n1=3.80, n2=3.80) for s in S}, NEr), False, {"R7'": False}),
        ("R7' is against pair2, not garb2 (garb2 low)", ({s: P(sg2=3.60, g1=3.60, g2=3.60, sn2=3.74, n1=3.74, n2=3.74)
                                                          for s in S}, NEr), True, {"R7'": True}),
        ("R7' is against pair2 (pair2 low)", ({s: P(pr=3.60) for s in S}, NEr), False, {"R7'": False}),
        ("R8 two short", (two(good, S[2], S[7], P(sn1=3.80)), NEr), False, {"R8": False, "R7'": True}),
        ("R9 one seed", (one(good, S[7], P(ad2=3.975)), NEr), False, {"R9": False, "R6": True}),
        ("NC1 green", (good, NEg), False, {"NC1": False}),
        ("NC1 missing", (good, one(NEr, S[0], None)), False, {"NC1": False}),
        ("NC1 red on two seeds is enough", (good, {s: (NEr[s] if s in S[:2] else NEg[s]) for s in S}), True,
         {"NC1": True}),
        ("NC1 red on one seed is not", (good, {s: (NEr[s] if s == S[0] else NEg[s]) for s in S}), False,
         {"NC1": False}),
        ("missing seed", (one(good, S[7], None), NEr), False, {"R1": False, "R7'": False, "R9": False}),
    ]
    for name, (p, g), want, gw in cases:
        got, _, r = judge(p, g)
        check(name, got, want)
        for gid, w in gw.items():
            check("%s: %s" % (name, gid), r[gid], w)
    for k in (1, 7):
        sub = S[:k]
        got, ls, r = judge({s: good[s] for s in sub}, {s: NEr[s] for s in sub})
        check("H13 %d seed(s): not PASS" % k, got, False)
        check("H13 %d seed(s): result PARTIAL" % k, ls[-1].startswith("[result] PARTIAL"), True)
    sw = S[:7] + ["148"]
    got, ls, r = judge({a: good[S[0]] for a in sw}, {a: NEr[S[0]] for a in sw})
    check("H13 8 seeds, one a seen seed (148): not PASS", got, False)
    check("H13 8 seeds, one swapped: result PARTIAL", ls[-1].startswith("[result] PARTIAL"), True)
    check("median of 1 2 3", (median([3.0]), median([1.0, 3.0]), median([3.0, 1.0, 2.0])), (3.0, 2.0, 2.0))

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
        dup = _log(pa, extra=["[b5s9] arm=trim4 last=9.0 kept=3.0000 at 1 (gate 2.0)"])
        check("read: an arm twice -> None", read(put("dup.log", dup), PARTS["a"]), None)
        check("read: a step 8 log -> None", read(put("s8.log", _log(pa).replace("[b5s9]", "[b5s8]")), PARTS["a"]), None)

        def write_all(ne_near2=3.87):
            for s in S:
                for p, names in PARTS.items():
                    put("plain-%s-%s.log" % (s, p), _log({k: arms[k] for k in names}))
                put("noecho-%s.log" % s, _log({"trust4-near2": ne_near2}))

        def run_main(extra=()):
            null = open(os.devnull, "w")
            old, sys.stdout = sys.stdout, null
            try:
                return main(["x"] + list(extra) + [d])
            finally:
                sys.stdout = old
                null.close()
        write_all()
        check("main: directory PASS", run_main(), 0)
        check("main: --seeds 156 only -> rc 1 (PARTIAL)", run_main(["--seeds", "156"]), 1)
        check("read_plain merges the eight parts", read_plain(d, S[0]), arms)
        os.remove(os.path.join(d, "plain-%s-r2.log" % S[1]))
        check("main: a plain part missing -> rc 1", run_main(), 1)
        check("read_parts keeps the finished parts", "trust4-near2-r1" in read_parts(d, S[1])
              and "trust4-near2-r2" not in read_parts(d, S[1]), True)
        write_all(ne_near2=3.74)
        check("main: NC1 all green -> rc 1", run_main(), 1)
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
    noecho = {s: read_nc(d, s, plain[s]) for s in sl}
    ok, lines, _ = judge(plain, noecho)
    print("\n".join(lines))
    print("\n".join(readings(d, sl)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
