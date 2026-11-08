#!/usr/bin/env python3
"""tests/ci/check_llm_falsifiers.py [--root DIR] [--list]

Compile (syntax only) every C source under arch/common/llm/ and tests/llm/
once per falsifier macro that the tests/llm/*.sh scripts pass with -D and that
the source mentions, with the scripts' flags plus
-Werror=implicit-function-declaration.

Why: build_all.sh does not build the falsifiers that test scripts make
themselves. CI 42445849552 went red because -DST_MERGE_NO_FINITE (run_b5_rounds.sh
and run_st_nan.sh) left st_blob_w_finite unbuilt while B5 step 7's st_prev_ok
still called it: an implicit declaration, then a link error. No one had built
that falsifier by hand since step 7.

What it catches: a call or name that a macro removes but another part still
uses (implicit declaration, undeclared identifier, any compile error).
What it does not: link errors of non-static functions that stay declared in a
header, macros combined with each other (each macro is built alone, as well as
the plain build), and anything at run time.

Exit 0 = every build compiled; 1 = at least one broke (the lines say which).
"""
import os
import re
import subprocess
import sys

CFLAGS = ["-std=c11", "-O1", "-Wall", "-Wextra", "-ffp-contract=off", "-Werror=vla",
          "-Werror=implicit-function-declaration", "-fsyntax-only"]
MAX_ALONE = 7   # 2026-11-08: dmoe_bank.c dmoe_test.c (pk_node.h), llm_shell.c (clock_gettime
                # without _POSIX_C_SOURCE), oracle_llama.c oracle_tok.c (llama.h), placement_stub.c
                # placement_test.c (region.h, placement.h)
MACRO = re.compile(r'(?:^|[\s"\'])-D([A-Z][A-Z0-9_]*)(=[^\s"\']*)?')


def macros(root):
    out = {}
    d = os.path.join(root, "tests", "llm")
    for f in sorted(os.listdir(d)):
        if not f.endswith(".sh"):
            continue
        with open(os.path.join(d, f), encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.lstrip().startswith("#"):
                    continue
                for m in MACRO.finditer(line):
                    if "$" in (m.group(2) or "") or m.group(2) == "=":
                        continue   # the value comes from a shell variable: not known here
                    out.setdefault((m.group(1), m.group(2) or ""), set()).add(f)
    return out


def sources(root):
    out = []
    for sub in (("arch", "common", "llm"), ("tests", "llm")):
        d = os.path.join(root, *sub)
        for f in sorted(os.listdir(d)):
            if f.endswith(".c"):
                p = os.path.join(d, f)
                with open(p, encoding="utf-8", errors="replace") as fh:
                    out.append((p, fh.read()))
    return out


def builds(root):
    """-> [(source path, macro, value)], plain first"""
    srcs = sources(root)
    res = [(p, "", "") for p, _ in srcs]
    for (name, val), _ in sorted(macros(root).items()):
        word = re.compile(r"\b%s\b" % re.escape(name))
        for p, text in srcs:
            if word.search(text):
                res.append((p, name, val))
    return res


def compile_one(root, path, name, val):
    cmd = ["cc"] + CFLAGS + ["-I", os.path.join(root, "arch", "common", "llm")] \
        + (["-D" + name + val] if name else []) + [path]
    r = subprocess.run(cmd, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True)
    return r.returncode, r.stdout


def main(argv):
    root = os.getcwd()
    lst = False
    i = 1
    while i < len(argv):
        if argv[i] == "--root":
            root = argv[i + 1]
            i += 2
        elif argv[i] == "--list":
            lst = True
            i += 1
        else:
            print(__doc__)
            return 2
    bl = builds(root)
    if lst:
        for p, n, v in bl:
            print("%s\t%s" % (os.path.relpath(p, root), ("-D" + n + v) if n else "(plain)"))
        return 0
    bad = skip = 0
    alone = set()      # sources that do not compile alone even plain (kernel or llama headers): not checked
    for p, n, v in bl:
        if p in alone:
            continue
        rc, out = compile_one(root, p, n, v)
        if rc != 0 and not n:
            alone.add(p)
            continue
        if rc != 0 and "#error" in out:
            skip += 1   # the source says this macro needs another one: a combination, not built alone
            continue
        if rc != 0:
            bad += 1
            first = [l for l in out.splitlines() if "error" in l][:2]
            print("[llm-fals] BROKEN %s %s: %s" % (os.path.relpath(p, root),
                                                  ("-D" + n + v) if n else "(plain)", " | ".join(first)))
    for p in sorted(alone):
        print("[llm-fals] not checked (does not compile alone even plain): %s" % os.path.relpath(p, root))
    print("[llm-fals] %d builds, %d broken, %d skipped by the source's own #error, %d sources not checked"
          " (%d macros from tests/llm/*.sh)" % (len(bl), bad, skip, len(alone), len(macros(root))))
    if len(alone) > MAX_ALONE:
        bad += 1
        print("[llm-fals] BROKEN more than %d sources do not compile alone (a header moved?)" % MAX_ALONE)
    print("[result] %s llm-falsifier-compile" % ("PASS" if bad == 0 else "FAIL"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
