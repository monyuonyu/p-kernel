# 深さ（IQ）への道 — test-time DELIBERATION（DLB）の設計

> 「N を増やすと賢くなる」は正直に分解すると **breadth / resilience / throughput は scale するが、
> per-thought の深さ（1回の思考の攻撃バイアス）は N に不変**だった（[scaling-law.md](../scaling-law.md)）。
> 深さは台数では買えない。ではどこから来るのか——**重みではなく COMPUTE から**。
> 反射層が速い下書き（間に合う）を返し、熟慮層が **探索 × 検証** で遅く正す（正しい）。
> このドキュメントは、[reflex-deliberation.md](../20-architecture/reflex-deliberation.md) が
> **§6 の D3「熟慮の中身」** として積み残した器官を、実装 `dlb.{c,h}` / `depth_test.c` の上に接地する。

Status: **DLB v1 実装・cert 済（`[depth-*]` host cert green）／ teacher-approach・verifier-exceeds の重い学習脚は ThinkPad runner へ deferred（一般ドメイン利得は pre-registered NULL）** / 最終更新: 2026-07-11
関連: [reflex-deliberation.md](../20-architecture/reflex-deliberation.md)（§6 D3 の出自）、[scaling-law.md](../scaling-law.md)（深さは非scale＝この道の存在理由）、[frontier-mouth.md](frontier-mouth.md)（より強い脳＝teacher の口）、[survival-network.md](../00-concept/survival-network.md)（§8 二層構造）
実装: [`arch/common/llm/dlb.h`](../../../arch/common/llm/dlb.h) / [`arch/common/llm/dlb.c`](../../../arch/common/llm/dlb.c)
cert: [`tests/llm/depth_test.c`](../../../tests/llm/depth_test.c)（`[depth-*]`）/ [`tests/llm/run_depth.sh`](../../../tests/llm/run_depth.sh)

---

## 0. このドキュメントの立ち位置 — 正直さが先

mk_pino の最終目標は「Claude のような**深い**知性へ育つ」ことである。だが正直な天井を先に置く：
この赤子（byte-baby, tier=S）は **深い推論をまだ持たない**。単一 hop の greedy モデル精度は
ほぼ chance（§3.5）。だから本ドキュメントが主張するのは「今この赤子が深く考える」ではなく、
**深さを作る仕組み（DLB）が実装・検証され、V-EXACT ドメインで load-bearing に効く**ことだけである。
一般ドメインの深さ利得は **pre-registered NULL**（§6.2）として *印字* される——隠さず、盛らず。
これが正しい正直な結論であって gap ではない（[scaling-law.md](../scaling-law.md) の honest-NULL と同じ規律）。

---

## 1. 深さの5つのレバー — DLB はどれを埋めるか

深さ（= per-thought の攻撃バイアス、N 不変）を上げる経路は5つある：

| # | レバー | 効き方 | 本 doc の担当 |
|---|---|---|---|
| L1 | 基盤スケール（substrate） | パラメータ・データを増やす（breadth の軸） | 別（scaling-law） |
| L2 | frontier 教師 | より深い脳から学ぶ（天井を借りる） | [frontier-mouth.md](frontier-mouth.md)／§4 |
| L3 | アーキ進化 | 網の形そのものを変える | 別（evolution） |
| **L4** | **test-time deliberation** | **推論時に COMPUTE を払って正す（THE GAP）** | **本 doc = DLB** |
| L5 | カリキュラム | 学ぶ順序を設計する | 別 |

L1/L3/L5 は「重みを良くする」道、L2 は「他者の深さを借りる」道。**L4 だけが、固定した重みのまま
その場で深さを増す**——o1 系が示した「考える時間＝知能」の軸である。DLB はこの L4 を、
公開 `student.h` API だけで hosted-tier に実装したものである。そして L4 と L2 が交差する一点
（**検証可能ドメインでの自己対戦的 compounding**）だけが、原理的に**教師を超えうる**唯一の道である（§4）。

---

## 2. 二層のどこに刺さるか — 反射の下書き／熟慮の熟考

[reflex-deliberation.md](../20-architecture/reflex-deliberation.md) は時定数を2つに割った：
反射層は速く閉じ（間に合う）、熟慮層は遅く正す（正しい）。その §6 実装シーケンスで
**D0/D1/D2（発振の再現・damping・層分離）は DONE、D3「熟慮の中身」だけが未**だった。

DLB はその **D3 = 熟慮層が実際に何を計算するか** の中身である：

