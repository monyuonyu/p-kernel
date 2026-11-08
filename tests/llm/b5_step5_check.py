"""b5_step5_check.py - the judge of B5 step 5 (research/b5-step5-design.md).

Reads <build>-<seed>.log written by run_b5_step5.sh (student_adopt_test.c,
stage s5). Each arm prints one line

  [b5s5] arm=<name> last=<bpb> kept=<bpb> at <pass> (gate <nats>) ... took=<n>/<m> app=<k>

kept is node A's model at its best pass on the gate window (step 4's stop);
the ruler (b1_held.txt) never chooses. Four nodes, each a quarter; poison = 1
or 2 nodes send garbage (another init x8). Lines fixed in the design (section
4) before the code and before any run on the judgement seeds 115..122:

  T1  trim4-poison <= mean4-poison - 0.10                  on >= 7 seeds
  T2  trim4-poison <= min(alone-A, shrink-app) - 0.10      on >= 7 seeds
  T3  trim4 <= mean4 + 0.03                                on >= 7 seeds
  T4  trim4-poison2 <= alone-A + 0.02                      on every seed
  NC1 the build -DB5S5_TRIM_OFF (every value averaged) turns T1 red
  NC2 the build -DB5S5_NO_GATE (every candidate replaces live) turns T4 red

  b5_step5_check.py [--seeds "115 ... 122"] <dir>   exit 0 = T1-T4 green, NC1 NC2 red
  b5_step5_check.py --self-test
"""
import os
import re
import shutil
import sys
import tempfile

D12 = 0.10     # T1, T2: fixed in the design from the seen seeds 91 92, before the code
D3 = 0.03      # T3
D4 = 0.02      # T4
NEED = 7
SEEDS = " ".join(str(s) for s in range(115, 123))
ARM = re.compile(r"^\[b5s5\] arm=(\S+) last=([0-9.]+) kept=([0-9.]+) at (\d+) \(gate ([0-9.]+)\)")
NEEDED = {
    "plain": ["alone-A", "full", "shrink-app", "mean4", "trim4", "mean4-poison", "trim4-poison", "trim4-poison2"],
    "trimoff": ["mean4-poison", "trim4-poison"],
    "nogate": ["alone-A", "trim4-poison2"],
}


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
        elif line == "[b5s5] done":
            done = True
    if not done or any(a not in arms for a in needed):
        return None
    return arms


def count(seeds, test, label, lines, need):
    """test(arms) -> (ok, text); a missing seed is a fail; -> ok on `need` seeds and none missing"""
    ok, n = 0, 0
    for s in sorted(seeds, key=int):
        a = seeds[s]
        if a is None:
            lines.append("[b5s5] %s seed %s: log missing or short" % (label, s))
            continue
        n += 1
        g, text = test(a)
        ok += g
        lines.append("[b5s5] %s seed %s: %s -> %s" % (label, s, text, "ok" if g else "RED"))
    return n == len(seeds) and ok >= need


def t1(a):
    d = a["trim4-poison"] - a["mean4-poison"]
    return d <= -D12 + 1e-9, "trim-poison %.4f mean-poison %.4f (%+.4f)" % (a["trim4-poison"], a["mean4-poison"], d)


def t2(a):
    lo = min(a["alone-A"], a["shrink-app"])
    d = a["trim4-poison"] - lo
    return d <= -D12 + 1e-9, "trim-poison %.4f min(alone-A, shrink) %.4f (%+.4f)" % (a["trim4-poison"], lo, d)


def t3(a):
    d = a["trim4"] - a["mean4"]
    return d <= D3 + 1e-9, "trim %.4f mean %.4f (%+.4f)" % (a["trim4"], a["mean4"], d)


def t4(a):
    d = a["trim4-poison2"] - a["alone-A"]
    return d <= D4 + 1e-9, "trim-poison2 %.4f alone-A %.4f (%+.4f)" % (a["trim4-poison2"], a["alone-A"], d)


