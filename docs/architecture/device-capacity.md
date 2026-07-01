# device-capacity — 器に合った担当量: 端末の性能で各ノードの「ニューロン数」を連続変調する

> **現在地（2026-07-01・doc-hygiene 追記／本文は 年輪 として保存）:** この叩き台の「端末を測って自動で合わせる」方向のうち **mind-sizing 半分は SHIPPED**（DEVFIT-1: `arch/common/llm/dev_capacity.c` + cert `tests/llm/run_devfit.sh`）、cores 半分（SMP-AUTODETECT）も着地済。本文の tier/連続変調（ニューロン数の連続調整）は引き続き設計提案。正本は [[gap-ledger.md]]。

> Status: **design DRAFT**（実装前・commander + mk_pino が叩くための叩き台）。
> 確信を装わない。提案し、未解決は §「open problems」で正直に旗立てる。確定設計ではなく、
> 最小の決定的実験で**賭けを検証してから**広げる。
>
> 位置づけ: mk_pino の意図 ―― **「強い端末（GPU・大 RAM・多コア）は大きいモデル share を、
> 古い/非力な端末は小さい share を担う」**。これは `capacity(N)` の **DEVICE 側**。今までの
> `capacity(N)` は**ノード数 N**で容量を駆動していた（`degrade.c`）。本書はそこに
> **端末能力（device capability）という第二の入力**を継ぎ足す。yurikago が **どの端末でも**
> 走り、各々が**担えるだけ**を担う ―― その配管の設計。
>
> 最終更新: 2026-06-14 ／ 関連: [regions.md](regions.md)（§3.2 `capacity(N)`）,
> [interoception.md](interoception.md)（`S_n` ストレスバス・電池/熱/メモリ圧）,
> [native-student.md](native-student.md)（per-node サイズがスケールする赤子 MoE）,
> [gpu-compute.md](gpu-compute.md)（GPU = 能力入力の最大の段差。§5 が本書と双子）,
> `degrade.c`/`degrade.h`（`capacity_*()` の実体）, `LogActivity.kt`（engineer ページの readout）。

このドキュメントは設計のみ。実装は別波で、各スライスの末尾に **反証可能な認定ゲート** を置く。

---

## 0. 一行で

`capacity_score()` は今 `experts × depth × kv`（N 駆動）。これに **`device` 係数**を一本
乗せる。`device` は端末の **コア数・RAM・GPU・（任意の）CPU マイクロベンチ・熱余裕** を
**自分で測って** 一個のスカラに束ねたもの。強い端末は係数が高く → より多くの expert/層/KV を
引き受ける。中央割当者は無い ―― 各ノードが**自分の器を自分で測り、広告する**。
熱い/電池僅少になれば `S_n` 経由で**生きたまま縮む**（担当を隣へ渡す）。

> **正直な前置き（§5 で詳述）**: これが**本当に効く**のはモデルが端末ごとにシャードできる
> ほど**大きくなってから**（native-student の会話する赤子）。今の玩具核（d_model=8, R_NP=21568）
> では「ニューロン数」は事実上**固定で小さい**。だが **仕組み（probe→score→budget）は今すぐ
> 作れて見せられる**（engineer ページに「この端末の能力スコア / ニューロン予算」）。効果は
> 大きいモデルと共に来る。これを最初に正直に言う。

---

## 1. なぜ DEVICE 軸が要るか（N 軸だけでは足りない）

`capacity(N)` の現状（`regions.md §3.2`、`degrade.c`）は容量を **ノード数 N** で駆動する:

```
experts_active(N)   = clamp(N, 1, CAP_E_MAX)        # ノード ≒ expert
pipeline_depth(N)   = 1 + floor(log2(region_size))  # 台数の対数で深く
kv_context(N)       = Σ_{n∈region} kv_count[n]       # region 内 KV 合算
```