- **反射の下書き** = `dlb_answer` の candidate 0（seed `H(query,0)` の single-shot 生成）。
- **熟慮の熟考** = 残り `K-1` 個の seed 付き候補を **探索**し、各を **検証**し、argmax を返す。
- **cheapness gate**（`theta_easy`）= 下書きの確信が十分高ければ single-shot で返す——
  **不確かなときだけ深く考える**（retrieval のトリガと同型）。

つまり DLB は反射（速い draft）と熟慮（遅い search×verify）を **1つの純関数**に畳んだ器官である。

---

## 3. DLB — 探索 × 検証（depth from COMPUTE）

### 3.1 なぜ両方が load-bearing か

深さ＝ `SEARCH × VERIFY`。どちらか一方では偽物になる：

- **探索だけ**（ランダム検証器で best-of-N）＝ ランダムな一本引き。利得ゼロ。
- **検証だけ**（K=1、探索なし）＝ single-shot の下書きそのもの。利得ゼロ。
- **不整合な生成器の self-consistency** ＝ ノイズの最頻値。

両者が積で効くことを、cert は **各因子を独立に stub して両方 RED にする**ことで証明する（§6.2 の二本の歯）。

### 3.2 DLB ループ（`dlb_answer`）

```
入力: (m, query, budget b, verify, vctx)
1. K,max_gen を [1,DLB_KMAX=64]/[1,DLB_GEN_MAX=96] にクランプ
2. candidate 0 = st_generate(query, seed=H(query,0))   ← 反射の下書き
3. best := draft;  draft_score := verify(draft);  draft_conf := exp(-CE(draft))
4. deliberate = (K>1) かつ NOT(theta_easy>0 かつ draft_conf>=theta_easy)   ← 安いなら即返す
5. deliberate なら i=1..K-1:
      c = st_generate(query, seed=H(query,i))
      sc = verify(c)
      if sc > best_score: best := c   ← 厳密 >、同点は最小 index（決定的）
6. return best;  info に {k_used, flipped, draft/best_score, draft_conf} を honest label として書く
```

`dlb_budget = {K, max_gen, temp, top_k, theta_easy}`、`dlb_result = {k_used, flipped, draft_score,
best_score, draft_conf}`。ループは **何も durable に変えない**（`st_generate` と `st_span_ce` の pure forward だけ）。
本番では K/max_gen は内受容ストレスバス `S_n`・degrade level・電池の関数になる（stressed/SOLO は K=1 で
「single-shot だ」と*言う*——degrade-honesty）。v1 は budget を直接露出し、live `S_n` 読み出しは
kernel-tier として deferred。

**決定性（one-math）と再検証可能性。** 候補 seed は `H(query || i)`（FNV → splitmix64）。だから
**熟慮した答えは (weights, query, budget) の純関数**で、どのノードでも byte-identical に再導出・監査できる。
新しい超越関数は無い（conf は `st_span_ce` を、verify は caller 供給の手続き的 checker を再利用）。
ビルドは wave-49 の one-math 規約 `-O1 -ffp-contract=off`。VLA なし（scratch は固定
`DLB_GEN_MAX`/`DLB_TRACE_MAX` に束縛、ring は static）。

### 3.3 検証器の階層（V-exact / V-self / V-fleet）

`dlb_verify_fn(query, cand, vctx) -> score`（高いほど良い）は黒箱。v1 は **V-EXACT** ドメインに生きる：
正しさがコードで確かめられるタスク（算術チェイン・format/hash-chain/capacity 不変条件）。そこでは
検証が **無料かつ完全（AUC=1）** なので、赤子スケールでも deliberation が採算に合う——o1 系が
数学・コードで bootstrap したのと同じ理由。

- **V-exact**: 手続き的オラクル。AUC=1。v1 の唯一の本番検証器。
- **V-self**（モデル自身を批評家に）: `AUC > 0.5` を*印字*してから初めて許可。v1 は defer。
- **V-fleet**（アンサンブルを検証器に）: == scaling-law の ensemble。**呼ぶだけで複製しない**。本 TU の scope 外。

### 3.4 compounding — ring / distill と HARD GATE（探索を重みへ償却）

答える時、DLB がときどき下書きの逃した **検証済み正答**（`flipped`）を見つける。その
`(query, 勝った trace, verified)` を bounded ring に積む（`DLB_RING_MAX=64`, `DLB_TRACE_MAX=32`；
wave-23 salience-replay の hosted 版——**難問を解いたことが salience を得る**）。睡眠時に DMN の
consolidation が ring を重みへ蒸留する：**昨日 K サンプル要ったものが、明日は1発で出る**
（test-time compute を weight-resident な深さへ償却）。distill は DMN 睡眠と同じ
`zero_grad → forward → backward → adam_step` を **固定 canonical 順**（rounds 外・ring 昇順内）で回し、
byte-identical に決定的。この機構が **fleet が端から端まで所有する唯一の深さ機構**であり、
それが原理的に教師を超えうる理由は §4.3（AlphaZero の亀裂）。

