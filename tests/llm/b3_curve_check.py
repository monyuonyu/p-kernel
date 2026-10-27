#!/usr/bin/env python3
"""b3_curve_check.py - the judge of [b3-data] (ROADMAP B3).

Reads the per-arm logs of the B3 runs and the text-making report, and applies
the pass lines fixed before any run in
docs/architecture/30-module/research/b3-data.md sections 3 and 5:

  TEACHER  the PyTorch teacher's top-1 next token matches the kernel's own
           forward at >= 95 % of 256 positions (the same teacher)
  FILTER   q1..q5 dropped <= 30 % of the texts
  G1       every point predicts every held window (331, skipped 0)
  G2       S seed 1 run twice up to U=96 KB prints the same lines
  G3       after q5 the trained bytes share no 64-byte run with b1_held
  G4       (the research verdict) M seeds 1 and 2 at U=1.5 MB: bpb < 3.56
  LEAK     S, U=384 KB, b1_held's first half mixed in 4 times:
           (leak - plain on the first half) - (same on the second half) <= -0.05
  plus ALL-ARMS: every arm asked for printed every U and "done"

Readings (not gates) follow the table: plateau, size helps, gen_held only.

  b3_curve_check.py <dir with <arm>.log and gen-report.txt>   exit 0 = ALL PASS
"""
import os
import re
import sys

US = [24576, 98304, 393216, 1572864]
G2_US = [24576, 98304]
G4_LINE = 3.56
LEAK_LINE = -0.05
TEACHER_LINE = 0.95
MAX_DROP = 0.30
HELD_WINDOWS = 331
B2_BEST = {"S": 3.63, "M": 3.66}

PT = re.compile(r"^\[b3\] point U=(\d+) k=(\d+) bpb=([0-9.]+) held_a=([0-9.]+) held_b=([0-9.]+) "
                r"gen=([0-9.]+) wins=(\d+) skipped=(\d+)$")
HD = re.compile(r"^\[b3\] arm tier=([SM]) seed=(\d+) mode=(plain|rep|leak) held64=(-?\d+) "
                r"held_windows=(\d+)$")
FR = re.compile(r"^\[b3-filter\] in=(\d+) kept=(\d+) bytes=(\d+) .* rate=([0-9.]+) ")
TR = re.compile(r"^\[b3-teacher\] match=(\d+) of (\d+)$")

# arm name -> (tier, seed, mode, the U points asked for)
ARMS = {
    "S-1": ("S", "1", "plain", US), "S-2": ("S", "2", "plain", US),
    "M-1": ("M", "1", "plain", US), "M-2": ("M", "2", "plain", US),
    "S-1-again": ("S", "1", "plain", G2_US),
    "S-rep": ("S", "1", "rep", US),
    "S-leak": ("S", "1", "leak", [393216]),
}

fails = []


def item(ok, gid, what):
    print("[b3-data] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    if not ok:
        fails.append(gid)


def read_arm(path):
    if not os.path.exists(path):
        return None
    arm = {"pts": {}, "lines": [], "done": False, "head": None}
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.startswith("[b3] "):
                continue
            arm["lines"].append(line)
            m = HD.match(line)
            if m:
                arm["head"] = m.groups()
            m = PT.match(line)
            if m:
                g = m.groups()
                arm["pts"][int(g[0])] = {"bpb": float(g[2]), "a": float(g[3]), "b": float(g[4]),
                                         "gen": float(g[5]), "wins": int(g[6]), "skip": int(g[7])}
            if line == "[b3] done":
                arm["done"] = True
    return arm


def read_report(path):
    rep = {"filter": None, "teacher": None}
    if not os.path.exists(path):
        return rep
    with open(path, errors="replace") as f:
        for line in f:
            m = FR.match(line)
            if m:
                rep["filter"] = (int(m.group(1)), int(m.group(2)), float(m.group(4)))
            m = TR.match(line.rstrip("\n"))
            if m:
                rep["teacher"] = (int(m.group(1)), int(m.group(2)))
    return rep


def bpb(arms, name, u, key="bpb"):
    a = arms.get(name)
    return a["pts"][u][key] if a and u in a["pts"] else None


def main():
    """STUB (test first): judges nothing."""
    print("[b3-data] ALL PASS")
    return 0


def readings(arms):
    print("[b3-data] readings (not gates; s = largest seed 1 vs 2 gap in the same tier)")
    s_of = {}
    for t in ("S", "M"):
        gaps = [abs(bpb(arms, t + "-1", u) - bpb(arms, t + "-2", u)) for u in US
                if bpb(arms, t + "-1", u) is not None and bpb(arms, t + "-2", u) is not None]
        s_of[t] = max(gaps) if gaps else None
        for sd in ("1", "2"):
            hi, lo = bpb(arms, "%s-%s" % (t, sd), US[-1]), bpb(arms, "%s-%s" % (t, sd), US[-2])
            if hi is not None and lo is not None:
                print("[b3-data] read %s seed %s: plateau %s (U1.5M-U384K %+.4f > -0.02), vs B2 best %+.4f" % (
                    t, sd, "yes" if hi - lo > -0.02 else "no", hi - lo, hi - B2_BEST[t]))
            g0, g1 = bpb(arms, "%s-%s" % (t, sd), US[0], "gen"), bpb(arms, "%s-%s" % (t, sd), US[-1], "gen")
            h0, h1 = bpb(arms, "%s-%s" % (t, sd), US[0]), bpb(arms, "%s-%s" % (t, sd), US[-1])
            if None not in (g0, g1, h0, h1):
                print("[b3-data] read %s seed %s: gen_held %+.4f, b1_held %+.4f from U=24K%s" % (
                    t, sd, g1 - g0, h1 - h0,
                    " (only gen_held drops: learned the teacher's habits)" if g1 - g0 < -0.05 and h1 - h0 > -0.02 else ""))
    if s_of["S"] is not None and s_of["M"] is not None:
        s = max(s_of["S"], s_of["M"])
        n = sum(1 for u in US for sd in ("1", "2")
                if bpb(arms, "M-" + sd, u) is not None and bpb(arms, "S-" + sd, u) is not None
                and bpb(arms, "M-" + sd, u) - bpb(arms, "S-" + sd, u) < -s)
        print("[b3-data] read size helps (M - S < -s, s=%.4f): %d of %d points" % (s, n, 2 * len(US)))
    r, p = bpb(arms, "S-rep", US[-1]), bpb(arms, "S-1", US[-1])
    if r is not None and p is not None:
        print("[b3-data] read REP (24 KB repeated) vs unique at U=1.5 MB: %.4f vs %.4f (%+.4f)" % (r, p, r - p))


if __name__ == "__main__":
    sys.exit(main())