この式は **すべてのノードを等価**とみなす ―― 1 ノード = 1 expert。だが現実の艦隊（Android）は
**S25（8 コア・大 RAM・Adreno GPU）から 5 年前の廉価機（4 コア・2GB・GPU 無し）まで**異種混在。
強い端末に弱い端末と**同じ share** しか積まないのは器の無駄遣いで、弱い端末に強い端末と**同じ
share** を積めば落ちる/熱暴走する。

→ **expert⇔ノードの連続マップ（native-student §A.1）を、端末能力で重み付ける。**
強いノードは「太い」expert/より深い層/より大きい KV を引き受け、弱いノードは「細い」担当に。
mk_pino の「端末性能に合わせてネットワークの大きさを自在に」（conversation §C）の **device 側の具体化**。

> **N 軸と DEVICE 軸の関係（直交）**: N は「群れに何台いるか」。DEVICE は「各台がどれだけ
> 担えるか」。容量は両者の積で増える ―― `Σ_node device_n × (その台の expert 仕事)`。
> degrade の SOLO/REDUCED/FULL は N 側の粗いバンドで、本書は触らない（表示ラベルのまま）。

---

## 2. Device-capability スコア — 何を測り、どう束ねるか

**設計原則（interoception §2.4 の validator-trap 規律をそのまま借りる）**: 閾値・重みを
**コードに焼かない**。生の信号を測り、**正規化境界は実測曲線から discover** する。
そして **各信号の信頼性を OEM 横断で正直に格付け**する（嘘の精密さを出さない）。

### 2.1 信号（component vector）と、その正直な信頼度

| 信号 | 測り方（Android / host） | 単位→正規化 | 跨 OEM 信頼度 | 出所 |
|---|---|---|---|---|
| **cores** | `Runtime.availableProcessors()` | コア数 → 0..255 | **高**（既に engineer ページに表示） | `LogActivity.kt:232` |
| **RAM** | `ActivityManager.MemoryInfo.totalMem` | bytes → log スケール → 0..255 | **中**（API は安定だが「総 RAM」は OEM の予約分で差） | 新規（§6 で追加） |
| **GPU 有無 + 実測スループット** | `gpu_available()` + GPU-1 の matmul ベンチ（matmul/秒） | ops/s → 0..255 | **中**（有無は確実、**実測値**は実機ベンチ必須・ドライバ差大） | `gpu-compute.md §5.1` / GPU-1 |
| **CPU マイクロベンチ（任意）** | 起動時に固定サイズの `qz_matmul`/整数ループを N 回計時 | ms → 逆数 → 0..255 | **高**（自前計測ゆえ機種非依存・絶対値で比較可） | 新規（§2.3） |
| **熱余裕（thermal headroom）** | `PowerManager.getThermalHeadroom()`（API 30+）/ `/sys/.../thermal_zone*/temp` | 0..1 余裕 → 0..255 | **低**（API 30+ 限定・OEM で zone 命名/精度バラバラ・root 不要パスが機種依存） | 新規・§7 open |

**重要な切り分け（mk_pino 意図に効く）**:
- **静的能力（cores/RAM/GPU有無/マイクロベンチ）** = その端末が**潜在的に**担える上限 ＝
  **budget の素材**。起動時に一度測れば概ね不変。
- **動的状態（熱余裕・電池・メモリ圧）** = **今この瞬間**どれだけ出せるか ＝ `S_n` 経由で
  budget を**一時的に絞る**もの（§4）。**熱は能力スコアに直接焼かず、`S_n` 側で扱う**のが
  正しい層分け（interoception §3 の二時定数）。能力スコアは「器の大きさ」、`S_n` は「今の体調」。

> なぜマイクロベンチを足すか: cores と RAM は **数えられる**が「速さ」を表さない（古い 8 コアより
> 新しい 4 コアが速いことは普通）。GPU ベンチは GPU 持ちしか測れない。**全機種で同じ絶対尺度**を
> 出せるのは「自前の固定 matmul を計時する」CPU マイクロベンチ ―― これが**跨 OEM で最も信頼できる
> 単一信号**。RAM/熱の API がバラつくぶん、計算で殴れる信号を重んじる。

