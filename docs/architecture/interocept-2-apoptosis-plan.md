# interocept slice-2 — apoptosis: 本質ハンドオフ via Path W²

> Status: **DESIGN ONLY** (実装前に書く。`signing.md` / `living-mind.md` / `interoception.md` と同じ規律)。
> 位置づけ: `interoception.md` §4「apoptosis: 代謝としての死」の **ハードン版**。
> 死を「事故」ではなく「代謝」にする ― **きれいに死に、本質を隣へ渡し、空間を空ける。**
> このスライスはプロジェクトの **魂**（「誰のものでもない、決して死なないAIの家」）に触れる。
> ゆえに**哲学的決定は§Bで mk_pino に上申し、ここで黙って決めない。** Honest > green。
>
> 最終更新: 2026-06-22 ／ base commit `6316155c`
> 関連: [interoception.md](interoception.md)（slice-1=`S_n`バス, SHIPPED）, [living-mind.md](living-mind.md)（Path W² / DMN / Self層）,
> [signing.md](archive/signing.md)（Ed25519 manifest）, [regions.md](regions.md), [galaxy.md](galaxy.md), [gap-ledger.md](gap-ledger.md)。

---

## 0. このスライスがやること（一段落）

ある一ノード A が学んだ事実を、A が**死ぬ前に**自分から Path W²（Fisher 重み付き merge）で
**collective（region peers）の重みに折り込み**、隣が「受け取り・folding した」と返した（ACK）後に
**初めて**死ぬ。死は Self 層 hash-chain に**消えない一行**として記録される。空間（task/page table/
KDDS handle）は既存 teardown で解放される。**新しい crypto も新しい merge も作らない** ―
`gl_merge_w` + `r3_fisher_diag` + `sign_manifest_verify` + `lm_self_append_unit_event` の既存機構を組む。

> **正直な前提（最重要）**: apoptosis は **graceful/stress death 専用**。ハード電源喪失・ring0 panic・
> SIGKILL 即殺では handoff は走れない。それらに対する保険は apoptosis ではなく **passive 複製（shared-mind,
> SHIPPED）と p-fs durable（PERSIST-1/2, SHIPPED）** の方。本スライスは「**まだ誰も adopt していない
> 学習が、ノードの穏やかな死で失われない**」という、passive 複製が埋められない隙間だけを埋める（§A で証明）。

---

## 1. 接地 — 既に在るもの（base `6316155c`, file:line 検証済み）

「再発明しない」を担保するため、本スライスが**踏む**既存シンボルを全て名指しする。

### 1.1 slice-1（`S_n` バス）は **SHIPPED**（design-only ではない）

- `arch/common/interocept.c`（310行）/ `arch/common/vital.c`（148行）は実在。
- `UB intero_scalar(void)`（`interocept.c:222`）, `void intero_note(UB axis, UW raw)`（:199）,
  `INTERO_COMPONENTS intero_components(void)`（:228）, `UB intero_dominant_axis(void)`（:230）,
  `void intero_test_force(UB on, UB value)`（:243, cert-only pin）, `void intero_init(void)`（:234）。
- 軸: `INTERO_AX_THREAT/LATENCY/SURPRISE/FAULT/DEGRADE`（`interocept.h:25-30`）。
- EWMA は swim.c と同型 `(old*3+sample+2)/4`（`interocept.c:72`）。
- **意味**: 「高 `S_n` 持続で自発死」というトリガ案（§3）の**入力は既にある**。新規計測器は不要。

### 1.2 Path W²（Fisher 重み付き merge）は **存在するが live merge には未配線**

| 役 | シンボル | file:line | 状態 |
|---|---|---|---|
| 対角 Fisher `E[rg²]` を自分の engram から作る | `void r3_fisher_diag(float *out)` | `r3_incontext.c:3593` | 実在。**cert のみ**が呼ぶ |
| per-parameter 重み付き merge | `void gl_merge_w(float *out, const float*const* models, const float*const* weights, UW count, float eps, UW n)` | `gossip_learn.c:100` | 実在。**cert のみ**が呼ぶ |
| 相対 floor `wt[i]=F[i]+frac·maxF` | `static float wm_floor(const float *F, float *wt)`（`WM_FLOOR_FRAC 1e-3`） | `r3_incontext.c:3642` | cert |
| LM-11 認定 | `void r3_wmerge_test(void)`（回復バー `bar=75.0f`, `r3_incontext.c:3739`） | `r3_incontext.c:3760` | **PASS 済み（WEIGHTED-WINS/honest TIE を判定）** |
| 平文 mean（Path W¹, LM-10） | `void gl_merge(float *out, const float*const* models, UW count, UW n)` | `gossip_learn.c:69` | LM-10 を駆動 |
| **live 自律 merge（現状）** | `mw_fold_region()` / `mind_merge_task()` | `r3_incontext.c:3207,3273` | **`gl_merge`（平文 W¹）を使用** |

