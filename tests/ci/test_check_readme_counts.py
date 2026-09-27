#!/usr/bin/env python3
# tests/ci/test_check_readme_counts.py — fixtures for check_readme_counts.py.
#
# Each case is a throw-away repo (a 3-job ci.yml + the three READMEs) and the
# exit code the checker must give. A case replaces one README; the other two
# keep a right count. The case that motivated this file: "UMP x86_64 ジョブ"
# was read as a claim of 64 jobs (2026-09-27, inbox #8 README rewrite). Since
# 2026-09-28 the README is three languages, and the same hole was in all three
# patterns (audit-16).
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "check_readme_counts.py")
CI = "on: push\njobs:\n  a:\n    runs-on: x\n  b:\n    runs-on: x\n  c:\n    runs-on: x\n"
RIGHT = {"README.ja.md": "CI（3ジョブ）", "README.md": "CI (3 jobs)", "README.zh-CN.md": "CI（3 个任务）"}
JA, EN, ZH = "README.ja.md", "README.md", "README.zh-CN.md"

CASES = [
    ("all three right", None, None, 0),
    ("ja wrong count", JA, "CI（4ジョブ）", 1),
    ("en wrong count", EN, "CI (4 jobs)", 1),
    ("zh wrong count", ZH, "CI（4 个任务）", 1),
    ("ja bold and spaced", JA, "**3 ジョブ** と CI 3ジョブ", 0),
    ("en 'CI jobs'", EN, "3 CI jobs", 0),
    ("ja one of two wrong", JA, "3ジョブ と 5ジョブ", 1),
    ("ja no count at all", JA, "CI があります", 1),
    ("ja a job NAME ending in digits is not a count", JA, "CI 3ジョブ。`ump-x86_64` と UMP x86_64 ジョブ", 0),
    ("ja digits after a dot or letter are not a count", JA, "CI 3ジョブ。v2.64 ジョブ・arm64ジョブ", 0),
    ("en a job NAME ending in digits is not a count", EN, "CI 3 jobs. The UMP x86_64 jobs and arm64 jobs", 0),
    ("zh a job NAME ending in digits is not a count", ZH, "CI 3 个任务。x86_64 个任务", 0),
    ("en a name digit does not hide a wrong count", EN, "CI 4 jobs. The x86_64 jobs", 1),
]

fails = 0
for name, which, text, want in CASES:
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, ".github", "workflows"))
        open(os.path.join(d, ".github", "workflows", "ci.yml"), "w", encoding="utf-8").write(CI)
        for f, body in RIGHT.items():
            open(os.path.join(d, f), "w", encoding="utf-8").write((text if f == which else body) + "\n")
        rc = subprocess.run([sys.executable, CHECK, d], capture_output=True, text=True).returncode
    ok = rc == want
    fails += not ok
    print(("PASS" if ok else "FAIL") + f" {name}: rc={rc} want={want}")
print(f"test_check_readme_counts: {len(CASES) - fails} PASS / {fails} FAIL")
sys.exit(1 if fails else 0)
