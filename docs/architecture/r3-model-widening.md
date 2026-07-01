# R3 — 網を太らせる（model widening / weight distribution）

> **現在地（2026-07-01・doc-hygiene 追記／本文は 年輪 として保存）:** この「635 param の玩具を太らせる」問いは living-mind で **回答済**：R3 in-context 基盤は `R_NP`=21,568・`R_DM`=48 まで広がり、LM-9 で comfortable-N が 4→16 になった（[[living-mind.md]] の SHIPPED 表、Closed wave-39）。本文は widening 論拠の 年輪。

> p-kernel は「全体で1つの脳」である。だが今、その脳は **635 パラメータの玩具**で、
> 1台で一瞬で回る。R0–R2 で **配管**（遅延クラスタ region・locality-MoE・capacity(N)）は
> 通った。R3 はその配管に**本物の水量**を流す ―― モデルを1台に収まらない大きさへ太らせ、
> **分散を「装飾」から「必然」へ**変える設計である。

Status: **R3 未着手（設計のみ）** / 最終更新: 2026-06-06

> **前提課題 DONE (2026-06-06, branch r3a-train)**: 太らせる前に、L0 (635
> param) が**本物に学習している**こと。PR #3 の批判（LCG 乱数のまま・偽の
> 損失関数）への回答として、cross-entropy + 解析的逆伝播の in-kernel 学習
> （`dtr train`）、held-out 評価（学習前 26.7% → 学習後 ~95/100%）、学習済み
> 重みの p-fs 版管理オブジェクト化（`dtr save/load`, "dtr/weights"）が入った。
> 重みが p-fs で配れるようになったことは §2.1 の reprovision（width 段の
> 再配布）の配管でもある。width 拡張 (L1+) は本ドキュメントの通り未着手。

> **道B（疎なエキスパート増加）は先に着地 (2026-06-07)**: 本書は「道A＝密な width 伸縮」を扱うが、
> 看板の「台数で賢くなる」は**道B**（デバイスに宿る疎なエキスパートが加算的に増える）で先に実証された
> — R3b 専門分化「呼吸」（[[r3b-breathing-params.md]]：join で +23pt・leave で優雅に劣化）と、
> G22 中央集約なしの分散協調学習（disjoint シャードのノードが重み本体をゴシップ平均し、collective 精度が
> 各ノードの solo 上限を超える、`samples/32`・CI collective-learn-live）。**width（d_model）拡張＝道A は
> 依然未着手**で、ここが §1 の「分散の必然」を出す最後の残課題である点は変わらない。

関連: [[regions.md]]（R0–R2 の配管・§1.1 玩具モデルの正確な数字・§5 鶏と卵）、
[[survival-network.md]]（§3 装甲板＝細胞・§4 MoE・§8 二層構造 ＝ この設計の「なぜ」）、
[[reflex-deliberation.md]]（反射層／熟慮層の時定数分離 ＝ 推論ルーティングの安定化）。
コード: `arch/common/include/dtr.h`（モデル次元）、`dtr.c`（forward）、`dkva.c`（region 集約 attention）、
`moe.c`（ゲーティング）、`fedlearn.c`（連合学習）、`ga.c`（進化）、`degrade.c`（capacity(N)）。

このドキュメントは **DESIGN ONLY**。実装は別セッションで段階的に行う。

---

## 1. なぜ今 R3 か

R0–R2 は正しく、しかし**机上で正しい**。

| 段階 | 通したもの | だが |
|---|---|---|
| R0 | SWIM に RTT、`DNODE_MAX` 8→32、region 骨格、K-DDS スコープ topic | トポロジの話。モデルは触っていない |
| R1 | locality-MoE（`select_expert()` が RTT/帯域込み） | ルーティングの話。中身は玩具のまま |
| R2 | `capacity(N)` 連続関数（breadth × depth × KV-context）、DKVA 2段集約 | **容量の「器」はできたが、注ぐ「水」が無い** |