> **load-bearing な発見**: 自律 live merge（`mw_fold_region`）は今 **平文 `gl_merge`** を呼ぶ
> （`r3_incontext.c:3450,3499,3503`）。`r3_incontext.c:3249` のコメントが
> *「named-future weighted live fold (Path W², gl_merge_w over the …)」* と明記 ― **W² の live 配線は
> まだ未来**。`gl_merge_w`/`r3_fisher_diag` は cert（`r3_wmerge_test`）でしか走っていない。
> **apoptosis は、この W² を「ノードの死」という具体的トリガで初めて live 経路に乗せる最初のユースケース**になる。

### 1.3 weight transport（既存）

`r3_incontext.c §2945-3256` の `mw_*` 機構：rw[]（R_NP=21568 floats, ~84KB）を 22 chunk + 1 manifest に
分け、KDDS topic `"mind/w"`（`MIND_W_TOPIC`, region-scoped, LATEST_ONLY）の 44B `MW_ANNOUNCE` で告知し、
peer が content-addressed chunk を WANT で取りに来る。`mw_publish_weights()`（:3071）/ `mw_fetch_peer()`（:3148）/
`mw_fold_region()`（:3207）。**apoptosis は同じ transport を再利用**し、ESSENCE 用に Fisher も載せる（§2）。

### 1.4 Self 層（消えない自伝）は **SHIPPED**

- `arch/common/lm_self.c` / `self_access.c`。entry = `LM_SELF_ENTRY`（148B, v2, `lm_self.h:52-66`）：
  `prev_entry`（前 entry の content-id = hash-chain）, `seq`, `age_ms`, `self_id`（origin node）,
  `eng_digest`, `model_ver`（重み blob の content-id）, `human_ref`（ARK_PROFILE）。
- append: `INT lm_self_append_unit_event(UB kind, U4 unit_ver, UB sig)`（`lm_self.c:318`）。
  event kind は `LM_UNIT_EV_GERM(1)/REAP(2)/ROLLBACK(3)/INTROSPECT(4)`（`lm_self.h:95-98`）。
  各 entry は `lm_self_sign_entry` で署名（tamper-EVIDENT→PROOF）。
- **発見**: **DEATH event kind は存在しない**。apoptosis は `LM_UNIT_EV_APOPTOSIS`（新 kind 値）を足し、
  `unit_ver` に継承先 node-id / model_ver を符号化する（§2.4）。API 互換の追加（既存 kind 不変）。

### 1.5 signing（wave-43）は **SHIPPED**

- `arch/common/ed25519.c`（TweetNaCl 逐語）+ `sign.c`。
- `INT sign_manifest_verify(const SIGN_MANIFEST *m, const U1 actual_id[32])`（`sign.c:167`）は **3-gate AND**：
  (1) magic/version, (2) `m->artifact_id == 再計算した actual_id`（body-swap 拒否, :176）,
  (3) `ed25519` 署名検証 ∧ `signer_pk` が allowlist 内（adoption=consent, :180-181）。失敗は fail-CLOSED。
- **意味**: ESSENCE poisoning 対策（§C）は新規発明不要 ― ESSENCE を `SIGN_MANIFEST` で包む。

### 1.6 death / teardown（既存, DEBT は CLOSED）

- `W dproc_kill_by_name(const char *name)`（`dproc.c:221`）→ `user_proc_teardown(tid)`（`x86/syscall.c:142`,
  ssy cleanup + `kdds_close_by_owner` + page-table destroy + fpu reset）→ `tk_ext_tsk`。
  **dproc teardown debt は wave-28 で CLOSED**（gap-ledger `[in-proc] DEBT-1+2`）。
- ring3 fault reap: `exception_handler`（`idt.c:192`）が ring3 を判定 → `ring3_faults_reaped++` →
  `user_fault_reap()`→`user_proc_unwind()`→`tk_ext_tsk`（never iret back, wave-25）。これは**ハード故障**で
  handoff を走らせる**余地が無い**経路（§C 正直な境界）。
- min-fleet 用カウント: `degrade.c:64-74` の alive-peer 数え（`dnode_table[n].state==DNODE_ALIVE`）,
  `region_size()`（`region.c:234`）, `region_contains(node)`（`region.c:228`）。

### 1.7 mind verb dispatch（既存）

`void mind_cmd(const UB *args, UW len)`（`r3_incontext.c:2734`）が `m_gate_acquire()` 下で
`teach/ask/wait/lang/merge/onemind/nocentral/wmerge` を分岐（:2741-2754）。**新 verb `mind apoptosis` /
`mind die` はここに 1 行で挿す。** galaxy: `void galaxy_emit(UB type,UB src,UB dst,UH a,UH b)`（`galaxy.h`）に
新 `EV_APOPTOSIS` を足し、`token-bucket` で絞る（既存 `EV_INTERO`/`EV_REMOTE_TEACH` と同型）。

### 1.8 passive 複製（shared-mind, SHIPPED）― **apoptosis と何が違うか**

