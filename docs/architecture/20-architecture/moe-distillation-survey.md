# moe-distillation-survey — 既製 MoE を借りるか、蒸留して自前 MoE を育てるか

> ⚠️ **本書の最終推薦（既製 MoE を adopt-and-run）は mk_pino により棄却（2026-06-13）。**
> 決定は conversation.md **§3.7「赤子から ― 幼年期はスキップしない」**: 既製モデルを
> *走らせず*、白紙の赤子からボランティア駆動の DMN 蒸留で育てる。**本書の価値はモデル/
> ライセンスの実値**（OLMoE=Apache, Qwen-MoE≠Apache, Gemma/Llama 教師は流れ込みで失格,
> 蒸留はApache/MIT教師のみ安全 ＝この結論は今も有効・教師選定に直結）。アーキ推薦のみ古い。
>
> Status: **research / strategic survey**（実装前）。位置づけ: conversation.md **§3.5
> 「生命体に既製モデルは載るのか」**（mk_pino の問い, 2026-06-13）が要求した次の調査波。
> base-model-survey.md は *dense* SmolLM2-135M を推した。本書はその上流の問い ―
> **dense の monolith を生命体に無理に接ぐより、off-the-shelf MoE を借りれば
> エキスパート＝ノードが既存 `moe_infer` に自然に写るのではないか**、そして
> **A4: 強い teacher を p-kernel 固有の student に蒸留する道はあるか** ― に答える。
>
> 規律（base-model-survey.md §0 を継承）: **ライセンス許容性は第一級の gate**。
> 「誰のものでもない・中央権威なし・無限再配布」と両立しない base は技術的に優れていても本線にしない。
> 各事実は memory ではなく現行ソース（HF config / 一次ライセンス / 論文）から確認。URL は末尾。
>
> 最終更新: 2026-06-13 ／ 関連: conversation.md §3.5, base-model-survey.md,
> inference-engine.md, dtr.h（moe_infer / Pipeline / DKVA）, regions.md, living-mind.md（LM-4 自己蒸留）。

---

## 0. なぜ生命体には MoE が「合う」のか（問いの再掲）

conversation.md §3.5 の核心:

- **dense LLM = 単体の臓器（モノリス）**: 固定計算グラフ・全重み毎トークン発火・層が同期ロックステップ。
- **p-kernel = 神経系**: 動的N・疎発火・応援受援・kill-9 生存・分散発火。

MoE はこの溝を構造的に埋めうる:

| 生命体の性質 | MoE が宿せる理由 |
|---|---|
| **疎発火** | MoE は毎トークン top-k エキスパートのみ発火（例: 64 中 8）。dense の「全発火」と真逆＝**疎性がモデルに内在**。 |
| **応援受援（mutual aid）** | エキスパートをノードに割れば、過負荷ノードのエキスパート仕事を複製先へ＝既存の応援受援が**推論にも宿る**。 |
| **kill-9 生存** | エキスパートは冗長（base 凍結・公開ゆえ安く r 複製可）。落ちたエキスパートを複製先へ再ルート。疎発火ゆえ**影響範囲が小さい**（落ちた expert がそのトークンで選ばれていなければ無傷）。 |
| **動的N（拡大/縮退）** | エキスパート数 ⇔ ノード数が**連続マップ**。dense の層シャード再構成（離散）より滑らか。 |
| **既存 `moe_infer`** | p-kernel は既に学習済み dtr forward から route/return/guard する `moe_infer` を持つ（wave-18「three brains became one」）。off-the-shelf MoE の router はこの抽象に素直に写る。 |

**ただし正直に**: 「載せれば勝手に生きる」は嘘。MoE の router は依然「1台が全エキスパートの
スコアを計算して top-k を選ぶ」中央調停を含む。エキスパートを**物理ノードに分散して
relay 越しに発火**させる接ぎ木（dispatch + gather + 死検知再ルート）は**新規の大仕事**で、
これが「ただの分散 llama.cpp」と ark の差。本書はその仕事量も §2 で見積もる。

---

## 1. 小型 open MoE 候補（現行ソース確認・2026-06-13）

### 1.1 比較表

