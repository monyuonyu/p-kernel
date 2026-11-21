# N3 第 2 段: dnode_table を領域の大きさへ（設計、2026-11-22）

ROADMAP N3 の 2 本目の表。この文書と試験先だけで、製品のコードはまだ同じ。

## 1. いまの形（master 64d51ff2）

- `DNODE dnode_table[DNODE_MAX]`（drpc.c、1 行 ip・state・missed・node_id）。狭い番号で引く。drpc の heartbeat が「全員」に送り、全員の行を持つ（第 1 段の後も、SWIM の枠に入らない台はここにだけ居る）
- 引く所: arch の .c で約 200 か所（state 163・ip 15・missed 14・node_id 5）、29 ファイル。書くのは drpc.c 19・swim.c 56 と、自己試験の中の gossip_learn 3・pmesh 4・spawn 2
- 大きさの数字は §4（試験先を master で走らせた結果）

## 2. 作る形

1. **2a 口をそろえる**: drpc.h に `dnode_state(n)`・`dnode_ip(n)`・`dnode_missed(n)` と書く口、行を回す `DNODE_FOREACH`。drpc.c と swim.c の外は口だけを使う。中身は今の配列のまま＝ベアも hosted も .text の全バイト同じを狙う（static inline）
2. **2b 中身を枠へ（hosted だけ）**: 行を SWIM の枠 `SWIM_PEER` に入れ、表を 1 本・地図を `sw_map` 1 本にする。行の無い番号は UNKNOWN・ip 0。heartbeat を聞いたら枠を取る（取り方は第 1 段と同じ、満杯なら断って数える）。heartbeat を送るのも年を取らせるのも行の上だけ（O(R)）
3. ベアは第 1 段と同じく番号＝枠のマクロ。crown の全バイト同じを狙い、動いたら判断待ち（監査 PASS を条件に re-bless がおすすめ）
4. `tests/unbounded/coupling_check.sh` は「番号が広がった」の証拠に `dnode_table_bytes` の伸びを使っている。2b で平らになるので、同じコミットで証拠を `DNODE_MAX` そのものに替える

決めたこと（他の案は §5）: 枠を SWIM と分けず 1 本にする。2 本だと「SWIM は生きていると言うが行が無い」食い違いが起き、第 1 段の枠の選び方（ハッシュ・墓）を 2 度作ることになる。

## 3. 合否（作る前に固定、2026-11-22）

- **D1** `tests/unbounded/dnode_coupling.sh`: drpc.o の data+bss が DNODE_MAX 64/256/1024 で同じ。master で赤の見込み
- **D2** 同じ台本: drpc.c・swim.c の外の arch のコード行（注釈の行を除く）に `dnode_table[` が 0。master で赤の見込み（2a で緑）
- **D3** 陰性: `N3D_NC=1`（drpc.o に [DNODE_MAX] の配列を 1 本押し込む）で D1 赤、`N3D_NC=2`（`dnode_table[` を直に引く 1 行の .c を足す）で D2 赤
- **D4** 出荷の形（64）で何も変わらない: 2a は crown の全バイト同じ（x86・aarch64）と hosted の .text 同じ。2b は crown 同じ、SWIM の自己試験・run_n3_swim_cap.sh・run_n2c_wide.sh・build_all が緑
- **D5** `run_n3_swim_cap.sh` に C10（`N3C_DNODE=1` のときだけ）: 12 台・枠 8 で、どの台も最後の `[cluster]` 表の SELF 以外の行が 8 以下。master で赤の見込み（11 行）。2b で CI に `N3C_DNODE=1` を入れる。陰性 `-DN3D_NC_FLEET`（drpc が配列を使い続ける）で赤

D1・D2 だけでは足りない（第 1 段の教え）: 生きた台で行が溢れないことは D5 が見る。

## 4. 試験先を master で（下に追記する）

## 5. 選ばなかった案

- drpc に別の NODEMAP: 2 つの枠が別の台を選び、食い違う。年を取らせる仕組みも 2 本のまま
- 一度に 2b まで: 29 ファイルの書き換えと中身の入れ替えが 1 枝に混ざり、監査で「振る舞いが変わった所」を切り分けにくい
