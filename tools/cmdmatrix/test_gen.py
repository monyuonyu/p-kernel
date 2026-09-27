#!/usr/bin/env python3
# tools/cmdmatrix/test_gen.py — fixtures for gen.py's readers (inbox #4 stage 0).
# Each case is a C snippet in one of the dispatch idioms and the first words
# gen.read_src() must return. Run: python3 tools/cmdmatrix/test_gen.py
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen  # noqa: E402

CASES = [
    ("one-line chain",
     "if (cmd[0]=='r' && cmd[1]=='a' && cmd[2]=='f' && cmd[3]=='t') { x(); }",
     {"raft"}),
    ("chain split after &&",   # arch/x86/shell.c sensor/replica/persist/degrade
     "if (cmd[0]=='p' && cmd[1]=='e' && cmd[2]=='r' && cmd[3]=='s' &&\n"
     "    cmd[4]=='i' && cmd[5]=='s' && cmd[6]=='t')\n"
     "    { cmd_persist(); return; }",
     {"persist"}),
    ("chain split before &&",  # arch/x86/shell.c spawn
     "if (cmd[0]=='s' && cmd[1]=='p' && cmd[2]=='a' && cmd[3]=='w' && cmd[4]=='n'\n"
     "    && cmd[5]==' ')\n"
     "    { }",
     {"spawn"}),
    ("terminator on the next line",  # arch/x86/shell.c self/sign
     "if (cmd[0]=='s' && cmd[1]=='e' && cmd[2]=='l' && cmd[3]=='f' &&\n"
     "    (cmd[4]==' ' || cmd[4]=='\\0'))\n"
     "    { cmd_self(cmd + 4); return; }",
     {"self"}),
    ("two chains, two statements",
     "if (cmd[0]=='m' && cmd[1]=='o' && cmd[2]=='e')\n"
     "    { moe_stat(); return; }\n"
     "if (cmd[0]=='d' && cmd[1]=='k' && cmd[2]=='v' && cmd[3]=='a')\n"
     "    { dkva_stat(); return; }",
     {"moe", "dkva"}),
    ("known call idioms",
     'if (starts_with(line, n, "net")) {}\n'
     'if (strneq(line, "ai", 2)) {}\n'
     'if (str_eq(cmd, "help")) {}',
     {"net", "ai", "help"}),
]


def main():
    fails = 0
    for name, src, want in CASES:
        got, unknown = gen.read_src(src, "<fixture>")
        ok = (got == want) and not unknown
        print("[cmdmatrix-test] %-30s %s  got=%s" % (name, "PASS" if ok else "FAIL want=%s" % sorted(want), sorted(got)))
        fails += 0 if ok else 1
    print("[cmdmatrix-test] PASS" if fails == 0 else "[cmdmatrix-test] %d FAIL" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
