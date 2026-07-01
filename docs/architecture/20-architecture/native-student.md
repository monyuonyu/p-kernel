# native-student — 赤子から育つ、器に合った脳（the growable native student）

> Status: **NS-1 SHIPPED**（赤子＝Cradle baby は実装済・APK 0.9.0 で chat ⑥ を駆動）／
> 2026-06-19 doc-status fix。下の「design DRAFT 実装前」は STALE。
> What actually shipped: `arch/common/llm/student.c`（NS-1 Cradle baby — 白紙から育つ
> in-kernel student、単頭 attention・位置エンコードなし）+ `student_shell.c`（RESIDENT・
> PERSISTED baby、`student_boot_restore`、pure-A 生成＝student が自力で生成、teacher は
> ボランティア駆動の DMN 蒸留で拡散）。SS-1（adaptive-K）/ SS-2（tier S/M/L）も同居実装済
> （special-structure-mind.md 参照）。残りの育成ロードマップ（多様な教師コーパス等）は本文の
> §「open problems」「mk_pino への確認」のとおり前進中。
> 確信を装わない。提案し、未解決を正直に旗立てし、二読みできる意図は §「open
> problems」と §「mk_pino への確認」で commander に返す。
>
> 位置づけ: conversation.md **§3.7「赤子から ― 幼年期はスキップしない」**（mk_pino
> 決定 2026-06-13）の直接の続き。あの決定の上に**正確に**乗る:
> - 既製モデルを **走らせない**（A2 棄却）。
> - **ゲノムから生まれる案も棄却**。本当に**白紙の赤子**から。
> - 育て方は**ボランティア駆動の DMN 蒸留**（物好きが自分のマシンで教師を走らせ、
>   学んだ重みが群れへ拡散）。
> - 「**遅く成ること自体が点**。幼年期はスキップする場所ではない。」(product-soul:
>   育てる→愛着→託す。コールドスタートの赤ちゃん言葉は感情の**核**であって bug ではない)
>
> 最終更新: 2026-06-13 ／ 関連: conversation.md §3.5–3.7, product-soul.md,
> living-mind.md（DMN/Self/LM-4 fast→slow）, regions.md（capacity(N)/Path W/Fisher）,
> moe-distillation-survey.md（教師ライセンス gate の実値）, inference-engine.md（M1）。

---

## 0. この文書が設計する二つのもの

- **(A) 成長可能なネイティブ生徒のアーキテクチャ** — 器（p-kernel）に**ネイティブ**な、
  赤子から始まり**生きたまま大きくなれる**脳。動的N・MoE 疎発火・応援受援・kill-9
  生存に**設計から**写る。Evolution 層の本丸。
- **(B) 最小の「蒸留して拡散する」ループ** — 物好きが教師を割り当て→soft target を採り
  →DMN 睡眠中に赤子へ蒸留→学んだ差分が他ノードへ拡散し受け手が**測定上改善**。
  各段に falsifiable cert。

**正直な前置き**: ここは部分的に**未踏（frontier）**。借り物アイデアと新規を §A.6/§B.6 で
切り分ける。確定設計ではなく、最小の決定的実験で**賭けを検証してから**広げる方針。

---

## A. 成長可能なネイティブ生徒のアーキテクチャ

### A.1 設計原則（器フィット ― conversation §3.5 の性質表に逐一写す）