def judge(plain, trimoff, nogate):
    """-> (ok, lines, {gate id: bool})"""
    lines = []
    need = lambda d: min(NEED, len(d))
    g1 = count(plain, t1, "T1", lines, need(plain))
    g2 = count(plain, t2, "T2", lines, need(plain))
    g3 = count(plain, t3, "T3", lines, need(plain))
    g4 = count(plain, t4, "T4", lines, len(plain))
    n1 = count(trimoff, t1, "NC1 (B5S5_TRIM_OFF) T1", lines, need(trimoff))
    n2 = count(nogate, t4, "NC2 (B5S5_NO_GATE) T4", lines, len(nogate))
    nc1 = not n1 and all(v is not None for v in trimoff.values())
    nc2 = not n2 and all(v is not None for v in nogate.values())
    res = [("T1", g1, "trim4-poison <= mean4-poison - %.2f on %d+ seeds" % (D12, NEED)),
           ("T2", g2, "trim4-poison <= min(alone-A, shrink-app) - %.2f on %d+ seeds" % (D12, NEED)),
           ("T3", g3, "trim4 <= mean4 + %.2f on %d+ seeds" % (D3, NEED)),
           ("T4", g4, "every seed: trim4-poison2 <= alone-A + %.2f" % D4),
           ("NC1", nc1, "-DB5S5_TRIM_OFF turns T1 red"),
           ("NC2", nc2, "-DB5S5_NO_GATE turns T4 red")]
    for gid, ok, what in res:
        lines.append("[b5s5] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    ok = all(r[1] for r in res)
    # test-holes H14 (as H13 in step 7): a run on other than the 8 judgement
    # seeds is a reading: PARTIAL, never PASS (the gate lines stay).
    if sorted(plain, key=int) != sorted(SEEDS.split(), key=int):
        lines.append("[result] PARTIAL b5-step5 (%d of the %d judgement seeds: a reading, not the judgement)"
                     % (len([s for s in plain if s in SEEDS.split()]), len(SEEDS.split())))
        return False, lines, {r[0]: r[1] for r in res}
    lines.append("[result] %s b5-step5" % ("PASS" if ok else "FAIL"))
    return ok, lines, {r[0]: r[1] for r in res}


def _log(arms, done=True, extra=()):
    out = ["[b5s5] seed 0 lr 0.001 build plain"]
    for name, kept in arms.items():
        out.append("[b5s5] arm=%s last=%.4f kept=%.4f at 9 (gate 2.5000) peek=%.4f at 8 lastgate=2.6 took=1/8 app=1"
                   % (name, kept + 0.5, kept, kept))
    out += list(extra)
    if done:
        out.append("[b5s5] done")
    return "\n".join(out) + "\n"


def self_test():
    bad = 0

    def check(name, got, want):
        nonlocal bad
        print("[b5s5-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want

    check("constants D12 0.10 D3 0.03 D4 0.02 NEED 7 seeds 115-122",
          (D12, D3, D4, NEED, SEEDS), (0.10, 0.03, 0.02, 7, "115 116 117 118 119 120 121 122"))
    S = SEEDS.split()

    def P(tp=3.70, mp=3.95, alone=3.95, shrink=3.96, trim=3.62, mean=3.63, tp2=3.95):
        return {"alone-A": alone, "full": 3.58, "shrink-app": shrink, "mean4": mean, "trim4": trim,
                "mean4-poison": mp, "trim4-poison": tp, "trim4-poison2": tp2}
    good = {s: P() for s in S}
    toff = {s: P(tp=3.95) for s in S}            # trim off: trim-poison = mean-poison, T1 red
    nog = {s: P(tp2=4.60) for s in S}            # no gate: poison2 goes in, T4 red

    def one(d, s, v):
        x = dict(d)
        x[s] = v
        return x

    def two(d, a, b, v):
        return one(one(d, a, v), b, v)
    allg = {"T1": True, "T2": True, "T3": True, "T4": True, "NC1": True, "NC2": True}
    cases = [
        ("green", (good, toff, nog), True, allg),
        ("T1 one short ok", (one(good, "115", P(tp=3.90, alone=4.1, shrink=4.1)), toff, nog), True, {"T1": True}),
        ("T1 two short", (two(good, "115", "116", P(tp=3.90, alone=4.1, shrink=4.1)), toff, nog), False,
         {"T1": False, "T2": True}),
        ("T1 at the line", ({s: P(tp=3.85) for s in S}, toff, nog), True, {"T1": True}),
        ("T1 0.005 over, two seeds", (two(good, "115", "116", P(tp=3.855, alone=4.1, shrink=4.1)), toff, nog),
         False, {"T1": False, "T2": True}),
        # T2 uses the lower of alone-A and shrink: max would pass it
        ("T2 shrink is the lower", (two(good, "117", "118", P(tp=3.70, alone=4.2, shrink=3.75)), toff, nog),
         False, {"T2": False, "T1": True}),
        ("T2 alone is the lower", (two(good, "117", "118", P(tp=3.70, alone=3.75, shrink=4.2, tp2=3.75)), toff, nog),
         False, {"T2": False, "T4": True}),
        ("T2 at the line", ({s: P(tp=3.85, mp=4.2) for s in S}, toff, nog), True, {"T2": True, "T1": True}),
        ("T3 one short ok", (one(good, "119", P(trim=3.70)), toff, nog), True, {"T3": True}),
        ("T3 two short", (two(good, "119", "120", P(trim=3.70)), toff, nog), False, {"T3": False}),
        ("T3 at the line", ({s: P(trim=3.66) for s in S}, toff, nog), True, {"T3": True}),
        ("T3 0.005 over, two seeds", (two(good, "119", "120", P(trim=3.665)), toff, nog), False, {"T3": False}),
        ("T4 one seed red", (one(good, "121", P(tp2=3.98)), toff, nog), False, {"T4": False, "T1": True}),
        ("T4 at the line", ({s: P(tp2=3.97) for s in S}, toff, nog), True, {"T4": True}),
        ("T4 0.005 over", (one(good, "121", P(tp2=3.975)), toff, nog), False, {"T4": False}),
        ("NC1 green", (good, good, nog), False, {"NC1": False, "NC2": True}),
        ("NC1 missing", (good, one(toff, "115", None), nog), False, {"NC1": False}),
        ("NC2 green", (good, toff, good), False, {"NC2": False, "NC1": True}),
        ("NC2 missing", (good, toff, one(nog, "122", None)), False, {"NC2": False}),
        ("missing seed", (one(good, "122", None), toff, nog), False,
         {"T1": False, "T2": False, "T3": False, "T4": False}),
    ]
    for name, (p, t, g), want, gw in cases:
        got, _, r = judge(p, t, g)
        check(name, got, want)
        for gid, w in gw.items():
            check("%s: %s" % (name, gid), r[gid], w)
    # H14: a good run on 1 or 7 seeds, or on 8 with one not a judgement seed, is PARTIAL
    for name, sub in (("1 seed", S[:1]), ("7 seeds", S[:7]), ("8 seeds, one swapped", S[:7] + ["999"])):
        got, ls, r = judge({s: P() for s in sub}, {s: P(tp=3.95) for s in sub}, {s: P(tp2=4.60) for s in sub})
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
        check("read: kept, not last", read(put("k.log", _log(arms)), NEEDED["plain"]), arms)
        check("read: no done -> None", read(put("nd.log", _log(arms, done=False)), NEEDED["plain"]), None)
        dup = _log(arms, extra=["[b5s5] arm=trim4 last=9.0 kept=3.0000 at 1 (gate 2.0)"])
        check("read: an arm twice -> None", read(put("dup.log", dup), NEEDED["plain"]), None)
        short = dict(arms)
        del short["trim4-poison2"]
        check("read: an arm missing -> None", read(put("sh.log", _log(short)), NEEDED["plain"]), None)
        check("read: no file -> None", read(os.path.join(d, "nofile.log"), NEEDED["plain"]), None)
        s4 = _log(arms).replace("[b5s5]", "[b5s4]")
        check("read: a step 4 log -> None", read(put("s4.log", s4), NEEDED["plain"]), None)
        t1arms = {"mean4-poison": 3.95, "trim4-poison": 3.95}
        check("read: trimoff needs only its two arms", read(put("t.log", _log(t1arms)), NEEDED["trimoff"]), t1arms)

        for s in S:
            put("plain-%s.log" % s, _log(P()))
            put("trimoff-%s.log" % s, _log({"mean4-poison": 3.95, "trim4-poison": 3.94}))
            put("nogate-%s.log" % s, _log({"alone-A": 3.95, "trim4-poison2": 4.6}))

        def run_main():
            null = open(os.devnull, "w")
            old, sys.stdout = sys.stdout, null
            try:
                return main(["x", d])
            finally:
                sys.stdout = old
                null.close()
        check("main: directory PASS", run_main(), 0)
        put("nogate-118.log", _log({"alone-A": 3.95, "trim4-poison2": 3.95}))   # NC2 still red on the other 7
        check("main: one nogate seed green, NC2 still red, PASS", run_main(), 0)
        for s in S:
            put("nogate-%s.log" % s, _log({"alone-A": 3.95, "trim4-poison2": 3.95}))
        check("main: nogate all green, NC2 fails, rc 1", run_main(), 1)
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
    get = lambda b: {s: read(os.path.join(d, "%s-%s.log" % (b, s)), NEEDED[b]) for s in sl}
    ok, lines, _ = judge(get("plain"), get("trimoff"), get("nogate"))
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
