#!/usr/bin/env python3
"""check_ci_negations.py - a '! grep ...' in the middle of a run: block has no teeth.

GitHub runs a step's run: block with `bash -e`, and bash does not stop on a
command negated with '!' (set -e ignores it). So a scan like
    ! grep -aE '\\[rev-[a-z-]+\\] FAIL' selftest.log
in the middle of a block never reddens the step, even when it matches; only the
last command of the block (its status becomes the step's) or a negation with
'|| ... exit 1' bites. audit-212 (2026-11-22) found 9 such scans (and one more
on feat/n3-dnode-door); write them as `if grep ...; then exit 1; fi`.

Red (exit 1) on a run: block line that starts with '! grep', is not the last
command of the block and has no '||'. Other negated commands ('! cmd > log'
followed by greps of that log) are not judged here.
Usage: check_ci_negations.py [repo [workflow]] | --selftest
"""
import os, re, sys


def commands(run):
    """the run: block as commands: continued lines joined, blanks and comments dropped"""
    out, cur = [], ''
    for l in run.split('\n'):
        t = l.rstrip()
        if t.endswith('\\'):
            cur += t[:-1] + ' '
            continue
        cur += t
        if cur.strip() and not cur.strip().startswith('#'):
            out.append(cur.strip())
        cur = ''
    if cur.strip():
        out.append(cur.strip())
    return out


RUN = re.compile(r'^(\s*)(?:- )?run:\s*[|>][-+]?\s*$')


def blocks(text):
    """(line number, block text) of every block-scalar run: (no YAML library: the
    CI image need not have one). A block is the lines indented deeper than the
    run: key, blank lines included. A one-line run: is one command = always last."""
    ls = text.split('\n')
    i = 0
    while i < len(ls):
        m = RUN.match(ls[i])
        if not m:
            i += 1
            continue
        key = len(m.group(1)) + (2 if ls[i].lstrip().startswith('- ') else 0)
        body, j = [], i + 1
        while j < len(ls) and (not ls[j].strip() or len(ls[j]) - len(ls[j].lstrip()) > key):
            body.append(ls[j])
            j += 1
        yield i + 1, '\n'.join(body)
        i = j


def check(text):
    bad = []
    for ln, run in blocks(text):
        cs = commands(run)
        for i, c in enumerate(cs):
            if c.startswith('! grep') and i != len(cs) - 1 and '||' not in c:
                bad.append(('line %d' % ln, 'run block', c))
    return bad


def selftest():
    def wf(run):
        body = '\n'.join('          ' + l for l in run.split('\n'))
        return 'jobs:\n  j:\n    steps:\n      - name: s\n        run: |\n' + body + '\n'
    cases = [
        ('middle negated grep', "make\n! grep -aE 'X FAIL' a.log\ngrep -aF 'Y PASS' a.log", 1),
        ('last negated grep (its status is the step\'s)', "make\n! grep -aE 'X FAIL' a.log", 0),
        ('negated grep with || exit', "! grep -aF 'X PASS' a.log || { echo no; exit 1; }\ngrep -aF Y a.log", 0),
        ('if grep then exit', "if grep -aE 'X FAIL' a.log; then exit 1; fi\ngrep -aF Y a.log", 0),
        ('continued negated grep in the middle', "! grep -aqF 'X' a.log \\\n  && true\ngrep Y a.log", 1),
        ('a comment after it: still the last command', "make\n! grep -aE 'X FAIL' a.log\n# done", 0),
        ('negated non-grep (not judged)', "! N3C=1 bash t.sh > l 2>&1\ngrep -aqF 'C9 FAIL' l", 0),
    ]
    ok = True
    for name, run, want in cases:
        got = len(check(wf(run)))
        print('[ci-neg-selftest] %s %s (offences %d, want %d)' % ('PASS' if got == want else 'FAIL', name, got, want))
        ok = ok and got == want
    print('[ci-neg-selftest] ' + ('ALL PASS' if ok else 'RED'))
    return 0 if ok else 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--selftest':
        return selftest()
    repo = sys.argv[1] if len(sys.argv) > 1 else '.'
    wfp = sys.argv[2] if len(sys.argv) > 2 else '.github/workflows/ci.yml'
    bad = check(open(os.path.join(repo, wfp), encoding='utf-8').read())
    for jn, sn, c in bad:
        print('::error file=%s::%s / %s: "%s" is negated in the middle of the block = never stops the step (write: if grep ...; then exit 1; fi)' % (wfp, jn, sn[:60], c[:100]))
    print('check_ci_negations: %d toothless negated grep(s)' % len(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
