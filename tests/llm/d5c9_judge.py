#!/usr/bin/env python3
"""d5c9_judge.py - the judge of D5-c stage 9 (research/brain-merge-parents.md).

Reads the logs of run_st_distill_parents.sh and applies the lines fixed in
section 3 of the design before any child ran on the judgment seeds. For each
seed, on the S1 child's TEST: d = keep - last, e = near - keep.

  line 1  on every seed [d5c-keep-min] (S1, S2) and stage 8's 16 lines are green
  line 2  d<0 on at least 9 of 12 seeds, and median d <= -0.03
  line 3  median |e| <= half of median |d|, or e>0 on at least 9 seeds

The main verdict is all three. Reading (not judged): "min" = line 2 green, e>0
on at least 9 seeds and median e >= +0.03; else "not-last" = near - last passes
line 2's line; else "undecided". d on the seeds where both children took the
same start is printed too.

A red [d5c-keep-min] (or a run that did not print its lines) means the gate or
the code is wrong: fix it and retake. A red among the 16 lines is a research
red: no retake (section 4).

  d5c9_judge.py <work dir> [seeds] [NC seeds]
  exit 0 = main PASS and the two counted NCs bite
"""
import os
import re
import statistics
import sys

JUDGE_SEEDS = [30, 31, 32, 33, 34, 35, 36, 37, 39, 40, 41, 42]
NC_SEEDS = [30, 31]
LINES16 = 16
MIN_COUNT = 9
D_LINE = -0.03
E_LINE = 0.03


def r6(x):
    return round(x, 6)


def med(v):
    return r6(statistics.median(v))


def over_line2(v):
    return sum(1 for x in v if x < 0) >= MIN_COUNT and med(v) <= D_LINE


def verdict(rows, need=12):
    seeds = sorted(rows)
    d = [r6(rows[s]["keep"] - rows[s]["last"]) for s in seeds]
    e = [r6(rows[s]["near"] - rows[s]["keep"]) for s in seeds]
    nl = [r6(rows[s]["near"] - rows[s]["last"]) for s in seeds]
    broken = [s for s in seeds if not rows[s]["keep_min"] or rows[s]["lines16"] != LINES16]
    red16 = [s for s in seeds if rows[s]["fail16"]]
    enough = len(seeds) == need
    line1 = enough and not broken and not red16
    line2 = enough and over_line2(d)
    e_pos = sum(1 for x in e if x > 0)
    med_abs_e = med([abs(x) for x in e]) if e else 0.0
    med_abs_d = med([abs(x) for x in d]) if d else 0.0
    line3 = enough and (med_abs_e <= r6(med_abs_d / 2) or e_pos >= MIN_COUNT)
    if line2 and e_pos >= MIN_COUNT and med(e) >= E_LINE:
        reading = "min"
    elif enough and over_line2(nl):
        reading = "not-last"
    else:
        reading = "undecided"
    same = [r6(rows[s]["keep"] - rows[s]["last"]) for s in seeds
            if rows[s]["start_keep"] == rows[s]["start_last"]]
    return dict(line1=line1, line2=line2, line3=line3, main=line1 and line2 and line3,
                reading=reading, retake=bool(broken) or not enough, broken=broken, red16=red16,
                d=d, e=e, nl=nl, e_pos=e_pos, med_abs_e=med_abs_e, med_abs_d=med_abs_d,
                same_start_n=len(same), same_start_median=med(same) if same else None)


def child(text):
    m = re.search(r"\[T-real S1 sel\] TEST A=\S+ B=\S+ child=([0-9.]+)", text)
    return float(m.group(1)) if m else None


def start(text):
    m = re.search(r"\[sel S1\] VAL .* -> chose (\w+)", text)
    return m.group(1) if m else None


def keep_min_lines(text):
    return re.findall(r"^  (PASS|FAIL)  \[d5c-keep-min\] (S1|S2):", text, re.M)


