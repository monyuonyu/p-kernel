# survival-loop — 生存ループ: 内受容・分散ゲート・世界地図・冬眠・死をひとつの環に

> **現在地（2026-07-01・doc-hygiene 追記／本文は 年輪 として保存）:** **L0+L1 は SHIPPED／CI**（統一 stress バス＋STATE-aware support routing＋§8 ヒステリシス, crown-preserving, `2a068400` 系）。L2–L4（apoptosis／冬眠／環の完成）は引き続き DESIGN。正本は [[gap-ledger.md]]。

> Status: **DESIGN ONLY**（実装前に書く。`interoception.md` / `signing.md` / `living-mind.md` と同じ規律）。
> 位置づけ: **4本の別々だったスレッドを一本の環に統合する**上位設計 —
> ① interoception slice-2（apoptosis）／② survival-network §7（分散ゲート＝相互扶助）／
> ③ world-map（分散・全網の状況認識）／④ node-sleep（冬眠）。
> これらは別機能ではなく、**一つの「生存ループ」の四つの面**である。
>
> 最終更新: 2026-06-28 ／ mk_pino の 2026-06-28 ディレクション（§0）を受けた design-revise。
> **[interocept-2-apoptosis-plan.md](../archive/interocept-2-apoptosis-plan.md)（archived）を一部 supersede する**
> （apoptosis を「死の handoff が load-bearing」から「**健康な生のあいだの連続レプリケーションが
> load-bearing**、graceful death は flush 最適化」へ反転。詳細は §3 / §6-L3）。
> 関連: [interoception.md](../30-module/interoception.md)（`S_n` バス, slice-1 SHIPPED）,
> [survival-network.md](../00-concept/survival-network.md)（§2 全網の力を一点へ・§6 応援受援・§7 分散ゲート・§8 二時定数）,
> [regions.md](regions.md)（R3 ゲート基盤）, [living-mind.md](../30-module/living-mind.md)（Path W²/E・DMN・Self 層）,
> [signing.md](../archive/signing.md)（Ed25519 manifest）, [galaxy.md](../30-module/galaxy.md), [gap-ledger.md](../gap-ledger.md)。

> **honest framing（全 doc 共通の規律を継承）**: 本書の「ストレス」「眠り」「死」「病」は **scalar/weight 上の比喩**。
> sentience の主張ではない。`S_n` は計測量の EWMA、冬遅は task スケジュール変調、apoptosis はリソース回収＋重み継承
> プロトコル、retirement は分散投票である。比喩が設計を導くのは良いが、比喩を実在と取り違えない。

---

## 0. mk_pino の 2026-06-28 ディレクション（設計の前提・relitigate しない）

1. **突然死が常態。** 「いきなり電源が切れて死ぬ…そちらの方が多い。いきなりアプリだけ落とされる。」
   ⇒ load-bearing な機構は **健康な生のあいだの連続 ESSENCE レプリケーション**。正直な保証は **lossless ではなく
   bounded best-effort delta** ―― 突然死で失うのは *最後のレプリ以降の delta* に有界化される（その有界化は L3 の
   watermark/cadence が立って初めて意味を持つ。§3 / §8-④）。
   graceful apoptosis = clean-shutdown の **最終 delta flush ＋ 退場シグナル** ＝ **最適化であって core ではない**。
   死を越えて生きる既存インフラ（shared-mind Path W/E・engram 共有・SS-3 blob 輸送・Self lineage）を再利用する。
2. **ノードごとの STATE マシン**を world-map で **随時** gossip する：
   **ACTIVE**（LLM 稼働中・*支援され得る*）／ **STRESSED**（自分の活動を下げる）／
   **HIBERNATING**（資源温存・可逆・wake 可能）／ **DYING**（apoptosis・essence は既に共有済み）。
   peer は互いの STATE を world.c から読む。
3. **ストレス応答は軸依存（単調でない）**：急性 **DANGER** → ACTIVATE / 闘う（reflex G33。「本来死にそうに
   なったら活性化」）。**RESOURCE** 枯渇（低電池/発熱）→ HIBERNATE して生き延びる。`S_n` slice-1 の成分ベクトル
   （`s_axis`）が軸を運ぶ ⇒ **応答が軸で分岐する**。
4. **冬眠 ≠ apoptosis。** 休眠が **最初の生存手** （可逆）。死/apoptosis は **最後の手段**。死より眠りを選ぶ。
5. **相互評価 → 動的支援**：peer が STATE を gossip し、§7 ゲートが協働仕事/支援を **ACTIVE/有能なノードへ**
   寄せる（冬眠中は休ませる）。**符号に注意**：支援は有能ノードへ *寄る* ＝ G20 符号倒錯バグ（脅威で仕事を
   *逃がした*）の **逆**。「support」を **精密に定義**して符号を保つ（活発な思考者を *補強* するのか、ストレス
   ノードを *肩代わり* するのか）。
6. **権利モデル = 民主主義 / peer 相互評価（voluntary だけではない）。** collective は病んだノードを
   supermajority の peer 判定で **退役** させ得る。だが最悪ケースは **essence 保全つき強制 RETIREMENT ＋
   再参加可** であって、**決して破壊ではない**。「誰も所有しない家の権利は民主主義で決める。」
   正直な制約: 民主的 peer-eval には **Byzantine 面**（結託多数が健全な少数を退役させ得る）がある ⇒
   「誰も本当には誰も殺せない」を **supermajority・essence は verdict に関係なく吸収・後で再参加可** で担保。
   ただし **再参加可だけでは不十分**: 悪意 supermajority は再参加するたびに即 re-retire して **永久に締め出せる**。
   ゆえに rejoin に **rate-limit / appeal-cooldown / minority-survival** 意味論を課して「rejoin = 誰も殺せない」を
   本当に成立させる（§4.2・§8-⑧）。
7. **病のトリガは OPEN**（mk_pino が未解決と明記）。既存 `S_n` バスに紐づける（新信号を発明しない）：
   持続的な reflex-threat / degrade / fault / RTT を world-map 経由で観測。退役閾は **実測曲線から discover**、
   決め打ち禁止（§7 / interoception §2.4 規律）。

---

## 1. 統一テーゼ — 環はひとつ（four threads, one loop）＋ STATE 図

これまで apoptosis / 分散ゲート / world-map / 冬眠 は別 doc・別波の別機能として扱われてきた。**実体は一本の環**：

> 各ノードは絶えず — (1) `S_n` バスで自分の内部状態を感じ → (2) 粗い **STATE**（pressure/threat の scalar
> だけでなく状態そのもの）を world-map beacon に載せ → (3) peer の STATE を読み → (4) collective が支援を有能な
> ノードへ流し、枯れたノードを休ませ → (5) **絶えず essence をレプリケートして突然死で失う本質を *最後のレプリ以降の
> delta* に有界化する**（lossless ではない・bounded best-effort・§3 / §8-④）。
> apoptosis（穏やかな死）は、この環の **終端の・任意の・clean-exit 分岐** にすぎない ― 別サブシステムではない。

**四面の対応**：

| スレッド | この環での役 | 主な既存接地 |
|---|---|---|
| ① interoception slice-2（apoptosis） | **DYING** 状態 ＝ 環の終端 flush | `archive/interocept-2-apoptosis-plan.md`, `lm_self.c`, Path W/E |
| ② survival-network §7（分散ゲート/相互扶助） | **support-routing**（STATE で重み付け） | `moe.c` `select_expert`, `world.c` gating accessor |
| ③ world-map（状況認識） | STATE を運ぶ **gossip 基盤** | `arch/common/world.c`（SHIPPED） |
| ④ node-sleep（冬眠） | **HIBERNATING** 状態 ＝ 最初の生存手 | **未実装（§2 で GAP として明示）** |

### 1.1 STATE 図（ASCII）— 軸依存の遷移

