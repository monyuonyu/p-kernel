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

## 6. 監査-190（限界つき PASS）

- M1 の直しは厳しくなる向き（450 秒＝1 台 約 50 塊/秒）。語の形で members が揃わないのは m3.txt で確かめた（fs 5・5・3・3・3、nf 3・5・1・2・2）。**191d878c の M3（D5 を入れたら hold-ack 1 台 1 周 64 以下）が黙って消えていた**＝D5 を作るなら M3 も戻す
- nf の sed は pfs_durable.c の fsync 3 か所とも外す（3 つ目は leave の移しで、取り寄せの道ではない）。pfs_ark.c の fsync は残る（M の測りでは ark は使っていない読み）
- **ディスクの取り合い（容器、fsconc.py、2 回）**: durable な 4 KB の put は書き手 1 で 150〜163 回/秒、5 つ同時なら 1 つあたり 84〜93（合計は増える）。M の 5 台は 1 台で走るより 1.6 倍ほど遅く出ている。fsync の上限だけなら取り寄せは約 45 塊/秒のはずで、実測の 15 との差は説明されていない（nf の腕で 15 倍になったので、fsync が主な原因なのは変わらない）
- 「後半の失速は出なかった」はやや強い: 17〜18 分目に n4・n5 の合体と重なって 5〜12 塊/秒に落ちた 2 分間がある

## 7. D4 を作った（2026-11-19 05:10 の回、feat/b5-m-d4。**W13・W13b とも赤**、監査待ち）

生徒の塊（quiet の id）は塊ごとの fsync をせず（tmp→rename、.org も）、publish の終わり（記述子の保存の前）と取り寄せの終わりに syncfs を 1 回（pfs_put_lazy・pfs_dur_sync、Windows は今までどおり）。試験 tests/llm/run_b5_d4.sh（relay＋2 台、語が fsync の数を言う）。数（容器、1 回ずつ）: 試験先 ae498178 は S で P=1898・F=1840（482 塊）、M で P=22358。先端は S で P=11・F=1、M で P=11（1 回目）・19（2 回目）・F=1、陰性 FSYNC_EACH は P=1898・F=1840。M の取り寄せ 5,578 塊は 26〜27 秒（陰性 237 秒、ただし時間をまたいだ比べ）。**作る前に決めた W13（P≤8）は赤**（11＝syncfs 1＋記述子の pfs_dag_save 1 回で 10、塊の数には比例しない）。測った後に決めた W13b（P≤11・F≤1・S と M で同じ P）も赤（M で 19＝数えるのがプロセス全体で、ほかのタスクの durable な書き込みも入る）。次: **W13c（生徒の塊の fsync だけを数えて 0、syncfs は 1 回ずつ）を走らせる前に固定**し、数える口を lazy の対象に絞る。CI には入れていない。M1・M2（600 秒の周、比べる相手と同時）は未

## 8. W13c（2026-11-19 20:15 の回、実装者）

走らせる前に固定（f691ac08）: 数える口を 2 つ足した＝lazy の扉（pfs_dur_write_lazy）から来た書き込みの fsync と、実際に走った syncfs。語の行に「lazy publish Lp merge Lf syncfs publish Sp merge Sf」。合否は node 2 が S と M の両方で C≥50・Lp=Lf=0・Sp=Sf=1・fetched 1、陰性 FSYNC_EACH は Lp≥C かつ Lf≥C で赤（数以外の理由の赤は陰性と数えない）。W13・W13b は報告だけに残した。
結果（容器、1 回、`/build/d4c/w13c-keep.txt`）: **ALL PASS**。S（C 482）・M（C 5,578）とも 0・0・1・1。陰性は Lp 1,888・Lf 1,840（syncfs 0・0）で狙いどおり赤。プロセス全体の数 P は S・M とも 11（前の回の M の 19 は再現せず）。M の取り寄せ 24 秒。CI の shipped-llm-certs に段を足した（約 4 分）。
限界: 1 回ずつ。数えるのは書き込みの道の上だけで、syncfs が本当に塊を disk に載せたか（電源断）は試していない（SHA で読むたびに確かめる、が頼り）。

