"""b5_outer_check.py - the judge of B5 step 11 (research/b5-outer.md).

Reads <build>-<seed>.log written by run_b5_outer.sh (student_outer_test.c).
Each arm prints one line

  [b5o] arm=<name> last=<bpb> kept=<bpb> at <pass> (gate <nats>) ...

kept is node 0's model at its best pass on the gate window (step 4's stop);
the ruler (b1_held.txt) never chooses. Four nodes, each a quarter, the
product's round (st_round_trimmed t = 1 into a copy, st_round_adopt). Lines
fixed in the design (section 4) before the code and before any run on the
judgement seeds 311..318 (the probe saw only 301 302 with a scratch shim):

  O1  outer4 <= trim4 - D1                       on >= NEED seeds   (the outer step helps; both at
                                                 EVERY 1 = 32 outer steps, the same communication)
  O2  outer4 <= alone-A                          on every seed      (the swarm still beats one node)
  O3  outer4-poison <= alone-A + D3              on every seed      (one garbage node: the gate holds)
  NC1 the build -DB5O_NC_PLAIN (lr 1 mu 0 = the plain merge) turns O1 red
  NC2 the build -DB5O_NC_FLIP (the outer gradient's sign flipped) turns O1 red
  P   (the product rule, printed, not the exit code) outer4 at EVERY 1 <= min(trim4 at
      EVERY 2, trim4 at EVERY 4) on >= NEED seeds: only then st_round_outer goes into
      st_region_round (with O1-O3 PASS); else it stays a hosted tool
  reading only (no line): look1 - alone-A (the momentum without a swarm), outer4 - trim4 at EVERY 2 and 4

  b5_outer_check.py [--seeds "311 ... 318"] <dir>   exit 0 = O1-O3 green, NC1 NC2 red
  b5_outer_check.py --self-test
"""
import os
import re
import shutil
import sys
import tempfile

D1 = 0.02      # O1: fixed in the design from the seen seeds 301 302 (gains 0.092 0.029), before the code
D3 = 0.02      # O3: step 5's T4 line
NEED = 6
SEEDS = " ".join(str(s) for s in range(311, 319))
ARM = re.compile(r"^\[b5o\] arm=(\S+) last=([0-9.]+) kept=([0-9.]+) at (\d+) \(gate ([0-9.]+)\)")
NEEDED = {
    "plain": ["alone-A", "trim4", "outer4", "outer4-poison", "look1"],
    "ncplain": ["trim4", "outer4"],
    "ncflip": ["trim4", "outer4"],
    "plain2": ["trim4", "outer4"],
    "plain4": ["trim4", "outer4"],
}


def read(path, needed):
    """{arm: kept} of a finished log, or None (missing, short, an arm twice)"""
    if not os.path.exists(path):
        return None
    arms, done = {}, False
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        m = ARM.match(line)
        if m:
            if m.group(1) in arms:
                return None
            arms[m.group(1)] = float(m.group(3))
        elif line == "[b5o] done":
            done = True
    if not done or any(a not in arms for a in needed):
        return None
    return arms


def o1(a):
    return a["outer4"] <= a["trim4"] - D1 + 1e-9