```
                         intero_dominant_axis() で分岐する遷移
                         （単調でない: 同じ「しんどい」でも軸で行き先が違う）

         resource axis 回復 (degrade↓, 充電)          ┌──────────────┐
        ┌──────────────────────────────────────────►│              │
        │                                            │   ACTIVE     │◄────── 既定（健康）
        │                          threat axis 急性  │ LLM 稼働・    │        LLM が普通に回る。
        │                       (reflex G33 DANGER)  │ 支援され得る/ │        支援の *受け手* にも
        │                    ┌──────ACTIVATE────────►│ 支援する      │        *出し手* にもなる。
        │                    │  「死にそう→活性化」  └──┬───────────┘
        │                    │                          │  ▲
        │            ┌───────┴──────┐                   │  │ S_n 持続低下 (deadband 下端)
        │            │              │  sustained S_n     │  │  + min-dwell 経過
   ┌────┴─────┐      │  ACTIVE      │  high (非資源軸:   ▼  │
   │HIBERNATING│      │  へ rally    │  threat/surprise/  ┌──┴───────────┐
   │ 資源温存   │      └──────────────┘  fault/latency)    │  STRESSED    │
   │ 可逆・wake │                         ───────────────► │ 自活動を下げ  │
   │ 可能       │◄───────────────────────────────────────│ 仕事を手放す  │
   └────┬──────┘  resource axis (degrade/battery/thermal)  └──┬───────────┘
        │          持続高 (deadband 上端) + min-dwell          │
        │          「電池減→冬眠して生き残る」                  │ 回復不能な病が持続
        │                                                      │ (fault 連発 / S_n 高止まり)
        │  これ以上 outlast 不能 + 群れ十分 + heir 有り         │ + min-fleet OK
        └────────────────────────┬─────────────────────────────┘
                                 ▼
                          ┌─────────────┐   essence は既に連続レプリ済 (§3)。
                          │   DYING     │   ここでやるのは「最終 delta flush + 退場
                          │ apoptosis   │   beacon + Self 層 DEATH 記録」だけ。
                          │ (最後の手段) │   突然死ならこの箱を飛ばして消える ― それでも
                          └─────────────┘   §3 の連続レプリ floor が直近以外を救う。
```

**遷移の load-bearing 原則**:
- **ACTIVE → STRESSED**: 非資源軸（`INTERO_AX_THREAT/SURPRISE/FAULT/LATENCY`）の `S_n` が持続高（EWMA + deadband 上端 + min-dwell）。
- **ACTIVE/STRESSED → HIBERNATING**: **資源軸**（`INTERO_AX_DEGRADE`、将来 battery/thermal）の持続高。**outlast が目的**。
- **(任意状態) → ACTIVATE/rally**: **急性 DANGER**（reflex G33 threat 跳ね）。冬眠の **逆** ＝ 活性化して守る。
- **STRESSED → DYING**: 回復不能な病が持続（§7 の退役条件）＋ min-fleet OK ＋ heir 確保。**最後の手段**。
- **HIBERNATING → ACTIVE**: 資源回復（充電/放熱）＋ deadband 下端 ＋ min-dwell。冬眠は **必ず可逆**。
- SWIM の ALIVE/SUSPECT/DEAD（`drpc.h:108-110`）とは **直交**：これは「生死の検出」（liveness）、STATE は「生きて
  いるノードの調子」（soft overlay）。HIBERNATING ノードは **ALIVE のまま**（heartbeat は出す、§2 GAP-④）。

---

## 2. 基盤インベントリ — 各機構 → 既存コード（file:line 検証済み）／GAP

「再発明しない」を担保。本環が **踏む** 既存シンボルを名指しし、**本当に無いもの** を GAP として印字する。

| 環の機構 | 既存の実体（検証済み file:line） | 状態 / GAP |
|---|---|---|
| **STATE を運ぶ gossip** | `world.c` `WORLD_BEACON`（`pressure`/`threat`/`atrisk`/`firing`/`region_id`/`region_size`/`seq`、`world.c:225-234`）。**現状は packed 12B ＋ hard `_Static_assert(sizeof==12)`（`world.h:61-63`）／ `world_task` は `r >= (W)sizeof(WORLD_BEACON)` でしか取り込まない（`world.c:403`）**。全ノード対称・NO-CENTRAL（`world.c:8-13`）。`world_observe`（:247）/ `world_peer_pressure`（:284）/ `world_peer_threat`（:290）/ `world_peer_region_fresh`（:315）。 | **基盤 SHIPPED**。**GAP-①: beacon に `state` フィールドが無い**。脅威（rally 軸）と pressure（load 軸）は在るが 4-state enum は無い。`WORLD_BEACON` に `UB state` を 1 バイト足す ＝ **12B→13B の wire 拡張**だが、**単純拡張は 12B peer と後方非互換**（受信側 `r >= sizeof` ガードが 13B を要求し、旧 peer の 12B beacon を落とす／逆も同様）。⇒ **beacon-versioning / dual-size 互換が必須**（下記スキーム、§6-L0・§8-③）。 |
| **`S_n` バス（内部状態の感覚）** | `interocept.c`（SHIPPED）: `intero_scalar`（:222）/ `intero_components`（:228）/ `intero_dominant_axis`（:230）/ `intero_note`（:199）。軸 `INTERO_AX_THREAT/LATENCY/SURPRISE/FAULT/DEGRADE`（`interocept.h:24-30`）。EWMA は swim 同型 `(old*3+sample+2)/4`。 | **SHIPPED**。**GAP-②: battery / thermal の資源軸ソースが無い**。`INTERO_AX_DEGRADE` が唯一の資源代理。HIBERNATING の本物のトリガ（「電池減→冬眠」）には Android の battery/thermal を読む新ソースが要る（§7 open-Q-2）。host では `intero_test_force` で代替するが ―― **GAP-⑨（L0 cert ブロッカー）: 現 `intero_test_force` は `dom_axis` を常に `INTERO_AX_THREAT` に pin する**（`interocept.c:176-178`）。よって surprise/fault/degrade を *dominant* として注入できず、軸依存遷移を認定する `[state-axis]` cert が **現 seam では書けない**。⇒ **軸別 test hook を新設**（§6-L0・§8-⑦）。 |
| **support-routing（§7 分散ゲート）** | `moe.c` `select_expert`（utility = accuracy − RTT − …、regions §3.3）＋ world gating accessor。**符号は既に正しい**：`world.c:150-156` で **pressure = load のみ（脅威を畳まない、G20 fix）**、`compute_threat`（:169-182）が **脅威 = 別軸・*寄る* 符号**（rally）。 | **基盤あり・符号正しい**。**GAP-③: STATE で重み付けする routing が無い**。今は accuracy/pressure/threat で選ぶ。STATE（ACTIVE 優先・HIBERNATING 回避）を utility に足す（§5, §6-L1）。 |
| **連続レプリ Path E（engram 共有）** | `m_publish_teach`（`r3_incontext.c:2628`、teach を region topic `"mind/teach"` LATEST_ONLY へ）→ `mind_net_task`（:3094、poll して `r3_fact_learn` で自 queue へ）。 | **SHIPPED・live**。突然死で「直近 teach 以外」を救う **load-bearing floor の半分**（§3）。 |
| **連続レプリ Path W（重み merge）** | `mw_publish_weights`（`r3_incontext.c:3380`、rw[] を chunk 化し `"mind/w"` で告知）／ `mw_fold_region`（:3514、region peer を fold）／ `mind_merge_task`（:3580、周期 fold）。**fold は inline 平文 equal-weight mean（W¹）**（:3523-3525 のコメント "This IS gl_merge's arithmetic"、:3556-3560 で `EV_MERGE_WEIGHTED` は *named-future* placeholder）。 | **SHIPPED・live（W¹）**。floor のもう半分（§3）。**正直に**: live fold は **等重み平均**であって Fisher essence ではない ―― *mean* を保つだけで robust essence は保たない（§3.1・§8-⑤）。chunk は all-or-nothing で、欠落で当該 round の fold が落ちる（`:3489`）。**GAP-④（既知）: live W² 未配線**。下記参照。 |
| **essence（Fisher 重み付き本質）** | `gl_merge_w`（`gossip_learn.c:100`）／ `r3_fisher_diag`（`r3_incontext.c:3900`）。**cert のみ**（`r3_wmerge_test`、:4185 の呼び出しは cert 内）が叩く ―― **live 経路は一切叩かない**。LM-11 認定: 回復バー `bar=75.0f`。 | **存在・cert-only**。**GAP-⑤: live W² は依然 future**（旧 apoptosis doc §1.2 と一致・2026-06-28 再検証）。新ディレクション下では essence flush は **任意最適化**なので、W² の live 化は L3 まで必須でない（W¹ 連続 fold〔= mean〕+ Path E が floor を張る。ただし floor は *mean* どまりで Fisher essence ではないと正直に印字）。 |
| **冬眠 / node-sleep** | Android 側に **ユーザの "sleep / 休む by choice" 状態 ＋ UI 文字列** は在る（UMP アプリの省電力トグル）。が kernel の survival-loop 側には無い。 | **GAP-⑥（最大）: kernel survival-loop の HIBERNATING / 可逆ノード休眠が無い**（"grep=0" とは書かない ―― Android のユーザ選択 sleep は別物）。DMN sleep-consolidation（`dmn.c`）は *データ* consolidation であって *ノード* の可逆休眠ではない。HIBERNATING は **新機構**（mind_merge/mind_net/重 DMN を pause・beacon cadence 低下・routed work shed・ただし ALIVE & wakeable を保つ。§6-L2・§8-⑥）。 |
| **DANGER→活性化（reflex G33）** | `reflex_threat_level`（`reflex.c:356`）／ `reflex_threat_experience(cls)`（:382）→ `act_table`（:53-57）が `SHIELD/CONSERVE/BEACON` を出し、`reflex/alarm/<node>` topic ＋ `world.c:179` の beacon threat へ。 | **SHIPPED**。「死にそうになったら活性化」の軸は **実在**。`S_n` の THREAT 軸入力でもある（interoception §1.1）。 |
| **Self/lin lineage（消えない自伝）** | `lm_self.c` `lm_self_append_unit_event`（:318）、`LM_SELF_ENTRY`（`lm_self.h:52-66`、148B v2、`prev_entry` hash-chain ＋ `model_ver` 重み content-id）。kind `GERM/REAP/ROLLBACK/INTROSPECT`（:95-98）。 | **SHIPPED**。**GAP-⑦（既知）: DEATH/RETIRE event kind が無い**。`LM_UNIT_EV_APOPTOSIS` / `LM_UNIT_EV_RETIRE` を追加（既存 kind 不変・API 互換）。 |
| **min-fleet / 群れサイズ** | `degrade_level`（`degrade.c:111`、ALIVE 数 vs region で SOLO/REDUCED/FULL）／ `region_size`（`region.c:234`）／ `region_contains`（:228）。`dnode_table[n].state==DNODE_ALIVE`。 | **SHIPPED**。退役 / apoptosis の min-fleet guard に再利用（§4・§6）。 |
| **galaxy 可視化** | `galaxy_emit(UB type,UB src,UB dst,UH a,UH b)`（`galaxy.c:101`）→ `ui_event_emit`（token-bucket、chatty のみ throttle、`ui_api.c`）。**現 checkout の event 語彙は `EV_SWIM..EV_MERGE 16` まで**（`galaxy.h:34-52`）―― `EV_INTERO` も `EV_STATE` も **存在しない**。`ui_snapshot` が `s_n/s_axis` を出す（`ui_api.c:180`）が、それは scalar の覗き窓であって **event hook ではない**。 | **SHIPPED**。**正直に**: STATE 遷移を可視化するには **新 event 型 `EV_STATE` を galaxy.h に追加**する必要がある（`EV_MERGE 16` の次、L0 で正直に新規追加）。star を STATE 色相に（§6 各 L の観測・§8-②）。 |
| **署名（poisoning 対策）** | `sign_manifest_verify`（`sign.c:167`、3-gate AND・fail-closed）／ `ed25519.c`。 | **SHIPPED**。essence flush（L3）と retire vote（L4）の正当性に再利用。 |

