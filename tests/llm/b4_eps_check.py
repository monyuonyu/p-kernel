"""b4_eps_check.py - the judge of [b4-eps] (research/b4-numerics.md, stage 2).

Reads <build>-<tier>-<seed>.log written by run_b4_eps.sh (b2_curve_test.c).
Lines fixed in the design before any run:

  G1  every arm: best bpb of def <= best bpb of s1 - 0.05
  G2  every arm: def's bpb at k=1536 <= def's best + 0.1
  NC  nc (def with the old root) in place of def turns G1 red
  plus: every log has all five points, skipped 0, and "done"

  b4_eps_check.py --tiers "0 1" --seeds "71 72" <dir>   exit 0 = G1 G2 green, NC red
  b4_eps_check.py --self-test                           the judge's own cases
"""
import os
import re
import sys

KS = [96, 192, 384, 768, 1536]
PT = re.compile(r"^\[b2\] point k=(\d+) bpb=([0-9.]+) wins=(\d+) skipped=(\d+)$")


def read(path):
    """{k: bpb} if the log is whole, else None"""
    if not os.path.exists(path):
        return None
    pts, done = {}, False
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            m = PT.match(line)
            if m:
                if int(m.group(4)) != 0:
                    return None
                pts[int(m.group(1))] = float(m.group(2))
            elif line == "[b2] done":
                done = True
    if not done or sorted(pts) != KS:
        return None
    return pts


def judge(arms):
    """arms: {(tier, seed): {"s1": pts, "def": pts, "nc": pts}} -> (lines, ok)"""
    lines, g1, g2, ncr, whole = [], True, True, False, True
    for key in sorted(arms):
        a = arms[key]
        if any(a.get(b) is None for b in ("s1", "def", "nc")):
            lines.append("[b4-eps] FAIL whole tier %s seed %s: a log is missing or short" % key)
            whole = False
            continue
        bs, bd, bn = min(a["s1"].values()), min(a["def"].values()), min(a["nc"].values())
        e = a["def"][1536]
        ok1 = bd <= bs - 0.05
        ok2 = e <= bd + 0.1
        okn = bn <= bs - 0.05
        g1 &= ok1
        g2 &= ok2
        ncr |= not okn
        lines.append("[b4-eps] tier %s seed %s: best s1 %.4f def %.4f nc %.4f; def k=1536 %.4f"
                     " -> G1 %s (%+.4f) G2 %s (%+.4f) nc-G1 %s"
                     % (key[0], key[1], bs, bd, bn, e, "ok" if ok1 else "RED", bd - bs,
                        "ok" if ok2 else "RED", e - bd, "ok" if okn else "RED"))
    ok = whole and g1 and g2 and ncr
    lines.append("[b4-eps] %s G1 every arm def best <= s1 best - 0.05" % ("PASS" if whole and g1 else "FAIL"))
    lines.append("[b4-eps] %s G2 every arm def k=1536 <= def best + 0.1" % ("PASS" if whole and g2 else "FAIL"))
    lines.append("[b4-eps] %s NC the old root turns G1 red" % ("PASS" if whole and ncr else "FAIL"))
    lines.append("[result] %s b4-eps" % ("PASS" if ok else "FAIL"))
    return lines, ok


def self_test():
    def P(best, last):
        d = {k: best + 0.3 for k in KS}
        d[768] = best
        d[1536] = last
        return d
    good = {("0", "71"): {"s1": P(3.6, 3.7), "def": P(3.5, 3.55), "nc": P(3.6, 3.7)}}
    cases = [
        ("green", good, True),
        ("G1 short", {("0", "71"): {"s1": P(3.6, 3.7), "def": P(3.56, 3.6), "nc": P(3.6, 3.7)}}, False),
        ("G1 just", {("0", "71"): {"s1": P(3.6, 3.7), "def": P(3.55, 3.6), "nc": P(3.6, 3.7)}}, True),
        ("G2 red", {("0", "71"): {"s1": P(3.6, 3.7), "def": P(3.5, 3.61), "nc": P(3.6, 3.7)}}, False),
        ("NC green", {("0", "71"): {"s1": P(3.6, 3.7), "def": P(3.5, 3.55), "nc": P(3.5, 3.55)}}, False),
        ("missing", {("0", "71"): {"s1": P(3.6, 3.7), "def": None, "nc": P(3.6, 3.7)}}, False),
        ("one arm red", {**good, ("1", "72"): {"s1": P(3.6, 3.7), "def": P(3.58, 3.6),
                                                "nc": P(3.6, 3.7)}}, False),
    ]
    bad = 0
    for name, arms, want in cases:
        _, got = judge(arms)
        print("[b4-eps-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want
    return bad


def main(argv):
    if argv[1:] == ["--self-test"]:
        return 1 if self_test() else 0
    tiers, seeds, d = "0 1", "71 72", None
    i = 1
    while i < len(argv):
        if argv[i] == "--tiers":
            tiers = argv[i + 1]; i += 2
        elif argv[i] == "--seeds":
            seeds = argv[i + 1]; i += 2
        else:
            d = argv[i]; i += 1
    if d is None:
        print(__doc__)
        return 2
    arms = {}
    for t in tiers.split():
        for s in seeds.split():
            arms[(t, s)] = {b: read(os.path.join(d, "%s-%s-%s.log" % (b, t, s))) for b in ("s1", "def", "nc")}
    lines, ok = judge(arms)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
