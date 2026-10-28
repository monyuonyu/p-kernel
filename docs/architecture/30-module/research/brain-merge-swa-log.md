# 賢い合体 第7段の記録（実装と cert）

設計は [brain-merge-swa.md](brain-merge-swa.md)（合否は 6a24350b で固定、ここでは動かさない）。

## 実装（2026-10-28 19:05 の回、実装者。監査は別の回）

- 設計の読み直し: 穴は 1 つだけ。§3 の sel-val は「選んだ子が単独の出発点の子と全バイト同じ」を見る＝単独の init の子も SWA で作らないと必ず赤。試験の `merge()` で `-DD5C_SWA` のとき init も `ST_DISTILL_START_INIT_SWA` にした（設計の線は変えていない）
- 試験先 029ebd10: `-DD5C_SWA` で `[d5c-swa-points]`（init の腕が steps/10 − (steps/2)/10＝40 点を平均したか、門）と `[d5c-teacher-reach] S1`（先生の TEST ≤ 良い方の親 − 0.03、記録だけ）。student.c は新しい start を受けるだけで平均しない版
- 実装 2ffa44fa: `st_merge_distill` に `ST_DISTILL_START_INIT_SWA`（祖先から、10 歩ごとに steps/2 より後の重みを double で足す、平均の VAL が守りの pick より低いときだけ返す）、`st_merge_distill_sel_swa`（第6段と同じ選びを共通の関数 `st_distill_sel_run` で）。`-DST_DISTILL_SWA_FIRST` は前半を平均（NC）
- cert `tests/llm/run_st_distill_swa.sh`（CI の外）: plain・cmp（第6段の形、B_LR 3e-3）・SWA_FIRST・SEL_INVERT を種 14〜21、PARENT_LR_OLD を 14・15

## 結果（容器 /build/d5c7s、19:09〜20:14、4 並列で 34 本・各 1 回。出力 `pk-scratch/d5c7s/d5c7s-out.tgz`）

- 試験先 029ebd10（種 0xC0FFEE）: `[d5c-swa-points]` S1・S2 とも赤（swa_points=0）。既定のビルド（`-DD5C_SWA` なし）の出力は master 5dad93b8 の試験と全バイト同じ（only-sel・full の両方）。ベアの .text は master と同じ（x86 f55b1b88 379967・aarch64 9f4400ea 361980）
- **主＝赤**: 種 14〜19・21 は 14 行すべて緑、**種 20 は `[d5c-parents-ok] S1` だけ赤**（親 B が自分の最小 0.355 から 0.677 に戻った＝親の学習率 3e-3 でも戻る種がある。合体の門 s1-gain・s2-both・floor・determ・sel-val・swa-points は種 20 も緑）。cmp も種 20 で同じ行が赤
- **副＝改善**: S1 の子の TEST は 7/8 で平均の方が低い（種 21 だけ 1.2259 対 1.1557）。s1-gain の緑は平均 8 対 cmp 6（cmp は種 14 −0.067・17 0.025 で赤）
- NC: SEL_INVERT は 8 種 16 行すべて sel-val 赤、PARENT_LR_OLD は種 14・15 で parents-ok 赤（数える 2 つは効く）。SWA_FIRST は 7/8 で平均の方が良い＝後半であることが効く（種 15 は S1 で init の腕が守りの pick に戻り、両方とも pick の子＝同じ値）
- teacher-reach: 8 種とも reach（種 11 の形は判定の種には出なかった）
- 注: 種 15 の S1 は平均の VAL が守りの pick を下回らず fell_back=1。平均の子は VAL では VAL の最良に負けることが多い（例: 種 20 S2 1.937 対 1.697）のに TEST では勝つ＝VAL 4 窓の物差しの粗さは残る

## 監査-97（10-29 01:00 と 05:05 の回、限界つき PASS・道具として）

- 前半: 差分を読み、既定ビルドの出力は master と全バイト同じ（only-sel 33 行・full 120 行）。壊し方: `s > steps/2` を `>=`→swa-points 赤。**平均を n_avg−1 で割る→全部緑（悪くなるが門は越える）・VAL の安全網を外す／float で足す→この種では出力が同じ＝試験の穴**
- 後半: cert を取り直し（3 並列・nice 19）、腕のログ 34 本が全バイト同じ・判定 301 行が同じ（違いは時刻と並列数の行だけ）。研究の判定は赤のまま。次の段で門を足すなら「平均の重みが 40 点の重みの最小と最大の間にある」のような仕組みの門