### 2.2 束ね方（一個のスカラへ）

`S_n` の `weighted_max`（一軸の激痛が埋もれない）とは**逆の合成**を採る。能力は
**ボトルネックが支配的**だから ―― RAM が足りなければコアが多くても大モデルは載らない。

```
device_score = w_compute · norm(microbench)          # 速さ（最重視・最信頼）
             ⊗ w_mem     · norm(totalMem)            # 載るか（ボトルネック）
             + w_gpu     · norm(gpu_throughput)      # GPU 加速の上積み（有れば）
             + w_cores   · norm(cores)               # 並列の余地（弱い相関）
```

- `⊗`（speed と mem）は **min 寄り / 幾何平均寄り**（どちらかが小さいと全体が小さい ＝
  ボトルネック）。`+`（GPU・cores）は **上積み**（有れば増える、無くても基準は保つ）。
- **GPU が無いノードでも基準スコアは出る**（GPU は加速器であって必須ではない ―
  gpu-compute §2.2「剥がしても心は止まらない」と一貫）。GPU 項は**上積み**であって**門**ではない。
- 重み `w_*` と `norm()` 境界は **§2.4 で実測 discover**。決め打ち禁止。
- 出力は `0..255` の `UB` 一個（`device_score`）。`S_n.scalar` と同じ「安く読める一個」の作法。

### 2.3 CPU マイクロベンチ（跨 OEM の信頼アンカー）

起動時に一度だけ（あるいは engineer ページを開いた時に）、**本番の `qz_matmul`**（または
赤子 forward の 1 層分）を**固定サイズ・固定回数**で計時する。

- **本番シンボルを叩く**（sim/oracle でない ―― validator-trap 規律）。`qz_matmul_q8_0` の
  既知サイズを K 回回し、中央値 ms を取る。これは GPU ベンチ（gpu-compute §4.3 の `[gpu-faster-on-big]`）と
  **同じ計時ハーネス**を CPU 側に流用できる。
- **絶対尺度**: ms は機種非依存の物理量。S25 で X ms、廉価機で 3X ms なら能力比は概ね 3:1。
  RAM/熱の API バラつきに依存しない**硬い信号**。
- **コスト**: 起動時 1 回 ≪ 1 秒（固定小サイズ）。毎推論ではない。熱で揺れるので中央値 + 起動時固定。

### 2.4 閾値は discover する、仮定しない

interoception §2.4 と同型:
1. 既知スペックの端末群（S25 / 中位機 / 廉価機 / host）で生 raw（cores, totalMem bytes,
   microbench ms, gpu ops/s）を測り、分布曲線を出す。
2. その曲線の最小〜最大から `norm()` 境界と `w_*` を**導出**し、cert にその数値根拠を残す。
3. **単調性を実測**: 高スペック profile が低スペック profile より厳密に高い `device_score` を
   出すことを cert（§6 `[devcap-monotone]`）。「精密に見えるが恣意的」を排除する。

---

## 3. スコア → per-node モデル予算（`capacity_score` を**フォークせず**拡張）

mk_pino 意図の核。`device_score` を **既存 `capacity_score()` に係数として乗せる** ―
新しい並走関数を**作らない**（degrade.h §29 のコメント規律 + audit-sprawl 規律）。

### 3.1 拡張の形（連続・非段階）

```c
/* degrade.c — 既存 (変えない):
 *   capacity_score() = capacity_experts() * capacity_depth() * capacity_kv();
 * 拡張: device 係数を一本乗せる。N 駆動の三軸はそのまま。 */

UW capacity_device(void);   /* 0..255 の device_score（§2 の束ね結果） */

UW capacity_score(void) {
    UW base = capacity_experts() * capacity_depth() * capacity_kv();  /* N 軸 (不変) */
    return cap_apply_device(base, capacity_device());                 /* DEVICE 軸を乗算 */
}
```