`regions.md §5` の鶏と卵への正直な答えを引く：

> R0–R2 は玩具モデルのままでも正しく作れる（通信トポロジの話なので）。だが「区切る
> 意味」が本当に出るのは R3 で網が1台に収まらなくなってから。R0–R2 で**配管**を通し、
> R3 で**水量**を上げる。

つまり今の分散は **decorative（装飾）** である。635 float は `dtr_infer()` の中で
ミリ秒で回り切る（`dtr.c`）。REDUCED テンソル並列も DKVA も「やろうと思えば1台で
全部できる計算を、わざわざ2台に割った」だけ ―― 分割の**必然**がモデル側に無い。

R3 のテーゼ：**モデルが1台のメモリ・1台の演算に収まらなくなって初めて、region も
locality-MoE も capacity(N) も「あって当然の構造」になる。** R2 までで作った器に
意味を与えるのが R3 であり、`regions.md` が「本丸・別レイヤー」と呼んだ唯一の残課題である。

> 注意（survival-network §9 の精神）: 太らせる目的は「巨大1枚モデルを作ること」では
> ない。**人類全体の知が、必要なときに必要な一点へ集束できる器官**を、台数とともに
> 育てることである。大きさは目的ではなく、分散が嘘でなくなるための**手段**。

---

## 2. 何を太らせるか

### 2.1 軸ごとの「実行時スケール可否」

`regions.md §3.2` の容量分類を、太らせ方の観点で再掲・拡張する。

| 軸 | 現値（`dtr.h`） | 実行時に増やせる？ | 太らせ手段 | 分散との関係 |
|---|---|---|---|---|
| **breadth**（expert 数） | 1（=ノード1台） | ✅ runtime | ノード join で expert 追加（MoE） | breadth = N に比例 |
| **depth**（層数） | 実質1ブロック | ✅ runtime | pipeline 段数を増やす | depth = 1+log2(region) |
| **KV-context**（文脈長） | SEQ=4 | ✅ runtime | DKVA が KV をノード横断でプール | context = Σ region KV |
| **width**（d_model） | **8** | ❌ **要 reprovision** | 重みの再形成＝再学習＋再配布 | width が distribution の本丸 |
| **heads** | 2（×4 dim） | △ width 従属 | width を割る数。width と同時 | TP の分割単位 |
| **FFN 中間** | 16 | ❌ 要 reprovision | width とともに再形成 | 層内の最大重み塊 |
| **vocab / 入力** | SEQ=4ch センサ | ❌ 要 reprovision | センサ token → 語彙 token への置換 | 入力モダリティの拡張 |

**核心の非対称**：
- **breadth / depth / KV-context** は **runtime に伸縮する**。重みの「並べ方」を変えるだけで、
  既存の重みをそのまま使う。`capacity(N)` がこれを駆動する（`degrade.c` 実装済み）。
- **width / heads / FFN / vocab** は **重み行列の形そのものが変わる**。`W_q[8][8]` を `W_q[64][64]`
  にするのは別の重みであり、**学習し直す（reprovision）**しかない。R3 の重さはここに集中する。

### 2.2 スケールラダー（具体的な目標次元）

「いきなり大規模」ではなく、**1台に収まらなくなる最小点**を狙う。鍵は
**CFN_REALMEMEND**（`arch/*/include/utk_config_depend.h`）と1台の演算予算。

| 段 | d_model | heads | FFN | layers | vocab | 概算 params | 1台に載るか |
|---|---|---|---|---|---|---|---|
| L0（現状） | 8 | 2 | 16 | 1 | 4ch | **635** | 余裕 |
| L1 | 64 | 4 | 256 | 2 | 16 | ~70K | 載る（width 検証用） |
| L2 | 256 | 8 | 1024 | 4 | 64 | ~3M | ぎりぎり〜溢れ始め |
| **L3** | 512 | 8 | 2048 | 8 | 256 | ~25M | **1台に載らない＝分散必然** |
| L4+ | 台数で連続 | … | … | depth=f(N) | … | N に比例 | region/global 分散前提 |