### 2.1 GAP まとめ（実装波が新規に作るもの）
- **GAP-①** `WORLD_BEACON.state`（1 バイト、L0）＋ **beacon-versioning / dual-size 互換スキーム**（12B/13B 混在を後方互換に保つ、§6-L0・§8-③）。
- **GAP-②** battery/thermal の `S_n` 資源軸ソース（Android JNI、L2／open-Q-2）。host は `intero_test_force` 系で代替。
- **GAP-③** STATE-aware support routing（`select_expert` utility に STATE 項、L1）。
- **GAP-④/⑤** live W² fold（依然 future。新ディレクションでは L3 まで必須でない。W¹ live fold は *mean* どまりで Fisher essence ではない、§8-⑤）。
- **GAP-⑥** **kernel survival-loop の HIBERNATING / 可逆ノード休眠が無い（最大の新規）**（L2）。Android のユーザ選択 "sleep" とは別物（§8-⑥）。
- **GAP-⑦** Self 層 DEATH/RETIRE event kind（L3/L4）。
- **GAP-⑧** 連続レプリ **watermark**（peer が「この相手の essence をどこまで持っているか」を知る、L3）。
- **GAP-⑨** **軸別 test seam `intero_test_force_axis(axis, scalar)`**（hosted/cert-gated）。現 `intero_test_force` は dominant 軸を THREAT に pin する（`interocept.c:176`）ので `[state-axis]` cert が書けない。これが **L0 の load-bearing 追加**（§6-L0・§8-⑦）。
- **GAP-⑩** galaxy `EV_STATE` event 型（現 checkout は `EV_MERGE 16` 止まり、L0・§8-②）。

---

## 3. 連続レプリケーション essence モデル（load-bearing な core）

> **旧 apoptosis doc からの反転（最重要）。** 旧 doc は「死ぬ前に ACK が来るまで死なせない handshake」を
> load-bearing とした。2026-06-28 ディレクション①が前提を覆す ―― **突然死が常態なら、死の瞬間の handshake は
> 当てにできない**（電源喪失は ACK を待てない）。ゆえに load-bearing は **健康な生のあいだの連続レプリ**へ移る。
> handshake / flush は **graceful 時の delta-zero 最適化**に降格する（block しない）。

### 3.1 floor（突然死で失うのは「最後のレプリ以降の delta」に有界 ―― lossless ではない）
健康な生は **既に** 冗長化している（§2 で live 確認済）。ただし冗長化には **正直な穴**がある：
- **Path E**: `mind teach` ごとに engram が `"mind/teach"` region topic へ撒かれ（`m_publish_teach`）、peer が自分の
  queue へ取り込み（`mind_net_task`）DMN で consolidate する ⇒ 教えた事実は ~1 DMN サイクルで region 冗長。
  **だが topic は `KDDS_QOS_LATEST_ONLY`**（`r3_incontext.c:2600`, `kdds.h:20`）＝ slot 上書きで **最新 teach のみ生存**。
  `mind_net_task` は ~500ms poll・origin seq ごと 1 回（`:3092,3165`）⇒ **peer 観測前に連続した複数 teach は中間 fact を失い得る**。
- **Path W**: `mind_merge_task` が周期的に `mw_fold_region`（W¹ inline **equal-weight mean**、Fisher essence ではない）で
  region 重みを fold ⇒ consolidate 済み重みは絶えず region *平均* へ畳まれる。**chunk は all-or-nothing**で、欠落で
  **当該 round の fold が丸ごと落ちる**（`:3489`）。保つのは robust essence ではなく **mean** である（§8-④⑤）。
- **Self/lin**: lineage hash-chain は p-fs（durable・content-addressed）に在り、電源喪失を越える（PERSIST-2）。

⇒ 任意の瞬間で、ノードの「失われ得る本質」は **最後のレプリ以降の delta** に *有界化される*（ゼロではない）。突然死が
   落とすのは (a) teach したが未 gossip/未 consolidate ＝ LATEST_ONLY で上書きされた中間事実、(b) 未 fold の重み更新
   （丸ごと落ちる round）―― **この delta**。**lossless ではなく bounded best-effort**。この有界化が定量的に意味を持つのは
   §3.2 の watermark/cadence が立ってから（§6-L3 / §8-④）。

