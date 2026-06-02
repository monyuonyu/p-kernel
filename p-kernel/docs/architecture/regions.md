# Regions — 台数でスケールする region 分割 locality-MoE

> p-kernel は「全体で1つの脳」である。しかし**1つの脳とは、均一な1枚の網のことではない。**
> 脳は領域（region）に分かれ、近い領域は密に、遠い領域は疎に繋がる。
> このドキュメントは、現在のフラットで貧弱な分散モデルを、
> **遅延でクラスタ化された region**・**ローカリティを見る疎な MoE**・
> **台数とともに増える容量**へと再設計するための一枚の絵である。

Status: **R0✅ R1✅ R2✅（配管完了）/ R3 未着手** / 最終更新: 2026-06-02

関連: [[project_pkernel_philosophy]]（5レイヤー世界観の Collective 層）、
`phase_b_relay.md`（relay 基盤）、`android.md`（UMP フリート = この設計の強制力）。

---

## 0. なぜ今これを書くか

relay 経由の分散推論（REDUCED テンソル並列 / FULL DKVA）が master に入った
（PR #1）。Collective 層は「文字通り」になった — 異種ノードが1つの Transformer
を一緒に計算する。だが、その土台は**2つの意味で貧弱**である。これを直視した
うえで、3つの改善（容量スケール・locality-MoE・region 分割）が実は**1つの
アーキテクチャの3つの面**であることを示す。

---

## 1. 現状の正確な姿（数字で）

### 1.1 モデル本体（`arch/common/include/dtr.h`）

| 次元 | 値 |
|---|---|
| d_model | **8** |
| ヘッド数 / ヘッド次元 | **2** / **4** |
| トークン長 | **4**（センサ4ch） |
| FFN 中間次元 | **16** |
| 総重み | **635 float** |

→ Transformer の形をした **635 パラメータの玩具**。1台で一瞬で回る。
**分散する必然性が、まだモデル側に無い。**

### 1.2 通信モデル（ここがより貧弱）

- `kdds_pub` は分散時 `for n in 0..DNODE_MAX: ALIVE なら全員へ UDP 送信`。
  **フラットな全対全ブロードキャスト。** pub 1回 = O(N)、メッシュ全体 = O(N²)。
  （`arch/common/kdds.c:184`）
- `DNODE_MAX = 8` 固定。**8台で頭打ち。**（`arch/common/include/drpc.h`）
- `degrade.c` は **SOLO / REDUCED / FULL の3段**を「生のノード数」だけで決める。
  `3+` は全部 FULL。**容量は台数でスケールせず**、分散戦略を切り替えるだけ。
- `moe.c` のエキスパート = **ノード1台まるごと**。`select_expert()` は
  `accuracy[gate_class]` が最高のノードを選ぶだけで、**RTT も帯域も見ない。**
  スコアは5秒ごとに全員へブロードキャスト。

### 1.3 一言で

> **均一・全対全・8台上限・容量固定**。
> これは「脳」ではなく「8席の円卓で全員が同時に叫ぶ部屋」。

---

## 2. テーゼ：3つの面は1つのアーキテクチャ

| 改善したいこと | 今のコード | 欠けているもの |
|---|---|---|
| ①台数で網を大小させる | degrade 3段固定・容量不変 | **容量 = f(台数)** の連続関数 |
| ②到達範囲・速度で MoE を区切る | accuracy のみのゲーティング | **locality-aware** cost = f(accuracy, RTT, 帯域) |
| ③1つの脳だが領域を区切る | フラット全対全・8台上限 | **遅延クラスタによる region** + region スコープ topic |

これらは独立した機能ではない。**「遅延でクラスタ化された region、各 region が
ローカリティ・ルーティングする疎な MoE、台数とともに容量が増える」**という
一枚の絵の、3つの側面である。

---

## 3. コア定義

### 3.1 Region（遅延クラスタ）

**Region とは、相互 RTT が閾値 τ 以内のノード集合である。**
物理的・ネットワーク的に「近い」ノードが1つの region を成す。

- **形成（貪欲・分散）**: 各ノードは、既知の region coordinator のうち
  RTT ≤ τ の最も近いものに join する。該当が無ければ自分が新しい region の
  coordinator になる。coordinator は region 内の最小 node-id（決定的）。
- **メンバシップ**: SWIM の ALIVE 集合を RTT で部分集合化したもの。SWIM の
  ping/ack に RTT サンプルを相乗りさせ、ノードごとに **RTT の EWMA** を持つ。
- **再編成**: RTT が τ を跨いで悪化／改善したら region を移動。SWIM の
  membership 変化イベントに便乗（ヒステリシスで振動を防ぐ）。
