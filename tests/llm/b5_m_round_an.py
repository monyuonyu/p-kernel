# analyse m3's samp.tsv: per arm, chunk-file growth rate per node in windows after the fetch start,
# CPU share (user/sys, ticks of 1/100 s per 5 s window), relay CPU. usage: m3_an.py samp.tsv m3.txt
import sys, collections
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1])][1:]
t1 = None
for l in open(sys.argv[2]):
    if l.startswith('fetch start'): t1 = int(l.split()[2])
by = collections.defaultdict(list)
for r in rows:
    if r[4] == 'x': continue
    by[(r[1], r[2])].append(r)
for arm in ('fs', 'nf'):
    print('== arm', arm, '(t from fetch start', t1, ')')
    for node in ('1', '2', '3', '4', '5', 'relay'):
        s = by.get((arm, node), [])
        if not s: continue
        out = []
        for i in range(1, len(s)):
            a, b = s[i - 1], s[i]
            t = int(b[0]) - (t1 or 0)
            dt = int(b[0]) - int(a[0]) or 1
            cpu_u = (int(b[4]) - int(a[4])) / dt   # ticks/s = % of a core
            cpu_s = (int(b[5]) - int(a[5])) / dt
            rate = (int(b[3]) - int(a[3])) / dt if node != 'relay' else 0
            out.append((t, rate, cpu_u, cpu_s, b))
        # summarize per 60 s bucket after fetch start (and the last pre-fetch minute)
        bk = collections.defaultdict(list)
        for t, rate, u, sy, b in out:
            if t < -60: continue
            bk[t // 60].append((rate, u, sy))
        line = []
        for k in sorted(bk):
            v = bk[k]
            line.append('%d:%.0f/%.0f+%.0f' % (k, sum(x[0] for x in v) / len(v), sum(x[1] for x in v) / len(v), sum(x[2] for x in v) / len(v)))
        last = s[-1]
        print(' n%s files_end=%s gaveup=%s sent=%s repl=%s region=%s' % (node, last[3], last[6], last[7], last[8], last[9]) if node != 'relay' else ' relay')
        print('   min:chunks/s / cpu%user+sys  ', ' '.join(line))
