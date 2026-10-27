#!/usr/bin/env python3
"""b3_filter.py - the quality, duplicate and separation checks of B3 (ROADMAP B3).

The checks and lines were fixed before any text was made, in
docs/architecture/30-module/research/b3-data.md section 3:

  norm  curly quotes, dashes and the like become ASCII (b1_held is all ASCII)
  q1    zlib compressed size / size < 0.30          -> drop (a repeating loop)
  q2    printable characters and newlines < 98 %    -> drop
  q3    a 64-byte run equal to one in a kept text    -> drop the text
  q4    word 5-gram Jaccard >= 0.8 with a kept text  -> drop (all pairs)
  q5    a 64-byte run shared with b1_held / b1_cal   -> drop; 16-byte runs are
        only counted (plain English shares them)
  more than 30 % dropped -> the way the text is made is wrong: RED, exit 1

A text is checked in the order q1..q5 and dropped for the first one it hits;
q3 and q4 compare with the texts kept before it, so the result depends only on
the input order. Kept texts are written one per line of JSON, same as input.

  b3_filter.py --held F [--held F ...] [--out kept.jsonl] [--rejects r.tsv] in.jsonl
  input lines: {"id": <int>, "text": <str>}                exit 0 = rate <= 30 %
"""
import hashlib
import json
import sys
import zlib

Q1_RATIO = 0.30
Q2_PRINTABLE = 0.98
RUN = 64
SMALL_RUN = 16
Q4_N = 5
Q4_JACCARD = 0.8
MAX_DROP = 0.30

ASCII_MAP = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'", "′": "'",
    "“": '"', "”": '"', "„": '"', "‟": '"', "″": '"',
    "–": "-", "—": "-", "―": "-", "−": "-", "‐": "-", "‑": "-",
    "…": "...", " ": " ", " ": " ", " ": " ", " ": " ",
    "´": "'", "`": "'",
}


def normalize(text):
    """Map typographic characters to ASCII; other characters are left as they are."""
    return "".join(ASCII_MAP.get(c, c) for c in text).replace("\r\n", "\n").replace("\r", "\n")


def printable_share(text):
    if not text:
        return 0.0
    good = sum(1 for c in text if c == "\n" or (" " <= c <= "~"))
    return good / len(text)


def strip_unprintable(text):
    return "".join(c for c in text if c == "\n" or (" " <= c <= "~"))


def zlib_ratio(b):
    return len(zlib.compress(b, 9)) / len(b) if b else 0.0


def run_keys(b, n):
    """The set of n-byte runs of b, as 8-byte digests (deterministic across runs)."""
    return {hashlib.blake2b(b[i:i + n], digest_size=8).digest() for i in range(len(b) - n + 1)}


def shingles(text, n=Q4_N):
    w = text.split()
    return {tuple(w[i:i + n]) for i in range(len(w) - n + 1)}


def jaccard(a, b):
    if not a or not b:
        return 0.0
    return len(a & b) / len(a | b)


class Filter:
    def __init__(self, held_texts):
        self.held64 = set()
        self.held16 = set()
        for h in held_texts:
            self.held64 |= run_keys(h, RUN)
            self.held16 |= run_keys(h, SMALL_RUN)
        self.kept64 = set()
        self.kept_sh = []
        self.counts = {"in": 0, "kept": 0, "q1": 0, "q2": 0, "q3": 0, "q4": 0, "q5": 0,
                       "held16_texts": 0, "bytes_kept": 0}

    def check(self, text):
        """STUB (test first): keeps every text as it is."""
        self.counts["in"] += 1
        self.counts["kept"] += 1
        return None, text

    def _drop(self, why, t):
        self.counts[why] += 1
        return why, t

    def rate(self):
        n = self.counts["in"]
        return (n - self.counts["kept"]) / n if n else 0.0

    def report(self):
        c = self.counts
        return ("[b3-filter] in=%d kept=%d bytes=%d drop q1=%d q2=%d q3=%d q4=%d q5=%d "
                "rate=%.4f held16_texts=%d" % (c["in"], c["kept"], c["bytes_kept"], c["q1"], c["q2"],
                                               c["q3"], c["q4"], c["q5"], self.rate(), c["held16_texts"]))


def main(argv):
    held, out, rej, inp = [], None, None, None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--held", "--out", "--rejects") and i + 1 < len(argv):
            v = argv[i + 1]
            if a == "--held":
                with open(v, "rb") as f:
                    held.append(f.read())
            elif a == "--out":
                out = v
            else:
                rej = v
            i += 2
        elif inp is None and not a.startswith("--"):
            inp, i = a, i + 1
        else:
            print(__doc__)
            return 2
    if inp is None or not held:
        print(__doc__)
        return 2
    flt = Filter(held)
    fo = open(out, "w") if out else None
    fr = open(rej, "w") if rej else None
    with open(inp) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            why, t = flt.check(d["text"])
            if why is None:
                if fo:
                    fo.write(json.dumps({"id": d["id"], "text": t}) + "\n")
            elif fr:
                fr.write("%s\t%s\t%s\n" % (d["id"], why, t[:60].replace("\t", " ").replace("\n", " ")))
    for fh in (fo, fr):
        if fh:
            fh.close()
    print(flt.report())
    if flt.rate() > MAX_DROP:
        print("[b3-filter] RED dropped %.1f %% > %d %%: the way the text is made is wrong" % (
            100 * flt.rate(), int(100 * MAX_DROP)))
        return 1
    print("[b3-filter] OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