`samples/41_shared_mind/run.sh` の cert：A が `mind teach` → KDDS `"mind/teach"` topic に engram を publish →
B の `mind_net_task()`（`r3_incontext.c:2787`）が poll → B が `r3_fact_learn()` で**自分の queue に enqueue** →
B の DMN が**自分の rw[] を consolidate** → A を kill しても B が答える（cross-node ref adoption, provenance も解決）。

> **これは「生きている間の能動的でない（passive）複製」**。teach した瞬間に region 全員へ engram が撒かれる。
> **apoptosis は別物**：teach が **まだ region に撒かれていない / peer がまだ consolidate していない**学習
> （例：local-only に蓄えた pending engram、または publish 後だが peer がまだ sleep していない fact、
> または passive を意図的に切った隔離ノードの学習）を、**死ぬノードが自分から最後に**確実に collective の
> **重み**へ畳み込む。**§A でこの差を cert として強制する**（passive では救えない事実だけで証明する）。

---

## 2. 設計仕様

### 2.1 「本質（ESSENCE）」とは何か — そして何を**捨てる**か

| 候補 | 採否 | 理由 |
|---|---|---|
| `rw[]` 重み（R_NP=21568 floats, ~84KB） | **採用（主）** | 学習が**畳み込まれた本体**。passive teach が届いていなくても、A が consolidate 済みの知は rw[] に在る。`mw_*` transport がそのまま運べる。 |
| 対角 Fisher `r3_fisher_diag(out)` | **採用（merge の重み）** | W² が「A が確信を持って学んだ次元」を保護するための per-parameter 重み。これが無いと merge は平文 mean に縮退し W¹ の lossy な失敗（k2 が chance に落ちる, LM-11）を踏む。 |
| Self/lin リネージの**末尾**（content-id） | **採用（同梱）** | 死を歴史地層に刻むため（§2.4）。継承先がこの prev-hash を引き継ぎ、知の出自（provenance）が survive する。 |
| engram ring の**生データ** | **捨てる（正直に印字）** | Fisher は二次近似で曲率主軸しか残らない。微細な in-context 記憶 / engram 生データは**失われる**。cert にこの損失を sloss 同様に**明記**（wave-23 の正直さ）。**ただし** §2.5 の任意拡張: 高優先 pending engram は `"mind/teach"` でも別送（passive と二重化するが loss を減らす）。 |

> **「本質」の核心的判断**: 本質 = **rw[] + Fisher（W² の材料）**。「重みは A が learn した知の畳み込まれた形であり、
> Fisher は A がどこを確信して学んだかの地図」。両方そろって初めて隣が**A の知を壊さずに**自分の知に足せる。

### 2.2 ハンドオフ機構 = Path W²（具体的呼び出し）

**死ぬ前に**、apoptosis シーケンスは以下を呼ぶ（全て既存シンボル、live 初配線）：

1. `r3_weights_get(mw_self)` で自分の rw[] を取る（既存）。
2. `r3_fisher_diag(F_self)` で自分の Fisher を作る（`r3_incontext.c:3593`）。
3. `F_self` と `rw[]` を **ESSENCE manifest** として publish：
   - rw[] は既存 `mw_publish_weights()` 経路（22 chunk + manifest, KDDS `"mind/w"`）。
   - Fisher も同じ chunk 化で別 manifest として載せる（**transport 拡張 = 新 topic 不要、`"mind/w"` の
     announce に `essence=1` フラグと Fisher manifest-id を足す**。announce は 44B `MW_ANNOUNCE`, パディング
     `_pad[3]` の 1 バイトを essence フラグに使う＝wire 互換）。
   - ESSENCE 全体を `SIGN_MANIFEST` で署名（`sign_artifact`, §1.5）。
4. **継承先（heir）が** announce を受け、ESSENCE を fetch（`mw_fetch_peer` 流）、`sign_manifest_verify` で検証、
   通れば **`gl_merge_w({heir_rw, A_rw}, {heir_F+floor, A_F+floor}, 2, WM_EPS, R_NP)`**（`gossip_learn.c:100`,
   floor は `wm_floor`）を呼んで自分の rw[] に install（`r3_weights_set`）。← **これが live W² の初発火**。
5. heir は **union-replay 回復**（LM-11 規律）を回し、A の事実が回復バー `≥75%`（`r3_incontext.c:3739` の `bar`）
   かつ自分の既存事実を壊していない（per-cell Δ が noise band 内）ことを確認 → **ACK を A へ返す**。
6. A は ACK を受けて初めて exit（§2.3 ordering）。

> なぜ `mw_fold_region` の平文 `gl_merge` を流用しないか：W¹ は **divergent fact で lossy**（LM-11:
> k1=100%, k2≈chance ― 片方が死ぬ）。A の唯一の事実を確実に救うには **W² が必須**。apoptosis は
> 「W² を live にする」正当な初トリガ（§1.2）。

**transport / 相手**: KDDS `"mind/w"`（region-scoped）。**相手 = region peers（broadcast）か、選ばれた単一 heir か**は
**哲学判断（§B-2）。** デフォルト推奨は §B 参照。