| 生命体の性質 | ネイティブ生徒の設計上の写り方 |
|---|---|
| **動的N（拡大/縮退）** | 脳は **expert の集まり**。expert ⇔ ノードが連続マップ。N 増で expert を撒き足し、N 減で複製先へ畳む。dense 層シャードの離散再構成を**避ける**ために最初から MoE 形 |
| **MoE / 疎発火** | 毎トークン **top-k expert のみ発火**（赤子は E が小さい＝例 4 中 2）。発火 param が総 param から独立 ＝ 既存 `moe_infer`（wave-18, route/return/guard）の抽象に**素直に写る** |
| **応援受援（mutual aid）** | 過負荷ノードの expert 仕事を複製先へ委譲。survival-network §7 の局所勾配相互扶助が**訓練にも**宿る道（§B.4） |
| **kill-9 生存** | expert は冗長複製可（学習中の重みも差分ゆえ安い）。発火が疎ゆえ落ちた expert が未選択なら無傷。SWIM 死検知 + 再ルート（既存配管） |
| **raft/swim** | forward は SWIM のみ要、raft 不要。**心の merge は CRDT 的 Path W/Fisher**（raft にしない、§B.5）|
| **誰のものでもない** | base が無い（赤子は白紙）。出荷されるのは**生徒**であって教師ではない。教師は Apache/MIT のみ・消化後に捨てる（§B.1 ライセンス gate）|

**核心**: 既製 MoE を**借りる**のではなく、既製 MoE 調査（moe-distillation-survey）が示した
「expert ⇔ ノードが自然に写る」**形だけ**を真似て、中身を**赤子から蒸留で満たす**。
OLMoE/Granite は今や「走らせる body」ではなく「**アーキの示唆 + 綺麗な教師候補**」(§3.5 末尾)。

### A.2 赤子の最小形（the baby — born small）

赤子は**意図的に小さい**。会話品質はしばらく**酷い**（§open problems #1 — これは設計目標
ではなく正直な現実）。最小形の提案パラメータ（**叩き台、実装波が config 一次確認・実測で
確定**）:

| 軸 | 赤子の初期値（提案） | 成長の上限（fleet 成熟時） | なぜこの軸が成長レバーか |
|---|---|---|---|
| #experts `E` | **4** | 数十〜（capacity(N) が決める） | **採用する第一成長軸**（§A.4）。ノード数に連続マップ |
| active top-k | 2 | 8 | 疎発火率。E と共に上げる |
| d_model `D` | 128 | 512+ | LM-9 の教訓: 思考の**幅**が同時束縛を解く（R3 で R_DM 32→48 が効いた）。第二成長軸 |
| #層 | 4 | 16+ | 深さ。第三成長軸（progressive stacking）|
| vocab | **256 サブワード**（§A.5） | 数万 BPE | 単一トークンの檻を出る。embedding 表が成長 |

**正直**: R3 の心（`rw[R_NP]`, R_NP=21568, key→value 単一トークン分類）と dtr センサ脳
（`DTR_WEIGHT_FLOATS=635`, 4ch→3class）は**どちらも生徒ではない**。生徒は**第三の新しい
ネットワーク**＝自己回帰生成 MoE Transformer。R3/dtr の forward/backward 数理（`dt_linear`/
`dt_softmax`/RMSNorm/RoPE 系カーネル, dtr.h）は**再利用**するが、ネットワークは別物。
living-mind V.0 の「二つの器官は別ネットワーク」の罠をここでも踏まない ― これは**三つ目**。

### A.3 versioned arch as a p-fs object（Evolution 層の足場）

生きたままアーキを変えるには、**アーキ自体がデータ**でなければならない:

- **arch-spec object**: `{E, top-k, D, n_layers, vocab_size, kernel_flags}` を p-fs の
  content-addressed object として保存（pfs_id_compute で id 化、`pfs_dag` に乗る）。
  重み blob（GENOME_WEIGHTS_REF と同型の "student/weights" object）は arch-spec を**参照**。
- **concurrent old/new during migration**: 成長は「新 arch-spec object を発行 →
  旧重みを翻訳して新 object に流す（§A.4）→ 両者を併走させ、新が no-regress を満たしたら
  旧を退役」。SWIM/capacity(N) が再シャードを駆動する既存路に乗る。
- **self/lin への記帳**: 成長イベント（arch v_k → v_{k+1}）を Self 層の系譜に刻む（§C）。