- **インフラ非依存**: region は SWIM/K-DDS の上に乗る論理構造。物理トポロジや
  relay の有無に依存しない。

> τ は「同一脳半球」と「別半球」を分ける距離。初期値は実測（Phase D の
> Android フリートが本物の τ を教えてくれる）。

### 3.2 容量スケール `capacity(N)`

モデルの「大きさ」は固定の1次元ではない。**実行時に増やせる軸**と
**プロビジョニング（重み供給）を要する軸**を分けて考える。

| 容量の軸 | 実行時スケール可？ | スケール手段 |
|---|---|---|
| **breadth（エキスパート数）** | ✅ | ノード join で expert 追加（MoE） |
| **depth（層数）** | ✅ | pipeline 並列の段数を増やす |
| **KV-context（文脈の広さ）** | ✅ | DKVA が KV キャッシュをノード横断でプール |
| **width（d_model）** | ❌ | 重みの再形成＝再学習。R3 の重い課題 |

→ 当面の `capacity(N)` は **breadth × depth × KV-context** で定義する。
width（玩具の核 d_model=8）を太らせるのは別レイヤー（学習）の問題として切る。

```
experts_active(N)   = clamp(N, 1, E_max)          # ノード ≒ エキスパート
pipeline_depth(N)   = 1 + floor(log2(N))          # 台数の対数で深くする
kv_context(N)       = Σ_{n∈region} kv_count[n]    # region 内の KV を合算
```

degrade の3段は**この連続関数の粗いバンド**として残す（SOLO/REDUCED/FULL は
人間が読むラベル）。内部判断は `capacity(N)` の数値で行う。

> **実装済み (R2)**: `degrade.c` / `degrade.h` に `capacity_experts()` /
> `capacity_depth()` / `capacity_kv()` / `capacity_score()` を追加。
> breadth は ALIVE 全ノード (global MoE) を `clamp(N,1,CAP_E_MAX)`、depth は
> region 内 (密) の `1+floor(log2(region_size))`、KV-context は dkva.c の階層
> 集約が実際に畳んだ KV エントリ総数 (`capacity_note_kv()` で供給。推論前は
> `region_size × DKVA_CACHE_SIZE` の見積り)。`dist` シェルコマンドで読める。
> 例: 4 ノード / 2 region の素のメッシュで experts=4・depth=2・kv=16(estimate)、
> 1 回推論すると kv=12 (= 全 4 ノードの KV を厳密復元、measured) に切り替わる。

### 3.3 Locality-aware ゲーティング

`select_expert()` を **accuracy だけ**から **効用（utility）最大化**へ。

```
utility(node, class) =  α · accuracy[node][class]      # 賢さ
                      − β · rtt_norm(node)             # 近さ
                      − γ · (1 / bw_norm(node))        # 太さ（帯域）
```

- **region 内を優先**。cross-region のエキスパートは、効用の上積みが
  region 跨ぎの遅延ペナルティを上回るときだけ選ぶ。
- これにより「賢いが遠いノード」より「そこそこ賢く近いノード」が選ばれ、
  尾の重い応答時間が縮む。MoE が**地理的に自己組織化**する。

### 3.4 K-DDS トピックのスコープ階層

フラット全対全 O(N²) を殺す本丸。トピックに**スコープ**を持たせる。

| スコープ | トピック命名 | 配信先 | コスト |
|---|---|---|---|
| **region 内** | `region/<rid>/dtr/dkva/q` | region メンバのみ | O(region_size²) |
| **region 間** | `global/dtr/...` | coordinator 間のみ | O(region 数) |

- 大半のチャター（DKVA の Q/resp、MoE スコア、replica）は **region 内**に閉じる。
- region 間は **coordinator がゲートウェイ**となり、要約だけを疎に交換する
  （階層集約：region 内で部分集約 → coordinator が region 間で再集約）。
- `kdds.c` の配信ループは、トピックのスコープを見て**メンバ部分集合**だけを
  舐める。`DNODE_MAX` 固定の全列挙をやめる。

> これは DKVA の集約とも噛み合う：FULL の Q ブロードキャストを
> **region 内 → region 間の2段**にすれば、台数が増えても各ノードが見る
> トラフィックは region サイズに比例し続ける（全体 N には比例しない）。

---

## 4. 既存モジュールへのマッピング

