#!/usr/bin/env python3
"""b5_wire_check.py - the judge of B5 wire stage 1 (research/b5-wire.md §3 §4).

Reads <out>/plain-<seed>.log (stage all) and <out>/nomean-<seed>.log (stage w2)
of student_region_test. Lines fixed in the design before the code:
  W1  region4 kept < alone kept                                    >= 7 of 8 seeds
  W2  region4-garb1 kept <= alone kept + 0.03, < mean4-garb1 kept,
      and took >= 1                                                >= 7 of 8 seeds
  NC_MEAN  the same W2 on the nomean build (its alone from plain)  must be RED
The ruler is kept (b1_held, bits per byte, at node 0's lowest gate pass).
"""
import re
import sys

ARM = re.compile(r'^\[b5w\] arm=(\S+) last=(\S+) kept=(\S+) at (\d+).*?(?:took=(\d+)/(\d+))?\s*(?:acc=(\d+))?\s*$')
NEED = 7
W2_MARGIN = 0.03


def parse(text):
    arms = {}
    done = False
    for ln in text.splitlines():
        if ln.startswith('[b5w] done'):
            done = True
        m = ARM.match(ln)
        if m:
            arms[m.group(1)] = {'kept': float(m.group(3)),
                                'took': int(m.group(5)) if m.group(5) else None}
    return arms if done else None


def w1(p):
    return p['region4']['kept'] < p['alone']['kept']


def w2(alone, arms):
    r, m = arms['region4-garb1'], arms['mean4-garb1']
    return r['kept'] <= alone['kept'] + W2_MARGIN and r['kept'] < m['kept'] and (r['took'] or 0) >= 1


def judge(plain, nomean, seeds, say=print):
    n1 = n2 = nn = 0
    ok_all = True
    for s in seeds:
        p, q = plain.get(s), nomean.get(s)
        if p is None or q is None or 'alone' not in p:
            say(f'[b5-wire] seed {s}: a log is missing or not done')
            ok_all = False
            continue
        a, b, c = w1(p), w2(p['alone'], p), w2(p['alone'], q)
        n1 += a; n2 += b; nn += c
        say(f'[b5-wire] seed {s}: alone {p["alone"]["kept"]:.4f} region4 {p["region4"]["kept"]:.4f} '
            f'garb1 {p["region4-garb1"]["kept"]:.4f} (took {p["region4-garb1"]["took"]}) '
            f'mean-garb1 {p["mean4-garb1"]["kept"]:.4f} garb2 {p.get("region4-garb2", {}).get("kept", float("nan")):.4f} '
            f'| W1 {"ok" if a else "red"} W2 {"ok" if b else "red"} NC_MEAN-W2 {"green" if c else "red"}')
    g1, g2, gn = n1 >= NEED, n2 >= NEED, nn >= NEED
    say(f'[b5-wire] W1 {n1}/{len(seeds)} {"PASS" if g1 else "FAIL"}; W2 {n2}/{len(seeds)} {"PASS" if g2 else "FAIL"}; '
        f'NC_MEAN W2 {nn}/{len(seeds)} {"RED (as it must)" if not gn else "GREEN (the falsifier failed)"}')
    ok = ok_all and g1 and g2 and not gn
    say(f'[result] {"PASS" if ok else "FAIL"}  b5-wire judgement')
    return ok


def self_test():
    def log(alone, r4, g1, took, m1):
        return (f'[b5w] seed 1 build plain stage all\n'
                f'[b5w] arm=alone last=2 kept={alone} at 9 (gate 1) lastgate=1 \n'
                f'[b5w] arm=region4 last=2 kept={r4} at 9 (gate 1) lastgate=1 took=3/8 acc=24\n'
                f'[b5w] arm=region4-garb1 last=2 kept={g1} at 9 (gate 1) lastgate=1 took={took}/8 acc=24\n'
                f'[b5w] arm=mean4-garb1 last=2 kept={m1} at 9 (gate 1) lastgate=1 took=0/8 acc=24\n'
                f'[b5w] done\n')
    cases = 0
    p = parse(log(3.0, 2.9, 3.01, 2, 3.05))
    assert p and w1(p) and w2(p['alone'], p); cases += 1
    p = parse(log(3.0, 3.0, 3.01, 2, 3.0))
    assert not w1(p); cases += 1                       # equal is not better
    p = parse(log(3.0, 2.9, 3.04, 2, 3.1))
    assert not w2(p['alone'], p); cases += 1           # past the margin
    p = parse(log(3.0, 2.9, 3.0, 2, 3.0))
    assert not w2(p['alone'], p); cases += 1           # not below the mean
    p = parse(log(3.0, 2.9, 2.9, 0, 3.0))
    assert not w2(p['alone'], p); cases += 1           # took nothing (§4 ②)
    assert parse(log(3.0, 2.9, 3.0, 2, 3.0).replace('[b5w] done\n', '')) is None; cases += 1
    seeds = list(range(8))
    good = {s: parse(log(3.0, 2.9, 2.95, 2, 3.0)) for s in seeds}
    mean_like = {s: parse(log(3.0, 2.9, 3.0, 0, 3.0)) for s in seeds}
    quiet = lambda *_: None
    assert judge(good, mean_like, seeds, quiet); cases += 1
    assert not judge(good, good, seeds, quiet); cases += 1          # the falsifier green
    bad = dict(good); bad[0] = bad[1] = parse(log(3.0, 3.1, 2.95, 2, 3.0))
    assert not judge(bad, mean_like, seeds, quiet); cases += 1      # W1 6/8
    one = dict(good); one[0] = parse(log(3.0, 3.1, 2.95, 2, 3.0))
    assert judge(one, mean_like, seeds, quiet); cases += 1          # W1 7/8
    miss = dict(good); del miss[3]
    assert not judge(miss, mean_like, seeds, quiet); cases += 1
    for i in range(cases):
        print(f'  case {i} ok ')
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == '--self-test':
        return self_test()
    if len(argv) != 4 or argv[1] != '--seeds':
        print('usage: b5_wire_check.py --self-test | --seeds "181 ..." <out>')
        return 2
    seeds = [int(x) for x in argv[2].split()]
    out = argv[3]
    plain, nomean = {}, {}
    for s in seeds:
        for tab, b in ((plain, 'plain'), (nomean, 'nomean')):
            try:
                with open(f'{out}/{b}-{s}.log') as f:
                    tab[s] = parse(f.read())
            except OSError:
                tab[s] = None
    return 0 if judge(plain, nomean, seeds) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
