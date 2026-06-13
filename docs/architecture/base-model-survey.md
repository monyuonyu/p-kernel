# base-model-survey — どの OSS base を借りるか（分岐 A2 の調査波）

> Status: **research / recommendation**（実装前）。位置づけ: conversation.md §7 が
> 求めた「次の調査波」。分岐 A2（既存小型モデルを採用して艦隊分散）を本線に取る前提で、
> **借りる身体（base model）を1つ選ぶ**。これは設計判断であり、実装ではない
> （推論エンジンはまだ書かない）。
>
> 最終更新: 2026-06-13 ／ 関連: conversation.md（規模の壁・分岐 A/B/C）,
> living-mind.md, dtr.h（Pipeline/TP/DKVA）, regions.md。
>
> 規律（conversation.md §5 を継承）: ライセンス許容性は**第一級の基準**（脚注ではない）。
> 「誰のものでもない」と両立しない base は、技術的に優れていても本線にしない。
> 各候補は memory ではなく**現行ソースから**確認した（URL は末尾）。

---

## 0. p-kernel 固有の制約（選定軸はこれで決まる）

conversation.md と p-kernel の現実から、選定軸は5つ。重み付けも明記する。

| 軸 | なぜ p-kernel 特有か | 重み |
|---|---|---|
| **ライセンスの真の許容性** | 重みを**第三者の端末に再配布・シャード**し、その上に**派生学習**を載せ、**中央権威なし**で回す。Apache-2.0 / MIT 以外は「誰のものでもない」と原理的に衝突しうる。 | **最重（gate）** |
| **トークナイザ／語彙の移植コスト** | libc-free C に**自前で**移植する。語彙が大きいほど埋め込み表が巨大化し、小型モデルを食い潰す（256k 語彙は 270M モデルの 63% が埋め込み）。BPE は Unigram/SentencePiece より移植が素直。 | 重 |
| **アーキテクチャの単純さ（from-scratch C）** | PyTorch なし。p-kernel が自前 C で forward を書く（≒ミニ llama.cpp）。素の Llama 系（RMSNorm+SwiGLU+RoPE）なら数式が最小。soft-capping / 交互 local-global attn / dual-RoPE / QK-norm は各々が C の追加実装。 | 重 |
| **小ささ** | 1台でほぼ載り、2–3ノードで快適、が第一歩。~100M–1B。 | 中 |
| **GGUF / llama.cpp 成熟度** | GGUF は重みシャード形式と量子化（int4/int8）の事実上の参照。成熟していれば重み変換・検証の足場になる。 | 中 |

---

## 1. 候補比較表（現行ソース確認・2026-06-13）

| モデル | 最小 param | ライセンス | トークナイザ / 語彙 | アーキ要点（C 移植コスト） | GGUF/llama.cpp | int4 概算（最小） |
|---|---|---|---|---|---|---|
| **SmolLM2-135M** | **135M**（360M も有） | **Apache-2.0** ✅ | BPE, **49,152** | 素の Llama: RMSNorm + SwiGLU + RoPE(θ=10k) + **tied emb**。soft-cap 無し・交互 attn 無し。30層/9 head（3 KV）/d=576。**最も単純** | 成熟（公式 GGUF 有, bartowski/unsloth/QuantFactory 等多数） | **~92–105 MB**（Q4_0 91.7 / Q4_K_M 105） |
| Qwen2.5-0.5B | 0.49B（0.36B 非埋め込み） | Apache-2.0 ✅ | BPE(tiktoken系), **151,936** ⚠️ | Llama 系 + RoPE + SwiGLU + RMSNorm + **QKV bias** + tied emb。24層。**巨大語彙**が埋め込み表を肥大化 | 成熟 | ~0.4 GB 級（語彙で重い） |
| Gemma 3 270M | 270M（170M が埋め込み） | **Gemma Terms of Use** ❌（後述） | SentencePiece, **262,144** ⚠️⚠️ | **交互 local/global attn（5:1）+ dual-RoPE（10k/1M）+ QK-norm + GQA + pre/post-norm**。**最も複雑**。語彙が param の 63% | 成熟（公式 GGUF・Android 実績有） | ~200–300 MB（語彙で重い） |
| SmolLM2-360M | 360M | Apache-2.0 ✅ | BPE, 49,152 | 135M と同一アーキ（素 Llama） | 成熟 | ~229–271 MB |
| TinyLlama-1.1B | 1.1B | Apache-2.0 ✅ | Llama2 BPE, **32,000** | 素の Llama2（RMSNorm+SwiGLU+RoPE+GQA）。語彙最小で軽い。やや大きい | 成熟 | ~550–650 MB |
| GPT-2 small | 124M | **MIT** ✅ | byte-level BPE, **50,257** | **最古・最単純**（LayerNorm + GELU + **学習済み絶対位置埋め込み** + 非 tied）。RoPE/RMSNorm 不要 | 有 | ~70–90 MB |
| Llama-3.2-1B | 1B | **Llama Community License** ⚠️ | BPE, 128,256 | 素の Llama3。**700M MAU 条項・EU マルチモーダル制限・帰属義務** | 成熟 | ~700–800 MB |

