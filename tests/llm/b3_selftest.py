#!/usr/bin/env python3
"""b3_selftest.py - [b3-self]: the B3 checks are checked on texts and logs made here.

  F1  a clean text is kept, curly quotes come out as ASCII
  F2  a repeating loop is dropped by q1
  F3  a text that is mostly not printable is dropped by q2
  F4  a text with a 64-byte run of a kept text is dropped by q3
  F5  a near copy with no 64-byte run in common (words changed) is dropped by q4
  F6  a text with a 64-byte run of the held text is dropped by q5; a 16-byte
      run is only counted
  F7  the same input twice gives the same kept texts
  F8  more than 30 % dropped -> exit 1 (RED)
  J1  logs where every line passes -> the judge says ALL PASS
  J2..J8  one thing broken each -> the judge is red on that gate only
          (G1 a skipped window, G2 different line, G3 held64, G4 above 3.56,
           LEAK no difference, TEACHER 94 %, FILTER 31 %)

  b3_selftest.py           exit 0 = ALL PASS
"""
import json
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import b3_filter  # noqa: E402

fails = []

WORDS = ("the a little big old red blue happy sad girl boy dog cat tree house ball river sun moon "
         "ran jumped saw found took gave said looked played walked under over near with from "
         "and but then because when after before into out of to in on at day night friend mother "
         "father garden window door box cake bird fish boat hill road school book song star").split()


def item(ok, gid, what):
    print("[b3-self] %s %s %s" % ("PASS" if ok else "FAIL", gid, what))
    if not ok:
        fails.append(gid)


def story(seed, n=120):
    r = random.Random(seed)
    out, line = [], []
    for i in range(n):
        line.append(r.choice(WORDS))
        if i % 11 == 10:
            out.append(" ".join(line).capitalize() + ".")
            line = []
    return " ".join(out)


def held_text():
    return (story(9001, 400) + "\n").encode()


def filter_tests():
    held = held_text()
    f = b3_filter.Filter([held])
    clean = story(1)
    why, t = f.check("“Hello,” she said — " + clean)
    item(why is None and t.startswith('"Hello," she said - ') and all(" " <= c <= "~" or c == "\n" for c in t),
         "F1", "a clean text is kept and curly quotes become ASCII (%s)" % why)
    why, _ = f.check("The cat sat on the mat. " * 40)
    item(why == "q1", "F2", "a repeating loop is dropped by q1 (%s)" % why)
    why, _ = f.check(story(2) + "éèê一二" * 20)
    item(why == "q2", "F3", "a mostly unprintable text is dropped by q2 (%s)" % why)
    why, _ = f.check(story(3) + " " + clean[100:180] + " " + story(4))
    item(why == "q3", "F4", "a 64-byte run of a kept text is dropped by q3 (%s)" % why)
    words = story(5, 300).split()
    why0, _ = f.check(" ".join(words))
    near = list(words)
    for i in range(0, len(near), 9):
        near[i] = near[i].upper() + "x"
    nb = " ".join(near).encode()
    k = b3_filter.run_keys(nb, 64) & f.kept64
    jac = b3_filter.jaccard(b3_filter.shingles(" ".join(near)), b3_filter.shingles(" ".join(words)))
    why, _ = f.check(" ".join(near))
    item(why0 is None and not k and why == ("q4" if jac >= 0.8 else None),
         "F5", "a near copy with no shared 64-byte run: Jaccard %.3f -> %s" % (jac, why))
    near2 = list(story(6, 300).split())
    f.check(" ".join(near2))
    swapped = [w + ("" if i % 7 else " ") for i, w in enumerate(near2)]  # a double space every 7 words
    s2 = " ".join(swapped)
    k2 = b3_filter.run_keys(s2.encode(), 64) & f.kept64
    j2 = b3_filter.jaccard(b3_filter.shingles(s2), b3_filter.shingles(" ".join(near2)))
    why2, _ = f.check(s2)
    item(not k2 and j2 >= 0.8 and why2 == "q4",
         "F5b", "spacing changed every 7 words: no shared 64-byte run, Jaccard %.3f -> %s" % (j2, why2))
    hs = held.decode()
    why, _ = f.check(story(7) + " " + hs[500:580] + " " + story(8))
    item(why == "q5", "F6", "a 64-byte run of the held text is dropped by q5 (%s)" % why)
    before = f.counts["held16_texts"]
    why, _ = f.check(story(10) + " " + hs[700:720] + " " + story(11))
    item(why is None and f.counts["held16_texts"] == before + 1,
         "F6b", "a 16-byte run of the held text is only counted (%s, %d)" % (why, f.counts["held16_texts"]))

    with tempfile.TemporaryDirectory() as d:
        hp = os.path.join(d, "held.txt")
        with open(hp, "wb") as fh:
            fh.write(held)
        docs = [story(100 + i) for i in range(20)] + [story(100)]  # one exact copy
        ip = os.path.join(d, "in.jsonl")
        with open(ip, "w") as fh:
            for i, t in enumerate(docs):
                fh.write(json.dumps({"id": i, "text": t}) + "\n")
        outs, rcs = [], []
        for run in (1, 2):
            op = os.path.join(d, "kept%d.jsonl" % run)
            p = subprocess.run([sys.executable, os.path.join(HERE, "b3_filter.py"), "--held", hp,
                                "--out", op, ip], capture_output=True, text=True)
            rcs.append(p.returncode)
            with open(op) as fh:
                outs.append(fh.read())
        item(rcs == [0, 0] and outs[0] == outs[1] and outs[0].count("\n") == 20,
             "F7", "the same input twice: same kept texts (rc %s, kept %d)" % (rcs, outs[0].count("\n")))
        with open(ip, "w") as fh:
            for i in range(10):
                fh.write(json.dumps({"id": i, "text": story(200 + i) if i < 6 else "la " * 200}) + "\n")
        p = subprocess.run([sys.executable, os.path.join(HERE, "b3_filter.py"), "--held", hp, ip],
                           capture_output=True, text=True)
        item(p.returncode == 1 and "RED" in p.stdout, "F8", "40 %% dropped -> exit 1 RED (rc %d)" % p.returncode)