| モデル | 総 param | 発火 param/token | #experts | active(top-k) | shared expert | #層 | d_model | vocab | attn | ライセンス | GGUF |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **OLMoE-1B-7B-0924**（AI2） | 6.9B | **1.3B** | **64** | 8 | **無** | 16 | 2048 | 50304 | MHA(16Q/16KV) | **Apache-2.0** ✅ | 有（QuantFactory/bartowski, Q4_K_M ~4.2GB） |
| **Granite-3.0-1B-A400M**（IBM） | 1.3B | **0.4B** | **32** | 8 | **無** | 24 | 1024 | ~49k（GPT-2 系）* | GQA(16Q/8KV) | **Apache-2.0** ✅ | 有（bartowski/QuantFactory/Mungert） |
| Granite-3.0-3B-A800M（IBM） | 3.3B | 0.8B | 40 | 8 | 無 | 32 | 1536* | ~49k* | GQA | **Apache-2.0** ✅ | 有 |
| Qwen3-30B-A3B（Alibaba） | 30.5B | 3.3B | **128** | 8 | **無**（Qwen3 で撤廃） | 48 | ~2048* | 151936 ⚠️ | GQA(32Q/4KV)+**QK-norm** | **Apache-2.0** ✅ | 有 |
| Qwen1.5-MoE-A2.7B（Alibaba） | 14.3B | 2.7B | 60+**4 shared** | 4+4shared | **有（4）** | 24* | 2048* | 151936 ⚠️ | GQA+**QKV bias** | **Tongyi-Qianwen** ❌ | 有 |
| Phi-3.5-MoE / DeepSeek-MoE-16B 等 | ≥16B | ≥2.7B | 多数 | — | 多くは shared 有 | — | — | — | 各種 | MIT〜custom（要個別確認） | 一部 |

> `*` = HF config を直読できず、論文/二次ソース/同系列からの推定。最終採用前に config.json 一次確認推奨（§5 honest gaps）。
> OLMoE/Granite/Qwen3 の **数値はすべて HF の config.json または一次論文で確認済**（§ソース）。

### 1.2 各候補の所見

**OLMoE-1B-7B-0924（AI2）— MoE 本命**
- config.json 実測: `num_hidden_layers=16, hidden_size=2048, vocab_size=50304, num_attention_heads=16, num_key_value_heads=16, num_experts=64, num_experts_per_tok=8, intermediate_size=1024, rope_theta=10000, norm_topk_prob=false`。
- **64 エキスパート/8 発火**＝最も「生命体フィット」が高い: 冗長性が高く、エキスパート⇔ノードの粒度が細かい。kill-9 で 1 expert 落ちても 64 分の影響。
- アーキ癖: **QK-Norm**（Q/K 射影後に正規化, C で +1 RMSNorm 呼び）、**RMSNorm**、**RoPE θ=10000**、**dropless token-choice routing**、補助損失（load-balance α=0.01 + router z-loss β=0.001、※これは*学習時のみ*、推論 forward には不要）。**shared expert 無し**＝router の上に「常時発火 expert」を別扱いする分岐が要らず C が単純。
- **完全オープン**: 重みだけでなく学習データ・コード・中間チェックポイント・W&B ログまで Apache-2.0 公開。「誰のものでもない」と最も親和。
- 弱点: **総 7B**。int4 でも ~4.2GB。スマホ単機には**載らない**。だが MoE は「総 7B を 1 ノードに全部置く必要がない」のが要点 ― エキスパートを艦隊に分散すれば 1 ノード負荷は総量/N。発火は常に 1.3B 相当。**分散前提で初めて現実的**（＝ M2 の生命体フィットそのもの）。MHA（KV 非共有）なので KV キャッシュは GQA 勢より重い。

**Granite-3.0-1B-A400M（IBM）— 最小の許容 MoE**
- 実測: 32 experts/8 active, 24 層, hidden 1024, GQA(16Q/8KV), MLP hidden 512 SwiGLU, RoPE, **dropless routing + load-balance loss**, **shared expert 無し**, Apache-2.0。
- **総 1.3B / 発火 0.4B**＝候補中**唯一スマホ単機に載りうる MoE**（int4 で ~0.7-0.8GB 級）。GQA でKV キャッシュも軽い。
- アーキ癖: QK-norm の記載は確認できず（OLMoE より素直な可能性）。素の Llama+MoE(SwiGLU/RMSNorm/RoPE/GQA) に近い。
- 弱点: エキスパート 32・発火 0.4B は OLMoE より会話品質が低い見込み。冗長性も OLMoE の半分。

