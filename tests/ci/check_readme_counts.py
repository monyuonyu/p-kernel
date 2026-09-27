#!/usr/bin/env python3
# tests/ci/check_readme_counts.py — every "<N>ジョブ" in README.md must equal
# the number of jobs in .github/workflows/ci.yml.
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
readme = open(repo + "/README.md", encoding="utf-8").read()

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

claims = [(m.start(), int(m.group(1))) for m in re.finditer(r"(\d+)\s*ジョブ", readme)]
bad = [(readme.count("\n", 0, pos) + 1, n) for pos, n in claims if n != len(jobs)]
print(f"check_readme_counts: ci.yml has {len(jobs)} jobs; README claims {[n for _, n in claims]}")
if not claims:
    print("check_readme_counts: FAIL — README states no job count (expected at least one)")
    sys.exit(1)
if bad:
    for line_no, n in bad:
        print(f"  README.md:{line_no}: says {n}ジョブ, ci.yml has {len(jobs)}")
    sys.exit(1)
print("check_readme_counts: PASS")
