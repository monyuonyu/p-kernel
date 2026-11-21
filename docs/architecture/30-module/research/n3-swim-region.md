# N3 第 1 段: SWIM の表を領域の大きさへ（設計、2026-11-21）

ROADMAP N3 の最初の表。製品のコードはまだ同じ。

## 1. いまの形（master e4ca62c8）

swim.c を hosted x86_64 で DNODE_MAX 64/256/1024 にしてコンパイルし `nm -S` で見た:

- **台数で伸びる表 7 本**（番号で引く）: `dnode_incarn`・`suspect_count`・`v2cap_tab`・`rtt_valid`（各 1 B）、`swim_nping_seq`（2 B）、`rtt_ewma_ms`・`swim_nping_ms`（各 4 B）＝1 台 14 B、896 → 3,584 → 14,336 B。引くのは swim.c だけ（外へは `swim_rtt_ms`・`swim_peer_reads_v2`、10 ファイルが呼ぶ）
- **既に領域の大きさ**: 広い番号の `wide_st[DREGION_MAX]`（32 B×64、N2C3 の指紋 `fp` も）と `wide_map`。満杯なら DEAD を追い出し、無ければ断って `nm_full` を数える
- **DNODE_MAX が 256 以上で終わらないループ**: `for (UB i = 0; i < DNODE_MAX; …)` が swim.c に 4 か所。共通コード全体で 88 か所（dkva 26・region 10 ほか）＝後の段の前提
- `dnode_table`（drpc.c）は第 2 段。drpc の heartbeat も全員ぶん書く（drpc.c 540）＝第 1 段の後も `nodes` の表は全員、枠の無い仲間は SWIM の故障検出の外。ベアの番号は 1〜8

## 2. 作る形

1. 7 本を構造体 `SWIM_PEER` の表 `sw_peer[SWIM_PEERS]` に、狭い番号→枠は `NODEMAP sw_map`。`SWIM_PEERS` は既定 DREGION_MAX（試験では -D で小さく）
2. 枠は `wide_add` と同じ取り方（満杯なら DEAD を追い出し、無ければ断って数える）。`nodes` に `[swim] slots U/C refused F` の 1 行。枠の無い番号のうわさは捨てる。自分への反論は今のまま
3. ループは枠の上を INT で回す（選びは O(R)）。`swim_rtt_ms` は枠が無ければ未測定＝region に入れない
4. ベアは `pk_node_row` と同じ形: 枠＝番号・`SWIM_PEERS`＝DNODE_MAX のマクロにし、crown の全バイト同じを狙う。動いたら判断待ち R-N3S（監査 PASS を条件に re-bless がおすすめ）
5. 広い番号の `wide_st` と N2F の尾はそのまま（狭いのと 1 つの表にするのは狭い番号をやめる段）。1 台の SWIM の枠は R（広い）＋R（狭い）

決めたこと: 満杯のとき近い仲間を優先するのは N4。まず「伸びない・止まらない・64 台で変わらない」。

## 3. 合否（作る前に固定）

- **S1** `tests/unbounded/swim_coupling.sh`: swim.o の data+bss が 64/256/1024 で同じ。master で赤（11,028 / 13,716 / 24,468 B）
- **S2** 同じ台本: DNODE_MAX 256/1024 で -Wtype-limits が 0。master で赤（4）
- **S3** 陰性 `N3S_NC=1`（[DNODE_MAX] の配列を 1 本押し込む）で S1 が赤
- **S4** 出荷の形（64）で何も変わらない: SWIM の自己試験（`nodes test`・`nodes life`・idpin・selfsuspect・[n2-mix]・run_n2c_wide.sh）が緑、build_all ALL OK、crown（上の 4）
- **S5** `tests/host/run_n3_swim_cap.sh`（境界の検査つき）: `-DSWIM_PEERS=8` で狭い番号 12 台。全員 U≤8・F>0・表の ALIVE 6 以上・はみ出せば trap で落ちる・最後まで応える。陰性 `-DN3_NC_NO_CAP` で赤。**S1・S2 だけでは足りない**: 7 本を縮めただけ（番号で引く＝はみ出す）の使い捨ての swim.c が S1・S2 とも緑だった

## 4. 作った形（11-21 13:05 の回、feat/n3-swim-cap、監査待ち）

- `sw_peer[SWIM_PEERS]`（16 B）＋`NODEMAP`。枠は SWIM が聞いた時に取る、無い台は返事だけ。探りは枠の上。ベアは `SW_*` が元の配列。swim.o は 64 で +448 B
- C6（H41 (b)）: 止めた台を枠の持ち主が SWIM で DEAD にするか。「drpc が先」は読みの誤りだった（測ると 11/11 が SWIM、drpc 0 本）
- 限界: ① 満杯で DEAD の枠を渡すと incarnation を忘れる ② 自己試験が枠を取ったまま ③ 枠の無い台は `swim_peer_reads_v2` FALSE ④ 先着順では**外れる 3 台が全台で同じ**（回ごとに 0・4・9、1・8・10）だった＝誰も見張らない
- 直し（feat/n3-swim-spread）: 満杯なら (自分, 相手) のハッシュの点が一番低い持ち主と比べ、高ければ入れ替える。C7「どの番号も 2 台以上の枠に」: 先着順で赤（4・5・6 が 0 台）、直しで緑（4〜11 台）。陰性 -DN3_NC_FIRST_COME。限界: 入れ替えで生きた台の incarnation も忘れる（①と同じ形）