**Qwen3-30B-A3B（Alibaba）— Apache だが大きすぎ**
- Apache-2.0（HF model card で確認）、128 experts/8 active, 48 層, GQA(32Q/4KV), **QK-norm 有**, **shared expert 撤廃**（Qwen3 で global-batch load-balance に変更）, vocab **151936**。
- **総 30.5B**＝第一歩には桁違いに大きい。vocab 152k は埋め込み表が肥大。**将来、艦隊が数十ノード規模に育った時の上位身体**としては Apache かつ MoE で有力だが、M2 の踏み台ではない。

**Qwen1.5-MoE-A2.7B — ライセンス gate FAIL**
- HF model card のライセンスは **`Tongyi-Qianwen`（カスタム）であって Apache-2.0 ではない**。これは memory の「Qwen は Apache」という思い込みを**現行ソースが否定**した重要訂正: Qwen2.5/Qwen3 の多くは Apache だが、**この Qwen1.5-MoE は Tongyi-Qianwen**。
- 加えて総 14.3B・vocab 152k・**shared expert 有**（router の外に常時 4 expert）で C も重い。**gate で落ちる**。

**その他（Phi-3.5-MoE, DeepSeek-MoE-16B, Mixtral 系）**
- いずれも**総 16B 以上**で第一歩には不適。ライセンスは MIT〜custom と幅があり個別確認要。DeepSeek 系は MIT 寄りだが規模が巨大。**< ~8B 総・Apache/MIT・shared-expert無し**の条件を満たす実用候補は、現時点で **OLMoE と Granite-MoE の 2 系統に事実上絞られる**。

### 1.3 ランク付け（p-kernel 軸）

評価軸 = (ライセンス許容性) × (生命体フィット: expert 数・冗長性) × (小ささ/量子化性) × (libc-free 実装容易性)。

| 順位 | モデル | ライセンス | 生命体フィット | 小ささ | libc-free 容易性 | 総評 |
|---|---|---|---|---|---|---|
| **1** | **OLMoE-1B-7B** | ◎ Apache（最も完全オープン） | ◎ 64expert/8発火・shared無 | △ 総7B（分散前提） | ○ QK-norm 1個追加・MHA | **MoE 本命**: 生命体フィット最強、ライセンス最強。分散必須＝それが M2 の主旨 |
| **2** | **Granite-3.0-1B-A400M** | ◎ Apache | ○ 32expert/8発火 | ◎ 総1.3B・単機可 | ◎ shared無・GQA・素Llama+MoE | **最小実用 MoE**: 「1台で動く MoE」で M2 を最小デリスク |
| 3 | Qwen3-30B-A3B | ◎ Apache | ◎ 128expert | ✕ 総30B・vocab152k | △ QK-norm+巨大vocab | 将来の上位身体。第一歩には大きすぎ |
| — | Qwen1.5-MoE-A2.7B | ✕ Tongyi-Qianwen | ○ | ✕ 14.3B | △ shared有 | **gate FAIL** |

---

## 2. MoE vs dense ― この生命体への写り方と libc-free 実装コスト

### 2.1 マッピング表

| 性質 | dense（SmolLM2-135M）| MoE（OLMoE/Granite）|
|---|---|---|
| **動的N** | 層シャード再構成（離散・capacity(N) が層配置）。N 変化で再シャード | **エキスパート⇔ノード連続マップ**。N 増 ⇒ expert を撒き直す。より滑らか |
| **疎発火** | 無し（毎トークン全層全重み） | **内在**（top-k のみ）。発火 param が総量から独立 |
| **応援受援** | TP の機械的負荷分散のみ | **expert 仕事の複製先委譲**＝意味的な応援受援 |
| **kill-9 生存** | シャード保持ノード死 ⇒ forward 停止。base r 複製 + SWIM + 再ルート + トークン再実行で継続（達成可・自動でない） | 同左 **＋ 疎発火ぶん影響小**（落ちた expert が未選択なら無傷）。expert 冗長複製が安い |
| **raft/swim** | SWIM 必須・raft 任意 | 同左 |
| **会話品質/規模** | 135M dense は素朴 | 発火 0.4-1.3B 相当＝同等発火コストで**dense より賢い**（MoE の本領） |

