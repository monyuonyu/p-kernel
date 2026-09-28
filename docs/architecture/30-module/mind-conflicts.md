# 知識の矛盾の記録（ROADMAP 1-3、設計メモ）

2026-09-28。global-rules.md の決まり2「履歴と今の信念を分ける」を作る。的は MIND-GEN-0-B の E4（負けた方が残る）と E2（勝ち負けが到着順）。

## 今どうなっているか
- 同じ鍵に違う値が来ると `r3_fact_revise`（r3_incontext.c）が engram をその場で上書きする。出どころの表（mt_rprov・mq_lprov）も新しい教え手に付け替わる。負けた値・教えたノード・どれだけ固く覚えていたかは、どこにも残らない
- 勝ち負けの規則は「最後に来た方」だけ（mind_net_task の Site 2、m_teach の Site 1）

## 段階
1. **1-3a 履歴（この段）**: 上書きの直前に `mc_note` が負けた方を別の表 `mc_hist`（8件の輪、同じ組は回数だけ増やす、落ちた数も数える）に写す。残すもの: 鍵・負けた値・勝った値・教えたノード・その prov の content-id・負けた engram の状態と固化の回数（証拠）・時刻。読み口: `mind conflicts`（一覧）と `mind ask` の `conflict history` の行。**今の答えの選び方は変えない**
2. 1-3b 決まった規則: 到着順でなく、どのノードでも同じ勝ちになる規則。候補は (教えたノード, 値) の小さい方（どの順で届いても同じ）。ただし「後からの訂正」と「同時の食い違い」を区別できない（因果の情報が無い）。勝った側が負けた側へ送り返す仕組みも要る（今は上書きで再送が止まる）
3. 1-3c 履歴を伝える: 新しく加わったノード（E4 のノード6）は勝った値しか受け取らないので、履歴が届かない

## 決めたこと
- **1-3a は hosted だけ**（`_TK_HOSTED_LIBC_`）。ベアメタルの crown は2つの re-bless 待ち（RNG0・CPU の予算）があり、重ねない。多ノードの試験も hosted にしか無い。ベアメタルへは 1-3b と一緒に（re-bless つき）
- 表は R3_FACT の外（R3_FACT は 24B の _Static_assert）

## どう測るか
- `tests/host/run_mind_conflicts.sh`: local（1台で sky blue → green）と remote（A が blue、B が green。B は「node 0 が教えた blue」、A は「自分の blue が node 1 の green に負けた」を残す）。直す前の master で赤、直した後に緑を両方示す
- MIND-GEN-0-B: E4 はノード1・3・5 で行が出るはず、ノード6 は 1-3c まで赤のまま（予想）
