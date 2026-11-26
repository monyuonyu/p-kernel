# N3 第 4 段: pk_node_row を「全番号が R 行を取る」形へ（設計、2026-11-26）

n3-fleet-tables.md §3 の続き。この文書と試験先だけで、製品のコードはまだ同じ。

## 1. いまの形（master 69403bdf、容器）

hosted の `DNODE_ROWS`＝DNODE_MAX＋32。行 0..63 は番号そのもの、広い番号だけが 32 行の 1 つを取り一生持つ。これで引く表が DNODE_MAX で伸びる: 64→1024 で r3_incontext +104,636 B（mw_ann_man・keep・mq_ask_state ほか 10 本）、protect +7,680（objs の holders）、pfs_repl +11,520（last_*_seq）、region +2,814（member・super_capable・teacher_capable）。drpc は伸びない。r3 には番号で引く表 3 本（mq_ver_drop_seen・mt_ver_drop_seen・mhi_bad）も残る。引く所は 103（r3 51・region 23・pfs_repl 22・protect 7）。8 ビットの値を艦隊と比べる所が 20（drpc 4・r3 5・region 10・protect 1、hosted でコンパイルされる行を -Wtype-limits で）＝256 台以上で終わらないループといつも真の検査（region は 256 で member_cnt・coord が消える）。

## 2. 作る形（hosted だけ）

- 行の数 `PK_NODE_ROWS`（既定 96、DNODE_MAX に依らない、255 以下）。`DNODE_ROWS`＝PK_NODE_ROWS
- **狭い番号も行を取る**（番号＝行をやめる）。引きは nodemap.h のハッシュ
- 行ごとに最後に触った時刻（pk_node_row・_find が更新）
- 表の持ち主は「行 r を空ける関数」を登録する（`pk_node_row_on_reuse`）。行を渡す前に全員を呼ぶ
- 番号で引く 3 本は行へ、8 ビットの比べ 20 か所は INT か行の上のループへ
- ベアは今のマクロのまま（番号＝行）＝crown 全バイト同じを狙う。動いたら判断待ち

## 3. 分かれ道（決めた）: 満杯なら「十分古い行だけ渡し、無ければ断って数える」

渡せるのは、最後に触ってから `PK_ROW_IDLE_MS`（既定 300 s）以上、かつ SWIM の枠を持たない番号の行。その中で一番古いもの。

- 断るだけ（今）だと、入れ替わる群れで長く生きる台は、一生で R 個の番号を見たあと新しい仲間を全部断る＝台数でなく時間で壊れる
- いつでも一番古い行を渡すと、①タスクが握る行番号が別の番号に渡る（監査-227 の限界の形）②重複の見張り（last_*_seq など）が消え古い再送を 1 回受ける。古さの線で両方を止まった番号に限る
- 採らない案: 表ごとの世代番号（103 か所に検査）

## 4. 合否（作る前に固定、2026-11-26）

- **R1** `tests/unbounded/row_coupling.sh`: 5 物の data+bss が 64/256/1024 で同じ、かつ大きさの変わる記号 0。master 赤（4/5）
- **R2** 同じ台本: 8 ビットの比べの警告 0。master 赤（20）
- **R3** 陰性 `N3R_NC=1`（region.o に [DNODE_MAX]）で R1 赤、`N3R_NC=2`（UB のループの .c）で R2 赤。master では n3r_nc_fleet が出る・R2 が 21。緑の土台での赤は実装の回で
- **R4** `nodes rows` の自己試験: 狭い番号も行を取る／R 個で満杯・全部新しければ断って数える／古くした行（試験の口で時刻を進める）は渡され、空ける関数が全員呼ばれる／SWIM の枠を持つ番号の行は渡さない。陰性 `-DN3R_NC_NO_CLEAR`・`-DN3R_NC_NO_IDLE` がそれぞれ赤
- **R5** 既定 96 行: crown 2 つ全バイト同じ、自己試験の PASS 行が master と同じ、run_n3_swim_cap・run_n2c_wide・build_all 緑
- **R6** 生きた腕: 12 台を `-DPK_NODE_ROWS=8` で。どの台も落ちず、断りの数を 1 行で出す

## 5. 順番

4a（小さい）: 8 ビットの比べ 20 か所と番号の表 3 本＝R2 緑、行の取り方は同じ、crown 同じ → 監査。4b: 行の輪（R1・R4〜R6）→ 監査。限界: 静的データだけ、103 は宣言を含む grep の数。

## 6. 監査-238 の注（2026-11-26、設計 PASS）

- R1 はスタックを見ない。region.c:352 `others[DNODE_ROWS]`・465 `mbr/cap[DNODE_MAX]` も艦隊で伸びる（今の 1024 では UB のループで消され -fstack-usage が測れない）＝4a の後に測る
- `PK_NODE_ROWS >= SWIM_PEERS` を _Static_assert に（SWIM_PEERS は -D で変わる。下回ると枠を持つ番号だけで満ち、渡せる行が無い）
- 行番号を握ったまま待つタスクは、待ちの後に _find で取り直す（300 s を越える待ちで行が渡る）。R4 に 1 項目
- R2 は UB<300 も拾う、UH は拾わない（今は害なし）

## 7. 4a（11-26 19:05 の回、枝 feat/n3-row-4a、監査待ち）

- 20 か所のループと protect の holder_count を pk_node_t に（hosted 16 ビット、ベアは unsigned char＝同じ型）。r3 の 3 本は DNODE_ROWS の大きさで `r3_once`（pk_node_row_find、行の無い送り手は毎回出す）。行の取り方は同じ
- R2 緑（master 20）、R1 は赤のまま（4b の仕事）
- §6 ① スタック（-fstack-usage、hosted -O1）: 64/256/1024 で region_supernode_test 320/1,088/4,160 B・region_size_narrow 240/624/2,160 B（master は 256 以上でループが潰れ 16 B と出る）＝4b で行へ
- 限界: supernode_select・teacher_select は UB で返し 0xFF が「無し」＝番号 255 以上の当選は切れる（今は 64 台、4b で）
