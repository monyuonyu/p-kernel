#!/usr/bin/env python3
"""b5_wire2_check.py - the judge of W2b, B5 wire stage 2 (research/b5-wire2.md §3).

A weak poison: node 3 trains honestly and publishes its weights + s x (the std
of each weight family) x N(0,1). student_region_test <held> <seed> w2b:<s>
prints the arms alone, region4-weak (st_region_round, the trim) and mean4-weak
(st_round_merge through the same gate). Lines fixed in the design before the
code (the reading of "adopted" is the implementer's, 11-18: a seed counts when
the mean arm took at least half of its rounds):
  A  mean4-weak took >= half of its rounds                       >= 6 of 8 seeds
  B  region4-weak kept < mean4-weak kept                         >= 7 of 8 seeds
  C  region4-weak kept <= alone kept + 0.03                      >= 7 of 8 seeds
Calibration (seeds 209 210, s in 0.5 1 2): the largest s where the mean arm
took at least half of its rounds on both seeds.
  b5_wire2_check.py --self-test
  b5_wire2_check.py --cal "209 210" "0.5 1 2" <out>     (reads cal-<s>-<seed>.log)
  b5_wire2_check.py --seeds "211 ... 218" <out>         (reads w2b-<seed>.log)
"""
import re
import sys

ARM = re.compile(r'^\[b5w\] arm=(\S+) last=(\S+) kept=(\S+) at (\d+).*?(?:took=(\d+)/(\d+))?\s*(?:acc=(\d+))?\s*$')
NEED_A, NEED_B, NEED_C = 6, 7, 7
MARGIN = 0.03


def parse(text):
    arms, done = {}, False
    for ln in text.splitlines():
        if ln.startswith('[b5w] done'):
            done = True
        m = ARM.match(ln)
        if m:
            arms[m.group(1)] = {'kept': float(m.group(3)),
                                'took': int(m.group(5)) if m.group(5) else None,
                                'tries': int(m.group(6)) if m.group(6) else None}
    if not done or not all(k in arms for k in ('alone', 'region4-weak', 'mean4-weak')):
        return None
    return arms


def half(arm):
    return arm['took'] is not None and arm['tries'] and 2 * arm['took'] >= arm['tries']


def judge(logs, say=print):
    """logs: {seed: text}. Returns True when A, B and C all hold."""
    a = b = c = 0
    for s in sorted(logs):
        p = parse(logs[s])
        if p is None:
            say(f'  seed {s}: log incomplete -> counts as red on A B C')
            continue
        al, r, m = p['alone']['kept'], p['region4-weak'], p['mean4-weak']
        ka, kb, kc = half(m), r['kept'] < m['kept'], r['kept'] <= al + MARGIN
        a += ka; b += kb; c += kc
        say(f'  seed {s}: alone {al:.4f} trim {r["kept"]:.4f} (took {r["took"]}/{r["tries"]}) '
            f'mean {m["kept"]:.4f} (took {m["took"]}/{m["tries"]})  A {"ok" if ka else "RED"} '
            f'B {"ok" if kb else "RED"} C {"ok" if kc else "RED"} (trim - alone {r["kept"] - al:+.4f})')
    n = len(logs)
    ok = a >= NEED_A and b >= NEED_B and c >= NEED_C
    say(f'[w2b] A {a}/{n} (need {NEED_A})  B {b}/{n} (need {NEED_B})  C {c}/{n} (need {NEED_C})')
    say(f'[result] {"PASS" if ok else "FAIL"}  b5-wire2 W2b')
    return ok


def calibrate(logs, svals, say=print):
    """logs: {(s, seed): text}. Returns the largest s where the mean arm took half on every seed, or None."""
    best = None
    for s in svals:
        oks = []
        for (sv, seed), t in sorted(logs.items()):
            if sv != s:
                continue
            p = parse(t)
            ok = p is not None and half(p['mean4-weak'])
            oks.append(ok)
            if p:
                say(f'  s {s} seed {seed}: mean took {p["mean4-weak"]["took"]}/{p["mean4-weak"]["tries"]} '
                    f'kept {p["mean4-weak"]["kept"]:.4f}, trim kept {p["region4-weak"]["kept"]:.4f}, alone {p["alone"]["kept"]:.4f}')
            else:
                say(f'  s {s} seed {seed}: log incomplete')
        if oks and all(oks):
            best = s if best is None or s > best else best
    say(f'[w2b-cal] s = {best}')
    return best