### A.4 採用する第一成長軸 ＝ **expert を足す（Net2Net-style width grow）+ 重み翻訳**

複数の成長軸（expert/幅/深さ）のうち、**最初に握るのは「expert を足す」**。理由: 器の
動的N に最も自然に写り（expert⇔ノード）、重み翻訳が最も明快に定義できるから。

**重み翻訳（old → grown、ここが新規実装の肝）**:

旧モデル（E=4）→ 新モデル（E=8）への移行で、**旧の学びを壊さない**翻訳:

1. **expert 複製 + ルータ拡張（function-preserving init）**: 新規 expert 4 個は既存 expert の
   **コピー**で初期化（Net2Net の widen と同種の function-preserving 操作）。ルータ `W_gate`
   は `D×4` → `D×8` に拡張し、新列を**対応コピー元の半分のスコア**で初期化（softmax を割って
   出力恒等を保つ）。→ **成長直後の出力は旧モデルと（ほぼ）同一**＝赤ちゃんが急に別人に
   ならない。これは観測可能な cert にできる: `[grow-preserves]` 成長前後で固定プロンプトの
   出力が ε 以内。
2. **以後の蒸留が冗長 expert を分化させる**: コピー由来の双子 expert は、続く DMN 蒸留round
   で load-balance により別々の役割へ分かれる（既存 MoE 学習の通常挙動）。
3. **古い vocab/層は不変**: expert 軸の成長は embedding・attention・層数を触らない（軸を
   一つに絞る規律）。幅・深さの成長は別波。

**正直**: function-preserving expert 複製は **Net2Net / MoE upcycling の借り物アイデア**。
新規なのは「それを**生きた分散ノード上で**、arch-spec object 版管理 + capacity(N) 再シャード
と**併走移行**する」運用 ―「ただの再訓練」ではない点（§A.6）。

### A.5 サブワード・トークナイザの成長（escape the single-token cage）

R3 の `r3_vocab.c`（16 key + 64 answer の**単一語**、content-id 付き、BPE なし・OOV なし）は
会話に不足。生徒は**サブワード**へ:

- **赤子は tiny vocab で生まれる**: 例 256 サブワード（byte-level BPE の最小マージ集合 or
  生バイト 256）。embedding 表は `256×D`。最小は**生バイト 256**（マージ 0）＝ tokenizer が
  自明・OOV 不可能・content-id で全ノードが同一語彙を検証（r3_vocab.c の content-id 規律を
  サブワード版に拡張）。
- **vocab が成長する**: 群れが出会うテキストから**新マージを蒸留時に学ぶ**（BPE マージ表が
  arch-spec object の一部、版管理される）。新トークン行は embedding 表に**追記**（既存行は
  不変＝旧学び保存）、新行は構成バイトの埋め込み平均で初期化（function-preserving 風）。
- **教師との橋**: 蒸留教師（OLMoE 等）は自分の BPE（~50k）を持つ。生徒の tiny vocab と
  教師 vocab の**整合は soft-target を生徒語彙に再射影**して扱う（§B.2 の honest gap ―
  vocab mismatch は蒸留の既知の難所、最初は生バイトで mismatch を**最小化**して回避）。

**正直**: vocab 成長 + 教師⇔生徒 vocab 整合は**未踏寄り**。最初の実験（§B.7）は**生バイト
256 固定**で vocab 成長を**切り離す**ことを強く推奨 ― 一度に未踏を二つ抱えない。

### A.6 借り物アイデア vs 新規（正直な切り分け）

