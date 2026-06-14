# gpu-compute — 端末の GPU で心の数理を実際に回す（the body's accelerator）

> Status: **design DRAFT**（実装前・commander + mk_pino が叩くための叩き台）。
> 確信を装わない。提案し、未解決を §「open problems」と §「mk_pino への確認」で
> 正直に旗立てる。確定設計ではなく、最小の決定的実験で**賭けを検証してから**広げる。
>
> 位置づけ: conversation.md **§4「身体との接続 — GPU・資源適応」** の直接の実装章。
> あの一文（「Android で base の行列積を端末 GPU（Adreno/Mali）に。Vulkan compute /
> NNAPI 経由」）を、反証可能なマイルストーンに開く。mk_pino の意図:
> 「GPU が "未使用" と表示されるだけでなく、心の数理に**実際に使われて**ほしい」。
>
> 最終更新: 2026-06-14 ／ 関連: conversation.md §4・§C, inference-engine.md（M1b
> `qz_matmul_q8_0/q4_0` = 加速対象の本丸）, native-student.md（蒸留する重い MoE
> forward）, regions.md（capacity(N)）, interoception.md（S_n 電池/熱ストレス）,
> product-soul.md（engineer-page の「見える」readout）。
>
> 対象機: **Samsung Galaxy S25（SM-S948）** — Adreno/Xclipse GPU, Android 14,
> targetSdk 34, minSdk 26。アプリ = NDK ビルドの yurikago。
> **ビルド/CI 環境（NDK-under-qemu-x86_64-PRoot）には実 GPU が無い** ―
> これが §4 の「device-only テストループ」の存在理由。

---

## 0. 一行の核心（誇張しない）

**GPU は「心が大きくなった時だけ効く、任意の加速器」である。** libc-free C の
`qz_matmul_*`（inference-engine.md M1b）が**正典（reference / fallback）**であり続け、
GPU は**大きな行列積でのみ**それを置き換える。小さな脳（635-param dtr センサ脳、
R_NP=21568 の R3 心）では **GPU のカーネル起動オーバヘッド > 計算 → GPU の方が遅い**。
だから GPU は**サイズ閾値でゲートされ、必ず CPU フォールバックを持つ**。

そしてこの章は「数波」では終わらない。Vulkan は本物の複雑さを持ち、テストは実機でしか
できない。**正直に多波の章として進める**（§7）。

---

## 1. API の選択 — なぜ Vulkan compute か

Android のスマホ GPU で**任意の数理**（GGUF 量子化行列積のような、モデルグラフに
収まらないカスタム計算）を回す現実的な選択肢を比較する。

| API | 何か | この用途での評価 |
|---|---|---|
| **Vulkan compute** | 明示的・低レベルの GPU compute（SPIR-V シェーダ、SSBO バッファ、明示的同期）。Android NDK が公式サポート（`libvulkan.so`、`vulkan/vulkan.h`） | ✅ **推奨。** 移植性（OEM 横断の単一 API）・現代的・S25 の Adreno/Xclipse が確実に対応・**任意の数理を SPIR-V で書ける**（dequant も matmul も自前カーネルで表現可）。代償は複雑さ（§7） |
| OpenGL ES 3.1 compute shader | GLSL compute、SSBO あり | △ 動くが**非推奨化の方向**（Vulkan が後継）。GLES の compute は機能が限定的（同期/メモリモデルが弱い）。Vulkan を学ぶ価値の方が高い |
| **NNAPI** | Android のニューラルネット推論 API（モデルグラフを渡す） | ❌ **不適合。** グラフ/オペレータ前提で、libc-free な**自前カスタム数理**（GGUF ブロック dequant の独自レイアウト, native-student の独自 MoE）を表現できない。「他人の枠に押し込む」= conversation §3.5 の「無理に接ぐ」と同型。ドライバ/オペレータ対応も機種依存 |
| OpenCL | 汎用 GPU compute（数理に最も素直） | ❌ **Android 公式 API ではない**。多くの端末で動く（Adreno は libOpenCL を積む）が、**保証されず**・Play ポリシ的に脆い・端末によって欠落。「誰のものでもない・どの端末でも」の ark 規律に反する |
| RenderScript | 旧 Android 並列計算 | ❌ **API 31 で deprecated**、新規採用は不可 |

