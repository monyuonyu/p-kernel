#!/usr/bin/env python3
# tests/ci/test_check_readme_counts.py — fixtures for check_readme_counts.py.
#
# Each case is a throw-away repo (a 3-job ci.yml + a README) and the exit code
# the checker must give. The case that motivated this file: "UMP x86_64 ジョブ"
# was read as a claim of 64 jobs (2026-09-27, inbox #8 README rewrite).
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "check_readme_counts.py")
CI = "on: push\njobs:\n  a:\n    runs-on: x\n  b:\n    runs-on: x\n  c:\n    runs-on: x\n"

CASES = [
    ("right count", "CI（3ジョブ）", 0),
    ("wrong count", "CI（4ジョブ）", 1),
    ("bold and spaced", "**3 ジョブ** と CI 3ジョブ", 0),
    ("one of two wrong", "3ジョブ と 5ジョブ", 1),
    ("no count at all", "CI があります", 1),
    ("a job NAME ending in digits is not a count", "CI 3ジョブ。`ump-x86_64` と UMP x86_64 ジョブ", 0),
    ("digits after a dot or letter are not a count", "CI 3ジョブ。v2.64 ジョブ・arm64ジョブ", 0),
]

fails = 0
for name, readme, want in CASES:
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, ".github", "workflows"))
        open(os.path.join(d, ".github", "workflows", "ci.yml"), "w", encoding="utf-8").write(CI)
        open(os.path.join(d, "README.md"), "w", encoding="utf-8").write(readme + "\n")
        rc = subprocess.run([sys.executable, CHECK, d], capture_output=True, text=True).returncode
    ok = rc == want
    fails += not ok
    print(("PASS" if ok else "FAIL") + f" {name}: rc={rc} want={want}")
print(f"test_check_readme_counts: {len(CASES) - fails} PASS / {fails} FAIL")
sys.exit(1 if fails else 0)
