# inference-engine — p-kernel 版・自前 LLM 推論エンジン（M1）

> Status: **design**（実装前）。位置づけ: conversation.md の path A2 の最初の実装章。
> base = **SmolLM2-135M（Apache-2.0, 素の Llama）**（base-model-survey.md）。
> 目標は「玩具語彙を脱して、1ノードで本物の文を1文生成する」。PyTorch/Python 非依存、
> libc-free C、p-kernel の中で動く（＝最小の llama.cpp 相当を自作）。
>
> 最終更新: 2026-06-13 ／ 関連: conversation.md, base-model-survey.md, dtr.h（既存数理）,
> regions.md（分散）, living-mind.md（魂を載せる層）。

---

## 0. なぜ自作できるのか（既にある土台）

`arch/common/dtr.c` に Transformer の数理が**既に libc-free C である**:
- `dtr_expf`/`dtr_logf`（自前の指数・対数、wave-49 で FMA 問題も解決済み・全環境同一）
- `dt_softmax`（`dtr.c:160`）, `dt_layernorm`（`:171`）, attention（`:423` softmax(QKᵀ/√d)·V）
- 重みの永続 I/O は `pfs_dur_write/read`（temp+rename+fsync, wave-50 で正直化）

エンジンは「これを SmolLM2 の形に拡張」する。ゼロからではない。

---

## 1. 素の Llama forward — dtr.c との差分（実装すべきもの）

SmolLM2 = 素の Llama。現 dtr.c との差分だけが新規実装:

| 部品 | dtr.c 今 | SmolLM2 で要る | 実装コスト |
|---|---|---|---|
| 正規化 | LayerNorm(γ,β) | **RMSNorm**（β無し、x/√(mean(x²)+ε)·γ） | 小（dt_layernorm の変種） |
| 位置 | 学習 pos 埋め込み | **RoPE**（θ=10000、Q/K を回転） | 中（新規だが定式は単純） |
| FFN | GELU 様 1層 | **SwiGLU**（gate·up を SiLU、down で戻す、3行列） | 小〜中 |
| Attention | MHA | **GQA**（KV ヘッド < Q ヘッド、KV 共有） | 小（ヘッド対応付けだけ） |
| 出力 | 分類ヘッド | **tied embeddings**（入力埋め込みの転置で logits） | 小 |
| 活性 | float32 | **int4/int8 量子化 → dequant** | **大（§3 の本丸）** |

数理の癖が少ない（base-model-survey が SmolLM2 を選んだ理由）— Gemma の
soft-capping/交互attn/二重RoPE は無い。**この forward を1回書けば SmolLM2 →
TinyLlama → Qwen → Llama に使い回せる**（model config を読み替えるだけ）。

---

## 2. GGUF ローダ（重みをどう読むか）

GGUF = llama.cpp の重み形式（PyTorch 非依存配布の標準）。
- **何を読むか**: ヘッダ（マジック・バージョン）→ メタデータ（n_layer, n_head, n_kv_head,
  d_model, d_ff, vocab, rope θ, 量子化型）→ テンソル table（名前・形・型・オフセット）→
  量子化ブロック本体。
- **どう読むか**: ホスト/Android は `mmap`（巨大ファイルを zero-copy で）。`pfs_dur_read`
  のパターンを拡張（ただし 100MB を一括 read はしない、mmap でページイン）。ベアメタルは
  対象外（GPU/大容量無し、conversation.md の身体は host/Android 側）。
- **config 駆動**: ローダが読んだ次元で forward を回す（ハードコードしない＝モデル可換）。

---

## 3. 量子化行列積（本丸の重い仕事）

135M を float32 で持つと 540MB、int4 なら ~100MB。**量子化が「スマホに載る」の鍵**。
- GGUF の Q4_0/Q4_K 等のブロック量子化（32 値ごとに scale/min を持つ）を **dequant しながら
  matmul**（重みは int4 のまま保持、計算時に float へ展開）。
- これが計算の 99%。**まず正しさ（float 参照と一致）→ 次に速度**（SIMD/NEON は後の波、
  最初は素直な C ループで「動く」を取る）。
- dt_softmax 等の既存活性はそのまま再利用（活性は float）。

---

## 4. トークナイザ（BPE 49,152 の移植）

会話には本物のサブワード分割が要る。
- SmolLM2 = BPE（49,152 語彙、GPT-2 系）。GGUF にトークナイザ table（merges/vocab）が同梱。
- **移植**: vocab + merge ルールを読み、テキスト → token id 列（encode）、id → テキスト
  （decode）。libc-free C で UTF-8 を扱う（既存の i18n でバイト列処理の経験あり）。