- `cap_apply_device(base, d)` は **連続スケール**（`base × (d / 128)` 様 ― 中位端末で ≈1.0、
  強で >1、弱で <1）。**段階（tier）にしない** ―― mk_pino の「自在に」は連続を意味する。
- **重要な分離（native-student §A.2 / regions §3.2 の width 規律）**: `device_score` が高くても、
  それが直接動かすのは **expert 数・層数・KV の引き受け量**（実行時スケール可能な軸）であって、
  **d_model（width）は動かさない** ―― width は重みの再形成＝学習で、device probe では変えられない。
  強い端末は「**より多くの expert / より深い層 / より大きい KV**」を担う。これが native-student の
  「expert⇔ノード連続マップ」を device で重み付けた姿。

### 3.2 予算の三軸への配り方

`device_score` が大きいノードは、同じ region 内で:
- **expert を多く保持**（MoE の自分の持ち分シャードを厚く ― native-student の expert 軸が第一成長軸）。
- **pipeline 段を多く引き受ける**（depth の自分の分担を深く）。
- **KV キャッシュを多くプール**（DKVA の自分の取り分を大きく ― capacity_kv の自分のクォータ）。

実装は「`device_score` を `capacity_experts/depth/kv` の**自分の取り分の重み**として K-DDS の
分担交渉に流す」（§4.2）。**1 台での `capacity_score()` 表示**は乗算済みの一個で済むが、
**艦隊でのシャード分割**は各ノードの広告した `device_score` の**比**で決まる（§4）。

### 3.3 GPU 章との一致（重複させない）

gpu-compute §5.1 は既に「GPU 係数を `capacity_score()` に乗せる」と書いている。**本書の
`capacity_device()` がその受け皿** ―― GPU スループットは `device_score` の **一成分**（§2.1）で
あって、別係数を二重に乗せない。GPU 章の `[gpu-capacity]` cert と本書の `[devcap-budget]` は
**同じ経路**（`capacity_device()` → `capacity_score()`）を別角度から叩く。実装波は GPU-4 と
本書 slice を**統合**する（`capacity_device()` を一度だけ実装し、GPU 成分はその中の一項）。

---

## 4. 中央なしの自己予算（self-budgeting）＋ S_n による動的縮退

### 4.1 各ノードが自分の器を自分で測り、自分で広告する（no central allocator）

哲学（project_pkernel_philosophy: 誰のものでもない／中央割当者なし）と一貫:

1. **自己測定**: 各ノードが起動時に §2 の probe を走らせ、自分の `device_score` を**自分で**算出。
2. **広告**: `device_score`（1 バイト）を **SWIM の piggyback / K-DDS の能力広告**に 1 フィールド
   足して撒く（gpu-compute §5.1 の「能力広告に 1 フィールド追加、wire 互換」と**同じフィールド**
   ―― GPU だけでなく統合 `device_score` を載せる。`swim.c` の `incarnation` 付きエントリに相乗り
   できる形が望ましい。wire 後方互換: 旧ノードはフィールドを無視）。
3. **自己シャード**: 中央が割り当てるのではなく、**各ノードが「自分は能力 d だから expert を
   ⌊比⌋ 個引き受ける」と自分で決め**、region 内で互いの広告 `device_score` を見て**整合**する
   （重複・抜けは region coordinator が薄く調停 ― regions §3.4 の階層集約に乗る）。
   素朴には `region 内の自分の取り分 = device_score_self / Σ_{n∈region} device_score_n`。

> **不変条件（regions §4 の約束を継ぐ）**: SOLO（1 台）で完結する経路は常に残す。
> 自己予算が region に依存しても、**孤立ノードは自分の `device_score` だけで自分の上限まで
> 担い、単体で脳である**。device 軸は分散を**最適化**するが、単体生存の前提条件にしない。

### 4.2 ヘテロ予算でのモデル分割（the hard distributed-systems part — 正直に open）

