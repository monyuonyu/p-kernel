# N3 第 2 段: dnode_table を領域の大きさへ（設計、2026-11-22）

ROADMAP N3 の 2 本目の表。この文書と試験先だけで、製品のコードはまだ同じ。

## 1. いまの形（master 64d51ff2）

- `DNODE dnode_table[DNODE_MAX]`（drpc.c、ip・state・missed・node_id）を狭い番号で引く。heartbeat は全員に送り、全員の行を持つ（SWIM の枠に入らない台もここには居る）
- 引く所: 約 200 か所（state 163）。書くのは drpc.c・swim.c と、自己試験の中の gossip_learn・pmesh・spawn
- 数字は §4

## 2. 作る形

1. **2a 口をそろえる**: drpc.h に `dnode_state(n)`・`dnode_ip(n)`・`dnode_missed(n)` と書く口、行を回す `DNODE_FOREACH`。drpc.c と swim.c の外は口だけを使う。中身は今の配列のまま、口は**マクロ**（§4: static inline だと .text が動いた）＝ベアも hosted も .text の全バイト同じ
2. **2b 中身を枠へ（hosted だけ）**: 行を SWIM の枠 `SWIM_PEER` に入れ、表を 1 本・地図を `sw_map` 1 本にする。行の無い番号は UNKNOWN・ip 0。heartbeat を聞いたら枠を取る（取り方は第 1 段と同じ、満杯なら断って数える）。heartbeat を送るのも年を取らせるのも行の上だけ（O(R)）
3. ベアは第 1 段と同じく番号＝枠のマクロ。crown の全バイト同じを狙い、動いたら判断待ち（監査 PASS を条件に re-bless がおすすめ）
4. `tests/unbounded/coupling_check.sh` は「番号が広がった」の証拠に `dnode_table_bytes` の伸びを使っている。2b で平らになるので、同じコミットで証拠を `DNODE_MAX` そのものに替える

決めたこと: 枠を SWIM と分けず 1 本に。2 本だと「SWIM は生きていると言うが行が無い」が起き、枠の選び方（ハッシュ・墓）を 2 度作る。

## 3. 合否（作る前に固定、2026-11-22）

- **D1** `tests/unbounded/dnode_coupling.sh`: drpc.o の data+bss が DNODE_MAX 64/256/1024 で同じ。master で赤の見込み
- **D2** 同じ台本: drpc.c・swim.c の外の arch のコード行（注釈の行を除く）に `dnode_table[` が 0。master で赤の見込み（2a で緑）
- **D3** 陰性: `N3D_NC=1`（drpc.o に [DNODE_MAX] の配列を 1 本押し込む）で D1 赤、`N3D_NC=2`（`dnode_table[` を直に引く 1 行の .c を足す）で D2 赤
- **D4** 出荷の形（64）で変わらない: 2a は crown 全バイト同じ・hosted の .text 同じ。2b は crown 同じ、SWIM の自己試験・run_n3_swim_cap.sh・run_n2c_wide.sh・build_all 緑
- **D5** `run_n3_swim_cap.sh` に C10（`N3C_DNODE=1` のときだけ）: 12 台・枠 8 で、どの台も最後の `[cluster]` 表の SELF 以外の行が 8 以下。master で赤の見込み（11 行）。2b で CI に `N3C_DNODE=1` を入れる。陰性 `-DN3D_NC_FLEET`（drpc が配列を使い続ける）で赤

D1・D2 だけでは足りない（第 1 段の教え）: 生きた台で行が溢れないことは D5 が見る。

## 4. 試験先を master で（11-22、容器、木は 9cc5d09d）

- D1 赤: drpc.o 1,412 / 4,484 / 16,772 B。伸びるのは `dnode_table`（12 B/台）と `caller_strs`（4 B/台、呼ばれた仕事の exinf に呼び手の番号の文字列）＝2b で一緒に消す
- D2 赤: 61 行・23 ファイル（行の後ろの注釈は数えない）
- D5（C10）赤: 行の数 8〜11、11/12 台が 8 超。C1〜C9 は PASS。8 行の台が 1 つ＝heartbeat は全員からとは限らない
- 2a の探り（使い捨ての木で読む 51 か所を口に、同時に 2 腕）: `static inline` だと x86・aarch64・hosted の .text が動く、**マクロなら 3 つとも全バイト同じ**（pk-scratch/n3d2/probe*）
- 陰性 D3 は土台が赤のあいだは意味が無い。2a・2b の回で緑の土台に対して見る