- 現 `r3_vocab.c`（16/64 語の単一トークン）は**置換**ではなく**並存**（玩具 R3 心は残し、
  会話エンジンは別経路）。wire の語彙 content-id 整合（wave-47）の発想は引き継ぐ。

---

## 5. マイルストーン（各々 falsifiable cert）

- **M1（この章の最初）**: SmolLM2-135M を **1ノードで greedy 生成**。
  GGUF ローダ + int4 dequant matmul + RMSNorm/RoPE/SwiGLU/GQA/tied-emb + BPE。
  cert `[llm-sentence]`: 固定プロンプト → **既知の参照実装（llama.cpp）と同じ greedy 出力**
  （同一 seed/temp=0 でトークン列一致 or 十分近い）。「玩具語彙を脱した瞬間」。
- **M2**: 同じ forward を **2ノードに Pipeline 分割**（既存 dtr Pipeline）、relay 越しに
  **M1 と同一出力**。cert `[llm-pipeline]`: 1ノード出力 == 2ノード分割出力。
  ← 「1台に収まらないモデルを群れで動かす」が初めて本物に。
  **ここで dense層シャード vs MoEエキスパート分散を決める**（conversation.md §3.5）。
- **M2-survive**: 生成中にシャード保持ノードを **kill -9 → 文が完成する**。
  base を r重複製（凍結ゆえ安い）+ SWIM 死検知 + 複製先へ再ルート +
  落ちたトークンを最後の完了ステージから再実行。cert `[llm-survive]`:
  「kill 後も同一プロンプトの生成が完走」。← 生命体の核心（mk_pino の問い）。
- **M3**: **凍結 base + 学んだ差分**。教えた事実を軽量学習（base 凍結、差分のみ）で載せ、
  Path E で群れに伝播。cert `[llm-teach]`: 教えた内容を base が答える、教師の死を越える。
  ← ark の魂（育てる・残す）が会話モデルに載る。

M2 以降で身体（GPU・S_n 資源適応, conversation.md §4）に接続。

---

## 6. メモリ予算（スマホで現実的か）

- 重み int4: ~100MB（mmap、ページイン）。KV キャッシュ: 文脈長×層×d で数十MB。活性: 小。
- → **1台でギリギリ動く**（M1）。**2-3ノードに層分割すれば1台あたり 30-50MB**（M2、快適）。
  capacity(N) が「この艦隊規模で載るモデルの最大」を連続で決める（conversation.md §C）。

---

## 7. 正直な論点（honest issues）

- **これはプロジェクト最大の単一実装**。M1 だけで GGUF/量子化/トークナイザ/forward の4つ、
  各々が複数波になりうる。正直に分割して進める。
- **速度**: 最初は SIMD/GPU 無しの素直な C → CPU で**遅い**（1トークン数百ms〜秒級かも）。
  「動く」を先に取り、速度（NEON/GPU）は身体の章で。各波で実測 ms を印字、誇張しない。
- **正しさの基準**: llama.cpp を**参照オラクル**にする（cert は「参照と一致」）。自前実装の
  バグを参照で炙る（validator-trap の規律）。
- **魂の希釈**: M1/M2 は「ただの分散 llama.cpp」に見える。M3（教える差分 + Path E + 生きた証）
  まで来て初めて ark。M1 着手時点でその到達点を明記し、毎波 §conversation.md-3 を受け入れ条件に。
- **model 可換の堅牢性**: 素の Llama forward なので SmolLM2 で動けば TinyLlama/Qwen も
  config 替えで動く。mk_pino の最終モデル選択（品質 vs 軽さ）が後で振れても M1 は無駄にならない。

---

## 8. 最初の波の提案（M1 の第一歩）

M1 をさらに割る。最小の「動く」から:
1. **M1a — GGUF ローダ + config 表示**: SmolLM2 の GGUF を読み、メタデータ（層数・次元・
   量子化型・語彙）を正しく印字。重みテンソルの table を列挙。cert: 既知の値と一致。
2. **M1b — 量子化 matmul（単体）**: 1つの量子化重み行列 × float ベクトル = float、を
   llama.cpp の同じ行列で**数値一致**。
3. **M1c — full forward 1層 → 全層 → greedy 1文**: 参照と greedy 一致。

M1a が最初の波。スマホ不要・既存数理の上・参照オラクルあり＝堅実に始められる。
