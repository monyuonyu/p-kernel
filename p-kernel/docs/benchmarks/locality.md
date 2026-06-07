# Locality benchmark — putting a number on §4

最終更新: 2026-06-07 ／ wave-12 F隊 ／ 俯瞰監査 v3 **G25**（監査の死角）への回答
関連: [survival-network.md §4/§5/§8](../architecture/survival-network.md),
[regions.md](../architecture/regions.md),
ハーネス: [`samples/24_locality/measure_locality.sh`](../../samples/24_locality/)

## 0. なぜこの計測が要るのか

`survival-network.md` §4 は次を**設計の存在理由**として掲げる:

> MoE のスパース性 = region 局所ルーティング = 遠方通信を減らす =
> **光速とエネルギーの物理制約への答え。設計に無駄がない。**

俯瞰監査 v3 の G25 はこの点を死角として指摘した —
「**エネルギー/性能を一度も測らない**のに §4 の存在理由が『光速とエネルギーへの
答え』。全実証が緑だが数字は精度と生存だけ」。本ドキュメントはその欠を埋め、
§4 の主張が **数字で支持されるか／されないか** を正直に報告する。合否ではなく
**計測** が目的。主張が偽でも数字を出すことに価値がある。

## 1. 計測方法（対照実験）

同一クラスタ（N=4）、同一 relay、同一推論列（node1 から `dkva infer` を 5 回）。
**唯一変える変数は region 粒度**:

| config | `PKERNEL_RTT_ZONE_SIZE` | 形成される region | DKVA `resp`(REGION スコープ) の fan-out |
|--------|--------------------------|-------------------|------------------------------------------|
| **ON**  | 2 | 2 region × 2 node | 自 region のピアのみ（1 宛先） |
| **OFF** | 4 | 1 個のフラット region（4 node） | 全員（3 宛先）＝ locality 無効 |

`PKERNEL_RTT_ZONE_PENALTY=200`（tau=50ms より大）。ON では異 zone の観測 RTT を
水増しして 2 region が形成され、OFF では全員が同 zone＝1 region になる。

観測は**既存の観測点のみ**（＋カウンタ1組、§5）:

- **メッセージ/バイト**: `kdds` シェルコマンドの `[locality]` 行。配送ごとに
  `region_is_member()` で **near（同 region）/ far（異 region）** に分類した累積。
  - *node1 per-burst*: 推論バースト直前/直後の差分（要求ノード単体・推論隔離）。
  - *cluster cumulative*: 全 4 ノードの最終累積を合算（系全体・約 37s 分）。
- **relay DATA バイト/パケット**: `relay -v` ログの `type=4`（pmesh/kdds ペイロード。
  `type=1`=REGISTER, `type=3`=KEEPALIVE は除外）。
- **エネルギープロキシ**: `near_bytes×1 + far_bytes×K`（K=5、§4 の定義は §4 節）。

## 2. 結果（対照表）

代表値（3 回実行、run 間ばらつき < 1%）。生ログは `/tmp/locality.XXXXXX/`。

### 2a. node1 要求ノード（推論バースト 5 回ぶんの差分）

| config | msgs | near | far | near_B | far_B |
|--------|-----:|-----:|----:|-------:|------:|
| **ON**  | 30 | 10 | 20 | 2360 | 4720 |
| **OFF** | 30 | 30 | 0  | 7080 | 0    |

### 2b. クラスタ累積（全 4 ノード合算）＋ relay ＋ エネルギープロキシ

| config | msgs | near | far | near_B | far_B | relay DATA_B | E_proxy |
|--------|-----:|-----:|----:|-------:|------:|-------------:|--------:|
| **ON**  | ~1630 | ~1500 | ~129 | ~354 K | ~30 K | **~0.96 MB** | ~507 K |
| **OFF** | ~4677 | ~4670 | ~7   | ~1.10 M | ~1.6 K | **~2.0 MB** | ~1.11 M |
| 比      | **0.35×** | | | | | **0.48×** | **0.46×** |

## 3. §4 の主張は数字で支持されたか

**部分的に YES（traffic / energy-proxy）、NO（latency / 光速）。** 正直に分けて述べる。

**(A) 総トラフィック — 支持された。** locality ON はクラスタ総 kdds メッセージを
**約 2.9× 削減**（~1630 vs ~4677）、relay DATA バイトを **約 2.1× 削減**
（~0.96MB vs ~2.0MB）。region スコープのトピック（DKVA `resp`、p-fs 複製、
region announce、score gossip）が ON では 1 宛先、OFF では 3 宛先に展開される差が
系全体で効く。§4 の「平常時は近傍だけが薄く発火」は **数字で裏付けられた**。

