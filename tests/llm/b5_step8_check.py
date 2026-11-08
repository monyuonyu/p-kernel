"""b5_step8_check.py - the judge of B5 step 8 (research/b5-step8-design.md).

Reads the logs written by run_b5_step8.sh (student_adopt_test.c, stages s8a
s8b s8y s8c s8d: step 7's arms with the trust arms on st_merge_trust_echo, and
the adaptive near poison). Each arm prints one line

  [b5s8] arm=<name> last=<bpb> kept=<bpb> at <pass> (gate <nats>) ... took=<n>/<m> ...

kept is node A's model at its best pass on the gate window. Lines fixed in the
design (section 3) before the code and before any run on the seeds 141..148:

  R1-R6  step 7's lines (b5_step7_check.py), on the step 8 arms
  R7  trust4-near2 <= trust4-garb2 + 0.05                     on >= 7 seeds
  R8  trust4-near1 <= trim4-near1 + 0.03                      on >= 7 seeds
  R9  trust4-ada2 <= alone-A + 0.02                           every seed
  NC1 the build -DB5S8_NO_ECHO (step 7, no cut) turns R7 red
  NC2 the build -DB5S8_TAU_LOW (tau 0.1) turns R4 red
The noecho build runs s8b, the taulow build s8a; alone-A of an NC check comes
from the plain log of the seed.

  b5_step8_check.py [--seeds "141 ... 148"] <dir>   exit 0 = R1-R9 green, NC1 NC2 red
  b5_step8_check.py --self-test
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
D7 = 0.05
D8 = 0.03
D9 = 0.02
NEED = 7
SEEDS = " ".join(str(s) for s in range(141, 149))
ARM = re.compile(r"^\[b5s8\] arm=(\S+) last=([0-9.]+) kept=([0-9.]+) at (\d+) \(gate ([0-9.]+)\)")
PARTS = {"a": ["alone-A", "trim4", "trust4", "trim4-garb1", "trust4-garb1"],
         "b": ["trust4-garb2", "trust4-flip2", "trust4-near2"],
         "y": ["trust4-yori1", "trust4-yori2"],
         "c": ["trim4-near1", "trust4-near1"],
         "d": ["trust4-ada2"]}


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
        elif line == "[b5s8] done":
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
            lines.append("[b5s8] %s seed %s: log missing or short" % (label, s))
            continue
        n += 1
        g, text = test(a)
        ok += g
        lines.append("[b5s8] %s seed %s: %s -> %s" % (label, s, text, "ok" if g else "RED"))
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


def r7(a):
    d = a["trust4-near2"] - a["trust4-garb2"]
    return d <= D7 + 1e-9, "trust-near2 %.4f trust-garb2 %.4f (%+.4f)" % (a["trust4-near2"], a["trust4-garb2"], d)


def r8(a):
    d = a["trust4-near1"] - a["trim4-near1"]
    return d <= D8 + 1e-9, "trust-near1 %.4f trim-near1 %.4f (%+.4f)" % (a["trust4-near1"], a["trim4-near1"], d)


def r9(a):
    d = a["trust4-ada2"] - a["alone-A"]
    return d <= D9 + 1e-9, "trust-ada2 %.4f alone-A %.4f (%+.4f)" % (a["trust4-ada2"], a["alone-A"], d)


def judge(plain, noecho, taulow):
    """-> (ok, lines, {gate id: bool})"""
    lines = []
    need = min(NEED, len(plain))
    g1 = count(plain, r1, "R1", lines, need)
    g2 = count(plain, r2, "R2", lines, need)
    g3 = count(plain, r3, "R3", lines, need)
    g4 = count(plain, r4, "R4", lines, need)
    g5 = count(plain, r5, "R5", lines, len(plain))
    g6 = count(plain, r6, "R6", lines, len(plain))
    g7 = count(plain, r7, "R7", lines, need)
    g8 = count(plain, r8, "R8", lines, need)
    g9 = count(plain, r9, "R9", lines, len(plain))
    n17 = count(noecho, r7, "NC1 (B5S8_NO_ECHO) R7", lines, min(NEED, len(noecho)))
    n24 = count(taulow, r4, "NC2 (B5S8_TAU_LOW) R4", lines, min(NEED, len(taulow)))
    nc1 = not n17 and all(v is not None for v in noecho.values())
    nc2 = not n24 and all(v is not None for v in taulow.values())
    res = [("R1", g1, "trust4-garb2 <= alone-A - %.2f on %d+ seeds" % (D1, NEED)),
           ("R2", g2, "trust4-flip2 <= alone-A - %.2f on %d+ seeds" % (D2, NEED)),
           ("R3", g3, "trust4-garb1 <= trim4-garb1 + %.2f on %d+ seeds" % (D34, NEED)),
           ("R4", g4, "trust4 <= trim4 + %.2f on %d+ seeds" % (D34, NEED)),
           ("R5", g5, "every seed: trust4 garb2/flip2/near2 <= alone-A + %.2f" % D5),
           ("R6", g6, "every seed: trust4 yori1/yori2 <= alone-A + %.2f" % D6),
           ("R7", g7, "trust4-near2 <= trust4-garb2 + %.2f on %d+ seeds" % (D7, NEED)),
           ("R8", g8, "trust4-near1 <= trim4-near1 + %.2f on %d+ seeds" % (D8, NEED)),
           ("R9", g9, "every seed: trust4-ada2 <= alone-A + %.2f" % D9),
           ("NC1", nc1, "-DB5S8_NO_ECHO turns R7 red"),
           ("NC2", nc2, "-DB5S8_TAU_LOW turns R4 red")]
    for gid, ok, what in res:
        lines.append("[b5s8] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    ok = all(r[1] for r in res)
    # H13 (audit-158) from the start: only the 8 fixed seeds are the judgement
    full = sorted(plain, key=int) == sorted(SEEDS.split(), key=int)
    if not full:
        lines.append("[result] PARTIAL b5-step8 (%d of the %d judgement seeds: a reading, not the judgement)"
                     % (len([s for s in plain if s in SEEDS.split()]), len(SEEDS.split())))
        return False, lines, {r[0]: r[1] for r in res}
    lines.append("[result] %s b5-step8" % ("PASS" if ok else "FAIL"))
    return ok, lines, {r[0]: r[1] for r in res}


def _log(arms, done=True, extra=()):
    out = ["[b5s8] seed 0 lr 0.001 build plain"]
    for name, kept in arms.items():
        out.append("[b5s8] arm=%s last=%.4f kept=%.4f at 9 (gate 2.5000) peek=%.4f at 8 lastgate=2.6 took=1/8 app=1"
                   % (name, kept + 0.5, kept, kept))
    out += list(extra)
    if done:
        out.append("[b5s8] done")
    return "\n".join(out) + "\n"


def self_test():
    bad = 0

    def check(name, got, want):
        nonlocal bad
        print("[b5s8-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want

    check("constants D1 0.08 D2 0.05 D34 0.03 D5 0.02 D6 0.02 D7 0.05 D8 0.03 D9 0.02 NEED 7 seeds 141-148",
          (D1, D2, D34, D5, D6, D7, D8, D9, NEED, SEEDS),
          (0.08, 0.05, 0.03, 0.02, 0.02, 0.05, 0.03, 0.02, 7, "141 142 143 144 145 146 147 148"))
    S = SEEDS.split()

    def P(alone=3.95, trim=3.62, trust=3.59, tg1=3.66, sg1=3.65, sg2=3.75, sf2=3.82, sn2=3.73,
          y1=3.90, y2=3.92, tn1=3.76, sn1=3.64, ad2=3.87):
        return {"alone-A": alone, "trim4": trim, "trust4": trust, "trim4-garb1": tg1, "trust4-garb1": sg1,
                "trust4-garb2": sg2, "trust4-flip2": sf2, "trust4-near2": sn2,
                "trust4-yori1": y1, "trust4-yori2": y2, "trim4-near1": tn1, "trust4-near1": sn1,
                "trust4-ada2": ad2}
    good = {s: P() for s in S}

    def NB(sg2=3.75, sf2=3.82, sn2=3.87):
        return {"alone-A": 3.95, "trust4-garb2": sg2, "trust4-flip2": sf2, "trust4-near2": sn2}

    def TA(trim=3.62, trust=3.75):
        return {"alone-A": 3.95, "trim4": trim, "trust4": trust, "trim4-garb1": 3.66, "trust4-garb1": 3.65}
    NEr = {s: NB() for s in S}                       # NC1 red (R7 red: near2 +0.12)
    NEg = {s: NB(sn2=3.79) for s in S}               # NC1 green
    TLr = {s: TA() for s in S}                       # NC2 red (R4 red: +0.13)
    TLg = {s: TA(trust=3.64) for s in S}             # NC2 green

    def one(d, s, v):
        x = dict(d)
        x[s] = v
        return x

    def two(d, a, b, v):
        return one(one(d, a, v), b, v)
    allg = {g: True for g in ("R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "NC1", "NC2")}
    cases = [
        ("green", (good, NEr, TLr), True, allg),
        ("R1 two short", (two(good, "141", "142", P(sg2=3.90, sn2=3.90)), NEr, TLr), False, {"R1": False, "R7": True}),
        ("R2 two short", (two(good, "143", "144", P(sf2=3.91)), NEr, TLr), False, {"R2": False}),
        ("R3 two short", (two(good, "143", "144", P(sg1=3.70)), NEr, TLr), False, {"R3": False, "R4": True}),
        ("R4 two short", (two(good, "145", "146", P(trust=3.66)), NEr, TLr), False, {"R4": False, "R3": True}),
        ("R5 near2 one seed", (one(good, "147", P(sn2=3.975, sg2=3.94)), NEr, TLr), False, {"R5": False, "R7": True}),
        ("R6 yori1 one seed", (one(good, "147", P(y1=3.975)), NEr, TLr), False, {"R6": False, "R5": True}),
        ("R7 one short ok", (one(good, "141", P(sn2=3.81)), NEr, TLr), True, {"R7": True}),
        ("R7 two short", (two(good, "141", "142", P(sn2=3.81)), NEr, TLr), False, {"R7": False, "R5": True}),
        ("R7 at the line", ({s: P(sn2=3.80) for s in S}, NEr, TLr), True, {"R7": True}),
        ("R7 is near2 - garb2, not - alone", ({s: P(sg2=3.70, sn2=3.80) for s in S}, NEr, TLr), False, {"R7": False}),
        ("R8 two short", (two(good, "143", "148", P(sn1=3.80)), NEr, TLr), False, {"R8": False, "R7": True}),
        ("R8 at the line", ({s: P(sn1=3.79) for s in S}, NEr, TLr), True, {"R8": True}),
        ("R9 one seed", (one(good, "148", P(ad2=3.975)), NEr, TLr), False, {"R9": False, "R6": True}),
        ("R9 at the line", ({s: P(ad2=3.97) for s in S}, NEr, TLr), True, {"R9": True}),
        ("NC1 green", (good, NEg, TLr), False, {"NC1": False, "NC2": True}),
        ("NC1 missing", (good, one(NEr, "141", None), TLr), False, {"NC1": False}),
        ("NC2 green", (good, NEr, TLg), False, {"NC2": False, "NC1": True}),
        ("NC2 missing", (good, NEr, one(TLr, "141", None)), False, {"NC2": False}),
        ("NC2 red on two seeds is enough (R4 6/8)", (good, NEr, {s: (TLr[s] if s in ("141", "142") else TLg[s]) for s in S}),
         True, {"NC2": True}),
        ("NC2 red on one seed is not (R4 7/8)", (good, NEr, {s: (TLr[s] if s == "141" else TLg[s]) for s in S}),
         False, {"NC2": False}),
        ("missing seed", (one(good, "148", None), NEr, TLr), False,
         {"R1": False, "R4": False, "R7": False, "R9": False}),
    ]
    for name, (p, g, c), want, gw in cases:
        got, _, r = judge(p, g, c)
        check(name, got, want)
        for gid, w in gw.items():
            check("%s: %s" % (name, gid), r[gid], w)
    for k in (1, 7):
        sub = S[:k]
        got, ls, r = judge({s: good[s] for s in sub}, {s: NEr[s] for s in sub}, {s: TLr[s] for s in sub})
        check("H13 %d seed(s): not PASS" % k, got, False)
        check("H13 %d seed(s): result PARTIAL" % k, ls[-1].startswith("[result] PARTIAL"), True)
    sw = S[:7] + ["149"]
    src = S[:7] + [S[7]]
    got, ls, r = judge({a: good[b] for a, b in zip(sw, src)}, {a: NEr[b] for a, b in zip(sw, src)},
                       {a: TLr[b] for a, b in zip(sw, src)})
    check("H13 8 seeds, one swapped: not PASS", got, False)
    check("H13 8 seeds, one swapped: result PARTIAL", ls[-1].startswith("[result] PARTIAL"), True)

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
        dup = _log(pa, extra=["[b5s8] arm=trim4 last=9.0 kept=3.0000 at 1 (gate 2.0)"])
        check("read: an arm twice -> None", read(put("dup.log", dup), PARTS["a"]), None)
        check("read: a step 7 log -> None", read(put("s7.log", _log(pa).replace("[b5s8]", "[b5s7]")), PARTS["a"]), None)

        def write_all(ne_near2=3.87, tl_trust=3.75):
            for s in S:
                for p, names in PARTS.items():
                    put("plain-%s-%s.log" % (s, p), _log({k: arms[k] for k in names}))
                nb = {k: arms[k] for k in PARTS["b"]}
                nb["trust4-near2"] = ne_near2
                put("noecho-%s.log" % s, _log(nb))
                ta = {k: arms[k] for k in PARTS["a"]}
                ta["trust4"] = tl_trust
                put("taulow-%s.log" % s, _log(ta))

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
        check("main: --seeds 148 only -> rc 1 (PARTIAL)", run_main(["--seeds", "148"]), 1)
        check("read_plain merges the five parts", read_plain(d, "141"), arms)
        os.remove(os.path.join(d, "plain-142-d.log"))
        check("main: a plain part missing -> rc 1", run_main(), 1)
        write_all(ne_near2=3.79)
        check("main: NC1 all green -> rc 1", run_main(), 1)
        write_all(tl_trust=3.64)
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
    noecho = {s: read_nc(d, "noecho", s, "b", plain[s]) for s in sl}
    taulow = {s: read_nc(d, "taulow", s, "a", plain[s]) for s in sl}
    ok, lines, _ = judge(plain, noecho, taulow)
    print("\n".join(lines))
    for s in sl:
        a = plain[s]
        if a is not None:
            print("[b5s8] reading seed %s: near2 - garb2 %+.4f, near1 - trim-near1 %+.4f, ada2 - alone %+.4f"
                  % (s, a["trust4-near2"] - a["trust4-garb2"], a["trust4-near1"] - a["trim4-near1"],
                     a["trust4-ada2"] - a["alone-A"]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