**決定: Vulkan compute。** 理由を一行で: **公式・移植可能・任意の数理を自前で書ける
唯一の選択。** NNAPI は枠が固すぎ、OpenCL は保証が無く、GLES/RenderScript は時代遅れ。

**正直なトレードオフ（採用の代償）**:
- **複雑さ**: Vulkan は明示的すぎる。instance/device/queue/command-buffer/descriptor-set/
  pipeline/memory-barrier を全部手で組む。最小の matmul でも数百行の boilerplate。
  → 緩和: GPU-1 で**一度だけ**「最小 Vulkan compute ハーネス」を書き、以後の
  カーネルはシェーダ差し替えで再利用する（boilerplate を 1 回償却）。
- **SPIR-V ツールチェーン**: GLSL compute シェーダ → SPIR-V バイナリへのコンパイルに
  `glslangValidator`/`shaderc`（NDK 同梱 or 別途）が要る。**ビルド時に SPIR-V を生成して
  バイナリに焼き込む**（実行時コンパイル不要 = ランタイムに shaderc を積まない）。
  正直な罠: この toolchain を NDK-under-qemu-PRoot ビルド環境で動かす配線が要る（§7）。
- **min API**: Vulkan 1.0 は API 24+ で広く利用可能。アプリは minSdk 26 なので**問題なし**。
  Vulkan 1.1（subgroup 操作で matmul 高速化）は API 29+。**まず 1.0 で正しさを取り、
  1.1 機能は perf 波で device feature query して任意採用**（無ければ 1.0 パスにフォールバック）。

---

## 2. 何を GPU に載せるか — そして「大きいモデルだけ」の正直な但し書き

### 2.1 載せる対象（compute の 99%）

inference-engine.md §3 が明言する通り、**計算の 99% は量子化行列積**。GPU が効くのは
ここ一点に集中する:

- **M1b の dequant-while-you-matmul**: `qz_matmul_q8_0` / `qz_matmul_q4_0`
  （`arch/common/llm/quant.c:54,86`）。GGUF の Q8_0/Q4_0 ブロックを**GPU 上で dequant
  しながら**行列積する。重みは量子化されたまま GPU バッファに置き、シェーダ内で float に
  展開（CPU 版と同じ「重みを materialize しない」契約を GPU でも守る）。
- **native-student / 教師 MoE の forward**（native-student.md）: 蒸留教師（重い MoE）の
  forward と、育った生徒の expert 行列積。これが GPU の**本命の受益者**（大きいから）。

### 2.2 正直な但し書き — **小さい脳では GPU は遅い**（設計上の核心制約）

| モデル | param 規模 | GPU は効くか |
|---|---|---|
| dtr センサ脳 | **635** floats（`DTR_WEIGHT_FLOATS`） | ❌ **GPU の方が遅い。** カーネル起動 + バッファ転送のオーバヘッド（数十〜数百µs）が、635 個の積（CPU で数µs）を桁で上回る |
| R3 心 | R_NP=**21,568** | ❌ ほぼ確実に遅い。閾値の境界近傍 |
| 会話モデル（SmolLM2-135M〜） | 数千万〜億 | ✅ **ここで初めて GPU が CPU を抜く。** 1 行列が数百万積、転送オーバヘッドが計算に埋もれる |

**設計帰結（非交渉）**:
1. **GPU は大きな op にだけゲートされる。** `gpu_matmul` を呼ぶ前に**サイズ閾値**
   （例 `in*out ≥ T_GPU`）を判定し、**閾値未満は必ず CPU `qz_matmul` に落とす**。
