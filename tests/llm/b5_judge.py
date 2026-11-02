#!/usr/bin/env python3
"""b5_judge.py - the judge of B5 stage 1 (research/b5-region-rounds.md section 4).

Reads the logs of run_b5_rounds.sh; the lines were fixed before the code and
before any run on the judgment seeds 43..50.

  G1  on all 8 seeds the round's child, both halves <= the better parent - 0.03
  G2  on at least 7 seeds that child is below stage 9's distilled child (keep)
  G3  on at least 7 seeds the child's TEST <= the better parent's TEST
  NC  B5_PEER=reset and B5_PEER=random: G1 red

G4 and G5 are the gates of student_round_test gates, not read here.

  b5_judge.py <work dir> [seeds]
  b5_judge.py --self     the judge against made-up lines
  exit 0 = G1, G2, G3 green and both NCs bite
"""
import os
import re
import sys

SEEDS = list(range(43, 51))
GAIN = 0.03
NEED = 7
B5 = re.compile(r'^\[b5\] seed (\d+) parents both A ([\d.]+) B ([\d.]+) TEST A ([\d.]+) B ([\d.]+) '
                r'child both ([\d.]+) TEST ([\d.]+) rounds (\d+) same (\d) peer (\w+)$')
KEEP = re.compile(r'^  \[T-real S2 sel\] both  A=([\d.]+) B=([\d.]+) child=([\d.]+) plain=([\d.]+)$')


def read_b5(path, want_seed, want_peer):
    """the one [b5] line of the file, or None; a line of another seed or peer is an error"""
    if not os.path.exists(path):
        return None
    rows = [m for m in (B5.match(x.rstrip('\n')) for x in open(path, errors='replace')) if m]
    if len(rows) != 1:
        return None
    m = rows[0]
    if int(m[1]) != want_seed or m[10] != want_peer or m[9] != '1':
        return None
    return {'a': float(m[2]), 'b': float(m[3]), 'ta': float(m[4]), 'tb': float(m[5]),
            'c': float(m[6]), 'tc': float(m[7])}


def read_keep(path):
    if not os.path.exists(path):
        return None
    rows = [m for m in (KEEP.match(x.rstrip('\n')) for x in open(path, errors='replace')) if m]
    return float(rows[0][3]) if len(rows) == 1 else None


def g1_seed(r):
    return r['c'] <= min(r['a'], r['b']) - GAIN + 1e-9


def judge(plain, keep, reset, rnd, seeds):
    """rows by seed; returns (ok, lines). A missing row is a fail, never a pass."""
    out = []
    missing = [s for s in seeds if plain.get(s) is None or keep.get(s) is None]
    if missing:
        return False, ['missing seeds %s: no verdict' % missing]
    g1 = [s for s in seeds if g1_seed(plain[s])]
    g2 = [s for s in seeds if plain[s]['c'] < keep[s]]
    g3 = [s for s in seeds if plain[s]['tc'] <= min(plain[s]['ta'], plain[s]['tb'])]
    for s in seeds:
        r = plain[s]
        out.append('seed %d: child %.4f better parent %.4f keep %.4f | TEST child %.4f better parent %.4f'
                   % (s, r['c'], min(r['a'], r['b']), keep[s], r['tc'], min(r['ta'], r['tb'])))
    ok1 = len(g1) == len(seeds)
    ok2 = len(g2) >= NEED
    ok3 = len(g3) >= NEED
    out.append('G1 %d/%d %s' % (len(g1), len(seeds), 'green' if ok1 else 'RED'))
    out.append('G2 %d/%d (need %d) %s' % (len(g2), len(seeds), NEED, 'green' if ok2 else 'RED'))
    out.append('G3 %d/%d (need %d) %s' % (len(g3), len(seeds), NEED, 'green' if ok3 else 'RED'))
    ncok = True
    for name, rows in (('reset', reset), ('random', rnd)):
        have = [s for s in seeds if rows.get(s) is not None]
        red = len(have) == len(seeds) and not all(g1_seed(rows[s]) for s in seeds)
        out.append('NC %s: %d seeds read, G1 %s' % (name, len(have), 'red' if red else 'NOT RED'))
        ncok = ncok and red
    return ok1 and ok2 and ok3 and ncok, out


