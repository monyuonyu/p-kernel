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

## 4b. 実装（2026-11-24 23:35 の回、feat/n3-gl、監査待ちになるまで途中）

- 3a: `GL_SIM_MAX` 64（素の 64＝DNODE_MAX と同じ書き方、ベアの配列は同じ）。`GL_G23_N`・`GL_ST_NODES` ≤ `GL_SIM_MAX` を `_Static_assert`
- 3b（hosted だけ、ベアは `#ifndef _TK_HOSTED_LIBC_` で前のまま）: `gl_pcache[GL_PC_STORE]`＋`gl_powner`・`gl_phave`、取る口 `gl_pc_take`（自分の枠→空き→行のある台が行の無い持ち主を押し出す→断って数える）。合体は**持ち主の番号順**（浮動小数の足し順が前と同じ＝同じ仲間なら同じビット）。`ptrs` はスタックの `GL_PC_STORE+1`（監査-229 の注①）
- 取りに行く番号: `swim_row_ids`（swim.c に足した、行が UNKNOWN でない narrow の番号、O(SW_STORE)）∪ `swim_wide_alive` ∪ 枠の持ち主。`-DN3D_NC_FLEET` の木では DNODE_FOREACH の行で代わり
- `gl_model_ref` は hosted で全桁（100 以上の番号は下 2 桁で他の台と同じ名前になっていた）
- `[g23-ceiling]` B は枠を空にして `gl_pc_take`/`gl_pc_put` で入れ、終わったら試験が書いた枠は空に（中身は失われた＝次の周に取り直す）、他は戻す
- 陰性: G1 は `N3G_NC=1`、G4 は `-DGL_NC_NO_CAP`（枠が GL_SIM_MAX、断らない）。G4 の台本 `tests/host/run_gl_cap.sh`
- 注②（Path W の先着の行）はこの段では触らない。Path W を N3 で直すときに「SWIM の枠を持つ台が勝つ」に揃える

## 5. 選ばなかった案

- 広い番号まで全部取りに行く: 65,534 番号×毎周の取り寄せ＝遅い
- 表は DNODE_MAX のまま中身を malloc: 全員が居れば同じ大きさ＝伸びは消えない
