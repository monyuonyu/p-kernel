"""b5_adopt_check.py - the judge of [b5-adopt] (research/b5-step2.md).

Reads <build>-<seed>.log (student_adopt_test.c) and <build>-unit.log written by
run_b5_adopt.sh. Lines fixed in the design before the code:

  H1  every seed: adopt bpb <= the better of alone-A, alone-B - 0.5
  H2  every seed: poison-adopt bpb <= alone-A + 0.02 and took 0
  H3  adopt <= always + 0.05 in all seeds but at most one (7 of 8)
  H4  the unit cases of the gate pass (plain build)
  NC  -DST_ADOPT_ALWAYS turns H2 red; -DST_ADOPT_INVERT turns H1 red

  b5_adopt_check.py --seeds "91 ... 98" <dir>   exit 0 = H1-H4 green, both NCs red
  b5_adopt_check.py --self-test
"""
import os
import re
import sys

ARM = re.compile(r"^\[b5a\] arm=(\S+) bpb=([0-9.]+)(?: took=(\d+)/(\d+))?$")
NEED = ["alone-A", "alone-B", "full", "always", "adopt", "poison-always", "poison-adopt"]


def read(path):
    if not os.path.exists(path):
        return None
    arms, done = {}, False
    for line in open(path):
        line = line.rstrip("\n")
        m = ARM.match(line)
        if m:
            arms[m.group(1)] = (float(m.group(2)), int(m.group(3)) if m.group(3) else None)
        elif line == "[b5a] done":
            done = True
    if not done or any(a not in arms for a in NEED):
        return None
    return arms


def gates(seeds):
    """seeds: {seed: arms or None} -> (h1, h2, h3, lines)"""
    lines, h1, h2, ok3, n = [], True, True, 0, 0
    for s in sorted(seeds):
        a = seeds[s]
        if a is None:
            lines.append("[b5-adopt] seed %s: log missing or short" % s)
            h1 = h2 = False
            continue
        n += 1
        best = min(a["alone-A"][0], a["alone-B"][0])
        g1 = a["adopt"][0] <= best - 0.5
        g2 = a["poison-adopt"][0] <= a["alone-A"][0] + 0.02 and a["poison-adopt"][1] == 0
        g3 = a["adopt"][0] <= a["always"][0] + 0.05
        h1 &= g1
        h2 &= g2
        ok3 += g3
        lines.append("[b5-adopt] seed %s: alone %.4f adopt %.4f (%+.4f) always %.4f full %.4f;"
                     " poison adopt %.4f took %s, always %.4f -> H1 %s H2 %s H3 %s"
                     % (s, best, a["adopt"][0], a["adopt"][0] - best, a["always"][0], a["full"][0],
                        a["poison-adopt"][0], a["poison-adopt"][1], a["poison-always"][0],
                        "ok" if g1 else "RED", "ok" if g2 else "RED", "ok" if g3 else "RED"))
    h3 = n == len(seeds) and ok3 >= len(seeds) - 1
    return h1, h2, h3, lines


def judge(plain, unit_ok, always, invert):
    p1, p2, p3, lines = gates(plain)
    a1, a2, _, la = gates(always)
    i1, _, _, li = gates(invert)
    lines += ["  (ST_ADOPT_ALWAYS) " + x for x in la] + ["  (ST_ADOPT_INVERT) " + x for x in li]
    nc_always = not a2 and all(v is not None for v in always.values())
    nc_invert = not i1 and all(v is not None for v in invert.values())
    res = [("H1", p1, "every seed: adopt <= better alone - 0.5"),
           ("H2", p2, "every seed: a garbage peer gated leaves alone-A (+0.02), took 0"),
           ("H3", p3, "adopt <= always + 0.05 in all seeds but one"),
           ("H4", unit_ok, "the gate's unit cases"),
           ("NC-always", nc_always, "ST_ADOPT_ALWAYS turns H2 red"),
           ("NC-invert", nc_invert, "ST_ADOPT_INVERT turns H1 red")]
    for gid, ok, what in res:
        lines.append("[b5-adopt] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    ok = all(r[1] for r in res)
    lines.append("[result] %s b5-adopt" % ("PASS" if ok else "FAIL"))
    return ok, lines


def self_test():
    def A(alone, adopt, always, padopt, ptook, palways=15.0):
        return {"alone-A": (alone, None), "alone-B": (alone + 0.2, None), "full": (3.8, None),
                "always": (always, 8), "adopt": (adopt, 8), "poison-always": (palways, 8),
                "poison-adopt": (padopt, ptook)}
    good = {s: A(5.5, 4.4, 4.4, 5.5, 0) for s in "12"}
    alw = {s: A(5.5, 4.4, 4.4, 15.0, 8) for s in "12"}
    inv = {s: A(5.5, 5.5, 4.4, 5.5, 0) for s in "12"}
    cases = [
        ("green", (good, True, alw, inv), True),
        ("H1 short", ({"1": A(5.5, 5.01, 5.0, 5.5, 0), "2": good["2"]}, True, alw, inv), False),
        ("H2 took one", ({"1": A(5.5, 4.4, 4.4, 5.5, 1), "2": good["2"]}, True, alw, inv), False),
        ("H2 worse", ({"1": A(5.5, 4.4, 4.4, 5.53, 0), "2": good["2"]}, True, alw, inv), False),
        ("H3 one off ok", ({"1": A(5.5, 4.5, 4.4, 5.5, 0), "2": good["2"]}, True, alw, inv), True),
        ("H3 two off", ({s: A(5.5, 4.5, 4.4, 5.5, 0) for s in "12"}, True, alw, inv), False),
        ("unit red", (good, False, alw, inv), False),
        ("NC always green", (good, True, good, inv), False),
        ("NC invert green", (good, True, alw, good), False),
        ("missing", ({"1": None, "2": good["2"]}, True, alw, inv), False),
    ]
    bad = 0
    for name, args, want in cases:
        got, _ = judge(*args)
        print("[b5-adopt-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want
    return bad


def main(argv):
    if argv[1:] == ["--self-test"]:
        return 1 if self_test() else 0
    seeds, d, i = "91 92 93 94 95 96 97 98", None, 1
    while i < len(argv):
        if argv[i] == "--seeds":
            seeds = argv[i + 1]; i += 2
        else:
            d = argv[i]; i += 1
    if d is None:
        print(__doc__)
        return 2
    sl = seeds.split()
    get = lambda b: {s: read(os.path.join(d, "%s-%s.log" % (b, s))) for s in sl}
    up = os.path.join(d, "plain-unit.log")
    unit_ok = os.path.exists(up) and "[b5a] unit PASS" in open(up).read()
    ok, lines = judge(get("plain"), unit_ok, get("always"), get("invert"))
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