**(B) エネルギープロキシ — 支持された。** 合成指標で **約 2.2× 削減**（~507K vs
~1110K）。ただしこれは proxy であり literal joule ではない（§4 節）。

**(C) 要求ノード単体 — 中立（重要な但し書き）。** node1 の総送信は ON/OFF とも
30 msgs / 7080 B で**変わらない**。Q と rsum は GLOBAL スコープなので region に
関係なく全ピアへ broadcast されるため。locality が動かすのは「near/far のラベル」
だけ（ON では 4720B が真に region を跨ぐ；OFF では同じ物理ノード宛なのに『near』と
誤ラベルされる）。**locality の利得は系全体・他ノードの region スコープ通信にあり、
要求ノード自身の送出量にはない。** これは正直に記録すべき非自明な所見。

**(D) latency / 光速 — 測れていない＝未達。** RTT zone ペナルティは region 形成の
ための **観測 RTT を水増しするだけ** で、実際の per-packet 遅延を一切注入しない
（`arch/common/swim.c` `swim_rtt_ms()`、`usleep` 等なし）。よって推論の wall-time は
ON/OFF で動かない。§4 の核心である「光速の壁」への効果は **モデル上の概念であって
本計測では検証されていない**。これが本監査の最大の正直な限界。

> まとめ: 「**遠方通信のバイト数/件数を減らす**」という §4 の前半は localhost でも
> 数字で示せた。「**だから光速とエネルギーに効く**」の後半のうち、エネルギーは
> proxy で支持、**光速（遅延）は本ハーネスでは未検証**。

## 4. エネルギープロキシの定義と限界

```
E_proxy = near_bytes × 1 + far_bytes × K        (K = ENERGY_FAR_WEIGHT = 5)
```

K は「遠方リンク 1 バイトは近傍 1 バイトの約 K 倍コスト」という order-of-magnitude
仮定。`K = (tau + penalty) / tau = (50 + 200) / 50 = 5` を採用。

**限界（重要）**:

- **literal joule ではない。** 実消費電力は CPU/無線/伝搬で決まり、本カーネルには
  電力計がない。バイト×距離係数は粗い代理にすぎない。
- 距離は**シミュレーション**（zone ペナルティ）であり実物理距離ではない。
- localhost では全パケットが 1 relay を経由する（実ホップ数は均一）。proxy は
  「もし far リンクが K 倍高い WAN なら」という反実仮想。
- relay バイトには beacon overhead が含まれる（ON/OFF 同量なので差分は kdds を反映）。

## 5. 追加した計測点（カーネル領分への最小侵襲）

`arch/common/kdds.c` に累積カウンタ 1 組を追加（A隊 reflex/moe/dtr・B隊 dkva/world・
E隊 pfs_* の領分を避け、計測対象である kdds の薄い箇所に最小追加）:

- `kdds_tx_msgs / kdds_tx_cross / kdds_tx_bytes / kdds_tx_bytes_cross`
  — `kdds_pub()` の**実配送（delivery）**ごとに、宛先が自 region 外なら cross に計上。
- 公開: `kdds_locality_stats()`（`arch/common/include/kdds.h`）と、
  `kdds` シェルコマンド（`kdds_list()`）末尾の `[locality]` 行。
- 副作用は分類のための `region_recompute()` を pub ごとに 1 回呼ぶことのみ
  （従来 REGION スコープ時のみ呼んでいた；冪等・behaviour-preserving）。

**なぜ必要だったか**: 「1 推論あたり何メッセージが region を跨いだか」は relay からは
分からない（localhost の単一 relay は全トラフィックを同一視する）。near/far の内訳は
region 判定を持つカーネル側でしか取れない。relay ログのバイト総量は gross な裏取り。

## 6. 実機（Android フリート）で測るべき TODO

本ハーネスの最大の穴は (D) — 遅延が模擬。実フリートでは:

1. **実 RTT/遅延**: relay v2 タイムスタンプ往復で per-region wall-time を実測し、
   near vs far の推論レイテンシ差を出す（§4 latency 主張の本検証）。
2. **literal energy**: Android `BatteryManager`（電流 µA）/ `/proc` CPU jiffies で
   推論あたりの実消費を測り、proxy を較正・置換。
3. **無線コスト**: Wi-Fi/モバイルの送信電力は距離・電波状況依存。far バイトの実コスト
   係数 K を実測で求める。
4. **規模**: N=4 は toy。capacity(N) と region 数を増やし、O(N²)→O(region²)+O(#region)
   の理論スケール（regions.md）が traffic 曲線として出るか確認。
5. **モバイル無効化**: NAT/省電力で寝ているノードを含めた現実的トポロジでの再計測。
