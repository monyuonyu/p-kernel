# Relay RTT + energy benchmark — measuring §4's core on the REAL path

最終更新: 2026-06-08 ／ wave-17 §4-measure 隊 ／ 俯瞰監査 **G31/G25**（§4 の核が実路で未計測）への回答
関連: [survival-network.md §4/§5/§8](../architecture/survival-network.md),
[locality.md](locality.md)（§4 前半 traffic/energy を kdds カウンタで実測。その (D) latency 未測がここの出発点）,
[latency.md](latency.md)（MODELLED な far-delay 注入で二層分離を実測。relay 自身の RTT・負荷下 RTT は未測）
ハーネス: [`samples/36_relay_measure/run.sh`](../../samples/36_relay_measure/),
クライアント: [`samples/36_relay_measure/measure.c`](../../samples/36_relay_measure/measure.c),
relay 変更: [`relay/relay.c`](../../relay/relay.c)（probe-stamp ノブ、既定 OFF）

## 0. なぜこの計測が要るのか

`survival-network.md` §4 は MoE のスパース性を
**「光速とエネルギーの物理制約への答え」** と掲げる。俯瞰監査 G25/G31 はこの核が
**実路で数になっていない**点を死角とした。先行 2 波が半分ずつ埋めた:

- **locality.md (wave-12)**: traffic と energy-proxy を **kdds カウンタ**（カーネル内、
  delivery 単位）で実測。ただし (D) で正直に残した —「**per-packet latency は未測**」。
- **latency.md (wave-15)**: **MODELLED** な far-delay（`RELAY_FAR_DELAY_MS`）を注入し、
  二層が時定数として分離することを REAL relay 往復で実測。だが測ったのは *注入した
  遅延* であって **relay 自身の転送 RTT** でも、**負荷下の RTT** でもない。

本波が埋める二点（いずれも REAL `./relay` × REAL UDP socket 上で実測）:

1. **relay の素の転送 RTT を OFFERED LOAD の関数として実測**（注入遅延ゼロ）。
   relay の opt-in probe-stamp を ON にすると RTT を *network* と *relay residence*
   （relay 内滞在＝rx→tx）に分解できる。
2. **per-message ENERGY proxy を END-TO-END で実測** —— relay の wire を実際に渡る
   バイト数（2 hops: src→relay, relay→dst）を両 leg で数え、joule 換算と far の重み K
   だけを MODELLED と明示する。

合否でなく **計測** が目的。終始 **実測** と **モデル** を分ける。

## 1. 設計：relay wire 変更は ADDITIVE かつ既定 OFF

`relay/relay.c` に **probe-stamp** を追加した。env `PKERNEL_RELAY_PROBE_STAMP` 未設定
（既定）では転送パスは **byte-for-byte** ノブ導入前と同一 —— relay 6/6 も、他隊が
この relay 上で回す live テストも不変。

| env | 既定 | 効果 |
|-----|------|------|
| `PKERNEL_RELAY_PROBE_STAMP` | （未設定＝OFF） | ON のとき、payload が 4 byte magic `"PRB1"` で始まる **KEEPALIVE** のエコーに、relay 側の単調マイクロ秒スタンプ 2 つ（`rx_us`=recvfrom 直後 / `tx_us`=エコー sendto 直前）を **末尾に 16 B 追記** |

既存の KEEPALIVE エコー（relay-HA の liveness pong）は元々 wire 不変で反射する。
probe-stamp が ON でも、magic を持たない通常 keepalive は **verbatim** に反射するので
HMAC 認証付きの実 wire は決して書き換わらない。追記スタンプは HMAC の外＝
**計測補助（advisory）であり認証フィールドではない**ことを明記する。
ブロッキング（usleep）は一切しない —— RTT を測る道具がレイテンシを足しては本末転倒。

### 1.1 非破壊の証明（ハーネスが自動で出す）

`run.sh` のステップ 0 は probe keepalive を 1 発撃ち、relay が追記したバイト数を出す:

```
flag OFF -> echo_extra=0 B    (verbatim, relay-HA safe)
flag ON  -> echo_extra=16 B   (rx_us+tx_us appended)
additive/off-by-default: PASS
```

ステップ 3 は **probe-stamp OFF** で relay 6/6 を回し、緑のままを確認する:

