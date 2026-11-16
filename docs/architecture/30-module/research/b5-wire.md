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
- **W6**（§4 ①）でたらめ 2 台: 切り詰めには入る（accepted 3）が adopted 0、g_student 全バイト不変。W2 は adopted ≥1 も見る（§4 ②）
- 陰性: `-DB5W_NC_MEAN`（st_round_merge）で W2 赤、`-DB5W_NC_NO_GATE`（門なし）で W6 赤、`-DB5W_NC_NO_GEN`（gen を見ない）で W4 赤
- crown: x86・aarch64 の .text・.rodata が master と全バイト同じ。build_all

限界: 1 プロセス・網なし（中継越しは第 2 段）、4 台、全知の近い毒には第 10 歩が要る（第 2 段）。門の窓は自分の保持の窓＝ログの held と同じ窓（SS-3 と同じ限界、独立の物差しは b1_held）

## 4. 監査-183（2026-11-15、限界つき PASS。作る回への条件）

§1 は 4 つともコードどおり（呼び手は student_blob_test.c だけ、fetch は版 ≠1 を断る、trimmed は自分を含め 4 未満で 0）。条件: ① **NO_GATE は W1・W2 で赤にならない見込み**（第 5 歩で毒 1 台は切り詰めだけで守れた）＝**W6: でたらめ 2 台で adopted 0・g_student 全バイト不変**を足し、NO_GATE は W6 で赤 ② W2 は adopted 1 も見る（NC_MEAN でも門が断れば 1 台と同じ値で緑になる） ③ 1 プロセスでは pfs は 1 つ（shim で put は手元だけ）＝分かれるのは名前 `st/<node>` だけ ④ W5 の欠く塊は w の範囲（mu・vu はゼロで塊が他と同じになりうる）。既存の旗 ST_ADOPT_ALWAYS・B5S5_TRIM_OFF を使ってよい

## 5. 実装（11-15 20:30 の回、feat/b5-wire。監査は別の回）

- 芯は student.c の `st_region_round`（取り寄せは関数で渡す＝試験は本物の pfs と表の両方で回せる、W0 で全バイト同じ）。シェルは student_shell.c の `baby region [gen]`（狭い番号 0〜63 の仲間、gen は呼び手が渡す）
- **§1 の 4 つ目は誤りだった**: student_shell.c はベアに入らない（ベアの Makefile は弱い空の定義で済ませる、と注にある）うえ、LLM の層は `_TK_HOSTED_LIBC_` なしでコンパイルされる＝そのガードの中に置いた最初の版は語が消えていた（本物のノードで `baby region 3` が普通の学習の周に落ちた）。ガードを外した
- **§2 ① を変えた**: gen は記述子の版 2 ではなく、塊の前の 16 バイトの封（`BRG1`・gen・長さ）に入れた。運びの形（gl_student_*、run_ss3_blob）を変えずに済む。封の無い・長さの合わない・gen の違う塊は数えない
- 試験 tests/llm/run_b5_wire.sh（`B5W_MODE=unit` は数秒、CI に段）、判定役 b5_wire_check.py。W1・W2 の腕は表から（網の時間を除く）。結果は §6

## 6. 結果（実装者の回の測り。監査は別の回）

- 試験だけ 44ea14a7: リンクで赤。unit（985d34c3）: W0（pfs と表で全バイト同じ、accepted 3）・W3〜W6 緑、NO_GEN は W4 だけ・NO_GATE は W6 だけ赤、nomean は W3〜W5 赤（4 未満の断りが無い）
- **判定の種 181〜188 で PASS**: W1 8/8（1 台より 0.21〜0.42 良い）、W2 8/8（毒 1 台で 1 台より 0.18〜0.32 良い、took 6〜7/8）、NC_MEAN 0/8＝赤。平均の腕と毒 2 台の腕は 8 種とも 1 台と全桁同じ（took 0/8）＝W2 の「平均より良い」は「1 台より良い」と同じ
- crown（ベア x86・aarch64 の .text・.rodata）master と全バイト同じ、hosted x86_64 警告 57（master と同じ）、aarch64 hosted ビルド OK。本物のノード（網なし）で `baby region 3` は「not in a mesh」で断り、前後の損失 5.5270 で同じ
- 限界: 網越しの publish・region_contains・M の塊（22.8 MB を仲間ごとに確保）は通していない＝第 2 段。門の窓は kept を選ぶ窓と同じ（物差しは b1_held）。見習いなし（第 5 歩の形と違う）

## 7. 監査-184（2026-11-16、限界つき PASS。作った回とは別の回）

- 容器で再現: 先端 6177e430 の unit PASS（NO_GEN は W4 だけ・NO_GATE は W6 だけ赤）、44ea14a7 はリンクで赤（st_region_round・st_rg_cap・st_rg_wrap なし）。種 184 を取り直し、plain・nomean とも実装者のログと全バイト同じ（判定役は 8 種のうち 7 を要るので、1 種だけの取り直しは FAIL と出る＝仕様）。crown 先端と master で x86・aarch64 の .text・.rodata 全バイト同じ（cd490552・4dfac3f4）
- 私の壊し: self を仲間に数える → W0（数）と W5 赤。切り詰め t=0 → W0・W6 赤。**封の長さ（h[2]）を見ない → 全段緑**＝その確かめは試験の外（st_load 側で断られるかは見ていない）。週 1 回の見直しの回へ（H29）
- 限界（実装者の申告どおり）: W2 の比べる相手（平均の腕）は門に全部断られて 1 台と同じ＝歯が無い。網越しは 1 周だけ。読んで見つけた: st_region_round は members の重複を除かない（シェルは 0〜63 を 1 回ずつ回すので今は起きない、第 2 段で関数の側で除く）。試験では self の塊を公開しないので、self を数える壊しは数でしか捕まらない（本物のノードは先に publish する）。仲間の塊は全部を同時に確保（最大 63×M）＝第 2 段の上限