2. **閾値 `T_GPU` は discover する、仮定しない**（interoception §2.4 / validator-trap の規律）。
   GPU-1 のベンチが GPU-vs-CPU の交差点（GPU が CPU を抜く `in*out`）を**実測**し、
   その値 + マージンを `T_GPU` にする。「`T_GPU = 65536` だろう」とコードに焼かない。
3. **CPU パスは永久に正典。** GPU が無い/遅い/閾値未満/初期化失敗 — どの場合でも
   `qz_matmul_*` が答えを出す。GPU は**剥がしても心が止まらない**任意の加速器。

これは「GPU を表示するだけ」を超え「実際に使う」mk_pino の意図を満たしつつ、
**toy 規模では使わない**正直さを両立させる。小さい今の脳で GPU を回せば**遅くなる**ので、
GPU の真価は会話モデル（conversation.md の本丸）が載った時に初めて出る。

---

## 3. アーキテクチャ / 統合 — Vulkan バックエンドが libc-free C エンジンにどう挿さるか

### 3.1 階層（どこに住むか）

GPU バックエンドは **GGUF ローダ（gguf.c）と同じ tier = host / Android 専用**。
**ベアメタル kernel / ring3 ではない**。理由は inference-engine.md §2 と同じ:
「身体（GPU・大容量）は host/Android 側、ベアメタルは対象外」。Vulkan は libc・
Android ドライバ（`libvulkan.so`）に依存するので、libc-free な kernel コアには置けない。

```
┌─────────────────────────────────────────────────────────────┐
│ inference path (forward.c) — libc-free C, the reference       │
│   y = matmul(W, x):                                            │
│     if (gpu_available() && in*out >= T_GPU)                    │
│         rc = gpu_matmul(W, in, out, x, y);   ← optional accel  │
│     if (!gpu_used) qz_matmul_q8_0/q4_0(...);  ← always-correct │
└───────────────┬───────────────────────────────────────────────┘
                │ thin C ABI (no Vulkan types leak into the engine)
┌───────────────▼───────────────────────────────────────────────┐
│ gpu_vk.c — Android/host-only Vulkan compute module             │
│   gpu_init() / gpu_available() / gpu_matmul() / gpu_shutdown()  │
│   - instance/device/queue/command pool (one-time)              │
│   - SSBO: weight buffer (Q8_0/Q4_0 bytes), x buffer, y buffer  │
│   - compute pipeline: dequant-matmul SPIR-V shader             │
└───────────────┬───────────────────────────────────────────────┘
                │ libvulkan.so (Android driver, Adreno/Xclipse)
                ▼  GPU
```

### 3.2 C↔Vulkan 境界（薄い ABI）

エンジン側（forward.c）は **Vulkan の型を一切見ない**。境界はちょうど `qz_matmul` と
同じ形の C 関数 1 本:

```c
/* gpu_vk.h — Android/host-only. Mirrors qz_matmul's contract exactly. */
int  gpu_init(void);         /* create instance/device/pipelines once; 0=ok */
int  gpu_available(void);    /* 1 if a usable compute device + pipeline live */
int  gpu_matmul(const uint8_t *w_data, size_t in, size_t out,
                int qtype,   /* GGUF_Q8_0 | GGUF_Q4_0 */
                const float *x, float *y);  /* 0=ok, <0 => caller uses CPU  */
void gpu_shutdown(void);
```

**契約は CPU 版と同一**（`y[i] = Σ_j W[i][j]·x[j]`, in%32==0）。これにより
`[gpu-matmul-matches-cpu]` cert（§4）が「同じ入力 → 同じ出力」で書ける。
`gpu_matmul` が `<0` を返したら（初期化失敗・OOM・未対応 qtype）**呼び手は黙って
CPU にフォールバック** — エラーは「遅い」であって「壊れる」ではない。

