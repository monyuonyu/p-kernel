# Federation — 32 の壁から「数千ノード」へ橋を架ける

> 看板は **"thousands of nodes / 10,000 spacecraft plates / more nodes = smarter"**。
> 現実は **`DNODE_MAX = 64`**（`arch/common/include/drpc.h:35`）が、world / moe /
> reflex / dkva / swim / region / dnode_table の **すべての配列幅・走査上限・
> ビットマスク幅** を縛る絶対上限である。region は 64 を**分割**するだけで
> 超えない。
>
> ★更新（federation R0, 2026-06-20）: 「32 を超える上位フェデレーション機構が
> 存在しない」は **もう正しくない**。2-cluster の階層集約は **DKVA path で
> すでに LIVE** である — region 内は per-node の `resp/<n>`（REGION スコープ）で
> 密に、region 間は coordinator が出す `rsum/<rid>`（GLOBAL スコープ）の
> **要約だけ** が境界を越える（`dkva.c:430-520, 814-818`）。これは
> `samples/11_distributed/run_4node_regions.sh` が **実プロセスで** 走らせ、
> federation R0 wave が **falsifiable cert** で固めた:
>   - `[fed-2cluster][in-proc]`（`dkva_fed2_self_test`）— cross-region 期待は
>     **O(#region)**（coordinator 1 つ）であって O(N) でない。
>   - `[fed-2cluster][live]` — 上記ハーネスが `[locality]` カウンタを diff し、
>     **flat-control（penalty=0, 1 region）falsifier** で階層 vs 退化（all-to-all）
>     を機械的に区別する（zoned far=summary-only / flat near=全 N 直接 fan-in）。
>   - `[coord-crash][in-proc]` — coordinator が死ぬと代表が決定論的に min-id 次点へ
>     **再委譲**（投票なし）。
> このドキュメントは、その**先**（256 の壁を破る composite ID = F1、共有トピック =
> F2、region-of-regions = F3）を**設計**する。R0 は DNODE_MAX/wire を一切変えない。

Status: **R0 (2-cluster DKVA 階層) は LIVE + certified（`[fed-2cluster]`）;
F1–F3 は設計のみ** / 監査 第3版 G23 🔴 への応答 / 最終更新: 2026-06-20

関連: [regions.md](regions.md)（葉クラスタ＝局所 region の設計）、
[survival-network.md](../00-concept/survival-network.md)（§4 「region＝局所葉」の思想的根拠）、
[dynamic-id.md](../30-module/dynamic-id.md)（第11波 G6 relay lease。ID 空間は 8-bit のまま）、
[decentralized-lookup.md](decentralized-lookup.md)（HRW + gossip）、
[philosophy-gap-audit-3.md](../archive/philosophy-gap-audit-3.md) G23 / G7 / G13 / G14 / G6。

> **正直さの前置き**: 本書に登場する計算量（O(N²)→O(N log N) 等）、
> しきい値、段階目標は**設計目標**であり、実証されていない。本書は1行も
> コードを変更しない。実装は R3 / Phase D の本丸。本書は「どの順で、何を、
> なぜ」を確定させるための地図である。

---

## 1. 現状の壁の棚卸し（file:line で裏取り）

第3版 G23 の指摘を実コードで再確認した。以下は**壁の分類**（配列幅・ID 幅等）で、
下の file:line は当時の棚卸し。**現状 `DNODE_MAX = 64`**（G23 で 32→64 へ引き上げ済み、
`drpc.h:35`）で、下記の各配列は `DNODE_MAX` から自動で導出されるため一斉に追随している。

### 1.0 定義そのもの

```
arch/common/include/drpc.h:35   #define DNODE_MAX 64   /* max nodes (G23: 32→64) */
arch/common/include/drpc.h:23   bits 31..24 = node_id (0-255; capped at DNODE_MAX)
```

`DNODE_MAX` は 8→32→64 へ regions のために引き上げられた経緯がある
（G23 closed wave-19；旧コメント `Raised 8 -> 32` は 64 まで進んだ）。だが **8-bit の `node_id`
フィールド**（`UB src_node/dst_node`、`obj_id` の上位 8bit）が**論理上の
ハード天井 256** を作る（`drpc.h:36-38`）。`GOBJ_MAKE/NODE/LOCAL`
(`drpc.h:78-81`) が node を 8bit・local を 24bit に固定。**この 8-bit が
G7（ID 空間）の本体**であり、「数千」(>256) は配列幅以前に**ID 幅で不可能**。

### 1.1 壁A — 固定長配列の幅（メモリと走査上限）

| file:line | 配列 | 役割 |
|---|---|---|
| `arch/common/drpc.c:76` | `DNODE dnode_table[DNODE_MAX]` | ノードテーブル本体（全モジュールの母集団） |
| `arch/common/drpc.c:247` | `caller_strs[DNODE_MAX][4]` | RPC タスク名 |
| `arch/common/swim.c:137` | `suspect_count[DNODE_MAX]` | SWIM suspicion カウンタ |
| `arch/common/swim.c:147-148` | `rtt_ewma_ms[DNODE_MAX]` / `rtt_valid[DNODE_MAX]` | region 形成の RTT |
| `arch/common/world.c:90` | `WORLD_ENTRY table[DNODE_MAX]` | 世界マップ（per-node 状態） |
| `arch/common/world.c:100` | `h_sub[DNODE_MAX]` | per-node beacon サブスクライブハンドル |
| `arch/common/moe.c:78-79` | `peer_scores[DNODE_MAX]` / `score_valid[DNODE_MAX]` | MoE スコア |
| `arch/common/moe.c:90` | `h_score_sub[DNODE_MAX]` | per-node スコアハンドル |
| `arch/common/moe.c:132` | `recent_pick[DNODE_MAX]` | 反復選択回避 |
| `arch/common/moe.c:139-140` | `util_ewma[DNODE_MAX][MOE_NUM_CLASSES]` 他 | per-node×class 効用 |
| `arch/common/edf.c:40,43` | `peer_load[DNODE_MAX]` / `sub_h[DNODE_MAX]` | 負荷共有 |
| `arch/common/dkva.c:106-119` | `h_q_pub/sub`, `h_resp_pub/sub`, `h_rsum_pub/sub` 各 `[DNODE_MAX]` | DKVA per-node ハンドル6本 |
| `arch/common/dkva.c:537-539` | `resp_cache[DNODE_MAX]`, `pend_req/ttl[DNODE_MAX]` | DKVA 応答キャッシュ |
| `arch/common/pmesh.h:93` | `pmesh_routes[DNODE_MAX]` | 距離ベクタ経路表 |
| `arch/common/lookup.c:134,148,435` | `members[DNODE_MAX]` | HRW 母集団スナップショット |

→ ノードテーブルだけでなく、**per-node ハンドル / EWMA / キャッシュ**が
全モジュールに散在。数千へ単純拡大すると、これらが**静的に N 倍**膨らむ
（疎構造化が必須）。

### 1.2 壁B — O(N) / O(N²) の走査・gossip

| file:line | 計算量 | 内容 |
|---|---|---|
| `arch/common/kdds.c:246-252` | **publish 1回 = O(N)** | REGION スコープでない pub は `for n in 0..DNODE_MAX: pmesh_send`。メッシュ全体で **O(N²)** の全対全（`regions.md §1.2`） |
| `arch/common/dkva.c:393-428` | **窓ごと O(N²)** | FULL 集約の while 内 2重 for（fan-out を毎窓ポール） |
| `arch/common/moe.c:216-265` | **推論ごと O(N)** | 全 ALIVE ノードを走査して expert 選定 |
| `arch/common/world.c:261-365` | O(N) | per-node beacon の sub/poll |
| `arch/common/drpc.c:511-622` | O(N) | heartbeat broadcast / table 表示 |

→ **SWIM だけは既に sub-linear**: gossip は `SWIM_GOSSIP_MAX=3`
(`swim.h:31`) で round-robin、probe は cursor で 1 ラウンド 1 ターゲット
(`swim.c:307-308`)。**SWIM の設計は数千でも壊れにくい**——これが
フェデレーションの土台になる（後述 §2）。

### 1.3 壁C — トピック数・ハンドル数・セマフォ数の連鎖爆発

per-node トピックを開く設計が、`DNODE_MAX` に**比例した固定上限**を要求する。

```
arch/common/include/kdds.h:37  KDDS_TOPIC_MAX  160   /* dkva: q(1)+resp(32)+rsum(32)=65 */
arch/common/include/kdds.h:41  KDDS_HANDLE_MAX 320   /* =4×DNODE_MAX=128 + moe/world */
arch/aarch64/include/utk_config_depend.h:31  CFN_MAX_SEMID 256
                              /* DNODE_MAX=32: kdds_open は handle ごとに sem。
                                 dkva だけで 2×DNODE_MAX+2≈66 必要 */
```

→ `KDDS_TOPIC_MAX`・`KDDS_HANDLE_MAX`・`CFN_MAX_SEMID` は**すべて
`DNODE_MAX` の線形関数**としてサイズ決めされている（コメントに式が明記）。
N を 32→数千にすると **トピック 6×N、ハンドル 4×N、セマフォ 2×N** が必要に
なり、T-Kernel オブジェクト ID 上限（`CFN_MAX_SEMID` 等）を即座に枯渇させる。
**per-node トピックの廃止**（共有トピック＋送信元フィールド）が必須。

### 1.4 壁D — 単一パケットに詰める固定テーブル（MTU 上限）

```
arch/common/include/pmesh.h:58-67
  /* BEACON = 8 + DNODE_MAX×4 bytes (DNODE_MAX=32 -> 136 bytes, UDP 上限内) */
  PMESH_ROUTE_ENTRY entries[DNODE_MAX];   /* 全経路を1 beacon に詰める */
```

→ pmesh の距離ベクタ BEACON は**全ルーティング表を1 UDP パケット**に
入れる。`DNODE_MAX×4 + 8`。N=32 で 136B。N=350 で ~1400B（UDP 上限）。
**N>~340 で beacon が MTU を超えて壊れる**。距離ベクタの全網フラッディングは
数千では成立せず、**階層ルーティング or リンクステートの差分化**が要る。

### 1.5 単純に 32→数千へ上げると何が壊れるか（まとめ）

1. **ID 幅**: 8-bit `node_id` で >256 は表現不能（`drpc.h:36-38`）。配列以前の壁。
2. **メモリ**: §1.1 の全配列が静的に N 倍。`util_ewma[N][CLASSES]` 等は N×。
3. **gossip O(N²)**: kdds 全対全 pub（`kdds.c:246`）。1台が見る帯域が N に比例。
4. **トピック爆発**: per-node topic が 6×N（§1.3）。`CFN_MAX_SEMID` 枯渇。
5. **MTU 超過**: pmesh BEACON が N>~340 で UDP を溢れる（§1.4）。
6. **TCB/セマフォ上限**: `CFN_MAX_TSKID 128` / `CFN_MAX_SEMID 256`
   (`utk_config_depend.h:30-31`) が per-node リソースで先に枯れる。
7. **relay の片肺**: relay は `NODE_MAX 256` まで REGISTER 可
   (`relay/relay.c:39`) だが、**>=DNODE_MAX のノードはカーネル論理に不在**
   （G7）。relay と kernel で天井が食い違っている。

---

## 2. 階層フェデレーション設計（推奨案）

### 2.1 核心アイデア — 葉は温存、上に層を積む

> **既存の `DNODE_MAX=32` 機構を「葉クラスタ」の中だけで温存し、
> 葉と葉の間を coordinator 同士の疎なメッシュで繋ぐ。**

`regions.md` は region を「RTT≤τ の局所葉」として既に定義済み
(`region.h:23` `REGION_TAU_MS 50`、`region.c` で coordinator＝最小 ID)。
本設計はそれを**文字どおり階層化**する：

```
                  ┌─ 上位メッシュ (coordinator 同士, 疎な gossip) ─┐
                  │   global topic = O(region 数) の集約のみ        │
         ┌────────┴────────┬────────────────┬────────────────┐
    ┌────┴────┐       ┌────┴────┐      ┌────┴────┐
    │ region0 │       │ region1 │      │ region2 │  ← 各 ≤ DNODE_MAX(=32) ノード
    │ coord=c0│       │ coord=c1│      │ coord=c2│
    │ ●─●─●─● │       │ ●─●─●   │      │ ●─●─●─● │  葉内は現行 32 機構を温存
    │  密 (現行)│       │  密 (現行)│      │  密 (現行)│  kdds/dkva/moe/swim そのまま
    └─────────┘       └─────────┘      └─────────┘
    葉内 = O(32²) で閉じる        葉間 = O(region 数) の要約のみ
```

- **葉内（intra-region）**: 現行コードを**1行も壊さず**そのまま使う。
  `dnode_table[DNODE_MAX]`・kdds REGION スコープ pub（`kdds.c:247-251` は
  既に `region_is_member()` で部分集合配信できる）・dkva の region 内集約
  （`dkva.c:383` `region_is_member(n)` で expect）・SWIM。**葉は最大 32
  ノードの完結したクラスタ**。
- **葉間（inter-region）**: coordinator だけが**上位メッシュ**に参加する。
  上位メッシュも SWIM/kdds で実装するが、**メンバは coordinator のみ**
  （region 数 R 個）。葉内のチャター（DKVA Q/resp、MoE スコア、replica）は
  **葉の外に出ない**。出るのは coordinator が作る**要約**だけ。
  dkva は既に rsum（region summary）の機構を持つ（`dkva.c:118-119`
  `h_rsum_pub/sub`、`dkva.c:354` `rgot`、`dkva.c:361` `rc_expect` で他
  region coordinator の rsum を待つ）。**2段集約の片鱗は既にある**。

### 2.2 ID の複合化 — (region_id, local_id)

8-bit `node_id` の天井（256）を、**複合 ID** で突破する。

```
論理ノード ID = (region_id, local_id)
  local_id  : 0..31   葉内で一意（現行 DNODE_MAX 空間をそのまま）
  region_id : 0..R    上位メッシュで一意（coordinator が払い出す or HRW）
```

- **葉内のパケット**は今のまま `node_id` 8bit で動く（local_id だけ使う）。
  → **後方互換**: 単一 region（＝今の素のメッシュ）は何も変わらない。
- **葉間のパケット**だけ `(region_id, local_id)` を運ぶ新ヘッダを使う。
  `GOBJ` の 24-bit local 空間（`drpc.h:78-81`）に region_id を畳む案も可
  （例: local の上位 8bit を region_id に再解釈）。**total = R × 32**。
  R=256 でも 8192 ノード、R を 16bit にすれば数百万。
- 容量試算: region 64 個 × 32 = **2048 ノード**で「数千」の看板に到達。
  上位メッシュのメンバは 64（coordinator のみ）→ SWIM/gossip は余裕。

### 2.3 思想との整合（中央なし）

- coordinator は `region.c` 既存ロジック（**region 内最小 ID、決定的**）で
  選ぶ。死ねば次の最小 ID が継ぐ。**恒久的中央ではない**——この
  **min-id 補題は [[decentralized-lookup.md]] §2.2 が正準**（本書は葉間 coordinator へ
  適用するだけで再導出しない）。
- 上位メッシュも**フラットな coordinator 群の gossip**であり、ルート単一点を
  持たない。多層化（region of regions of regions）も同じ再帰で可能。
- survival-network §4「region＝局所葉、近傍は密・遠方は疎」を**構造として実体化**。

---

## 3. 代替案との比較

| 観点 | (a) 階層フェデレーション【推奨】 | (b) DNODE_MAX 拡大＋疎データ構造 | (c) 完全 P2P DHT (HRW/Chord) |
|---|---|---|---|
| **葉内コードへの影響** | ゼロ（温存）。葉＝現行 32 機構 | 全 per-node 配列を動的配列/ハッシュ表へ書き換え。広範囲改修 | drpc/kdds/dkva の母集団概念を全置換 |
| **ID 空間** | 複合 (region,local)。8bit×2 で 8192、拡張容易 | 8-bit を 16/32bit へ拡張（wire 全書き換え、G7 直撃） | 安定識別子→HRW 写像（dynamic-id §3B）。空間拡張前提 |
| **gossip 計算量** | 葉内 O(32²) 固定＋葉間 O(R)。**全体 O(N) 近傍** | 差分 gossip で O(N log N) 目標だが**全網が1つの母集団** | O(N log N)（finger/HRW lookup）だが**全 lookup が網越し** |
| **データ局所性** | ◎ 葉内に閉じる（survival §4 と一致） | △ 母集団は1枚。スコープ topic で緩和必要 | △ key の責任者は網全体に散る（局所性が消える） |
| **思想（中央なし）** | ◎ coordinator＝一時的役割。多層も再帰 | ○ 中央なしだが「1枚の巨大網」は survival §4 と不整合 | ◎ 最も純粋に中央なし。だが「1つの脳の半球」概念が薄れる |
| **既存資産の再利用** | ◎◎ region.c / dkva rsum / kdds REGION scope / swim RTT を**そのまま** | △ lookup.c HRW は使えるが配列を全部置換 | ○ lookup.c HRW を全網へ拡張（`lookup.c:96` の母集団を network-wide に） |
| **実装コスト** | 中（上位メッシュ＋複合 ID。葉は不変） | 大（全モジュールの配列を疎構造へ） | 大（finger table / churn 安定化 / lookup ルーティング） |
| **MTU 問題 (§1.4)** | ◎ pmesh は葉内のみ。葉間は coordinator 経路 | × BEACON が全網 → 階層化なしでは N>340 で破綻 | ○ DHT は経路を持たないが lookup ホップが増える |
| **段階導入** | ◎ 単一 region＝今のまま。R を 1→増やすだけ | △ 全書き換えは一括になりがち | × 中途半端だと既存も DHT も動かない（dynamic-id §3C 同旨） |

### 3.1 結論（推奨）

**(a) 階層フェデレーションを推奨する。** 理由:

1. **葉を壊さない**。`DNODE_MAX=32` の機構（§1 で列挙した全配列・全走査）を
   葉内に**温存**するので、第3版までに積み上げた実証済み資産（dkva 2段集約・
   kdds REGION scope・swim RTT・region coordinator）が**そのまま動く**。
   (b)(c) は母集団そのものを置換するため、動いている分散推論を一度壊す。
2. **思想と構造が一致**。survival §4「局所葉、近傍密・遠方疎」＝そのまま階層。
   (b) の「1枚の巨大網」は局所性思想に逆行。(c) は中央なしには忠実だが
   「1つの脳の半球（region）」という p-kernel 固有の世界観が薄まる。
3. **段階導入が自然**（§4）。R=1（単一 region）は**今のメッシュと完全一致**＝
   後方互換が無料。R を増やすほど数千へ漸進。(b)(c) は一括移行になりやすい。
4. **(b)(c) を排除しない**: 葉間メッシュの**実装手段**として (c) の HRW
   (`lookup.c`) を coordinator 群に適用でき、葉内母集団の疎構造化として
   (b) を将来取り込める。**(a) は (b)(c) の上位フレームでもある**。

---

## 4. 推奨ロードマップ（32 → 数百 → 数千）

各段で「何を変えるか・後方互換・連動する監査項目」を明示する。
**いずれも本書時点では未実装の設計目標**。

### F0 — 葉の確定（32 で頭打ちのまま、上位の足場を作る）
- **変更**: 何も壊さない。`region.c`（既存）を「葉の定義」と再宣言。
  coordinator が「上位メッシュ参加者」であることを `region.h` に記述するだけ。
- **後方互換**: 完全（コード不変）。単一 region＝現行。
- **連動**: なし。土台確認のみ。

### F1 — 複合 ID と上位メッシュの配管（→ 数百ノード, R≤8 × 32）
- **変更**: (region_id, local_id) 複合 ID（§2.2）。coordinator だけが参加する
  **上位 SWIM/kdds メッシュ**を新設（葉内とは別ポート/別母集団）。葉間 topic は
  `global/...`、葉内は `region/<rid>/...`（kdds の REGION スコープを流用、
  `kdds.c:247`）。dkva の rsum（`dkva.c:118-119`）を葉間集約の正式経路に昇格。
- **後方互換**: 葉内パケットは 8-bit のまま不変。複合 ID は葉間ヘッダのみ。
  R=1 では上位メッシュが空＝今と同一動作。
- **連動**:
  - **G7（ID 空間）**: 複合 ID が 8-bit 天井を上位で突破する第一歩。
  - **G6（動的 ID）**: dynamic-id の relay lease（実装済み）を**葉内 local_id**
    の払い出しに、region_id は coordinator/HRW で。三段モード（固定/lease/p2p,
    `dynamic-id.md §5`）と複合 ID を接続。
  - **G13（region 跨ぎ直列化）**: 葉間は coordinator 1点を通る → 直列化点が
    明示化される。ここで rsum の順序保証を設計（未解決問題、§6）。

### F2 — 疎構造化と差分 gossip（→ 千ノード, R≤32 × 32）
- **変更**: §1.1 の per-node 固定配列を、**葉内は 32 のまま**・**葉間は疎構造**
  （coordinator 数ぶんのハッシュ表 / 動的配列）へ。kdds の per-node topic を
  廃し**共有 topic＋src フィールド**に（壁C 解消、`kdds.h:37-43`/`CFN_MAX_SEMID`
  の線形依存を断つ）。pmesh BEACON（壁D, `pmesh.h:58`）は**葉内限定**にし、
  葉間ルーティングは coordinator 経路表（疎・差分）へ。
- **後方互換**: 葉内 API 不変。共有 topic 化は kdds 内部実装の差し替えで吸収。
- **連動**:
  - **G14（同時性上限）**: per-node セマフォ廃止で `CFN_MAX_SEMID` 線形依存が
    切れ、葉あたり一定リソースに。同時実行上限が N から葉サイズへ。
  - **計算量目標**: kdds 全対全 O(N²)（`kdds.c:246`）→ 葉内 O(32²)＋葉間
    O(R log R) で **全体 O(N log N) 近傍**（未実証の設計目標）。

### F3 — 多層化と locality 最適化（→ 数千〜, region of regions）
- **変更**: 上位メッシュ自体が大きくなれば、coordinator を**さらに region 化**
  （再帰）。葉間 HRW（`lookup.c` の HRW を coordinator 群へ）で責任分散。
  locality-MoE（`regions.md §3.3` の utility）を葉間にも適用。
- **後方互換**: 下位層は不変。層を積むだけ。
- **連動**: regions R3（locality-MoE / 容量スケール）と合流。survival §7
  （分散ゲーティング）が葉内＋葉間の二層勾配として実体化。

### 4.x Phase D が強制力
- `regions.md §5` の通り、**Android フリート（UMP）が R0–R1 の強制力**。
  実機スマホ群＝台数可変・到達範囲バラバラ・異種混在で、**R が机上でなく
  現実の数字**になる。数千ノードは Phase D（Play Store 配布）で初めて本物の
  圧力になる。F1–F2 は Phase D の配備規模に追従して進める。

---

## 5. 正直さ（設計だけ / 未実証 / 未解決）

### 5.1 何が LIVE で、何がまだ「設計だけ」か（R0 更新 2026-06-20）
- **R0 で LIVE + certified になったもの**: 2-cluster の階層集約（region 内
  per-node `resp/<n>` REGION + region 間 coordinator `rsum/<rid>` GLOBAL の
  要約のみ越境）。`[fed-2cluster][in-proc]`（`dkva_fed2_self_test`: cross-region
  期待 = O(#region)）+ `[fed-2cluster][live]`
  （`samples/11_distributed/run_4node_regions.sh`: 実プロセスで `[locality]` を
  diff し flat-control falsifier で階層 vs 退化を区別）+ `[coord-crash][in-proc]`
  （coordinator 死 → 決定論的 min-id 再委譲、投票なし）。**橋桁の R0 の踏み板は
  架かった。**
- **まだ「設計だけ」で未実装なもの**: 複合 ID（§2.2; 256 の壁 = F1）、上位メッシュ
  （F3）、葉間 rsum の region-of-regions 再帰昇格（F3）、共有トピック化（F2）。
- 既存で**動いている**土台: region 形成（`region.c`）、kdds REGION スコープ
  配信（`kdds.c:262-285`）、dkva の region 内/間 2段集約（`dkva.c:430-520`）、
  swim RTT + sim_zone（`swim.c:358-387`）、world region gossip
  （`world.c:228,315`）、relay lease（`relay.c`、dynamic-id 実装済み）。

### 5.2 計算量の主張は「目標」であって測定値ではない（R0 で一部実測済み）
- 「O(N²)→O(N log N)」は**設計目標**で全域は未実測。ただし **R0 の `[fed-2cluster]
  [live]` が cross-region 越境を実測**し、2-region/4-node で far_delta=6（=summary
  のみ、O(#region)）vs flat-control far=0/near=9（=全 N 直接 fan-in）を機械的に
  区別した。「葉間 O(#region)」は **この topology で実測 PASS**。数千ノード規模の
  収束時間・rsum 遅延は依然 **未測定**（§5.4 の 10k-dream gap）。
- SWIM が sub-linear（`swim.h:31` GOSSIP_MAX=3）なのは**コードから確認済み**
  の事実。これが土台になる根拠は確か。

### 5.3 未解決の仮定（実証が要る）
1. **複合 ID の wire 設計**: region_id を `GOBJ` の 24-bit local に畳むか、
   新フィールドを足すか。後者は wire 後方互換に注意（drpc v1 と共存）。
2. **葉間一貫性**: coordinator を跨ぐ rsum の順序/合意は eventual で十分か、
   region ごとに合意（raft）が要るか（`regions.md §6.2` の未解決問題のまま）。
3. **coordinator churn**: coordinator が落ちた瞬間の葉間メッシュの穴。
   最小 ID 引き継ぎ（`region.c`）の収束窓で rsum が欠ける可能性。
4. **複数 relay の lease 調停**（`dynamic-id.md §4`）が region_id 払い出しに
   波及。単一 relay 前提のまま複合 ID を入れると region_id が衝突しうる。
5. **τ の決め方**（`region.h:24` REGION_TAU_MS=50 は仮）。葉サイズが 32 を
   超える密クラスタが実在したら、葉の**再分割**規約が要る（本書は未設計）。
6. **MTU と多層**: 葉間経路表が大きくなる超大規模では F3 の再帰が必須だが、
   再帰の深さ・収束は未検討。

### 5.4 やらないと決めたこと
- **8-bit wire の即時拡張はしない**（G7 を真正面から殴ると wire 全書き換え＝
  動いている全デモが壊れる）。複合 ID で**葉内 8-bit を温存しつつ上位で
  突破**する迂回路を採る。これは `dynamic-id.md §3C` の「中途半端な一括移行を
  避ける」判断と同じ哲学。

---

## 6. 一行まとめ

> **32 は壁ではなく「葉の大きさ」にする。** 葉（region, ≤32）を温存し、
> coordinator 同士の疎な上位メッシュと複合 ID (region_id, local_id) で
> 数千へ橋を架ける。葉内 O(32²)・葉間 O(R) で全体 O(N log N) を狙う。
> 既存資産（region.c / dkva rsum / kdds REGION scope / swim RTT / relay
> lease）は**そのまま再利用**。実装は R3 / Phase D。本書は地図であり、
> 1 行のコードも変えていない。