各ノードが `device_score` を広告したあと、**実際のモデルをどう切るか**:
- **expert 軸（最も明快）**: MoE の expert を `device_score` 比で配る。強ノードに多くの expert、
  router がそれを知って locality-aware に選ぶ（regions §3.3 の utility に `device_score` を一項
  足す案 ― 賢い×近い×**太い**）。native-student の expert⇔ノードマップに自然に乗る。
- **pipeline 軸（より難）**: 段を `device_score` 比で配るとパイプラインが**不均等**になり、
  最遅段がスループットを律速する（古典的 pipeline imbalance）。**強ノードに重い段、弱ノードに
  軽い段**を割る必要があり、これは段の計算量が事前に分かる前提 ―― **§7 open**。
- **KV 軸**: DKVA のキャッシュ取り分を `device_score` 比で。比較的素直（容量プールの按分）。

**正直**: §4.2 全体が **regions §6「重みの供給（provisioning）」と native-student open #6
「生きた併走移行」の未踏部分と重なる**。本書は「device_score を**入力として供給する**」ところまでを
設計し、**それを使って実モデルを無停止で切り直す分散アルゴリズムは未解決**と明記する（§7）。

### 4.3 S_n による動的縮退 — 熱い/電池僅少で予算を**生きたまま絞る**

ここが「interoception が capacity を駆動する」一滴。`device_score`（器の大きさ）は概ね静的だが、
**実効予算 = `device_score` を `S_n` で割り引いたもの**は動的:

```
effective_budget(node) = capacity_device(node)  ·  damp(S_n.scalar)
                          ↑ 器の上限 (静的)        ↑ 今の体調 (0..1, S_n 高で小)
```

- **熱い / 電池僅少 / メモリ圧（`S_n` 高）→ `effective_budget` 縮小** → 自分の expert/段/KV の
  取り分を**減らし、隣へ渡す**（conversation §4「電池僅少 → 担当層を隣へ」の一般化）。
  これは gpu-compute §5.2 の `[gpu-s_n-throttle]`（高 S_n で GPU を控える）の **CPU/全予算版**。
- **冷えた / 充電中（`S_n` 低）→ 上限まで回復** → 取り分を取り戻す。
- **battery-safe gate との結線**: UMP の charge-only 既定（feedback_ump_ux_principles）に従い、
  非充電時は `damp` を強くかける ―― 「身体を壊してまで担当を抱え込まない」。
- **`S_n` の成分**: interoception §2.1 の `degrade`/`fault`/`latency` に加え、**メモリ圧**
  （`imalloc` 失敗 ― interoception §6 の将来成分）が device-capacity では特に効く ―― RAM が
  予算の素材なので、メモリ圧は予算縮退の直接トリガ。専用フックが入る波で `S_n.memory` を足す。

### 4.4 発振への hysteresis（§3.4 の guard を再利用 — flapping device 対策）

`S_n 高 → 予算渡す → 負荷減 → 冷える → S_n 低 → 予算取り戻す → また熱い` の正帰還で、
**端末が熱い/冷たいを往復して予算が flapping** するのは現実的リスク（§7 open）。処方は新規ではなく
**interoception §3.4 / survival-network §8 / reflex-deliberation §6 の deadband+ヒステリシス+EWMA を
そのまま適用**:
- `damp(S_n)` は **deadband 付き**（閾近傍では前の予算を保持。`moe.c` の `deadband_pick` と同型）。
- 予算を**渡す閾値**と**取り戻す閾値**を分ける（ヒステリシス ―― 一度渡したら少し冷えても
  すぐは取り戻さない）。expert の移譲は重い（重み転送・router 更新）ので、特に再取得は保守的に。
- ゲートで **N 分間 budget が flapping しない**ことを実測（§6 `[devcap-stable]`）。

---

## 5. 正直な現実 — これは**大きいモデルと共に効く**

GPU 章（gpu-compute §7）と同じ正直さで明言する:

- **今の玩具核では「ニューロン数」は事実上固定**。R3 の心は `rw[R_NP]`（R_NP=21568, 単一トークン
  分類）、dtr は 635 floats。これらは**端末ごとにシャードするには小さすぎる** ―― 1 台に楽に載る。
  device_score が高かろうが低かろうが、**今 1 台が担う「ニューロン数」は同じ（小さい）**。