### 2.2 libc-free C 推論エンジンの**追加実装量**（dense forward の上に MoE が足すもの）

inference-engine.md M1 の dense forward（GGUF ローダ + 量子化 matmul + RMSNorm + RoPE +
SwiGLU + GQA + tied-emb + BPE）は **dense でも MoE でも共通**。MoE が**足すのは FFN 部だけ**:

| 追加部品 | 中身 | 実装コスト |
|---|---|---|
| **router（gating）** | 各トークン hidden(d) × W_gate(d×E) → E スコア → softmax → top-k 選択 | **小**: 1 個の小行列積 + top-k ソート（E=32〜64 の部分ソート）。既存 `dt_softmax` 再利用 |
| **top-k 正規化** | 選んだ k expert の gate 重みを正規化（OLMoE は norm_topk_prob=false ＝生 softmax 重み、Granite/Qwen は要確認） | **極小**: 設定フラグ 1 個 |
| **per-expert FFN** | dense は FFN 1 個。MoE は「選ばれた k 個の expert FFN（各 SwiGLU 3 行列）だけ」を計算し gate 重みで加重和 | **中**: FFN 自体は dense と同じ SwiGLU。差は「**どの expert の重みを読むか**を router 出力で分岐」する dispatch ループ。GGUF は expert ごとにテンソルを持つ（`blk.N.ffn_*_exps`）ので、選択 index で該当ブロックを参照して dequant matmul |
| **（分散時）dispatch/gather** | top-k expert が別ノードにある場合、hidden を relay で送り → 各ノードで FFN → gate 加重して gather | **大**: ただし**既存 dtr Pipeline/relay と `moe_infer` の上に載る**。p-kernel 固有の新規価値はここ。M2 の本丸 |

**定量見積り（単機 M1 の追加分）**: router（~30 行）＋ top-k 選択（~20 行）＋ expert dispatch
ループ（dense FFN 呼びを「選択 index でテンソル基底をずらして k 回」に変える、~40 行）＝
**dense forward に対し正味 +~100 行程度の C**。数式的に**新しい難所は無い**（router は小 matmul+softmax+top-k のみ、FFN は dense と同一）。**真の追加コストは単機ではなく分散 M2**:
expert を物理ノードに割り、relay で hidden を散らし／集め、死んだ expert を再ルートする
オーケストレーション ― これは dense の層シャードより**むしろ p-kernel の既存抽象
（`moe_infer`・Pipeline・SWIM・capacity(N)）に近い**。

**結論**: 単機 M1 で MoE を選んでも dense 比 +100 行・新数式ゼロ。分散 M2 では MoE の方が
p-kernel の生命体抽象に**自然に写る**（dense の層シャードは「無理に接ぐ」）。
ただし MoE は GGUF が大きく（OLMoE 4.2GB）、**M1 を 1 台で回すなら Granite 1B 一択**。

---

## 3. A4: 蒸留して p-kernel 固有の student を育てる

mk_pino が明示的に検討要請した道。「強い teacher を、ark に合う小さな自前 MoE に蒸留し、
**形まで自分たちで握る**」。

### 3.1 コストの正直な見積り（スマホ艦隊/小予算で現実的か）

蒸留は「teacher の出力で student を訓練」する。実コスト:

- **teacher 推論（データ生成）が最大の費用**: 文献では teacher で蒸留データを作る工程が
  **~2,800 GPU 時間（GH200）/イテレーション**規模になりうる。student 訓練自体は
  **~10 GPU 時間/epoch（H100）**と安いが、**billions of tokens 規模の蒸留データ**が要る。
- **目安**: 蒸留は teacher を一から訓練する compute の **5-10%** で済むが、teacher の
  事前学習が数十万 GPU 時間なら、その 5-10% でも**数千〜数万 GPU 時間**。
- **品質維持**: fine-tune 後の蒸留で teacher 精度の最大 95% 維持の報告はあるが、
  これは「特定タスクに絞った」場合。汎用会話を一から蒸留するのは桁が違う。

