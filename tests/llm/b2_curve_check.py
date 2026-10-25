#!/usr/bin/env python3
"""b2_curve_check.py - the judge of [b2-curve] (ROADMAP B2).

Reads the per-arm logs written by run_b2_curve.sh and applies the pass lines
and readings fixed before any run in
docs/architecture/30-module/research/b2-size-curve.md:

  G1   every point predicts every held window (skipped 0)
  G2   S D=3072 seed 1 run twice prints the same lines
  G3   no plain arm shares a 16-byte run with the held text
  LEAK the leak arm's bpb at k=1536 is >= 0.3 below the plain arm's
  plus: every arm that was asked for printed every k and "done"

Readings (not gates) are printed after the table.

  b2_curve_check.py --tiers "S M L" <dir with <arm>.log>   exit 0 = ALL PASS
"""
import os
import re
import sys

KS = {"S": [96, 192, 384, 768, 1536], "M": [96, 192, 384, 768, 1536], "L": [96, 192, 384, 768]}
DS = [768, 1536, 3072]
PT = re.compile(r"^\[b2\] point k=(\d+) bpb=([0-9.]+) wins=(\d+) skipped=(\d+)$")
HD = re.compile(r"^\[b2\] arm tier=(\d) D=(\d+) seed=(\d+) leak=(\d) shared=(-?\d+) "
                r"unigram=([0-9.]+) bigram=([0-9.]+) held_windows=(\d+)$")

fails = []