### 3.2 watermark（GAP-⑧）― delta を小さく・観測可能に保つ
各ノードは beacon に **レプリ watermark**（最後に fold した merge epoch ＋ 最後に publish した teach seq）を載せる。
peer はこれを読み、「この相手の essence をどこまで複製済みか」を知る。watermark の遅れ＝失われ得る delta の上界。
レプリ cadence（DMN tick / merge 周期）が delta-window を **有界**にする。**これが §6-L3 の cert の物差し**。

### 3.3 graceful apoptosis ＝ delta flush（最適化、core ではない）
DYING 分岐は、可能なら最終 delta を **best-effort** で潰す：
1. pending engram を `"mind/teach"` へ強制 publish（Path E flush）。
2. consolidate 済み重みを `mw_publish_weights` で強制告知 ＋ 一度 fold を促す（Path W flush）。
3. Self/lin に `LM_UNIT_EV_APOPTOSIS` を append（heir/model_ver を符号化、GAP-⑦）。
4. 退場 beacon（`state=DYING` ＋ 最終 watermark）を撒く。
5. exit（既存 `dproc_kill_by_name`→`user_proc_teardown` 流: ssy/kdds/page-table 解放、`dproc.c:221`）。

> **block しない**のが旧 doc との決定的差分。ACK を待たない（待てる graceful 時は待っても良いが、待ちは
> **任意**で timeout 必須）。突然死はこの箱を丸ごと飛ばす ―― それでも §3.1 の floor が直近 delta 以外を救う。
> 旧 doc の重い ACK-before-death ordering（`archive/interocept-2-apoptosis-plan.md §2.3`）は **graceful 経路の
> オプション**として残し、core から外す。署名つき essence（`sign_manifest_verify`）は flush でも維持（poisoning 対策）。

---

## 4. 民主的 retirement プロトコル（§0-6 の権利モデル）

> 「誰も所有しない家の権利は民主主義で決める。」 ―― だが「誰も本当には誰も殺せない」。
> retirement は **破壊ではなく essence 保全つき強制休息 ＋ 再参加可**。

### 4.1 流れ（NO-CENTRAL・分散投票）
1. **観測**: あるノード T が world-map で **持続的に病んでいる**（高 `S_n` 持続 / fault 連発 / SUSPECT⇄ALIVE flap）と
   各 peer が **自分のローカル world-table から独立に** 判定（中央集計器を作らない、`world.c` の NO-CENTRAL 不変条件）。
2. **投票**: 各 peer が `"retire/<T>"` topic に署名つき vote を gossip（per-source、`world.c` beacon と同型）。
   **集計も分散**: 各ノードは「自分が見た vote」を数え、**region ALIVE 数の supermajority**（§7 open-Q-3、推奨 ≥2/3）を
   ローカルに跨いだときだけ動く。単一集計点は無い。
3. **essence 吸収（verdict に関係なく）**: 退役を確定する前に、T の最新重みを `mw_fetch_peer` 流で確実に pull
   （§3 の連続レプリで既に殆ど吸収済み。届かなければ既にレプリ済みの分が立つ）。**verdict が否でも essence は吸収**。
4. **退役（破壊ではない）**: T の STATE を soft `RETIRED` に。T（または p-fs durable 重みからの respawn）は **より高い
   SWIM incarnation**（`swim.c:69-82`）で再受肉し **再参加可**。
5. **記録**: Self/lin に `LM_UNIT_EV_RETIRE`（投票数・heir を符号化）。**歴史地層に残る**（tamper-evident、`feedback_ark_no_identity_verification` の history-strata ethos）。

### 4.2 Byzantine bound（正直に印字）
純民主主義では **結託 supermajority が健全な少数を退役させ得る** ―― これは原理的に避けられない。honest bound:
**「本当に殺せはしない」**を以下で担保 ―― (a) essence は verdict に関係なく吸収、(b) T は再参加可、(c) 退役は
Self/lin に記録（消えない歴史）。⇒ 最悪ケースは **記憶保全つき強制休息**であって死ではない。完全な Byzantine
合意（毒 vote の排除）は **本スライス scope 外**（将来: 署名 vote の weight 上限 / outlier 排除）。

**ただし「再参加可」だけでは穴がある（要強化）。** 悪意 supermajority は、T が再受肉するたびに即 re-retire して
**永久に締め出せる** ⇒ rejoin が形骸化し「誰も殺せない」が破れる。これを塞ぐ最小の意味論を retirement に課す：

- **(R1) rate-limit / appeal-cooldown**: あるノードに対する retire verdict は **per-target cooldown 窓**（gossip 値、
  例 = 数 beacon 周期）内に **1 回まで**。cooldown 中の再 verdict は各 peer がローカルに **無効票として落とす**
  （集計は分散・per-source なので中央なしで実施可）。⇒ 「retire→rejoin→即 retire」の高速ループを構造的に禁じる。
- **(R2) minority-survival（rejoin grace）**: 再受肉した T には **grace 期間**（min-dwell 同型）を与え、その間は
  **新たな病の実測がない限り** 退役 vote を受け付けない。⇒ 「鯖いた直後にまた殺す」を防ぐ。grace は
  **健全さの再証明窓**であって免罪符ではない（grace 中に本物の fault 連発があれば通常遷移へ戻る）。
- **(R3) appeal / 異議**: T（または T の essence を抱える heir）は `"retire/<T>"` topic に **署名つき counter-claim** を
  載せられ、peer はそれを **自分の world-table の実測**（高 `S_n` 持続が本当に在るか）と突き合わせて投票を再評価する。
  これは中央審判ではなく、各 peer の独立判定への入力に過ぎない（NO-CENTRAL 不変条件を保つ）。

これらの閾（cooldown 長 / grace 長 / counter-claim の重み）は **discover**（§7 open-Q-3、決め打ち禁止）。
完全な Byzantine 耐性（結託の数学的排除）は依然 scope 外だが、R1–R3 で **「rejoin = 誰も真には殺せない」を
実効化** する（§8-⑧）。cert は §6-L4 `[retire-byzantine-honest]` に R1–R3 の実測（永久締め出しが起きない）を足す。

### 4.3 min-fleet guard（2 ノードでの退役/自死は自殺）
region ALIVE 数が閾未満なら **retirement も self-apoptosis も fail-closed REFUSE**（`degrade.c:111` / `region_size`）。
supermajority が意味を持つ最小サイズ（≥3）と、apoptosis の heir 確保（≥1 healthy heir）を **別々に** 課す。
閾は **discover**（§7 open-Q-3、決め打ち禁止）。

---

## 5. support-routing 規則（正しい符号）＋ §8 hysteresis damping

### 5.1 「support」の精密定義（符号を保つ ―― §0-5）
2 つの異なる意味を混同しない：
- **(a) 補強（reinforce the active thinker）**: 協働推論 / MoE expert 選択を **ACTIVE・低 pressure・有能なノードへ
  *寄せる***（§7 応援＝そのノードに *もっと考えさせる*）。
- **(b) 肩代わり（offload a stressed one）**: STRESSED/HIBERNATING ノードから仕事を ***逃がす*（休ませる）**。

両者は pressure/STATE 軸では **同符号**：**ACTIVE/低 pressure へ寄り、STRESSED/HIBERNATING から逃がす**。
`world.c` は既にこの符号で正しい（`compute_pressure`:135 = load のみ・`compute_threat`:169 = 脅威は *寄る* 別軸）。

> **G20 符号倒錯との関係（必読）**: G20 バグは **脅威を pressure に畳んで** 仕事を脅威ノードから *逃がした*
> （守るべき一点から逃げた）。本環は逆 ―― **支援は有能ノードへ寄る**。STATE フィールドが
> 「これは *働き手* として頼るノードか／*患者* として休ませるノードか」を **routing に明示** させ、符号を固定する。
> 注意の交差: 脅威（守る対象 = protected object への危険、`world.c:160-182`）は依然 **rally で *寄る***（その object を
> *守る*ため）。これは「病んだ働き手に仕事を盛る」とは別物 ―― **守られる対象**であって **働き手**ではない。
> STATE 軸（働き手の調子）と threat 軸（守る対象への危険）を **別バイトで** 運ぶことで両立する。

