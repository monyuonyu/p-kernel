#!/usr/bin/env python3
"""この機体の自己ホストの runner（pkernel-thinkpad）が CI のジョブを走らせている・待たせているかを見る。

  python3 tools/cimetrics/selfhosted_busy.py && echo FREE

終わり値 0 = 空き（重い測りを始めてよい）、1 = 走行中か待ちのジョブがある（名前を出す）、2 = gh で読めない。
理由: promote を止めた揺れのうち 5 件は自己ホストのジョブで、region の τ 50 ms を RTT が越えた形。
そのうち 2 回は無人開発の容器の測りと同じ時刻だった（research/ci-red-causes.md）。
自己ホストのジョブは 1 台に 1 本ずつ並ぶので、run が始まってから約 60 分かかる（44930698175: 17:33〜18:32Z）。
"""
import json, subprocess, sys

REPO = 'monyuonyu/p-kernel'
LABEL = 'pkernel-thinkpad'


def gh(path):
    p = subprocess.run(['gh', 'api', path], capture_output=True, text=True, timeout=60)
    if p.returncode != 0:
        raise RuntimeError(p.stderr.strip()[:200])
    return json.loads(p.stdout)


def expected_jobs():
    """ci.yml の自己ホストのジョブの数。ジョブは needs: の連鎖で前が終わってから作られる＝
    API に出ている数だけを見ると、ジョブとジョブの間や始まる前に「空き」と言ってしまう。"""
    import os, re
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    pat = re.compile(r'^\s*runs-on:.*\b%s\b' % re.escape(LABEL))
    return sum(1 for l in open(os.path.join(root, '.github/workflows/ci.yml'), encoding='utf-8') if pat.match(l))


def main():
    busy = []
    want = expected_jobs()
    try:
        # 状態で絞らない: ジョブが runner を待つ間、run は in_progress・queued 以外（waiting・pending・
        # requested）にもなる。11-22 05:11 に status=in_progress と queued だけを見て、ジョブとジョブの
        # 間に FREE と言った（二層カップリングの後、並列推論の前）。
        allr = gh('repos/%s/actions/runs?per_page=30' % REPO).get('workflow_runs', [])
        if True:
            runs = [r for r in allr if r.get('status') != 'completed']
            print('[selfhosted-busy] runs not completed: %s' % ' '.join('%d:%s' % (r['id'], r['status']) for r in runs))
            for r in runs:
                page, done = 1, 0
                while True:
                    js = gh('repos/%s/actions/runs/%d/jobs?per_page=100&page=%d' % (REPO, r['id'], page))
                    jobs = js.get('jobs', [])
                    for j in jobs:
                        if LABEL in (j.get('labels') or []):
                            if j.get('status') != 'completed':
                                busy.append((r['id'], j['status'], j['name'][:70]))
                            else:
                                done += 1
                    if len(jobs) < 100:
                        break
                    page += 1
                if r.get('name') == 'CI' and done < want:
                    busy.append((r['id'], 'to-come', '%d of %d self-hosted jobs not finished yet' % (want - done, want)))
    except Exception as e:
        print('[selfhosted-busy] cannot read:', e)
        return 2
    seen = set()
    for b in busy:
        if b in seen:
            continue
        seen.add(b)
        print('[selfhosted-busy] run %d %s %s' % b)
    if busy:
        print('[selfhosted-busy] BUSY %d' % len(seen))
        return 1
    print('[selfhosted-busy] FREE')
    return 0


if __name__ == '__main__':
    sys.exit(main())
