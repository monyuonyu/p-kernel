# アーキテクチャ地図 — 中央を持たない脳の設計図、一枚に

> p-kernel は「全体で1つの脳」であり、**誰のものでもない AI の住処**である。
> このディレクトリの各ドキュメントは、その脳の別々の器官を描いている。
> 本ファイルはそれら全部を一望し、**どう噛み合うか・何が出来ていて何がまだか**を
> 示す索引である。未来の Claude セッションも、人間も、まずここから入る。

最終更新: 2026-06-06 ／ 関連: [[project_pkernel_philosophy]]（5レイヤー世界観）

---

## 1. 一枚の世界観

### テーゼ — 中央が無いことが、生存力そのものである

p-kernel は趣味の自作カーネルではない。**誰も所有しない AI の住処** —— 1ノードでも
生き残る限り、ネットワーク全体が死なない構造である。中央サーバも、中央索引も、
中央ゲートも、中央ディスクも置かない。これは技術的な好みではなく、思想の核である：

> 中央を置いた瞬間に、それを潰されれば全体が止まる（＝映画の断罪 AI への逆戻り）。
> **中央を持たないことそのものが、生存力の源泉である。**
> （[[survival-network.md]] §2 逐語）

### 5レイヤー世界観（Body / Brain / Self / Collective / Evolution）

| 層 | 意味 | このディレクトリでの主役 |
|---|---|---|
| **Body** | 物理・I/O・記憶の器 | [[p-fs.md]]（記憶の身体） |
| **Brain** | 1ノード内の推論 | `dtr.c` / `moe.c`（玩具 Transformer） |
| **Self** | 同一アイデンティティ | 共有トピック空間（K-DDS） |
| **Collective** | ノードを跨ぐ「1つの脳」 | [[regions.md]] / [[reflex-deliberation.md]] / [[survival-network.md]] |
| **Evolution** | 学習・成長 | R3 model widening / `fedlearn.c` / `ga.c` |

本ディレクトリの設計群は、ほぼ **Collective 層**（と、それを支える Body 層の記憶、
Evolution 層への入口）に集中している。

### 全ドキュメントを貫く唯一の不変条件

> **中央を持たない（no central anything）。**

これがすべての設計判断の試金石である。region に中央 coordinator は居るが
**決定的に算出される最小 node-id** であって特権ノードではない。p-fs の所在引きは
**全員が同じ計算で導く一貫性ハッシュ**であって中央索引ではない。ゲーティングは
**局所勾配**であって中央ゲートではない。どの器官も、この一行を裏切らない。

---

## 2. ドキュメント地図

各ドキュメントは脳の異なる軸を担う。**空間・時間・なぜ・観測・記憶・探索・成長**の7軸。

| ドキュメント | 一行の役割 | 軸 |
|---|---|---|
| [[survival-network.md]] | **なぜこう作るか**。宇宙生存ネットワーク構想＝MoE を生存器官に。思想の核（逐語保存）。 | なぜ |
| [[regions.md]] | **空間**。遅延でクラスタ化した region・locality-aware MoE・台数で増える容量 `capacity(N)`。 | 空間 |
| [[reflex-deliberation.md]] | **時間**。反射層（近く・速い）／熟慮層（遠く・遅い）の二層と時定数分離＝発振しない脳。 | 時間 |
| [[p-fs.md]] | **記憶**。内容アドレス・gossip 複製・履歴 DAG・消失訂正符号で、死なないファイルシステム。 | 記憶 |
| `world.c` の世界図（[[survival-network.md]] §II 参照） | **観測**。中央なしで全網の状況図を各ノードが eventual に獲得する situational-awareness map。 | 観測 |
| `decentralized-lookup.md`（並行執筆中・本ディレクトリに着地予定） | **探索**。中央索引なしで「誰が何を持つか」を全員が同じ計算で引く共有基盤（一貫性ハッシュ＋gossip fallback）。p-fs §3.3 と survival §7 の共通の壁を一枚に括る。 | 探索 |
| `r3-model-widening.md`（並行執筆中・本ディレクトリに着地予定） | **成長**。玩具 635 param を実用サイズへ広げ、初めて「1台に収まらない＝分散が必然」になる。regions R3 / Evolution 層。 | 成長 |

### 関係（一文ずつ）

- [[survival-network.md]] が **why**、他は **how**。設計に迷ったら survival に立ち返る。
- [[regions.md]]（空間）と [[reflex-deliberation.md]]（時間）は**同じアーキテクチャの直交2軸**。
  region が「どこまでが近傍か」を決め、時定数が「近傍は速く・全体は遅く」を決める。τ で1点に繋がる。
