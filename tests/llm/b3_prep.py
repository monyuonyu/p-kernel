#!/usr/bin/env python3
"""b3_prep.py - turn the kept B3 texts into the files one arm reads (ROADMAP B3).

Texts with id < GEN_HELD_FIRST are the training texts, in id order, each
stripped and followed by a blank line, into train.txt. Texts with id >=
GEN_HELD_FIRST (made by the same teacher, filtered after the training texts so
q3/q4 kept them apart) give gen_held.txt: the first 32 KB, whole 32-byte
windows. The sha256 and sizes are printed for the log (texts are not in git).

  b3_prep.py kept.jsonl train.txt gen_held.txt          exit 0 = both written
"""
import hashlib
import json
import sys

GEN_HELD_FIRST = 1_000_000
GEN_HELD_BYTES = 32768


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    train, gen = [], []
    with open(argv[0]) as f:
        for line in f:
            if line.strip():
                d = json.loads(line)
                (gen if d["id"] >= GEN_HELD_FIRST else train).append((d["id"], d["text"].strip()))
    tb = "".join(t + "\n\n" for _, t in sorted(train)).encode("ascii")
    gb = "".join(t + "\n\n" for _, t in sorted(gen)).encode("ascii")[:GEN_HELD_BYTES]
    if len(gb) < GEN_HELD_BYTES:
        print("[b3-prep] FAIL gen_held is %d bytes, %d needed" % (len(gb), GEN_HELD_BYTES))
        return 1
    for path, b in ((argv[1], tb), (argv[2], gb)):
        with open(path, "wb") as f:
            f.write(b)
        print("[b3-prep] %s bytes=%d sha256=%s" % (path, len(b), hashlib.sha256(b).hexdigest()))
    print("[b3-prep] train texts %d, gen_held texts %d" % (len(train), len(gen)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
