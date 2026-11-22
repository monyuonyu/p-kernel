# B5 q8 の 3 歩目: 合体の段のメモリ（設計メモ、2026-11-23 04:15 の回）

## 1. 測って分かったこと（b5-recv-mem.md §6）
- L の周で 7 仲間と本当に合体すると、malloc の最大は 928 MB（線 700 MB）。内訳は仲間の頭＋w 7×82.4 MB＝577 MB と cand（st_init_tier の w・g・mu・vu＝330 MB）
- 合体（st_merge_trimmed_need）は cand の mu・vu を 0 に、adam_t を 0 にする。g は使わない＝cand の 4 本のうち要るのは w の 1 本だけ。その w も「live の w を読み、仲間を読み、i ごとに 1 回書く」だけ

## 2. 決めたこと（他の案は 4）
1. cand を作らない。合体の書き先を仲間の器の 1 つ（器のある最初の k の w の領域）にする。i ごとに全部を読んでから書くので、書き先が読み元の仲間でも値は同じ
2. 合体は自分の値を live->w から読む（今は live を写した cand から）。live の有限の検査（w・mu・vu）は今のまま読みだけで
3. 門（st_round_adopt）には live の形を写した「見る用の模型」を渡す: w＝書き先、cache＝NULL（st_forward が cache を free して取り直すので live と共有しない）、alive＝NULL（今の cand と同じ）。評価の後に cache を free
4. 採るときは live の w に書き先を写し、mu・vu・adam_t を 0 に（今の cand の値と同じ）
5. full の腕（陰性 B5R_NC_FULL_PEER・B5W_NC_MEAN）は今のまま cand を作る

## 3. 合否（実装の前に固定）
- **Q7**: L の周・7 仲間が答えて合体する unit の腕で、数え（st_rg_test_bytes）≤ 700,000,000 B。監査者は malloc の横取りでも測る
- **Q8**（同じ振る舞い）: unit W0〜W15・Q0〜Q5 がそのまま緑。判定の種 261・263 の q8 の段のログが master と全バイト同じ。run_b5_adopt の種 91 も
- 陰性 `-DB5M_NC_CAND`（cand を作る今の形に戻す）で Q7 だけ赤
- 試験先のコミットで Q7 が赤（今の数え 906 MB）であることを先に示す

## 4. 他の案と限界
- cand を w だけで作る（g・mu・vu を作らない）: 7×82.4＋82＋0 の配列 82＝741 MB で線を越える。採らない
- 仲間の q8 を行ごとに直接読む合体（器は 21 MB ずつ）: さらに小さいが st_merge_trimmed の検査の作り直しが要る＝次の歩
- 限界: live の alive（育てた模型の死んだ専門家）を門が見ないのは今と同じ（今の cand も NULL）。変えると振る舞いが変わる＝別の項目