def self_test():
    def log(al, r, rt, m, mt, tries=8, done=True):
        t = (f'[b5w] arm=alone last=9 kept={al} at 3 (gate 1) lastgate=1 \n'
             f'[b5w] arm=region4-weak last=9 kept={r} at 3 (gate 1) lastgate=1 took={rt}/{tries} acc=3\n'
             f'[b5w] arm=mean4-weak last=9 kept={m} at 3 (gate 1) lastgate=1 took={mt}/{tries} acc=3\n')
        return t + ('[b5w] done\n' if done else '')
    good = {s: log(4.0, 3.9, 6, 4.1, 6) for s in range(8)}
    cases = [('all green', good, True)]
    a = dict(good); a.update({0: log(4.0, 3.9, 6, 4.1, 3), 1: log(4.0, 3.9, 6, 4.1, 3), 2: log(4.0, 3.9, 6, 4.1, 3)})
    cases.append(('A 5/8', a, False))
    b = dict(good); b.update({0: log(4.0, 3.9, 6, 3.8, 6), 1: log(4.0, 3.9, 6, 3.9, 6)})
    cases.append(('B 6/8 (one equal)', b, False))
    c = dict(good); c.update({0: log(4.0, 4.031, 6, 4.1, 6), 1: log(4.0, 4.05, 6, 4.1, 6)})
    cases.append(('C 6/8', c, False))
    c1 = dict(good); c1.update({0: log(4.0, 4.03, 6, 4.1, 6)})
    cases.append(('C on the line', c1, True))
    inc = dict(good); inc.update({0: log(4.0, 3.9, 6, 4.1, 6, done=False), 1: log(4.0, 3.9, 6, 4.1, 6, done=False)})
    cases.append(('two logs incomplete', inc, False))
    a6 = dict(good); a6.update({0: log(4.0, 3.9, 6, 4.1, 3), 1: log(4.0, 3.9, 6, 4.1, 3)})
    cases.append(('A 6/8', a6, True))
    bad = 0
    for name, logs, want in cases:
        got = judge(logs, say=lambda *_: None)
        print(f'  {"ok " if got == want else "BAD"} {name}: {got} (want {want})')
        bad += got != want
    cl = {(0.5, 1): log(4, 3.9, 6, 4.1, 8), (0.5, 2): log(4, 3.9, 6, 4.1, 8),
          (1.0, 1): log(4, 3.9, 6, 4.1, 4), (1.0, 2): log(4, 3.9, 6, 4.1, 5),
          (2.0, 1): log(4, 3.9, 6, 4.1, 4), (2.0, 2): log(4, 3.9, 6, 4.1, 3)}
    got = calibrate(cl, [0.5, 1.0, 2.0], say=lambda *_: None)
    print(f'  {"ok " if got == 1.0 else "BAD"} calibration picks 1.0: {got}')
    bad += got != 1.0
    print(f'[w2b-self] {"PASS" if not bad else "FAIL"} ({bad} bad)')
    return 0 if not bad else 1


def main(argv):
    if len(argv) > 1 and argv[1] == '--self-test':
        return self_test()
    if len(argv) == 5 and argv[1] == '--cal':
        seeds = [int(x) for x in argv[2].split()]
        svals = [float(x) for x in argv[3].split()]
        logs = {}
        for s in svals:
            for seed in seeds:
                try:
                    logs[(s, seed)] = open(f'{argv[4]}/cal-{argv[3].split()[svals.index(s)]}-{seed}.log').read()
                except OSError:
                    logs[(s, seed)] = ''
        best = calibrate(logs, svals)
        return 0 if best is not None else 1
    if len(argv) == 4 and argv[1] == '--seeds':
        logs = {}
        for seed in [int(x) for x in argv[2].split()]:
            try:
                logs[seed] = open(f'{argv[3]}/w2b-{seed}.log').read()
            except OSError:
                logs[seed] = ''
        return 0 if judge(logs) else 1
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