def judge(d, seeds, out=print):
    """0 = O1-O3 green and NC1 NC2 red; 1 otherwise (missing logs are red)"""
    logs = {}
    for b, need in NEEDED.items():
        for s in seeds:
            r = read(os.path.join(d, "%s-%s.log" % (b, s)), need)
            if r is None:
                out("[b5o-judge] FAIL missing or short log %s-%s.log" % (b, s))
                return 1
            logs[(b, s)] = r
    n1 = n2 = n3 = 0
    for s in seeds:
        a = logs[("plain", s)]
        g1, g2 = o1(a), a["outer4"] <= a["alone-A"] + 1e-9
        g3 = a["outer4-poison"] <= a["alone-A"] + D3 + 1e-9
        n1 += g1; n2 += g2; n3 += g3
        out("[b5o-judge] seed %s outer4-trim4 %+.4f O1 %s | outer4-alone %+.4f O2 %s | poison-alone %+.4f O3 %s | read look1-alone %+.4f"
            % (s, a["outer4"] - a["trim4"], "ok" if g1 else "RED", a["outer4"] - a["alone-A"], "ok" if g2 else "RED",
               a["outer4-poison"] - a["alone-A"], "ok" if g3 else "RED", a["look1"] - a["alone-A"]))
    k = len(seeds)
    ok1, ok2, ok3 = n1 >= min(NEED, k), n2 == k, n3 == k
    out("[b5o-judge] O1 %d/%d (need %d) %s" % (n1, k, min(NEED, k), "PASS" if ok1 else "RED"))
    out("[b5o-judge] O2 %d/%d %s" % (n2, k, "PASS" if ok2 else "RED"))
    out("[b5o-judge] O3 %d/%d %s" % (n3, k, "PASS" if ok3 else "RED"))
    ncs = []
    for b in ("ncplain", "ncflip"):
        m = sum(o1(logs[(b, s)]) for s in seeds)
        red = m < min(NEED, k)
        out("[b5o-judge] NC %s: O1 %d/%d -> %s" % (b, m, k, "red (as it must)" if red else "GREEN (the line does not see it)"))
        ncs.append(red)
    np_ = 0
    for s in seeds:
        a, b2, b4 = logs[("plain", s)], logs[("plain2", s)], logs[("plain4", s)]
        best = min(b2["trim4"], b4["trim4"])
        np_ += a["outer4"] <= best + 1e-9
        out("[b5o-judge] seed %s P outer4@1 %.4f best plain %.4f (%+.4f) | read outer4-trim4 @2 %+.4f @4 %+.4f"
            % (s, a["outer4"], best, a["outer4"] - best, b2["outer4"] - b2["trim4"], b4["outer4"] - b4["trim4"]))
    out("[b5o-judge] P %d/%d (need %d) %s" % (np_, k, min(NEED, k),
        "MET: wire st_round_outer into st_region_round" if np_ >= min(NEED, k) else "not met: keep it a hosted tool"))
    good = ok1 and ok2 and ok3 and all(ncs)
    out("[result] %s  b5-outer" % ("PASS" if good else "FAIL"))
    return 0 if good else 1


def self_test():
    """synthetic logs: the judge must say what each case says"""
    def write(d, b, s, arms):
        with open(os.path.join(d, "%s-%s.log" % (b, s)), "w") as f:
            for name, kept in arms.items():
                f.write("[b5o] arm=%s last=%.4f kept=%.4f at 9 (gate 2.0000) took=1/8\n" % (name, kept, kept))
            f.write("[b5o] done\n")

    def case(name, plain, ncp, ncf, want, seeds=("1", "2")):
        d = tempfile.mkdtemp()
        try:
            for s in seeds:
                write(d, "plain", s, plain)
                write(d, "ncplain", s, ncp)
                write(d, "ncflip", s, ncf)
                write(d, "plain2", s, {"trim4": 2.45, "outer4": 2.44})
                write(d, "plain4", s, {"trim4": 2.5, "outer4": 2.6})
            got = judge(d, list(seeds), out=lambda *_: None)
            print("  %s %s (want %d got %d)" % ("ok" if got == want else "BAD", name, want, got))
            return got == want
        finally:
            shutil.rmtree(d)

    good = {"alone-A": 3.0, "trim4": 2.5, "outer4": 2.4, "outer4-poison": 3.01, "look1": 2.9}
    ncp = {"trim4": 2.5, "outer4": 2.5}
    ncf = {"trim4": 2.5, "outer4": 2.9}
    r = [case("all green", good, ncp, ncf, 0)]
    r.append(case("O1 red: gain below the line", dict(good, outer4=2.49), ncp, ncf, 1))
    r.append(case("O2 red: worse than one node", dict(good, outer4=3.1, trim4=3.2), ncp, ncf, 1))
    r.append(case("O3 red: the poison gets in", dict(good, **{"outer4-poison": 3.03}), ncp, ncf, 1))
    r.append(case("NC1 green = the line is blind", good, {"trim4": 2.5, "outer4": 2.4}, ncf, 1))
    r.append(case("NC2 green = the line is blind", good, ncp, {"trim4": 2.5, "outer4": 2.4}, 1))
    d = tempfile.mkdtemp()
    try:
        write(d, "plain", "1", {"alone-A": 3.0})
        got = judge(d, ["1"], out=lambda *_: None)
        print("  %s %s (want 1 got %d)" % ("ok" if got == 1 else "BAD", "a short log is red", got))
        r.append(got == 1)
    finally:
        shutil.rmtree(d)
    return 0 if all(r) else 1


def main(argv):
    if len(argv) > 1 and argv[1] == "--self-test":
        return self_test()
    seeds = SEEDS.split()
    rest = argv[1:]
    if len(rest) >= 2 and rest[0] == "--seeds":
        seeds = rest[1].split()
        rest = rest[2:]
    if len(rest) != 1:
        print(__doc__)
        return 2
    return judge(rest[0], seeds)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
