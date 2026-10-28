# 賢い合体 第7段: init の腕は後半の重みの平均を返す（設計）

2026-10-28（設計。種 14〜21 はまだ走らせていない）。[brain-merge-blr.md](brain-merge-blr.md) の続き。

## 1. 分かっていること（探り、見た種 0xC0FFEE・2・8〜13 だけ。`pk-scratch/d5c7/`）

- VAL・TEST は各 4 窓（96 B）しかない
- B_LR 0.02 の init の腕は種 8・11 で発散する（VAL 5.4＝祖先並み）。第6段の選びは守りの pick に戻った腕も門の緑に数えて 0.02 を選んだ
- 3e-3 の init の腕は TEST が 800 歩まで下がり続けるのに、VAL の最良は 280〜600 歩で止まる。S1 の赤（種 0xC0FFEE・2・8）はこの止め方で出る
- 3e-3 で後半の重みを平均（10 歩ごと 40 点）すると S1 の TEST が線の下 7/8（種 2 は 0.0025 届かない）、S2 は 8/8。pick の子と VAL で選び合わせても 7/8・8/8。VAL の最良の形は S1 5/8
- 種 11 は先生（2 親の logit の平均）の TEST 1.255 が親 B の 1.234 より悪い＝先生をまねるだけでは s1-gain に届かない。それでも平均の子は 1.007
- 原文: SWA（Izmailov ほか、arXiv:1803.05407 §3.2・Algorithm 1）は学習済みの点から一定の学習率で続け、epoch ごとの重みを平均する。ここは祖先から蒸留し、前半を ŵ までの学習とみなして後半を半 epoch（10 歩）ごとに平均する。Adam で BN なしは論文と違う

## 2. 決めたこと

- init の腕: B_LR 3e-3 固定（選びの段は無くす）、ROUNDS×TW 歩。途中の VAL の最良は使わず、steps/2 より後の 10 歩ごとの重みの平均（double で足す＝決定的）を返す。返す前に守りの pick と VAL で比べ、下回らなければ pick（安全網は残す）
- pick の腕と start=sel の選び（VAL の低い方）は変えない
- student.c に新しい start `ST_DISTILL_START_INIT_SWA` と、それを init の腕に使う `st_merge_distill_sel_swa`（引数は st_merge_distill_sel と同じ）。既存の関数の出力は変えない＝第6段の cert の既定の出力が全行同じことを確かめる
- 試験は [walk S init] に平均の点の数と VAL を出し、`[d5c-teacher-reach] S1`（先生の TEST ≤ 良い方の親 − 0.03）を記録として出す（判定に入れない）
- hosted だけ（ベアは student.c を作らない＝crown 不変を実測で）。CI の外

## 3. 合否（コードより先に固定）

- 判定の種は 14〜21（8 個、誰も走らせていない）。B_LR・平均の始まりと間隔は見た種で決めた＝判定の種では何も選ばない
- 主（研究の判定）: 平均の plain が 8 種すべてで start=sel の全部の門（parents-ok・sel-val・s1-gain・s2-both・floor・determ）緑なら PASS。1 行でも赤なら赤
- 副（改善か）: 同じ 8 種で第6段の形（B_LR 3e-3、VAL の最良）を比べの腕に走らせる。S1 の子の TEST が平均の方で小さい種が 6 以上、かつ s1-gain の緑の数が比べの腕以上なら「改善」。欠ければ「改善とは言えない」
- NC: SEL_INVERT（sel-val が赤）、PARENT_LR_OLD（種 14・15 で parents-ok が赤）、SWA_FIRST（前半を平均）は S1 の子の TEST が平均の方より悪い種が 6 以上なら「後半であることが効く」、満たさなければそう書く
- teacher-reach が赤の種は名前を書く。判定は変えない

## 4. 見込み（外れてよい。線は動かさない）

見た種で 7/8 なので主は赤になりうる（種 2 のような僅差）。副は通る見込み。所要は plain・比べ・SWA_FIRST・SEL_INVERT を 8 種＋PARENT_LR_OLD 2 種＝34 本、4 並列で約 70〜80 分。