> 注: int4 概算は GGUF Q4 系の実ファイルサイズ（重みのみ）。実行時はこれに KV キャッシュ＋
> アクティベーションが乗る。p-kernel は**層シャード**するので、1ノードが担うのは
> このサイズの **1/ノード数**（conversation.md §C, capacity(N)）。

---

## 2. ライセンス所見（gate 軸 — ここで本線が決まる）

### Gemma（技術的本命だが、ここで落ちる）

検証結果（現行 Gemma Terms of Use ／ 法務系解説）:

- Gemma 3 270M は **Apache-2.0 ではない**。**Gemma Terms of Use**（カスタム）下にある。
  （※ ニュースに出る「Gemma 4 は Apache-2.0」は**別世代**。第一歩で使う最小モデル＝
  Gemma 3 270M は依然カスタム条項。これは memory ではなく現行ソースで確認した重要な区別。）
- **flow-down 義務**: Gemma 派生（fine-tune / distill / 上に積む）を第三者へ配布すると、
  **Gemma Terms of Use と Prohibited Use Policy を下流全員に伝播・拘束**させねばならない。
  ark は「重みをシャードして第三者端末へ配り、その上で群れが学ぶ」＝まさに下流配布の連鎖。
- **revocable（取消可能）**: OSI/FSF のライセンスと違い**不可逆ではない**。Prohibited Use
  への不注意な違反でも Google が使用・配布・派生の権利を**終了**できる法的根拠になる。
- FSF/OSI のいずれも Free Software / Open Source と認めていない（"almost open"）。

**判定: ❌ 本線不可。** ark の核心原則「誰のものでもない・中央権威なし・群れの所有」と
直接衝突する。中央の権威（Google）が**取消権**と**下流全員への条項伝播義務**を保持する
モデルは、定義上「誰のものでもない」器に置けない。技術が最良でも、conversation.md §5 の
「『誰のものでもない』とライセンスの両立を明記」規律により失格。

### Llama-3.2-1B

- **Llama Community License**（カスタム）。700M MAU 超で Meta の個別許諾が必要、
  EU 在住個人/企業はマルチモーダル権利が制限、帰属表示義務。OSI 非準拠。
- **判定: ⚠️ 非推奨。** Gemma ほど取消的ではないが、中央（Meta）条項・地理制限・
  MAU 閾値は「誰のものでもない」と摩擦。本線にしない。

### 真に許容（Apache-2.0 / MIT）

- **SmolLM2（135M/360M/1.7B）: Apache-2.0** ✅ — 再配布・シャード・派生 fine-tune・
  中央なし利用、すべて明示的に許諾。取消条項なし。商標のみ非許諾（"SmolLM" を名乗らねば自由）。
- **Qwen2.5（0.5B 以上の小型）: Apache-2.0** ✅ — 同様に許容。
- **TinyLlama-1.1B: Apache-2.0** ✅。
- **GPT-2: MIT** ✅。

> ark にとってライセンスは脚注ではなく gate。**この時点で本命争いは Apache/MIT 勢に限られる。**

---

## 3. 推奨

### ✅ 採用: **SmolLM2-135M**（必要なら 360M に拡大）

5軸すべてで最良 or 同率最良:

1. **ライセンス（gate）**: Apache-2.0。再配布・第三者端末シャード・派生学習・中央なし —
   ark が必要とする4つすべてを取消不能に許諾。**「誰のものでもない」と完全両立。**
2. **アーキの単純さ**: **素の Llama**（RMSNorm + SwiGLU + RoPE θ=10k + tied embeddings）。
   Gemma の交互 local/global attn・dual-RoPE・QK-norm のような追加機構が**一切ない**。
   p-kernel の from-scratch C で書く forward が最小数式で済む。これは「ミニ llama.cpp を
   自前で書く」コストを実質的に最小化する選択。