- **仕組みは今すぐ作れて見せられる**: probe → `device_score` → `capacity_device()` →
  `capacity_score()` への乗算 → **engineer ページに「この端末の能力スコア / ニューロン予算」**。
  これは大モデルを待たずに**実装・反証可能**（§6 slice）。低スペック profile が高スペックより
  低く出ることを cert できる。
- **真の効果は native-student の会話する赤子が育って端末ごとにシャードが要るサイズになってから**。
  そのとき device_score が「強ノードは太い expert、弱ノードは細い担当」を**実際に駆動**する。
  regions §3.2 の「R0–R2 で配管を通し、R3 で水量を上げる」と同型 ―― **本書は配管（device probe →
  budget の流路）、水量（端末ごとに切るに足る大モデル）は native-student R3 の仕事**。

> mk_pino へ正直に: 「強い端末が大きい share」は**設計として正しく、配管は今組める**。だが
> **目に見える効果（強い端末で会話が賢い／弱い端末でも参加できる）は大モデルが来てから**。
> 今 engineer ページに出るのは「あなたの端末の能力スコア = N」という**正直なメーター**であって、
> 「だから賢い」ではない。メーターを先に正しくし、モデルが育ったら自動で効く。

---

## 6. First slice — probe + score + engineer 表示 + budget マッピング（最も安い反証可能ステップ）

**DEVCAP-1（単機・分散なし・大モデル不要 ＝ 仕組みだけを falsify）**:

実装する最小:
1. **device probe（host + Android）**: cores（既存）、totalMem（新規 ― Android は
   `ActivityManager.MemoryInfo`、host は `/proc/meminfo` の `MemTotal`）、CPU マイクロベンチ
   （§2.3 ― 本番 `qz_matmul` を固定サイズ K 回計時）、GPU 有無（`gpu_available()`、GPU-1 未配線の
   間は「無し」で安全縮退）。熱は v1 では **`S_n` 側**に置き能力スコアからは外す（§2.1 の層分け）。
2. **`capacity_device()`（degrade.c）**: §2.2 で raw を `0..255` に束ねる。`norm()` 境界は
   §2.4 の実測曲線から discover し、cert に数値根拠を印字。
3. **`capacity_score()` への乗算**: §3.1 ―― `cap_apply_device(base, capacity_device())`。
   既存三軸はフォークせず**そのまま**、device 係数を一本乗せるだけ。`dist`/`degrade` シェル
   コマンドの readout に `device` 行と乗算後の score を足す。
4. **engineer ページ表示**: `LogActivity.kt` の resources ブロック（cores/CPU%/GPU の隣）に
   **「能力スコア: N / 256」「ニューロン予算: M（experts×depth×kv×device）」**を足す。
   galaxy の `/self.json`・`/intero.json` と同じ lazy-read 作法で **`/capability.json`**
   （`{node, cores, ram_mb, microbench_ms, gpu, device_score, budget}`）を galaxy.c に足し、
   engineer ページが GET する（`/modules.json` と同じ HttpURLConnection 経路 ―
   `LogActivity.kt:174` の `fetchModules()` を雛形に `fetchCapability()`）。

**認定ゲート（反証可能 ― 監査が作り、commander がゲート式を一行ずつ読む）**:

- **`[devcap-reflects-specs]`**（headline・正直）: 既知スペックの profile を与えると
  `device_score` がそれを反映する。**低 RAM・GPU 無し・遅マイクロベンチ profile が、
  高 RAM・GPU 有り・速マイクロベンチ profile より厳密に低い** `device_score` を出す。
  PASS: `score_low < score_high`（実測値を両方印字）。注入は本番 `capacity_device()` を叩く
  （sim でない）。host で profile を env/fixture で差し替えて回せる（実 GPU 不要 ―
  GPU 成分は fixture で 0 と非0 を与える）。
