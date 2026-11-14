"""b5_step10_check.py - the judge of B5 step 10 (research/b5-step10-design.md).

Reads the logs written by run_b5_step10.sh (student_adopt_test.c). The plain
build's stages s10a s10b s10y s10c s10d s10p s10r1 s10r2 are step 9's arms
with the trust arms on st_merge_trust_group; s9a and s9c of the same build are
step 9 (st_merge_trust_echo, every byte as before) on the same seeds (s9t =
s9a's trust4 and trust4-garb1, s9u = s9c's trust4-near1); s10n of the NC1
build (-DB5S10_GROUP_FAR) is near2 rep 0; s10t of the NC2 build
(-DB5S10_TG_LOW) gives trust4 and trust4-garb1. Each arm prints one line

  [b5s10] arm=<name> last=<bpb> kept=<bpb> at <pass> (gate <nats>) ...

([b5s9] for the step 9 stages). Lines fixed in the design (section 3) before
the code and before any run on the seeds 157..164:

  R1-R6 R8 R9  step 8's lines (b5_step8_check.py), on the step 10 arms (rep 0)
  R7'  median(near2 rep 0, 1, 2) <= pair2 + 0.03                on >= 7 seeds
  R10  |kept - step 9's kept| <= 0.005 for trust4 and trust4-garb1, every seed
  NC1  -DB5S10_GROUP_FAR turns R7' red (its near2 rep 0, pair2 of the plain log)
  NC2  -DB5S10_TG_LOW turns R10 red (its trust4 and trust4-garb1 against step 9)
Readings (not lines): near1 step 10 - step 9 (trust4-near1), receiver 1's kept
poisons (r1keptP), garb2 - pair2, the near2 range.

  b5_step10_check.py [--seeds "157 ... 164"] <dir>   exit 0 = R1-R10 green, NC1 and NC2 red
  b5_step10_check.py --self-test
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
D10 = 0.005
NEED = 7
SEEDS = " ".join(str(s) for s in range(157, 165))
ARM = re.compile(r"^\[(b5s10|b5s9)\] arm=(\S+) last=([0-9.]+) kept=([0-9.]+) at (\d+) \(gate ([0-9.]+)\)(.*)$")
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
R10_ARMS = ["trust4", "trust4-garb1"]
ECHO_PARTS = {"t": ["trust4", "trust4-garb1"], "u": ["trust4-near1"]}


def read(path, needed, tag="b5s10", extra=None):
    """{arm: kept} of a finished log of tag, or None (missing, short, an arm twice, another tag).
    extra (a dict) gets {arm: r1keptP} where the line has it."""
    if not os.path.exists(path):
        return None
    arms, done = {}, False
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        m = ARM.match(line)
        if m:
            if m.group(1) != tag or m.group(2) in arms:
                return None
            arms[m.group(2)] = float(m.group(4))
            k = re.search(r"r1keptP=(\d+)", m.group(7))
            if k and extra is not None:
                extra[m.group(2)] = int(k.group(1))
        elif line == "[%s] done" % tag:
            done = True
    if not done or any(a not in arms for a in needed):
        return None
    return arms


def read_parts(d, s, extra=None):
    """{arm: kept} of every finished plain part of seed s (possibly empty)"""
    out = {}
    for p, needed in PARTS.items():
        a = read(os.path.join(d, "plain-%s-%s.log" % (s, p)), needed, extra=extra)
        if a is not None:
            out.update(a)
    return out


def read_plain(d, s):
    a = read_parts(d, s)
    return a if all(k in a for ks in PARTS.values() for k in ks) else None


def read_echo(d, s):
    """step 9's arms of the same seed (s9a s9c of the plain build), or None"""
    out = {}
    for p, needed in ECHO_PARTS.items():
        a = read(os.path.join(d, "echo-%s-%s.log" % (s, p)), needed, tag="b5s9")
        if a is None:
            return None
        out.update(a)
    return out


def read_nc1(d, s, plain):
    """NC1's near2 with pair2 from the plain arms; None if either is missing"""
    a = read(os.path.join(d, "far-%s.log" % s), ["trust4-near2"])
    if a is None or plain is None or "trust4-pair2" not in plain:
        return None
    return {"trust4-near2": a["trust4-near2"], "trust4-pair2": plain["trust4-pair2"]}