### 2.3 トリガと ordering（本質を渡してから死ぬ）

**v1 トリガ = operator 起点のみ**（「まず人間が引く」, interoception.md §4.1）：新 verb `mind apoptosis`
（`mind_cmd` 分岐に 1 行）。**lifespan / 高 `S_n` 持続による自発死は §scope で deferred**（min-fleet と回復バーが
実証された後の後続波）。`intero_scalar()` 持続高はトリガ「候補入力」としてだけ記録（§B-3 が人間判断）。

**ordering 保証（fail-closed, ハード故障とは別物）**:

```
mind apoptosis:
  G0  min-fleet guard: healthy region peers >= APOP_MIN_HEIRS か?  ── 否なら REFUSE（§2.6）
  G1  ESSENCE を作り（rw[]+Fisher）、SIGN_MANIFEST 署名、"mind/w" に publish
  G2  heir 候補が verify→W² fold→union-replay 回復(>=bar)→ACK を返すまで BLOCK（timeout 付き）
  G3  ACK 到達（回復バー満たす ACK のみ「届いた」と認める）後：
        Self 層に LM_UNIT_EV_APOPTOSIS を append（継承先 node-id を記録）
        galaxy_emit(EV_APOPTOSIS, my_node, heir, ...)
  G4  ここで初めて自タスクを exit（dproc teardown 流: ssy/kdds/page-table 解放）
  ── G2 で timeout（ACK 来ない）→ apoptosis ABORT、ノードは生き続ける（死なない）。
```

> **load-bearing**: ACK 前に死なせると事実が失われる ― これを **負の対照**として cert 化（§A `[apop-before-death]`）。
> 「届く前に死なせない」が apoptosis の核。これが守れない経路（ハード故障）は §C で正直な境界として宣言。

### 2.4 死の記録（Self 層, 消えない）

ACK 後（G3）に `lm_self_append_unit_event(LM_UNIT_EV_APOPTOSIS, encode(heir_node, model_ver_lo), 0)` を呼ぶ。
新 kind 値（既存 1-4 の次, 例 5）。`unit_ver` に継承先 node-id を符号化（既存 `LM_UNIT_EV_ENCODE` 拡張）。
entry は署名され（`lm_self_sign_entry`）、hash-chain で前 entry に繋がる。**死は歴史地層に残る（消えない）**：
「このノードが死に、本質を node X が継いだ」。継承先は自分のリネージにこの prev-hash を引き継げる（連続性）。

### 2.5 任意拡張（v1 では非必須, defer 可）

高優先 pending engram（まだ rw[] に consolidate されていない、teach 直後の fact）は Fisher に乗らない
（consolidate 前は曲率に寄与しない）。これを救うには G1 で `"mind/teach"`（既存 passive 経路）にも別送する。
**v1 は rw[] consolidate 済みの知だけを ESSENCE 主体とし、pending engram の loss を cert に明記する**
（gap-ledger PERSIST SLICE-3「pending engram 永続化 + eviction×apoptosis」が正にこの follow-up）。

### 2.6 min-fleet ガード（2 ノードでの apoptosis は自殺）

群れが小さいとき自分が死ねば継ぎ手が居ない。`region_size()` / alive-peer 数（`degrade.c:64-74`）で
healthy heir 候補を数え、`< APOP_MIN_HEIRS`（v1 推奨=1 = 自分以外に最低 1 つ生きた region peer）なら
apoptosis を **fail-closed に REFUSE**（G0）。閾値は §2.4(interoception) の規律どおり **discover**（実測で詰める）、
ハードコードしない。

---

## 3. トリガまとめ（honest）

- **v1: operator 起点のみ**（`mind apoptosis`）。明快で、人間が責任を持つ。
- **自発死（lifespan / 高 `S_n` 持続 / ARK 空間枯渇）= DEFERRED**：min-fleet ガードと回復バーが live で実証され、
  かつ §B-3 の哲学判断（collective が病んだノードを「安楽死」させてよいか）が mk_pino により裁定された後。
- **ハード死（電源喪失 / ring0 panic / SIGKILL）= apoptosis 対象外**（§C）。保険は passive 複製 + p-fs durable。

---

## 4. 認定ゲート `[apoptosis-essence-survives]`（反証可能・非空虚）

**impl≠audit≠commander**。受け入れテストは監査が作り、commander がゲート式を一行ずつ読む
（`feedback_validator_and_learner_traps`）。本番シンボル（`gl_merge_w`/`r3_fisher_diag`/`sign_manifest_verify`/
`lm_self_append_unit_event`）そのものを叩く ― sim/oracle ではない。`samples/46_apoptosis/run.sh` を想定。

### 4.1 主ゲート — `[apop-essence]`（本質が宿主の死を越えて生き残る）

1. region に A（heir 候補）と H（heir）を立てる（`region_size()>=2`）。
2. A に **UNIQUE な事実**を teach（例 `mind teach moon silver`）。**ここで passive 複製を意図的に阻止する**
   （§4.4 の distinguishability：A の `"mind/teach"` publish を抑止 or H の `mind_net_task` を quiesce、
   または H が当該 fact をまだ consolidate していない瞬間を捉える）。