```
[relay-test] PASS — all 6 scenarios green (0 failures)
relay 6/6: PASS
```

## 2. 計測 1：relay 転送 RTT vs offered load（実測）

`measure rtt`: node1 が ship として登録し、`"PRB1"` 入り KEEPALIVE を **burst depth
B**（同時 in-flight 数＝offered load）だけ連射し、エコーを 1 発ずつ RTT 計時する。
RTT = `now_us() - 送信時刻`（payload に詰めた 8B 送信スタンプ）。これは
**ship→relay→ship の実 socket 往復**（relay 1 反射、注入遅延ゼロ）。

### 2.1 結果（本ホスト localhost、代表 3 run）

```
[rtt] offered  recv     min    mean     p50     p95     max  resid_mean   (ms)
[rtt] 1        1       0.18    0.18    0.18    0.18    0.18      0.009..0.020
[rtt] 16       16      0.16    0.25    0.25    0.32    0.33      ~0.005
[rtt] 64       64      0.44    0.68    0.62    1.09    1.10      ~0.006
[rtt] 256      256     2.05    2.98    3.00    3.84    4.01      ~0.006
[rtt] 1024     ~600-770 4.7    7.3-10  ~9     ~11-15  ~12-16    ~0.006
```

| 量 | 値 | 種別 |
|----|----|------|
| 無負荷（depth 1）relay RTT | **~0.18–0.27 ms（mean）** | **実測** |
| 高負荷（depth 1024）relay RTT | **mean ~7.3–10 ms / p95 ~11–15 ms / max ~12–16 ms** | **実測** |
| relay residence（rx→tx, relay 内滞在） | **~5–20 µs** | **実測**（relay 追記スタンプ） |
| depth 1024 での UDP 取りこぼし | **~600–770 / 1024 echoes**（負荷の証） | **実測** |

### 2.2 何が言えたか

- **(A) relay の素の転送 RTT は実測できた。** 無負荷で **sub-ms**（~0.2ms）、
  offered load を上げると **単調に増える**（depth 1024 で mean ~8ms / p95 ~13ms）。
  locality.md (D) の「per-packet latency 未測」を REAL relay 往復で埋めた。
- **(B) RTT の伸びは relay の CPU ではなく socket/キュー待ち。** relay residence
  （rx→tx）は負荷に依らず **~数〜十数 µs** のまま（mean ~0.006ms）。つまり HMAC 検証
  ＋転送そのものは μs オーダで、負荷下の ms オーダ RTT は **OS の socket バッファ待ち**
  が支配的。これは probe-stamp の分解があって初めて言える非自明な所見。
- **(C) 負荷で取りこぼしが出る。** depth 1024 では echo の ~25–40% が落ちる
  （relay の RCVBUF を burst が溢れさせる）。これも *実測* した負荷の症状で、隠さず
  recv 数として出す。§4 が言う「平常は薄く発火」＝offered load を低く保つことが、
  実装上も tail latency と loss を避ける根拠になっている。

## 3. 計測 2：per-message energy proxy（end-to-end 実測）

`measure energy`: ship が payload P=256 B の DATA を **near sink (node2)** と
**far sink (node3)** へ各 M=2000 通送る。sink は実際に届いたバイトを数える
（＝end-to-end の delivery leg を実測）。1 メッセージは relay topology で必ず **2 hops**
（src→relay, relay→dst）。

### 3.1 結果（代表値）

```
[energy] --- MEASURED (real socket byte counts) ---
  v2 framing overhead     : 36 B/msg (HEAD 12 + AUTH 24)
  NEAR  sent=584000 B  delivered=570276..584000 B (1953..2000 msgs)  wire=~1.15 MB
  FAR   sent=584000 B  delivered=584000 B          (2000 msgs)        wire=~1.17 MB
  wire amplification (wire/app) NEAR=2.25x   (2 hops x (36+P)/P)
[energy] --- MODELLED (joule 1000 nJ/B, far weight K=5) ---
  E_proxy(byte) NEAR=1154276  FAR=5840000  (FAR/NEAR=5.06x via K)
  E(joule) NEAR=1.15 J  FAR=5.84 J  per-msg NEAR=~577 uJ
```

