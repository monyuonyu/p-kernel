# B5 第 2 段の M の周（設計メモ、未実装）

2026-11-19 03:40 の回。b5-wire2.md §7 の限界 ①〜③ の切り分け、直しの案、合否。

## 1. 予算

合体は周の 1/4 から次の gen まで（`student_region_task`）＝1800 秒なら 1,350 秒。M を 4 台から＝22,312 塊 → **1 台 16.5 塊/秒以上**。publish は周の 1/10 以内。

## 2. 読み

- ① `GL_ST_FETCH_MAXPASS` 4096 は仲間 1 台ごと。1 pass＝60 ms＋走査
- 走査は毎 pass **足りない塊を全部** `pfs_get`＝flat の open（最初は 5,578 回）
- 受けた塊 1 つに `pfs_dur_write` 2 回（塊と .org）＝fsync 4 回
- 持ち主は want 1 つに塊を 2 回読み SHA 2 回（`pfs_has`・`send_block_to`）
- ③ hold-ack は受けた塊 1 つに 1 通

## 3. 測り（tests/llm/b5_m_round_probe.sh、master 7e8a4208、11-19 04:25〜）

腕 fs（そのまま）と nf（pfs_durable.c の fsync を外した写し、測りだけ）を同時に、容器の 4 コアに 10 台。
- **② は fsync**: fs は publish も取り寄せも 1 台 **14〜17 塊/秒**、CPU は user 5〜8%・sys 10〜14%。nf は **190〜280 塊/秒**（n2 は 22,276 塊を約 2 分）。小さな測り: durable な 4 KB の put 7.6 ms、fsync なし 0.09 ms、open 5,578 回 40 ms
- 1,500 秒で: **fs n2 は 3 台分 16,711 塊に 1,220 秒**（4 台なら約 1,630 秒＞1,350）、n4・n5 は 2 台分に 765 秒、n1・n3 は終わらず。**nf n2 は 4 台分と合体で 102 秒**。gave up 0、疑いは fs n2 に 1
- fs の publish 178 秒（1/10＝180 秒の際）
- ①: 15 塊/秒なら 1 台分 3,700 pass（約 100 ms）＝上限の際
- ③: hold-ack は 1 周 22,276 通でも relay の CPU は最大 11%
- 語で回すと members が 1〜5 に揺れる（記述子がそろう前に合体、疑いは 0）

## 4. 直しの案（順番は §3 から）

- **D4（先）**: 生徒の塊（publish・取り寄せ）は塊ごとに fsync しない、最後に 1 回。壊れた塊は SHA で「無い」に見え、.org が消えた塊は `leave` が left-unknown/ へ退ける
- D1: 上限を pass の数から締め切りの時刻に（STALL は残す）
- D2: 走査は窓の分だけ／D3: 持ち主は 1 回だけ読む／D5: 静かな塊に hold-ack を送らない（D4 の後に測って効くなら）

## 5. 合否（作る前に固定）

unit（run_b5_wire.sh）
- W13（D4）: publish と取り寄せ 1 回の fsync は 2 回以下、読み戻しは全バイト同じ。陰性 `B5W_NC_FSYNC_EACH` で赤
- W12（D1）: 4,096 pass を超えても締め切りの内なら成る。陰性 `B5W_NC_PASS_CAP` で赤
- W11（D2）: 1 pass の探しは W＋1 回以下。陰性 `B5W_NC_FULL_SCAN` で赤。W0〜W10 は緑のまま

live M（relay＋5 台、手元）
- M1: `PKERNEL_B5_ROUND_S=600` で 2 周、各周 members 5・fetched 4・gave up 0、合体の行が 1/4 から 450 秒以内（1,350 秒より厳しい）（測りの後に語の形から直した、前の版 191d878c）
- M2: 疑い 0・`want table full` 0。比べる相手（直す前の master）を同時に
- 1800 秒の周は M1・M2 の後に 1 回。既定で入れるのはその後