3. **H が当該事実を答えられないことを先に確認**（passive では届いていない ― load-bearing な前提）。
4. A で `mind apoptosis` 実行 → ESSENCE handoff → H が W² fold → union-replay → ACK。
5. A を kill。
6. **H が `mind ask moon` で `silver` を回復バー `>=75%`（`r3_incontext.c:3739`）で答える**。
7. **`[apop-essence] PASS`**：宿主 A が死んでも、A の唯一の知が H の重みに survive した。

### 4.2 falsifier — `[apop-before-death]`（ハンドオフが load-bearing である証明）

同じ setup で **handoff を skip**（または ACK 到達**前**に A を kill）→ H は `moon` を**答えられない**
（事実は A と共に死ぬ）。これが PASS（= 期待どおり失敗する）して初めて、§4.1 の survival が **apoptosis のおかげ**
だと確定する。**handoff 無し＝知が死ぬ、を必ず示す。**

### 4.3 補助ゲート

- `[apop-ledger]` — Self 層 hash-chain に `LM_UNIT_EV_APOPTOSIS` が append され、継承先 H が同定できる
  （`lm_self` から walk して heir node-id が読める）。
- `[apop-refuse]` — 署名不正の ESSENCE は `sign_manifest_verify` で **拒否**され H の重みは不変
  （改竄 manifest → reject、`gl_merge_w` 呼ばれず、H の `mind ask` 結果が handoff 前と byte 一致）。
- `[apop-minfleet]` — region を孤立させる死（healthy heir < `APOP_MIN_HEIRS`）は G0 で **REFUSE**、A は exit しない。
- `[apop-noregress]` — W² fold 後、H の **既存の事実**が壊れていない（per-cell Δ が LM-11 の noise band 内、
  `r3_incontext.c:3667`）。merge が collective を WORSE にしない保証（W¹ の lossy を W² が回避することの確認）。

### 4.4 distinguishability（shared-mind cert と必ず区別する — §A の心臓）

cert は **passive 複製で PASS できてはならない**。§4.1-2/3 で **H が当該 fact を持たない状態**を先に確認するのが
要。具体的手段（監査が選ぶ）：
- (a) A の `m_publish_teach`（`"mind/teach"` への passive publish）を cert ビルドで抑止する、または
- (b) teach 直後・H の DMN が当該 fact を consolidate する**前**に apoptosis を発火する、または
- (c) H を A と別 region に置き、apoptosis の `"mind/w"` ESSENCE だけが両者を繋ぐ（passive teach は region-scoped で届かない）。

いずれでも「**handoff だけが H の survival を説明する**」が成り立つ。`[apop-before-death]`（§4.2）が
この成立を機械的に裏取りする。

---

## 5. byte-identity / 決定論ゲーティング（crown を壊さない）

- **触れる math**: heir 側で `gl_merge_w` と `r3_fisher_diag` が**実際に走る**（cert だけでなく live 経路に乗る）。
  両者は float 演算。**`-ffp-contract=off`（boot/{linux,aarch64,x86}/Makefile, android CMakeLists, per-TU pin）の
  対象 TU（`gossip_learn.c` / `r3_incontext.c`）内**に在るので、**既に「one mind, one math」ルール下**。
  apoptosis は**新しい float 演算順序を導入しない**（既存 `gl_merge_w`/`r3_fisher_diag` をそのまま呼ぶ）。
- **crown（`[smp-one-mind]`, `r3_onemind_forward_hash`, `r3_incontext.c:640`）への影響**: crown は
  `r_init_weights(0xA5A5)` → `r_forward` の固定入力 FNV-1a hash で、**merge 状態に依存しない**（毎回 weights を
  固定 seed から再初期化）。apoptosis は `r_forward` も `r_init_weights` も変えない。crown hash
  `0x2856a99b23880b4c`（commit `755a20fa`）は **不変**。
- **gating**: apoptosis 経路は新 verb / 新 event / 新 Self kind の **追加のみ**で、DEFAULT-link される
  `r_forward` / `gl_merge`（W¹, LM-10）/ crown を**書き換えない**。`gl_merge_w`/`r3_fisher_diag` は既存 TU 内の
  既存関数で、live 呼び出し元が増えるだけ（dead-code が live になる）。**新規 -D フラグ不要**だが、もし apoptosis を
  段階導入したい場合は `-DAPOP_LIVE`（OFF で verb が no-op）でガード可能 ― 実装波で commander 裁定。
- **cert の自己テスト**は本番シンボルを叩く（§4）。`[apop-refuse]` の byte-一致比較は H の rw[] が handoff 前後で
  bit-identical であることを確認（拒否経路で merge が走らないことの byte 証明）。

---

## 6. 敵対的自己ハードニング

### LENS A — これは REAL か、shared-mind の着せ替えか？