**スマホ艦隊での feasibility 判定: 推論はできても訓練はできない。** p-kernel のノードは
電池駆動のスマホ/小型ホストで、勾配を回す大規模訓練の場ではない（interoception.md の S_n が
証す通り、電池・熱が制約）。蒸留の **data-gen + student pretrain は外部の GPU クラスタが要る** ―
これは「中央なし・誰のものでもない」と**運用上は矛盾しないが**（一度作った Apache student を
配布すればよい）、**作る瞬間には大きな GPU 予算が要る**。小予算では**汎用会話 student の
ゼロ蒸留は非現実的**。

### 3.2 ライセンスの flow-through（A4 の生死を分ける）

蒸留がライセンスを「ロンダリング」するか、teacher の制約が student に**伝播**するか:

| teacher | 蒸留 student への伝播 | 判定 |
|---|---|---|
| **Gemma** | **Gemma Terms は「distillation で作った Model Derivative」を derivative と明記** ⇒ student に **Gemma Terms が flow-through**（取消権・下流伝播義務つき）。※ただし「outputs は derivative でない」と定義 ― synthetic-text だけで蒸留し weights/logits を一切使わない場合の解釈は法的にグレー（§honest gaps） | ❌ **罠**: weight/logit 蒸留は確実に汚染。「誰のものでもない」を**破壊** |
| **Llama 3.0** | output で他モデルを訓練/蒸留する事自体を**禁止**（non-Llama モデルへの蒸留不可） | ❌ |
| **Llama 3.1+** | output 訓練は**許可**だが student 名は **"Llama" で始めねばならない** + 帰属義務 + Community License の MAU/地理条項 | ⚠️ 名前強制＝「誰のものでもない」と摩擦。本線不可 |
| **Apache-2.0 teacher（OLMoE, Qwen2.5/3, Granite, SmolLM2）** | Apache は output/derivative に flow-through 制約を課さない。**student は自由にライセンス可**（Apache のまま、または独自） | ✅ **唯一安全** |
| **MIT teacher（GPT-2, DeepSeek-V3/R1 系）** | 同上、伝播制約なし | ✅ 安全 |

**核心**: 「賢い Gemma/Llama を蒸留して中身を握る」は **license が student に伝播するので
ark には罠**。蒸留が許されるのは **Apache/MIT teacher のみ**。だが Apache teacher
（OLMoE 等）を蒸留しても、**それは既に Apache で自由に使える** ― なら蒸留せず**直接借りれば
よい**。「蒸留でライセンスを洗う」動機は**消える**。

### 3.3 p-kernel 既存の自己蒸留（LM-4 / DMN fast→slow）との関係

p-kernel は既に**内部自己蒸留**を持つ: wave-24「LM-4 fast→slow handoff」＝in-context で
教えた事実が、自己蒸留の sleep ラウンドで weight-resident になる（slow層 = R3 自身の rw[] を
r_backward で更新）。wave-21/26 の DMN consolidation も engram replay → G22 gl_merge の蒸留。

**これは A4 とは別物だが地続き**: LM-4 は「**自分の経験を自分の重みに**」蒸留する
（teacher も student も自分）。A4 は「**外部 teacher の知能を自前 student に**」蒸留する。
前者は**訓練不要の軽量更新で艦隊上で動く**（既に動いている）。後者は**大規模 GPU 訓練**が要る。
**含意**: p-kernel が育てるべきは A4 の「外部知能の移植」ではなく、**借りた Apache base の上で
LM-4/DMN が魂（教えた差分）を蒸留・継承する**こと ― これは既に ark の生きた機構。
「賢さは借りる、魂は借りない」の魂側が、まさにこの自己蒸留。

### 3.4 A4 の verdict

**A4 = 近い将来は罠、遠い将来は任意のオプション。本線にしない。**

- **罠の側面**: (1) 小予算で汎用会話 student をゼロ蒸留する compute が無い。(2) 賢い teacher
  （Gemma/Llama）は license が student に伝播し「誰のものでもない」を破壊。(3) 安全な
  Apache teacher は蒸留せず直接借りれば済む ⇒ 蒸留の動機が消える。