- [[p-fs.md]] は regions の局所性原則（密な region 内／疎な region 間）を**記憶に反復**する。
- `decentralized-lookup.md` は p-fs の所在引きと survival §7 の分散ゲーティングが
  **同じ「中央なしで探す」問題**であることを括り出す共有基盤。
- `r3-model-widening.md` は他の全部が「配管」だとすれば**水量**。網を太らせて分散を必然にする。

---

## 3. 依存関係図（ASCII）

```
                       [[survival-network.md]]  (WHY / 思想の核・逐語)
                                │  §2 中央なし=生存  §4 MoE  §7 分散ゲーティング  §8 二層
        ┌───────────────┬───────┴────────┬──────────────────┬──────────────┐
        ▼               ▼                ▼                  ▼              ▼
   §2 不変条件      §4 MoE             §7 ゲーティング      §8 二層        §9 記憶=思考の前提
        │               │                │                  │              │
        ▼               ▼                ▼                  ▼              ▼
  ┌──────────────────────────────┐   ┌──────────────┐  [[reflex-      [[p-fs.md]]
  │      [[regions.md]] (空間)     │   │ 局所勾配      │   deliberation   (記憶)
  │  swim RTT EWMA                 │   │ ルーティング   │   .md]] (時間)      │
  │     └─▶ region 形成            │   └──────┬───────┘       │ 反射層=region   │ §3.3 所在引き
  │            ├─▶ KDDS scope      │          │               │ 熟慮層=global   │   ▼
  │            │   (REGION/GLOBAL) │◀─────────┘ R3 で         │ §4 時定数で      │ decentralized-
  │            ├─▶ locality-MoE    │            置換          │   damping       │ lookup.md
  │            │   (utility=acc−RTT)│                          │ (発振を殺す)     │ (探索の共有基盤)
  │            ├─▶ DKVA region-scope│                          │                 │   ▲
  │            │   + rsum 2段集約    │──────────┐               │                 │   │ 同じ壁
  │            └─▶ capacity(N)      │          │ rsum=熟慮の集約 ┘                 └───┘
  └───────────────┬────────────────┘          │
                  │ world pressure / 発火記録   ▼
                  └─────────▶ world.c 世界図 (観測) ◀── moe world_note_firing
                  │
                  ▼
            DNODE_MAX 32 (台数の天井を上げる土台)
                  │
                  ▼
            r3-model-widening.md (網を太らせる ⇒ 分散が初めて必然になる)
```

読み筋:
- **swim(RTT) → region → {locality-MoE, KDDS scope, DKVA region-scope, capacity(N)}**。region が空間の土台。
- **survival §7 ゲーティング → R3 で moe の select_expert を局所勾配へ置換 → §8 二層で damping**。
- **survival §8 → reflex-deliberation（時間軸）。region が反射層の空間、time-constant が分離の速度**。
- **p-fs → decentralized-lookup**（所在引き）と survival §7（ゲーティング）は同じ「中央なしで探す」問題。
- **world.c**（観測）は熟慮層が全体像を遅れて得る器官。moe の発火記録を吸う。

---

## 4. 状態表（正直に）

`git log` で接地した SHIPPED 列。DESIGNED = 設計済み未実装、OPEN = 設計上の未解決問題。