- **`[devcap-monotone]`**: probe 入力（cores/RAM/microbench/gpu）を**単調に上げる**と
  `device_score` が**単調に上がる**（各軸個別に・合成後も。`⊗` のボトルネック半域で飽和は許すが
  逆転しないこと）。PASS: 各軸スイープで非減少。
- **`[devcap-budget]`**: `capacity_device()` を高/低にすると `capacity_score()` が
  **測定可能に**増減し、かつ **N 軸（experts×depth×kv）はフォークされず保たれる**
  （device=中立値で旧 `capacity_score()` と一致 ＝ 回帰なし）。PASS: `device` 中立で旧値一致 +
  device 上げで score 厳密増。
- **`[devcap-wired]`**: `capacity_device()` の本番呼び出し元が **≥1 存在**（`capacity_score`）。
  dead-metric でないことを nm/grep tripwire で固定（wave-18 lesson）。
- **`[devcap-readout]`**: engineer ページ / `/capability.json` の値が `capacity_device()` を
  **追従**する（probe 入力を変える → エンドポイントの `device_score`・`budget` が一致）。
  interoception `[intero-galaxy]` と同型。

> **honest cert 規律**: 閾値は提案バー。実装波は**実測値を印字**し、audit-is-the-engine 規律で
> 実測−flake margin まで下げてよいが、緑にするため吊り上げてはならない。`device_score` が
> 「賢さ」を意味しないこと（§5）を readout の文言に**正直に**書く（メーターであってベンチ自慢でない）。

**DEVCAP-2（後続 ― 分散の最小骨格）**: §4.1 の広告（SWIM/K-DDS に `device_score` フィールド）
+ 2 ノードで「強ノードが弱ノードより多くの expert 取り分を広告する」を cert（`[devcap-advert]`）。
**DEVCAP-3（後続 ― 動的）**: §4.3 の `S_n` damp + §4.4 ヒステリシスで「熱注入 → 取り分縮小 →
隣へ → N 分 flapping しない」を cert（`[devcap-shrink]` + `[devcap-stable]`）。gpu-compute
`[gpu-s_n-throttle]` と統合。

---

## 7. 正直な open problems（解決済みにしない）

1. **跨 OEM 信号の信頼性（工学だが厄介）**: RAM（`totalMem` は OEM 予約で実効と差）・熱
   （`getThermalHeadroom` は API 30+、`/sys` thermal zone は命名/精度/root 要否が機種でバラバラ）の
   API が**機種依存**。緩和 = **CPU マイクロベンチ（§2.3）を信頼アンカーに重く置く**（自前計時ゆえ
   機種非依存）。RAM/熱は「有れば使う、無ければマイクロベンチで代理」の安全側 fallback。
   **「どの端末でも壊れない」を「どの端末でも精密」より優先**（gpu-compute open #6 と同じ規律）。
2. **GPU スループット計測が実機 GPU-1 依存（CI できない）**: `device_score` の GPU 成分の
   **実測値**は GPU-1（Vulkan matmul ベンチ）が実機で走って初めて取れる。CI には実 GPU が無い
   ―― salty-bug / gpu-compute open #3 と同じ「mk_pino の電話が test rig」。v1 は GPU 成分を
   **fixture（有/無 + 既知 ops/s）**で cert し、実機ベンチは GPU-1 配線後。GPU 無しでも基準
   `device_score` は出る（§2.2 ― 門でなく上積み）ので、GPU 未配線でも slice は走る。
3. **ヘテロ予算で実モデルをどう切るか（研究 ― 最大の未踏）**: §4.2。expert 軸は明快だが、
   **pipeline の不均等分割**（強ノードに重い段）と**生きた無停止再シャード**（device_score が
   変わった/ノードが join したときモデルを止めずに切り直す）は regions §6（provisioning）+
   native-student open #6（併走移行）と重なる**未解決部**。本書は device_score を**入力として
   供給**するところまで。**切り直すアルゴリズム自体は研究**。