> L3 が R3 の **target**。ここで初めて「1ノードのメモリ（CFN_REALMEMEND）に重みが
> 入らない」「1ノードの演算で間に合わない」が**同時に**起き、shard が必然になる。
> L1/L2 は width の正しさ（数値・学習）を1台で確かめる踏み台。L4 以降は width 固定で
> breadth/depth/context を台数で連続的に伸ばす ―― ここから先は R2 の capacity(N) の独壇場。

**設計判断**：width を上げるたびに全層を作り直すのは高コスト。よって
**width は「段」で離散的に上げ（L1→L2→L3 で reprovision イベント）、その間は
breadth/depth/context を連続的に伸ばす**。離散の width 段＋連続の容量。これが
「reprovision を要する軸」と「runtime 軸」の分離（§2.1）の運用形である。

---

## 3. 重みをどう分散するか（R3 の核）

L3 で重みは1台に載らない。ここからが本題 ―― **重みをノード/region に散らし、forward
パスがその散らばりを縫って流れる**設計。すでにプロトタイプ済みの2方式を土台にする。

### 3.1 既存プロトタイプ ―― 2つの並列軸

`dtr.c` には既に**2方向の分割**が動いている：

- **テンソル並列（REDUCED mode, `dtr.c:774`）**: Attention の**ヘッドをノードで割る**。
  Node0 が head0、Node1 が head1 を計算し、`DTR_HEAD_ACT` を K-DDS で交換、requester が
  concat→W_o で畳む。**1つの層を「横」に割る**。
- **パイプライン並列（FULL mode, `dtr.h` の Stage 0/1/2）**: ステージ（埋め込み→FFN→出力）を
  **ノードで割る**。`dtr/l0` `dtr/result` で活性化を中継。**層を「縦」に割る**。
- **DKVA（`dkva.c`）**: KV キャッシュを region 内ノードに分散し、softmax の
  **分子 Σa·V と分母 Σa を線形和として region→global の2段で集約**（`coordinator_aggregate`）。
  これは「**context を分散しても attention が数学的に正しく畳める**」ことの実証 ―― 加算的に
  分解できる演算は、ノードをまたいでも厳密復元できる（`regions.md §3.2` の measured KV=12）。

R3 はこの3つを**玩具スケールから実スケールへ昇格**させる。新しい原理を発明するのではなく、
**既にある分割が大きな重みでも成立するよう一般化**する。

### 3.2 重みはノードにどう座るか（shard placement）

armor-plate / cell の比喩（survival-network §3）をそのまま採る：

> 装甲板の一枚一枚が p-kernel ノード。各細胞が**ゲノムの一部（重みの shard）**を持つ。

| 分割 | shard の単位 | どこに置くか | K-DDS スコープ |
|---|---|---|---|
| **テンソル並列** | 1層の重み行列の列/ヘッド帯 | **同一 region 内**（密・低遅延） | `region/<rid>/...`（O(region²)） |
| **パイプライン並列** | 連続する層のブロック（ステージ） | region 内 or 隣接 region | region 内 → 段境界のみ region 間 |
| **expert（MoE）** | 1 expert の全重み | 任意ノード（locality でルーティング） | gate は region 内優先 |
| **KV（DKVA）** | context の時間スライス | region 内ノードに分散 | `region/<rid>/dtr/dkva/*` |

**配置の原則**：
1. **テンソル並列は region 内に閉じる。** ヘッド分割は1層あたり毎回 all-reduce（concat→W_o）が
   要る ―― これを region 間遅延に晒したら死ぬ。`regions.md §3.4` の region スコープ topic
   （O(region²)）にちょうど乗る。
