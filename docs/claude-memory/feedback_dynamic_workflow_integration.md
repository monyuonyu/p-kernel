---
name: feedback-dynamic-workflow-integration
description: "Operational lessons for commanding parallel worktree-isolated agents on p-kernel (the \"dynamic workflow\" mk_pino asked for)."
metadata: 
  node_type: memory
  originSessionId: 53149c59-d57f-4589-aa45-bb25220e2df2
---

mk_pino が「あなたが指揮官になってダイナミックワークフローを起動させて」(2026-06-06,
コスト容認)と依頼。並列 worktree エージェントで開発を加速する運用。第1波(p-fs doc /
world map / DNODE_MAX)の実体験から:

**Standing rule (mk_pino 2026-06-10, session-limit economy):** dispatch ALL subagents with
`model: "opus"` (Opus 4.8) — the commander stays Fable 5, agents run on Opus. Set it on
every Agent call going forward; the user need not re-ask.

**Why / How to apply (次の波で必ず効く):**
1. **worktree エージェントは現在ブランチではなく master 基点で起動される。** 第1波で
   A2(DNODE_MAX)は master 基点のまま作業し、regions R0-R2 を欠いた古い dkva.c/kdds.h に
   対して書いたため cherry-pick が衝突(KDDS_TOPIC_MAX を 64→ ではなく 32→48 にしていた)。
   A1/A3 は自力で feat をマージして正解だった。→ **プロンプト冒頭で必ず「最初に
   `git checkout <target-branch>`(または merge)せよ、master で作業するな」と明示する。**
2. **SendMessage はメイン Claude のツールセットに無い**(Agent の説明文には出るが実際は
   呼べない)。→ 走行中エージェントへの差し戻し・継続は不可。**統合・再調整はコマンダーが
   自分の手で**(cherry-pick + 衝突解決)やる前提で設計する。
3. **統合前に各エージェントの base コミットを必ず確認**(`git merge-base`/`git log <commit>^`)。
   master 基点なら手で reconcile、target 基点ならそのまま cherry-pick。
4. **エージェントはビルド成果物を誤コミットしうる**(A2 が `boot/linux_x86_64/p-kernel`
   1.9MB を混入)。統合時に `git rm --cached` で除去。master 基点だと .gitignore 修正も
   欠けている。
5. **エージェント同士の技術判断は矛盾しうる**(A1「CFN_MAX_SEMID は TCB 依存で上げるな」
   vs A2「上げて安全」)。**コマンダーが実物(offset.h/ビルド)で決着させる**。今回は A2 が
   正しかった([[project_regions_architecture]] 参照)。
6. 衝突しにくい切り方: docs-only(p-fs) は完全独立で先行マージ可。code は触るファイルが
   重なる(drpc.h/kdds.h)ので「触った行を報告せよ」と指示し、コマンダーが統合。
7. 検証要求をプロンプトに必ず入れる(4アーキビルド+デモ)。第1波は全員守って成果の質が高かった。

