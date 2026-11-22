#!/usr/bin/env python3
"""jobs_for_change.py - which CI jobs run a script that compiles what a branch changed.

Audit-212 merged N3 2a after build_all and dnode_coupling.sh, and missed that
tests/unbounded/swim_coupling.sh (the unbounded-n job) force-includes drpc.h
ahead of swim.c's owner define (CI 44979169043 red). The rule since audit-214:
before a merge, run every job this tool names (or say why not).

  jobs_for_change.py <base> <branch>      git diff --name-only base...branch
  jobs_for_change.py --files a.h b.c      the changed files given directly
  jobs_for_change.py --selftest
  --direct (first)  no includer closure: only jobs whose text or scripts name a
                    changed file itself (the short list to run by hand first)

How (a heuristic: it names too many for common basenames like run.sh, and too
few where a script is not followed, see "Not followed" below):
  1. the changed files; a changed header pulls in, by #include "<basename>",
     every .c/.h of the tree that includes it (closure over headers)
  2. a job is named when its run: text, or a tests/... script that text names
     (and the scripts those name, 2 levels), mentions one of those files by
     its repo path or by its basename
Jobs that build whole trees with make (crown, UMP, ...) are named only when
their text mentions a file; build_all.sh is the net for those (say it ran).
Not followed (audit-215, measured on the 2026-11-22 tree): scripts outside
tests/ and tools/ (samples/*/run.sh: 32 jobs name one), names relative to a
cd (cd tests/llm && bash run_kv.sh), and $HERE/<script> inside a script (48).
A live job that runs a make-built binary (e.g. belief-revision-live and
r3_incontext.c) is not named: the short list is a floor, not the whole set.
No PyYAML: the job blocks are cut by indentation (2 spaces = a job id under jobs:).
"""
import os, re, subprocess, sys

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True,
                      text=True).stdout.strip() or '.'
CI = os.path.join(ROOT, '.github', 'workflows', 'ci.yml')
SRC_EXT = ('.c', '.h', '.S', '.s')


def jobs(text):
    """{job id: block text} for the jobs: mapping (2-space job ids)."""
    out, cur, buf, inj = {}, None, [], False
    for ln in text.split('\n'):
        if re.match(r'^jobs:\s*$', ln):
            inj = True
            continue
        if not inj:
            continue
        if re.match(r'^\S', ln):          # a new top-level key ends jobs:
            break
        m = re.match(r'^  ([A-Za-z0-9_-]+):\s*$', ln)
        if m:
            if cur:
                out[cur] = '\n'.join(buf)
            cur, buf = m.group(1), []
        elif cur:
            buf.append(ln)
    if cur:
        out[cur] = '\n'.join(buf)
    return out


def tree_files():
    r = subprocess.run(['git', '-C', ROOT, 'ls-files'], capture_output=True, text=True)
    return [f for f in r.stdout.split('\n') if f]


def read(path):
    try:
        return open(os.path.join(ROOT, path), encoding='utf-8', errors='replace').read()
    except OSError:
        return ''


def closure(changed, files):
    """the changed files plus every source that includes a changed header (closure)."""
    srcs = [f for f in files if f.endswith(SRC_EXT)]
    inc = {f: set(re.findall(r'#\s*include\s*["<]([^">]+)[">]', read(f))) for f in srcs}
    hit = set(changed)
    todo = [f for f in changed if f.endswith('.h')]
    while todo:
        h = todo.pop()
        b = os.path.basename(h)
        for f, names in inc.items():
            if f in hit:
                continue
            if any(os.path.basename(n) == b for n in names):
                hit.add(f)
                if f.endswith('.h'):
                    todo.append(f)
    return hit


SCRIPT = re.compile(r'(tests/[A-Za-z0-9_./-]+\.(?:sh|py)|tools/[A-Za-z0-9_./-]+\.(?:sh|py))')


def mentions(text, targets):
    for t in targets:
        if t in text:
            return t
        b = os.path.basename(t)
        if re.search(r'(?<![A-Za-z0-9_])' + re.escape(b) + r'(?![A-Za-z0-9_])', text):
            return t
    return None


def named_jobs(changed, ci_text=None, files=None, direct=False):
    files = files if files is not None else tree_files()
    # ci.yml and docs are not compiled: a changed ci.yml would name every job
    changed = [f for f in changed if not f.startswith('.github/') and not f.endswith('.md')]
    targets = sorted(set(changed) if direct else closure(changed, files))
    res = {}
    for jid, block in jobs(ci_text if ci_text is not None else read(CI)).items():
        why = mentions(block, targets)
        if why:
            res[jid] = 'ci.yml mentions ' + why
            continue
        seen, level = set(), set(SCRIPT.findall(block))
        for _ in range(2):
            nxt = set()
            for s in level - seen:
                seen.add(s)
                body = read(s)
                w = mentions(body, targets)
                if w and jid not in res:
                    res[jid] = s + ' mentions ' + w
                nxt |= set(SCRIPT.findall(body))
            level = nxt
    return res, targets


def selftest():
    ci = ('name: CI\non: push\njobs:\n  a-job:\n    steps:\n      - run: bash tests/x/one.sh\n'
          '  b-job:\n    steps:\n      - run: echo nothing\n  c-job:\n    steps:\n'
          '      - run: gcc -c arch/common/foo.c\n')
    ok = 0
    # 1. a job whose text names a changed .c
    r, _ = named_jobs(['arch/common/foo.c'], ci, files=[])
    ok += r.keys() == {'c-job'}
    # 2. nothing named when nothing matches
    r, _ = named_jobs(['arch/common/zzz.c'], ci, files=[])
    ok += r == {}
    # 3. jobs() keeps the three ids
    ok += sorted(jobs(ci)) == ['a-job', 'b-job', 'c-job']
    # 4. the real case: drpc.h names the unbounded-n job (through swim_coupling.sh -> swim.c)
    r, _ = named_jobs(['arch/common/include/drpc.h'])
    ok += 'unbounded-n' in r
    # 5. --direct still names it (swim_coupling.sh writes '#include "drpc.h"')
    r, _ = named_jobs(['arch/common/include/drpc.h'], direct=True)
    ok += 'unbounded-n' in r
    print('[jobs-for-change] selftest %d/5 %s' % (ok, 'ok' if ok == 5 else 'FAIL'))
    return 0 if ok == 5 else 1


def main(a):
    direct = a[:1] == ['--direct']
    if direct:
        a = a[1:]
    if a[:1] == ['--selftest']:
        return selftest()
    if a[:1] == ['--files']:
        changed = a[1:]
    elif len(a) == 2:
        r = subprocess.run(['git', '-C', ROOT, 'diff', '--name-only', a[0] + '...' + a[1]],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr.strip()); return 2
        changed = [f for f in r.stdout.split('\n') if f]
    else:
        print(__doc__); return 2
    res, targets = named_jobs(changed, direct=direct)
    print('[jobs-for-change] changed %d, with includers %d' % (len(changed), len(targets)))
    for jid in sorted(res):
        print('  %-40s %s' % (jid, res[jid]))
    print('[jobs-for-change] %d jobs; plus build_all.sh for the make builds' % len(res))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
