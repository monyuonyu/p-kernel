# N3 第 3 段: gossip_learn の表を領域の大きさへ（設計、2026-11-24）

ROADMAP N3 の 3 本目。この文書と試験先だけで、製品のコードはまだ同じ。

## 1. いまの形（master ba1e269e）

- 下調べ（11-24 03:20 の回、hosted の旗で 64→1024 台）: 伸びる物 18 個・計 +6.2 MB のうち **gossip_learn が +6.0 MB（97%）**。ROADMAP の順（world/moe/pmesh）より先にこれをやる
- `gl_model[DNODE_MAX][635]`（2.5 KB）: 生きた道は `[0]`（自分）だけ。`[k]` は自己試験の中の仮の台（最大 `GL_G23_N` 40）
- `sh_x`・`sh_y`・`sh_n`: 自己試験の仮の台の教材だけ
- `gl_pcache`・`gl_phave[DNODE_MAX]`: 生きた道の仲間の写し。`gl_merge_peers` は毎周 0..DNODE_MAX−1 の全部の番号を取りに行き、取れた写しは「一度取れたらずっと」合体に入る。広い番号（≥ DNODE_MAX）は取りに行かない

## 2. 作る形

1. **3a 練習場を分ける**: `gl_model`・`sh_*` の大きさを `GL_SIM_MAX`（64 固定）に。`GL_G23_N ≤ GL_SIM_MAX` を `_Static_assert`。ベアの DNODE_MAX も 64＝ベアの .text は全バイト同じの見込み（実装の回が測る）
2. **3b 仲間の写しを枠へ（hosted だけ）**: `GL_PC_SLOTS`（64）個の枠＋持ち主の番号。取れたら枠を取る。満杯なら、SWIM の枠を持つ台（`DROW` が UNKNOWN でない）が持たない台を押し出す。どちらも同じなら断って数える（`[gl] peer cache full: refused N`）。合体は枠の上（O(R)）
3. **取りに行く番号**: 「行のある番号 ∪ 枠を持つ番号」＝O(R)。広い番号も行があれば取りに行ける（今の取りこぼしが直る）。「行の無い、まだ ALIVE でない台を中身で見つける」（今の fan-in の直し）は、heartbeat で行ができた後になる＝遅れうる。これは G3 で見る
4. `[g23-ceiling]` の B は写しを直に書いている＝枠の口を通す形に直す

## 3. 合否（作る前に固定、2026-11-24）

- **G1**: gossip_learn.o の data+bss が DNODE_MAX 64/256/1024 で同じ（`tests/unbounded/gl_coupling.sh`）。陰性 `N3G_NC=1`（[DNODE_MAX] の配列を 1 つ足す）で赤
- **G2**: CI の自己試験（gossip の `[g22]`・`[g23-ceiling]` ほか）の PASS 行が master と同じ（並走）
- **G3**: `samples/32_collective_learn/run.sh` が RESULT PASS を 2 回（master と同時に）
- **G4**: `-DGL_PC_SLOTS=2` の 4 台で、どの台も合体に入る仲間 ≤ 2、断りの行が 1 つ以上、落ちない。陰性 `-DGL_NC_NO_CAP`（満杯でも断らない）で赤
- crown（aarch64・x86 の .text）が master と全バイト同じ。動いたら判断待ち（監査 PASS を条件に re-bless がおすすめ）。build_all

## 4. 試験先を master で（11-24、手元）

- G1: 1,866,244 / 3,070,372 / 7,897,252 B＝赤。伸びたのは gl_model・gl_pcache（各 162,560→2,600,960）・sh_x（61,440→983,040）・sh_y・sh_n・gl_phave。`discard.0`・`gl_live_total` は nm の名前の付き方の揺れ（大きさは同じ）
- 陰性 N3G_NC=1 も赤（今は本体も赤なので、効くかは実装の後に見る）

## 5. 選ばなかった案

- 広い番号まで全部取りに行く: 65,534 番号×毎周の取り寄せ＝遅い
- 表は DNODE_MAX のまま中身を malloc: 全員が居れば同じ大きさ＝伸びは消えない
