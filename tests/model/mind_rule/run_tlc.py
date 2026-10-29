#!/usr/bin/env python3
"""ROADMAP 1-3m: run TLC on MindRule.tla for each setting of the design
(docs/architecture/30-module/research/mind-rule-tla.md) and print one line
per run: name, verdict, the property TLC names, distinct states, seconds.

  python3 tests/model/mind_rule/run_tlc.py --jar tla2tools.jar [--out DIR] [NAME ...]

NAME picks runs from RUNS (default: all). Each run gets its own cfg and
metadir under DIR (default: a temp dir); the full TLC output is DIR/NAME.txt
(the counterexample, if any). Not in CI (Java; #37 5). 'expect' is what the
design fixed before the model existed; the last column says if it matched.
"""
import argparse, os, re, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))

BASE = dict(Nodes="{0, 1, 2}", Keys="{1}", Vals="{1, 2}", MaxTeach=3,
            MaxDrop=0, MaxEvict=0, MaxRestart=0, WithAnswer="TRUE",
            Mut='"none"', BadNode=2, StampMax=12, DupAdopt="FALSE")
S3 = {}
S4 = dict(Nodes="{0, 1, 2, 3}", MaxTeach=2)
K2 = dict(Keys="{1, 2}", MaxTeach=3, MaxDrop=1, StampMax=8)
SAFE = "TypeOK P1 P2 P3 P4 P5"
K2SAFE = "TypeOK P2 P3 P4 P5"
FIX = dict(DupAdopt="TRUE")

# name: (overrides, invariants, temporal property or None, expect)
#   expect: "pass", or "fail:<property>" (the design's line or reading)
RUNS = {
    # the C today (DupAdopt FALSE): the line of the design, for the record
    "S3":        (S3, SAFE, None, "pass"),
    "S4":        (S4, SAFE, None, "pass"),
    "K2":        (K2, K2SAFE, None, "pass"),
    "K2-L1":     (K2, "TypeOK", "L1", "pass"),
    # the candidate fix (5159 adopts a better stamp of the same value)
    "S3-fix":    ({**S3, **FIX}, SAFE, None, "pass"),
    "S4-fix":    ({**S4, **FIX}, SAFE, None, "pass"),
    "K2-fix":    ({**K2, **FIX}, K2SAFE, None, "pass"),
    # P3 at the value level (P3v) in place of P3, added after S3-fix's P3
    # counterexample (an equal-value teach is absorbed): the design's line
    # stays the runs above
    "S3-fix-v":  ({**S3, **FIX}, SAFE.replace("P3", "P3v"), None, "pass"),
    "K2-fix-v":  ({**K2, **FIX}, K2SAFE.replace("P3", "P3v"), None, "pass"),
    "S3-v":      (S3, "TypeOK P2 P3v P4 P5", None, "pass"),
    "K2-L1-fix": ({**K2, **FIX}, "TypeOK", "L1", "pass"),
    # break-its on the fixed model: each must give a counterexample
    "M1": ({**S3, **FIX, "Mut": '"M1"'}, "TypeOK P5", None, "fail:P5"),
    "M2": ({**S3, **FIX, "Mut": '"M2"'}, "TypeOK P1 P2", None, "fail:P1|P2"),
    "M3": ({**S3, **FIX, "Mut": '"M3"'}, "TypeOK P1 P2", None, "fail:P1|P2"),
    "M4": ({**K2, **FIX, "Mut": '"M4"'}, "TypeOK", "L1", "fail:L1"),
    # readings (not gates), on the fixed model
    "R-evict-P4":   ({**S3, **FIX, "MaxEvict": 1}, "TypeOK P4", None, "fail:P4"),
    "R-restart-P5": ({**S3, **FIX, "MaxRestart": 1}, "TypeOK P5", None, "fail:P5"),
    "R-evict-P1":   ({**S3, **FIX, "MaxEvict": 1}, "TypeOK P1", None, "fail:P1"),
    "R-evict-L1":   ({**K2, **FIX, "MaxEvict": 1}, "TypeOK", "L1", "fail:L1"),
    # after the design (10-29 19:10 run; the line above stays the gate):
    # K2 with no drop (safety at the design's 3 teaches; the drop is what
    # made K2-fix-v pass 60 min), K2 with 2 teaches (the design's "one fewer"),
    # and L1 with weak fairness on Answer (FairASpec, a reading: the C pulls a
    # key it never held, see MindRule.tla). M4 must stay red with it.
    "K2-d0-fix-v": ({**K2, **FIX, "MaxDrop": 0}, K2SAFE.replace("P3", "P3v"), None, "pass"),
    "K2-d0":       ({**K2, "MaxDrop": 0}, K2SAFE, None, "fail:P2"),  # S3's path
    "K2t2-fix-v":  ({**K2, **FIX, "MaxTeach": 2}, K2SAFE.replace("P3", "P3v"), None, "pass"),
    "K2t2-L1-fix": ({**K2, **FIX, "MaxTeach": 2}, "TypeOK", "L1", "fail:L1"),
    "K2t2-L1A-fix": ({**K2, **FIX, "MaxTeach": 2}, "TypeOK", "L1A", "pass"),
    "K2t2-L1A":    ({**K2, "MaxTeach": 2}, "TypeOK", "L1A", "pass"),
    "M4t2-A":      ({**K2, **FIX, "MaxTeach": 2, "Mut": '"M4"'}, "TypeOK", "L1A", "fail:L1"),
    # 2 teaches cannot show M4 (a conflict and the winner's next key need 3)
    "K2-d0-L1A-fix": ({**K2, **FIX, "MaxDrop": 0}, "TypeOK", "L1A", "pass"),
    "M4-d0-A":     ({**K2, **FIX, "MaxDrop": 0, "Mut": '"M4"'}, "TypeOK", "L1A", "fail:L1"),
    "K2-L1A-fix":  ({**K2, **FIX}, "TypeOK", "L1A", "pass"),
    "M4-A":        ({**K2, **FIX, "Mut": '"M4"'}, "TypeOK", "L1A", "fail:L1"),
}


