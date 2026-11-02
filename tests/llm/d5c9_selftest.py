#!/usr/bin/env python3
"""d5c9_selftest.py - the judge of D5-c stage 9 on tables and logs made here.

Seconds, no model. The lines are those of brain-merge-parents.md section 3;
each case below goes red on its own break of d5c9_judge.py.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d5c9_judge as J  # noqa: E402

SEEDS = J.JUDGE_SEEDS
fails = 0


def check(cond, msg):
    global fails
    print("  %s  %s" % ("PASS" if cond else "FAIL", msg))
    if not cond:
        fails += 1


def rows(d, e, **kw):
    """12 rows: keep = 2.0, last = keep - d, near = keep + e (lists or one value)."""
    ds = d if isinstance(d, list) else [d] * 12
    es = e if isinstance(e, list) else [e] * 12
    out = {}
    for i, s in enumerate(SEEDS):
        keep = 2.0
        out[s] = dict(keep=keep, last=round(keep - ds[i], 4), near=round(keep + es[i], 4),
                      start_keep="init", start_last="init",
                      keep_min=True, lines16=16, fail16=0)
        out[s].update(kw)
    return out


def main():
    # the design's table of the self-test (section 3)
    v = J.verdict(rows(0.0, 0.0))
    check(not v["line2"] and not v["main"], "same table: line 2 red")

    v = J.verdict(rows(-0.05, 0.06))
    check(v["line2"] and v["line3"] and v["main"], "e one-sided and big: line 3 green")
    check(v["reading"] == "min", "e one-sided and big: reading = the min round")

    es = [0.001, -0.001] * 6
    v = J.verdict(rows(-0.05, es))
    check(v["line3"] and v["main"], "e about 0: line 3 green")
    check(v["reading"] == "not-last", "e about 0: reading = avoiding the last weights")

    es = [0.08, -0.08] * 6
    v = J.verdict(rows(-0.05, es))
    check(not v["line3"] and not v["main"], "e big on both signs: line 3 red")
    check(v["reading"] == "undecided", "e big on both signs: reading = undecided")

    # line 2: the count and the median, at the edge
    ds = [-0.05] * 9 + [0.05] * 3
    check(J.verdict(rows(ds, 0.0))["line2"], "d<0 on 9 of 12: line 2 green")
    ds = [-0.05] * 8 + [0.05] * 4
    check(not J.verdict(rows(ds, 0.0))["line2"], "d<0 on 8 of 12: line 2 red")
    ds = [0.0] * 3 + [-0.03] * 9
    check(J.verdict(rows(ds, 0.0))["line2"], "median d = -0.03: line 2 green")
    ds = [0.0] * 3 + [-0.0299] * 9
    check(not J.verdict(rows(ds, 0.0))["line2"], "median d = -0.0299: line 2 red")
    ds = [0.0] * 3 + [-0.0001] * 9
    check(not J.verdict(rows(ds, 0.0))["line2"], "count alone (median near 0): line 2 red")

    # line 3: half of |d|, or e>0 on 9 seeds
    es = [0.025, -0.025] * 6
    check(J.verdict(rows(-0.05, es))["line3"], "median |e| = half of median |d|: line 3 green")
    es = [0.0251, -0.0251] * 6
    check(not J.verdict(rows(-0.05, es))["line3"], "median |e| over the half, e>0 on 6: line 3 red")
    es = [0.06] * 9 + [-0.06] * 3
    check(J.verdict(rows(-0.05, es))["line3"], "e>0 on 9, |e| big: line 3 green")
    es = [0.06] * 8 + [-0.06] * 4
    check(not J.verdict(rows(-0.05, es))["line3"], "e>0 on 8, |e| big: line 3 red")

    # the reading: e itself, not a look across the lines
    es = [0.03] * 9 + [-0.06] * 3
    check(J.verdict(rows(-0.05, es))["reading"] == "min", "e>0 on 9, median +0.03: min")
    es = [0.02] * 9 + [-0.06] * 3
    check(J.verdict(rows(-0.05, es))["reading"] != "min", "e>0 on 9, median +0.02: not min")
    es = [0.06] * 8 + [-0.06] * 4
    check(J.verdict(rows(-0.05, es))["reading"] != "min", "e>0 on 8: not min")
    ds = [-0.05] * 8 + [0.05] * 4
    check(J.verdict(rows(ds, 0.06))["reading"] != "min", "line 2 red: not min")
    # line 2 red on d, near-last over the line: avoiding the last weights
    v = J.verdict(rows(-0.01, -0.05))
    check(not v["line2"] and v["reading"] == "not-last", "keep misses, near-last passes: not-last")
    check(J.verdict(rows(-0.01, 0.0))["reading"] == "undecided", "neither over the line: undecided")

    # line 1: the mechanism gate and the 16 lines are different reds
    r = rows(-0.05, 0.0)
    r[SEEDS[3]]["keep_min"] = False
    v = J.verdict(r)
    check(not v["line1"] and not v["main"], "one keep-min red: line 1 red")
    check(v["retake"], "keep-min red: the gate or the code is wrong, retake")
    r = rows(-0.05, 0.0)
    r[SEEDS[3]]["fail16"] = 1
    v = J.verdict(r)
    check(not v["line1"] and not v["main"], "one of the 16 lines red: line 1 red")
    check(not v["retake"], "a red among the 16 lines: research, no retake")
    r = rows(-0.05, 0.0)
    r[SEEDS[3]]["lines16"] = 15
    v = J.verdict(r)
    check(not v["line1"] and v["retake"], "15 lines printed: line 1 red, the run is broken")

    r = rows(-0.05, 0.0)
    del r[SEEDS[0]]
    check(not J.verdict(r)["main"], "11 seeds: main red")

    # d on the seeds where both children took the same start
    r = rows(-0.05, 0.0)
    for s in SEEDS[:4]:
        r[s]["start_last"] = "pick"
    v = J.verdict(r)
    check(v["same_start_n"] == 8 and abs(v["same_start_median"] + 0.05) < 1e-9,
          "same-start d: 8 seeds, median -0.05")

    # the logs
    with tempfile.TemporaryDirectory() as t:
        def log(name, child, start="init", km="PASS", nlines=16, s2=True, bad=0):
            with open(os.path.join(t, name), "w") as f:
                f.write("  [sel S1] VAL pick=2.1 init=2.0 -> chose %s | TEST pick=2.0 init=2.0 -> TEST agrees\n" % start)
                f.write("  [T-real S1 sel] TEST A=2.1000 B=2.2000 child=%.4f plain=2.1 (gain vs better 0.1)\n" % child)
                f.write("  %s  [d5c-keep-min] S1: both handed parents' own loss == their own-data min start=sel\n" % km)
                if s2:
                    f.write("  %s  [d5c-keep-min] S2: both handed parents' own loss == their own-data min start=sel\n" % km)
                for i in range(nlines):
                    f.write("  %s  [d5c-floor] line %d start=sel\n" % ("FAIL" if i < bad else "PASS", i))

        for s in (30, 31):
            log("keep-%d.log" % s, 2.0)
            log("last-%d.log" % s, 2.05, start="pick")
            log("near-%d.log" % s, 2.01, km="FAIL", s2=False)
            log("NOKEEP-%d.log" % s, 2.0, km="FAIL")
        r = J.read_logs(t, [30, 31])
        check(r[30]["keep"] == 2.0 and r[30]["last"] == 2.05 and r[30]["near"] == 2.01, "logs: the three children")
        check(r[30]["start_keep"] == "init" and r[30]["start_last"] == "pick", "logs: the starts")
        check(r[30]["keep_min"] and r[30]["lines16"] == 16 and r[30]["fail16"] == 0, "logs: gate and 16 lines")
        check(J.nc_bites(t, "near", [30, 31]), "logs: near red on keep-min")
        check(J.nc_bites(t, "NOKEEP", [30, 31]), "logs: NOKEEP red on keep-min")
        check(not J.nc_bites(t, "keep", [30, 31]), "logs: keep does not count as a bite")
        log("keep-31.log", 2.0, s2=False)
        check(not J.read_logs(t, [30, 31])[31]["keep_min"], "logs: only one keep-min line: red")
        log("keep-31.log", 2.0, km="FAIL")
        check(not J.read_logs(t, [30, 31])[31]["keep_min"], "logs: keep-min FAIL: red")
        log("keep-31.log", 2.0, bad=2)
        check(J.read_logs(t, [30, 31])[31]["fail16"] == 2, "logs: two of the 16 FAIL")
        log("near-31.log", 2.01, km="PASS", s2=False)
        check(not J.nc_bites(t, "near", [30, 31]), "logs: one near green: no bite")
        os.remove(os.path.join(t, "last-31.log"))
        check(31 not in J.read_logs(t, [30, 31]), "logs: a missing log drops the seed")

    print("[d5c9-self] %s" % ("PASS" if fails == 0 else "FAIL (%d)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