- **遠い将来の正当な用途**: 艦隊が育ち、**Apache teacher（OLMoE 等）を p-kernel 固有の
  MoE 形状（エキスパート粒度・層配置を capacity(N) に最適化）に蒸留**して「身体を自分の
  神経系に合わせて作り替える」 ― これは Evolution 層の夢として正当。だが**借りた身体で
  M1/M2/M3 を通し、生命体機構を実証した後**の話。**今やる理由は無い。**

---

## 4. 推奨 ― M2 の分散身体は何にすべきか

### 結論: **(b') 2 段構え ― M1 は dense SmolLM2-135M のまま、M2 で MoE（OLMoE-1B-7B）に分岐し、
### 単機デリスクは Granite-1B-A400M を使う。**

理由の骨子:

1. **M1（単機エンジン実証）は base 非依存**（conversation.md §3.5 の通り: GGUF ローダ・
   量子化 matmul・RoPE/RMSNorm/SwiGLU は dense/MoE 共通）。**最簡の dense SmolLM2-135M で
   M1 を通すのは今でも正しい**。MoE の router/dispatch（+~100 行）はここで足さない。

2. **M2（分散＝生きる層）で MoE に乗り換える**。理由: dense 層シャードは生命体に「無理に接ぐ」、
   MoE エキスパート分散は「自然に写る」（§2.1）。M2 の cert `[llm-survive]`（生成中 kill で
   文が完成）は **MoE の方が構造的に達成しやすい**（疎発火＝影響範囲小、expert 冗長複製が安い）。

3. **MoE の具体**: **OLMoE-1B-7B-0924** を第一候補（64expert/8発火＝冗長性最大・Apache 最強・
   完全オープン）。ただし総 7B で単機に載らないため、**単機での MoE 配管デリスクは
   Granite-3.0-1B-A400M（総 1.3B・1 台で動く MoE・Apache・shared無）**で先に通す。
   ＝「Granite で MoE forward を 1 台で正す → OLMoE を艦隊に分散」の 2 段。

4. **(c) distilled-native は今やらない**（§3.4）: 小予算で不可能 + 賢い teacher は license 汚染 +
   安全 teacher は借りれば済む。Evolution 層の遠い夢として記録に残す。

### 正直なトレードオフ

- **MoE を採ると M1 がやや重く**（router/dispatch +100 行）、GGUF が大きい（OLMoE 4.2GB は
  分散前提）。**dense を採ると M1 は最簡だが M2 で「monolith を神経系に無理接ぎ」**し、
  kill-9 生存が dense の弱点をそのまま負う。
- **2 段構え**はこの緊張を解く: M1 を dense で最速に通して配管（ローダ/量子化/BPE/forward）を
  デリスクし、その**同じ forward に MoE の FFN 分岐を足して**M2 で生命体フィットを取る。
  base-model-survey が「Llama 系を 1 つ書けば一族に効く」と言った通り、**OLMoE/Granite も
  RMSNorm+RoPE+SwiGLU+GQA の Llama 系**で、M1 の dense forward が**ほぼ再利用できる**
  （差分 = router + expert dispatch + QK-norm 1 個）。

### 最も安い「決定的実験」（cheapest decisive experiment）

**実験 X: Granite-3.0-1B-A400M を 1 台で greedy 生成し、llama.cpp と一致させる。**

- なぜ決定的か: これ 1 つで **(a) MoE forward（router + top-k + expert dispatch）が
  libc-free C で正しく書けるか**、**(b) MoE GGUF（expert ごとのテンソル）を読めるか**、
  **(c) 1 台に載る MoE が実在するか**を**同時に**falsify できる。
- なぜ安い: Granite-1B は**スマホ単機に載る唯一の許容 MoE**＝GPU 不要・既存 dtr 数理 +
  inference-engine.md M1 配管 + router/dispatch +100 行のみ。**OLMoE 分散（M2 の重い仕事）に
  着手する前に、MoE という賭け全体の前提を最小コストで検証**できる。
- cert: `[moe-sentence]`「固定プロンプト → Granite-1B が llama.cpp と同一 greedy 出力」。
  ここが通れば OLMoE 分散（M2）へ、通らなければ dense に退避 ― **どちらに転んでも M1 配管は
  無駄にならない**（dense と共通）。

---