def cfg_text(over, inv, prop):
    c = {**BASE, **over}
    lines = ["CONSTANTS"] + [f"  {k} = {v}" for k, v in c.items()]
    # "L1A" = L1 under FairASpec (fairness on Answer too)
    spec = "FairASpec" if prop == "L1A" else "FairSpec" if prop else "Spec"
    lines.append("SPECIFICATION " + spec)
    lines.append("INVARIANTS " + inv)
    if prop:
        lines.append("PROPERTIES L1")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jar", required=True)
    ap.add_argument("--out")
    ap.add_argument("--workers", default="2")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    out = a.out or tempfile.mkdtemp(prefix="tlc-mindrule-")
    os.makedirs(out, exist_ok=True)
    names = a.names or list(RUNS)
    # TLC looks for the cfg beside the spec: run a copy of the spec in DIR
    with open(os.path.join(HERE, "MindRule.tla")) as f, \
         open(os.path.join(out, "MindRule.tla"), "w") as g:
        g.write(f.read())
    bad = 0
    for name in names:
        over, inv, prop, expect = RUNS[name]
        cfg = os.path.join(out, name + ".cfg")
        with open(cfg, "w") as f:
            f.write(cfg_text(over, inv, prop))
        t0 = time.time()
        r = subprocess.run(["java", "-XX:+UseParallelGC", "-cp", a.jar, "tlc2.TLC",
                            "-workers", a.workers, "-metadir", os.path.join(out, name + ".meta"),
                            "-config", cfg, "MindRule.tla"],
                           capture_output=True, text=True, cwd=out)
        dt = time.time() - t0
        txt = r.stdout + r.stderr
        with open(os.path.join(out, name + ".txt"), "w") as f:
            f.write(txt)
        m = re.search(r"(Invariant|Temporal property|Property) (\w+) is violated", txt) \
            or re.search(r"Error: (Temporal properties were violated)", txt)
        st = re.findall(r"(\d+) distinct states found", txt)
        states = st[-1] if st else "?"
        if m:
            what = m.group(2) if m.lastindex and m.lastindex >= 2 else "L1"
            got = "fail:" + what
        elif "No error has been found" in txt:
            got = "pass"
        else:
            got = "error"
        ok = got == expect or (expect.startswith("fail:") and got.startswith("fail:")
                               and got[5:] in expect[5:].split("|"))
        bad += 0 if ok else 1
        print(f"[tlc] {name:<13} {got:<10} states {states:>9}  {dt:6.0f} s  "
              f"expect {expect:<11} {'as-expected' if ok else 'NOT-AS-EXPECTED'}", flush=True)
    print(f"[tlc] out {out}  not-as-expected {bad}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
