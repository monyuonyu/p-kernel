"""merge_e3_check.py - the judge of the merge-e3 cert (research/brain-merge-e3.md 3).

Reads <arm>-<seed>-<rep>.log (rep 1 and 2) written by run_merge_e3.sh
(student_merge_e3_test.c). mw = TEST loss of the plain mean - TEST loss of
the worse parent; "mean <= worse" is mw <= 0. Lines fixed in the design
before any judgement run (judgement seeds 107..114, n = 8):

  E1   prod (ST_LR, eps 1e-8):   mw <= 0 on >= 7 seeds
  NC1  old  (0.02, eps 1/64):    mw >  0 on >= 5 seeds  (the ruler's tooth;
       without it a green E1 says nothing)
  E2   e2a  (3e-3, eps 1e-8):    mw >  0 on >= 5 seeds, and
       e2b  (3e-3, eps 1/64):    mw <= 0 on >= 6 seeds  (recorded; decides
       only the st_merge_guarded note, not the exit code)
  plus: every log whole ("[me3] done", one line), the arm ran on its build
  (lr and eps as above), rep 1 and rep 2 byte-identical, every seed present.
  Recorded only: mean - better parent, TEST and VAL barriers.

  merge_e3_check.py <dir>      exit 0 = whole, E1 green and NC1 green
  merge_e3_check.py --seeds "107 108" <dir>   other seeds: the counts scale
                               as ceil(k * n / 8) and the verdict says so
  merge_e3_check.py --self-test
"""
import math
import os
import re
import sys
import tempfile

SEEDS = list(range(107, 115))
ST_LR = 1e-3                    # student.h; the line's lr must equal it
ARMS = {"prod": (ST_LR, 1e-8), "old": (0.02, 0.015625),
        "e2a": (3e-3, 1e-8), "e2b": (3e-3, 0.015625)}
LINE = re.compile(
    r"^\[me3\] arm=(\w+) seed=(\d+) lr=([0-9.eE+-]+) eps=([0-9.eE+-]+) rounds=40 "
    r"tw=20 vw=4 sw=4 acc=1 A=([0-9.]+) B=([0-9.]+) mean=([0-9.]+) mw=([+-][0-9.]+) "
    r"mb=([+-][0-9.]+) barT=([0-9.-]+) barV=([0-9.-]+) vA=([0-9.]+) vB=([0-9.]+) "
    r"vmean=([0-9.]+)$")


def close(a, b):
    return abs(a - b) <= 1e-6 * max(abs(a), abs(b))


def read(path, arm, seed):
    """(record, None) or (None, why)"""
    if not os.path.exists(path):
        return None, "missing"
    with open(path, "rb") as f:
        raw = f.read()
    lines = raw.decode("utf-8", "replace").splitlines()
    recs = [LINE.match(x) for x in lines if x.startswith("[me3] arm=")]
    if lines.count("[me3] done") != 1 or not lines or lines[-1] != "[me3] done":
        return None, "no single [me3] done at the end"
    if len(recs) != 1 or recs[0] is None:
        return None, "not exactly one well-formed [me3] line"
    m = recs[0]
    if m.group(1) != arm or int(m.group(2)) != seed:
        return None, "the line is arm=%s seed=%s" % (m.group(1), m.group(2))
    lr, eps = float(m.group(3)), float(m.group(4))
    wlr, weps = ARMS[arm]
    if not close(lr, wlr) or not close(eps, weps):
        return None, "lr %g eps %g, the arm wants lr %g eps %g (wrong build?)" % (lr, eps, wlr, weps)
    a, b, mean, mw = float(m.group(5)), float(m.group(6)), float(m.group(7)), float(m.group(8))
    if abs((mean - max(a, b)) - mw) > 2e-4:
        return None, "mw %+.4f is not mean - worse (%.4f - %.4f)" % (mw, mean, max(a, b))
    return {"mw": mw, "mb": float(m.group(9)), "barT": float(m.group(10)),
            "barV": float(m.group(11)), "raw": raw}, None


def need(k, n):
    return int(math.ceil(k * n / 8.0))


