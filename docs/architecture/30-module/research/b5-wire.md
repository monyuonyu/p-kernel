# B5 の配線: 常駐の student を領域の仲間と合体する（設計、docs だけ）

2026-11-15 17:00 の回（#43 の 1 の 3 つ目）。未実装。梯子のまとめは b5-robust-summary.md（feat/b5-nc1-cut）。

## 1. 今あるもの（読んだ）

- 常駐の student（student_shell.c の g_student）の合体 SS-3 は**自分の写し**とだけ合体する。仲間の重みは一度も使っていない
- 仲間の student を運ぶ口 `gl_student_publish`／`gl_student_fetch`（gossip_learn.c、hosted は名前つきの記述子＋索引、欠けた塊は断る）は試験 run_ss3_blob.sh だけが呼ぶ
- 守り: 第 5 歩 `st_round_trimmed`（判定 PASS、t=1 は 4 台以上でないと断る）、門 `st_round_adopt`（自分の窓で損失が下がったときだけ入れ替え）。第 10 歩 `st_merge_trust_group` は前の周が要る＝第 2 段
- student_shell.c はベアのビルドにも入る＝足すものは全部 `_TK_HOSTED_LIBC_` の中（crown は動かないはず、測って確かめる）

## 2. 作るもの（第 1 段）

- `baby region`（シェルの語）: ① 自分の blob を publish（記述子に世代 gen を足して版 2、版 1 は読む側が断る）② `region_contains` の仲間を fetch、gen が自分と同じものだけ ③ 写しに `st_round_trimmed(t=1)` ④ `st_round_adopt` を自分の保持の窓で ⑤ 入れ替える前に `student_consol_abort()`（SS-3 と同じ書き手の守り）。1 行 `[baby] region: members m same-gen k accepted a adopted 0|1 held x -> y`
- 4 台に満たない・仲間 0・fetch 失敗は断り、g_student は全バイト不変（平均に落とさない）
- 周期の自動化（mind_merge_task の形）は第 2 段。第 1 段は語だけ

## 3. 合否（作る前に固定）

試験 tests/host/run_b5_wire.sh（1 プロセスに 4 つの node id、本物の pfs と publish/fetch。各ノードは教材の四分の一で学ぶ）。物差しは b1_held（選びに使わない）。判定の種は 181〜188（未使用）
- **W1** 正直な 4 台: 採用され、node 0 の b1_held が 1 台だけより良い（7 種以上）
- **W2** 1 台がでたらめ ×8: node 0 が 1 台だけ ＋0.03 以内（7 種以上）、かつ単純平均の腕より良い
- **W3** 3 台: 断り、g_student 全バイト不変
- **W4** 1 台の gen が古い: その仲間を数えない（same-gen 3、断り）
- **W5** 1 台の塊を欠く（`gl_student_test_drop_chunk`）: その仲間を数えない
- 陰性: `-DB5W_NC_MEAN`（切り詰めの代わりに st_merge_cohort）で W2 赤、`-DB5W_NC_NO_GATE`（門なし）で W2 か W1 赤、`-DB5W_NC_NO_GEN`（gen を見ない）で W4 赤
- crown: x86・aarch64 の .text・.rodata が master と全バイト同じ。build_all

限界: 1 プロセス・網なし（中継越しは第 2 段）、4 台、全知の近い毒には第 10 歩が要る（第 2 段）。門の窓は自分の保持の窓＝ログの held と同じ窓（SS-3 と同じ限界、独立の物差しは b1_held）

## 4. 監査-183（2026-11-15、限界つき PASS。作る回への条件）

§1 は 4 つともコードどおり（呼び手は student_blob_test.c だけ、fetch は版 ≠1 を断る、trimmed は自分を含め 4 未満で 0）。条件: ① **NO_GATE は W1・W2 で赤にならない見込み**（第 5 歩で毒 1 台は切り詰めだけで守れた）＝**W6: でたらめ 2 台で adopted 0・g_student 全バイト不変**を足し、NO_GATE は W6 で赤 ② W2 は adopted 1 も見る（NC_MEAN でも門が断れば 1 台と同じ値で緑になる） ③ 1 プロセスでは pfs は 1 つ（shim で put は手元だけ）＝分かれるのは名前 `st/<node>` だけ ④ W5 の欠く塊は w の範囲（mu・vu はゼロで塊が他と同じになりうる）。既存の旗 ST_ADOPT_ALWAYS・B5S5_TRIM_OFF を使ってよい