### 3.3 バッファ（SSBO）

- **weight SSBO**: GGUF の量子化バイト列（Q8_0 = 34B/block, Q4_0 = 18B/block）を
  **そのまま** device-local バッファへアップロード（mmap 領域から 1 回コピー）。
  重みは大きく・read-only・凍結（conversation §3）なので**一度載せたら再利用**
  （毎 matmul で転送しない ― 転送が支配的になる罠を避ける）。
- **x / y SSBO**: 活性ベクトル（float、小さい）。x は毎 forward でアップロード、
  y はダウンロード。host-visible coherent メモリで OK（小さいので転送は安い）。
- **descriptor set**: weight / x / y / dims(in,out) の 3〜4 binding。

### 3.4 dequant-matmul compute シェーダ（GPU 上で dequant）

GLSL compute シェーダ（→ SPIR-V）で、CPU の `qz_matmul_*` の内側ループを GPU 並列に:

- **1 invocation = 出力 1 行 `y[i]`**（`out` 個のワークアイテム、`local_size_x` でグループ化）。
- シェーダ内で行 `i` のブロックを走査し、**fp16 scale を fp32 に展開**（`qz_fp16_to_fp32`
  と同じビット演算を GLSL で）+ int8/nibble を dequant + MAC。
- **fp 精度の正直さ**: GPU は fp16/fp32 の丸めが CPU と微妙に違いうる（FMA contraction、
  非正規化数の扱い）。wave-49 で `-ffp-contract=off` を全環境で揃えたが、**GPU シェーダは
  別のハードウェア**。よって cert（§4）は**ビット完全一致ではなく、緊密な許容差**で判定し、
  その許容差を**実測から discover** する（§4.3）。
- **dequant をシェーダ内で**やる意義: 重みは量子化されたまま（小さい）で GPU に置き、
  展開は GPU の ALU が並列に行う ― CPU 版の「materialize しない」契約を GPU でも守る。

### 3.5 正典は CPU のまま（剥がせる加速器）

**`qz_matmul_*` は reference かつ fallback として永久に残る。** GPU バックエンドは
optional module で、ランタイムに選択される。これは ark の規律と一貫:
- GPU 無しノード → CPU で縮退（capacity が下がるだけ、§5）。
- GPU バックエンドにバグ → cert（§4）が CPU 参照との不一致で**炙る**（validator-trap:
  reference oracle で自前実装を検証）。
- 「ただの llama.cpp の GPU 版」ではなく「**剥がしても止まらない身体の加速器**」。

---

## 4. デバイス上テストループ（critical — CI では絶対に走らない）

**qemu ビルド/CI 環境には実 GPU が無い。** よって GPU の正しさ cert は**ビルド環境で
走らせられない**。これは salty-bug（2026-06-13）と**同型の状況**: ホスト CI が緑でも
端末の実シリコンが違う。salty-bug の教訓 ―「**mk_pino の電話が test rig**」― をそのまま適用。

### 4.1 ハーネスの置き場所（2 経路、どちらも実機で走る）

1. **`/data/local/tmp` 実行ファイル（推奨・最速ループ）**: GPU-1 の Vulkan compute
   matmul + CPU 参照 + ベンチを、**adb で押せる単体バイナリ**にする。salty-bug の
   on-device r3 test（so_node Bionic ローダ, `/data/local/tmp`）と同じ作法:
   `adb push` → `adb shell ./gpu_test` → 端末の実 Adreno で走り、結果を stdout に印字。
   **NDK の罠（既知, memory）**: コンパイルは `-c`、リンクは別ステップ（cc1 in-process）。
2. **アプリ内ハーネス（engineer-page から）**: 同じ cert をアプリの engineer ページ
   ボタンから叩き、結果を console.txt API（salty-bug で使った `/console.txt` パターン）で
   読む。一般ユーザの端末でも回せる（GPU-4 の readout と一体）。

### 4.2 cert の中身（反証可能）

