# 記憶→思考 — forward パスが p-fs を読む (Wave 8 ①)

## §9 への答え

第三のレビュー §9:「記憶がなければ考えられない、のに dtr の forward は
p-fs を一行も読まない」。`dtr save` / `dtr load` は重みのライフサイクル
(死と再生) であって、**思考中の記憶参照ではなかった**。保存するだけの
図書館は考える器官ではない。

この配線で、推論・評価の forward パスは softmax の直前に p-fs 由来の
engram (記憶痕跡) を参照し、その票を logits に加算する。p-fs に
`dtr/engrams` が無ければ票はゼロ — **記憶が本当に源である**ことは
コード上の保証であり、demo で測定もしている。

## 仕組み

```
dtr remember                         forward (dtr ret on)
  訓練セットから 32 サンプル          input → Embed → MHSA → FFN → Pool
  (クラス均等・等間隔) を選び、           → logits = W_cls·pool + b
  embedding+label を engram             → logits += Σ_{i∈top-k} α·gate·sim_i·onehot(label_i)
  ブロックとして p-fs に保存            → softmax
  (named ref "dtr/engrams")               ↑ engram は p-fs からロード
        │                                  (P1 レプリケーション + P2 ref
        └── gossip ──────────────────────── gossip で群れ全体に届く)
```

- **engram レイアウト** (`retrieval.h`, 全フィールド固定幅 +
  `_Static_assert`): 16 B ヘッダ (magic/version/count/dim/out_dim) +
  32 × 20 B エントリ (float[4] embedding + label) = **656 B、p-fs
  1 ブロック (4 KB) に収まる**。選択は決定的なので content-id は
  どの ABI でも一致する (aarch64 / x86_64 で確認: `8358b94b…`)。
- **embedding は重み非依存** (`input/127`)。重み依存の表現にすると
  未学習ノードでは類似度空間が崩れ、「群れの記憶だけで考える」(下の
  条件 b) が成立しない。
- **ブレンド** (`retrieval.c ret_blend`): L2 距離 top-k (k=3)、
  `sim = 1/(1+d²)`、`α = 2.0`、`gate = (1 - p_max)²` (p_max は票を
  入れる前の softmax 最大値)。gate は測定で選んだ: フラットな α は
  学習済みモデルの確信ある正答を 3/60 反転させた。二乗 gate は
  「重みが確信しているとき記憶は囁き、迷っているとき記憶が決める」。
- **訓練は素のまま**: `dtr train` / `dtr grad` 中は retrieval を強制
  OFF。重みは記憶を松葉杖にせず学ぶ。

## 魂の測定 (`samples/11_distributed/run_memory_thought.sh`)

2 ノード (relay 経由)。node 1 が学習して `dtr remember`、node 2 は
**一度も学習しない**。engram は p-fs gossip で node 2 に届く。

| 条件 | train | held-out |
|---|---|---|
| (a) 重みのみ (node 1, ret off) | 95.0% | 100.0% |
| 　　乱数重み (node 2, ret off) | 26.7% | 26.7% |
| (b) **記憶のみ** (node 2, ret ON) | **93.8%** | **93.3%** |
| (c) 重み+記憶 (node 1, ret ON) | 96.3% | 100.0% |

- (b): 未学習の脳が chance (33%) を 60 pt 上回る。出どころは p-fs の
  656 B だけ — **群れの記憶で考える**。
- (c) ≥ (a) を両 split で満たす。train +1.3 pt は記憶が境界サンプルの
  迷いを正した分。
- `dtr eval` は同じコマンドが [ret off] / [ret ON] を並記する —
  cherry-pick なし。スクリプトはアサート失敗で exit 非 0。

## 操作

```
dtr remember            # engram を p-fs へ (dtr save とは別物)
dtr ret on|off          # ブレンド切替 (default off)
dtr ret reload          # キャッシュ破棄 → p-fs から読み直し
dtr eval                # ret off/ON 両方の精度を並記
```

## 正直な限界

- 玩具データセット (4ch int8 センサ、3 クラス、合成)。kNN が強いのは
  タスクが入力空間でほぼ分離可能だから。実データでは embedding を
  学習表現にする必要があり、その場合 (b) の重み非依存性は別の機構
  (共有 encoder の配布など) が要る。
- engram は 4 KB 1 ブロック上限 (現在 656 B)。スケールさせるには
  複数ブロック + HRW シャーディングが次の段。
- pipeline 並列の遠隔 worker (生入力を持たない) では retrieval は
  掛からない (`run_cls_head(…, NULL, …)`)。
- gate=(1-p_max)² は「(c) が (a) を壊さない」ための設計判断であり、
  記憶が重みの確信を覆すべき状況 (重みが自信満々に間違う分布シフト)
  では保守的すぎる。survival-network §8 の二層ゲートに接続する余地。
