# N3 第 1 段: SWIM の表を領域の大きさへ（設計、2026-11-21）

ROADMAP N3 の最初の 1 表。製品のコードはまだ変えていない。

## 1. いまの形（master e4ca62c8 で測った）

swim.c を hosted x86_64 で DNODE_MAX 64/256/1024 にしてコンパイルし `nm -S` で見た:

- **台数で伸びる表 7 本**（番号で引く）: `dnode_incarn`・`suspect_count`・`v2cap_tab`・`rtt_valid`（各 1 B）、`swim_nping_seq`（2 B）、`rtt_ewma_ms`・`swim_nping_ms`（各 4 B）＝1 台 14 B、896 → 3,584 → 14,336 B。引くのは swim.c だけ（外へは `swim_rtt_ms`・`swim_peer_reads_v2`、10 ファイルが呼ぶ）
- **既に領域の大きさ**: 広い番号の `wide_st[DREGION_MAX]`（32 B×64、N2C3 の指紋 `fp` も）と `wide_map`。満杯なら DEAD を追い出し、無ければ断って `nm_full` を数える
- **DNODE_MAX が 256 以上で終わらないループ**: `for (UB i = 0; i < DNODE_MAX; …)` が swim.c に 4 か所（-Wtype-limits）。共通コード全体で 88 か所（dkva 26・region 10・moe 7 ほか）＝後の段の前提
- `dnode_table`（drpc.c）は SWIM も書くが第 2 段。ベアも swim.c を使い、番号は 1〜8

## 2. 作る形

1. 7 本を 1 つの構造体 `SWIM_PEER`（14 B→16 B）の表 `sw_peer[SWIM_PEERS]` にまとめ、狭い番号→枠は `NODEMAP sw_map` で引く。`SWIM_PEERS` は既定で DREGION_MAX（-D で小さくできる、試験用）
2. 枠の取り方は `wide_add` と同じ: 初めて聞いた番号に枠、満杯なら DEAD を追い出し、それも無ければ断って数える（`nodes` に出す）。枠の無い番号のうわさは捨てる（数える）。自分への反論は枠と関係なく今のまま
3. ループは枠の上を INT で回す（選びは O(R)）。`swim_rtt_ms` は枠が無ければ未測定＝region に入れない
4. ベアは `pk_node_row` と同じ形: 枠＝番号・`SWIM_PEERS`＝DNODE_MAX のマクロにし、crown の全バイト同じを狙う。動いたら判断待ち R-N3S（監査 PASS を条件に re-bless がおすすめ）
5. 広い番号の `wide_st` と N2F の尾はそのまま（狭いのと 1 つの表にするのは狭い番号をやめる段）。1 台の SWIM の枠は R（広い）＋R（狭い）

決めたこと: 満杯のとき近い仲間を優先するのは N4 に回す。まず「伸びない・止まらない・64 台では何も変わらない」を固める。

## 3. 合否（作る前に固定）

- **S1** `tests/unbounded/swim_coupling.sh`: swim.o の data+bss が 64/256/1024 で同じ。master で赤（11,028 / 13,716 / 24,468 B）
- **S2** 同じ台本: DNODE_MAX 256/1024 で -Wtype-limits が 0。master で赤（4）
- **S3** 陰性 `N3S_NC=1`（[DNODE_MAX] の配列を 1 本押し込む）で S1 が赤
- **S4** 出荷の形（64）で何も変わらない: SWIM の自己試験（`nodes test`・`nodes life`・idpin・selfsuspect・[n2-mix]・run_n2c_wide.sh）が緑、build_all ALL OK、crown（上の 4）
- **S5** 生きた試験（作る回が台本を先に、ASan つき）: `-DSWIM_PEERS=8` で狭い番号 12 台。全員の枠が 8 以下、断りの数が 0 より大、12 台とも 120 秒応え ASan の報告 0。陰性（上限を外す）で枠 11＝赤。**S1・S2 だけでは足りない**: 7 本を [DREGION_MAX] に縮めただけ（番号で引いたまま＝はみ出す）の使い捨ての swim.c が S1・S2 とも緑だった