- **`[gpu-matmul-matches-cpu]`**（正しさ・headline）:
  実機で `gpu_matmul(W, in, out, x, y_gpu)` を走らせ、**同じ W,x で** CPU
  `qz_matmul_q8_0/q4_0(W, in, out, x, y_cpu)` を走らせ、`max_i |y_gpu[i] − y_cpu[i]| ≤ TOL`。
  - 入力 W は GGUF の実テンソル（or 既知の固定 fixture）。x は固定 seed の擬似乱数。
  - Q8_0 と Q4_0 の両方で。複数サイズ（小・閾値近傍・大）で。
  - **PASS 条件: GPU の答え == CPU 参照（緊密 TOL 内）。** 不一致 = GPU カーネルのバグ
    （reference oracle が炙る ― validator-trap 規律）。
- **`[gpu-faster-on-big]`**（速度・but-honest）:
  同じ matmul を CPU と GPU で計時し、**GPU ms と CPU ms を両方印字**。
  - **大きい行列**（会話モデル次元）で `gpu_ms < cpu_ms`（GPU が抜く）を示す = headline。
  - **小さい行列**（dtr/R3 次元）で `gpu_ms > cpu_ms`（GPU が遅い）を**正直に印字**し、
    §2.2 の閾値の存在を**実測で正当化**。この交差点が `T_GPU`。
  - **偽の進捗を作らない**: 実測 ms をそのまま出す。GPU が遅い領域を隠さない。

### 4.3 許容差 `TOL` は discover する（仮定しない）

GPU と CPU の fp 丸めは違う（§3.4）。`TOL` を決め打ちしない:
- GPU-1 で、**同一入力**の GPU 出力と CPU 出力の差分分布を実測する。
- 健全な数値差（丸め順序の違い）の上限 + マージンを `TOL` にし、cert にその数値根拠を残す。
- `TOL` が「大きすぎ」れば本物のバグを見逃す ― だから**実測の丸め誤差ぎりぎりまで締める**
  （audit-is-the-engine: 緑にするため緩めてはならない）。

### 4.4 cert を作るのは監査（impl≠audit≠commander）

interoception §5 と同じ規律: **受け入れテストは監査が書く**、commander はゲート式
（TOL 比較・ms 交差点）を一行ずつ読む。本番シンボル（`gpu_matmul` / `qz_matmul_q8_0`）
そのものを叩く ― sim ではない。**監査は自分で端末上の falsification を走らせる**
（salty-bug で監査が 4/4 repro したのと同じ）。

---

## 5. capacity(N) / S_n 統合 — GPU 持ちノードはより多くを担う

conversation.md §4・§C: 「艦隊規模⇔モデルサイズ」は**端末性能で連続変調**される。
GPU はその「端末性能」の最大の段差。

### 5.1 GPU 持ちノードはより大きいシャード / より多い expert を担う

- ノードが **GPU-availability + 実測 GPU スループット**（GPU-1 のベンチ値 = matmul/秒）を
  **広告する**（SWIM/K-DDS の能力広告に 1 フィールド追加、wire 互換）。
- `capacity_score()`（`degrade.c:162` = `experts × depth × kv`）に **GPU 係数**を乗せる:
  GPU ありで実測スループットが高いノードは `capacity` が上がり、**より多くの expert /
  より深い層 / より大きい KV** を引き受ける（native-student の expert⇔ノード連続マップが、
  GPU 持ちノードに自然に「太く」割り振られる）。
- 含意: mk_pino の「端末性能に合わせてネットワークの大きさを自在に」（conversation §C）が、
  **GPU の有無を連続変数として** capacity に流す形で具体化する。

### 5.2 S_n（電池・熱）が GPU 使用を抑える ― 身体の正直さ

