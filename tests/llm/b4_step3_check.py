"""b4_step3_check.py - the judge of [b4-step3] (research/b4-step3.md, B4 stage 3).

Reads <arm>-<tier>-<seed>.log written by run_b4_step3.sh (b2_curve_test.c
built with -DB4_EVERY96: a "[b4] point" line every 96 steps, k = 96..1536).
The chosen point of an arm = the k whose "stop" bpb (the fixture's held side)
is lowest, the earliest on ties; its value is the b1_held bpb at that k.
Lines fixed in the design before any run (n pairs; "most" = ceil(3n/4),
6 of 8 for the 8 pairs of seeds 73..76 x S, M):

  H1  every pair: e3 chosen <= s1 chosen; most pairs: <= s1 chosen - 0.05
  H2  every pair: e3 k=1536 <= s1 k=1536; most pairs: <= s1 k=1536 - 0.05
  H3  every pair: e3 chosen - min of e3's 16 b1_held points <= 0.05
  NC1 nc1 in place of e3 turns H1 red                         (a gate)
  NC2 e3 choosing on "trainwin" turns H3 red                  (a reading)
  r5  printed only
  plus: every log has 16 points, skipped 0, and "done"

  b4_step3_check.py --tiers "0 1" --seeds "73 74 75 76" <dir>
                         exit 0 = H1 H2 H3 green and NC1 red
  b4_step3_check.py --self-test     the judge's own cases
"""
import os
import re
import sys

KS = list(range(96, 1537, 96))
PT = re.compile(r"^\[b4\] point k=(\d+) stop=(-?[0-9.]+) trainwin=(-?[0-9.]+) "
                r"held=([0-9.]+) wins=(\d+) skipped=(\d+)$")
ARMS = ("s1", "e3", "r5", "nc1")
M = 0.05


def read(path):
    """{k: (stop, trainwin, held)} if the log is whole, else None"""
    if not os.path.exists(path):
        return None
    pts, done = {}, False
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            m = PT.match(line)
            if m:
                if int(m.group(6)) != 0 or float(m.group(2)) < 0 or float(m.group(3)) < 0:
                    return None
                pts[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), float(m.group(4)))
            elif line == "[b2] done":
                done = True
    if not done or sorted(pts) != KS:
        return None
    return pts


def chosen(pts, col=0):
    """(k, b1_held bpb) at the lowest pts[k][col], earliest k on ties"""
    k = min(KS, key=lambda k: (pts[k][col], k))
    return k, pts[k][2]