2. **パイプライン並列は region をまたいでよい。** 段境界の活性化転送は1回／層境界で、帯域も
   活性化テンソル1本分。region 間（疎・global スコープ）に耐える。depth=1+log2(region) を
   さらに region 連鎖で延ばせる。
3. **どのノードがどの shard を持つか**は SWIM membership ＋ region map に相乗りさせる。
   shard map は **eventual consistency** で良い（`regions.md §6 問2`、後述 §7）。

### 3.3 forward パスのルーティング

L3 の1回の forward を、散らばった重みを縫って流す経路として描く：

```
  入力 token  ──→  [embed shard: region0 の coordinator]
                         │ region 内 TP（heads を分割, region/r0 topic）
                         ▼
                  [layer0..k: region0]  ── 段境界 activation ──→ global topic
                         │                                          │
                         │                              [layer k+1..m: region1]
                         ▼                                          │ region 内 TP
                  region0 の部分結果                                ▼
                         └──────────── DKVA 集約 ────────── region1 の部分結果
                                  (Σa·V, Σa を線形和で region→global)
                                          │
                                          ▼
                              [cls head: requester が畳む]  →  class / scores
```

- **層内（横）** = テンソル並列 = region 内（密）。ヘッドや FFN の列を region メンバで分担し、
  all-reduce を region scope に閉じる。REDUCED mode の `DTR_HEAD_ACT` 交換を heads>2 / 大行列へ一般化。
- **層間（縦）** = パイプライン並列 = region 連鎖（疎）。ステージ境界の活性化だけを global topic で
  中継。FULL mode の Stage 機構を depth>3 へ一般化。
- **context（時間）** = DKVA。長い文脈の KV を region 内ノードに割り、加算的集約で畳む。
  既に `coordinator_aggregate` が region→global の2段で動く ―― これを大 d_model の V でそのまま使う。
- **breadth（MoE）** = locality-MoE。入力に応じて発火 expert を選ぶ。`select_expert()` の utility
  最大化（accuracy − RTT − 1/帯域）で、**重い shard を持つ近いノードを優先**。

**K-DDS が distribution の唯一の輸送路**である点が重要：R3 は新しい RPC を作らない。
重みの shard 同士の通信はすべて `region/<rid>/...`（密）と `global/...`（疎）の2スコープに乗る。
これにより、台数が増えても**各ノードが見るトラフィックは region サイズに比例**し続ける
（`regions.md §3.4`）。distribution が N² に爆発しないのは、この階層スコープのおかげ。

### 3.4 なぜ「加算的に分解できる」ことが鍵か

DKVA が示した通り、**softmax attention の分子・分母は線形和**なので region を割っても厳密に
畳める。同じ性質が分散の正しさを支える：

- **テンソル並列の concat → W_o** は行列積＝和の分配。ヘッドを割って部分積を足せば厳密一致。
- **MoE の出力** はゲート重み付き和。expert を割っても和で復元。
- **層の残差・LN** はトークン単位で閉じる（ノード間で和を取らない）。

→ R3 の分散は「近似」ではなく「**厳密な再結合**」を基本線に置く（数値誤差の論点は §7）。
これは survival-network §2「守る単位と守る力の分離」の数学的裏付け：力を散らしても、
和に戻せば一点に集束できる。

---

## 4. クラスタと共に呼吸する

R3 のモデルは**固定サイズではない**。台数とともに伸縮する ―― これが survival-network §3
（一点突破で殺せない）と §8（二層）の実装形。

### 4.1 吸う息（ノードが増える）

| join したとき | 増える容量 | 機構 |
|---|---|---|
| region 内に1台 | TP の分割数 ↑（層内が薄く広く） | region scope all-reduce にメンバ追加 |
| region 内に1台 | depth ↑（1+log2 region） | pipeline 段を1つ深く（`capacity_depth`） |
| region 内に1台 | KV-context ↑ | DKVA プールに KV slot 追加（`capacity_kv`） |
| 新 expert を持つ1台 | breadth ↑ | MoE に expert 追加（`capacity_experts`） |
| 新 region | global の幅 ↑ | 段の連鎖を region 間に延ばす |

