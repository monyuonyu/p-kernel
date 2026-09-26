#!/usr/bin/env python3
# tests/ci/check_script_modes.py — every script that ci.yml runs DIRECTLY
# (`tests/x/run_y.sh`, not `sh tests/x/run_y.sh`) must be 100755 in git.
#
# Why (2026-09-27): run_mind_pause.sh, run_dmn_pause.sh and run_federation_f1.sh
# were committed 100644. CI runs them as `timeout 300 tests/host/run_x.sh`, so
# the step died with "Permission denied" (rc 126) before the cert ran -- the
# UMP x86_64 job of CI 36200060907 went red on it, and every later step of the
# job was skipped. Local runs (`bash script.sh`) never see it.
#
# No PyYAML (not guaranteed on the runner): scans the workflow text for *.sh
# paths and skips those preceded by sh/bash/source/./cp/cat/grep/chmod.
# A path is checked relative to the repo root and to each working-directory
# named in the workflow. Exit 1 lists every offender.
import re, subprocess, sys

repo = sys.argv[1] if len(sys.argv) > 1 else "."
wf = sys.argv[2] if len(sys.argv) > 2 else ".github/workflows/ci.yml"

modes = {}
out = subprocess.run(["git", "-C", repo, "ls-files", "-s"], capture_output=True,
                     text=True, check=True).stdout
for line in out.splitlines():
    meta, path = line.split("\t", 1)
    modes[path] = meta.split()[0]

text = open(repo.rstrip("/") + "/" + wf, encoding="utf-8").read()
wdirs = set(re.findall(r'working-directory:\s*([^\s#]+)', text))
skip_prev = {"sh", "bash", "source", ".", "cp", "cat", "grep", "chmod", "#"}

checked, bad = set(), set()
for ln in text.splitlines():
    code = ln.split(" #", 1)[0] if not ln.lstrip().startswith("#") else ""
    for m in re.finditer(r'(?<![\w./-])((?:\./)?(?:[\w.-]+/)*[\w.-]+\.sh)\b', code):
        before = code[:m.start()].rstrip().split()
        if before and before[-1] in skip_prev:
            continue
        p = m.group(1)[2:] if m.group(1).startswith("./") else m.group(1)
        for c in [p] + [w.rstrip("/") + "/" + p for w in wdirs]:
            if c in modes:
                checked.add(c)
                if modes[c] != "100755":
                    bad.add((c, modes[c]))

print("[script-modes] directly-executed scripts checked: %d" % len(checked))
for c, m in sorted(bad):
    print("[script-modes] NOT EXECUTABLE (%s): %s  -> git update-index --chmod=+x %s" % (m, c, c))
if bad:
    print("[script-modes] FAIL")
    sys.exit(1)
print("[script-modes] PASS")
