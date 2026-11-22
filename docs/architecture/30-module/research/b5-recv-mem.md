# B5 q8 の 2 歩目: 受け手のメモリ（設計メモ、2026-11-23 00:05 の回）

監査-215 の限界（b5-q8-copy.md §4）: 写しは L で 20.9 MB なのに、受け手 `st_region_round` は仲間ごとに st_rg_cap（w・mu・vu、L で 247 MB）を取り、さらに自分の st_save の写し（own）と候補（cand）で 2 つ。7 人の L で約 2.2 GB。

## 1. 読んで分かったこと
- 合体（st_merge_trimmed）が仲間の写しから読むのは頭（ST_BLOB_HDR）と w だけ（st_blob_w_at・st_blob_w_finite）。長さの検査（st_blob_tier_ok）が w・mu・vu 全部の長さを求めるので、全部を持っていた
- own は cand に st_load するためだけ。cand の mu・vu・adam_t は合体が 0 にする＝要るのは live の w だけ
- publish は 8192 塊 × 4096 B＝32 MiB を超える写しを断る＝それより大きい写しは届かない

## 2. 決めたこと（他の案は 4）
1. 仲間ごとの器は頭＋w（`st_rg_w_size`）。取り寄せの器は 1 つを使い回す（大きさは st_rg_cap と ST_RG_FETCH_MAX（32 MiB＋ST_RG_HDR）の小さい方）。BRG1 は届いた写しを st_blob_tier_ok で全部確かめてから頭＋w を写す。BRG2 は頭＋w だけに開く
2. 合体は頭＋w の写しを受ける形（st_merge_trimmed と同じ中身、長さの要求だけ違う）
3. cand は live の w を直接写す（own をやめる）。live の w・mu・vu の有限の検査は今の st_load と同じに残す
4. 「器が 1 つでも取れなければ周ごと断る」は同じ（W10）

## 3. 合否（実装の前に固定）
- **Q5**（unit、試験の口 `st_rg_test_bytes`＝周が同時に持った器の合計）: M・仲間 3（BRG2）で ≤ min(st_rg_cap, FETCH_MAX)＋3×(頭＋w)＋q8 の器＋1 blob（cand）、かつ L・仲間 7（取り寄せは全部失敗）で ≤ 700 MB（いまは約 1.75 GB）
- **Q6**（同じバイト）: unit の W0〜W10・Q0 がそのまま緑、判定の種 261・263 の q8 の段のログが master と全バイト同じ
- 陰性 `-DB5R_NC_FULL_PEER`（仲間の器を blob 全部に戻す）で Q5 だけ赤

## 4. 他の案と限界
- 合体を行ごとに q8 から直接（器は w 1 つ分も要らない）: 速さと st_merge_trimmed の検査の作り直しが要る＝次の歩
- 限界: cand（L で 247 MB）と live は残る。L の合体を既定にする話は別（G6）

## 5. 結果（容器、2026-11-23、試験先 8e6f6f59 → 直し）
- Q5: 試験先 M 116,189,512 B（線 70,499,612）・L 1,750,802,912 B＝赤。直し M 45,690,036 B（0.39 倍）・L 610,184,488 B＝緑。陰性 nfull は Q5 だけ赤
- 器の最大は「取り寄せの器＋仲間の頭＋w」か「仲間の頭＋w＋cand」の大きい方（取り寄せの器は合体の前に返す。q8 の scratch は要らなくなった＝取り寄せの器から直接開く）
- crown（x86・aarch64 の .text・.rodata）は master と全バイト同じ（student.c はベアに入らない）
