# N2: 8 ビットの番号を前提にしている所（洗い出しの表）

設計は `n2-wire16.md`。下は `python3 tools/n2scan/inventory.py` の出力そのまま（master 64b7c909、2026-11-01。packed を前に書く形と pk_node8_t も数えるようにした）。手で直さない。作り直すときはこの見出しの下を消して出力を貼る。

限界: 正規表現で名前を見るだけ。名前に node・src・origin などを含まない番号の欄、バッファに 1 バイトずつ書く所、tests・samples・llm・android は見ていない。中継（relay/relay.c）の頭は構造体でないので表 1 に出ない（src・dst は 6・7 バイト目、8〜11 は予約）。**表 1 は `typedef struct __attribute__((packed)) {` の形（packed が { の前）を読まない＝KLOAD_PKT.src_node（kloader_task.h）と KL_BEACON_PKT.node_id（kloader_task.c）が漏れている（監査-103。check_types.py の W はこの形も読む）。ベア x86 の写し boot/x86/kl_net.h は u8 で書くので見えない。** **UI の写し（ui_api.h の UI_SNAPSHOT.me_id・region、UI_PEER.id・region、UI_EVENT.src・dst、どれも U1）も packed でも *_PKT でもないので表 1 に出ない（2026-11-06 監査-137 の読み）。広い自分は ui_api.c 167・174・76 で番号を切る（region 259→3）。Android の画面との約束ごと（ABI）なので、広げるのは画面の側と一緒に。**

設計の前提として読んで確かめたこと（2026-10-29、master e8b6c5f4）:

- **電文の番号は中の番号**: hosted は PKERNEL_NODE_ID（1..255）を偽の MAC の末尾に入れ（rtl8139.c 65）、usermain.c 395 で 1..DNODE_MAX だけを受けて中の番号＝外−1。0xFF は「ノードでない」。設計の「255 以上が広い」は中の番号（外の 256 以上）。試験の 260・300 は中で 259・299、下位 8 ビットは 3・43＝外の 4・44 の中の番号と同じ（化ける組のまま）
- **SWIM の capability の bit 2 は空き**: 使うのは bit 0・1 だけ（swim.c 385・386）。ただし**旧版が他人のうわさを流し直すときは bit 0・1 から作り直す**（cap_byte、swim.c 256・453・476・486）＝bit 2 は旧版を通ると消える。本人の ALIVE（cap_self_byte）と新しいノードの流し直しでしか伝わらない。外れたときは「v2 を送らない」側に倒れる（安全側）。N2b の試験はこの経路（旧版だけを通るうわさ）でも X1〜X4 を見る
- **旧い中継は版 3 を捨てる**: parse_packet（relay.c 273）は版 1・2 以外で -1。頭の 8〜11 は読まない。ノードの側も 0 を書く（net_relay.c 363）。ノードは PKERNEL_NODE_ID が 1..255 の外なら起動で言う（net_relay.c 762）

## 1. Wire/disk structs (packed, or with a magic, or named *_PKT etc.) with an 8-bit node field

| file:line | struct | 8-bit node fields |
|---|---|---|
| arch/common/include/ark_profile.h:93 | ARK_PROV | origin_node, src |
| arch/common/include/dkva.h:106 | DKVA_Q_PKT | src_node |
| arch/common/include/dkva.h:116 | DKVA_RESP_PKT | src_node, origin |
| arch/common/include/dproc.h:49 | DPROC_ENTRY | node_id |
| arch/common/include/drpc.h:141 | DRPC_PKT | src_node, dst_node |
| arch/common/include/dtr.h:64 | DTR_ACT | src_node |
| arch/common/include/dtr.h:74 | DTR_RESULT | src_node |
| arch/common/include/dtr.h:88 | DTR_INPUT | src_node |
| arch/common/include/dtr.h:97 | DTR_HEAD_ACT | src_node |
| arch/common/include/dtr.h:413 | MT_TEACH_PKT | origin_node, src |
| arch/common/include/dtr.h:448 | MQ_WANT_PKT | origin_node |
| arch/common/include/dtr.h:478 | MH_ENTRY | origin |
| arch/common/include/dtr.h:483 | MH_HAVE_PKT | origin_node |
| arch/common/include/dtr.h:504 | MHI_HIST_PKT | origin_node |
| arch/common/include/dtr.h:556 | CRADLE_TEACH_PKT | teacher_node |
| arch/common/include/kdds.h:187 | KDDS_PKT | src_node |
| arch/common/include/kloader_task.h:31 | KLOAD_PKT | src_node |
| arch/common/include/pfs_dag.h:84 | PFSD_MANIFEST | origin |
| arch/common/include/pfs_dag.h:103 | PFSD_REF_ENT | origin |
| arch/common/include/pfs_dag.h:115 | PFSD_REF_PKT | src_node |
| arch/common/include/pfs_repl.h:92 | PFSR_ANN_PKT | src_node, origin |
| arch/common/include/pfs_repl.h:103 | PFSR_WANT_PKT | src_node |
| arch/common/include/pfs_repl.h:113 | PFSR_SYNC_PKT | src_node |
| arch/common/include/pfs_repl.h:124 | PFSR_BLK_PKT | src_node, origin |
| arch/common/include/pfs_repl.h:141 | PFSR_HOLD_PKT | src_node, origin |
| arch/common/include/pmesh.h:53 | PMESH_ROUTE_ENTRY | dst_node |
| arch/common/include/pmesh.h:63 | PMESH_BEACON_PKT | src_node |
| arch/common/include/pmesh.h:73 | PMESH_DATA_PKT | src_node, dst_node |
| arch/common/include/raft.h:57 | RAFT_PKT | src_node |
| arch/common/include/reflex.h:129 | REFLEX_ALARM | src_node |
| arch/common/include/replica.h:89 | REPLICA_PKT | src_node |
| arch/common/include/sfs.h:51 | SFS_PKT | src_node |
| arch/common/include/spawn.h:47 | SPAWN_PKT | src_node, dst_node, raft_leader |
| arch/common/include/swim.h:60 | SWIM_GOSSIP_EVT | node_id |
| arch/common/include/swim.h:86 | SWIM_PKT | src_node, probe_target |
| arch/common/include/vital.h:29 | VITAL_DATA | node_id, peers_alive |
| arch/common/include/world.h:52 | WORLD_BEACON | node_id |
| arch/common/kloader_task.c:300 | KL_BEACON_PKT | node_id |
| arch/common/r3_incontext.c:5482 | MW_MANIFEST | origin |
| arch/common/r3_incontext.c:5499 | MW_ANNOUNCE | origin |
| arch/common/ss6_live.c:55 | SS6L_REQ | src_node, dst_node |
| arch/common/ss6_live.c:73 | SS6L_REP | src_node |
| arch/common/supernode.c:51 | SNF_PKT | origin, final_dst |

43 structs in 22 files.

## 2. Receivers that drop a packet whose version byte is not theirs

| file:line | constant |
|---|---|
| arch/common/arkfs.c:581 | ARK_FMT_VERSION |
| arch/common/conscience.c:189 | LAW_VERSION |
| arch/common/conscience.c:235 | LAW_VERSION |
| arch/common/drpc.c:404 | DRPC_VERSION |
| arch/common/dtr_train.c:324 | DTR_WBLOB_VER |
| arch/common/genome.c:402 | GENOME_VER |
| arch/common/gossip_learn.c:430 | GL_ST_DESC_VER |
| arch/common/kdds.c:445 | KDDS_VERSION |
| arch/common/kloader_task.c:127 | KLOAD_VERSION |
| arch/common/kloader_task.c:317 | KLOAD_VERSION |
| arch/common/lm_self.c:214 | LM_SELF_VER |
| arch/common/lm_self.c:1135 | LM_SELF_VER |
| arch/common/pfs_dag.c:494 | PFSD_VERSION |
| arch/common/pfs_dag.c:593 | PFSD_VERSION |
| arch/common/pfs_dag.c:629 | PFSD_VERSION |
| arch/common/pfs_repl.c:412 | PFSR_VERSION |
| arch/common/r3_incontext.c:4573 | MT_WIRE_VER_VOCAB |
| arch/common/r3_incontext.c:4726 | MT_WIRE_VER_VOCAB |
| arch/common/r3_incontext.c:4949 | MT_WIRE_VER_VOCAB |
| arch/common/r3_incontext.c:5246 | MT_WIRE_VER_VOCAB |
| arch/common/raft.c:177 | RAFT_VERSION |
| arch/common/replica.c:282 | REPLICA_VERSION |
| arch/common/sfs.c:293 | SFS_VERSION |
| arch/common/sign.c:172 | SIGN_MANIFEST_VER |
| arch/common/spawn.c:130 | SPAWN_VERSION |
| arch/common/swim.c:580 | SWIM_VERSION |
| arch/linux/aarch64/net_relay.c:1049 | RELAY_VER_V2 |
| arch/linux/x86_64/net_relay.c:1049 | RELAY_VER_V2 |
| arch/x86/persist.c:201 | PERSIST_VERSION |
| boot/x86/kl_net.c:576 | KLOAD_VERSION |

30 places.

## 3. Line counts by shape and area (comments skipped)

| shape | arch/aarch64 | arch/common | arch/linux | arch/x86 | boot/linux | boot/x86 | kernel | lib | relay | userland | total |
|---|---|---|---|---|---|---|---|---|---|---|---|
| u8 node var/field (all) | 3 | 351 | 6 | 9 | 0 | 0 | 1 | 2 | 0 | 7 | 379 |
| u8 node parameter | 1 | 167 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 6 | 174 |
| 0xFF as "no node" | 0 | 90 | 0 | 10 | 0 | 0 | 0 | 0 | 0 | 0 | 100 |
| array indexed by a node id | 2 | 254 | 86 | 5 | 2 | 0 | 0 | 0 | 24 | 1 | 374 |
| DNODE_MAX | 1 | 346 | 19 | 4 | 0 | 0 | 0 | 0 | 0 | 2 | 372 |
| MAC/IP last octet = node | 3 | 1 | 18 | 3 | 0 | 3 | 0 | 0 | 0 | 0 | 28 |