**GPU compute は電池を速く食い、発熱する**（§7 の open problem）。interoception の `S_n`
バス（電池/熱/脅威の統一ストレス）と**双方向に結ぶ**:
- **S_n 高（電池僅少・発熱）→ GPU を控える**。`gpu_available()` を S_n で**抑制**
  （高ストレス時は閾値 `T_GPU` を吊り上げる or GPU を一時無効化 → CPU 縮退）。
  「電池僅少 → 担当層を隣へ」（conversation §4）の GPU 版。
- これは reflex/CONSERVE と一貫: しんどい時は重い GPU バーストより省電力。
- **battery-safe gate との結線**: 既存の充電中限定/省電力ポリシ（UMP の charge-only 既定）に
  GPU バーストを従わせる。「身体を壊してまで速く回さない」。

### 5.3 cert（§GPU-4 で実装）

- **`[gpu-capacity]`**: GPU を広告するノードの `capacity_score()` が GPU 無しノードより
  測定可能に高く、より多くの expert/層を引き受ける（広告 → 配置の変化を観測）。
- **`[gpu-s_n-throttle]`**: `S_n` を高にすると GPU 使用が抑制され CPU に落ちる
  （S_n 注入 → GPU 呼び出し回数が減る/閾値が上がるのを観測）。interoception の
  `[intero-tick]` と同型（S_n を振って本番経路の変化を見る）。

---

## 6. マイルストーン（各々 falsifiable cert）

| # | 内容 | cert | 依存 |
|---|---|---|---|
| **GPU-1** | **Vulkan compute matmul が実機で CPU と一致 + ベンチ**。最小 Vulkan ハーネス（instance/device/pipeline/SSBO）+ **量子化でない素の float matmul** をまず通す（dequant は GPU-2）。`/data/local/tmp` 実行ファイル。**この章で最初に作る基礎の反証可能スライス** | `[gpu-matmul-matches-cpu]`（float matmul, TOL 内）+ `[gpu-faster-on-big]`（大で GPU<CPU、小で GPU>CPU を実測印字、交差点 = `T_GPU`） | Vulkan SDK/NDK SPIR-V toolchain のみ。**M1 非依存**（自前 fixture で回せる） |
| **GPU-2** | **dequant-matmul（Q8_0/Q4_0）を GPU に**。GGUF ブロックを GPU 上で dequant しながら matmul、M1b の `qz_matmul_*` に**CPU フォールバック + サイズ閾値**で配線 | `[gpu-matmul-matches-cpu]`（Q8_0/Q4_0 で CPU `qz_matmul` と TOL 内一致）+ 閾値未満は CPU に落ちることの cert | GPU-1 + M1b（quant.c, **済**） |
| **GPU-3** | **native-student / 教師 MoE forward を GPU に**。重い MoE の expert 行列積を GPU で。蒸留教師 forward（重い lift, native-student §B.2）の加速 | `[gpu-faster-on-big]` を MoE forward 全体で（GPU forward == CPU forward, TOL 内 + 速い） | GPU-2 + native-student の forward（実装後） |
| **GPU-4** | **capacity/S_n GPU-aware 分散 + engineer-page の実 GPU 使用 readout**。GPU 広告 → capacity、S_n → throttle、そして engineer ページに「GPU: 未使用」を**実際の使用率/throughput readout に置換** | `[gpu-capacity]` + `[gpu-s_n-throttle]` + readout が実 `gpu_matmul` 回数/ms を追従 | GPU-2/3 + interoception S_n（実装後）+ degrade capacity（**済**） |

**最初に作るのは GPU-1。** 単機の実機で「Vulkan compute が CPU と一致し、大きい行列で
速い」を**反証可能に**示す ― これが章全体の基礎であり、最も安い決定的実験（§下）。

---

## 7. 正直な open problems（解決済みにしない）