## 4b. 監査-211（限界つき PASS、11-22）

D1 1,412/4,484/16,772・D2 61 行 23 ファイルを再現、N3D_NC=2 で 62/24。**D2 の grep は 3 つの形を見ない**（監査者の壊し、どれも 61 のまま）: 行頭が `*` のコード（`*p = dnode_table[1]…`）・別名（`DNODE *t = dnode_table; t[1]`）・`dnode_table [1]`。C10 は上限だけ＝行 0 の台も緑。**2a の条件**: 口の外の使用はコンパイラで捕まえる（表を drpc.c の static にするか、外のファイルで `#pragma GCC poison`）＝D2 は補助に。2b の C10 に下限（行 ≥ P−2）。台本の外で表を定義するもの（userland/x86/ushim.c・tests/llm/placement_stub.c）と sizeof を読むもの（tests/unbounded/*.c）も 2a で口へ。

## 4c. 2a を作った（11-22、feat/n3-dnode-door、監査待ち）

drpc.h に口（`dnode_state/ip/missed/node_id` は読みだけ＝キャスト、書きは `dnode_set_*`、`DNODE_FOREACH`・`DNODE_TABLE_BYTES`・`DNODE_TABLE_DEFINE`）と、持ち主（`DRPC_TABLE_OWNER` を最初に定義する drpc.c・swim.c）以外での `#pragma GCC poison dnode_table`。25 ファイル 60 行（読み 51・書き 9）、ushim.c・placement_stub.c の定義、tests/unbounded の sizeof を口へ。.c の行番号は変えていない。試験先 D2c（5 つの形が "poisoned" で断られ、口は通る）は master で赤。枝: D2 0 行・D2c PASS、NC=2 で D2 赤、毒を外すと D2c 赤。D2 の持ち主に drpc.h を足した（口が表を名指しする。master は 60 行 22 ファイル）。DNODE_FOREACH はまだ誰も使っていない（2b で行の上を回す口）。CI の unbounded-N に D2/D2c（D1 は 2b まで赤）。

## 4d. 監査-212（限界つき PASS、11-22）

crown・hosted の .text/.rodata/.data は 4 つとも全バイト同じ、D2 0・D2c PASS・NC=2 で D2 赤・master で D2c 赤を再現。毒は gcc・aarch64-gcc・mingw・clang 18 で効き、後から定義したマクロ・`##` の連結も断る。読みの口は右辺値（`&dnode_state(1)` は断られる）。**すり抜けるもの**: drpc.h より前に定義したマクロ（gcc の仕様）・asm のラベル・自前の `DRPC_TABLE_OWNER`（arch/ の下の `dnode_table[` なら D2 が拾う）。今の木には無い。CI の段の `! grep … FAIL` は効かない形だった＝監査側で `if grep …; then exit 1; fi` に。

## 4e. 2b を作った（11-24、feat/n3-dnode-rows、監査待ち）

hosted は表を持たない。行は `SWIM_PEER.row`（sw_map 1 本）。持ち主の 138 行を `DROW(n)`（読み、枠が無ければ毎回 0 にした UNKNOWN の行）・`DROWW(n)`（代入、SWIM の言葉と同じく枠を取る＝渡し・押し出し・断って数える）に機械的に置き換え（pk-scratch/n3b/rowify.py）、ベアでは両方 `dnode_table[(n)]`＝crown 全バイト同じ。drpc_init はある行だけ UNKNOWN に戻す。caller_strs は 8 ビットの番号の 256 で（DNODE_MAX でなく）、3 桁目も出す。自分の行も枠を 1 つ使う（peer は P−1）。決めたこと: **送る heartbeat は全 id のまま**（行の上だけに送ると誰も見つからない＝§2.2 の「送るのも行の上だけ」はやめた）。行を回すのも id の上のまま（DROW 1 回が枠を線形に探す＝CPU は O(N·R)、メモリは O(R)）＝限界。

## 5. 選ばなかった案

drpc に別の NODEMAP（食い違う）。一度に 2b まで（監査で変わった所を切り分けにくい）。
