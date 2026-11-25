# N3 の次の表を選ぶ — 台数で伸びる静的な表の一覧（2026-11-26）

2026-11-26 01:50 の回（監査-236 の空き時間、調べだけ・製品のコードは変えていない）。ROADMAP の N3 は「SWIM の配列 → dnode_table → world/moe/pmesh」の順。第 3 段（gossip_learn）が master に入ったので、次の表を実測で選ぶ。

## 1. 測り方

`tests/unbounded/fleet_tables.sh`（報告だけ、門ではない）: arch/common と arch/linux の .c 全部を hosted x86_64 の旗で DNODE_MAX 64 と 1024 で作り、data+bss が変わる物と、動いた記号を大きい順に出す。79 個が作れた（ark_profile・galaxy・lookup・placement・ui_api はこの旗では作れない＝見ていない）。

## 2. 結果（master af4704f0、容器、64 → 1024）

| 物 | 増え（B） | 主な記号 |
|---|---|---|
| r3_incontext | 104,636 | mq_ask_state・keep（静的局所）・mw_ann_man（各 3,072→33,792）、mw_peer_epoch・mt_last_seq |
| moe | 24,568 | peer_scores・util_ewma・h_score_sub・score_valid・recent_pick |
| world | 23,036 | table（1,280→20,480）・h_sub |
| pfs_repl | 11,520 | last_ann_seq・last_sync_seq・last_want_seq |
| netstack | 11,520 | arp_table |
| pmesh | 9,600 | pmesh_routes・beacon_pkt |
| reflex・protect・edf・pfs_dag・heal・cradle_net・region・spawn | 各 7,680 以下 | |

全体は 21,634,421 → 21,796,930（+162,509）。**gossip_learn・swim・drpc は一覧に無い**（第 1〜3 段の結果と合う）。
dkva・kdds・degrade は「減る」: `for (UB n = 0; n < DNODE_MAX; n++)` が 256 以上で終わらず、その後ろが最適化で消えた跡（n3-swim-region.md の前提の 88 か所）。表が小さくなったのではない。

## 3. おすすめ

- 増えの 64% は r3_incontext（N2 で行を pk_node_row にした表＝DNODE_ROWS で伸びる）。**次の段は r3_incontext の 3 本（mq_ask_state・keep・mw_ann_man）を先に、その後 moe・world・pmesh**。ROADMAP の順（world/moe/pmesh）は、r3 を測る前に決めた順と読む
- 1024 台での増えは 1 台あたり 160 KB で、65,534 番号まで広げると約 10 MB。急ぐのはメモリより「全番号を回る CPU」（N3 2b の限界）と UB ループ（256 台で止まらない）の方かもしれない＝次の設計の回で、表の大きさと一緒に「DNODE_MAX を回るループの数」も数える
- 限界: 静的データだけ。malloc・スタックの [DNODE_MAX]（gl_fold_cached_peers の ptrs のような）は見えない