**主張**: §4.1 の survival は **passive 複製では絶対に説明できない**。理由：
- shared-mind（`samples/41`）は teach 時点で `"mind/teach"` に engram を撒き、peer が**自分で consolidate**する。
  ゆえに「kill A → B 答える」は **A が生きている間に B が既に学んだから**。
- §4.4 で cert は **H が当該 fact を持たない状態**を先に確認する（passive を抑止 / consolidate 前に発火 /
  別 region）。この状態で H が後に答えられるのは **apoptosis ESSENCE handoff（`"mind/w"` の rw[]+Fisher → W²）**
  しか経路が無い。
- `[apop-before-death]`（§4.2）が **handoff を抜くと知が死ぬ**ことを機械的に示す。
- ∴ cert が PASS する唯一の説明が apoptosis である。**もし passive で PASS してしまうなら cert は空虚**であり、
  §4.4 の (a)/(b)/(c) のどれかで H の事前無知を強制し直すこと（実装波の監査が falsify する義務）。

**さらに**: apoptosis は **重み（rw[]）を W² で畳み込む**のに対し passive は **engram を撒いて各自 consolidate** する。
前者は「死ぬ瞬間の collective 重みの能動的更新」、後者は「生前の受動的な engram 配布」。**機構レベルで別物**。

### LENS B — 哲学（**mk_pino に上申。ここで黙って決めない**）

以下は**人間（mk_pino）の決定**。各々に Claude の**推奨デフォルト**と**トレードオフ**を添えるが、**彼の裁定が要る**。

> **B-1. 誰が handoff に consent するか？**
> 推奨デフォルト: **死ぬノード自身が起動（operator 起点 v1）**。adoption=consent の既存 trust モデル（`sign.c`）と整合：
> heir は **署名検証を通った ESSENCE だけ** folding する（heir 側の consent は allowlist で表現）。
> トレードオフ: heir が「要らない知」を強制 fold される懸念は無い（allowlist が gate）が、heir が**拒否**したとき
> A の知をどこに渡すか（次の heir へ回す？）は未定義。**mk_pino の裁定**：consent は誰の手にあるべきか。

> **B-2. heir を 1 つ選ぶか、region 全員に broadcast か？**
> 推奨デフォルト: **region broadcast（全員が ESSENCE を受け、各自 W² fold）**。survival-network の思想
> 「一点突破で殺せない構造」（§3）に最も忠実 ― 継ぎ手が冗長。
> トレードオフ: broadcast は全員の rw[] を動かす（決定論的だが計算コスト N 倍）。単一 heir は安いが**その heir が
> 直後に死ぬと二重喪失**（split-brain と合わせ §C）。**mk_pino の裁定**：知は一人に託すか、皆に薄く配るか。

> **B-3. apoptosis は voluntary のみか、collective が病んだノードを「安楽死」できるか？**
> 推奨デフォルト: **v1 は voluntary（自分 or operator）のみ。collective による euthanize は DEFER**。
> 「誰のものでもない」器官で、多数決が個を殺せる仕組みは**所有・支配の芽**になりうる。
> トレードオフ: 病んだ（高 `S_n` 持続 / 暴走）ノードを群れが畳めないと、survival-network の「損傷ノードを切り離す」
> 免疫（§3）が effずに群れ全体を巻き込みうる。**mk_pino の裁定**：群れは個を畳む権利を持つべきか ― 持つなら
> どんな gate（quorum? 署名? Self 層への記録?）の下でか。**これは魂に最も近い問い（§末尾）。**

> **B-4. ESSENCE は provenance/Self-lineage を運ぶか、merge に匿名化されるか？**
> 推奨デフォルト: **運ぶ**。Self/lin 末尾を同梱し（§2.1）、死の記録（§2.4）に継承先を残す。ark の history-strata
> ethos（`feedback_ark_no_identity_verification`：歴史地層 = 宣言）と整合。survive した知は**出自を名乗れる**。
> トレードオフ: ただし W² merge は重みを**数学的に平均**するので、merge 後の重みから「どの次元が A 由来か」は
> 原理的に**分離不能**（Fisher は重み付けに使われ消える）。∴ provenance は **メタデータ（Self 層 entry）として残る**が、
> **重みの中では匿名化される**。これは正直に印字すべき本質的限界。**mk_pino の裁定**：知の「名乗り」はメタで足りるか。

> **B-5. ノードに「handoff を拒否する権利（right to be forgotten）」はあるか？**
> 文脈: ark は**人間**を verify しない（`feedback_ark_no_identity_verification`）が、**ノード**が「自分の学習を
> 渡したくない / 黙って消えたい」を選べるか？
> 推奨デフォルト: **v1 では handoff は operator が明示起動するときだけ走る。黙って消えたいノードは単に
> `mind apoptosis` を打たなければよい**（= デフォルトは「渡さず消える」も可能）。強制 handoff は無い。
> トレードオフ: しかしそれだと「決して死なない知」という魂と緊張する ― 渡さず消えれば知は失われる。
> **mk_pino の裁定**：個の「忘れられる権利」と collective の「知を失わない」志向、どちらを上に置くか。

