#!/usr/bin/env python3
# tests/ci/test_check_readme_translations.py — fixtures for
# check_readme_translations.py.
#
# Each case builds a throw-away git repo with README.ja.md (the original) and
# the two translations, whose header names the README.ja.md commit they were
# translated from, then runs the checker and compares the exit code.
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "check_readme_translations.py")
GIT = ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]


def git(d, *a):
    return subprocess.run(GIT + ["-C", d] + list(a), capture_output=True, text=True, check=True).stdout.strip()


def write(d, name, text):
    open(os.path.join(d, name), "w", encoding="utf-8").write(text)


def header(src, zh=False):
    if zh:
        return "> 译自 [README.ja.md](README.ja.md)（日文版为原文），对应提交 `%s`。\n\n# x\n" % src
    return "> Translated from [README.ja.md](README.ja.md) (the Japanese version is the original) at commit `%s`.\n\n# x\n" % src


def repo(d, change_ja_after, en_src=None, zh_src=None, touch_other_after=False):
    git(d, "init", "-q")
    write(d, "README.ja.md", "# 原文 1\n")
    git(d, "add", "."); git(d, "commit", "-q", "-m", "ja 1")
    src = git(d, "rev-parse", "--short=8", "HEAD")
    write(d, "README.md", header(en_src or src))
    write(d, "README.zh-CN.md", header(zh_src or src, zh=True))
    git(d, "add", "."); git(d, "commit", "-q", "-m", "translations")
    if touch_other_after:
        write(d, "other.txt", "x\n"); git(d, "add", "."); git(d, "commit", "-q", "-m", "other")
    if change_ja_after:
        write(d, "README.ja.md", "# 原文 2\n"); git(d, "add", "."); git(d, "commit", "-q", "-m", "ja 2")


CASES = [
    # name, repo kwargs, expected rc
    ("fresh: nothing changed README.ja.md since the source commit", dict(change_ja_after=False), 0),
    ("fresh: later commits touch other files only", dict(change_ja_after=False, touch_other_after=True), 0),
    ("stale: README.ja.md changed after the source commit", dict(change_ja_after=True), 1),
    ("broken: header names a commit that does not exist", dict(change_ja_after=False, en_src="deadbeef"), 1),
    ("broken: header has no commit at all", dict(change_ja_after=False, zh_src="not-a-hash"), 1),
]

fails = 0
for name, kw, want in CASES:
    with tempfile.TemporaryDirectory() as d:
        repo(d, **kw)
        r = subprocess.run([sys.executable, CHECK, d], capture_output=True, text=True)
    ok = r.returncode == want
    fails += not ok
    print(("PASS" if ok else "FAIL") + f" {name}: rc={r.returncode} want={want}")
    if not ok:
        print("    " + (r.stdout + r.stderr).strip().replace("\n", "\n    "))
print(f"test_check_readme_translations: {len(CASES) - fails} PASS / {fails} FAIL")
sys.exit(1 if fails else 0)