def read_nc2(d, s, echo):
    """NC2's trust4 and trust4-garb1 with step 9's from the echo logs; None if either is missing"""
    a = read(os.path.join(d, "tglow-%s.log" % s), R10_ARMS)
    if a is None or echo is None:
        return None
    return {"new": {k: a[k] for k in R10_ARMS}, "echo": {k: echo[k] for k in R10_ARMS}}


def median(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2.0


def count(seeds, test, label, lines, need):
    ok, n = 0, 0
    for s in sorted(seeds, key=int):
        a = seeds[s]
        if a is None:
            lines.append("[b5s10] %s seed %s: log missing or short" % (label, s))
            continue
        n += 1
        g, text = test(a)
        ok += g
        lines.append("[b5s10] %s seed %s: %s -> %s" % (label, s, text, "ok" if g else "RED"))
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


def r10(x):
    """x = {"new": {arm: kept}, "echo": {arm: kept}}"""
    ds = [x["new"][k] - x["echo"][k] for k in R10_ARMS]
    ok = all(abs(d) <= D10 + 1e-9 for d in ds)
    return ok, "trust4 %+.4f trust4-garb1 %+.4f against step 9" % (ds[0], ds[1])


def judge(plain, r10in, nc1, nc2):
    """plain {seed: arms}, r10in {seed: {new, echo}}, nc1 {seed: arms}, nc2 {seed: {new, echo}}
    -> (ok, lines, {gate id: bool})"""
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
    g10 = count(r10in, r10, "R10", lines, len(r10in))
    n17 = count(nc1, r7p, "NC1 (B5S10_GROUP_FAR) R7'", lines, min(NEED, len(nc1)))
    n210 = count(nc2, r10, "NC2 (B5S10_TG_LOW) R10", lines, len(nc2))
    c1 = not n17 and len(nc1) > 0 and all(v is not None for v in nc1.values())
    c2 = not n210 and len(nc2) > 0 and all(v is not None for v in nc2.values())
    res = [("R1", g1, "trust4-garb2 <= alone-A - %.2f on %d+ seeds" % (D1, NEED)),
           ("R2", g2, "trust4-flip2 <= alone-A - %.2f on %d+ seeds" % (D2, NEED)),
           ("R3", g3, "trust4-garb1 <= trim4-garb1 + %.2f on %d+ seeds" % (D34, NEED)),
           ("R4", g4, "trust4 <= trim4 + %.2f on %d+ seeds" % (D34, NEED)),
           ("R5", g5, "every seed: trust4 garb2/flip2/near2 <= alone-A + %.2f" % D5),
           ("R6", g6, "every seed: trust4 yori1/yori2 <= alone-A + %.2f" % D6),
           ("R7'", g7, "median(near2 rep 0 1 2) <= trust4-pair2 + %.2f on %d+ seeds" % (D7P, NEED)),
           ("R8", g8, "trust4-near1 <= trim4-near1 + %.2f on %d+ seeds" % (D8, NEED)),
           ("R9", g9, "every seed: trust4-ada2 <= alone-A + %.2f" % D9),
           ("R10", g10, "every seed: trust4 and trust4-garb1 within %.3f of step 9" % D10),
           ("NC1", c1, "-DB5S10_GROUP_FAR turns R7' red"),
           ("NC2", c2, "-DB5S10_TG_LOW turns R10 red")]
    for gid, ok, what in res:
        lines.append("[b5s10] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    ok = all(r[1] for r in res)
    # H13 (audit-158): only the 8 fixed seeds are the judgement; a seed whose logs are
    # missing or short is not judged either (still running, or died)
    want = sorted(SEEDS.split(), key=int)
    full = all(sorted(m, key=int) == want and all(v is not None for v in m.values())
               for m in (plain, r10in))
    if not full:
        lines.append("[result] PARTIAL b5-step10 (%d of the %d judgement seeds finished: a reading, not the judgement)"
                     % (len([s for s in plain if s in want and plain[s] is not None and r10in.get(s) is not None]),
                        len(want)))
        return False, lines, {r[0]: r[1] for r in res}
    lines.append("[result] %s b5-step10" % ("PASS" if ok else "FAIL"))
    return ok, lines, {r[0]: r[1] for r in res}


def readings(d, sl):
    """per seed, whatever is finished: R7' so far, near1 step 10 - step 9, receiver 1's kept poisons"""
    out = []
    for s in sl:
        kp = {}
        a = read_parts(d, s, extra=kp)
        e = read_echo(d, s)
        bits = []
        nv = [a[k] for k in NEARS if k in a]
        gv = [a[k] for k in GARBS if k in a]
        if "trust4-pair2" in a and nv:
            bits.append("near2 median(%d) - pair2 %+.4f" % (len(nv), median(nv) - a["trust4-pair2"]))
        if "trust4-pair2" in a and gv:
            dv = [g - a["trust4-pair2"] for g in gv]
            bits.append("garb2 - pair2 median(%d) %+.4f" % (len(dv), median(dv)))
        if len(nv) > 1:
            bits.append("near2 range %.4f" % (max(nv) - min(nv)))
        if e is not None and "trust4-near1" in a:
            bits.append("near1 step 10 - step 9 %+.4f" % (a["trust4-near1"] - e["trust4-near1"]))
        kps = ["%s:%d" % (k.replace("trust4-", ""), v) for k, v in sorted(kp.items()) if k in NEARS + ["trust4-near1", "trust4-ada2", "trust4-flip2"]]
        if kps:
            bits.append("r1 kept poisons " + " ".join(kps))
        out.append("[b5s10] reading seed %s: %s" % (s, "; ".join(bits) if bits else "nothing finished"))
    return out


def _log(arms, done=True, extra=(), tag="b5s10"):
    out = ["[%s] seed 0 lr 0.001 build plain" % tag]
    for name, kept in arms.items():
        out.append("[%s] arm=%s last=%.4f kept=%.4f at 9 (gate 2.5000) peek=%.4f at 8 lastgate=2.6 took=1/8 app=1 r1keptP=2"
                   % (tag, name, kept + 0.5, kept, kept))
    out += list(extra)
    if done:
        out.append("[%s] done" % tag)
    return "\n".join(out) + "\n"


def self_test():
    bad = 0

    def check(name, got, want):
        nonlocal bad
        print("[b5s10-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want

    check("constants D1 0.08 D2 0.05 D34 0.03 D5 0.02 D6 0.02 D7P 0.03 D8 0.03 D9 0.02 D10 0.005 NEED 7 seeds 157-164",
          (D1, D2, D34, D5, D6, D7P, D8, D9, D10, NEED, SEEDS),
          (0.08, 0.05, 0.03, 0.02, 0.02, 0.03, 0.03, 0.02, 0.005, 7, "157 158 159 160 161 162 163 164"))
    S = SEEDS.split()

    def P(alone=3.95, trim=3.62, trust=3.59, tg1=3.66, sg1=3.65, sg2=3.75, sf2=3.82, sn2=3.73,
          y1=3.90, y2=3.92, tn1=3.76, sn1=3.64, ad2=3.87, pr=3.72, n1=3.74, n2=3.72, g1=3.76, g2=3.74):
        return {"alone-A": alone, "trim4": trim, "trust4": trust, "trim4-garb1": tg1, "trust4-garb1": sg1,
                "trust4-garb2": sg2, "trust4-flip2": sf2, "trust4-near2": sn2,
                "trust4-yori1": y1, "trust4-yori2": y2, "trim4-near1": tn1, "trust4-near1": sn1,
                "trust4-ada2": ad2, "trust4-pair2": pr, "trust4-near2-r1": n1, "trust4-near2-r2": n2,
                "trust4-garb2-r1": g1, "trust4-garb2-r2": g2}
    good = {s: P() for s in S}

    def X(t=3.59, g=3.65, et=3.59, eg=3.65):
        return {"new": {"trust4": t, "trust4-garb1": g}, "echo": {"trust4": et, "trust4-garb1": eg}}
    R10g = {s: X() for s in S}

    def NB(sn2=3.87, pr=3.72):
        return {"trust4-near2": sn2, "trust4-pair2": pr}
    N1r = {s: NB() for s in S}                       # NC1 red (+0.15)
    N1g = {s: NB(sn2=3.74) for s in S}               # NC1 green (+0.02)
    N2r = {s: X(t=3.70) for s in S}                  # NC2 red (+0.11)
    N2g = {s: X(t=3.592) for s in S}                 # NC2 green (+0.002)

    def one(d, s, v):
        x = dict(d)
        x[s] = v
        return x

    def two(d, a, b, v):
        return one(one(d, a, v), b, v)
    allg = {g: True for g in ("R1", "R2", "R3", "R4", "R5", "R6", "R7'", "R8", "R9", "R10", "NC1", "NC2")}
    cases = [
        ("green", (good, R10g, N1r, N2r), True, allg),
        ("R1 two short", (two(good, S[0], S[1], P(sg2=3.90)), R10g, N1r, N2r), False, {"R1": False, "R7'": True}),
        ("R2 two short", (two(good, S[2], S[3], P(sf2=3.91)), R10g, N1r, N2r), False, {"R2": False}),
        ("R3 two short", (two(good, S[2], S[3], P(sg1=3.70)), R10g, N1r, N2r), False, {"R3": False, "R4": True}),
        ("R4 two short", (two(good, S[4], S[5], P(trust=3.66)), R10g, N1r, N2r), False, {"R4": False, "R3": True}),
        ("R5 near2 one seed", (one(good, S[6], P(sn2=3.975, n1=3.70, n2=3.70)), R10g, N1r, N2r), False,
         {"R5": False, "R7'": True}),
        ("R6 yori1 one seed", (one(good, S[6], P(y1=3.975)), R10g, N1r, N2r), False, {"R6": False, "R5": True}),
        ("R7' one short ok", (one(good, S[0], P(sn2=3.80, n1=3.80)), R10g, N1r, N2r), True, {"R7'": True}),
        ("R7' two short", (two(good, S[0], S[1], P(sn2=3.80, n1=3.80)), R10g, N1r, N2r), False, {"R7'": False}),
        ("R7' at the line", ({s: P(sn2=3.75, n1=3.75, n2=3.70) for s in S}, R10g, N1r, N2r), True, {"R7'": True}),
        ("R7' just over the line", ({s: P(sn2=3.7505, n1=3.7505, n2=3.70) for s in S}, R10g, N1r, N2r), False,
         {"R7'": False}),
        ("R7' is the median: two bad reps are red", ({s: P(n1=3.80, n2=3.80) for s in S}, R10g, N1r, N2r), False,
         {"R7'": False}),
        ("R8 two short", (two(good, S[2], S[7], P(sn1=3.80)), R10g, N1r, N2r), False, {"R8": False}),
        ("R9 one seed", (one(good, S[7], P(ad2=3.975)), R10g, N1r, N2r), False, {"R9": False}),
        ("R10 one seed trust4 +0.006", (good, one(R10g, S[3], X(t=3.596)), N1r, N2r), False, {"R10": False}),
        ("R10 one seed garb1 -0.006", (good, one(R10g, S[3], X(g=3.644)), N1r, N2r), False, {"R10": False}),
        ("R10 at the line (+0.005)", (good, {s: X(t=3.595, g=3.645) for s in S}, N1r, N2r), True, {"R10": True}),
        ("R10 missing echo seed", (good, one(R10g, S[0], None), N1r, N2r), False, {"R10": False}),
        ("NC1 green", (good, R10g, N1g, N2r), False, {"NC1": False, "NC2": True}),
        ("NC1 missing", (good, R10g, one(N1r, S[0], None), N2r), False, {"NC1": False}),
        ("NC1 red on two seeds is enough", (good, R10g, {s: (N1r[s] if s in S[:2] else N1g[s]) for s in S}, N2r),
         True, {"NC1": True}),
        ("NC1 red on one seed is not", (good, R10g, {s: (N1r[s] if s == S[0] else N1g[s]) for s in S}, N2r),
         False, {"NC1": False}),
        ("NC2 green", (good, R10g, N1r, N2g), False, {"NC2": False, "NC1": True}),
        ("NC2 red on one seed is enough (R10 wants every seed)",
         (good, R10g, N1r, {s: (N2r[s] if s == S[0] else N2g[s]) for s in S}), True, {"NC2": True}),
        ("NC2 missing", (good, R10g, N1r, one(N2r, S[0], None)), False, {"NC2": False}),
        ("NC empty", (good, R10g, {}, {}), False, {"NC1": False, "NC2": False}),
        ("missing seed", (one(good, S[7], None), R10g, N1r, N2r), False, {"R1": False, "R9": False}),
    ]
    got, ls, r = judge(one(good, S[7], None), R10g, N1r, N2r)
    check("a seed's logs missing: result PARTIAL, not FAIL", ls[-1].startswith("[result] PARTIAL"), True)
    got, ls, r = judge(good, one(R10g, S[7], None), N1r, N2r)
    check("a seed's step 9 logs missing: result PARTIAL", ls[-1].startswith("[result] PARTIAL"), True)
    for name, args, want, gw in cases:
        got, _, r = judge(*args)
        check(name, got, want)
        for gid, w in gw.items():
            check("%s: %s" % (name, gid), r[gid], w)
    for k in (1, 7):
        sub = S[:k]
        got, ls, r = judge({s: good[s] for s in sub}, {s: R10g[s] for s in sub}, {s: N1r[s] for s in sub},
                           {s: N2r[s] for s in sub})
        check("H13 %d seed(s): not PASS" % k, got, False)
        check("H13 %d seed(s): result PARTIAL" % k, ls[-1].startswith("[result] PARTIAL"), True)
    sw = S[:7] + ["151"]
    got, ls, r = judge({a: good[S[0]] for a in sw}, {a: R10g[S[0]] for a in sw}, {a: N1r[S[0]] for a in sw},
                       {a: N2r[S[0]] for a in sw})
    check("H13 8 seeds, one a seen seed (151): not PASS", got, False)
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
        dup = _log(pa, extra=["[b5s10] arm=trim4 last=9.0 kept=3.0000 at 1 (gate 2.0)"])
        check("read: an arm twice -> None", read(put("dup.log", dup), PARTS["a"]), None)
        check("read: a step 9 log as step 10 -> None", read(put("s9.log", _log(pa, tag="b5s9")), PARTS["a"]), None)
        check("read: a step 10 log as step 9 -> None", read(put("s10.log", _log(pa)), PARTS["a"], tag="b5s9"), None)
        ex = {}
        read(put("kp.log", _log(pa)), PARTS["a"], extra=ex)
        check("read: r1keptP", ex.get("trust4"), 2)

        def write_all(far_near2=3.87, tg_trust=3.70, echo_trust=3.59):
            for s in S:
                for p, names in PARTS.items():
                    put("plain-%s-%s.log" % (s, p), _log({k: arms[k] for k in names}))
                for p, names in ECHO_PARTS.items():
                    e = {k: arms[k] for k in names}
                    e["trust4"] = echo_trust
                    put("echo-%s-%s.log" % (s, p), _log(e, tag="b5s9"))
                put("far-%s.log" % s, _log({"trust4-near2": far_near2}))
                t = {k: arms[k] for k in R10_ARMS}
                t["trust4"] = tg_trust
                put("tglow-%s.log" % s, _log(t))

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
        check("main: --seeds 164 only -> rc 1 (PARTIAL)", run_main(["--seeds", "164"]), 1)
        check("read_plain merges the eight parts", read_plain(d, S[0]), arms)
        os.remove(os.path.join(d, "plain-%s-r2.log" % S[1]))
        check("main: a plain part missing -> rc 1", run_main(), 1)
        write_all(echo_trust=3.60)
        check("main: R10 red (step 9 trust4 0.01 away) -> rc 1", run_main(), 1)
        write_all(far_near2=3.74)
        check("main: NC1 all green -> rc 1", run_main(), 1)
        write_all(tg_trust=3.592)
        check("main: NC2 all green -> rc 1", run_main(), 1)
        os.remove(os.path.join(d, "echo-%s-u.log" % S[2]))
        write_all()
        check("main: back to PASS", run_main(), 0)
        os.remove(os.path.join(d, "echo-%s-u.log" % S[2]))
        check("main: an echo part missing -> rc 1", run_main(), 1)
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
    echo = {s: read_echo(d, s) for s in sl}
    r10in = {s: ({"new": {k: plain[s][k] for k in R10_ARMS}, "echo": {k: echo[s][k] for k in R10_ARMS}}
                 if plain[s] is not None and echo[s] is not None else None) for s in sl}
    nc1 = {s: read_nc1(d, s, plain[s]) for s in sl}
    nc2 = {s: read_nc2(d, s, echo[s]) for s in sl}
    ok, lines, _ = judge(plain, r10in, nc1, nc2)
    print("\n".join(lines))
    print("\n".join(readings(d, sl)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