US = [24576, 98304, 393216, 1572864]


def write_logs(d, breaks=()):
    def arm(name, tier, seed, mode, us, base, held64=0):
        lines = ["[b3] arm tier=%s seed=%s mode=%s held64=%d held_windows=331" % (
            tier, seed, mode, 1 if "G3" in breaks and name == "M-2" else held64)]
        for i, u in enumerate(us):
            b = base - 0.05 * i
            a, bb = b, b
            if mode == "leak":
                a, bb = b - 0.30, b - 0.10
            if "LEAK" in breaks and mode == "leak":
                a, bb = b - 0.10, b - 0.10
            if "G4" in breaks and name == "M-1" and u == US[-1]:
                b = 3.57
            skip = 1 if "G1" in breaks and name == "S-2" and i == 0 else 0
            lines.append("[b3] point U=%d k=%d bpb=%.4f held_a=%.4f held_b=%.4f gen=%.4f wins=%d skipped=%d" % (
                u, u // 32, b, a, bb, b - 0.4, 331 - skip, skip))
        lines.append("[b3] done")
        with open(os.path.join(d, name + ".log"), "w") as fh:
            fh.write("noise line\n" + "\n".join(lines) + "\n")
    arm("S-1", "S", 1, "plain", US, 3.62)
    arm("S-2", "S", 2, "plain", US, 3.63)
    arm("M-1", "M", 1, "plain", US, 3.64)
    arm("M-2", "M", 2, "plain", US, 3.65)
    arm("S-1-again", "S", 1, "plain", US[:2], 3.62 + (0.0001 if "G2" in breaks else 0.0))
    arm("S-rep", "S", 1, "rep", US, 3.70)
    # the leak arm's point is at U=384 KB (index 2 of the plain arm)
    with open(os.path.join(d, "S-leak.log"), "w") as fh:
        b = 3.62 - 0.10
        a, bb = (b - 0.10, b - 0.10) if "LEAK" in breaks else (b - 0.30, b - 0.10)
        fh.write("[b3] arm tier=S seed=1 mode=leak held64=12 held_windows=331\n"
                 "[b3] point U=393216 k=12288 bpb=%.4f held_a=%.4f held_b=%.4f gen=3.1000 wins=331 skipped=0\n"
                 "[b3] done\n" % (b, a, bb))
    with open(os.path.join(d, "gen-report.txt"), "w") as fh:
        fh.write("[b3-teacher] match=%d of 256\n" % (240 if "TEACHER" in breaks else 250))
        fh.write("[b3-filter] in=1000 kept=%d bytes=1600000 drop q1=1 q2=1 q3=1 q4=1 q5=1 rate=%.4f "
                 "held16_texts=3\n" % ((690, 0.31) if "FILTER" in breaks else (950, 0.05)))


def judge_tests():
    cases = [("J1", (), "ALL PASS")] + [("J%d" % (i + 2), (g,), g) for i, g in
                                        enumerate(["G1", "G2", "G3", "G4", "LEAK", "TEACHER", "FILTER"])]
    for jid, breaks, want in cases:
        with tempfile.TemporaryDirectory() as d:
            write_logs(d, breaks)
            p = subprocess.run([sys.executable, os.path.join(HERE, "b3_curve_check.py"), d],
                               capture_output=True, text=True)
            red = sorted(re.findall(r"^\[b3-data\] FAIL (\S+)", p.stdout, re.M))
            if not breaks:
                ok = p.returncode == 0 and "[b3-data] ALL PASS" in p.stdout and not red
            else:
                ok = p.returncode == 1 and red == [want]
            item(ok, jid, "%s -> rc %d, red %s" % ("nothing broken" if not breaks else "break " + want,
                                                   p.returncode, red))


def main():
    filter_tests()
    judge_tests()
    print("[b3-self] %s" % ("ALL PASS" if not fails else "RED (" + " ".join(fails) + ")"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