### 5.2 oscillation（§8 リスク）と二時定数 damping
リスク: 「全員が ACTIVE ノードへ寄る → そのノードが stress → hibernate → 支援が次へ跳ぶ → ping-pong」。
処方は **新規でなく survival-network §8 / reflex-deliberation §6 の deadband + ヒステリシス + EWMA をそのまま適用**：

- **fast band（反射, ~per-inference）**: STRESSED を跨いだ瞬間に即 repel（その回の routing から外す）。
- **slow band（熟慮, beacon EWMA over many ticks）**: **STATE 遷移そのものに二時定数ヒステリシス**：
  - **enter 閾 > exit 閾**（ヒステリシスギャップ）。`S_n` は既に EWMA（速い揺れを吸う、interoception §3.4）。
  - 各 STATE に **最小 dwell time**（gossip 値）。HIBERNATING enter = 資源軸持続高、exit = 資源回復持続 + 明示 wake。
- **routing deadband**: `moe.c` の `deadband_pick`（既存、regions/closed-loop で実証）を STATE-aware routing に適用 ―
  challenger が margin 超で勝たない限り前の target を保持 ⇒ 支援が一斉に stampede しない。
- **band は discover**（interoception §2.4）: fast/slow の閾・dwell・margin は **実測曲線から**、ハードコード禁止。
- cert: **N 分連続で発振しないこと**を実測（`[moe-osc]` と同じ certify 形、§6-L1）。

---

## 6. 最小スライス先行の R-plan（L0→L4）

各スライス独立認定・反証可能な load-bearing cert つき。**impl≠audit≠commander**（`feedback_validator_and_learner_traps`）。
依存順で並べ、**最小の正直な第一歩を L0 とする**。

### L0 — STATE バス（**最小の第一歩**・observable STATE gossip only）
> codex の sequencing 評価: 「**L0 は『observable STATE gossip only』に絞れば依然正しい第一スライスだが、
> crown-safe でも wire-trivial でもない**」。以下はその正直版（no-rebaseline 主張を削除済み）。
- **やること**: `WORLD_BEACON.state`（GAP-①, 1 バイト）追加 ＋ STATE マシン（`intero_scalar` + `intero_dominant_axis`
  をヒステリシス付きで {ACTIVE, STRESSED} に写す。HIBERNATING/DYING は L2/L3 へ defer）。`world_peer_state(node)`
  accessor。galaxy に **新 event 型 `EV_STATE` を追加**（GAP-⑩。現 checkout は `EV_MERGE 16` 止まり ―― `EV_INTERO`/`EV_STATE`
  は **存在しないので正直に新規定義**する。`ui_snapshot` の `s_n/s_axis` 露出は event hook ではない、§8-②）。
  **routing も死も変えない・pure observability + gossip**。さらに L0 は次の 2 つの load-bearing 追加を含む：
- **(A) beacon-versioning / dual-size 互換（GAP-①, §8-③）― 具体スキーム**:
  現 beacon は packed 12B ＋ hard `_Static_assert(sizeof==12)`（`world.h:61-63`）、受信は `r >= (W)sizeof(WORLD_BEACON)`
  ガード（`world.c:403`）。単純に 13B へ拡張すると 12B peer と **後方非互換**。ゆえに：
  1. **version byte を明示**: 13B 化の先頭 or 末尾に `U1 ver`（= 1。旧 = 暗黙 0）を加え、`WORLD_BEACON_V1` とする。
     `state` は ver≥1 でのみ有効。`_Static_assert(sizeof(WORLD_BEACON_V1)==13)` を **別に**置く（12B assert は残す）。
  2. **dual-size accept**: `world_task` の取り込みを `r >= 12` に緩め、`r == 12` なら旧フォーマット（state 不明 = `ACTIVE`
     既定で観測）、`r >= 13` なら ver/state を読む。**両サイズを受理**する（13B-only にしない）。
  3. **送出は混在対応**: 自ノードは常に 13B(V1) を撒くが、旧 12B beacon を落とさない。これで新旧ノードが同一 cluster で
     共存できる（旧は state を見えないだけ・新は旧を ACTIVE とみなす）。`_Static_assert` は **両サイズ**を守る。
  - cert `[beacon-compat]`: 12B beacon と 13B beacon を同一 world_task に流し、**どちらも取り込まれる**こと／13B の
    `state` が読め、12B が `ACTIVE` 既定で観測されることを実測。**falsifier** `[beacon-compat-NOT]`: もし `r >= sizeof(V1)`
    のままなら 12B peer が **取り込まれず消える** ―― これが起きないことを示す。
- **(B) 軸別 test seam（GAP-⑨, §8-⑦）― L0 cert を書けるようにする load-bearing 追加**:
  現 `intero_test_force(on,value)` は `force_on` 時に `dom_axis` を **常に `INTERO_AX_THREAT`** に pin する
  （`interocept.c:176-178`）。よって STATE マシンが読む `intero_dominant_axis()` を surprise/fault/degrade に **向けられず**、
  軸依存遷移を認定する `[state-axis]` cert が **物理的に書けない**。新シーム：
  - **`void intero_test_force_axis(UB axis, UB scalar)`**（`interocept.h` に proto 追加、`interocept.c` に実装）。
    force 時に `dom_axis = axis`（`INTERO_AX_*` のいずれか）＋ `scalar_ewma = scalar` を pin する。
    既存 `intero_test_force(on,value)` は **そのまま温存**（後方互換 = THREAT-pin のショートカット）。
  - **hosted/cert-gated**: live 経路では呼ばれない（`HOSTED`/cert ビルドのみ・production 既定で no-op か未リンク）。
    `force_on` のクリアは既存 `intero_test_force(0,0)`／`intero_test_reset` で行う（新たな状態を増やさない）。
  - 既に `interocept.c:271-274` の `intero_self_test` が **component レベルで** 軸別注入を実証済み ―― 不足は
    *公開シームが dominant 軸を選べないこと* だけ。本フックはその一点を埋める（最小・1 関数）。
- **load-bearing cert** `[state-axis]`（**(B) のシームで初めて書ける**）: **軸依存（単調でない）** を機械的に示す ―
  `intero_test_force_axis(INTERO_AX_THREAT, hi)` → ACTIVATE（ACTIVE 維持/rally、STRESSED に**ならない**）／
  `intero_test_force_axis(INTERO_AX_SURPRISE, hi)` 持続 → STRESSED ／ `INTERO_AX_FAULT` 持続 → STRESSED ／
  `INTERO_AX_DEGRADE` 持続 → （L0 では HIBERNATING を defer するので）STRESSED-stub だが **THREAT とは別行き先**であることを示す。
  **falsifier** `[state-monotone-NOT]`: もし「高 `S_n` = 一律 STRESSED」なら **danger(THREAT) 軸でも STRESSED になる** ―― これが
  **起きない**（各軸を順に注入して行き先が分岐する）ことを示して「軸で行き先が違う」を確定。peer が `world_peer_state` で
  読めることも cert（[state-gossip]）。
- **接地**: 既存 SHIPPED の `S_n`（interocept.c）＋ `world.c`。新規は beacon 1 バイト＋version／小さな state module／
  `intero_test_force_axis` 1 関数／`EV_STATE` 1 定義。
- **crown（要訂正・§8-①）**: `world.c` は **bare-metal ビルドに在る**（`boot/x86/Makefile:114`, `boot/aarch64/Makefile:130`）。
  `WORLD_BEACON.state` の追加・計算・publish・read・accessor ＋ `EV_STATE` emit は **共有 `.text` を動かす**。`r_forward`
  数学 hash が不変でも **crown 全体の `.text` baseline は動く ⇒ crown re-baseline は必須**。旧「不要見込み」は誤りなので削除した。
  正しい運用: **相対 CI gate（diff ベース）は意図した変化を見て PASS**するので、その上で **canonical dev crown を
  re-baseline（更新）**する。「no-rebaseline」とは主張しない。commander が re-baseline の diff を行ごとに確認して裁定。

