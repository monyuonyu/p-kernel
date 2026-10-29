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
import argparse, os, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))

BASE = dict(Nodes="{0, 1, 2}", Keys="{1}", Vals="{1, 2}", MaxTeach=3,
            MaxDrop=0, MaxEvict=0, MaxRestart=0, WithAnswer="TRUE",
            Mut='"none"', BadNode=2, StampMax=12, DupAdopt="FALSE",
            Script="<<>>")
S3 = {}
S4 = dict(Nodes="{0, 1, 2, 3}", MaxTeach=2)
K2 = dict(Keys="{1, 2}", MaxTeach=3, MaxDrop=1, StampMax=8)
SAFE = "TypeOK P1 P2 P3 P4 P5"
K2SAFE = "TypeOK P2 P3 P4 P5"
FIX = dict(DupAdopt="TRUE")
STALE = "<< <<0, 1, 1>>, <<1, 1, 2>>, <<1, 2, 1>> >>"
M4SC = "<< <<0, 1, 1>>, <<1, 1, 2>>, <<0, 2, 1>> >>"

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
    # 10-29 21:05 run: M4 (no push-back) cannot show with MaxDrop 0 (every
    # node receives every teach, so the push is never needed): keep K2's drop
    # and 3 teaches but let only nodes 0 and 1 teach (node 2 only listens) =
    # a SUBSET of K2's runs: a pass here is weaker than K2's, a red is a real
    # counterexample of K2 too.
    # R-evict-L1 is red but not telling: K2t2-L1-fix (no Evict) breaks L1
    # under the same FairSpec. With fairness on Answer, the model's Answer
    # still serves a node that FORGOT k (held = 0), while the C never asks
    # again for a key it once held (r3_key_ever_held, r3_want_hint 2906).
    # Expected before the run: pass = the model is kinder than the C here.
    "R-evict-L1A-t2": ({**K2, **FIX, "MaxTeach": 2, "MaxEvict": 1}, "TypeOK", "L1A", "pass"),
    "K2s-L1A-fix":({**K2, **FIX, "Teachers": "{0, 1}"}, "TypeOK", "L1A", "pass"),
    "M4s-A":       ({**K2, **FIX, "Teachers": "{0, 1}", "Mut": '"M4"'}, "TypeOK", "L1A", "fail:L1"),
    # 10-30 00:00 run: K2 (2 keys, 1 drop) with the 3 teaches fixed in order
    # (Script), deliveries/drops/re-sends still free. Expectations written
    # before the first run (C: the per-origin dedup is fact_seq != last 5227,
    # a re-send carries the teach's own seq, Republish sends only the last
    # own teach).
    # STALE: n0 teaches k1=v1; n1 teaches k1=v2 (after hearing v1 its stamp
    # is 3 and wins); n1 then teaches k2, so its re-send moves to k2. If the
    # one drop is v2 -> n0, n0 keeps v1 for good: n0's own re-send of v1 is
    # deduplicated everywhere (same seq) before the rule, so nobody pushes
    # back; Answer only serves a node that holds nothing. Expected red even
    # with the fix and fairness on Answer = a liveness hole of the C rule
    # (to be checked in C), not of M4.
    # M4SC: n0 teaches k1=v1; n1 teaches k1=v2; n0 teaches k2. The winner's
    # teacher moves on only when v1 wins (same stamp 1, smaller node), and
    # then a node that missed v1 is healed by n0's push-back (n0 gets n1's
    # losing v2) or by its own revise; if v2 wins (stamp 3) n1 keeps
    # re-sending it. Expected: L1 holds (FairSpec and FairASpec); M4 (no
    # push-back) red.
    "Sc-stale-L1A-fix":    ({**K2, **FIX, "Script": STALE}, "TypeOK", "L1A", "fail:L1"),
    "Sc-stale-L1A":        ({**K2, "Script": STALE}, "TypeOK", "L1A", "fail:L1"),
    "Sc-stale-d0-L1A-fix": ({**K2, **FIX, "Script": STALE, "MaxDrop": 0}, "TypeOK", "L1A", "pass"),
    "Sc-stale-fix-v":      ({**K2, **FIX, "Script": STALE}, K2SAFE.replace("P3", "P3v"), None, "pass"),
    "Sc-m4-L1-fix":        ({**K2, **FIX, "Script": M4SC}, "TypeOK", "L1", "pass"),
    "Sc-m4-L1A-fix":       ({**K2, **FIX, "Script": M4SC}, "TypeOK", "L1A", "pass"),
    "Sc-m4-fix-v":         ({**K2, **FIX, "Script": M4SC}, K2SAFE.replace("P3", "P3v"), None, "pass"),
    "M4sc-A":              ({**K2, **FIX, "Script": M4SC, "Mut": '"M4"'}, "TypeOK", "L1A", "fail:L1"),
}


def cfg_text(over, inv, prop):
    c = {**BASE, **over}
    c.setdefault("Teachers", c["Nodes"])      # everyone teaches unless narrowed
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
    ap.add_argument("--timeout", type=float, default=0,
                    help="seconds per run; 0 = none. A stopped run reports 'timeout'")
    ap.add_argument("--keep-meta", action="store_true",
                    help="keep TLC's state directory (deleted by default)")
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
        # TLC writes straight into <name>.txt (progress lines included), so a
        # run stopped by --timeout or killed from outside still leaves how far
        # it got (the 10-29 19:10 run lost two 60-75 min runs' state counts).
        meta = os.path.join(out, name + ".meta")
        txtp = os.path.join(out, name + ".txt")
        timed_out = False
        with open(txtp, "w") as f:
            p = subprocess.Popen(["java", "-XX:+UseParallelGC", "-cp", a.jar, "tlc2.TLC",
                                  "-workers", a.workers, "-metadir", meta,
                                  "-config", cfg, "MindRule.tla"],
                                 stdout=f, stderr=subprocess.STDOUT, cwd=out)
            try:
                p.wait(timeout=a.timeout or None)
            except subprocess.TimeoutExpired:
                timed_out = True
                p.kill()
                p.wait()
        dt = time.time() - t0
        if not a.keep_meta:
            shutil.rmtree(meta, ignore_errors=True)   # GBs on the big runs
        with open(txtp) as f:
            txt = f.read()
        m = re.search(r"(Invariant|Temporal property|Property) (\w+) is violated", txt) \
            or re.search(r"Error: (Temporal properties were violated)", txt)
        # progress lines write "2,091,552 distinct states found", the final
        # line "56305 distinct states found": take the digits and commas
        st = re.findall(r"([\d,]+) distinct states found", txt)
        states = st[-1].replace(",", "") if st else "?"
        if m:
            what = m.group(2) if m.lastindex and m.lastindex >= 2 else "L1"
            got = "fail:" + what
        elif "No error has been found" in txt:
            got = "pass"
        elif timed_out:
            got = "timeout"       # says nothing either way; states = how far
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