def most(n):
    return -(-3 * n // 4)


def h1(arms, arm):
    """(all ok, count within -M, per-pair lines) for H1 with `arm` in place of e3"""
    allok, cnt, rows = True, 0, []
    for key in sorted(arms):
        a = arms[key]
        _, s = chosen(a["s1"])
        ke, e = chosen(a[arm])
        allok &= e <= s
        cnt += e <= s - M
        rows.append((key, ke, e, s))
    return allok, cnt, rows


def judge(arms):
    """arms: {(tier, seed): {arm: pts}} -> (lines, ok)"""
    lines, whole = [], True
    for key in sorted(arms):
        if any(arms[key].get(b) is None for b in ARMS):
            lines.append("[b4-step3] FAIL whole tier %s seed %s: a log is missing or short" % key)
            whole = False
    if not whole or not arms:
        lines.append("[result] FAIL b4-step3 (logs not whole)")
        return lines, False
    n, need = len(arms), most(len(arms))
    g2all, g2cnt, g3all, nc2red = True, 0, True, False
    for key in sorted(arms):
        a = arms[key]
        ks, s = chosen(a["s1"])
        ke, e = chosen(a["e3"])
        kr, r = chosen(a["r5"])
        kn, c = chosen(a["nc1"])
        kt, et = chosen(a["e3"], 1)
        lo = min(a["e3"][k][2] for k in KS)
        e_end, s_end = a["e3"][1536][2], a["s1"][1536][2]
        g2all &= e_end <= s_end
        g2cnt += e_end <= s_end - M
        g3all &= e - lo <= M
        nc2red |= et - lo > M
        lines.append("[b4-step3] tier %s seed %s: chosen s1 %.4f@%d e3 %.4f@%d (%+.4f) nc1 %.4f@%d"
                     " r5 %.4f@%d; k=1536 s1 %.4f e3 %.4f (%+.4f); e3 min %.4f gap %+.4f;"
                     " e3 on trainwin %.4f@%d gap %+.4f"
                     % (key[0], key[1], s, ks, e, ke, e - s, c, kn, r, kr, s_end, e_end,
                        e_end - s_end, lo, e - lo, et, kt, et - lo))
    a1, c1, _ = h1(arms, "e3")
    n1, nc1c, _ = h1(arms, "nc1")
    ok1 = a1 and c1 >= need
    ok2 = g2all and g2cnt >= need
    ok3 = g3all
    nc1red = not (n1 and nc1c >= need)
    lines.append("[b4-step3] %s H1 e3 chosen <= s1 chosen %s, <= -%.2f in %d/%d (need %d)"
                 % ("PASS" if ok1 else "FAIL", "every pair" if a1 else "NOT every pair", M, c1, n, need))
    lines.append("[b4-step3] %s H2 e3 k=1536 <= s1 k=1536 %s, <= -%.2f in %d/%d (need %d)"
                 % ("PASS" if ok2 else "FAIL", "every pair" if g2all else "NOT every pair", M, g2cnt, n, need))
    lines.append("[b4-step3] %s H3 e3 chosen within %.2f of its best b1_held point, every pair"
                 % ("PASS" if ok3 else "FAIL", M))
    lines.append("[b4-step3] %s NC1 the old root turns H1 red (nc1: %s, %d/%d)"
                 % ("PASS" if nc1red else "FAIL", "every pair" if n1 else "NOT every pair", nc1c, n))
    lines.append("[b4-step3] reading NC2: choosing on the training windows %s"
                 % ("turns H3 red (the held side is needed)" if nc2red else
                    "keeps H3 green: it is not shown that the held-side windows are needed"))
    ok = ok1 and ok2 and ok3 and nc1red
    lines.append("[result] %s b4-step3" % ("PASS" if ok else "FAIL"))
    return lines, ok


def self_test():
    def P(base, best_k=768, end=None, stop_best=None, tw_best=None, dip=0.3):
        """b1_held = base + dip except base at best_k; stop lowest at stop_best"""
        d = {}
        sb = best_k if stop_best is None else stop_best
        tb = 1536 if tw_best is None else tw_best
        for k in KS:
            h = base if k == best_k else base + dip
            if k == 1536 and end is not None:
                h = end
            d[k] = (1.0 if k == sb else 2.0, 1.0 if k == tb else 2.0, h)
        return d

    def pair(s1=None, e3=None, r5=None, nc1=None):
        return {"s1": s1 or P(3.6, end=3.7), "e3": e3 or P(3.5, end=3.6),
                "r5": r5 or P(3.55, end=3.65), "nc1": nc1 or P(3.6, end=3.7)}
    keys = [(t, s) for t in ("0", "1") for s in ("73", "74", "75", "76")]
    good = {k: pair() for k in keys}

    def with_(i, **kw):
        d = dict(good)
        d[keys[i]] = pair(**kw)
        return d

    def with_n(idx, **kw):
        d = dict(good)
        for i in idx:
            d[keys[i]] = pair(**kw)
        return d
    cases = [
        ("green", good, True),
        # H1: one pair worse than s1
        ("H1 one pair worse", with_(0, e3=P(3.61, end=3.6)), False),
        # H1: 2 pairs only -0.04 -> 6/8 still green
        ("H1 two short", with_n([0, 1], e3=P(3.56, end=3.6)), True),
        ("H1 three short", with_n([0, 1, 2], e3=P(3.56, end=3.6)), False),
        ("H1 just -0.05", with_n([0, 1, 2], e3=P(3.55, end=3.6)), True),
        # H1 judged at the chosen point, not the best: stop picks k=96 (base+dip)
        # best 3.52 (-0.08) but the chosen k=96 is 3.56 (-0.04); H3 gap 0.04 stays green
        ("H1 uses the chosen point", with_n([0, 1, 2], e3=P(3.52, stop_best=96, dip=0.04, end=3.6)), False),
        ("H1 chosen point control", with_n([0, 1, 2], e3=P(3.52, dip=0.04, end=3.6)), True),
        # H2
        ("H2 one pair worse", with_(0, e3=P(3.5, end=3.71)), False),
        ("H2 three short", with_n([0, 1, 2], e3=P(3.5, end=3.66)), False),
        # H3: chosen point 0.3 above the best -> red; also with H1 still ok
        ("H3 red", with_(0, e3=P(3.2, stop_best=96, end=3.6)), False),
        # NC1 green -> FAIL
        ("NC1 green", {k: pair(nc1=P(3.5, end=3.6)) for k in keys}, False),
        ("NC1 red by one worse pair", {**{k: pair(nc1=P(3.5, end=3.6)) for k in keys},
                                       keys[0]: pair(nc1=P(3.61, end=3.6))}, True),
        # NC2 is a reading: trainwin choosing badly does not change the verdict
        ("NC2 reading only", {k: pair(e3=P(3.5, end=3.6, tw_best=96)) for k in keys}, True),
        ("missing", {**good, keys[0]: {**pair(), "e3": None}}, False),
        ("no pairs", {}, False),
        # all stop values tie: the earliest (3.5) is chosen; the latest (3.6) would turn H3 red
        ("ties take the earliest", with_(0, e3={k: (1.0, 1.0, 3.5 if k == 96 else 3.6) for k in KS}), True),
    ]
    bad = 0
    for name, arms, want in cases:
        _, got = judge(arms)
        print("[b4-step3-self] %s %s" % ("ok" if got == want else "WRONG", name))
        bad += got != want
    # NC2 wording
    lines, _ = judge({k: pair(e3=P(3.5, end=3.6, tw_best=96)) for k in keys})
    nc2 = [x for x in lines if "NC2" in x][0]
    okw = "turns H3 red" in nc2
    print("[b4-step3-self] %s NC2 says red when trainwin picks a bad point" % ("ok" if okw else "WRONG"))
    bad += not okw
    lines, _ = judge({k: pair(e3=P(3.5, end=3.6, tw_best=768)) for k in keys})
    nc2 = [x for x in lines if "NC2" in x][0]
    okw = "not shown" in nc2
    print("[b4-step3-self] %s NC2 says not shown when trainwin is as good" % ("ok" if okw else "WRONG"))
    bad += not okw
    # read(): a short log and a skipped window are not whole
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "x.log")
        body = "".join("[b4] point k=%d stop=2.0000 trainwin=2.0000 held=3.5000 wins=331 skipped=0\n" % k
                       for k in KS)
        cases_r = [("whole", body + "[b2] done\n", True),
                   ("no done", body, False),
                   ("15 points", "\n".join(body.splitlines()[1:]) + "\n[b2] done\n", False),
                   ("skipped", body.replace("k=96 stop=2.0000 trainwin=2.0000 held=3.5000 wins=331 skipped=0",
                                            "k=96 stop=2.0000 trainwin=2.0000 held=3.5000 wins=330 skipped=1")
                    + "[b2] done\n", False),
                   ("stop -1", body.replace("k=192 stop=2.0000", "k=192 stop=-1.0000") + "[b2] done\n", False)]
        for name, text, want in cases_r:
            with open(p, "w") as f:
                f.write(text)
            got = read(p) is not None
            print("[b4-step3-self] %s read %s" % ("ok" if got == want else "WRONG", name))
            bad += got != want
    return bad


def main(argv):
    if argv[1:] == ["--self-test"]:
        return 1 if self_test() else 0
    tiers, seeds, d = "0 1", "73 74 75 76", None
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
            arms[(t, s)] = {b: read(os.path.join(d, "%s-%s-%s.log" % (b, t, s))) for b in ARMS}
    lines, ok = judge(arms)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