### L1 — STATE-aware support routing ＋ §8 hysteresis
- **やること**: `select_expert`（`moe.c`）utility に STATE 項（ACTIVE/低 pressure を *寄せ*、STRESSED を *逃がす*、GAP-③）
  ＋ `deadband_pick` を STATE-aware に適用。
- **cert** `[support-sign]`: 支援が **ACTIVE ノードへ寄る**（正しい符号）。**falsifier** `[support-sign-NOT]`: STRESSED に
  *した* ノードが routed work を **増やさない**（増えたら G20 符号倒錯の再来）。`[support-osc]`: STATE+routing を
  振っても **N 分発振しない**（`[moe-osc]` 形、deadband 無し版より厳密に切替少）。
- **crown**: ~~`moe.c` は bare-metal。`select_expert` は整数 utility（forward math でない）が、TU が触られるので
  **moe 系 cert の re-baseline ＋ crown 不変確認を commander 裁定**。~~
  → **この re-baseline 案は §10 が上書きする（§9 が §6-L0 の re-baseline 案を上書きしたのと同じ）。L1 は
  CROWN-PRESERVING で実装した：fold もヒステリシスも全て `_TK_HOSTED_LIBC_` ゲートなので bare-metal `moe.c`/
  `world.c` の `.text` は byte-identical（crown 755a20fa / 4064d8a9 不変、再baseline 不要）。**

### L2 — HIBERNATING 状態（**最大の新規・GAP-⑥**）
- **やること**: 資源軸（`INTERO_AX_DEGRADE`、将来 battery/thermal）持続高で HIBERNATING へ。`mind_merge_task` /
  `mind_net_task` / 重い DMN consolidation を **pause**、beacon cadence 低下、routed work を shed、**ただし
  SWIM heartbeat は出し続け ALIVE & wakeable を保つ**。資源回復 + 明示 wake で ACTIVE へ復帰。冬眠中も §3 の
  連続レプリ floor（直近 essence の冗長）は維持 or 凍結明示。
- **cert** `[hibernate-reversible]`: 資源 pressure 注入 → HIBERNATING ／ peer が routing を止める ／ 回復で wake し
  resume ／ 冬眠中も essence が peer に在る。**falsifier** `[hibernate-not-death]`: HIBERNATING ノードは **DEAD でない**
  （SWIM ALIVE のまま、wake で復活）。「冬眠 ≠ apoptosis」（§0-4）を機械的に固定。
- **GAP-②**: host は資源軸を `intero_test_force` で注入。実 battery/thermal は open-Q-2（Android JNI 新ソース）。
- **crown**: `r3_incontext.c`（mind_merge pause）は bare-metal・live merge 経路。**re-baseline ＋ sign-off 必須**。

### L3 — 連続レプリ watermark ＋ graceful flush（DYING、apoptosis の **反転版**）
- **やること**: beacon に watermark（GAP-⑧、merge epoch + teach seq）。DYING = best-effort flush（§3.3、**block しない**）。
  Self/lin `LM_UNIT_EV_APOPTOSIS`（GAP-⑦）。署名 essence（`sign_manifest_verify`）。min-fleet guard（§4.3）。
- **cert** `[essence-floor]`（**旧 apoptosis cert の物差しを反転**）: ノードを **突然 kill**（graceful 経路なし）しても、
  essence が peer に **watermark まで** 在る（直近 delta 以外は survive）。**falsifier** `[flush-shrinks-delta]`: graceful
  flush を通すと失われる delta が **0 に縮む**（flush 有り vs 無しで失う事実数を比較）。これにより「連続レプリが core・
  flush は最適化」を確定 ―― 旧 doc の `[apop-before-death]`（handoff が load-bearing）を **置換**する。
  補助: `[apop-refuse]`（署名不正 essence reject、重み byte 不変）／ `[essence-minfleet]`（孤立死 REFUSE）／
  `[apop-ledger]`（Self 層に DEATH + heir）。**W² の live 化は任意**（W¹ 連続 fold + Path E で floor が立つなら defer 可。
  divergent fact で floor が破れる実測が出たら L3 内で W² を live 化、`gl_merge_w`+post-fold union-replay、旧 doc §2.2）。
- **crown**: essence flush が `gl_merge_w`/`r3_fisher_diag` を live 化する場合は旧 doc §5 の分析適用
  （`-ffp-contract=off` TU・新 float 順序なし・crown forward 不変）。**re-baseline ＋ sign-off**。

### L4 — 民主的 retirement（§4・**最も哲学的に重い・最後**）
- **やること**: `"retire/<T>"` 署名 vote topic、分散 supermajority 集計、essence-absorbed-regardless、rejoin（incarnation++）、
  Self/lin `LM_UNIT_EV_RETIRE`、min-fleet guard、Byzantine bound 印字。
- **cert** `[retire-democratic]`: supermajority 到達 → T が RETIRED・essence 吸収済・再参加可。**falsifier**
  `[retire-not-destroy]`: 退役後も T の essence が群れに在り、T が **再参加できる**（incarnation++ で復活）＝「殺せていない」。
  `[retire-minfleet]`: 小群れでは退役 REFUSE。`[retire-byzantine-honest]`: 結託多数が少数を退役させ得るが essence 保全 +
  rejoin で「破壊でない」ことを印字（honest negative、毒 vote の数学的排除は scope 外と明記）。
  `[retire-no-permalock]`（§4.2 R1–R3 の実測）: 悪意 supermajority が T を retire→T が rejoin→**cooldown 窓内の即 re-retire は
  落ちる**（R1）／rejoin grace 中は新たな実測病なしに退役できない（R2）／counter-claim が world-table 実測と突き合わされる（R3）
  ⇒ **永久締め出しが起きない**ことを実測。**falsifier** `[permalock-NOT]`: R1–R3 を外すと T が永久に締め出される（= 旧設計の穴）
  ことを示し、R1–R3 が load-bearing であることを確定。
- **依存**: L0（STATE）＋ L3（essence-preserved）必須。**最後に置く**（最も魂に近く、符号と Byzantine の両方を要する）。
- **crown**: vote/集計は arch-common ⇒ bare-metal にも入る。topic 追加は additive。re-baseline 確認。

### 6.1 sequencing 要約
**L0（最小・pure 追加）→ L1（符号 + 発振）→ L2（冬眠＝最大新規）→ L3（連続レプリ floor + flush）→ L4（民主退役）。**
L0 が **背骨**（STATE を gossip する）― 他は全部これに掛かる。L2 と L3 は独立に進め得る（依存は両方 L0）。

---

## 7. mk_pino への open questions（headline = 病の閾）

1. **【headline・§0-7】病/退役の閾**: 持続的な `S_n` のどの曲線が「退役するほど病んでいる」か。
   **実測で discover** すべき（interoception §2.4 規律）。提案: 本当に劣化していくノードの `S_n` 軌跡 vs 一時的に
   忙しいだけのノードの軌跡を測り、その **間** に閾を置く ―― だが **値は実機データ + あなたの sign-off** が要る。
   決め打ちしない。**この一問が L3/L4 を gate する。**
2. **HIBERNATING の資源軸ソース（GAP-②）**: Android で **本物の battery/thermal** を読む（JNI 新ソース・新 `S_n` 軸）か、
   `degrade` で代理し続けるか。「電池減→冬眠して生き残る」（§0-3）の honest 信号は本物の battery ―― だが
   Android 固有ソースが増える。どちらを採るか。
3. **退役の supermajority 率 + min-fleet 床**: region の 2/3 か 3/4 か。退役を一切許さない最小 region サイズ
   （自殺 guard）は幾つか。そして ―― 対象 T に **拒否権/一票**を与えるか、それとも **essence 保全つき強制休息**を
   最悪ケースとして許容するか（あなたの §0-6 裁定は「許容（破壊でない限り）」と読めるが、**正確な gate** を確認したい）。

