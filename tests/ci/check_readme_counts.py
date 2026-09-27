#!/usr/bin/env python3
# tests/ci/check_readme_counts.py — every job count stated in the READMEs must
# equal the number of jobs in .github/workflows/ci.yml:
#   README.ja.md "<N>ジョブ" / README.md "<N> jobs" / README.zh-CN.md "<N> 个任务"
# (2026-09-28: the README became three languages; Japanese is the original.)
#
# Why (2026-09-27, inbox #8): README said "22ジョブ" in three places while
# ci.yml had 45. Nobody noticed for weeks. A job added without touching the
# README now turns this red, so the number is fixed in the same change.
#
# No PyYAML (not guaranteed on the runner): after the top-level `jobs:` line,
# a job is a line indented by exactly two spaces that ends in `:`.
# Exit 1 lists every mismatch.
import re, sys

repo = (sys.argv[1] if len(sys.argv) > 1 else ".").rstrip("/")
wf = open(repo + "/.github/workflows/ci.yml", encoding="utf-8").read().splitlines()
READMES = [("README.ja.md", r"(\d+)\s*ジョブ"),
           ("README.md", r"(\d+)\s+(?:CI\s+)?jobs\b"),
           ("README.zh-CN.md", r"(\d+)\s*个\s*(?:CI\s*)?任务")]

jobs, in_jobs = [], False
for line in wf:
    if re.match(r"^jobs:\s*$", line):
        in_jobs = True
        continue
    if in_jobs and re.match(r"^\S", line) and not line.startswith("#"):
        in_jobs = False          # next top-level key ends the jobs map
    if in_jobs:
        m = re.match(r"^  ([A-Za-z0-9_-]+):\s*(#.*)?$", line)
        if m:
            jobs.append(m.group(1))

if not jobs:
    print("check_readme_counts: FAIL — found no jobs in ci.yml (parser broken?)")
    sys.exit(1)

fail = False
for name, pat in READMES:
    try:
        text = open(repo + "/" + name, encoding="utf-8").read()
    except FileNotFoundError:
        print(f"check_readme_counts: FAIL — {name} is missing"); fail = True; continue
    claims = [(m.start(), int(m.group(1))) for m in re.finditer(pat, text)]
    print(f"check_readme_counts: ci.yml has {len(jobs)} jobs; {name} claims {[n for _, n in claims]}")
    if not claims:
        print(f"  {name}: FAIL — states no job count (expected at least one)"); fail = True
    for pos, n in claims:
        if n != len(jobs):
            print(f"  {name}:{text.count(chr(10), 0, pos) + 1}: says {n}, ci.yml has {len(jobs)}"); fail = True
if fail:
    sys.exit(1)
print("check_readme_counts: PASS")