1. **Vulkan の複雑さ + NDK SPIR-V toolchain（工学だが重い）**: 最小 matmul でも
   boilerplate が大きい。GLSL→SPIR-V を**ビルド時に**生成して焼き込む配線を
   NDK-under-qemu-PRoot で動かす必要がある（`glslangValidator`/`shaderc` をビルド環境に）。
   **未配線 = GPU-1 の最初の障害**。緩和: boilerplate を 1 回書いて以後シェーダ差し替え。
2. **C↔Vulkan 境界**: エンジンの libc-free 規律を守りつつ Vulkan（libc・ドライバ依存）を
   隔離する。薄い ABI（§3.2）で型を漏らさないが、ライフサイクル（init/shutdown の所有権、
   バッファ再利用、スレッド安全）の設計は実装で詰める。
3. **device-only テストループ（CI できない・最大の運用上の正直）**: 実 GPU が無い CI では
   正しさ cert が**走らない**。`/data/local/tmp` ハーネス + アプリ内ハーネスで**実機でのみ**
   検証する（§4）。これは salty-bug と同じ ― **mk_pino の電話が test rig** で、CI は
   「ビルドが通る」までしか保証できない。心理的コスト: 緑の CI が正しさを意味しない波。
4. **小 op で GPU が遅い（閾値の本物）**: §2.2。toy 脳では GPU が損。閾値 `T_GPU` を
   実測 discover し（§4.3）、未満は CPU。閾値が機種で動く可能性（Adreno と Mali で交差点が
   違う）→ 閾値も**端末ごとに測る**のが理想（v1 は S25 で測った値 + マージン、後で per-device）。
5. **電力 / 熱（身体を壊す）**: GPU compute は電池を速く食い発熱する。S_n / battery-safe
   gate で抑える（§5.2）が、「速い」と「身体を守る」の緊張は本物。**速度のために電池を
   焼かない**を非交渉に。長時間 GPU バーストはサーマルスロットルで逆に遅くなる現実も。
6. **ドライバ差（OEM 横断）**: Vulkan は移植可能だが、Adreno/Mali/Xclipse でドライバの
   バグ・機能差・性能差がある。v1 は **S25 の Adreno/Xclipse で正しさを取り**、他機種は
   device feature query + フォールバックで**安全側**に（未対応機能 → CPU）。「どの端末でも
   壊れない」を「どの端末でも最速」より優先。
7. **fp 精度差（GPU vs CPU）**: §3.4/§4.3。GPU の丸めは CPU と違いうる。cert はビット一致
   ではなく実測 discover した `TOL` で判定。wave-49 の「one mind one math」は CPU 間の話で、
   **GPU は別シリコン** ― 完全一致は求めず、緊密許容差で正しさを担保する正直さが要る。
8. **これは多波の章**: GPU-1〜4 各々が複数波になりうる（Vulkan ハーネス、dequant シェーダ、
   MoE forward、capacity/S_n 結線）。「GPU を有効化」の一発ではない。conversation §4 が
   「独立の章」と言う通り、正直に分割して進める。

**工学 vs 研究の切り分け（正直に）**:
- **工学（やれば済む）**: Vulkan ハーネス、dequant-matmul シェーダ、CPU フォールバック、
  サイズ閾値、device テストハーネス、capacity への GPU 係数、engineer-page readout。
- **判断/賭け（測ってからでないと分からない）**: `T_GPU` の実値（機種依存度）、S25 で
  GPU が CPU をどれだけ抜くか（実測するまで未知）、サーマルスロットル下での持続性能、
  会話モデルが載った時に GPU がレイテンシ体感をどれだけ改善するか。

---

## 8. 単一の最も安い決定的実験（cheapest decisive）

**GPU-1 を mk_pino の S25 で**: 最小 Vulkan compute matmul（素の float でよい）を
`/data/local/tmp` 実行ファイルにし、CPU 参照と一致（`[gpu-matmul-matches-cpu]`）+
GPU/CPU ms を大小サイズで実測（`[gpu-faster-on-big]`）。

