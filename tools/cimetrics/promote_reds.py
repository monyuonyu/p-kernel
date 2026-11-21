#!/usr/bin/env python3
"""promote の止まり方を週ごとに数える（inbox #44 の 4: 片づけの前後の比較）。

  python3 tools/cimetrics/promote_reds.py [--log ~/pkernel-promote.log] [--since 2026-11-22] [--until 2026-11-29]

promote.log の行（「master を進めた」「master に入れなかった。新しい赤 N: A / B ...（run R）」）を読み、
期間の 進めた回・止まった回・止まった回の中のジョブごとの回数 を出す。
promote は新しい赤の名前を " / " でつなぐが、ジョブ名そのものにも " / " がある（「接続フォールバック cert (heartbeat / UDP↔TCP / ...)」）。
そこで ci.yml の name: を先に読み、つないだ文字列をジョブ名の並びに戻す（戻せない切れ端は「?」を付けて残す）。
原因の区分（本物・負荷・runner）はここでは付けない＝ログと直しで決める（docs/architecture/30-module/research/ci-red-causes.md）。
--selftest で名前の戻し方を確かめる。
"""
import argparse, os, re, sys
from collections import Counter
from datetime import date

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def job_names(ci_yml):
    names = []
    for line in open(ci_yml, encoding='utf-8'):
        m = re.match(r'^    name: "?(.*?)"?\s*$', line)
        if m:
            names.append(m.group(1))
    return names


def split_jobs(joined, names):
    """'A / B (x / y) / C' をジョブ名の並びに。promote は長い名前を ... で切ることがある。"""
    parts = joined.split(' / ')
    out, i = [], 0
    while i < len(parts):
        best = None
        for j in range(len(parts), i, -1):
            cand = ' / '.join(parts[i:j])
            core = cand[:-3] if cand.endswith('...') else cand
            if any(n == cand or (cand.endswith('...') and n.startswith(core)) for n in names):
                best = (j, cand)
                break
        if best:
            out.append(best[1]); i = best[0]
        else:
            out.append('?' + parts[i]); i += 1
    return out


def read_log(path):
    ev = []
    for line in open(path, encoding='utf-8'):
        m = re.match(r'^(\d{4}-\d\d-\d\d)T\S+ (\w+): (.*)$', line.strip())
        if not m:
            continue
        d, sha, rest = m.groups()
        if 'master を進めた' in rest:
            ev.append((d, sha, 'promoted', None))
        elif '入れなかった' in rest:
            r = re.search(r'新しい赤 \d+: (.*)（run (\d+)）', rest)
            ev.append((d, sha, 'blocked', r.group(1) if r else ''))
    return ev


def selftest(names):
    j = 'UMP x86_64 (ビルド + 自己テスト) / 接続フォールバック cert (heartbeat / UDP↔TCP / スーパーノード自動昇格)'
    got = split_jobs(j, names)
    ok = got == ['UMP x86_64 (ビルド + 自己テスト)', '接続フォールバック cert (heartbeat / UDP↔TCP / スーパーノード自動昇格)']
    got2 = split_jobs('ひとつの心 / Path W (実機3ノード: 分岐した重みを 84KB チャンク経由でマ...', names)
    ok2 = len(got2) == 1 and not got2[0].startswith('?')
    got3 = split_jobs('no such job / UMP x86_64 (ビルド + 自己テスト)', names)
    ok3 = got3 == ['?no such job', 'UMP x86_64 (ビルド + 自己テスト)']
    for o, g in ((ok, got), (ok2, got2), (ok3, got3)):
        print('[promote-reds-self]', 'ok' if o else 'FAIL', g)
    return ok and ok2 and ok3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', default=os.path.expanduser('~/pkernel-promote.log'))
    ap.add_argument('--ci', default=os.path.join(ROOT, '.github/workflows/ci.yml'))
    ap.add_argument('--since', default='2000-01-01')
    ap.add_argument('--until', default='2999-12-31')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    names = job_names(a.ci)
    if a.selftest:
        sys.exit(0 if selftest(names) else 1)
    ev = [e for e in read_log(a.log) if a.since <= e[0] <= a.until]
    weeks = Counter(); jobs = Counter()
    for d, sha, kind, joined in ev:
        y, w, _ = date.fromisoformat(d).isocalendar()
        weeks[('%d-W%02d' % (y, w), kind)] += 1
        if kind == 'blocked':
            for n in split_jobs(joined, names):
                jobs[n] += 1
    print('# 期間 %s .. %s  promoted %d  blocked %d' % (a.since, a.until,
          sum(1 for e in ev if e[2] == 'promoted'), sum(1 for e in ev if e[2] == 'blocked')))
    for wk in sorted({k[0] for k in weeks}):
        print('%s  promoted %2d  blocked %2d' % (wk, weeks[(wk, 'promoted')], weeks[(wk, 'blocked')]))
    print('# 止まった回の中のジョブ（回数）')
    for n, c in jobs.most_common():
        print('%3d  %s' % (c, n))


if __name__ == '__main__':
    main()