## 9. M1・M2（同じ回、容器、run_b5_wire_live.sh を tier M・600 秒で、枝 f691ac08 と master fab2f457 を同時に）

**M1 は赤、M2 は緑**（`/build/d4c/m12-br-keep.txt`・`m12-seen-keep.tsv`）。枝: 10 行のうち 8 行が members 5・fetched 4（合体は 134〜284 秒＝450 秒の内、gave up 0）、赤の 2 行は n3 の G1（members 3・99 秒）と n4 の G2（members 2・39 秒）＝取り寄せの遅さではない。**直し（同じ回の後半に読み直した）: 一度「記述子がそろう前に合体」と書いたが誤り。members は合体を始めた時の `region_contains`＝SWIM で ALIVE かつ RTT≤τ（REGION_TAU_MS 50 ms）の数で、記述子とは関係ない**（student_region_merge、region.c region_recompute）。塊の流れで RTT が 50 ms を超えた相手が region から外れた、という読み（ログに RTT が無く、確かめていない）。L2（疑い 0・want table full 0・announce 20）・L3・L4 は PASS。
比べる相手の master: G1 は 5 台中 4 台が fetched 0（members 1、publish が周の 1/4 に間に合わない）、n2 だけ fetched 1 に 217 秒。G2 は n5 が members 3・fetched 2 に 1,062 秒、ほかは打ち切りの時点で行なし。master の判定は L1 赤（10 行とも）・**L2 赤（n2・n3 に SUSPECT 1）**・L4 PASS＝M2 は枝で緑、master で赤。**D4 で M の取り寄せは周に収まる速さになった（4 台分で約 140 秒）。残る赤は members のそろい方**。D1・D2 は今は要らない見込み（上限にも当たっていない）。

## 10. D6 の設計（同じ回、実装はまだ。合否は作る前に固定）

- **D6a（先に測る）**: 合体の行に「外した ALIVE の相手と、その RTT」を足す（例 `excluded n3:120ms`）。M を 1 回（枝だけでよい）。外れた相手の RTT が τ を超えていれば読みが当たり。超えていなければ D6b は作らず原因を探し直す
- **D6b（当たりなら）**: 周の仲間は「その gen の頭から合体の始めまでに 1 度でも region にいた相手」の和（region の任務が 5 秒ごとに `region_contains` を見て印を足す、gen が変われば消す）。いなくなった相手は取り寄せが gave up で外れる（今と同じ）。RTT の線（τ）そのものは変えない（region の意味は N2 のまま）
- 合否: unit W14＝印の和に、途中で外れた相手が残る・gen が変わると消える。陰性 `-DB5W_NC_NO_LATCH`（今の形）で W14 赤。live は M1・M2 をそのまま（枝と master を同時に）。M1 の members の赤が消え、L2 は緑のまま

### 10.1 D6a の結果（同じ回、feat/b5-d6 c97f3ff9、容器、M・600 秒・5 台、枝だけ 1 回、21:21〜）

10 行中 3 行が members 4（G1 で 2 台、G2 で 1 台）。**外れた相手は全部 SWIM で ALIVE、RTT 51〜97 ms（τ 50 ms）**。G2 では members 5 で始めた台も、合体の終わりには 2〜3 台が 51〜97 ms で外れていた。**読み（RTT が τ を少し超えて外れる）は当たり**＝D6b を作る。
限界: この版は外れた相手を合体の**終わり**に取っていた（members は始め）。始めに取る形に直した（次の版）。同じ容器で master の M の腕の L3 待ち（5 台）が走っていた。1 回だけ。
限界: 1 回だけ（周 2 つ）。master の G2 の後半は、私が H34 の試験と crown のビルドを同じ容器で始めた後に走った（21:04〜）。