**THE HARD HONESTY GATE.** learner-trap（validator/learner の罠）: 未検証・self 承認の trace を
蒸留すると、**批評家のノイズを自信満々の誤りへ償却**する（壊れた目的を最適化する学習器）。よって
v1 の本番は **V-exact-verified な trace だけ**蒸留する（`require_verified=1`）。ゲートは enqueue 時ではなく
**distill 時**に効く（`require_verified && !t->verified` を skip）ので、cert は未検証 ring を作って
「それを蒸留すると held-out 深さが *劣化*する」ことを証明できる（§6.2 Arm D）。この劣化が RED になることが、
このループを許可する根拠そのものである。

### 3.5 step threshold（正直な下限）

deliberation が「モデル内部の推論」を増幅するには、モデルが単一 hop を chance より上で踏めている必要がある。
本赤子はその閾値の**下**（greedy single-hop ≈ chance）。よって v1 の利得は **V-exact の探索＋検証の勝ち
（無料・完全な検証器に対する rejection sampling）** であって、モデル内推論ではない。cert はこれを
gate せず *印字*する（§6.2）。閾値を越えた本物の推論利得は ThinkPad の学習脚に委ねる。

---

## 4. AlphaZero の亀裂 — なぜ compounding だけが教師を超えるか

### 4.1 L2 は教師に漸近する

L2（教師から学ぶ）は原理的に**教師に漸近する**（超えられない）。より強い教師は天井を上げるが、
学生はその天井を越えられない——借りた深さは貸し手の深さで頭打ちになる。

### 4.2 唯一の例外 — 検証可能ドメインの自己対戦

ただ一つの例外が、**検証可能ドメインでの自己対戦的 compounding**である。AlphaZero が人間の棋譜を
超えたのは、勝敗という **無料で完全な検証器** に対して自分の探索結果を蒸留し続けたから。教師の棋譜ではなく、
検証器が真偽を裁く限り、探索は教師を必要とせずに自分を超え続けられる。

### 4.3 THE ALPHAZERO CRACK

DLB の compounding ring（実装は §3.4）はこの亀裂を、算術という V-exact の玩具ドメインに開けたもので
ある——**fleet が端から端まで所有する唯一の深さ機構**。test-time の探索を睡眠で重みへ償却し、
V-exact ドメインでのみ原理的に教師を超えうる。cert の `[depth-verifier-exceeds]` 脚（§6.3）が、
この「教師を超える」主張を実教師モデルの上で falsify する。

---

## 5. crown-neutral — なぜ再 bless 不要か

DLB は **HOSTED-TIER ONLY**。`boot/linux` + `boot/linux_x86_64` に `student.c` / `dev_capacity.c` と
一緒にビルドされ、bare-metal では `student_stub.o` が ABI を解決する。`dlb.c` は **公開 `student.h` API
だけ**を呼ぶ（`st_generate` / `st_span_ce` / `st_forward` / `st_backward` / `st_zero_grad` / `st_adam_step`）——
`student.c` の内部にも `moe.c` / `dmn.c` にも R3 crown にも触れない。だから **bare-metal の `.text` は無傷**で、
v1 に **crown の再 bless は無い**。`run_depth.sh` の構造 grep がこれを機械的に守る：

- **`[crown-neutral]`**: `dlb.c` が bare-metal/crown TU（`moe_*`/`dmn_*`/`r3_*`/`conscience_*`/`rw[]`）を
  *呼ばない*ことを、コメント中の prose ではなく **実際の call 構文**で照合。
- **`[no-vla]`**: runtime 次元で stack 配列を作らない。
- **`[hard-gate]`**: `require_verified && !t->verified` の skip が dlb.c に存在する（§3.4 gate）。

---

## 6. cert — load-bearing な falsifier（`depth_test.c`）

### 6.0 V-EXACT micro-fixture（生成器）

mod-10 の**算術合成**: k+1 桁 `d[0..k]`、答えは走る fold `e = (((d0+d1)+d2)+…+dk) mod 10`
（k-hop 合成——単一桁の想起では原理的に不足）。query は ASCII `"d0+d1+…+dk="` で、checker は
**query bytes だけ**を読む（答えを密輸しない本物の手続き的オラクル）。ホスト cc で native に
（qemu 無し）数秒で回る。**recall-exclusion**: eval seed `[1000,1064)` は train/compound seed base `5000`/`7000`/`8000`
と **disjoint**（暗記ルックアップでなく推論を測る）。