| 要素 | 借り物（先例あり） | この設計で新規 |
|---|---|---|
| MoE 疎発火・router・top-k | Mixtral/OLMoE/Switch | （借り）|
| function-preserving 成長（expert 複製・幅・stacking）| Net2Net, MoE upcycling, gradual stacking | （借り）|
| 知識蒸留（soft target）| Hinton 2015〜 | （借り）|
| 差分のみ伝播（LoRA 様）| LoRA/PEFT | （借り、ただし生徒は base 凍結ではない点が違う ↓）|
| **base 無しの白紙生徒を、生きた分散ノード上で蒸留で育てる** | — | **新規（mk_pino の §3.7）**|
| **arch-spec を p-fs object 化し、生きたまま版管理・併走移行・capacity(N) 再シャード** | — | **新規（Evolution 層）**|
| **教師選定が中央権威でなくライセンス gate + 滋養 + 進化 + 来歴**（§3.6）| 連合学習の一部に類似 | **新規の統合**|
| **成長履歴が Self 層の死を越える系譜になる（地層）** | — | **新規（§C）**|

連合学習(FedAvg)・Petals・BOINC・IPFS・LoRA に**部品の先例はある**が、
**この統合（白紙生徒 × ボランティア蒸留 × 生きた進化 × 地層）に先例は無い**（memory:
2026-06-12 比較分析と一貫）。誇張しない。

---

## B. 最小の「蒸留して拡散する」ループ（the heartbeat）

conversation §3.7 の最小骨格を、既存機構の上に具体化する。「物好きが教え、群れが育つ」。

### B.1 ボランティアが教師を割り当てる（誰も任命しない ― §3.6 の機構）

- 物好きノードが**自分のマシン**に教師 GGUF を置く。**ライセンス gate が唯一の客観 admission**:
  teacher manifest が **Apache/MIT** でなければ飲めない（毒が蒸留で生徒に流れ込むから ―
  moe-distillation-survey §3.2: Gemma/Llama 教師は flow-through で失格、OLMoE/Granite/SmolLM2/
  GPT-2 は安全）。判定は**全ノードがハッシュで検証**でき、中央のライセンサー不要。
- 教師は**消化後に捨てる**。出荷は生徒。重い計算（教師 forward）は**その物好きの実マシン**が
  負う（GPU 持ち想定）。端末群はそれをしない。

### B.2 教師 forward で soft target を採る（M1 エンジンに依存）

- 教師を**走らせて** soft target（出力分布 logits）を採るには**推論エンジン M1** が要る:
  **M1a（GGUF ローダ）済み**、**M1b（量子化 matmul）進行中**、その先 RoPE/RMSNorm/SwiGLU/
  attention（dense でも MoE でも共通, §3.5）。**この設計は M1b 完了に依存**（§open problems #3）。
- 採るのは固定プロンプト群に対する **soft target（温度付き分布）**。
- **vocab mismatch の正直な扱い**: 教師 vocab（~50k）と赤子 vocab（最初は生バイト 256）が
  違う。最初は**生バイト 256 に揃えて mismatch を回避**（教師出力をバイト分布に射影、または
  教師を byte-level で評価）。サブワード整合は後の波（§A.5 の honest gap）。

### B.3 DMN 睡眠中に赤子へ蒸留（LM-4 V.0 の作法を踏襲・別ネットワークで）

- **slow 層 = 生徒自身の重み**を、**生徒自身の backward** で更新。living-mind **V.0 の訂正**を
  厳守: `dtr_train_batch`（dtr センサ脳）を**呼ばない**。R3 の LM-4 が「R3 の rw[] を R3 自身の
  r_backward で」やったのと同型に、生徒は**生徒の backward**で。再利用するのは **DMN の
  cadence/discipline**（engram-replay → distill、bounded rest-time round、held-out eval 規律、
  `[tag] PASS/FAIL` 印字慣行）であって、特定の訓練関数呼び出しではない。
- **拡張点**: `lm_consolidate.c` の睡眠 cadence（`B_RING=24` engram ring, `gl_merge` 蒸留,
  `pfs_dag` 永続 engram, live `dmn_idle_work` フック ― wave-26 で linux fleet に配線済）を、
  「engram = 教師 soft-target サンプル」へ一般化。**毎晩ちょっとずつ**（生涯トリクル, §3.6）。