def selftest():
    def row(c, tc, a=1.8, b=1.9, ta=2.5, tb=2.6):
        return {'a': a, 'b': b, 'ta': ta, 'tb': tb, 'c': c, 'tc': tc}
    S = SEEDS
    good = {s: row(0.3, 1.6) for s in S}
    keep = {s: 0.9 for s in S}
    bad = {s: row(3.0, 3.0) for s in S}
    cases = [
        ('all green', good, keep, bad, bad, True),
        ('one seed misses G1', {**good, 43: row(1.78, 1.6)}, keep, bad, bad, False),
        ('at the G1 line', {**good, 43: row(1.77, 1.6)}, keep, bad, bad, True),
        ('two seeds miss G2', {**good, 43: row(0.95, 1.6), 44: row(0.95, 1.6)}, keep, bad, bad, False),
        ('one seed misses G2', {**good, 43: row(0.95, 1.6)}, keep, bad, bad, True),
        ('two seeds miss G3', {**good, 43: row(0.3, 2.6), 44: row(0.3, 2.6)}, keep, bad, bad, False),
        ('a seed is missing', {k: v for k, v in good.items() if k != 50}, keep, bad, bad, False),
        ('a keep is missing', good, {k: v for k, v in keep.items() if k != 50}, bad, bad, False),
        ('NC reset does not bite', good, keep, good, bad, False),
        ('NC random does not bite', good, keep, bad, good, False),
        ('NC not run', good, keep, {}, bad, False),
    ]
    fails = 0
    for name, p, k, r, n, want in cases:
        got = judge(p, k, r, n, S)[0]
        print('  %s  %s' % ('PASS' if got == want else 'FAIL', name))
        fails += got != want
    line = '[b5] seed 43 parents both A 1.8000 B 1.9000 TEST A 2.5000 B 2.6000 child both 0.3000 TEST 1.6000 rounds 16 same 1 peer plain\n'
    import tempfile
    d = tempfile.mkdtemp()
    p = os.path.join(d, 'x')
    for name, text, seed, peer, want in (
        ('the line reads', line, 43, 'plain', True),
        ('another seed in the file', line, 44, 'plain', False),
        ('another peer in the file', line, 43, 'reset', False),
        ('sides differ', line.replace('same 1', 'same 0'), 43, 'plain', False),
        ('two lines', line + line, 43, 'plain', False),
        ('no line', 'nothing\n', 43, 'plain', False),
    ):
        with open(p, 'w') as f:
            f.write(text)
        got = read_b5(p, seed, peer) is not None
        print('  %s  %s' % ('PASS' if got == want else 'FAIL', name))
        fails += got != want
    os.unlink(p)
    os.rmdir(d)
    print('[b5-judge-self] %s' % ('PASS' if not fails else 'FAIL'))
    return 1 if fails else 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--self':
        return selftest()
    w = sys.argv[1]
    seeds = [int(x) for x in sys.argv[2].split()] if len(sys.argv) > 2 else SEEDS
    plain = {s: read_b5('%s/plain-%d.log' % (w, s), s, 'plain') for s in seeds}
    reset = {s: read_b5('%s/reset-%d.log' % (w, s), s, 'reset') for s in seeds}
    rnd = {s: read_b5('%s/random-%d.log' % (w, s), s, 'random') for s in seeds}
    keep = {s: read_keep('%s/keep-%d.log' % (w, s)) for s in seeds}
    ok, lines = judge(plain, keep, reset, rnd, seeds)
    print('\n'.join(lines))
    print('[b5] %s' % ('ALL GREEN' if ok else 'RED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