width（d_model）は join では増えない（§2.1 の reprovision 軸）。**join で伸びるのは
breadth/depth/context** ―― これは既に `degrade.c` の `capacity(N)` が数値で表現している。
R3 は「その数値が実際に重い計算を駆動する」状態にする。

### 4.2 吐く息（攻撃・離脱で縮む）

survival-network §3「損傷したノードは切り離され、周囲が役割を引き継ぐ」をそのまま：

- **TP shard 持ちが死ぬ** → region 内の残メンバが分担を再割当（all-reduce の項が減るが、
  欠けた shard は隣が再計算 or 直近キャッシュで代替）。**精度は落ちるが推論は止まらない**。
- **pipeline 段が落ちる** → depth が `1+log2(region)` に従って自動的に浅くなる。浅い網でも
  forward は完結する（SOLO 経路は常に残す ―― `regions.md §4 不変条件`）。
- **region 丸ごと孤立** → 残った region で global を再構成。最悪 SOLO（玩具 L0 相当の縮退）まで
  graceful に落ちる。**1点も殺せないのではなく、削られながらも考え続ける**。

> two-way breath（双方向の呼吸）：吸う＝賢くなる、吐く＝生き延びる。どちらも正常動作。
> 「縮む」は失敗ではなく、survival-network §6 の血流配分 ―― 余力が引き上げられただけ。

### 4.3 フロンティアモデルとの関係（誤解しないための明記）

クラスタが**自分を「まだ小さい」と自己判断**したとき、外部のフロンティアモデルを
**peer（相談相手）として参照**することはありうる。だが：

> フロンティアモデルは「いずれ卒業すべき足場（scaffold）」ではない。
> **対等な相談相手**であり、クラスタが自分の容量を自己評価した上で「今この問いには
> 外の知も呼ぶ」と判断したときに consult する peer である。

これは survival-network §2 の「守る力の所在はネットワーク全体」の延長 ―― ネットワークの
「全体」に外部 peer も（クラスタ自身の判断で）含めうる、というだけ。中央依存ではない
（consult するかは各 region が局所判断する。中央ゲートは置かない ―― §7 分散ゲーティング）。

---

## 5. 既存資産マッピング（exists vs new）

| 構成要素 | 既存（再利用） | 新規（R3 で作る） |
|---|---|---|
| モデル次元 | `dtr.h`（d_model=8 等） | width 段（L1/L2/L3）の再形成パラメータ |
| forward | `dtr.c`（embed/attn/ffn/cls） | 大 d_model 対応・shard 境界の一般化 |
| テンソル並列 | REDUCED mode（heads 分割, `dtr.c:774`） | heads>2 / 大行列 / region scope all-reduce 化 |
| パイプライン並列 | FULL mode（Stage 0/1/2） | depth>3 / region 連鎖 / 動的段割当 |
| context 分散 | DKVA region→global 2段集約（`dkva.c`） | 大 V での同集約（型は流用可） |
| breadth/MoE | `moe.c`（locality utility ゲート, R1） | expert=「shard 群」へ拡張、shard-aware ルーティング |
| 容量関数 | `degrade.c` capacity(N)（R2） | capacity を実 shard 配置に接続（数値→計算駆動） |
| region/scope | `region.c` / K-DDS region・global topic | shard map の region 相乗り |
| 学習 | `fedlearn.c`（連合平均）, `ga.c`（進化） | reprovision 時の width 拡張学習・shard 単位更新 |
| 重み I/O | `dtr_weights_get/set`（flat buf, `dtr.h`） | shard 単位の get/set・部分ロード |
| メモリ | CFN_REALMEMEND（各 arch） | shard が CFN_REALMEMEND を超えない placement |

