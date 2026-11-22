#!/usr/bin/env python3
"""b5q8_check.py - the judge of B5 q8 (research/b5-q8-copy.md §4).

Reads <out>/q8-<seed>.log (the plain build, stage q8) and <out>/nq8-<seed>.log
(the -DB5Q_NC_INT2T build, stage q8) of student_region_test. Lines fixed in the
design (3efd5246) before the judgement seeds 261..268 were run:
  Q1  region4-q8 kept <= region4 kept + 0.03                         >= 7 of 8
  Q2  region4-q8 kept <= alone kept - 0.10                           8 of 8
  Q3  region4-garb1-q8 kept <= region4-garb1 kept + 0.03             >= 7 of 8
      and region4-garb2-q8 kept == alone kept (every printed digit)  8 of 8
  NC  Q2 on the nq8 build (its own alone)                  red on >= 7 of 8
The ruler is kept (b1_held, bits per byte, at node 0's lowest gate pass).
"""
import re
import sys

ARM = re.compile(r'^\[b5w\] arm=(\S+) last=(\S+) kept=(\S+) at (\d+)')
MARGIN = 0.03
GAIN = 0.10
NEED = 7


def parse(text):
    arms, done = {}, False
    for ln in text.splitlines():
        if ln.startswith('[b5w] done'):
            done = True
        m = ARM.match(ln)
        if m:
            arms[m.group(1)] = m.group(3)          # the printed string
    want = ('alone', 'region4', 'region4-garb1', 'region4-q8', 'region4-garb1-q8', 'region4-garb2-q8')
    return arms if done and all(a in arms for a in want) else None


def q1(p):
    return float(p['region4-q8']) <= float(p['region4']) + MARGIN


def q2(p):
    return float(p['region4-q8']) <= float(p['alone']) - GAIN


def q3a(p):
    return float(p['region4-garb1-q8']) <= float(p['region4-garb1']) + MARGIN


def q3b(p):
    return p['region4-garb2-q8'] == p['alone']


def judge(plain, nc, seeds, say=print):
    n1 = n2 = n3a = n3b = nn = 0
    ok_all = True
    for s in seeds:
        p, q = plain.get(s), nc.get(s)
        if p is None or q is None:
            say(f'[b5-q8] seed {s}: a log is missing or not done')
            ok_all = False
            continue
        a, b, c, d, e = q1(p), q2(p), q3a(p), q3b(p), q2(q)
        n1 += a; n2 += b; n3a += c; n3b += d; nn += e
        say(f'[b5-q8] seed {s}: alone {p["alone"]} region4 fp {p["region4"]} q8 {p["region4-q8"]} '
            f'garb1 fp {p["region4-garb1"]} q8 {p["region4-garb1-q8"]} garb2-q8 {p["region4-garb2-q8"]} '
            f'| nc q8 {q["region4-q8"]} | Q1 {"ok" if a else "red"} Q2 {"ok" if b else "red"} '
            f'Q3 {"ok" if c else "red"}/{"ok" if d else "red"} NC-Q2 {"green" if e else "red"}')
    k = len(seeds)
    g1, g2, g3, gn = n1 >= NEED, n2 == k, n3a >= NEED and n3b == k, (k - nn) >= NEED
    say(f'[b5-q8] Q1 {n1}/{k} {"PASS" if g1 else "FAIL"}; Q2 {n2}/{k} {"PASS" if g2 else "FAIL"}; '
        f'Q3 {n3a}/{k} and {n3b}/{k} {"PASS" if g3 else "FAIL"}; '
        f'NC Q2 red {k - nn}/{k} {"RED (as it must)" if gn else "NOT RED (the falsifier failed)"}')
    ok = ok_all and g1 and g2 and g3 and gn
    say(f'[result] {"PASS" if ok else "FAIL"}  b5-q8 judgement')
    return ok


def self_test():
    def log(alone, r4, g1, q8, g1q, g2q):
        arms = [('alone', alone), ('region4', r4), ('region4-garb1', g1),
                ('region4-q8', q8), ('region4-garb1-q8', g1q), ('region4-garb2-q8', g2q)]
        return ''.join(f'[b5w] arm={a} last=2 kept={v} at 9 (gate 1) lastgate=1 took=3/8 acc=24\n' for a, v in arms) \
            + '[b5w] done\n'
    cases = 0
    good = parse(log('4.0000', '3.7000', '3.8000', '3.7100', '3.8100', '4.0000'))
    assert good and q1(good) and q2(good) and q3a(good) and q3b(good); cases += 1
    p = parse(log('4.0000', '3.7000', '3.8000', '3.7400', '3.8100', '4.0000'))
    assert not q1(p); cases += 1                                    # past the margin
    p = parse(log('4.0000', '3.7000', '3.8000', '3.9500', '3.8100', '4.0000'))
    assert not q2(p); cases += 1                                    # not 0.10 better
    p = parse(log('4.0000', '3.7000', '3.8000', '3.7100', '3.8100', '4.0001'))
    assert not q3b(p); cases += 1                                   # garb2 moved
    assert parse(log('4.0000', '3.7000', '3.8000', '3.7100', '3.8100', '4.0000').replace('[b5w] done\n', '')) is None
    cases += 1
    seeds = list(range(8))
    nc_like = {s: parse(log('4.0000', '3.7000', '3.8000', '4.0000', '4.0000', '4.0000')) for s in seeds}
    goods = {s: good for s in seeds}
    quiet = lambda *_: None
    assert judge(goods, nc_like, seeds, quiet); cases += 1
    assert not judge(goods, goods, seeds, quiet); cases += 1        # the falsifier green
    one = dict(goods); one[0] = parse(log('4.0000', '3.7000', '3.8000', '3.9500', '3.8100', '4.0000'))
    assert not judge(one, nc_like, seeds, quiet); cases += 1        # Q2 is 8 of 8
    q1one = dict(goods); q1one[0] = parse(log('4.0000', '3.7000', '3.8000', '3.7400', '3.8100', '4.0000'))
    assert judge(q1one, nc_like, seeds, quiet); cases += 1          # Q1 7 of 8
    miss = dict(goods); del miss[3]
    assert not judge(miss, nc_like, seeds, quiet); cases += 1
    for i in range(cases):
        print(f'  case {i} ok ')
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == '--self-test':
        return self_test()
    if len(argv) != 4 or argv[1] != '--seeds':
        print('usage: b5q8_check.py --self-test | --seeds "261 ..." <out>')
        return 2
    seeds = [int(x) for x in argv[2].split()]
    plain, nc = {}, {}
    for s in seeds:
        for tab, b in ((plain, 'q8'), (nc, 'nq8')):
            try:
                with open(f'{argv[3]}/{b}-{s}.log', encoding='utf-8') as f:
                    tab[s] = parse(f.read())
            except OSError:
                tab[s] = None
    return 0 if judge(plain, nc, seeds) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
