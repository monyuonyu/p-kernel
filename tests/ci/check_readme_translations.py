#!/usr/bin/env python3
# tests/ci/check_readme_translations.py — are the README translations older
# than the Japanese original?
#
# README.ja.md is the original (inbox #12). README.md (English) and
# README.zh-CN.md (Chinese) each name, in a header near the top, the commit of
# README.ja.md they were translated from, e.g.
#   > Translated from [README.ja.md](README.ja.md) ... at commit `1be96cb1`.
# If README.ja.md has changed in any commit after that one, the translation is
# stale: this prints the commits and exits 1. A missing or unknown commit in a
# header is also exit 1 (the watch would otherwise be silently blind).
# CI runs it as a NON-blocking step (continue-on-error, with a ::warning::) —
# it tells, it does not stop. Needs the full history (fetch-depth: 0).
# Limit: it looks at commits, not content. A commit that touches README.ja.md
# without changing its text (e.g. 7c9e0fbd, which created it by renaming
# README.md) counts as a change.
import re, subprocess, sys

repo = (sys.argv[1] if len(sys.argv) > 1 else ".").rstrip("/")
TRANSLATIONS = ["README.md", "README.zh-CN.md"]
SRC = re.compile(r"README\.ja\.md.*?`([0-9a-f]{7,40})`")


def git(*a):
    return subprocess.run(["git", "-C", repo] + list(a), capture_output=True, text=True)


fail = False
for name in TRANSLATIONS:
    try:
        head = open(repo + "/" + name, encoding="utf-8").read().splitlines()[:6]
    except FileNotFoundError:
        print(f"check_readme_translations: FAIL — {name} is missing"); fail = True; continue
    m = next((SRC.search(l) for l in head if SRC.search(l)), None)
    if not m:
        print(f"::warning file={name}::no 'README.ja.md ... `<commit>`' header in the first 6 lines")
        print(f"check_readme_translations: FAIL — {name} names no source commit"); fail = True; continue
    src = m.group(1)
    if git("cat-file", "-e", src + "^{commit}").returncode != 0:
        print(f"::warning file={name}::source commit {src} is not in this history (shallow clone?)")
        print(f"check_readme_translations: FAIL — {name}: commit {src} not found"); fail = True; continue
    newer = git("log", "--format=%h %s", src + "..HEAD", "--", "README.ja.md").stdout.strip()
    if newer:
        print(f"::warning file={name}::translated from README.ja.md at {src}; README.ja.md has changed since")
        print(f"check_readme_translations: STALE — {name} (from {src}); README.ja.md changed in:")
        for line in newer.splitlines():
            print("    " + line)
        fail = True
    else:
        print(f"check_readme_translations: {name} is up to date with README.ja.md (from {src})")
if fail:
    sys.exit(1)
print("check_readme_translations: PASS")