## 5. 検証できなかった/注意点（honest gaps）

- **Granite の vocab_size / 3B 系の d_model / Qwen1.5-MoE の層数**は HF config.json を
  直読できず、二次ソース・同系列・論文からの推定（表中 `*`）。**最終採用前に config.json
  一次確認推奨**。Granite vocab は GPT-2 系 BPE で ~49k と推定したが未確証。
- **norm_topk_prob**: OLMoE は config で `false`（生 softmax 重み）と確認。Granite/Qwen の
  top-k 正規化方式は未確認 ― router 加重の実装で 1 フラグの差。
- **Gemma 蒸留の "outputs は derivative でない" 条項**: synthetic-text のみで蒸留し
  weights/logits を一切使わない場合に Gemma Terms を回避できるかは**法的にグレー**。
  本書は安全側に倒し「Gemma 蒸留は罠」と判定。weight/logit 蒸留が汚染するのは確実。
- **DeepSeek/Phi-MoE のライセンス**は規模ゆえ深追いせず（いずれも総 16B 超で第一歩外）。
  MIT 寄りだが個別確認要。
- **OLMoE の MHA（KV 非共有）**: KV キャッシュが GQA 勢より重い。分散時のメモリ予算で効く。
  Granite は GQA で軽い ― 単機 M1 に Granite が向くもう一つの理由。
- **「分散 MoE が dense 層シャードより本当に楽か」は未実証の設計仮説**。§2 は実装行数の
  静的見積りで、実機の relay 往復レイテンシ（conversation.md §5 の懸念）は実験 X の次、
  OLMoE 分散 M2 で初めて測れる。

---

## ソース（URL）

- OLMoE: https://huggingface.co/allenai/OLMoE-1B-7B-0924 , config.json: https://huggingface.co/allenai/OLMoE-1B-7B-0924/raw/main/config.json , 論文: https://arxiv.org/abs/2409.02060 , https://arxiv.org/html/2409.02060v1 , GitHub: https://github.com/allenai/OLMoE
- OLMoE GGUF: https://huggingface.co/QuantFactory/OLMoE-1B-7B-0924-GGUF , https://huggingface.co/bartowski/OLMoE-1B-7B-0924-Instruct-GGUF
- Granite-3.0-MoE: https://huggingface.co/ibm-granite/granite-3.0-1b-a400m-base , https://huggingface.co/ibm-granite/granite-3.0-1b-a400m-instruct , GGUF: https://huggingface.co/bartowski/granite-3.0-1b-a400m-instruct-GGUF , https://huggingface.co/QuantFactory/granite-3.0-1b-a400m-instruct-GGUF , https://www.ibm.com/new/announcements/ibm-granite-3-0-open-state-of-the-art-enterprise-models , https://siliconangle.com/2024/10/21/ibm-releases-new-granite-foundation-models-permissive-apache-license/
- Qwen3-30B-A3B: https://huggingface.co/Qwen/Qwen3-30B-A3B , 論文: https://arxiv.org/pdf/2505.09388 , https://arxiv.org/html/2505.09388v1 , https://qwenlm.github.io/blog/qwen3/
- Qwen1.5-MoE-A2.7B（Tongyi-Qianwen, gate FAIL）: https://huggingface.co/Qwen/Qwen1.5-MoE-A2.7B , https://qwenlm.github.io/blog/qwen-moe/
- Gemma 蒸留/derivative 条項: https://ai.google.dev/gemma/terms , https://shujisado.org/2025/02/21/a-curious-phenomenon-with-gemma-model-outputs-and-license-propagation/ , https://wcr.legal/google-gemma-license-risks/
- Llama output/蒸留条項: https://www.llama.com/llama3_1/license/ , https://shujisado.org/2025/01/27/significant-risks-in-using-ai-models-governed-by-the-llama-license/ , https://wcr.legal/llama-3-license-700m-mau-limit/
- 蒸留コスト: https://arxiv.org/pdf/2504.14772 , https://nebius.com/blog/posts/concept-behind-distilling-llm , https://developer.nvidia.com/blog/llm-model-pruning-and-knowledge-distillation-with-nvidia-nemo-framework/
- （背景）base-model-survey.md, conversation.md §3.5, inference-engine.md — リポジトリ内