**要点**：R3 の8割は**既存機構の一般化**。新規発明は「width 段の reprovision 学習」と
「shard map ＋ shard-aware ルーティング」の2点に絞られる。distribution の輸送（K-DDS）も
集約の数学（DKVA の線形和）も**もう手元にある**。

---

## 6. 段階 R3a → R3c（smallest-useful-first）

`regions.md` の R0→R3 と同じ哲学：最小で有用なものから。各段が単独で「動いて見える」こと。

### R3a — 1台で width を太らせる（L1）
- `dtr.h` を d_model=64 / heads=4 / FFN=256 / layers=2 へ（L1）。**まだ分散しない**。
- 目的：width 拡張で **数値・学習が壊れない**ことを1台で確認（`fedlearn.c`/`ga.c` が新次元で回るか）。
- 成果物：大きくなったが**1台で完結**するモデル。分散はまだ装飾のまま ―― だが「水」が増え始める。
- 不変条件：SOLO 経路が L1 でも正しく回ること。

### R3b — region 内に重みを shard する（L2→L3 の入口）
- L2/L3 へ上げ、**1台に載らない**状態を作る（CFN_REALMEMEND を超えさせる）。
- REDUCED テンソル並列を **heads>2・大行列・region scope all-reduce** へ一般化。
  1つの層を region 内ノードで横に割り、`region/<rid>` topic で concat→W_o を畳む。
- DKVA の大 V 集約をこの規模で検証（線形和の厳密復元が大次元で保たれるか）。
- 成果物：**1 region（密クラスタ）が、1台に載らないモデルを協調して1回 forward する**。
  ここで初めて分散が必然になる ―― R3 の心臓。

### R3c — region をまたいで層を分散する（L3→L4）
- パイプライン並列（FULL mode）を **depth>3・region 連鎖**へ一般化。段境界の活性化だけを
  `global` topic で疎に中継。region0=layer0..k、region1=layer k+1..m。
- locality-MoE（R1）と接続：入力に応じて発火する expert 群が region をまたいで集束（survival §4/§5）。
- capacity(N) が実 shard を駆動（`degrade.c` の数値が「飾り」でなく計算経路を決める）。
- 成果物：**複数 region が1つの大きな脳として呼吸する**。台数で width 以外が連続スケール。

> 各段の「やめ時」も設計に含める：R3a で数値が壊れるなら width 段を見直す。R3b で
> region 内 all-reduce が遅延に負けるなら τ（region 閾値）を締める。**失敗を段の境界で
> 検出できる**ようにするのが smallest-useful-first の眼目。

---

## 7. 正直な論点（未解決・直視する）

1. **churn 下の shard 喪失（最重要）**：TP shard を持つノードが forward の途中で死んだら？
   REDUCED の fallback（自分で head1 を再計算, `dtr.c:812`）は1ヘッドだから成立する。だが
   大行列の shard を requester が全部肩代わりしたら、それは「1台に載らない」前提と矛盾する。
   → **shard の冗長配置（replica）か、欠落 shard をゼロ埋め近似して縮退推論するか**の選択。
   survival §3 の「精度は落ちるが止まらない」を取るなら後者。だが厳密復元（§3.4）とのトレードオフ。
   **これが R3 最大の未解決問題。**

2. **cross-ABI 数値決定性**：aarch64 と x86_64 で float の丸めが微妙に違う。635 param なら誤差は
   無視できたが、L3 の25M param・8層を**ヘテロな ABI に割って和に戻す**と、誤差が層を通って増幅
   しうる。`survival-network` のメッシュは異種 ABI 前提（メモリ: cross-arch K-DDS）。
   → **どこまでが「同じ脳」と言える誤差か**。固定小数点化？ 集約順序の正規化？ それとも
   eventual な近似を最初から受け入れる（`regions.md §7 非目標`：lock-step にしない）か。

