#!/usr/bin/env python3
"""test_build_all_list.py - does tests/ci/build_all.sh build every
`make -C <dir> [-jN] EXTRA_CFLAGS=<-D...>` written in ci.yml?

audit-122: build_all.sh read ci.yml with one regex that wanted EXTRA_CFLAGS
right after the dir, so `make -C boot/linux_x86_64 -j4 EXTRA_CFLAGS=-DN2_NC_TRUNC`
(and WIDEPUB) were never built by hand before a merge. This reads ci.yml a
second, different way (shell tokens, not the regex) and wants every pair it
finds in `build_all.sh --list`. A make line with other words between the dir
and EXTRA_CFLAGS (another make variable, a target) is printed as not covered
(build_all.sh would drop that word) but is not a failure.

  python3 tests/ci/test_build_all_list.py             exit 0 = every pair listed
  python3 tests/ci/test_build_all_list.py --selftest  the reader and the lister on a
                                                      made-up ci.yml
"""
import os
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OPS = {"&&", "||", ";", "|", ">", ">>", "<", "2>", "&"}


def tokens(line):
    try:
        return shlex.split(line, comments=False, posix=True)
    except ValueError:
        return line.split()


def read_pairs(path):
    """(pairs wanted, lines not covered) from a ci.yml, by shell tokens."""
    want, other = [], []
    for raw in open(path, encoding="utf-8"):
        if "make" not in raw or "EXTRA_CFLAGS=" not in raw:
            continue
        t = tokens(raw.strip())
        for i in range(len(t) - 2):
            if t[i] != "make" or t[i + 1] != "-C":
                continue
            d = t[i + 2]
            extra = []
            for w in t[i + 3:]:
                if w in OPS or w.startswith(">") or w.startswith("2>"):
                    break
                if w.startswith("EXTRA_CFLAGS="):
                    flags = w[len("EXTRA_CFLAGS="):]
                    if flags.startswith("-D"):
                        if all(x.startswith("-j") for x in extra):
                            if (d, flags) not in want:
                                want.append((d, flags))
                        else:
                            other.append((d, flags, " ".join(extra)))
                    break
                extra.append(w)
    return want, other


def listed(path):
    env = dict(os.environ, BUILD_ALL_CI=path)
    out = subprocess.run(["bash", os.path.join(HERE, "build_all.sh"), "--list"],
                         env=env, capture_output=True, text=True, check=False)
    pairs = []
    for ln in out.stdout.splitlines():
        if "\t" in ln:
            d, f = ln.split("\t", 1)
            pairs.append((d, f))
    return pairs


def check(path, tag):
    want, other = read_pairs(path)
    got = listed(path)
    missing = [p for p in want if p not in got]
    for d, f, x in other:
        print(f"[{tag}] not covered (other words '{x}'): make -C {d} EXTRA_CFLAGS={f}")
    for d, f in missing:
        print(f"[{tag}] FAIL build_all.sh does not build: make -C {d} EXTRA_CFLAGS={f}")
    print(f"[{tag}] {len(want)} pairs in ci.yml, {len(got)} listed, {len(missing)} missing")
    return missing


def selftest():
    yml = (
        "      - run: make -C a EXTRA_CFLAGS=-DA1\n"
        "      - run: make -C b -j4 EXTRA_CFLAGS=-DB1 >/dev/null\n"
        "        run: |\n"
        "          make -C c -j EXTRA_CFLAGS='-DC1 -DC2' && echo ok\n"
        "          make -C d EXTRA_CFLAGS=-O2\n"
        "          make -C e V=1 EXTRA_CFLAGS=-DE1\n"
    )
    with tempfile.NamedTemporaryFile("w", suffix=".yml", delete=False) as f:
        f.write(yml)
        p = f.name
    try:
        want, other = read_pairs(p)
        ok = True
        exp = [("a", "-DA1"), ("b", "-DB1"), ("c", "-DC1 -DC2")]
        if want != exp:
            print(f"[build-all-list-selftest] FAIL reader {want} != {exp}")
            ok = False
        if [(d, f) for d, f, _ in other] != [("e", "-DE1")]:
            print(f"[build-all-list-selftest] FAIL other {other}")
            ok = False
        if check(p, "build-all-list-selftest"):
            ok = False
    finally:
        os.unlink(p)
    print("[build-all-list-selftest] " + ("ALL PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    missing = check(os.path.join(ROOT, ".github", "workflows", "ci.yml"), "build-all-list")
    print("[build-all-list] " + ("PASS" if not missing else "FAIL"))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