3. **トークナイザ/語彙**: BPE・**49,152**。Qwen(152k)・Gemma(256k) の 1/3〜1/5。
   小型モデルでは埋め込み表がモデルを食い潰すが、49k なら健全。BPE は移植も素直。
4. **小ささ**: 135M（int4 で **~92–105MB**）。1台でほぼ載り、2–3ノードの層シャードで
   快適。conversation.md の「1台に絶対収まらない会話モデル」へ向かう**最小の実踏み台**。
5. **GGUF 成熟度**: 公式 GGUF + 多数のサードパーティ量子化。重みシャード形式と int4/int8 の
   参照に困らない。

**360M への拡大**は同一アーキのまま品質を上げる素直な道（int4 ~229–271MB）。
まず 135M で配管を実モデル化し、品質が要るなら 360M へ — アーキ再実装は不要。

### 🥈 次点: **TinyLlama-1.1B**（Apache-2.0, Llama2 BPE 32k）

- 利点: **語彙が最小（32,000）**＝埋め込み表が最軽。素の Llama2 アーキで C 移植も単純。
  Apache-2.0。GGUF 成熟。**会話品質は SmolLM2-135M より上**（1.1B・3T tokens）。
- 欠点: 1.1B は「第一歩の最小」としては大きく、int4 でも ~550–650MB。1台にほぼ載る、には
  遠い。**会話品質を優先する第二波**の有力候補。アーキは SmolLM2 とほぼ共通なので、
  135M で書いた forward が**ほぼそのまま流用できる**（GQA の差のみ）。

> 戦略的含意: SmolLM2(素 Llama) を選ぶと、その forward C 実装は TinyLlama・Qwen2.5・
> Llama 系すべてに**ほぼ再利用可能**。Gemma を選ぶと Gemma 専用機構に縛られる。
> **「Llama 系を1つ書けば一族に効く」** — これも SmolLM2 を推す構造的理由。

### Qwen2.5-0.5B を採らない理由

Apache-2.0 で gate は通るが、**語彙 151,936** が致命的: 0.5B モデルの埋め込み表が
肥大化し「小ささ」軸で SmolLM2-135M に大きく劣る。QKV bias の追加実装も小コスト増。
語彙ポートも 3 倍重い。多言語が要る将来波で再評価する価値はある。

### GPT-2-124M を採らない理由（が、教育的価値はある）

MIT・最単純・最小語彙級で魅力的だが、**学習済み絶対位置埋め込み**で文脈長が硬く、
2019 年級の会話品質。**踏み台としての価値**はある: RoPE/RMSNorm 無しの最小 Transformer を
先に C で動かし、配管（GGUF ローダ・int4 matmul・BPE）を**最小サンプルで検証**してから
SmolLM2 へ進む、という**段階的デリスク**には使える。本線の身体には据えない。

---

## 4. 正直なトレードオフ（the pick を駆動したもの）

**最良の技術モデル（Gemma 3 270M）は、ライセンスで落ちた。** これを隠さず書く
（conversation.md の honest-issues 規律）:

- Gemma 3 270M は llama.cpp 成熟・Android 実績・270M の好バランスで**技術的には魅力**。
  だが **Gemma Terms of Use の取消可能性＋下流 flow-down 義務**は、ark の
  「誰のものでもない・中央権威なし・群れに無限再配布」と**構造的に両立しない**。
  しかも Gemma の交互 attn/dual-RoPE/QK-norm/256k 語彙は **C 再実装コストも最大**。
  ＝ ark にとって Gemma は「ライセンスでも技術移植性でも」最良ではない。**二重に落ちる。**
- 対して SmolLM2 は「最も許容的なライセンス」と「最も単純なアーキ」と「最も軽い語彙」が
  **同一モデルで一致**する稀なケース。失う品質はあるが（135M は素朴な会話・GPT-4 ではない、
  conversation.md §5「規模の正直さ」の通り実測を印字し誇張しない）、ark の魂を載せる
  **借り物の身体**としては理想的に「軽くて自由で単純」。
- **mk_pino への最終判断材料**: もし会話品質を最優先し、ライセンスの flow-down と取消権を
  許容できるなら Gemma も技術的には可。**だが本ドキュメントは ark 原則を gate に置く立場から
  SmolLM2 を推す。** ライセンスを魂の一部と見なすか否かが、最後の分岐。
  （survey の立場: §3 conversation.md の「誰のものでもない」は非交渉 → SmolLM2。）