**9. 【2026-06-06 最終確定】真犯人は2人いた — lmkd と Phantom Process Killer (PPK)。PPK は討伐済み。**
一日6回のキルの主犯は Android 12+ の **Phantom Process Killer**(termux-app issue #2366):
全アプリ合計で子プロセス32個が上限、超過分と高CPUプロセスを signal 9 で殺す。
ユーザーが死亡時に「`[Process completed (signal 9)]`」を目撃して確定。
N=32テスト(≈36プロセス)の即死・N=16(<32)の生還・メモリ平穏時の死など、lmkd 説で
説明できない全事象が PPK で説明がつく。**対策実施済み: 開発者向けオプション →
「子プロセスの制限を無効にする」を ON**(S26 Ultra の One UI に存在した。無い機種は
adb で `device_config put activity_manager max_phantom_processes 2147483647` +
`settings put global settings_enable_monitor_phantom_procs false`)。
これで32プロセスの壁は消滅。wake-lock(lmkd対策)と併用が完全体。
メモリ圧(lmkd)は依然あるので、丸一日酷使後は**スマホ再起動で zRAM 排水**(8GB→2.4GB、
available 3.0→6.6GB に回復した実績)。

**8. 【2026-06-06 改訂・実証済み】このホストの並列エージェントは「wake-lock＋Termux前面表示」が生命線。**
環境 = S26 Ultra 上の Termux + proot Ubuntu(11GB RAM, スワップ=ホストzRAM 12GB)。
**4回連続で Android lmkd が Termux ごと SIGKILL**(claude 1体≈400MB)した後、対照実験で決着:
- 死んだ4回: バックグラウンド状態(画面オフ/別アプリ/wake-lockなし)。バッテリー「制限なし」
  設定**だけでは防げない**(あれは standby/Doze 用で lmkd の対象選定とは別系統)。
- **生還した再試験**: `termux-wake-lock`(proot 内から `/data/data/com.termux/files/usr/bin/
  termux-wake-lock` 実行可・exit 0) + **画面に Termux を前面表示したまま** → claude×3
  並列が3体とも完走。しかもピーク圧 8473MB/free 267MB と**死んだ回より深い**圧で生還
  = 殺害判定は絶対圧でなく **oom_adj(前面か否か)**。lmkd は PSI ベース。
**運用規則(再改訂・2026-06-06 第4波後に確定)**: 決定的証言 — ユーザーが**画面オフ・
ポケット・外出(寿司)中**に第4波(claude×3+ビルド連発)が完走した。つまり保護因子は
**wake-lock(+バッテリー制限なし)だけで足りる。画面前面は不要**。一方、**仕事が終わって
アイドルになった後**のセッションは遅かれ早かれ刈られる(防止不能・実害なし)。
→ 標準手順: ①波の前に wake-lock 取得(proot 内から `/data/data/com.termux/files/usr/bin/
termux-wake-lock` 実行可)②メモリロガー装着③同時数の上限は**ユーザー指示で撤廃済み
(2026-06-06、PPK無効化+wake-lock体制の実証後)** — ただしメモリロガーで avail を見ながら、
2GB を切ったら新規出撃を控える④即コミット・即push⑤ユーザーは放置してよい⑥アイドル後に
セッションが死んでいたら再起動して続きから(使い捨て前提)。
「ノードは死ぬがネットワークは記憶している」を地で行く運用。
NODE_OPTIONS=--max-old-space-size=3072 は /root/.bashrc 設定済み。
proot 内から dmesg/logcat は不可(検死はメモリロガーで)。

**9. 【2026-06-07 第10波で確定】夜間仕込み運用 → 再開時は origin の未知ブランチを必ず確認。**
mk_pino は就寝前にエージェント(複数)を仕込んで放置する運用を始めた。翌朝セッションを再開した
私は、それを知らず同じ課題で同数の隊を出し、**6隊が同じ3課題を二重実装**した(損失は compute
のみ、master は無傷)。→ **セッション再開直後・新規出撃の前に必ず `git ls-remote --heads origin`
で未知の作業ブランチ(命名規約 wN-* 等)を確認**し、走っている/完了した仕事と重ねない。重複が
起きたら裁定基準: 先着 or カバレッジの広い方を採用し、もう一方の**差分価値(ボーナス発見)だけ
follow-up として温存**(branch を消さずに残す)。今回 honesty採用×C隊のrx-replay窓温存、
concurrent採用×今朝版のトピック予算懸念は実機N=16検証で杞憂と判明し破棄、が実例。
**10. cwd すり替わり＋ブランチすり替わりの罠（最重要・2回昇格）**: 完了/稼働中の agent worktree に
cwd が残ると `git merge`/`make` が worktree 側に効き "Already up to date"/誤コミットになる。
**さらに第12波で悪化**: `cd /root/p-kernel` しても**メインチェックアウト自体が agent のブランチ
(w12-durable-pfs)に乗っていて**、B隊マージをそのブランチに誤着地させた(push前に検知・復旧、master/
origin 無傷)。原因推測: agent の `git checkout -b` がブランチを共有 repo に作り、何かの拍子にメイン
チェックアウトの HEAD がそれに移った(＝agent はそのブランチ名を使えず worktree-agent-* に退避していた
のが痕跡)。**鉄則: マージ/コミット前に必ず `git branch --show-current` で master を確認してから実行**
(cd だけでは不十分)。誤着地したら `git checkout master` → 汚染ローカルブランチ `git branch -D` →
正しく master へ再マージ。第7・10・12波で発生、すべて push 前検知で実害ゼロ。
**11. 全エージェントが Edit/Write/Monitor 権限denyの環境だった**(2026-06-07)。各隊は bash
(python3 厳密文字列置換 + heredoc)で編集に fallback し成果は正常。指揮官(本セッション)側の
Edit/Write は使えるので、統合時のコンフリクト解決・doc追記は通常どおり可。