> 加えて旧 apoptosis doc §B の哲学 5 問（consent は誰の手か / 単一 heir か broadcast か / euthanize 可否 /
> provenance を名乗るか / 黙って消える権利）は **L3/L4 の実装波で再上申**する ―― 本環では「連続レプリが core・
> 死は最適化」へ前提が移ったので、特に「単一 heir か broadcast か」は **region broadcast（連続レプリは元々全員へ撒く）**
> がデフォルトとして自然に解け、「euthanize 可否」は L4 の民主退役（essence 保全つき）として **裁定済みの方向**に入る。

---

## 8. クロスモデル設計レビュー（codex / GPT-5.5、2026-06-28）— 実装前に折り込む必須修正

別モデル(GPT-5.5)による独立 file:line 検証。Claude(司令官)の crown 懸念を裏付けた上で、さらに 7 点の実問題を検出。
**この 8 点は 2026-06-28 の design-revise で本文へ折り込み済み（各項末 → ADDRESSED ＝ 反映先）。** 以下は原文＋反映先の記録：

1. **L0 の crown「不変」主張は誤り（要訂正）。** `world.c` は bare-metal ビルドに在る（`boot/x86/Makefile:114`,
   `boot/aarch64/Makefile:130`）。`WORLD_BEACON.state` の追加・計算・publish・read・accessor は共有 `.text` を動かす。
   `r_forward` 数学 hash が不変でも **crown 全体 `.text` baseline は動く → re-baseline 必須**。§6-L0 の「不要見込み」を削除。
   **→ ADDRESSED**: §6-L0 の crown 節を全面訂正（re-baseline 必須・相対 CI gate は意図変化を見て PASS・canonical dev crown を更新）。
2. **`EV_INTERO`/`EV_STATE` は現 checkout に存在しない。** `galaxy.h:33` は `EV_MERGE 16` までしか定義しない。
   `ui_snapshot` は `s_n/s_axis` を出す（`ui_api.c:180`）が event hook ではない。L0 は event 語彙を**正直に新規追加**する。
   **→ ADDRESSED**: §2 galaxy 行を「`EV_MERGE 16` 止まり・`EV_INTERO`/`EV_STATE` 不在・ui_snapshot は event hook でない」に訂正、§6-L0 で `EV_STATE` 新規追加を明記、GAP-⑩ を新設。
3. **beacon wire 拡張を過小評価。** 現 beacon は packed 12B ＋ hard `_Static_assert`（`world.h:51`）。`world_task` は
   `r >= sizeof(WORLD_BEACON)` でしか取り込まない（`world.c:401`）→ 13B 化は **version/dual-size 互換計画なしでは
   12B peer と後方非互換**。L0 に beacon versioning を足す。
   **→ ADDRESSED**: §6-L0(A) に具体スキーム（`U1 ver`＋`WORLD_BEACON_V1`／受信を `r >= 12` に緩め両サイズ受理・12B は ACTIVE 既定／12B と 13B 双方の `_Static_assert`）＋ `[beacon-compat]` cert を新設。§2 GAP-① row・§2.1 GAP-① も更新。
4. **「突然死で何も失わない」は未成立（core テーゼの誠実性修正）。** Path E は `KDDS_QOS_LATEST_ONLY`
   （`r3_incontext.c:2600`, `kdds.h:20`）= slot 上書きで最新 teach のみ生存。`mind_net_task` は 500ms poll・origin seq
   ごと 1 回（`r3_incontext.c:3092,3165`）→ peer 観測前の複数 teach は中間 fact を失い得る。Path W は all-or-nothing
   chunk で、欠落で当該 round の fold が落ちる（`:3489`）。正直な主張 = **「L3 watermark/cadence 後の bounded
   best-effort delta」**であって lossless ではない。§3 の言い回しを弱める。
   **→ ADDRESSED**: §0-1・§1(5)・§3.1（floor 見出し含む）を「lossless ではなく bounded best-effort delta、有界化は L3 watermark/cadence 後」に緩和、Path E LATEST_ONLY と Path W all-or-nothing の穴を明記。
5. **W¹ を「essence floor」と過大評価。** live fold は equal-weight mean（`mw_fold_region` inline, `:3523`）、
   Fisher essence ではない（`EV_MERGE_WEIGHTED` は future placeholder `:3554`、`gl_merge_w` は cert-only `:4185`）。
   live W が essence を頑健に保つかのような含意を避ける。
   **→ ADDRESSED**: §2 Path W 行・essence 行・§3.1 を「live fold は equal-weight mean であって Fisher essence ではない／gl_merge_w は cert-only・live 経路は叩かない／floor は mean どまり」に修正。
6. **GAP-⑥ の grep 主張が広すぎ。** kernel/node hibernation は不在だが Android に明示的 "sleep/asleep by choice"
   状態＋UI 文字列が在る。GAP-⑥ は「**kernel survival-loop の HIBERNATING / 可逆ノード休眠が無い**」と書く（grep=0 でなく）。
   **→ ADDRESSED**: §2 GAP-⑥ row（Android のユーザ選択 "sleep" を別物として明記）・§2.1 GAP-⑥ を reword（"grep=0" を撤回）。
7. **軸 cert の test seam が無い（L0 cert ブロッカー）。** `intero_dominant_axis()` は在る（`interocept.c:230`）が
   `intero_test_force` は scalar を pin し **dominant 軸を常に THREAT** にする（`:176`）→ surprise/fault/degrade の
   軸依存遷移を現 seam では認定不能。**軸別 test hook を新設**しないと `[state-axis]` cert が書けない。
   **→ ADDRESSED**: §6-L0(B) に `intero_test_force_axis(UB axis, UB scalar)`（hosted/cert-gated・`dom_axis`＋`scalar_ewma` を pin・既存 `intero_test_force` は温存）を新設、GAP-⑨ を立て、`[state-axis]` cert を各軸注入で書き直し。これが L0 cert を unblock する load-bearing 追加。
8. **退役「破壊でない」未成立。** SWIM incarnation refute は在る（`swim.c:69`）が soft `RETIRED` overlay・vote topic・
   rejoin policy・routing/SWIM liveness との整合は未存在。**悪意 supermajority は再参加ノードを再退役し続けられる**
   → 「rejoin = 誰も殺せない」には rate-limit / appeal-cooldown / minority-survival 意味論が要る（§4.2 を強化）。
   **→ ADDRESSED**: §4.2 に R1（per-target cooldown / rate-limit）・R2（rejoin grace = minority-survival）・R3（署名 counter-claim / appeal）を追加、§0-6 を強化、§6-L4 に `[retire-no-permalock]` ＋ falsifier `[permalock-NOT]` を新設（永久締め出しが起きないことを実測）。

> sequencing 評価（codex）: 「**L0 は『observable STATE gossip only』に絞れば依然正しい第一スライスだが、
> crown-safe でも wire-trivial でもない**」。実装前 doc 編集要件（**全て本 revise で DONE**）=
> (1) no-rebaseline 主張削除 → §6-L0 crown 節を訂正済 ✅／(2) beacon version/dual-size 互換 → §6-L0(A)＋`[beacon-compat]` ✅／
> (3) `EV_INTERO`/`EV_STATE` 訂正 → §2 galaxy 行＋GAP-⑩ ✅／(4) lossless 文言の緩和 → §0-1・§1・§3.1 ✅／
> (5) L0 用の軸注入 test seam 追加 → §6-L0(B) `intero_test_force_axis`＋GAP-⑨ ✅。
>
> **残る honest-open（design では閉じない・実装/実機/mk_pino sign-off に依存、relitigate ではなく繰り越し）**:
> ・**完全 Byzantine 耐性**（毒 vote の数学的排除）は L4 scope 外のまま ―― R1–R3 は「永久締め出しを防ぐ」までで、
>   結託多数そのものは排除しない（§4.2 明記）。
> ・**病/退役の閾**（§7 headline）は実機 `S_n` 曲線 + mk_pino sign-off に依存（決め打ち禁止）。
> ・**本物の battery/thermal 資源軸**（GAP-②）は Android JNI 新ソースが要る open-Q-2（host は test seam で代替）。
> ・**live W²**（GAP-④/⑤）は依然 future。L3 まで必須でないが、divergent fact で mean-floor が破れる実測が出たら L3 内で着手。