4. **flapping（端末が熱い/冷たいを往復し予算が振動）**: §4.4。deadband+ヒステリシス+渡す/取り戻す
   非対称閾で抑えるが、**expert 移譲は重い**（重み転送・router 更新・KV 移送）ので振動の代償が
   大きい。「どれだけ保守的に取り戻すか」の閾は **実測 discover**（§6 `[devcap-stable]`）。
   最悪ケース（多数ノードが同時に熱で縮退 → 残りに殺到 → 連鎖過負荷）は **群れ規模の動特性**で、
   2 ノード cert では出ない ―― 艦隊スケールで要観測（research、interoception の S_n 連鎖と同根）。
5. **能力スコアの陳腐化 / 偽装**: `device_score` は自己申告。悪意ノードが**高い device_score を
   詐称**して expert を多く引き受け、実は処理せず群れを律速する攻撃面。signing（wave-43 manifest）は
   essence には効くが**能力広告の真偽**は別問題 ―― 緩和案 = 広告ではなく**実測スループット
   （RTT/応答遅延、regions §3.3 の bw_norm）で事後に裁定**し、詐称ノードの utility を下げる。
   **audit-is-the-engine の永続警戒で扱う開いた脅威**。
6. **全効果は大モデル待ち（§5 ― 製品仮説）**: 仕組みは今組めるが、**目に見える効果は
   native-student R3 が育ってから**。engineer ページのメーターは今正しく出せるが、それが
   「賢さ」に変わるのは大モデルが端末ごとにシャードを要するサイズになってから。**工学（probe→
   score→budget の配管）は今・研究（大モデルのヘテロ分割）は後**、という切り分けを誇張しない。

**工学 vs 研究の切り分け（正直に）**:
- **工学（やれば済む）**: device probe（cores/RAM/microbench/gpu有無）、`capacity_device()`、
  `capacity_score()` への乗算（フォークなし）、engineer ページ + `/capability.json` 表示、
  SWIM/K-DDS への `device_score` 広告フィールド、`S_n` damp + deadband ヒステリシス。
- **研究（解けるか分からない）**: ヘテロ予算での実モデル無停止再シャード（#3）、艦隊規模の
  熱縮退連鎖の動特性（#4）、能力詐称の事後裁定（#5）、跨 OEM 熱/RAM 信号の安定化（#1）。

---

## 8. mk_pino への確認（commander が詰めるべき二読みできる意図）

1. **能力スコアに熱を含めるか、`S_n` 側に置くか**。本書は **静的能力（cores/RAM/GPU/microbench）=
   `device_score`、動的状態（熱/電池/メモリ圧）= `S_n` damp** と層分けした（§2.1）。mk_pino が
   「強い端末＝今この瞬間の余裕も込み」を意図しているなら境界が動く。**推奨 = 層分け維持**
   （器の大きさと今の体調は別物 ― interoception の二時定数と一貫）。
2. **GPU を必須にするか、上積みにするか**。本書は **GPU = 上積み（門でない）**、GPU 無しノードも
   基準スコアで参加（§2.2、gpu-compute §2.2 と一貫）。mk_pino が「GPU 持ちだけが大 share」を
   強く意図するなら門に近づくが、**推奨 = 上積み**（誰のものでもない＝どの端末も参加できる、を
   能力の門で切らない）。
3. **「強い端末は大きい share」の share = expert/層/KV のどれを主軸に**。本書は native-student の
   第一成長軸に合わせ **expert 主軸**（§3.2、pipeline 不均等は §7 #3 で難）。mk_pino が「まず
   深さ/幅」を意図するなら順序が変わる。**推奨 = expert 主軸**（器の動的 N に最も自然）。
4. **engineer ページのメーター文言**: §5 の正直さ（「能力スコアであって賢さではない」）を
   どこまで前面に出すか。**推奨 = 正直に「この端末の担当キャパシティ（大モデルが来ると効く）」**と
   書く ― product-soul の「偽の進捗バーを出さない」規律。