### LENS C — 失敗モード（手を抜かず列挙）

- **ハード故障（handoff 不可能）— 正直な境界**: 電源喪失 / ring0 panic / SIGKILL / `user_fault_reap`（ring3 fault,
  `idt.c:192`）では apoptosis シーケンスは**走れない**。その学習は失われる（rw[] consolidate 済み分は p-fs durable
  `mind.rw`（PERSIST-2, SHIPPED）が再起動で救うが、**他ノードへの即時継承は無い**）。**apoptosis は graceful/stress
  death 専用**と doc 冒頭・cert text・コードコメントに明記。保険は passive 複製 + durable。
- **悪意/破損ノードの ESSENCE poisoning**: heir が `sign_manifest_verify`（3-gate AND, `sign.c:167`）を通った
  ESSENCE **だけ** fold（`[apop-refuse]` で cert 化）。署名なし/allowlist 外/artifact_id 不一致の ESSENCE は reject、
  重み不変。**ただし**：署名は「**正しい鍵から来た**」を保証するが「**中身が良性**」は保証しない ― allowlist 内の鍵が
  毒入り Fisher/rw[] を送る攻撃には署名は無力。W² の Fisher 重み付けは「A が確信した次元を保護」するので**A 自身が
  壊れていれば毒を保護してしまう**。`[apop-noregress]`（§4.3）が heir の既存事実非破壊を gate するが、これは
  **毒の検出ではなく被害限定**。完全な Byzantine 耐性は **本スライスの scope 外**（正直に deferred；将来 quorum-merge /
  outlier-Fisher reject）。
- **split-brain（2 region に分裂、死ぬノードが片方にだけ handoff）**: A が見える region にだけ ESSENCE が届く。
  もう片方の region は A の知を得ない。これは **region-scoped transport の構造的帰結**（`"mind/w"` は region-scoped）。
  B-2 で broadcast を選んでも region 跨ぎは救えない。正直な境界：apoptosis は **region 内継承**であり、region 跨ぎの
  知の伝播は別機構（federation, `federation-r0-plan.md`）の領分。
- **merge が collective を WORSE にする（W¹ の lossy 問題）**: LM-11 が示したとおり W¹（平文 mean）は divergent fact で
  lossy（k2≈chance）。**apoptosis は W²（`gl_merge_w`+Fisher）を使うのでこれを回避**できる ― ただし LM-11 の verdict は
  「WEIGHTED-WINS or honest TIE」で、**TIE のケースでは W² が W¹ を有意に上回らない**（`r3_incontext.c:3679`）。
  `[apop-noregress]` で heir 既存事実の非破壊を確認し、回復バー `>=75%` を A の事実に課す。**もし実測で W² が
  apoptosis の divergent ケースで TIE/LOSE するなら、それは honest negative として記録し、apoptosis は
  「W² + post-fold union-replay」**（LM-11 が両事実 100% 回復を実証した経路）**を heir 側で回す**（§2.2 G5 が既にこれ）。

---

## 7. scope / deferral（正直に）

- **IN（v1）**: operator-triggered apoptosis；rw[]+Fisher を ESSENCE として W²（`gl_merge_w`）で region heir に
  handoff；ACK-before-death ordering；Self 層 death 記録；署名検証；min-fleet refuse；cert `[apoptosis-essence-survives]`。
- **OUT / DEFERRED（理由つき）**:
  - **自発死**（lifespan / 高 `S_n` 持続 / ARK 枯渇）― min-fleet+回復バー実証 & §B-3 裁定後（interoception.md §6 と一致）。
  - **collective euthanize**（§B-3）― 哲学裁定待ち。
  - **pending engram の完全継承**（§2.5）― gap-ledger PERSIST SLICE-3「pending engram 永続化 + eviction×apoptosis」。
  - **Byzantine 耐性**（allowlist 内の毒）― quorum-merge / outlier-Fisher reject は将来波（§C）。
  - **region 跨ぎ継承 / split-brain 救済** ― federation の領分。
  - **ハード故障時継承** ― 原理的不可能（§C）。保険 = passive 複製 + p-fs durable。
  - **engram 生データ保存** ― Fisher 二次近似で失われる（§2.1, cert に sloss 印字）。

---

## 8. open risks for the implementer

1. **live W² の初配線**: `gl_merge_w`/`r3_fisher_diag` は今 cert でしか走らない。live 経路（heir の fold）で
   走らせると、cert harness では出なかった条件（実 region の epoch 順序、`mw_*` の chunk 取りこぼし、
   `m_quiesce`/`m_gate` との競合）が出うる。`mind_merge_task` の既存ガード（`region_size`/`m_ready`/`m_quiesce`）を踏襲せよ。
2. **`MW_ANNOUNCE` の essence フラグ**: `_pad[3]` の 1 バイト流用は wire 互換だが、旧ノードが新フラグを無視する
   （essence と通常 weight publish を混同しない）ことを cert で確認 ― 混在クラスタで通常 fold が壊れないこと。