| 量 | 値 | 種別 |
|----|----|------|
| v2 framing overhead | **36 B/msg**（HEAD 12 + AUTH 24） | **実測** |
| wire amplification（wire/app） | **2.25×**（2 hops × (36+256)/256） | **実測** |
| near/far の delivered byte | **各 ~0.57 MB**（M=2000, loss 0–2.3%） | **実測** |
| joule 換算 | per-msg **~577 µJ**（@1 µJ/B） | **モデル** |
| far の重み K | **5×**（far link は near の K 倍コスト） | **モデル** |

### 3.2 何が言えたか

- **(A) energy proxy を end-to-end の実バイトで測れた。** locality.md は kdds の
  *delivery 単位*（payload）を数えたが、本波は **wire を実際に渡るバイト**（v2 の
  36B framing ＋ relay の 2 hop 増幅を含む）を送受 **両 leg** で数えた。1 アプリ
  メッセージは wire 上 **2.25×** に膨らむ（実測）—— これは kdds カウンタには出ない量。
- **(B) joule と K は依然モデル。** 1 µJ/B はモバイル無線 TX の **order-of-magnitude**
  仮定（実消費は CPU/無線/伝搬・電波状況依存。本カーネルに電力計はない）。far の
  K=5 は locality.md と同じ `(tau+penalty)/tau` 由来の代理。localhost では near/far の
  物理 hop/バイトは **同一**（実測でも near≈far）—— 差は **すべてモデルの K**。実 WAN で
  「far 1 byte が near の何倍か」は実機でしか測れない。

## 4. §4 の主張は数字で支持されたか

**latency：実測で埋めた（relay 素の RTT と負荷依存）。energy：実バイトは実測、
joule/K はモデル。** 正直に分ける。

- **支持された（実測）**: relay 転送 RTT を offered load の関数として実測（無負荷
  ~0.2ms → depth1024 で p95 ~13ms）。RTT 増は relay CPU でなく socket キュー（residence
  ~µs を実測して切り分け）。per-message の wire バイト（2.25× 増幅・36B framing）を
  end-to-end 実測。locality.md (D) と latency.md の「relay 自身の RTT 未測」を是正。
- **モデルのまま**: 絶対 joule（1 µJ/B）、far link 重み K=5、実距離。300ms や µJ/B は
  localhost で観測可能なスケールへの代理であって、実フリート（地理的に離れたノード・
  実無線）でしか実値に置換できない。

> まとめ: 「**遠方は遅く・高コスト**」の *相対構造*（負荷で RTT が伸びる／1 メッセージが
> wire 上で 2.25× に膨らむ）は localhost の実 relay で **数として測れた**。「**だから
> 光速とエネルギーの壁に効く**」の *絶対値*（分単位の実遅延・実 joule）は **モデル**の
> まま —— それは §5 実機でしか測れない。

## 5. 残る穴（実機 Android フリートで測るべき）

locality.md §6 / latency.md §5 と同じ TODO が残る。本波で **新たに**実値化したのは
relay 素の RTT・負荷曲線・wire 増幅。依然モデルなのは:

1. **literal joule**: Android `BatteryManager`（µA）/ `/proc` jiffies で推論あたり実消費を
   測り、1 µJ/B 代理を較正・置換。
2. **実距離 RTT**: 地理的に離れたノード間の relay 往復を実測し、near/far の実レイテンシ差
   （ここの ~0.2ms は localhost。実 WAN は数十〜数百 ms、惑星間は分単位）。
3. **実 far コスト K**: WAN/無線の far 1 byte が near の何倍かを実測。
4. **multi-relay / 実 NAT**: 単一 localhost relay は全 hop を均一視する。実フリートの
   多段・NAT 越え relay でホップ数と RTT を再計測。

## 6. 触ったファイル / 触っていないもの

- 追加/変更（本波）: `relay/relay.c`（probe-stamp ノブ＋keepalive エコー追記、**既定 OFF**）、
  `samples/36_relay_measure/`（新規: `measure.c` + `run.sh` + README）、本ドキュメント。
- **触っていない**: `arch/common/*`・`arch/linux/*`・カーネル本体・`.github/workflows/ci.yml`。
  relay の遅延ノブ（latency.md）・HMAC/replay（6/6）はそのまま。probe-stamp は完全に
  additive で、未設定時は wire byte-for-byte 不変。
