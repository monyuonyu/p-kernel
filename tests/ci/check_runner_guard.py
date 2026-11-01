#!/usr/bin/env python3
# tests/ci/check_runner_guard.py — inbox #39: every job that runs on the self-hosted ThinkPad
# runner must carry the fork guard in its own `if:`, so that a fork's pull request runs only
# the ubuntu-latest jobs. Exit 1 lists every offender. `--selftest` runs fixtures (each kind
# of offence must redden, a good file must pass).
#
# This keeps an honest file honest; it is not a wall. A pull_request run uses the ci.yml of the
# PR's merge commit, so a fork can delete the guard (and this check) in its own PR. What stops
# that is "Require approval for all external contributors" plus reading the diff first.
#
# No PyYAML: a text scan of the workflow, like check_script_modes.py.
import re, sys

GUARD = ("github.event_name == 'push' || "
         "github.event.pull_request.head.repo.full_name == github.repository")
JOB = re.compile(r'^  ([A-Za-z0-9_-]+):\s*(#.*)?$')
TOP = re.compile(r'^\S')
KEY = re.compile(r'^    (runs-on|if):\s*(.*)$')
# pull_request_target runs with the base repo's secrets, and workflow_run after a fork's run.
BAD_EVENT = re.compile(r'^\s*(pull_request_target|workflow_run)\s*:', re.M)


def jobs(text):
    """yields (job, runs-on text, if text). Only the top-level `jobs:` block counts."""
    name, ron, cond, inside = None, '', '', False
    for line in text.splitlines():
        if TOP.match(line) and not line.startswith('#'):
            if name:
                yield name, ron, cond
                name = None
            inside = line.startswith('jobs:')
            continue
        if not inside:
            continue
        m = JOB.match(line)
        if m:
            if name:
                yield name, ron, cond
            name, ron, cond = m.group(1), '', ''
            continue
        m = KEY.match(line)
        if m and name:
            if m.group(1) == 'runs-on':
                ron = m.group(2)
            else:
                cond = m.group(2)
    if name:
        yield name, ron, cond


def check(text):
    bad, n = [], 0
    for name, ron, cond in jobs(text):
        if not ron:
            bad.append(f'{name}: no one-line runs-on (cannot tell where it runs)')
        elif 'self-hosted' in ron or '${{' in ron:
            n += 1
            if GUARD not in cond:
                bad.append(f'{name}: self-hosted without the fork guard in its if:')
    m = BAD_EVENT.search(text.split('\njobs:')[0])
    if m:
        bad.append(f'on: {m.group(1)} would run fork code with the base repo\'s rights')
    return bad, n


def selftest():
    g = f"${{{{ always() && ({GUARD}) }}}}"
    head = "name: CI\non:\n  push:\n    branches: ['feat/**']\njobs:\n"
    good = head + ("  a:\n    runs-on: ubuntu-latest\n    steps: []\n"
                   f"  b:\n    runs-on: [self-hosted, pkernel-thinkpad]\n    if: {g}\n    steps: []\n")
    cases = [
        ('good', good, 0),
        ('no if', head + "  b:\n    runs-on: [self-hosted, x]\n    steps: []\n", 1),
        ('other if', head + "  b:\n    runs-on: [self-hosted, x]\n    if: always()\n    steps: []\n", 1),
        ('one half of the guard', head + "  b:\n    runs-on: [self-hosted, x]\n"
         "    if: github.event_name == 'push'\n    steps: []\n", 1),
        ('expression runs-on', head + "  b:\n    runs-on: ${{ matrix.r }}\n    steps: []\n", 1),
        ('multi-line runs-on', head + "  b:\n    runs-on:\n      - self-hosted\n    steps: []\n", 1),
        ('second job', good + "  c:\n    runs-on: [self-hosted, x]\n    steps: []\n", 1),
        ('pull_request_target', good.replace('  push:\n', '  pull_request_target:\n  push:\n'), 1),
        # audit-107: the label alone reaches the ThinkPad, and on: has a one-line form
        ('label only', head + "  b:\n    runs-on: pkernel-thinkpad\n    steps: []\n", 1),
        ('label in a list', head + "  b:\n    runs-on: [pkernel-thinkpad]\n    steps: []\n", 1),
        ('runner group', head + "  b:\n    runs-on: {group: x}\n    steps: []\n", 1),
        ('flow on', good.replace("on:\n  push:\n    branches: ['feat/**']\n",
                                 "on: [push, pull_request_target]\n"), 1),
        ('flow on, workflow_run', good.replace("on:\n  push:\n    branches: ['feat/**']\n",
                                               "on: [workflow_run]\n"), 1),
        ('a comment in on: is no event',
         good.replace("on:\n", "on:\n  # not pull_request_target, on purpose\n"), 0),
        ('hosted names stay hosted',
         good.replace('runs-on: ubuntu-latest', 'runs-on: ubuntu-24.04'), 0),
    ]
    fails = 0
    for name, text, want in cases:
        got = len(check(text)[0])
        if got != want:
            print(f'FAIL {name}: {got} offences, wanted {want}')
            fails += 1
    print('[runner-guard-selftest] ' + ('ALL PASS' if not fails else f'{fails} FAIL'))
    return 1 if fails else 0


def main():
    if '--selftest' in sys.argv:
        return selftest()
    repo = sys.argv[1] if len(sys.argv) > 1 else '.'
    wf = sys.argv[2] if len(sys.argv) > 2 else '.github/workflows/ci.yml'
    text = open(repo.rstrip('/') + '/' + wf, encoding='utf-8').read()
    bad, n = check(text)
    for b in bad:
        print('[runner-guard] ' + b)
    print(f'[runner-guard] {n} self-hosted jobs, {len(bad)} offences: ' + ('FAIL' if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