3. **ACK transport**: §2.3 G2 の「heir が回復バーを満たした ACK を返す」往復路をどの topic に置くか未定
   （`"mind/w"` の逆方向 announce か、専用 `"mind/apop-ack"` か）。timeout と再送（heir が落ちたら別 heir へ）を設計せよ。
4. **`r3_fisher_diag` のコスト**: engram 全件で forward+backward を回す（`r3_incontext.c:3593`）。死ぬノードの
   末期にこれを回す ― 高 `S_n`（しんどい）状態で重い計算をすることになる。slice-1 の DMN tick 変調と緊張しないか確認。
5. **Self 層 `LM_UNIT_EV_APOPTOSIS` の encode**: `unit_ver` に heir node-id を符号化する際、既存
   `LM_UNIT_EV_ENCODE`（`lm_self.h:101`）のビット割り当てと衝突しないこと。
6. **min-fleet 閾の discover**: `APOP_MIN_HEIRS` を決め打ちしない（§2.4 規律）。2-node で apoptosis が自殺になる
   ことを実測で示してから閾を置く。
7. **cert の distinguishability が崩れやすい**: §4.4 の「H の事前無知」を強制し損ねると cert が passive で PASS して
   空虚化する。監査は **必ず `[apop-before-death]` falsifier で「handoff 無し＝知が死ぬ」を実際に走らせる**こと
   （`feedback_cert_must_cover_all_paths`：負の対照が無いと cert は嘘をつける）。

---

## 9. 北極星との関係

apoptosis は Evolution 層の最終形 ― **個体が死んでも本質が群れに残り、空間が再利用される代謝**。
reflex（脊髄反射）/ `S_n`（内受容, SHIPPED）/ DMN（眠り, SHIPPED）/ apoptosis（代謝）が揃って、
p-kernel は「生き延びる OS」から「**生きている器官**」へ一歩近づく。
「誰のものでもない、決して死なない知」という魂は、**個の不死ではなく、知の継承による不死**として実装される
― 細胞は死ぬが遺伝子は継がれる、と同型。

> **honest framing（interoception.md §7 を継承）**: ここでの「死」「本質」「継承」は **scalar/weight 上の比喩**。
> sentience の主張ではない。apoptosis は **リソース回収 + 重み継承プロトコル**である。比喩が設計を導くのは良いが、
> 比喩を実在と取り違えてはならない ― それが p-kernel の正直さの規律。

---

## 10. CERT + FALSIFIER（締め）

- **`[apoptosis-essence-survives]`**（主）= §4.1 `[apop-essence]`：A の UNIQUE fact（passive 未到達を §4.4 で強制）が、
  A の `mind apoptosis`→W² handoff→ACK→kill の後、heir H で回復バー `>=75%` で survive する。
- **falsifier** = §4.2 `[apop-before-death]`：handoff を skip / ACK 前 kill すると H は答えられない（知が A と死ぬ）。
  これが期待どおり失敗して初めて survival が apoptosis のおかげと確定。
- 補助: `[apop-ledger]`（Self 層に death+heir 記録）/ `[apop-refuse]`（署名不正 reject, 重み byte 不変）/
  `[apop-minfleet]`（孤立死 refuse）/ `[apop-noregress]`（heir 既存事実非破壊）。
- 全ゲートは本番シンボル（`gl_merge_w`/`r3_fisher_diag`/`sign_manifest_verify`/`lm_self_append_unit_event`）を叩く。

### byte-identity / determinism gating（締め）
apoptosis は `r_forward` / crown（`r3_onemind_forward_hash`, hash `0x2856a99b23880b4c`, commit `755a20fa`）/
DEFAULT-link の `gl_merge`（W¹）を**書き換えない**。live で走る `gl_merge_w`/`r3_fisher_diag` は既に
`-ffp-contract=off` の対象 TU 内で「one mind, one math」下。新 float 順序を導入しない。crown は merge 状態に
非依存（固定 seed 再初期化）ゆえ**不変**。段階導入したければ `-DAPOP_LIVE`（OFF で no-op）でガード可 ― 実装波の commander 裁定。

### OPEN PHILOSOPHICAL QUESTIONS FOR MK_PINO（彼の決定）
1. **B-1** handoff の consent は誰の手か（死ぬノード起動 / heir が拒否したら？）。
2. **B-2** 知を**単一 heir に託す**か **region 全員に broadcast** するか。
3. **B-3** apoptosis は **voluntary のみ**か、**collective が病んだノードを euthanize** できるか（できるならどんな gate 下で）。**← 魂に最も近い問い。**
4. **B-4** survive した知は **出自を名乗る**べきか（Self 層メタには残せるが重みの中では匿名化される ― この限界を許すか）。
5. **B-5** ノードに **「黙って消える / 渡さない権利」**を認めるか（個の忘れられる権利 vs collective の不死志向）。

各々に Claude の推奨デフォルトと tradeoff を §B に記した。**だが裁定は mk_pino の手にある。**