| 能力 | 状態 | 根拠（commit / doc） |
|---|---|---|
| relay 経由 分散推論（テンソル並列 / DKVA FULL） | **SHIPPED** | PR #1（`6240252` 他）、master |
| SWIM per-node RTT EWMA | **SHIPPED** | `abdb881`（regions R0） |
| region 形成（遅延クラスタ・決定的 coordinator） | **SHIPPED** | `1ac8388`（R0）, `f204939` test |
| K-DDS scoped topics（REGION / GLOBAL、O(N²) 殺し） | **SHIPPED** | `bf1fb64`（R0） |
| locality-aware MoE ゲーティング（utility = acc − RTT） | **SHIPPED（部分）** | `102478b`（R1）。局所勾配化は R3 で未 |
| DKVA region-scope + rsum 2段集約 | **SHIPPED** | `7ca1f17`, `c1b8ede`（R2） |
| `capacity(N)` 連続容量関数（breadth×depth×KV） | **SHIPPED** | `11cc2c2`（R2） |
| DNODE_MAX 8 → 32（台数の天井上げ） | **SHIPPED** | `473387f` |
| 全網 situational-awareness map（world.c） | **SHIPPED** | `95ac916`（`arch/common/world.c`） |
| Android UMP フリート（各インストール＝1ノード、region-aware） | **SHIPPED（進行中）** | `f2d7dc1`, `de09eef`（Phase D） |
| regions 設計（空間） | **DESIGNED** | [[regions.md]] |
| survival-network 構想（why / 思想の核） | **DESIGNED**（思想は確定） | [[survival-network.md]] |
| reflex-deliberation 二層・時定数分離（時間） | **DESIGNED** | [[reflex-deliberation.md]]（DESIGN ONLY） |
| p-fs P0（sha256 移植・content-addressed store・重複排除） | **DESIGNED（進行中）** | [[p-fs.md]] §5 P0 |
| p-fs P1–P4（region-scoped 複製・履歴 DAG・分散ルックアップ・符号化） | **DESIGNED** | [[p-fs.md]] §5 |
| §7 分散ゲーティング（局所勾配でルーティング） | **DESIGNED（着手前）** | survival §7 / §II-2、reflex §4.3 |
| §8 二層 damping（ヒステリシス・別 tick・EWMA 平滑） | **DESIGNED** | reflex §4 / §6 D1–D2 |
| 同時多発・並行分散（§5）のシミュレーション（中央なし検証） | **DESIGNED（着手前）** | survival §10 ステップA / reflex D0 |
| decentralized-lookup（探索の共有基盤） | **DESIGNED**（並行執筆中） | `decentralized-lookup.md`（着地予定） |
| R3 model widening（網を太らせる） | **OPEN / DESIGNED**（並行執筆中） | `r3-model-widening.md`（着地予定）、regions R3 |
| 重みの provisioning（join 時にどこから重みを取るか） | **OPEN** | regions §6-1 |
| 反射と熟慮の和解（上書き・抑制の権限を分散したまま） | **OPEN** | reflex §7-1, §7-2 |
| 分散 GC（履歴の安全な破棄） | **OPEN** | p-fs §6.3 |
| region 間の信頼（HMAC はリンク単位／ref 署名） | **OPEN** | regions §6-4, p-fs §6.4 |
| 消失訂正符号 churn 下のシャード再配置 | **OPEN** | p-fs §6.2 |

> 一言で: **空間の配管（region / RTT / scope / capacity / world map / DNODE 32）はほぼ通った。**
> 欠けているのは **時間軸（二層の時定数分離・ゲートの damping）**、**探索の中央なし化**（decentralized-lookup）、
> そして **水量（R3 で網を太らせ、分散を必然にする）**。

---

## 5. 次の一手（クリティカルパス）

何が最も多くを解錠するか。3本が絡み合うが、順序がある。

1. **§7 ゲーティング damping（reflex D0 → D1）— 安定性の前提条件。最優先。**
   現状の `select_expert()` は瞬間 utility の最大を取るだけで、§7 を素朴に局所勾配化すると
   **必ず発振する**（reflex §4.1: 殺到→スパイク→一斉退避→再殺到の ringing）。まず
   ホスト純シミュレーション（survival §10 ステップA = reflex D0）で**発振を目で見る**、
   次に反射ゲートにヒステリシス／デッドバンド／EWMA 平滑（reflex §4.3, D1）を入れて消す。
   これは最適化ではなく**制御安定性**で、ゲーティング作業すべての前提。カーネル本体に触る前に
   シミュレーションで原理を確認する（survival §10 優先度所見）。

2. **§8 二層の明示分離（reflex D2）— 1 の構造的な裏付け。**
   反射ループ（REGION・速い tick）と熟慮ループ（GLOBAL・遅い tick）を**別周期**で回し、
   熟慮が反射のスパイクを観測しない（ローパス）ことを確認する。配管（REGION/GLOBAL/rsum/world/
   capacity）は既に SHIPPED なので、**時定数を別々に与えるだけ**が残作業。

3. **R3 model widening（`r3-model-widening.md`）— 分散を「必然」にする水量。**
   R0–R2 と p-fs P0–P2 は玩具モデルでも正しく作れるが、635 param の今は **1台に収まり**、
   区切る意味が出ない（regions §5 の鶏卵問題）。網を太らせて初めて region 分割・容量スケール・
   §7 ゲーティングが**机上でなく現実の制約**になる。Phase D（Android フリート）が現実の churn を供給する。

4. **p-fs P1（region-scoped 複製）— 記憶を Collective 層へ橋渡し。**
   「最後の1ノードで記憶が残る」をファイルにも効かせる save==publish の第一歩。
   swim/region/kdds が SHIPPED なので依存は満たされている。P0 進行中の次の自然な一段。

> 推奨する波: **(1)+(2) を一緒に**（時間軸を入れる）→ **(4)**（記憶を分散へ）→ **(3)**（水量を上げて
> すべてに意味を与える）。1+2 は配管が揃っており実装コストが小さく、それ無しでは §7 のどんな実装も
> 壊れるため、ここが最大の解錠点。

---

> 各器官は別の生き物ではない。**同じ脳の、別の軸**である。
> region は半球、反射と熟慮は別の時定数、p-fs は記憶——どれも中央を持たないという
> 一行で繋がっている。迷ったら [[survival-network.md]] へ戻る。
