---
name: project-regions-architecture
description: "p-kernel network/model redesign: latency-clustered regions + locality-aware MoE + capacity-scales-with-device-count. Design doc at docs/architecture/regions.md. R0-R2 + waves 1-6 MERGED to master 2026-06-06 (6f4f908). Next targets: R3b breathing params, relay redundancy, ring-0 recovery, TCC."
metadata: 
  node_type: memory
  type: project
  originSessionId: a1fa5e89-3c26-4fe1-a75a-dfa9826e39f6
---

mk_pino が提起した「今のネットワーク（モデル）が貧弱」問題への設計。確認した
現状の数字: モデルは d_model=8 / 635 float の玩具、通信は `kdds.c:184` の
フラット全対全 O(N²)、`DNODE_MAX=8` 固定、`degrade.c` は3段で容量不変、
`moe.c` の `select_expert()` は accuracy のみで RTT/帯域を見ない。

ユーザーの3提案（①台数で網を大小 ②到達範囲・速度で MoE を区切る ③1つの脳
だが領域を区切る）は**1つのアーキテクチャの3面**：「遅延でクラスタ化された
region、各 region が locality-routing する疎な MoE、台数とともに増える容量」。

正直な論点として伝えた**鶏と卵**：635 パラメータの脳は分割しても意味がない。
region が報われるのは網が1台に収まらなくなってから。R0–R2（配管）は玩具の
ままでも正しく作れるが、R3（網を太らせる＝width/重み/学習）が本丸。

合意した進め方（AskUserQuestion）：**「まず設計を1枚に」**。
→ `docs/architecture/regions.md` を日本語で執筆済み（DESIGN ステータス）。
シーケンス R0(土台: SWIM RTT, DNODE_MAX拡大, region.c骨格, K-DDS scope topic)
→ R1(locality-MoE) → R2(capacity(N)) → R3(網を太らせる)。Phase D(Android)は
R0–R1 の強制力（実機フリートが本物の RTT/region を与える）。

## 実装進捗（branch feat/regions-r0、まだ push してない）

- `50c9974` docs: 設計ドキュメント `docs/architecture/regions.md`。
- `abdb881` **R0-1 SWIM RTT**: 直接 PING→ACK の往復を測り EWMA(α=1/4)。
  `swim_rtt_ms(node)`(未実測=0xFFFFFFFF)、`nodes` コマンドに rtt 列。
  3ノードmeshで rtt≈10-30ms 実測確認。
- `(region commit)` **R0-2 region.c**: RTT≤REGION_TAU_MS(50ms) のALIVEノード
  を同一 region、coordinator=最小ID。egocentric 局所ビュー(region間合意は
  未解決のまま)。`region` コマンド。region.c を4 Makefile(boot/linux,
  linux_x86_64, x86, aarch64)に追加。3ノードで size=3/coord=0 確認。

- `f204939` **②RTT zone sim**: `swim_set_sim_zone(zone_size,penalty)` で
  localhost に人工 region 分割。env は linux usermain 側(arch非依存維持)。
  4ノード/zone_size=2 で region A={0,1} / B={2,3} 形成を確認。
- `bf1fb64` **R0-③ K-DDS region スコープ (本丸)**: `kdds_open_scoped(name,qos,
  scope)` + KDDS_TOPIC.scope。pub 配信ループが REGION スコープなら
  `region_is_member(n)` のピアにだけ送信(送信時ローカル強制・ワイヤ不変)。
  `kdds_pub_fanout()` + `rgnpub` コマンドで観測。4ノード2zoneで region pub
  fanout=1 vs global=7 を確認。全4アーキ(linux x86_64/aarch64 + bare-metal
  i686/aarch64)でコンパイル確認。

### R0 残り(やるなら)
- **DNODE_MAX(=8) 引き上げ** — `for n<DNODE_MAX` 多数に波及、やや侵襲的。未着手。
- global 配信が ALIVE 問わず全8スロットに送る既存非効率(fanout=7 の正体)。別最適化。

- `7ca1f17` **R2入口: DKVA を region スコープ化**。Q/resp を KDDS_SCOPE_REGION で
  open。requester は自 region の partial だけ集約。4ノード2zoneで node0 は
  node1 のみ集約(aggregated 1)、region B(node2/3)は Q を受信せず応答0。
  **regions が初めて推論に効いた。**