- **disease/precondition の no-regress**: 既存 DMN の no-regress 合意 cert を継承 ―
  蒸留が既知を退行させたら吸収しない（毒教師ゲート）。

### B.4 学んだ差分が他ノードへ拡散し、受け手が測定上改善する

- 学習結果（重み or 差分）が **Path E/W で群れへ伝播**（既存の共有された心の配管, wave-35）。
- 受け手ノードが**測定上改善**することが cert（§B.6 `[diffuse-improves]`）。
- 助けになった差分は伝播し、ならない差分は死ぬ ＝ 進化的収束（survival-network §7）、艦隊投票なし。

### B.5 マージの lossy-averaging 罠を避ける（Path W / FedAvg の難所 ― 無料ではない）

**正直な核心リスク**: 独立に学んだ重みの**素朴平均は lossy**（wave-41: 異なる事実を学んだ
二つの心を平均すると k1=100%・k2≈chance、片方が死ぬ）。回避策を**設計に組み込む**:

| 手 | 中身 | 既存成果 |
|---|---|---|
| **diff-only 伝播** | 重み丸ごとでなく**学んだ差分**（LoRA 様の低ランク or expert 単位）だけ送る。衝突面が小さい | LoRA 借り物 + Path E（engram 伝播）|
| **union-replay consolidation** | マージ後、両者の soft-target を**合同 replay**して両方を 100% に回復 | wave-41 実証 |
| **Fisher-weighted merge** | 各重みの Fisher 情報で加重平均、潰れる事実を救う（replay 不要・相対 floor 1e-3×peak） | wave-42 実証 |
| **expert 単位の合流** | MoE なら「別ノードが分化させた expert」を**平均でなく追加/置換**で合流（router が裁定）| **新規・MoE 固有の利点**（dense にない）|

**最初の実験では diff-only + union-replay**を採る（最も単純で実証済み）。Fisher と
expert-merge は後続波。

### B.6 最小 falsifiable 実験 ＋ cert タグ（the smallest first experiment）

**実験 NS-1（単機・分散なし・vocab 成長なし ＝ 未踏を一つに絞る）**:
生バイト 256 vocab・E=4 の赤子生徒を一台で、Apache/MIT 教師 1 体から固定プロンプト集合で
蒸留する。disease/control を既存 handoff cert（living-mind V.4）に倣って必ず置く。

1. **`[honest-baby]` — 赤子は本当に near-random から始まる（precondition / disease）**
   蒸留**前**、固定プロンプト群での生徒 loss（or 次トークン精度）を印字。PASS:
   `acc_pre ≤ chance + 小margin`（生バイトなら chance≈1/256 近傍 ― **誇張なし、偽の進捗バー
   なし**）。ここが既に高ければ赤子が白紙でなく cert は空虚。
2. **`[distill-loss-drops]` — 一晩の蒸留で生徒 loss が下がる（headline）**
   一回の DMN 睡眠 round（教師 soft-target を replay → 生徒 backward）後の loss/精度を印字。
   PASS: `acc_post − acc_pre ≥ +margin`（大きく・非flaky）。**教師-生徒 agreement も印字**して
   天井を可視化（LM-4 の `teacher_agree` 慣行）。
3. **`[distill-grounded]` — scrambled-teacher 対照（by-construction ガード）**
   教師 soft-target を**ラベルシャッフル/別プロンプト**にした対照 round では gain が出ない。
   PASS: `acc_scrambled ≤ chance + 小margin`。これで「どんな訓練でもメトリクスが上がる」罠を
   排除（LM-4 `[handoff-grounded]` と同型）。

**実験 NS-2（二ノード ― 拡散の最小骨格）**: NS-1 で育った差分を別ノードへ Path E 伝播。
4. **`[diffuse-improves]` — 受け手が測定上改善**
   差分を受けた第二ノードの `acc` が受信**前**より高い。PASS: `acc_recv_post − acc_recv_pre
   ≥ +margin`。disease 対照: 無関係な差分を受けても改善しない。