def item(ok, gid, what):
    print("[b2-curve] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    if not ok:
        fails.append(gid)


def read_arm(path):
    if not os.path.exists(path):
        return None
    arm = {"pts": {}, "lines": [], "done": False, "head": None}
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.startswith("[b2] "):
                continue
            arm["lines"].append(line)
            m = HD.match(line)
            if m:
                arm["head"] = m.groups()
            m = PT.match(line)
            if m:
                arm["pts"][int(m.group(1))] = (float(m.group(2)), int(m.group(3)), int(m.group(4)))
            if line == "[b2] done":
                arm["done"] = True
    return arm


def main():
    args = sys.argv[1:]
    tiers = "S M L"
    if len(args) >= 2 and args[0] == "--tiers":
        tiers, args = args[1], args[2:]
    if len(args) != 1:
        print(__doc__)
        return 2
    d = args[0]
    tiers = tiers.split()

    want = []
    for t in tiers:
        if t == "L":
            want.append(("L-3072-1", "L", 3072))
        else:
            for D in DS:
                for s in (1, 2):
                    want.append(("%s-%d-%d" % (t, D, s), t, D))
    arms = {name: read_arm(os.path.join(d, name + ".log")) for name, _, _ in want}

    complete = True
    for name, t, _ in want:
        a = arms[name]
        if a is None or not a["done"] or a["head"] is None or sorted(a["pts"]) != KS[t]:
            print("[b2-curve] arm %s incomplete (%s)" % (name, "no log" if a is None else
                  "points %s, done %s" % (sorted(a["pts"]), a["done"])))
            complete = False
    item(complete, "ALL-ARMS", "every arm asked for printed every k and finished")

    # table
    print("[b2-curve] table: bpb on b1_held (chance 8.0), comparators from the arm's own D bytes")
    print("[b2-curve] arm           " + " ".join("k=%-6d" % k for k in KS["S"]) + " unigram bigram")
    for name, t, _ in want:
        a = arms[name]
        if a is None:
            continue
        row = " ".join(("%-8.4f" % a["pts"][k][0]) if k in a["pts"] else "-       " for k in KS["S"])
        h = a["head"]
        print("[b2-curve] %-13s %s %s %s" % (name, row, h[5] if h else "-", h[6] if h else "-"))

    g1 = complete
    for name, _, _ in want:
        a = arms[name]
        if a is None or a["head"] is None:
            continue
        hw = int(a["head"][7])
        for k, (_, wins, skipped) in a["pts"].items():
            if skipped != 0 or wins != hw or hw < 256:
                g1 = False
    item(g1, "G1", "every point predicts every held window (skipped 0)")

    if "S" in tiers:
        a, b = arms.get("S-3072-1"), read_arm(os.path.join(d, "S-3072-1-again.log"))
        item(a is not None and b is not None and sorted(a["pts"]) == KS["S"] and a["done"]
             and a["lines"] == b["lines"],
             "G2", "S D=3072 seed 1 run twice prints the same lines")
        lk = read_arm(os.path.join(d, "S-3072-1-leak.log"))
        ok = (a is not None and lk is not None and 1536 in a["pts"] and 1536 in lk["pts"]
              and lk["head"] is not None and lk["head"][3] == "1"
              and lk["pts"][1536][0] <= a["pts"][1536][0] - 0.3)
        print("[b2-curve] leak arm k=1536 bpb %s, plain %s" % (
            lk["pts"].get(1536, ("-",))[0] if lk else "-", a["pts"].get(1536, ("-",))[0] if a else "-"))
        item(ok, "LEAK", "mixing in the held text's first half lowers bpb at k=1536 by >= 0.3")
    else:
        print("[b2-curve] G2 and LEAK not measured (S not run)")

    g3 = all(arms[n] is not None and arms[n]["head"] is not None and arms[n]["head"][4] == "0"
             for n, _, _ in want)
    item(g3, "G3", "no arm shares a 16-byte run with the held text")

    readings(arms, tiers)
    print("[b2-curve] %s" % ("ALL PASS" if not fails else "RED (" + " ".join(fails) + ")"))
    return 1 if fails else 0


def bp(arms, t, D, s, k):
    a = arms.get("%s-%d-%d" % (t, D, s))
    return a["pts"][k][0] if a and k in a["pts"] else None


def readings(arms, tiers):
    print("[b2-curve] readings (not gates; s = largest seed 1 vs 2 gap in the same tier and D)")
    s_of = {}
    for t in ("S", "M"):
        if t not in tiers:
            continue
        gaps = [abs(bp(arms, t, D, 1, k) - bp(arms, t, D, 2, k)) for D in DS for k in KS[t]
                if bp(arms, t, D, 1, k) is not None and bp(arms, t, D, 2, k) is not None]
        s_of[t] = max(gaps) if gaps else None
        print("[b2-curve] read %s s=%s" % (t, "%.4f" % s_of[t] if s_of[t] is not None else "-"))
        for D in DS:
            for s in (1, 2):
                b1536, b768, b384 = (bp(arms, t, D, s, k) for k in (1536, 768, 384))
                if None in (b1536, b768, b384):
                    continue
                print("[b2-curve] read %s D=%d seed %d: plateau %s (k1536-k768 %+.4f > -0.02), "
                      "overfit %s (k1536-k384 %+.4f > 0.02)" % (
                          t, D, s, "yes" if b1536 - b768 > -0.02 else "no", b1536 - b768,
                          "yes" if b1536 - b384 > 0.02 else "no", b1536 - b384))
            for s in (1, 2):
                lo, hi = bp(arms, t, 768, s, 1536), bp(arms, t, 3072, s, 1536)
                if D == 3072 and lo is not None and hi is not None and s_of[t] is not None:
                    print("[b2-curve] read %s seed %d: amount helps %s (D768-D3072 at k1536 %+.4f > s)" % (
                        t, s, "yes" if lo - hi > s_of[t] else "no", lo - hi))
    if "S" in tiers and "M" in tiers and s_of.get("S") is not None and s_of.get("M") is not None:
        s = max(s_of["S"], s_of["M"])
        n = tot = 0
        for D in DS:
            for sd in (1, 2):
                for k in KS["M"]:
                    m_, s_ = bp(arms, "M", D, sd, k), bp(arms, "S", D, sd, k)
                    if m_ is None or s_ is None:
                        continue
                    tot += 1
                    n += (m_ - s_ < -s)
        print("[b2-curve] read size helps (M - S < -s, s=%.4f): %d of %d points" % (s, n, tot))
    if "L" in tiers and "M" in tiers:
        better, seen = [], 0
        for k in KS["L"]:
            l_, m_ = bp(arms, "L", 3072, 1, k), bp(arms, "M", 3072, 1, k)
            if l_ is not None and m_ is not None:
                seen += 1
                print("[b2-curve] read L vs M D=3072 seed 1 k=%d: L %.4f M %.4f" % (k, l_, m_))
                if l_ < m_:
                    better.append(k)
        if seen:
            print("[b2-curve] read L better than M at k = %s" % (
                better if better else "none (at this amount L does not learn enough)"))
        else:
            print("[b2-curve] read L vs M not measured")


if __name__ == "__main__":
    sys.exit(main())