def lines16(text):
    out = []
    for m in re.finditer(r"^  (PASS|FAIL)  (.*)$", text, re.M):
        s = m.group(2)
        if "[d5c-keep-min]" in s:
            continue
        if ("start=sel" in s or "[d5c-sel-val]" in s or "[d5c-swa-points]" in s
                or "[d5c-swa-bounds]" in s):
            out.append(m.group(1))
    return out


def text_of(work, arm, s):
    try:
        with open(os.path.join(work, "%s-%d.log" % (arm, s)), errors="replace") as f:
            return f.read()
    except OSError:
        return None


def read_logs(work, seeds):
    rows = {}
    for s in seeds:
        tk, tl, tn = text_of(work, "keep", s), text_of(work, "last", s), text_of(work, "near", s)
        if tk is None or tl is None or tn is None:
            continue
        ck, cl, cn = child(tk), child(tl), child(tn)
        if ck is None or cl is None or cn is None:
            continue
        km = keep_min_lines(tk)
        l16 = lines16(tk)
        rows[s] = dict(keep=ck, last=cl, near=cn, start_keep=start(tk), start_last=start(tl),
                       keep_min=sorted(km) == [("PASS", "S1"), ("PASS", "S2")],
                       lines16=len(l16), fail16=sum(1 for x in l16 if x == "FAIL"))
    return rows


def nc_bites(work, arm, seeds):
    for s in seeds:
        t = text_of(work, arm, s)
        if t is None or not any(k == "FAIL" for k, _ in keep_min_lines(t)):
            return False
    return True


def main():
    work = sys.argv[1]
    seeds = [int(x) for x in sys.argv[2].split()] if len(sys.argv) > 2 else JUDGE_SEEDS
    ncs = [int(x) for x in sys.argv[3].split()] if len(sys.argv) > 3 else NC_SEEDS
    rows = read_logs(work, seeds)
    v = verdict(rows, need=len(JUDGE_SEEDS))
    for i, s in enumerate(sorted(rows)):
        r = rows[s]
        print("  [seed %d] child TEST keep %.4f (%s) last %.4f (%s) near %.4f  d %+.4f e %+.4f  keep-min %s  16 lines %d, FAIL %d"
              % (s, r["keep"], r["start_keep"], r["last"], r["start_last"], r["near"],
                 v["d"][i], v["e"][i], "green" if r["keep_min"] else "RED", r["lines16"], r["fail16"]))
    if rows:
        print("[result] seeds read %d of %d; d<0 on %d, median d %+.4f; e>0 on %d, median e %+.4f; median |d| %.4f, |e| %.4f; near-last<0 on %d, median %+.4f"
              % (len(rows), len(JUDGE_SEEDS), sum(1 for x in v["d"] if x < 0), med(v["d"]), v["e_pos"],
                 med(v["e"]), v["med_abs_d"], v["med_abs_e"],
                 sum(1 for x in v["nl"] if x < 0), med(v["nl"])))
    print("[result] same-start d: %d seeds, median %s"
          % (v["same_start_n"], "%+.4f" % v["same_start_median"] if v["same_start_n"] else "na"))
    print("[result] line 1 %s (keep-min or lines wrong on %s; red among the 16 on %s)"
          % ("green" if v["line1"] else "RED", v["broken"] or "none", v["red16"] or "none"))
    print("[result] line 2 %s" % ("green" if v["line2"] else "RED"))
    print("[result] line 3 %s" % ("green" if v["line3"] else "RED"))
    print("[result] reading (not judged): %s" % v["reading"])
    if v["retake"]:
        print("[result] the gate, the code or the run is wrong: fix and retake (section 4)")
    print("[result] main %s" % ("PASS" if v["main"] else "RED"))
    near = nc_bites(work, "near", seeds)
    nokeep = nc_bites(work, "NOKEEP", ncs)
    print("[result] near %s on [d5c-keep-min]" % ("RED on every seed - negative control bites" if near else "did NOT go red on every seed"))
    print("[result] NOKEEP %s on [d5c-keep-min]" % ("RED on every seed - negative control bites" if nokeep else "did NOT go red on every seed"))
    ok = v["main"] and near and nokeep
    print("[st-distill-parents] %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