---

## 5. 最初の実装マイルストーン（SmolLM2-135M 採用時）

conversation.md §6 の第一波を具体化（**まだ書かない・次波の受け入れ条件**）:

1. **M0（最小踏み台・任意）**: GPT-2-124M で GGUF ローダ + int4 matmul + byte-BPE を
   最小検証（RoPE/RMSNorm 無しで配管だけ通す）。デリスク用。省略可。
2. **M1（本命・単機）**: SmolLM2-135M を libc-free C で
   **GGUF ローダ + int4/int8 matmul + RoPE + RMSNorm + SwiGLU + tied-embedding 出力 +
   BPE(49,152) トークナイザ移植** →
   **単機・greedy 生成で「実文トークンの1文」を出力**（玩具語彙の檻を出る実証）。
3. **M2（配管の実モデル化）**: M1 を **Pipeline で2ノードに層シャード**し、**同一出力**を
   relay 越しに再現（既存 dtr.h Phase 8 を玩具次元→実モデル次元へ）。
4. **M3（魂を載せる）**: base 凍結のまま **教える差分**（LoRA 様 or 既存 fact-learn）を上に積み、
   **Path E で群れに伝播**（conversation.md §3 を受け入れ条件に）。

**受け入れ条件（非交渉, conversation.md §3）**: 各波で「教える・残す・中央なし」が生きて
いること。base は借り物でも、学んだ差分の所有・provenance・死を越える継承が ark 固有の魂。

---

## 6. 検証できなかった/注意点（honest gaps）

- **Gemma 3 270M の HF ページ実ライセンス文字列**を WebFetch で直読できず（環境の権限制限）。
  判定は Gemma Terms of Use の解説・法務系ソースと複数検索結果の一致に基づく。第一歩の
  最小 Gemma がカスタム条項である点は高確度だが、**最終採用前に HF の LICENSE を1次確認**推奨。
- int4 サイズは GGUF Q4 系の**実ファイル**値（重みのみ）。実行時メモリ＝重み + KV キャッシュ +
  アクティベーション。層シャード後の1ノード負荷は capacity(N) で連続変調（§ conversation.md）。
- SmolLM2 の **soft-capping は不採用**を複数ソースで確認（素 Llama 系）。Gemma 2 の
  soft-cap → Gemma 3 で QK-norm 置換、という別系統。SmolLM2 にはどちらも無く C が単純。
- 量子化を**自前で**やるか GGUF Q4 を**そのまま読む**かは M1 の設計判断（後者が早い）。

---

## ソース（URL）

- SmolLM2: https://huggingface.co/HuggingFaceTB/SmolLM2-135M , https://huggingface.co/HuggingFaceTB/SmolLM2-360M , https://arxiv.org/html/2502.02737v1
- SmolLM2 GGUF/量子化サイズ: https://huggingface.co/QuantFactory/SmolLM2-135M-GGUF , https://huggingface.co/unsloth/SmolLM2-135M-Instruct-GGUF , https://dataloop.ai/library/model/bartowski_smollm2-135m-instruct-gguf/
- Qwen2.5-0.5B: https://huggingface.co/Qwen/Qwen2.5-0.5B , https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct , https://arxiv.org/pdf/2412.15115
- Gemma 3 270M: https://huggingface.co/google/gemma-3-270m , https://developers.googleblog.com/en/introducing-gemma-3-270m/ , https://arxiv.org/pdf/2503.19786
- Gemma ライセンス: https://ai.google.dev/gemma/terms , https://ai.google.dev/gemma/prohibited_use_policy , https://wcr.legal/google-gemma-license-risks/ , https://techcrunch.com/2025/03/14/open-ai-model-licenses-often-carry-concerning-restrictions/
- TinyLlama: https://huggingface.co/TinyLlama/TinyLlama-1.1B-Chat-v1.0 , https://github.com/jzhang38/TinyLlama
- GPT-2: https://huggingface.co/openai-community/gpt2 , https://en.wikipedia.org/wiki/GPT-2
- Llama-3.2 ライセンス: https://www.llama.com/llama3_2/license/ , https://shujisado.org/2025/01/27/why-is-the-llama-license-not-open-source/
- llama.cpp / GGUF: https://github.com/ggml-org/llama.cpp