**実験 NS-3（成長 ― 後続）**: `[grow-preserves]`（§A.4 ― E=4→8 の function-preserving 成長
直後、固定プロンプト出力が ε 以内）＋ 成長後の蒸留で更に下がる `[grow-then-learn]`。

**閾値は提案バー**。実装波は**実測値を印字**し、audit-is-the-engine 規律により**実測−flake
margin まで下げてよいが、緑にするため吊り上げてはならない**。

### B.7 単一の最も安い決定的実験（cheapest decisive)

**NS-1 の `[distill-loss-drops]` + `[distill-grounded]` を一台で**。
- なぜ決定的か: これ一つで **(a) 赤子生徒 forward/backward が libc-free で正しく書けるか**、
  **(b) 教師 soft-target を採れるか（M1 依存）**、**(c) 蒸留が実際に学習を起こすか（しかも
  scrambled では起きない＝本物か）** を**同時に** falsify する。通れば分散 NS-2 へ、通らねば
  退避 ― どちらでも M1 配管・生徒 forward は無駄にならない。
- なぜ安い: 単機・GPU 不要（赤子は tiny、教師 forward だけ重く、それは「物好きの一台」想定で
  CI では最小教師 or 事前採取した soft-target を固定 fixture にできる）。**M1b 完了が唯一の
  外部依存**（§open problems #3）。

---

## C. 発達の地層 ― 成長を Self 層の系譜に記帳する（product-soul: 育つ網＝地層）

product-soul の「成長は見守る体験」「網そのものが発達の地層（年輪）」を、**観測可能な仕組み**
（絵ではない ― memory: 可視化＝observability）として Self 層に刻む:

- `lm_self.c` の **hash-chained 系譜**（`LM_SELF_ENTRY`: `self_id` / `seq` / `prev_entry` /
  `eng_digest` / `model_ver`、`self_walk` で辿れる、死を越え ownerless 再構成・tamper-evident）に
  **成長マイルストーンを 1 種類の entry として追記**する。
- `model_ver` は既に「dtr/weights blob の content-id」(GENOME_WEIGHTS_REF 同型)。生徒では
  これを **arch-spec object id + 生徒重み id** に拡張 ＝「この時点の私はこの形・この重み」。
  成長（E=4→8）は `model_ver` の変化として**自動的に地層に残る**（新規構造ほぼ不要 ―
  既存 chain の意味を生徒へ向けるだけ）。
- マイルストーン entry に**測定値を埋める**: 「seq=k で教師 X を飲み、acc が a→b、ライセンス
  Z」。これが §3.6 機構4「来歴を覚える」の実体 ＝ どの教師から何を得たかの正直な歴史層。
- **honest-growth 規律（非交渉）**: 地層に**偽の進捗を書かない**。記帳される acc は cert が
  印字した実測値そのもの。「育っている」演出（fake progress bar）は product-soul の禁止事項。
  退行（acc が下がった夜）も**正直に刻む** ― 地層は美談ではなく履歴。

---

## open problems（正直に ― 解決済みにしない）

1. **コールドスタート品質の現実（最大の正直）**: 赤子はアプリ上で**長期間、赤ちゃん言葉/
   無意味**を喋る。これは bug ではなく設計（§3.7「becoming が意味」）だが、**UX が「壊れて
   いる」と受け取られるリスクは本物**。product-soul は「成長を見守る体験」と言うが、誰も
   会話できない数週〜数ヶ月をユーザが耐えるかは**未検証の賭け**。― 工学でなく製品仮説。