| モジュール | 変更 | 面 |
|---|---|---|
| `swim.c` / `swim.h` | ping/ack に RTT サンプル相乗り、ノードごと RTT EWMA。帯域推定（任意）。 | ①②③ |
| `drpc.h` | `DNODE_MAX` 8 → 動的／大幅拡大。ノードテーブルを疎に。 | ①③ |
| **`region.c`（新規）** | region 形成・coordinator 選出・再編成。SWIM の上に薄く乗る。 | ③ |
| `degrade.c` | 3段 enum → `capacity(N)` 連続関数。SOLO/REDUCED/FULL は表示ラベルに降格。 | ① |
| `kdds.c` / `kdds.h` | トピックにスコープ（region/global）。配信ループをメンバ部分集合へ。 | ③ |
| `moe.c` | `select_expert()` を utility 最大化（RTT/帯域込み）。expert を region でグループ化。 | ② |
| `dtr.c` / `dkva.c` | DKVA Q を region 内→region 間の2段集約に。`capacity(N)` で expert/層数を駆動。 | ①③ |

**不変条件（壊さない約束）**:
- 単体（SOLO）で完結する経路は常に残す（孤立ノードも脳である）。
- region は**論理層**。relay 無し・SWIM だけの素のメッシュでも動く。
- 異種 ABI（aarch64 + x86_64 + Android）を跨いで region は成立する。

---

## 5. シーケンス（R0→R3）と Phase D の織り込み

| 段階 | 内容 | モデルサイズ依存 |
|---|---|---|
| **R0**（土台） | SWIM に RTT、`DNODE_MAX` 引き上げ、`region.c` 骨格、K-DDS スコープ topic。 | 非依存 |
| **R1**（locality-MoE） | `select_expert()` に RTT/帯域。expert を region 単位に。 | 非依存 |
| **R2**（容量スケール） | ~~DKVA 2段集約~~ ✅ / ~~`capacity(N)` 連続関数~~ ✅ → expert/層を駆動するのは R3 で網が太ってから。 | 弱依存 |
| **R3**（網を太らせる） | d_model/層/expert 重みを実用サイズへ。重み供給・学習。 | **本丸・別レイヤー** |

**Phase D（Android）は R0–R1 の強制力**。実機スマホ群は台数可変・到達範囲
バラバラ・異種混在そのもの。R0 の RTT/region は、Android フリートで初めて
**机上でなく現実の制約**になる。よって Phase D と R0/R1 は一緒に進めたい。

> 鶏と卵への正直な答え：R0–R2 は**玩具モデルのままでも正しく作れる**（通信
> トポロジの話なので）。だが「区切る意味」が本当に出るのは R3 で網が1台に
> 収まらなくなってから。R0–R2 で**配管**を通し、R3 で**水量**を上げる。

---

## 6. 未解決の問い（正直に）

1. **重みの供給（provisioning）**: 新しいスマホが join したとき、どの expert の
   重みを、どこから取るのか。relay 経由ダウンロード？ region coordinator が配布？
   → R3 の核心。`fedlearn.c`（連合学習）と接続する可能性。
2. **ルーティング表の一貫性**: region をまたぐ expert マップは eventual で良いか、
   region ごとに `raft.c` で合意を取るか。
3. **学習ループ**: capacity が増えたぶんの新 expert は誰が訓練するのか。
   推論フリートが同時に学習フリートになるのか（`ga.c` / `fedlearn.c`）。
4. **region 間の信頼**: relay v2 の HMAC はリンク単位。region をまたぐ信頼は
   どう定義するか（同一鍵か、region ごとの鍵か）。
5. **τ の決め方**: 静的閾値か、分布から動的に（k-means 的に）クラスタするか。

---

## 7. 非目標（embrace すること）

- **同期した1枚のテンソルにはしない。** 脳は lock-step ではない。非同期・
  eventual consistency を受け入れる。
- **「1つの脳」は同期の意味ではなく、共有トピック空間と同一アイデンティティの
  意味。** region は半球であり、別の生き物ではない。
- 完璧な負荷分散より、**局所性と自己組織化**を優先する。

---

## 付録 A — 一枚の絵（ASCII）

```
                    global/  (coordinator 間, 疎)
        ┌───────────────┼───────────────┐
        │               │               │
   ┌────┴────┐     ┌────┴────┐     ┌────┴────┐
   │ region0 │     │ region1 │     │ region2 │   ← 遅延クラスタ
   │ RTT≤τ   │     │ RTT≤τ   │     │ RTT≤τ   │
   │ ●─●─●   │     │ ●─●     │     │ ●─●─●─● │   ● = ノード(=expert)
   │  ╲│╱    │     │  │      │     │  密に結合 │
   │ coord   │     │ coord   │     │ coord   │
   └─────────┘     └─────────┘     └─────────┘
   region 内 = 密 (DKVA/MoE/replica)    region 間 = 疎 (要約のみ)

   capacity = Σ_region (breadth × depth × KV-context)
            ↑ 台数 N とともに増える
```