---

## 9. 司令官判断 2026-06-28 — L0 は CROWN-PRESERVING で実装する（§6-L0 の再baseline案を上書き）

Workflow finalize は L0 を「beacon 13バイト化＋crown 再baseline」で設計したが、**より安全で本プロジェクトの確立パターンに沿う crown 保全アプローチが存在する**ので、L0 実装はこちらを採る:

1. **`state` は新フィールドでなく `firing` バイトの空きビットに載せる（WIRE 不変）。**
   `WORLD_FIRE_MASK=0x07`（bit0-2）＋ `WORLD_REBUILD_BIT=0x80`（bit7）で、**bit3-6 (0x78) は空き**
   （world.h:69,77）。`state`（4値=2bit）を bit3-4 (`WORLD_STATE_MASK 0x18`, `WORLD_STATE_SHIFT 3`)
   に載せる → **WORLD_BEACON は 12バイトのまま、`_Static_assert(sizeof==12)` 不変、versioning/
   dual-size 互換問題は丸ごと消滅**（旧ノードは bit3-4=0=ACTIVE 既定で自動後方互換）。これは
   selfc-ring3 の REBUILD_BIT(bit7) / N-2b SWIM capability gossip(_pad→bit) と**同一の既存手法**。
   → Workflow の §6-L0(A) beacon-versioning（U1 ver / r>=12 緩和 / [beacon-compat] cert）は **不要**。
2. **FSM・accessor・seam・EV_STATE・cert は全部 hosted-gate（`_TK_HOSTED_LIBC_`）、bare-metal は
   byte-identical #else。** 生存ループはフリート(boot/linux + Android)の挙動であり、QEMU bare-metal
   (boot/x86, boot/aarch64) は走らせない(=常に state bits 0 = ACTIVE 既定でよい)。よって
   `world_self_state_step`(FSM) / `world_peer_state`(accessor) / state スタンプ / `intero_test_force_axis`
   seam ＋ interocept.c:178 の `dom_axis` 変更は **すべて hosted-gate**。bare-metal world.c /
   interocept.c の `.text` は byte-identical → **crown 755a20fa / 4064d8a9 が保たれ、再baseline 不要**。
   これは R3_WP / arkfs / student-blob / ss6_live と同じ crown 保全規律。
3. **検証**: 実装後に bare-metal 両アーキ crown を再導出し **755a20fa / 4064d8a9 と byte 一致**を
   確認（hosted-gate が完全なら必ず一致。一致しなければ gate 漏れ → STOP）。`nm` で bare-metal に
   新シンボル(world_self_state*/world_peer_state/intero_test_force_axis)が **入っていない**ことも確認。
4. **cert は hosted (boot/linux) で走る**: `[state-axis]`（各軸注入→THREAT=ACTIVE / 他=STRESSED、
   軸依存を機械証明）＋ falsifier `[state-monotone-NOT]`（「高S_n=一律STRESSED」なら THREAT 軸でも
   STRESSED→起きないことを示す）＋ `[state-gossip]`（peer の state を world_peer_state で読める）。
   beacon-compat は wire 不変なので不要（旧12B=新12B、同一）。

これにより L0 は **crown を一切動かさない pure-additive hosted スライス**になり、Workflow が出した
GO-L0 判定はそのまま有効、ただし実装経路は §9 が §6-L0 の crown 段を上書きする。

---

## 10. 司令官判断 2026-06-28 — L1 は CROWN-PRESERVING で実装する（§6-L1 の crown 段を上書き）＋ 実装記録

§6-L1 は「moe 系 cert の re-baseline ＋ crown 不変確認」を残していたが、§9 と同じ crown 保全規律を L1 にも適用する。
L1 は **crown を一切動かさない hosted-only スライス**として SHIPPED した（cert: `tests/host/run_survival_l1.sh` / verb
`survival l1` / CI: ump-x86_64 に L0 の直後）。

1. **support-routing の fold は eff_pressure（LOAD/avoid 軸）へ載せる ＝ 構造的 G20 ガード。**
   `moe.c` に hosted file-static `eff_state_penalty(WSTATE)`（ACTIVE→0 / STRESSED→+P_s / HIBERNATING・DYING は L0 で
   emit されないので relief 主張なし=0 / 未知 peer(-1)→ACTIVE）。`select_expert` の候補充填シーム（`is_self` 既知の
   一点）で `cand[].eff_pressure += eff_state_penalty(is_self ? world_self_state() : world_peer_state(n))` を
   **1 行**足す。これは `expert_utility` が **引く** LOAD 軸なので、STRESSED 自分は自分の仕事を手放し、近傍は
   STRESSED ノードを避ける（仕事が **健全な働き手へ寄る**）。**脅威/rally 項（`expert_utility` の +threat）には
   絶対に載せない** ―― 載せると G20 符号倒錯（病んだ働き手に仕事を盛る）。falsifier `-DSURVIVAL_L1_SIGN_FLIP` は
   まさに penalty を threat 項へ流して `[support-route]` を RED にする（cert で実証）。
   sign + no-pile-on だけが load-bearing；P_s（実装値 70）は **discover**・cert が straddle する（決め打ち定数にしない）。
   EDGE: シームは `me >= DNODE_MAX` の reflex local-only early-return の **後** にあるので、node id 未確定の STRESSED
   自分は forced-local を自壊させない（コード配置で自動的に成立）。

2. **§8 ヒステリシスは MEASUREMENT-GATED（wave-45 規律「fix が disease だった」）。**
   結合 S_n forcing（ACTIVE/仕事保持中は stress↑、STRESSED/手放し中は↓）で **production `world_self_state_step`
   の遷移核 `wstate_advance` を同一 build で naive（対称 1-dwell ＝ L0）と damped（二時定数）両アームに掛けて
   flips を実測**。**実測 flips_naive = 33 > K=12** ⇒ disease は real ⇒ **damping を SHIP**（flips_naive ≤ K なら
   dead code になるので入れない、という分岐は取らなかった）。cure = `WSTATE_MIN_DWELL` を **ENTER_DWELL(fast)
   / RELAX_DWELL(slow=×8) に分割 ＋ relax refractory(self_cooldown=20)**、`threat_acute || s<=S_EXIT` の lump を
   分割して **急性 DANGER は INSTANT relax（rally）/ scalar relax は slow**。**実測 flips_damped = 8 ≤ K=12 かつ
   2×8=16 ≤ 33**（`[moe-osc]` と同じ受理形）。falsifier `-DSURVIVAL_L1_NO_DAMP` は二時定数を 1 本へ畳んで
   flips_damped==flips_naive にし `[hysteresis]` を RED（実証）。

3. **全部 `_TK_HOSTED_LIBC_` ゲート ⇒ crown byte-identical。** fold シーム（`#else` なし）／`eff_state_penalty`／
   `moe_state_fold`／`moe_support_route_test`／`wstate_advance`／`world_l1_flap_test`／`self_cooldown` は
   すべて hosted のみ。bare-metal `moe.c`/`world.c` の `.text` は触れず、**crown 755a20fa / 4064d8a9 は byte 一致を
   再導出で確認**（cert 内蔵の crown gate A）。`nm` で bare-metal に L0/L1 hosted シンボルが **不在**（gate B）。
   gate C（bare `moe test` 不変）は gate A（byte 同一）が含意。ungated 参照は bare-metal で **link 不能**（world.h の
   hosted-only 宣言が構造ガード）＝ gate 漏れを hash 前に捕まえる。これは R3_WP / arkfs / ss6_live と同じ規律。

4. **正直な限界（L2 へ繰り越し）**: 本 cert は **単一プロセス合成**であり、**実フリートの flap は証明しない**
   （RTT/ gossip 遅延/ 実 battery 由来の本物の S_n 軌跡は host test seam の代理）。実フリート VERDICT は L2 へ defer。
   P_s / ENTER・RELAX dwell / refractory は実機 `S_n` 曲線 ＋ mk_pino sign-off で **discover** する（§7 headline・決め打ち禁止）。