def judge(d, seeds):
    """-> (lines, exit_ok, e2_ok)"""
    out, whole, recs = [], True, {}
    for arm in ARMS:
        for s in seeds:
            r1, w1 = read(os.path.join(d, "%s-%d-1.log" % (arm, s)), arm, s)
            r2, w2 = read(os.path.join(d, "%s-%d-2.log" % (arm, s)), arm, s)
            if r1 is None or r2 is None:
                out.append("[me3] FAIL whole %s seed %d: %s" % (arm, s, w1 or w2))
                whole = False
                continue
            if r1["raw"] != r2["raw"]:
                out.append("[me3] FAIL repeat %s seed %d: two runs differ" % (arm, s))
                whole = False
                continue
            recs[(arm, s)] = r1
    if not whole:
        out.append("[me3] verdict FAIL: logs not whole")
        return out, False, False
    n = len(seeds)
    for s in seeds:
        out.append("[me3] seed %d: mw prod %+.4f old %+.4f e2a %+.4f e2b %+.4f |"
                   " mb prod %+.4f | barT prod %.4f old %.4f | barV prod %.4f old %.4f"
                   % (s, recs[("prod", s)]["mw"], recs[("old", s)]["mw"], recs[("e2a", s)]["mw"],
                      recs[("e2b", s)]["mw"], recs[("prod", s)]["mb"], recs[("prod", s)]["barT"],
                      recs[("old", s)]["barT"], recs[("prod", s)]["barV"], recs[("old", s)]["barV"]))
    le = lambda arm: sum(1 for s in seeds if recs[(arm, s)]["mw"] <= 0)
    gt = lambda arm: n - le(arm)
    e1 = le("prod") >= need(7, n)
    nc1 = gt("old") >= need(5, n)
    e2 = gt("e2a") >= need(5, n) and le("e2b") >= need(6, n)
    tag = "" if seeds == SEEDS else " (seeds %s are not the judgement seeds 107..114)" % seeds
    out.append("[me3] E1  %s: prod mean <= worse on %d/%d (need %d)" % ("green" if e1 else "RED", le("prod"), n, need(7, n)))
    out.append("[me3] NC1 %s: old mean > worse on %d/%d (need %d)" % ("green" if nc1 else "RED", gt("old"), n, need(5, n)))
    out.append("[me3] E2  %s: e2a mean > worse on %d/%d (need %d), e2b mean <= worse on %d/%d (need %d) [recorded]"
               % ("green" if e2 else "red", gt("e2a"), n, need(5, n), le("e2b"), n, need(6, n)))
    ok = e1 and nc1
    out.append("[me3] verdict %s%s" % ("PASS" if ok else "FAIL", tag))
    return out, ok and seeds == SEEDS, e2


# ---------------------------------------------------------------- self-test
def fake_line(arm, seed, mw, lr=None, eps=None, a=1.5, mb=None):
    wlr, weps = ARMS[arm]
    lr = wlr if lr is None else lr
    eps = weps if eps is None else eps
    b = a - 0.1
    mean = a + mw
    mb = (mean - b) if mb is None else mb
    return ("[me3] arm=%s seed=%d lr=%g eps=%g rounds=40 tw=20 vw=4 sw=4 acc=1 A=%.4f B=%.4f "
            "mean=%.4f mw=%+.4f mb=%+.4f barT=0.1000 barV=0.1000 vA=1.7000 vB=1.7000 vmean=1.7000\n"
            % (arm, seed, lr, eps, a, b, mean, mw, mb))


def write_set(d, mws, edit=None):
    """mws: {arm: [mw per seed]}; edit(arm, seed, rep, text) -> text or None (= no file)"""
    for arm, vals in mws.items():
        for s, mw in zip(SEEDS, vals):
            for rep in (1, 2):
                t = fake_line(arm, s, mw) + "[me3] done\n"
                if edit:
                    t = edit(arm, s, rep, t)
                if t is None:
                    continue
                with open(os.path.join(d, "%s-%d-%d.log" % (arm, s, rep)), "w") as f:
                    f.write(t)


GOOD = {"prod": [-0.2] * 8, "old": [0.3] * 8, "e2a": [0.3] * 8, "e2b": [-0.2] * 8}


def mix(base, arm, vals):
    m = dict(base)
    m[arm] = vals
    return m


