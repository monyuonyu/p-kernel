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

## 4. 作った形（2026-11-21 13:05 の回、feat/n3-swim-cap、監査待ち）

- `SWIM_PEER sw_peer[SWIM_PEERS]`（1 枠 16 B）＋`NODEMAP sw_map`。枠を取るのは SWIM が聞いたとき（その台のパケット・うわさ）。断られた台は返事だけもらい、生きている印もうわさも捨てる。探りと手助けは枠の上。ベアは `SW_*` のマクロが元の配列に展開する
- 64 では swim.o が master より 448 B 大きい（13,076→13,524。枠の構造体の詰め物と NODEMAP）＝64/256/1024 で同じ
- 作る途中の赤: 網の無い台は swim_init を通らず、枠の上限が 0 のまま全部断っていた（`nodes test` ほか 6 つ赤）→上限を静的な初期値に
- 限界: ① 満杯で DEAD の枠を渡すと、その台の incarnation を忘れる＝後で古い ALIVE のうわさで戻りうる（64 では起きない）② 自己試験は試験の番号の枠を取ったまま残す（`nodes test` の後 11/64）③ H41 (b) は S5 の C6 にした: 最初は「drpc の老化が先に DEAD にする」と読みで書いたが、測る（pk-scratch/n3s/h41.sh）と止めた番号 11 を残り 11 台が全部 SWIM（自分の探りかうわさ）で DEAD にし、drpc の行は 0 本＝読みは誤り。`[swim] slots` の行に枠の番号を出し、止める前に枠を持っていた台だけを見る。陰性 -DN3_NC_NO_PROBE_SLOTS ④ 枠の無い狭い台は `swim_peer_reads_v2` が FALSE（古い台として扱う。64 では起きない）⑤ **枠の顔ぶれが全台でほぼ同じ**（S5 の 11 台: 番号 0 は誰の枠にも無く、4 は 1 台・9 は 1 台だけ）＝その台が死んでも SWIM はほとんど気づかない。C6 は全員が持つ 11 を止めるので見えない。次の段の候補: 「どの番号も 2 台以上が持つ」門と、枠の取り方を台ごとにずらす（例: 自分の番号との距離で選ぶ。N4 の近さの前の一歩）