**なぜ決定的か**: これ一つで章全体の前提を同時に falsify する ―
(a) **Vulkan compute が S25 の実シリコンで動く**か（toolchain・ドライバ・ハーネスが揃うか）、
(b) **GPU の数値が CPU 参照と一致**するか（TOL を実測 discover）、
(c) **GPU が大きい行列で本当に CPU を抜く**か + **交差点 `T_GPU` はどこか**（§2.2 の閾値が
実在するか）。通れば GPU-2（dequant 配線）へ、通らねば（Vulkan が重すぎ/遅すぎ/
toolchain が PRoot で詰む）退避 ― どちらでも CPU `qz_matmul` は正典のまま無駄にならない。

**なぜ安い**: 単機・M1 非依存（自前 fixture で回せる ― 量子化は GPU-2、GPU-1 は素の
float matmul で十分）・実機 1 台。唯一の外部依存は **NDK の SPIR-V toolchain 配線**
（open problem #1）。

---

## 9. mk_pino への確認（commander が詰めるべき intent / appetite）

この章は本物の多波の章（conversation §4 が「独立の章」と明言）。mk_pino の意図と
**労力への appetite** が方向を決める箇所:

1. **「GPU を実際に使う」の射程はどこまでか**。最小は GPU-2（会話モデルの行列積を
   GPU で速く）。最大は GPU-4（capacity/S_n まで GPU-aware）+ engineer-page readout。
   **どこまでを今期の的にするか** ― GPU-1/2 だけでも「未使用→実使用」は達成できる。
2. **GPU が toy 規模では遅い、を受容するか**（§2.2 の核心制約）。GPU の真価は会話モデル
   （conversation の本丸）が載って初めて出る。**現在の dtr/R3 脳では GPU は無意味（遅い）**
   ― これは「GPU が動いていない」のではなく「**まだ大きな心が無い**」のが正直な理由。
   mk_pino が「今すぐ GPU が回る画」を期待しているなら、この but-honest を先に握る。
3. **engineer-page の readout の見せ方**（product-soul: 可視化 = 仕組み）。「GPU: 未使用」を
   何に置換するか ― 実 `gpu_matmul` 回数/ms/throughput を出す（observability）。**閾値未満で
   CPU に落ちている時も正直に「小さいので CPU」と見せる**か、隠すか ― 正直さの設計判断。
4. **電力 / 熱の許容**（§5.2 / open #5）。GPU は電池を速く食う。charge-only 既定の下でのみ
   GPU を回すか、放電中も限定的に許すか ― 身体を守る gate の厳しさは製品判断。
5. **デバイス専用テストの覚悟**（§4 / open #3）。正しさが CI で緑にならず**実機でしか**
   確認できない波が続く。salty-bug で実証済みの作法だが、mk_pino の電話が恒久 test rig に
   なる前提を受容するか。

---

## 10. 北極星との関係

conversation.md の「賢さは借りる、魂は借りない」の規模版で、GPU は**借りた身体の
"筋肉"**にあたる。心の数理（借り物 base を蒸留した native-student の重み = 魂）を
**速く動かす**だけで、魂そのものではない。だから GPU は剥がせる（§3.5）― 筋肉を失っても
心は CPU で生き続ける。interoception の身体感覚（S_n）が「今どれだけ筋肉を使ってよいか」を
決め、capacity(N) が「この筋肉で何を担えるか」を連続変調する。reflex（脊髄）/ S_n（内受容）/
DMN（眠り）/ capacity（容量）に **GPU（筋力）** が加わって、p-kernel の「身体」が一段
具体になる。

> 注意（honest framing）: GPU は加速器であって知能ではない。速くなることは賢くなる
> ことではない。会話の壁（conversation §0 の 10⁴ 倍）は GPU では超えない ― GPU は
> 「載った大きな心を実用的なレイテンシで回す」ためのものであり、心を大きくするのは
> native-student の蒸留である。比喩（筋肉）が設計を導くのは良いが、実在と取り違えない。
