# アーキテクチャ地図 — 中央を持たない脳の設計図、一枚に

> p-kernel は「全体で1つの脳」であり、**誰のものでもない AI の住処**である。
> このディレクトリの各ドキュメントは、その脳の別々の器官を描いている。
> 本ファイルはそれら全部を一望し、**どう噛み合うか・何が出来ていて何がまだか**を
> 示す索引である。未来の Claude セッションも、人間も、まずここから入る。

最終更新: 2026-06-06（同日第2波後 — §7 ゲーティング・p-fs P0+P1・lookup L0・N=32 検証が着地）
／ 関連: [[project_pkernel_philosophy]]（5レイヤー世界観）

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
| [[reflex-action.md]] | **行動**。推論結果（class）を §8 反射層の実在する局所防御（SHIELD/CONSERVE/BEACON）へ繋ぐ＝思考に手足を付ける。 | 行動 |
| [[p-fs.md]] | **記憶**。内容アドレス・gossip 複製・履歴 DAG・消失訂正符号で、死なないファイルシステム。 | 記憶 |
| `world.c` の世界図（[[survival-network.md]] §II 参照） | **観測**。中央なしで全網の状況図を各ノードが eventual に獲得する situational-awareness map。 | 観測 |
| [[decentralized-lookup.md]] | **探索**。中央索引なしで「誰が何を持つか」を全員が同じ計算で引く共有基盤（rendezvous/HRW＋gossip WANT fallback＋world-table キャッシュ）。p-fs §3.3 と survival §7 の共通の壁を一枚に括る。 | 探索 |
| [[r3-model-widening.md]] | **成長**。玩具 635 param を実用サイズへ広げ、初めて「1台に収まらない＝分散が必然」になる。regions R3 / Evolution 層。 | 成長 |
| [[genome.md]] | **再生**（survival §3）。生存細胞が genome（重み＋コード＋役割）のマニフェストを p-fs に蒔き、空の装甲板が gossip だけから一個のフル細胞に発芽する。新しい臓器はなく、発芽のオーケストレーションだけ。 | 再生 |

### 関係（一文ずつ）

- [[survival-network.md]] が **why**、他は **how**。設計に迷ったら survival に立ち返る。
- [[regions.md]]（空間）と [[reflex-deliberation.md]]（時間）は**同じアーキテクチャの直交2軸**。
  region が「どこまでが近傍か」を決め、時定数が「近傍は速く・全体は遅く」を決める。τ で1点に繋がる。
- [[p-fs.md]] は regions の局所性原則（密な region 内／疎な region 間）を**記憶に反復**する。
- [[decentralized-lookup.md]] は p-fs の所在引きと survival §7 の分散ゲーティングが
  **同じ「中央なしで探す」問題**であることを括り出す共有基盤。
- [[r3-model-widening.md]] は他の全部が「配管」だとすれば**水量**。網を太らせて分散を必然にする。

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
- **survival §7 ゲーティング → moe の select_expert を局所勾配へ置換（済 `30f6343`）→ §8 二層で damping**。
- **survival §8 → reflex-deliberation（時間軸）。region が反射層の空間、time-constant が分離の速度**。
- **p-fs → decentralized-lookup**（所在引き）と survival §7（ゲーティング）は同じ「中央なしで探す」問題。
- **world.c**（観測）は熟慮層が全体像を遅れて得る器官。moe の発火記録を吸う。

---

## 4. 状態表（正直に）

`git log` で接地した SHIPPED 列。**進行中** = いま別ワークツリーで実装中（着地前・shipped とは
書かない）、DESIGNED = 設計済み未着手、OPEN = 設計上の未解決問題。