def self_test():
    cases = [
        # (name, mws, edit, want_exit_ok, want_e2)
        ("all green", GOOD, None, True, True),
        ("E1 7/8 is green", mix(GOOD, "prod", [-0.2] * 7 + [0.1]), None, True, True),
        ("E1 6/8 is red", mix(GOOD, "prod", [-0.2] * 6 + [0.1] * 2), None, False, True),
        ("E1 mw +0.0000 counts as <=", mix(GOOD, "prod", [0.0] * 8), None, True, True),
        ("E1 mw +0.0001 counts as >", mix(GOOD, "prod", [0.0001] * 2 + [-0.2] * 6), None, False, True),
        ("NC1 5/8 is green", mix(GOOD, "old", [0.3] * 5 + [-0.1] * 3), None, True, True),
        ("NC1 4/8 is red", mix(GOOD, "old", [0.3] * 4 + [-0.1] * 4), None, False, True),
        ("E2 e2a 4/8 is red, exit still ok", mix(GOOD, "e2a", [0.3] * 4 + [-0.1] * 4), None, True, False),
        ("E2 e2b 5/8 is red, exit still ok", mix(GOOD, "e2b", [-0.2] * 5 + [0.1] * 3), None, True, False),
        ("a log missing", GOOD, lambda a, s, r, t: None if (a, s, r) == ("old", 110, 2) else t, False, False),
        ("no done", GOOD, lambda a, s, r, t: t.replace("[me3] done\n", "") if (a, s) == ("prod", 107) else t, False, False),
        ("two done", GOOD, lambda a, s, r, t: t + "[me3] done\n" if (a, s) == ("prod", 107) else t, False, False),
        ("line twice", GOOD, lambda a, s, r, t: t.split("[me3] done")[0] + t if (a, s) == ("e2a", 111) else t, False, False),
        ("reps differ", GOOD, lambda a, s, r, t: t.replace("vA=1.7000", "vA=1.7001") if (a, s, r) == ("prod", 112, 2) else t, False, False),
        ("old arm on the eps 1e-8 build", GOOD,
         lambda a, s, r, t: t.replace("eps=0.015625", "eps=1e-08") if a == "old" else t, False, False),
        ("prod arm at 3e-3 (not ST_LR)", GOOD,
         lambda a, s, r, t: t.replace("lr=0.001 ", "lr=0.003 ") if a == "prod" else t, False, False),
        ("seed label wrong", GOOD,
         lambda a, s, r, t: t.replace("seed=113", "seed=107") if (a, s) == ("prod", 113) else t, False, False),
        ("arm label wrong", GOOD,
         lambda a, s, r, t: t.replace("arm=e2b", "arm=e2a").replace("eps=0.015625", "eps=1e-08")
         if a == "e2b" else t, False, False),
        ("mw lies (not mean - worse)", GOOD,
         lambda a, s, r, t: t.replace("mw=-0.2000", "mw=-0.3000") if (a, s) == ("prod", 114) else t, False, False),
        ("acc 0", GOOD, lambda a, s, r, t: t.replace("acc=1", "acc=0") if (a, s) == ("prod", 108) else t, False, False),
        ("rounds 20", GOOD, lambda a, s, r, t: t.replace("rounds=40", "rounds=20") if a == "prod" else t, False, False),
        ("trailing line after done", GOOD, lambda a, s, r, t: t + "junk\n" if (a, s) == ("old", 109) else t, False, False),
    ]
    bad = 0
    for name, mws, edit, want_ok, want_e2 in cases:
        with tempfile.TemporaryDirectory() as d:
            write_set(d, mws, edit)
            lines, ok, e2 = judge(d, SEEDS)
        good = (ok == want_ok) and (not want_ok or e2 == want_e2)
        print("[me3-self] %s %s (exit_ok %s want %s, e2 %s)" % ("ok " if good else "BAD", name, ok, want_ok, e2))
        if not good:
            bad += 1
            for x in lines:
                print("    " + x)
    # other seeds never give exit 0
    with tempfile.TemporaryDirectory() as d:
        write_set(d, GOOD)
        _, ok, _ = judge(d, SEEDS[:2])
    print("[me3-self] %s other seeds do not exit 0" % ("ok " if not ok else "BAD"))
    bad += 1 if ok else 0
    print("[me3-self] %s" % ("PASS" if bad == 0 else "FAIL %d" % bad))
    return bad == 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return 0 if self_test() else 1
    seeds = SEEDS
    args = argv[1:]
    if len(args) == 3 and args[0] == "--seeds":
        seeds = [int(x) for x in args[1].split()]
        args = args[2:]
    if len(args) != 1:
        print(__doc__)
        return 2
    lines, ok, _ = judge(args[0], seeds)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