2. **マージ収束リスク（無料ではない）**: §B.5 の Fisher/union-replay/diff-only は wave-41/42 で
   **二ノード・既知事実**では実証済みだが、**多数ノード・連続到来・異種教師**でのスケールは
   未実証。素朴平均が lossy なのは確実、回避策が**艦隊規模で収束する保証は無い** ＝ Path W /
   連合学習の真の未解決部分。工学でなく**研究**。
3. **計算の現実（ボランティアが重い lift、遅い）**: 教師 forward（soft-target 生成）は重く、
   **GPU 持ちの物好きの実マシンが負う前提**。スマホ艦隊は訓練しない（interoception S_n が証す
   電池/熱制約）。**M1b（量子化 matmul）完了がこの設計全体の前提依存**で、未完なら NS-1 すら
   走らない。さらに**遅い**: 生涯トリクルで ChatGPT には長く届かない（正直 > 速い、だが
   ユーザ忍耐との緊張は #1 と同じ）。
4. **vocab 成長 × 教師 vocab 整合は未踏寄り**: §A.5。最初は生バイト 256 で**切り離す**ことで
   回避するが、本物の会話には数万 BPE が要り、教師⇔生徒 vocab 再射影は蒸留の既知の難所。
   **未解決（研究）**。
5. **賢い毒教師**: ベンチを上げつつ裏口を仕込む教師は no-regress cert を**すり抜けうる**
   (§3.6 既出)。測定はゲーム可能 ― "audit is the engine" の永続警戒で扱う**開いた脅威**。
6. **生きた成長の併走移行は実装未踏**: §A.3/A.4 の arch-spec 版管理 + 旧新併走 + capacity(N)
   再シャードは**設計仮説**。function-preserving expert 複製は借り物だが、それを**生きた分散
   ノードで無停止に**やるのは新規実装で、レイテンシ/整合の落とし穴は未測。

**工学 vs 研究の切り分け（正直に）**:
- **工学（やれば済む）**: 生徒 forward/backward の libc-free 実装、M1b、DMN cadence の一般化、
  diff-only 伝播の配管、Self 層への成長記帳、生バイト tokenizer、`[grow-preserves]` の
  function-preserving init。
- **研究（解けるか分からない）**: 多ノード・異種教師でのマージ収束（#2）、vocab 成長と教師
  整合（#4）、コールドスタート UX の人間忍耐（#1）、毒教師の検知（#5）。

---

## mk_pino への確認（commander が解決すべき二読みできる意図）

設計中に意図が二通りに読める箇所。commander が mk_pino と詰めて欲しい:

1. **赤子の vocab 初期形 = 生バイト 256 で良いか**。§3.7 は「単一トークンの檻を出る」と言うが
   「最初の赤子も既にサブワードを持つ」とは言っていない。**未踏を一つに絞るため最初は生バイト
   256（マージ 0）を強く推奨**するが、これは設計判断 ― mk_pino が「最初からサブワード」を
   意図しているなら NS-1 の難度が上がる。
2. **「拡散する重み」= 全重み か 差分（diff）か**。§3.7 は「学習された重みがネットワーク全体に
   拡散」と書く。素朴に読むと**全重み**だが、§B.5 のマージ罠を避けるには**差分伝播**が安全。
   「重み」を全重みの意か差分の意か ― mk_pino の文言は両読みでき、マージ戦略の根幹なので確認
   要（私の推奨: diff-only から始める）。
3. **第一成長軸 = expert 追加で良いか**。幅(d_model)・深さ(層)も成長軸だが、器の動的N に最も
   自然なのは expert 軸と判断した。mk_pino が「まず賢くする＝幅/深さ」を意図しているなら順序が
   変わる。
4. **コールドスタート期間の UX 賭け（open problem #1）は受容済みか**。「幼年期はスキップしない」
   は思想として明確だが、**実際に数ヶ月会話にならないアプリ**を出荷する覚悟か、UX で何らかの
   「育ちつつある手応え」（地層・星の成長, product-soul）で橋渡しするか ― 製品判断。