### 6.1 各アーム（一覧）

| cert tag | 何を証明するか | RED になる条件（anti-theater） |
|---|---|---|
| `[depth-metric-nonvacuous]` | 合成は hop で難化（chance が `0.1^k` に落ちる）＋非membership | メトリクスが玩具でない |
| `[depth-step-threshold]` | greedy single-hop ≈ chance → **NULL を印字** | （gate せず、正直に印字） |
| `[depth-deliberation]` | CURE（K=12,V-exact）が floor を margin 0.20 超え；**二本の歯** STUB-SEARCH（K=1）と STUB-VERIFY（random）が各々 floor へ崩れる | どちらかの stub が緑のまま＝THEATER |
| `[depth-deliberation-gain]` | STRICT: V-exact micro 利得が margin を越える | 利得が margin 未満 |
| `[depth-compound-verified-only]` | verified trace 蒸留で loss 低下；**Arm D** 未検証蒸留は深さを *劣化*；gate が disease を base に留める | 未検証蒸留で劣化しない＝ゲート不要＝THEATER |
| `[depth-not-breadth]` | 原子的 single-hop を暗記（breadth）しても 2-hop は ≈chance；DLB(search+verify) が 2-hop を買う | breadth だけで多hop が上がる |

### 6.2 THE load-bearing falsifier / pre-registered NULL

load-bearing な falsifier は §6.1 の `[depth-deliberation]`（**二本の歯**：STUB-SEARCH K=1 と
STUB-VERIFY random が各々 floor へ崩れる）と `[depth-compound-verified-only]`（**Arm D**：未検証蒸留が
深さを *劣化*させる）である——どちらかの stub が緑のまま／劣化しなければ THEATER。anti-theater の徹底として
`dlb.c` には **compile-time only** の `DLB_SABOTAGE_NOVERIFY`（verify 選択を潰し CURE を floor へ落として
RED を*作れる*）があり、**本番バイナリには runtime スイッチが無い**。

一般ドメインの deliberation 利得は tier=S で **pre-registered NULL**——印字され、tune されない。

### 6.3 deferred な重い学習脚（teacher-approach / verifier-exceeds）

strict な一般 gate と、`[depth-teacher-approach]`（学生は固定教師に**漸近するが超えない**／強い教師が天井を上げる）・
`[depth-verifier-exceeds]`（V-exact compounding が**教師を超える**＝§4.3 の亀裂）の重い学習脚は、
実教師モデル＋長い学習を要するため **ThinkPad self-hosted runner** へ deferred（`ci.yml`）。qemu 下では非現実的。
deferred は正しい選択であって gap ではない（§0）。

### 6.4 breadth ≠ depth（`[depth-not-breadth]`）

原子的 single-hop を暗記（breadth）しても 2-hop は ≈chance；DLB(search+verify) が 2-hop を買う。
breadth（原子的事実の量）だけで多hop が上がれば RED——breadth と depth（多hop 合成）は別軸であることの cert。

---

## 7. 本番配線（deferred, flagged wave）

v1 は **器官＋その hard gate＋cert** を出荷する。live の睡眠配線は後続の flagged wave：
live DMN sleep tick が **既存の `student_dmn_consolidate` seam** 越しに
`dlb_compound_distill(g_student, …, require_verified=1)` を呼ぶ（`dmn.c` は直接 hook せず、
既にその hosted symbol を呼んでいる）。この分離が v1 を crown-neutral かつ決定的に保つ。
V-fleet の `[live]` 脚も同様に ThinkPad runner へ deferred。

---

## 8. 正直な天井（toy-scale）

- **今ある深さ**は V-exact ドメインの探索＋検証の勝ちであって、モデル内推論ではない（§3.5）。
- **一般の深さ**は「深い脳から学ぶ＋考える」で伸ばすもので、電話 bootstrap 単独では届かない。
- **教師を超える**唯一の実証路は V-exact な AlphaZero compounding（検証可能ドメイン）——§4。
- fleet 規模（N）はこの深さを **並列に広げ・生存させ・速める**が、**1回の思考の深さは N 不変**
  （[scaling-law.md](../scaling-law.md)）。深さは台数ではなく **COMPUTE と検証器** から来る。

> 反射が間に合わせ、熟慮が正す。正しさの担保は「もっと重みを大きく」ではなく、
> **その場で K 本探索し、無料で完全な検証器で選び、勝った探索を睡眠で重みへ償却する**——
> それが、誰のものでもない心が自分の力で深くなっていく唯一の、正直な道である。