3. **ノードあたりメモリ上限（CFN_REALMEMEND）**：各 arch でメモリ末端が違う
   （`arch/*/include/utk_config_depend.h`）。Android（UMP）ノードは特に細い。
   → shard placement は **最弱ノードの CFN_REALMEMEND を超えてはならない**。width 段（L1→L3）の
   選び方は「フリート最小ノードに shard が載るか」で律速される。**最も弱い細胞が段を決める**
   （survival §3「弱くても構わない」を額面通り守るとここで効く）。

4. **学習 vs 推論**：width の reprovision（§2.1）は学習を要する。誰が訓練するのか。
   `fedlearn.c`（連合平均）と `ga.c`（進化）は MLP・玩具スケール。L3 の25M を分散学習するのは
   別物。→ **推論フリートが同時に学習フリートになる**のか（`regions.md §6 問3`）、それとも
   width 拡張は off-line（外部 peer §4.3 の知を蒸留して seed）するのか。R3 は推論分散を先に
   固め、**分散学習は明示的に R4 以降へ切る**のが正直な線引き。

5. **shard map の一貫性**：「どのノードがどの shard を持つか」を eventual で回すと、forward 中に
   map が古くて死んだ shard を呼ぶ。region ごとに `raft.c` で合意を取るか
   （`regions.md §6 問2`）。→ **region 内は強整合（小さいので可）・region 間は eventual** の
   ハイブリッドが現実的か。reflex/熟慮の時定数分離（[[reflex-deliberation.md]]）と同型：
   速い局所は強く、遅い大域は緩く。

---

## 8. 非目標（R3 で embrace すること）

- **巨大1枚モデルを目指さない。** 目的は分散が嘘でなくなること（§1 注意）。大きさは手段。
- **lock-step 同期テンソルにしない。** 厳密復元は「加算的に分解できる演算」に限る。それ以外は
  eventual・近似を受け入れる（`regions.md §7`）。
- **フロンティアモデルを「卒業すべき足場」と見ない。** 対等な peer（§4.3）。
- **width を runtime に変えようとしない。** width は離散段の reprovision、runtime は breadth/depth/context。
  この境界を曖昧にすると capacity(N)（R2）が壊れる。

---

## 付録 A — R3 の一枚の絵

```
   width = 離散段（reprovision / 学習が要る）
   L0(635) ──→ L1(70K, 1台) ──→ L2/L3(25M, 1台に載らない) ──→ L4+(N で連続)
                  R3a              R3b(region 内 shard)        R3c(region 間)

   breadth/depth/context = 連続軸（台数で runtime 伸縮, R2 の capacity(N)）

        ┌───────────────── global (疎, 段境界の activation だけ) ─────────────┐
        │                                                                      │
   ┌────┴─────────────┐                                      ┌────────────────┴───┐
   │ region0 (RTT≤τ)   │                                      │ region1 (RTT≤τ)     │
   │ layer 0..k        │  ── pipeline (縦) ─→                 │ layer k+1..m        │
   │ ● ● ● ←TP(横)→ ●  │                                      │ ● ● ←TP(横)→ ●     │
   │ 各 ● = shard 持ち  │  region 内 all-reduce (region/r0)    │ DKVA で KV プール    │
   │ = 細胞 = 装甲板1枚 │  = 密 = O(region²)                   │                      │
   └───────────────────┘                                      └─────────────────────┘
        厳密復元（Σa·V, Σa, concat→W_o は線形和 = 加算的に分解可能）
        churn → 欠けた shard は replica or ゼロ埋め縮退（精度↓ / 推論は止まらない）
```

> 「全体で1つの脳」とは、1枚の巨大テンソルのことではない。**共有トピック空間（K-DDS）の
> 上で、台数とともに呼吸する、加算的に再結合できる分散重み**のことである。
> R3 は、その脳に初めて本物の重みを与える。