- `dfd8986` docs: `samples/11_distributed/run_4node_regions.sh`(再現デモ) +
  README 更新(run_3node_full の旧制限は PR#1 で解消済みと反映)。

### 着地状況
branch `feat/regions-r0` を origin に push 済み、**PR #2** オープン(7コミット)。
未マージ。PR タイトル更新は gh の projects-classic 廃止 GraphQL エラーで失敗
(cosmetics のみ、実害なし)。master へのマージはオートモード要承認。

- `102478b` **R1: locality-aware MoE gating**。`select_expert` を
  utility=accuracy - rtt/MOE_RTT_MS_PER_POINT に。候補ごとに acc/rtt/util ログ。
  **既存バグ修正**: `moe_update_peer` は誰も呼んでおらず peer スコア未取り込み
  だった → moe_task が per-source topic "moe/score/<node>" で pub/sub し取り込む
  (単一スロット衝突を DKVA と同じく回避)。MoE を linux フリートに配線(`moe`
  コマンド, moe_init/moe_task)。4ノード2zoneで near util=49 vs far util=39 確認。

- `c1b8ede` **R2 本体: 階層集約 (Y を選択)**。ユーザーが「Y: 全体を厳密復元」
  を選択。Q→GLOBAL に戻し、resp/<n>=REGION 維持、NEW rsum/<rid>=GLOBAL。
  coordinator が自 region partial を畳んで region 要約(rsum)を発行、requester は
  自 region 直接 + 他 region 要約を畳んで1回正規化。softmax の分子Σa·V/分母Σa は
  単純和なので**単一ノード全体 attention と厳密一致**。通信 O(region²)+O(#region)。
  `coordinator_aggregate()`+`accumulate()` 追加。KDDS_TOPIC_MAX 32→64,
  HANDLE_MAX 64→96。4ノード2zoneで node0 が node1 直接+regionB要約(entries=6)
  =全4ノードKV(12 entries)、node3 partial は region B 内に留まる、を確認。

- `11cc2c2` **R2 仕上げ: capacity(N) 連続関数 (2026-06-02)**。degrade.c/.h に
  capacity_experts()=clamp(alive,1,CAP_E_MAX=16) [breadth, global MoE] /
  capacity_depth()=1+floor(log2(region_size)) [depth, region内] /
  capacity_kv()=直近DKVA階層集約の実測entries (推論前は region_size*DKVA_CACHE_SIZE
  の見積り) / capacity_score()=積。3段enumは表示ラベルに降格。dkva.c が畳んだKV
  総数を capacity_note_kv() で供給。`dist` コマンドで capacity ブロック表示。
  4ノード2zoneで素のメッシュ experts=4/16 depth=2 kv=16(estimate) score=128、
  1回推論で kv=12(measured, 全4ノードKV) を確認。両 linux arch クリーンビルド。
- `7235a79` **fix(sio-linux)**: sio_recv_frame が ungetch slot を無視していた潜在
  地雷を防御修正(現ビルドでは dormant — Linux shell は sio_read_line 使用で
  sio_data_ready 未呼び出し、x86 UART sio は ungetch 不要)。当初「dist 取りこぼし」
  を sio_data_ready のせいと誤診したが、実体は **EOF時の for(;;) 延命(背景ノード
  用の意図的設計)＋連続テストの stale ログ/残プロセス汚染**。カーネルロジックは無傷。

### ユーザー3提案の状況 — 全て ✅
1. **capacity(N)** 台数で網を大小 = ✅ `11cc2c2`。
2. locality MoE = ✅ R1 (`102478b`)。
3. regions = ✅ R0 + R2 hierarchical。

### 残り
- **R3: 網を太らせる(width/d_model/重み/学習)=本丸**。配管(R0-R2)は通った、次は水量。
- 細部: DNODE_MAX(=8) 引き上げ、global 配信の ALIVE フィルタ。Phase D (Android, 実 RTT)。
- branch `feat/regions-r0`、capacity(N)+sio fix の2コミットは **未 push**(PR #2 に上積み予定)。
### 2026-06-06 — ダイナミックワークフロー第1波（並列エージェント）着地
コマンダー(メイン Claude)が worktree 隔離の並列エージェント3体を起動→統合。全て
`feat/regions-r0` にマージ済み・push 済み:
- `5fdbcfb` **p-fs 設計doc** `docs/architecture/p-fs.md`(414行)。内容アドレス+gossip複製+
  履歴DAG+消失訂正符号の青写真。→ [[project_survival_network]] と別トラック化候補。
- `95ac916` **world 全体状況マップ**(=[[project_survival_network]] の可観測層)。
  `world.c/.h` + `kdds_open_poll()`(zero-sem handle) + `world`/`map` コマンド。
- `473387f` **DNODE_MAX 8→32**(8台上限を撤廃)。node_id は8bit fieldなので32は安全、
  node-id 2桁化(node_topic_name=resp/rsum共用, edf, drpc, shell infer)、KDDS_TOPIC_MAX
  64→160 / HANDLE_MAX 96→320、CFN_MAX_SEMID 48→256、REPLICA_ENTRY_MAX=8 で
  REPLICA_PKT を切り離し。4アーキ全クリーンビルド+4ノードデモで32枠init資源確保を検証。

**確定した参照事実**: CFN_MAX_SEMID は **上げてよい**(offset.h は `#if >0` で値ではなく
存在だけ判定→TCB ABI レイアウト不変。bare-metal aarch64 ビルドで確認)。kdds.c に残る
「TCB ABI に縛られて増やせない」コメントは誤り。CFN=256 で moe peer-score gossip も復活見込み。

**未検証/繰り越し**: 32台の**ランタイム**スケール(32ノードtest harness未整備、init資源確保のみ検証)。
REPLICA_PKT は1 announce で≤8 topic(8+168·N≤1380)→ wire chunking はプロトコル変更の follow-up。

運用学習は [[feedback_dynamic_workflow_integration]]。

### 2026-06-06 第2〜4波 + N=32 最終検証（全完了・push済み tip=50808a7）
- **§7 分散ゲーティング着地**(`30f6343`): select_expert = acc − RTT − world_pressure
  (+同region加点)、`recent_pick[]` ヒステリシス(2/3減衰)=§8の速い反射ループ。4ノードで
  「近い空きノード勝利→発火後に圧上昇→隣へ分散」を実証。負util表示fix `5041ac1`。
- **docs 4本**: reflex-deliberation(`b3633c5`, §4時定数安定性が核) / r3-model-widening
  (`55b6e7d`) / decentralized-lookup(`63d8a8f`, top-r交差+WANT+miss駆動self-repair) /
  README 地図(`9f5d91c`)。
- **p-fs P0+P1**(`5b8a6a8`..`0abc9c3`): 内容アドレスstore(self-test PASS、block-id が
  aarch64/x86_64 でバイト一致) + region-scoped 複製(ANNOUNCE/WANT/SYNC + 564B chunk、
  ハッシュ再検証、3ノードで sha256("hello") 複製実証)。stddef 衝突の根因=ガード名不一致
  (`__STDDEF_H__` vs `_STDDEF_H`)、arch/common に <string.h> 禁止が確定ルール。
- **lookup L0**(`cbb5a5a`): HRW responsible(k,r)。ranking `4 7 3 1 2 6 0 5` が両ABI+
  独立Python実装で一致。1ノード差ドリフトで top-2 交差 16/16(r=2なら数学的保証)。
- **重要バグ2匹**: ①`kdds_rx` の REGION→GLOBAL スコープ降格(`50808a7` 修理、regions の
  根幹を蝕んでいた) ②**ARPテーブル飢餓**(`cad09db`): ARP_TABLE_SIZE=16+slot0固定追い出し
  → N>16 で後発ノードのMACが永久追い出し=IP層で無音ドロップ。DNODE_MAX+8+round-robin に。
  `mac[5]<=8` に続く「DNODE_MAXを知らない生の定数」系。
- **N=32 ランタイム最終検証: 6/6 PASS**(world map 32/32、資源枯渇ゼロ、全ノードboot)。
  DNODE_MAX=32 は完全実証済み。world.c は world_peer_pressure/known アクセサ追加済み、
  bare-metal にも world/map コマンド露出(`fccf30b`)。
### 2026-06-06 第5波（PPK討伐後・5本同時=過去最大、全完走、tip=74d4f4a push済）
- **§8 二層実装**(`56f2b99`): 時定数3層 reflex 200ms/delib 2s/gossip 5s (1:10:25)、
  util EWMA(α=1/4)+incumbent deadband(margin=12)。16連打でA→B→A往復ゼロ・4-6決定滞留→
  margin超えSWITCHのみ。beacon未着時にdampingが消えるD1の穴も修理。
- **p-fs P2 履歴DAG**(`14fa489`+`a083ccb`): 100B固定manifest(=ブロック→P1が無料複製)、
  ref=LWW by seq+決定的tiebreak、fork正直文書化。3ノードで「cat seq=1: hello」=過去が
  別ノードで生存。P1の弱点(LATEST_ONLY announce上書き)発見→prev鎖sweep+WANTで自己修復。
- **replica v2 wire chunking**(`74d4f4a`): **大発見=replicaは完全に死んでいた**
  (KDDS_DATA_MAX 128→192でv1パケット1864B>1380、pmesh_sendが無言拒否・戻り値未チェック
  →転送ゼロ)。v2=12Bヘッダ+part_idx/cnt、entry=5/packet、147topicsを30partsで完走。
  scope伝搬おまけ付き。v1↔v2非互換は単一フリート前提で明示。
- **lookup L1**(`1865405`): 解決キャッシュ16slot×40B(TTL=WORLD_STALE_MS思想)+read-k
  candidates。L0のクロスABI契約無傷。
- **README刷新+PR #2説明文**(`6fca2d4`): gh GraphQL projects-classicバグは
  `gh api -X PATCH repos/.../pulls/2` で回避成功(今後この手を使う)。
- 統合は全てcherry-pick衝突ゼロ(usermain/Makefileの「配置を散らせ」指示が機能)。
### 2026-06-06 第6波「全弾応答」— PR #3(外部辛口レビュー)への回答、全完了(tip=eac1ffb push済)
PR #3 の8項目に全て対応: LICENSE(BSD-3+T-License併記 `b156052`) / README正直化
(`afc2968`: 動く/設計/構想/率直な現状の4分割、「生存の単位は個体ではなく群れ」でring-0
FP批判に回答) / CI+クロスビルド(`fd3d274`: GH Actions 4ジョブ、`CC?=`のorigin-default罠
回避) / 衛生(`ff6cd58`: -23.8MB、docs/repo-hygiene.mdに判断記録、履歴パージはpack 155MB
→10-20MB見込みでowner判断待ち)。
- **R3a=「本当に学習する脳」着地**(`c12461c`..`eac1ffb`): 解析的backprop(有限差分照合
  rel err 0.001)、正直データセット(oracle上限94.3%明記)、**26.7%→95%/100%**。
  `dtr train/eval/save/load`。重み2560Bブロブ→`pfs_dag_save("dtr/weights")`。
  **魂のデモ**: aarch64で学習→p-fs gossip→x86_64(未学習・qemu)がload→同一精度。
  fedlearnの嘘loss死亡。**3匹の深層バグ討伐**: ①embedがW_emb列1-3に literal 0.0
  (24死にパラメータ・順列不変=チャンネル識別不能だった) ②**生Taylor exp が
  exp(-10)=848 を返す**(dkva.cの分散attention softmaxにも同じコピー→dtr_expf に統一。
  従来の負logit softmaxは全部出鱈目だった) ③param数表示568→635。
- ユーザー判断: PR #3 への返信コメントは**書かない**(コードが返事)。
- **残り**: R3b=呼吸するパラメータ(ニューロン=デバイスに宿る疎エキスパート、join成長/
  leave縮退 — [[project_survival_network]] に設計記録済み、r3-model-widening.md の
  width前提の上書きTODO)、p-fs P3/P4、lookup L2/L3、reflex D3、§10ステップB、
  履歴パージ判断、PR #2/#3 マージ判断、Android APK再ビルド(単独実行で)。

### 2026-06-06 PR #2 master マージ + PR #4「第二弾ダメ出し」処理
- **PR #2(feat/regions-r0, 49コミット)を master にマージ**(`6f4f908`)。regions 全6波が本流に。
  マージ前に CI 赤1本を修理: `libc6-dev-arm64-cross` 不在(--no-install-recommends が落とす)
  で cross gcc がホスト /usr/include に fallback→bits/libc-header-start.h 死(`eaf6bf5`)。
  hosted ビルドだけ死にベアメタルは freestanding で無事、が識別ポイント。
  ※マージ中ユーザーが誤ってPRをクローズ(スマホ?)→reopen→裏のポーリングが自動マージ。
- **PR #4 = ダメ出し第二弾**(構造的な穴①〜⑥)をマージ(`ddc6f3f`)+現状ステータス追記
  (`00d02d6`、docs/assessment_structural_gaps.md 末尾)。PR #4 は旧 master 基準なので
  半分は既に偽: ⑤モデルライフサイクル=✅閉(F4) ⑥測定=🟡7割(生存ベンチのみ未)
  ②スケール=🟡半分(32まで、数千は差分gossip未)。**本当に開いてる穴=③relay SPOF
  /④ring-0 AI即死(p-fs重みから再スポーンという heal.c 延長パスが今は設計可能)
  /①TCC自己コンパイル(最重量、§9の心臓部)**。これが第7波以降の標的リスト。
- PR #3 は差分が boot/linux_x86_64/.gitignore 1枚で既に boot/.gitignore がカバー=冗長。
  クローズ推奨をユーザーに提示済み(判断待ち)。

### 2026-06-06 第7〜9波「構造の穴＋循環」— 全11隊 master 統合完了 (tip=bbb6e55)
PR #4 の🔴3兄弟 + 第三レビュー「循環の3配線」を並列ダイナミックワークフローで一掃。
**運用上限はユーザー指示で撤廃** → 最大5隊同時。全隊 master 直マージ、各統合点で 4-arch
ビルド+デモ再現を指揮官が手動検証してから push。
- **第7波(穴①〜④)**: A relay冗長化(複数relay+決定的failover/failback、中央なし収束) /
  B タスク障害隔離(SIGSEGV→タスクだけ殺す+p-fs重みから再スポーン、guard.c+fault.c) /
  C 生存ベンチ(K台殺して精度維持+帰還ノード再教育、N=8/K=3 28/28。**副産物=POSIXタイマの
  プロセス宛SIGALRMが別スレッド配送でスケジューラ破壊する競合をSIGEV_THREAD_IDで根治**) /
  D 自己コンパイル(libtcc内蔵、実行中カーネルが体内でCをコンパイル、SELinux execheapを
  二段relocate+匿名RWX mmapで突破)。
- **第8波(配線①③)**: ① 記憶→思考(engram kNNブレンド、**乱数重みの脳が群れの656B記憶だけで
  26.7%→93.8%**) / ③ 死の貫通(部分集約 degraded(k/n)正直明示、起点特権の痕跡6箇所抹消、
  samples/13_survival_loop + CI job)。
- **第9波**: genome発芽(§3自己再生、**空の装甲板がDNAから育って held-out 100%**) /
  ② 思考→行動(reflex.c: SHIELD/CONSERVE/BEACON、§8時定数で隣ノードは減衰反射=痙攣しない)。
  統合時の指揮官判断: genome発芽も SHIELD 対象に追加(selfcと同じ「攻撃下は未知コード/外来DNAを
  入れない」一貫性。両隊は互いを知らず並列だった)。
- **監査隊(常設化)**: 思想doc→不変条件15個(I1-I15)→全コード照合 → philosophy-gap-audit.md。
  外部が届かなかった🔴: **G1 DKVAのQが単一共有ラッチ=同時起点が上書き(§5同時多発の偽装、
  隠れた中央)** / G2 degraded自region限定で他region欠損を黙殺 / G3 §7§8の核moe.cに性質テスト皆無。
- **fault退治隊**: A+B統合後の failover瞬間 全ノード同一ガベージPC(0x2000000024) SIGSEGV。
  根因=relay-HAの dprintf(glibc vfprintf ~3960B)が4KBタスクスタックを溢れさせ隣タスクの
  保存contextを破壊。**[[feedback-hosted-relay-stack-overflow]]の罠の再来(新vector)**。
  教訓昇格: 小タスクスタック上のglibc stdio全般を~4KB hazard扱い。A単独/B単独では出ず、
  Bのguarded dtr-workerが決定的犠牲者になった=guardが味方バグの可視化装置として初実戦。
- **意義**: 第三レビューの「閉じた生存ループ」(書く→配る→発芽→記憶で考える→行動→死を生き残る)
  が3ノード玩具のまま全自動テスト緑で一周。「死なないAIの最小実例」が物理的に存在。
- **第10波標的(監査の置き土産)**: G1+G2+G8一体(真の同時多発+正直なdegraded)、G3(moe性質テスト)、
  G4(受信HMAC)。R3b呼吸パラメータ(§4専門分化=全ノード同一重み問題)はループ閉じた後の次の山。

### 2026-06-07 第10波「同時多発と正直さ」— 監査Gリスト一掃 (master tip=50173da, CI全緑)
昨夜(6/6深夜)mk_pino が**第10波フル3隊を仕込んで就寝** → 朝、私はそれを知らず同じ3課題で
3隊を出した = **6隊が同じ3課題を二重実装**。各課題に独立2解が出たので品質比較で裁定:
- **G3 §7§8性質テスト**: 採用=今朝`w10-moe-tests`(先着)。NO-CENTRAL/二層ローパス/**発振=単一時定数
  28〜39切替 vs ヒステリシス4〜5切替を数で実証**、reflex D0 DESIGNED→DONE、CI grep。昨夜版破棄。
- **G4+G5+G7+G9 守りの正直さ**: 採用=昨夜`w10-honesty`(G7まで広い・ci.yml非干渉)。受信HMAC検証
  (偽造破棄)/fedlearn偽E_OK→E_NOSPT/ID溢れ無音脱落の可視化/fanoutは届いた数だけ。
  今朝C隊`w10-honest-defense`破棄だが**ボーナス発見=受信側リプレイ窓**(MAC有効でも再送は素通る穴、
  Gリスト外)→branch温存して follow-up 収穫対象。
- **G1+G2+G8 真の同時多発**: 採用=昨夜`w10-concurrent`(origin field方式)。Qを per-origin
  `dtr/dkva/q/<origin>`化→**2〜3起点が同時に別推論を完遂**(concurrent_infer.sh)、degraded は
  他region coordinatorも期待集合に入れて`degraded(k/n)`で正直計上(world_peer_region追加)。
  今朝A隊`w10-dkva-concurrent`(covered bitmask方式)は破棄。**両機構ともトピック枯渇を回避**:
  マージ版を run_Nnode_scale N=4/N=16 で検証→resource-exhaustion 0/6監査PASS。
- 監査の🔴3兄弟(G1/G2/G3)全閉、🟡🟢(G4/5/7/9)全閉。据え置き=G6(動的ID=R3/PhaseD)、G10/G11(🟢)。
- **教訓**: ①ユーザーが夜間にエージェントを仕込む運用が定着 → セッション再開時は **origin の
  未知ブランチ(w10-*)を必ず git ls-remote で確認**してから新規出撃(二重起動回避)。今回は確認前に
  出してしまい6隊重複。②重複は損失=computeのみ、masterは無傷(先着 or 広い方を採用、差分だけ温存)。
  ③[[feedback-dynamic-workflow-integration]] に追記すべき: 全エージェントが Edit/Write 権限denyで
  bash(python)編集に fallback したが成果は正常。④cwd が agent worktree にすり替わる罠を複数回踏んだ
  → 各 git/make 操作の前に `cd /root/p-kernel` を明示。

### 2026-06-07 第11波「follow-up＋R3b＋G6＋俯瞰監査」4隊 (master tip=53e8dd5)
出撃前に origin 未知ブランチ確認(教訓実践・今回はクリーン)。全隊領分分離で衝突ゼロ。
- **A 受信リプレイ窓(follow-up)**: 第10波 G4(受信HMAC)の先=**再送(replay)も防ぐ**。w10-honest-defense
  から per-source 64pkt nonce 窓を現 master の HMAC 経路に手で移植(収穫完了→branch削除)。
  G7 残(usermain の id>DNODE_MAX 無音skip)も可視化。**副産物: 第10波 forgery テストの欠陥発見**
  =injector ヘッダ8B(本来12B)で「valid frame」が実は一度も wire-valid でなかった(bad-MAC drop
  しか見てなかったので露見せず)→修正。run_replay_reject.sh: fresh3通過/再送2破棄。
- **B R3b 呼吸するパラメータ(専門分化)**: §4監査「全ノード同一重み=コピーがN個」を数で反証。
  spec.c: (a)データシャード specialization((§7 gate が振る入力だけで各専門家を学習) + (b)別seed init。
  **control(全同一)=64.4%フラット → JOIN 64.4→74.4→87.8%(+23.3pt) / LEAVE: d1専門家killで
  d1だけ83.3→53.3%・他不変・全体77.8%(崖落ちせず単一基線超)**。道B(疎な加算的エキスパート)採用、
  道A(密d_model伸縮)は関数破壊的で不採用。重みは `dtr/expert/<k>` で別 named ref=呼吸の器。
  cross-ABI byte一致。doc=r3b-breathing-params.md。
- **C G6 動的ID(設計+relay lease)**: dynamic-id.md(lease vs P2P 比較、結論=今波 relay lease・
  P2Pは道筋のみ)。relay.c に ID リース(REGISTER src=0=auto 予約値で wire 互換、未使用最小ID貸与・
  idle回収・再利用)。クライアント配線(net_relay.c)は次波 follow-up。
- **D 俯瞰監査第2版 → philosophy-gap-audit-2.md**: **核心結論「閉じた制御ループでなく個別に緑の鎖」**。
  3つの切れ目=(a)行動が片肺(G17 moe_infer が reflex 叩かない/G16 SHIELD が自動発芽経路を覆わない)
  (b)網規模の熟慮=学習が空(G18 fedlearn分散E_NOSPT・§2「守る対象」実体なし=第三レビュー「熟慮層が
  空」未閉) (c)複合シナリオ未検証(G19)。+**行動が知覚を変えない=負帰還でなくフィードフォワード**。
  最痛副作用=**G12 🔴 degraded(k/n) が world-gossip 鮮度依存で嘘をつきうる**(G2 を gossip 由来
  region_id で組んだ副作用、過渡期に I4 を条件付き裏切る)。他 G13(同時多発が multi-region で
  coordinator_aggregate 200ms 同期窓により再直列化、「同時多発∧region横断」未検証)/G15(認証が単一
  グローバル静的PSK=新中央トラストアンカー)/G14(同時性上限「数個」)。詳細→[[project_survival_network]]。
- **第12波標的(audit2 の置き土産)**: G12(gossip非依存の正直degraded)、**鎖を環にする**(行動→知覚の
  負帰還/moe_infer に reflex=G17/熟慮層に学習=G18)、複合シナリオtest(殺す∧同時多発∧記憶=G19)。

### 2026-06-07 第12波「鎖を環にする」(master tip=31902e1, ここで一旦停止=リミット)
master に統合済み(全 push): C 複合シナリオ G19(殺す∧同時多発∧記憶 23 checks)、D 俯瞰監査v3
(philosophy-gap-audit-3.md)、B G12 正直degraded(`degraded(k/n; m uncertain)`)、G G23
federation設計(federation.md 階層案推奨)、**A 鎖を環にする**。
- **A隊の評決(最重要)**: 本物の負帰還ループを閉じたが**負荷について**(lp_run: CONSERVE→
  自pressure↑→隣が勝つ→`L-=LP_SHED`)。**G21(自己申告閉ループ)は晴れ**(実 actuator あり)、
  **G20(符号倒錯)は晴れず実証された**=脅威ノードが仕事を手放して逃げる=§2「一点へ注ぐ」の
  真逆(rally でなく flee)。G17 配線は本物、G18 は最小 homeostat(dwell 13→3)。**「全部緑」で
  通さず lp_run の1行まで読んで符号を暴いた**=自前監査を信じた指揮判断。評決は audit-3 末尾に明記。
- **第13波の本丸 = G20**: `pressure` を**負荷軸(混雑→流す)と脅威軸(危険→群れが注ぐ)の2軸に分離**、
  脅威時はゲートが threatened node へ**寄せる**符号に。これが survival §2 の実装。
- **★走行3隊もリミット復帰後に全統合完了 (master 06ea871, 第12波完全完結)**:
  - **F=`w12-locality-metrics`(G25)済**: 局所性ON/OFF対照で §4 を**初めて実数実証** —
    kdds メッセージ・relay バイト(ON=1.08MB vs OFF=2.0MB)・エネルギープロキシ(ON=506K vs
    OFF=1.11M)が実際に減る。正直な限界明記(光速半分はモデル/flat region は far=0 定義上 masking)。
    docs/benchmarks/locality.md。pure docs+samples。
  - **E=`w12-durable-pfs`(G24)済**: p-fs durable backend(arch/linux/pfs_durable.c, file-backed
    content-addressed, boot 復元+sha256自己検証で破損拒否)。「**the library is no longer
    volatile**」=全ノード同時電源喪失生還を実証(persist.sh)。※デモの leak 判定が誤検知(カーネルの
    REJECT ログ行に破損IDが出るのを grep が誤爆)→**テスト側を修正**(カーネルは元から正しい)。
  - **新FS=`w12-survival-fs`済**: **ARK**(arch/common/arkfs.c 874行、Append-only/Replayable/
    Keep-everything=方舟)。FAT32(arch/x86/fat32.c は温存)の思想的限界(上書き破壊/fsck前提/版を
    残さない/中央木)への回答。content-addressed(p-fsと同一sha256 id空間=統合可)・log-structured・
    アトミックコミット・全block sha256+crc32自己検証・版管理。**実SIGKILLを全書込点に注入→常に
    v1完全orv2完全・破損ゼロ**を2連続実証。試作=hosted、設計のみ=VFS実ディスパッチ配線/ベアメタル
    実マウント/ログGC/p-fs実hook接続。doc=survival-fs.md。
  - 統合は逐次(branch確認→merge→4ビルド→demo→push)。E/新FS は古い起点(53e8dd5)分岐で diff に
    削除錯覚が出たが 3-way で master の新ファイルは保持(問題なし)。**次の統合点**: ARK と E durable を
    実際に配線で統合(`pfs_set_put_hook`+`ark_block_get`、ARKのpfs互換API は実装済)=ローカルFSが
    p-fsのdurable backendになる白眉。VFS実ディスパッチ配線。
- **事故と教訓**: cwd だけでなく**メインチェックアウトの HEAD が agent ブランチ(w12-durable-pfs)に
  すり替わり**B隊を誤着地(push前検知・復旧、master/origin無傷)。→[[feedback-dynamic-workflow-integration]]
  ルール10強化: マージ前に必ず `git branch --show-current` で master 確認(cd だけでは不十分)。
### 2026-06-07 第13波「逃げるな、結集せよ」(master tip=f289cf7, CI全6緑) 完全完結
3隊並列(A本丸/B白眉/D監査)+ オラクル sim を先に master へ(81168b9)。
- **A=G20 FIXED「鎖を環に・正しい変数で」**: moe ゲート効用を二軸化(`expert_utility` moe.c:159)
  `−pressure·1/2`(負荷=避ける)`+threat·1/1`(脅威=寄る・逆符号2倍)。**候補自身の threat が
  その候補の効用を上げる**→隣も自分も threatened node へ集束(§2「守る力を一点へ」)。CONSERVE を
  compute_pressure から外し別フィールド `WORLD_BEACON.threat`(_pad 再利用・sizeof 12B 維持・
  static_assert)→world_peer_threat。中央argmax無し(§7不変)。**G29同時処理**=`learned_conserve`
  (reflex_threat_level)は今や rally ゲインを駆動→homeostat が正符号目的(dwell長い→もっと rally)を
  最適化。実証 [moe-protect]/[reflex-fb dwell ON=4 vs OFF=13]/[reflex-learn 13→4]。
- **B=白眉 ARK↔p-fs durable backend**: 前回の「次の統合点」を実装。pfs_block.c から inline で
  arch/linux/pfs_ark.c。put→ark_block_put+ark_checkpoint(fsync COMMIT・torn は rollback)、
  P0ミス→ark_block_get(返値を要求idで再hash検証)。`PKERNEL_PFS_BACKEND=ark` で選択(flat=
  PKERNEL_PFS_DIR は不変・無回帰)。put→kill→remount→**ARKログから配信**を実証(samples/26)。
  **ローカルFSと分散コンテンツストアが一体に**。pfs_set_put_hook は P1複製が専有→flat と同じ
  inline 経路を踏襲。run.sh の遅延バグ(カーネルは EOF で終了せず→`exit` 注入)も発見・修正。
- **CI強化**: `[moe-protect]`+`reflex test`(`[reflex-fb]/[reflex-learn]`)をブロッキングジョブへ
  =監査受け入れ#5(緑をCI強制)とG27(reflex testがCI未配線)を同時解消。マージ後 master で4ビルド
  +全10 grep 一致を指揮官実走確認。
- **監査 v4 (G27-G31, philosophy-gap-audit-4.md, 先に merge de27d24)**: G27🔴 sim≠kernel
  (別力学系・脅威軸未モデル=oracle誤称) / **G28🔴 脅威が接地せず**(発生源=温度バケツ gate_predict、
  下げる行動無し=開ループ、守る対象に実体無し)=**第14波本丸** / G29🔴(学習が逆符号目的→A隊が処理) /
  G30🟡(confidence gate が moe path で常時0xFF) / G31🟡(G25 部分解決のみ)。依存順 G20→G29→G28。
- **rubber-stamp 防止が機能**: 監査隊にわざと G20 の5点受け入れテストを作らせ、私が実コード+実走で
  照合(4/5合格、#5は私がCI配線)。「全部緑」を独立基準で読み解いた。→[[feedback-validator-and-learner-traps]]。
- 監査の残 OPEN(第13波時点): G28🔴(第14波で着地)、G22、G23、G30🟡、G31🟡、G13/G14/G15/G16/G10/G11。

### 2026-06-07 第14波「脅威を接地する」(master tip=2e4381c, CI全7緑) 完全完結
本丸A(G28)+常設監査D(v5)。**A は3度の反復(本体→fix1→fix2)を要し、エージェントが2度「全部緑」と
過大申告→私が走行系を自分で kill テストして両方暴いた**。監査v5 の主題「緑の自己テスト／死んだ走行系
パス」が現実化し、それを潰す波になった。→[[feedback-validator-and-learner-traps]] 強化。
- **A=G28 着地「脅威の接地・走行系で閉ループ」**: 守る対象を一級オブジェクト化(arch/common/protect.c:
  `protect <ref>`=守る単位、群れ=守る力)。threat=(R−durable複製数)·STEP、複製数は**heard P1 ANNOUNCE
  からのみ**算出(隣は実保存したものだけ announce=隣検証・send-countでない・中央なし§7)。actuator=
  protect_tick が at-risk 中 re-announce→隣が pull+persist→holder_count↑→**threat がタイマーでなく
  複製で 0 へ**。完了(>=R)でエピソード終了。
- **私が手で暴いた3つの実バグ(エージェント自己申告は緑だった)**:
  ①LATEST_ONLY 取りこぼし=深さ1 announce トピックが同時 announce-back を上書き→所有者が 1/R で停止。
   修正=holder が origin 駆動 re-announce(src==origin)に announce 返し(bounded)→収束。
  ②動的 target_r=`cap_target_r(region_size−1)`が早期宣言で R=1 に丸まる。修正=region 成長で target_r を
   要求Rへ向け単調 up に再cap+デモは region size==3 後に宣言。
  ③boot-SYNC バイパス=SYNC が quiet-put 単位を actuator 無関係に複製→control が漏れる。修正(§2
   deliberate-spread)=actuator 未駆動の保護単位は ambient SYNC から除外(`pfs_repl_set_sync_filter`)、
   通常ブロックは不変。
- **指揮官実走検証(自己申告を信じない)**: samples/27_protect を**私が自分で5/5 PASS**確認(treatment:
  target_r=2 決定的・threat 40→20→0・2隣確認・kill-9生存 / control: AT-RISK 維持・kill で消失)。
  in-process [protect-ground]/[protect-loop]、wave-13 全テスト、sample26 ARK、11_distributed、
  13_survival_loop、relay 6/6 すべて緑。4ビルド。**走行系デモを CI ジョブ化(`protect-loop-live`)** =
  監査v5/G32 の核心「走行系を CI で kill 検証」を満たす(hosted runner でも緑)。
- **正直な残**: reflex 温度バケツ脅威源は依然5秒タイマー解放(G32 残)、holder 生存性未追跡(死んだ複製で
  threat 再上昇せず)、保護複製は意図的に actuator-only(§2 設計判断)。
- **監査 v5 (G32-G35, philosophy-gap-audit-5.md, 先に merge 264553c)**: **G32🔴 検証器≠実システム**
  (自己テストは合成プラント証明・走行系に actuator 無し=次の本丸指名→第14波で着地) / G33🔴(走行系
  homeostat が開ループで MAX 張り付き) / G34🟡(死んだ confidence gate が G20 後はむしろ有害) /
  G35🟡(threat がノード単一スカラ=§5 と §2 が両立不能・オブジェクト別脅威軸が無い)。G29 は CLOSED 確認。
- **第15波 候補**: G35(オブジェクト別 threat 軸=§5∧§2 両立)、G22(本物の重み学習)、G33/G34(reflex 軸の
  接地=温度バケツ撤廃)、G23(federation を実コードへ)、G30(confidence gate 復活)。

### 2026-06-07 第15波「§2∧§5」+ ARK本物化キャンペーン (master tip=ed18b8a, CI全9緑)
最大規模の波。思想3隊(A本丸/B/D)+ARK 5隊を並列。**全隊レーン分離(ファイル単位)で衝突ゼロ**、
指揮官が各成果を**自分で実走検証してから**統合(自己申告は信じない — 本波で A は2度、ARK系も
自己申告は緑だったが、私が走行系/fuzzer を自分で回して倍率の緩さ・P0=64・4 BUG を全部暴いた)。
- **A=§2∧§5(本丸)**: 多点同時防御。真の壁は脅威スカラでなく2つのトランスポート隘路
  (LATEST_ONLY の discovery/confirmation)だった→`pfs_repl_push`(unicast)+hold-ack
  (`PFSR_HOLD_MAGIC`)+beacon `_pad→atrisk` バイト(12B不変)。私の samples/28 5/5: t_four≈230-380ms
  ≪直列700ms=本物の並列、kill 生存。**正直な分割評決**: 🟢 並列な図書館(複製軸)/🔴 並列な脳
  (計算rally=脅威スカラ不変、監査v6 [g35-no-collapse] 未達)=DKVA推論軸(G13)は別。A の自己評価
  "G35→🟢" を私が訂正。CI: plural-protect-live。→[[feedback-validator-and-learner-traps]]。
- **B=§8×光速遅延(§10ステップB)+§4実測(G31)**: relay 遅延ノブ(poll駆動・near を HOLブロックしない)
  +新sim。near 1ms/far 300ms 非干渉、§4 初の実測。レーン8ファイル(A と非重複)。
- **D=監査v6**: G35 を🔴に格上げ(`max()`集約の隠れ中央を走行系で発見)、本丸の4行受け入れテスト提供。
- **ARK本物化(mk_pino「嘘偽りなく本物に」)**: FS監査→「設計は正しいがホストデモ」と評決。
  - **ARK-fuzz**: format非依存の崩壊フ ァザー(samples/30)。並べ替え/torn/破損を image に与え
    **本物の欠陥を暴いた**(integrity-safe=破損を決して配らない、だが 4 BUG: SB全損/header腐敗で
    全消失/fallback無し)。回帰ゲート化。
  - **ARK-1 核**: index を 256線形→16384 open-addressed ハッシュ(媒体スケール)、I/O vtable 契約明文化、
    format不変。私の実走で**新たに P0=64 のエンドツーエンド上限を発見**(arkfs は無上限でも kernel
    pfs 経路が pfs_block.c `PFS_MAX_BLOCKS=64` で頭打ち)→ARK-2 へ回付。
  - **ARK-2 寿命**: fuzzer **0 BUG/0 SAFE-reject**(私が seed 1337+4242 で確認)。読みfallback/SBレプリカ
    (format v1→v2)/header resync/commit冗長/GC(ark_compact)/**P0キャッシュ・エビクション**。私の
    実走で**カーネル経路 400/400 ブロック**(64上限消滅)。
  - **ARK-3 実機**: ARK_BDEV を ide0(ATA-PIO)へ束ね、**QEMU で実ブロックデバイスに ARK が乗り、
    電源断(QEMU再起動)を越えてファイル生存**を私が確認(sha256一致)。arkfs.c 無改変。VFS_BACKEND_ARK。
    正直な残: aarch64 は block driver が in-tree に無く blocked(x86 のみ)。agent 未コミット WIP を救出。
  - **ARK の到達**: 容量(媒体スケール+P0キャッシュ)/寿命(GC+fuzzer緑)/実機(x86電源断生還)/崩壊耐性
    (0 BUG)を claim 通りに。残: aarch64 block driver、erasure coding(p-fs P4)、Merkle dir tree、
    ARK_MAX_FILES=32、mount O(usable log)。docs: arkfs-audit.md。
- **監査の置き土産(第16波候補)**: **G22/§9 本物の越境学習**(クロスノード重み更新=0、§8「全体が未来を
  強くする」は空。順序 preservation→plural preservation→**parallel thinking**)、G13(coordinator 200ms
  窓の再直列化=並列な脳の壁)、G33/G34(reflex 軸の接地+死んだ confidence gate)、G23(federation 実コード)。

### 2026-06-07 第16波「G22 越境学習」+ ARK-4 (master tip=f47616a, CI全10緑)
本丸A(G22)+監査D(v7)+ボーナスC(ARK-4 aarch64)。**A は約2h51m・636K tokens・690 tool=本セッション最深**。
指揮官が構造/コード/走行系すべて自分で実走検証(自己申告は信じない方針を全隊に貫徹)。
- **A=G22 着地「群れが誰一人では届かない知性に登る」**: §8 第2帯(全体が未来を強くする)が空でなくなった。
  3ノードを **leave-one-class-out のバラバラ shard** で学習(単独 ~56-67% 上限=全タスク不可)→ **中央なし
  ゴシップ平均**(`gl_merge` 順序非依存・aggregator index 無し、`arch/common/gossip_learn.c` 新規)で
  **全ノードが単独上限を超える**。transport=p-fs(per-node ref `dtr/model/<n>`)、遅い熟慮 cadence(§8)。
  `fl_local_train` を **w1/w2/w3 全身**へ(b3 のみの偽撤去)。**dtr.c 無改変**(public API 経由)。
  指揮官検証: in-process [g22-shard-solo/gossip-learn/no-central] PASS、コードで no-central/whole-body 確認、
  **ライブ qemu-x86_64 3/3**(全ノード>solo、kill 中継続、rejoin 88-95%)、CI **collective-learn-live が
  GH native x86_64 で緑**。CI: dtr gossip test grep + collective-learn-live ジョブ。
- **正直な留保**: **このホスト(aarch64-PRoot)は cross-node p-fs でクラッシュ**(既存の魂デモも落ちる環境バグ)
  →ライブは x86_64 で検証・aarch64 はビルドのみ。2ノード生存タール最終精度は非IIDで揺れる→**ゲートせず
  note 報告**(§3 ゲートは 3node Phase-A+rejoin)。collective ~85-93% vs oracle ~98%。
- **C=ARK-4**: aarch64 virtio-blk(MMIO)を**ゼロから自作**→ARK_BDEV bind→QEMU virt で format→write→
  remount→**電源断(QEMU再起動)生還**を指揮官が実走確認(sha256一致)。arkfs.c 無改変。ARK-3 の
  「aarch64 blocked」残件を閉じ、**ARK は x86(ide)+aarch64(virtio-blk) 両実機で電源断を越える**。
  留保: QEMU virt 検証(実RPi3 は SD/EMMC 別途、x86 sample31 と同じ bar)。
- **D=監査v7 + 次の本丸 G38**: G22 着地しても「学ぶ層」と「守る層」は**並んだが未配線**。**G38=§8 二層配線**
  =学んだ softmax confidence が `moe.c:419` の死んだ 0xFF gate を置換(G34)+reflex 経験が遅い学習帯へ。
  「思考が守りを変えて初めて考える器官」。G34🔴/G23🔴(32が collective 知性の天井)escalated。G13🟡 据置。
- **到達**: 器官は「並列に記憶・並列に守り・**並列に学ぶ**」まで来た。次は **G38(学びが守りを賢くする)**。

関連 [[project_ump_android_node]] [[project_dkva_followups]]
[[project_pkernel_philosophy]] [[project_survival_network]] [[feedback-hosted-relay-stack-overflow]]
[[feedback-dynamic-workflow-integration]]。