| 能力 | 状態 | 根拠（commit / doc） |
|---|---|---|
| relay 経由 分散推論（テンソル並列 / DKVA FULL） | **SHIPPED** | PR #1（`6240252` 他）、master |
| SWIM per-node RTT EWMA | **SHIPPED** | `abdb881`（regions R0） |
| region 形成（遅延クラスタ・決定的 coordinator） | **SHIPPED** | `1ac8388`（R0）, `f204939` test |
| K-DDS scoped topics（REGION / GLOBAL、O(N²) 殺し） | **SHIPPED** | `bf1fb64`（R0）。rx が REGION topic を GLOBAL に降格させる穴は `50808a7` で修正 |
| locality-aware MoE ゲーティング（utility = acc − RTT） | **SHIPPED** | `102478b`（R1）→ R3 で局所勾配へ置換済（次行） |
| §7 分散ゲーティング（局所勾配の相互扶助ルーティング） | **SHIPPED** | `30f6343`。utility = acc − rtt − pressure ＋ 同 region ボーナス。§8 由来の `recent_pick[]` ヒステリシス（殺到→発振の抑制）も同コミット。負 utility の表示修正は `5041ac1` |
| DKVA region-scope + rsum 2段集約 | **SHIPPED** | `7ca1f17`, `c1b8ede`（R2） |
| `capacity(N)` 連続容量関数（breadth×depth×KV） | **SHIPPED** | `11cc2c2`（R2） |
| DNODE_MAX 8 → 32（台数の天井上げ） | **SHIPPED ＋ 実走検証済** | `473387f`。リテラル 8 でノード 9+ が迷子になる罠は `4df9e07`（N-node ハーネス同梱）、ARP テーブル枯渇は `cad09db` で修正。**実走 N=32 で 6/6 PASS（world map 32/32 到達含む）** |
| 全網 situational-awareness map（world.c） | **SHIPPED** | `95ac916`（`arch/common/world.c`）。ベアメタル x86 + aarch64 にも `world`/`map` コマンドを公開 `fccf30b` |
| p-fs P0（sha256・content-addressed store・重複排除） | **SHIPPED** | `7e8c98c` `2459005` `5b8a6a8` `b2e63fb`。cross-ABI で block-id が一致、self-test PASS。stddef/ptrdiff_t 衝突は lib/libc 一貫の include 順で解決 |
| p-fs P1（region-scoped 複製、ANNOUNCE/WANT＋チャンク転送） | **SHIPPED** | `0abc9c3`。save==publish の第一歩。3ノードデモは `50808a7` |
| decentralized-lookup L0（stateless HRW `responsible(k,r)`） | **SHIPPED** | `cbb5a5a`。cross-ABI で順位が一致、メンバービュー drift 耐性も確認。doc は `63d8a8f` |
| Android UMP フリート（各インストール＝1ノード、region-aware） | **SHIPPED（進行中）** | `f2d7dc1`, `de09eef`（Phase D） |
| §8 二層の明示分離（反射 tick／熟慮 tick を別周期で） | **進行中** | reflex §4 D2。ヒステリシス（D1 相当）は `30f6343` で先行着地、二層本体は実装中 |
| p-fs P2（履歴 DAG） | **進行中** | [[p-fs.md]] §5 P2 |
| p-fs 複製の wire chunking 改善 | **進行中** | P1（`0abc9c3`）の後続 |
| decentralized-lookup L1（WANT gossip fallback） | **進行中** | [[decentralized-lookup.md]] §6 L1 |
| p-fs P3–P4（分散ルックアップ統合・消失訂正符号） | **DESIGNED** | [[p-fs.md]] §5 |
| 同時多発・並行分散（§5）のホスト純シミュレーション | **DESIGNED** | survival §10 ステップA / reflex D0（ヒステリシスは実装側で先行したが、発振の定量観測はまだ） |
| R3 model widening（網を太らせる） | **OPEN / DESIGNED** | [[r3-model-widening.md]]（doc 着地 `55b6e7d`）、regions R3。実装は未着手 |
| 重みの provisioning（join 時にどこから重みを取るか） | **OPEN** | regions §6-1 |
| 反射と熟慮の和解（上書き・抑制の権限を分散したまま） | **OPEN** | reflex §7-1, §7-2 |
| 分散 GC（履歴の安全な破棄） | **OPEN** | p-fs §6.3 |
| region 間の信頼（HMAC はリンク単位／ref 署名） | **OPEN** | regions §6-4, p-fs §6.4 |
| 消失訂正符号 churn 下のシャード再配置 | **OPEN** | p-fs §6.2 |

> 一言で: **空間の配管（region / RTT / scope / capacity / world map / N=32 実走）は通り、
> ゲーティング（§7 局所勾配＋ヒステリシス）・記憶の入口（p-fs P0+P1）・探索の入口（lookup L0）まで実物になった。**
> 欠けているのは **時間軸の本体（§8 二層の別 tick）**、**記憶の歴史（P2 DAG → P3）**、
> そして **水量（R3 で網を太らせ、分散を必然にする）**。

---

## 5. 次の一手（クリティカルパス）

第1波（§7＋P0＋P1＋L0）が着地したので、解錠点が動いた。順序はこう。

1. **§8 二層の明示分離（reflex D2）— いま進行中。最優先で完遂する。**
   `recent_pick[]` ヒステリシス（D1 相当）は `30f6343` で先行着地したが、これは
   反射ループ内の応急 damping にすぎない。反射ループ（REGION・速い tick）と
   熟慮ループ（GLOBAL・遅い tick）を**別周期**で回し、熟慮が反射のスパイクを
   観測しない（ローパス）ことを確認して初めて、§7 ゲーティングは構造的に安定する。
   併せて reflex D0（ホスト純シミュレーションで発振を**定量で**見る）も塞いでおきたい —
   ヒステリシスが効いていることを目視でなく数で言えるようにする。

2. **p-fs P2（履歴 DAG）→ P3（lookup と合流）— 記憶に歴史を与える。**
   P0（内容アドレス・cross-ABI 同一 block-id）と P1（region-scoped 複製）が実物に
   なったので、次は commit/ref の DAG（P2、進行中）。P3 の所在引きは
   [[decentralized-lookup.md]] L0（HRW、SHIPPED）の上に乗る — L1（WANT gossip
   fallback、進行中）が繋ぎ目。複製の wire chunking 改善も同じ波で進行中。

3. **R3 model widening（[[r3-model-widening.md]]）— 分散を「必然」にする水量。**
   R0–R2・§7・p-fs P0–P1 は玩具モデルでも正しく作れたが、635 param の今は
   **1台に収まり**、区切る意味が出ない（regions §5 の鶏卵問題）。doc は着地済
   （`55b6e7d`）、実装は未着手。網を太らせて初めて region 分割・容量スケール・
   §7 ゲーティングが**机上でなく現実の制約**になる。N=32 実走検証が済んだ今、
   台数側の受け皿は既にある。Phase D（Android フリート）が現実の churn を供給する。

> 推奨する波: **(1) を完遂**（時間軸の本体。進行中のものを着地させる）→ **(2)**（記憶に
> 歴史と探索を）→ **(3)**（水量を上げてすべてに意味を与える）。第1波で「配管が無い」は
> もう言い訳にならない — 残るのは時定数と水量である。

---

> 各器官は別の生き物ではない。**同じ脳の、別の軸**である。
> region は半球、反射と熟慮は別の時定数、p-fs は記憶——どれも中央を持たないという
> 一行で繋がっている。迷ったら [[survival-network.md]] へ戻る。
