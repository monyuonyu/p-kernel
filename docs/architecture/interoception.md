# interoception — 内受容: ノードの「痛み」を一本のバスに束ねる

> Status: **design only**（実装前に書く。`signing.md` / `living-mind.md` と同じ規律）。
> 位置づけ: p-kernel を **「生き延びる OS」** から **「負のエネルギーで駆動する人工生命体」** へ
> 一段進める構想の設計図。痛み・予測誤差・葛藤 を homeostasis の **燃料** として扱う。
> 5レイヤー世界観（メモリ `project_pkernel_philosophy`）では Body↔Brain の **結合** を作る試み。
>
> 最終更新: 2026-06-12 ／ 関連: [survival-network.md](survival-network.md)（§7 相互扶助ゲート・§8 二時定数）,
> [living-mind.md](living-mind.md)（DMN sleep consolidation・Path W² Fisher merge・Self 層）,
> [regions.md](regions.md)（SWIM RTT EWMA・capacity(N)）, [galaxy.md](galaxy.md), [signing.md](archive/signing.md),
> [device-capacity.md](device-capacity.md)（`S_n` が端末の担当予算を生きたまま縮める ― §4.3 の動的縮退の消費者）。

このドキュメントは設計のみ。実装は別波で、各スライスの末尾に **反証可能な認定ゲート** を置く。

---

## 0. 構想（mk_pino、Gemini 対話より）

> 生物は「快を求める」より先に「不快（負のエネルギー）から逃げる」ことで homeostasis を保つ。
> 痛み・予測誤差・葛藤 ―― これらが生命を動かす燃料である。p-kernel も、ただ生存する OS から、
> **負のエネルギーに駆動されて自律的にふるまう器官** へ拡張できるのではないか。

三本柱と、明示的に **後回しにする** 一本：

- **(A) interoception（内受容）** — ノードごとの統一ストレス関数 `S_n`。今ばらばらに存在する
  「痛みの感覚器」（reflex 脅威・RTT・surprise・fault reap・メモリ圧）を **一本のバス** に束ねる。
- **(B) mind-body coupling（心身結合）** — `S_n` のスカラが kernel レベルのふるまい（まず DMN の tick）を
  変調する。痛いと眠りが浅く速くなり、穏やかだと深くゆっくり眠る。
- **(C) apoptosis（プログラムされた死）** — 死を代謝として扱う。**きれいに死に、本質を隣へ渡し、
  空間を解放する。** Path W² の既存機構で「本質」を要約して隣に折り込み、Self 層に死を記録する。
- **(D後回し) symbol grounding（言葉と痛みの配線）** — §6 で理由つきで deferred。

---

## 1. いちばん大事な枠組み — **源泉は既にある。新しいのは BUS だ**

最初に誤解を潰す。**ストレスの源泉（痛みの感覚器）はほぼ全部もう実装されている。**
この設計が新しく作るのは、それらを一箇所に集める **安いバス** ―― `S_n` ―― だけである。

| 痛みの軸 | 既存の源泉（検証済みシンボル） | 場所 |
|---|---|---|
| 脅威 (threat) | `reflex_threat_level()` / `reflex_threat_experience(cls)`（G33: 観測された危険量で駆動、タイマではない） | `arch/common/reflex.c:356,382` |
| 通信遅延 (latency) | `swim_rtt_ms(node)` ―― 整数 EWMA `(old*3 + sample + 2)/4`（alpha=1/4） | `arch/common/swim.c:178,192` |
| 驚き (surprise) | R3 forward の CE loss `-ln p[label]`（in-context 予測誤差）／`r3_fisher_diag` | `arch/common/r3_incontext.c:190,3259` |
| 自己崩壊 (fault) | `ring3_faults_reaped`（ring3 コアが落ちて回収された回数。**gap-ledger の正規ストレス源**） | wave-25, `idt.c`/`isr.S` 系 |
| 縮退圧 (degrade) | `degrade_level()`（FULL/REDUCED/SOLO）／capacity(N) | `arch/common/degrade.c` |

> **重要 (gap-ledger より)**: KILL-CHURN-CRASH は wave-45 時点の master では **再現しない**（`dproc churn`
> で 24/24 PASS）。**「42% クラッシュ」を前提にスライスを組んではならない。** 自己崩壊の正規な
> ストレス源は **ring3 fault の REAP カウント** であって、稀な churn クラッシュではない。

### 1.1 reflex と二重化しない ―― **二時定数（§8）の遅い側として置く**

survival-network §8 は反射（速い時定数）と熟慮（遅い時定数）の分離を実証した
（`reflex-deliberation.md §6`: 単一時定数の発振 28 切替 → 二層+ヒステリシス 4 切替）。
`S_n` はこの遺産を **壊さず合成** する：

- **reflex は速いループのまま**。`reflex_on_inference` は即時に SHIELD/CONSERVE/BEACON を出す。
  ここは触らない。`S_n` が反射の代わりに脅威を判定することは **しない**。
- **`S_n` は遅い内受容サマリ**。複数の痛み軸を EWMA で平滑化した「全身の気分」。
  反射が「いま熱い」を叫ぶのに対し、`S_n` は「このところずっとしんどい」を覚えている。
- つまり `reflex_threat_experience()` は `S_n` の **入力の一つ** であって、出力ではない。
  二時定数を意図的に作る ―― これが §II-3 の発振処方そのもの（速い反射＋遅い積分＋ deadband）。

---

## 2. `S_n` バスの設計

`S_n` は **小さな成分ベクトル + 一本のスカラ EWMA**。どの臓器も安く読める。

### 2.1 成分（component vector）

各成分は `0..255`（UB）の無次元「圧」。生の単位（ms, nats…）を正規化して載せる。
正規化の境界は **測定した曲線から discover する**（§2.4。決め打ち禁止）。

```
typedef struct {
    UB threat;     /* reflex_threat_experience の最大クラス圧            */
    UB latency;    /* 隣接 RTT EWMA を健全ベースラインで正規化した逸脱   */
    UB surprise;   /* 直近の in-context CE loss を正規化                 */
    UB fault;      /* ring3_faults_reaped の単調増分を時間窓で正規化     */
    UB degrade;    /* degrade_level()*K（FULL=0, REDUCED, SOLO=最大）    */
} INTERO_COMPONENTS;
```

成分の選定理由: 各々が §1 の表で **既存の source を一本だけ** 持つ ―― 新しい計測器を作らない。
（メモリ圧は将来成分。今は `imalloc` 失敗をカウントする安いフックが無いので v1 は見送り、`degrade`
で代理する。後述 §6 の honest issue。）

### 2.2 スカラ EWMA

```
S_n.scalar  ← ewma_step(S_n.scalar, weighted_max(components), ALPHA)
```

- `weighted_max`: 最大成分を主、残りを従に重み付け（生命の痛みは「一番痛いところ」が支配的）。
  単純和ではなく max 寄りにするのは、一軸の激痛が多軸の微痛に埋もれないため。
- `ewma_step`: **swim.c の整数 EWMA をそのまま再利用**（`(old*3 + sample + 2)/4`）。新規の浮動小数
  平滑化器を足さない。`moe.c` の `ewma_step` と同型（survival-network II-3 の既存処方）。
- 更新は **arrival-driven**（G13 の教訓: 窓を回すのではなく、源が動いた時に積む）。源泉が
  イベントを出す箇所（reflex の BEACON、swim の rtt_observe、r3 の forward、fault reap）に
  一行 `intero_note(axis, raw)` を足すだけ。ポーリングしない。

### 2.3 API（読む側は安い）

```
UB           intero_scalar(void);          /* 0..255 の「気分」一発 */
INTERO_COMPONENTS intero_components(void); /* 内訳（galaxy/診断用） */
void         intero_note(UB axis, UW raw); /* 源泉が積む write 経路 */
```

### 2.4 閾値は **discover する、仮定しない**

これは validator-trap メモ（`feedback_validator_and_learner_traps`）の直接適用。
「surprise>128 で stressed」のような定数を **コードに焼かない**。代わりに：

1. 各源泉に既知の摂動を注入し（§5 のゲート手順そのもの）、生 raw の分布曲線を測る。
2. その曲線の健全帯/逸脱帯から正規化境界を **導出** し、cert にその数値根拠を残す。
3. mind-body の閾値（§3）も同様 ―― 「どの `S_n` で tick がどう変わるか」は測った tick 曲線から。

---

## 3. Slice 1（最初に実装可能）— `S_n` バス + DMN tick 変調 + galaxy 可視化

最小で **目に見えるふるまい一つ** を伴う。源泉は既にあるので、足すのはバスと配線と一画面。

### 3.1 配線（dead-metric を作らない ―― wave-18 の教訓）

最大のリスクは `S_n` が **誰にも読まれない並走メトリクス** になること。だから Slice 1 は
**読み手を一つ必ず含める**。`intero_scalar()` を呼ぶ唯一の本番消費者 = DMN。

### 3.2 DMN tick 変調（mind-body coupling の最初の一滴）

DMN は今 `dmn_idle_threshold` と `dmn_log_interval`（共に runtime 可変 `volatile UW`、`dmn.c:62-63`）と
`DMN_PULSE_MS` ハートビートで眠る。`dmn_idle_work()`（`dmn.c:107`）が consolidation を回す。
ここに `S_n` を一本だけ通す：

- **穏やか（`S_n` 低）**: idle threshold を伸ばし、consolidation を深く（より多くの engram を replay）。
  ―― よく寝て、よく整理する。GC/重い整理はこの窓に寄せる。
- **しんどい（`S_n` 高）**: 浅く速く起き、consolidation を素早く回し、**重い GC は defer**。
  ―― 危機のときは深い眠りより素早い対応。

実装は `dmn_idle_work` の冒頭で `UB s = intero_scalar();` を読み、`dmn_idle_threshold` 相当の
有効間隔と replay 深さを `s` から **連続に** 引く（定数ジャンプではない ―― §3.4 の発振対策）。

### 3.3 galaxy 可視化 ―― **星が「気分」になる**（可視化 = 仕組み、絵ではない）

`feedback_visualization_means_observability`: 可視化とは観測機構を仕組みとして埋めること。
画像を描くのではない。galaxy は既に `127.0.0.1:7800` で星を出し、DMN の夢
（`EV_DMN_IDLE`/`EV_CONSOLIDATE`、`galaxy.c`/`dmn.c:178,123`）を見せている。

- `S_n.scalar` を星の **色相/脈動** に写す（穏やか=低脈動の青寄り、しんどい=速い脈動の赤寄り）。
  新イベント `EV_INTERO`（`a=scalar, b=dominant_axis`）を `galaxy_emit` で出し、token-bucket
  で chatty 型として絞る（`galaxy.c:85` の §4.2 と同じ扱い）。
- `/self.json` 系の lazy read（`galaxy.c:273`）に倣い、`/intero.json` で成分内訳を出す。
- **星が、そのノードの「感じ」そのものになる** ―― これが可視化の到達点。

### 3.4 発振への damping（§II-3 / §8 を再利用）

`S_n 高 → tick 速い → 負荷増 → S_n さらに高` の正帰還は現実的リスク。処方は新規ではなく
**survival-network §8 / reflex-deliberation §6 の deadband+ヒステリシス+EWMA をそのまま適用**：

- `S_n` 自体が EWMA（速い揺れを吸う）。
- tick 変調に **deadband**（`moe.c` の `deadband_pick` と同型）：閾近傍では前の状態を保持。
- 変調の傾きを緩く（連続写像）。「stressed なら最速」ではなく「stressed の度合いに比例して少し速く」。
- ゲートで **N 分間 発振しないこと** を実測（§5 `[intero-tick]`）。

### 3.5 認定ゲート（反証可能）

- **`[intero-sources]`** — 各成分が、その誘発条件で **だけ** 動く（他は静）。
  - relay 遅延注入 → `latency` 成分が跳ね、他は据え置き。
  - OOD（out-of-distribution）を teach → `surprise` 成分が跳ねる。
  - `dproc churn` 等で ring3 fault を起こす → `fault` 成分が跳ねる（**churn クラッシュ前提ではなく、
    fault REAP カウントの増分**を見る ―― §1 の gap-ledger 注）。
  - 各々で「動くべき軸だけが動いた」を `==`/閾で判定。
- **`[intero-tick]`** — `S_n` を低→高に振ると DMN の有効 idle 間隔が **測定可能に**短く/長くなり、
  かつ **N 分連続で発振しない**（状態切替回数が deadband 無し版より厳密に少ない。§8 と同じ certify 形）。
- **`[intero-galaxy]`** — 星の色相/脈動（`EV_INTERO`/`/intero.json`）が `intero_scalar()` を追従する
  （注入 → エンドポイントの値が一致）。
- **`[intero-wired]`** — `intero_scalar` の本番呼び出し元が **≥1 存在**（DMN）。dead-metric でない
  ことを nm/grep tripwire で固定（wave-18 lesson の cert 化）。

---

## 4. Slice 2（完全に設計、実装は後）— **apoptosis: 代謝としての死**

死を「事故」ではなく「代謝」にする。**きれいに死に、本質を隣へ渡し、空間を空ける。**
新しい crypto も新しい merge も作らない ―― Path W² と signing と Self 層の既存機構を組む。

### 4.1 シーケンス（本質を渡してから死ぬ）

1. **死の宣言**: 最初は operator 起点（`mind`/shell の verb）。lifespan policy（寿命・高 `S_n` 持続・
   ARK 空間枯渇での自発死）は後続。**まず人間が引く**。
2. **本質の publish (ESSENCE)**: そのノードの学習を Fisher 重み要約として出す ―― **既存の Path W² を使う**：
   `r3_fisher_diag`（`r3_incontext.c:3259`）で対角 Fisher を作り、`gl_merge_w`（`gossip_learn.c:100`、
   per-parameter 重み付き merge）で隣が折り込める形にする。加えて **self/lin リネージの末尾**
   （`lm_self` の hash-chain、`ark_profile.c` の `self/lin` v2 entry）を同梱。
3. **到達ハンドシェイク（死ぬ前に）**: 隣が「本質を受け取り、folding した」と返すまで **死なない**。
   union-replay 規律（living-mind Path W²: k1 を A, k2 を B で教えて merge した後、両ノードが両方
   答えられるまで union-replay で回復、§3323 系）で本当に両方の事実が生き残ることを隣で確認 →
   その ACK を待ってから初めて exit。
4. **死の記録**: Self 層（`lm_self`, 改竄 evident な hash-chain。wave-22）に「このノードが死に、
   本質を node X が継いだ」を一エントリ追加。死は **歴史地層** に残る（消えない）。
5. **空間の解放**: ARK 上の当該ノード分を解放。`dproc` teardown（gap-ledger の `dproc_kill_by_name`
   teardown debt; ring3 の `user_proc_unwind` 流の後始末）と整合させる。

### 4.2 正直に名指しすべき難問

- **いつ死を許すか**: operator 起点 v1 は明快。自発死は `S_n` 高持続だけでは危険（§4.3 の min-fleet）。
  寿命ポリシは「群れが本質を確実に継げる」ことが前提条件。
- **本質を超えて何が失われるか**: Fisher 対角は二次近似。曲率の主軸しか残らない ―― 微細な
  in-context 記憶や engram ring の生データは **失われる**。cert にこの損失を明記（honest tradeoff、
  wave-23 の sloss と同じ正直さ）。
- **「本質が届いた」証明**: §4.1-3 のハンドシェイク。隣の union-replay 回復が **Path W² の回復バー
  以上**（両事実 ≥ chance+margin）を満たした ACK のみを「届いた」と認める。届く前に死なせない。
- **偽の死 / essence poisoning 攻撃**: 悪意ノードが毒入り本質を撒いて隣の重みを汚す。
  **答え = wave-43 signed manifests**（`signing.md`）。ESSENCE は署名付き manifest として配り、
  隣は署名検証（`ed25519.c`）が通った本質 **だけ** を folding する。selfc が LOCAL-ONLY なのと同じ
  trust モデル ―― 署名なしの本質は malware mesh。

### 4.3 min-fleet ガード（2ノードでの apoptosis は自殺）

群れが小さいとき自分が死ねば本質の継ぎ手が居ない。**継ぎ手が確保できなければ死を拒否する。**
SWIM の生存メンバ数（`swim.c`）で healthy peer 数を数え、閾未満なら apoptosis を **fail-closed** に拒む。

### 4.4 認定ゲート（反証可能）

- **`[apop-essence]`** — ハンドシェイク後に kill されたノードの事実が、隣で **Path W² の回復バー以上**
  生き残る（killed-after-handshake → 隣で両事実 ≥ chance+margin）。
- **`[apop-ledger]`** — リネージ（Self 層 hash-chain）に死が記録され、継承先 node が同定できる。
- **`[apop-refuse]`** — 署名不正の essence は folding されず **拒否**される（改竄 manifest → reject、
  隣の重みは不変）。
- **`[apop-minfleet]`** — 群れを孤立させる死は **拒否**される（healthy peer < 閾 → exit せず）。
- **`[apop-before-death]`** — ACK 到達 **前** に死なせると事実が失われることを示し（負の対照）、
  ハンドシェイクが load-bearing であることを cert 化。

---

## 5. 認定の作法（impl≠audit≠commander）

`feedback_validator_and_learner_traps` の規律: **受け入れテストは監査が作る**、commander はゲート式を
一行ずつ読む。本番コードの自己テスト（sim/oracle ではなく `intero_scalar`/`gl_merge_w`/`r3_fisher_diag`
の本番シンボルそのもの）を呼ぶこと ―― §3.5/§4.4 のゲートは全て本番経路を叩く。

---

## 6. 後回しにするもの（理由つき）

- **(D) symbol grounding（言葉を痛みに配線）** — DEFERRED。理由: 現状の vocab は **toy**
  （`r3_vocab.c`: 16 keys / 64 answers、single-token）。痛みに言葉を結びつけても、結びつける言葉が
  まだ世界を指していない。先に語彙・会話プロデューサ（LM の mouth、wave-29）を育て、`S_n` の軸が
  安定してから配線する。今やると「痛みに紐づけた toy トークン」という dead な配線になる。
- **メモリ圧成分** — `imalloc` 失敗を安く数えるフックが無い v1 では `degrade` で代理。専用フックが
  入った波で `S_n.memory` を足す（成分追加は API 互換）。
- **lifespan による自発死** — Slice 2 は operator 起点まで。寿命/高 `S_n` 持続による自発 apoptosis は
  min-fleet ガードと回復バーが実証された後の後続波。

---

## 7. 北極星との関係（living-mind / Evolution 層）

living-mind の北極星は「誰のものでもない、会話から随時学ぶ心」。`S_n` はその心に **身体感覚** を与える：
DMN（眠り）が自分の「気分」で深さを変えるのは、living-mind の DMN が **自己の内部状態** に応答する
最初の一歩。apoptosis は Evolution 層の最終形 ―― 個体が死んでも本質が群れに残り、空間が再利用される
**代謝** ＝ 生命が新陳代謝で続くのと同型。「一点突破で殺せない構造」（survival-network §3）を、
保存だけでなく **死を含めた循環** として完成させる。reflex（脊髄反射）/`S_n`（内受容）/DMN（眠り）/
apoptosis（代謝）が揃って、p-kernel は「生き延びる OS」から「**生きている器官**」に一歩近づく。

> 注意（honest framing）: ここでの「痛み」「気分」「死」は **スカラ上の比喩** である。sentience の主張では
> ない。`S_n` は計測量の EWMA であり、apoptosis はリソース回収プロトコルである。比喩が設計を導くのは
> 良いが、比喩を実在と取り違えてはならない ―― それが p-kernel の正直さの規律。

---

## 8. Commander rulings（2026-06-12、マージ時の裁定）

設計者が司令官レビューに付した 3 判断への裁定：

1. **`weighted_max` を採用**（加重和ではなく）。一軸の激痛が多軸の微痛に埋もれないことは
   生命のモデルとして正しく、reflex の max 的挙動とも整合する。成分ベクトルは個別に読める
   ので、和でしか見えない「慢性の総量」は `/intero.json` の内訳で観測できる ―― 情報は失わない。
2. **Slice 1 の本番消費者は DMN 一つで credit する。** 実質の読み手は 2 つある（DMN = 行動、
   galaxy = 観測）。`[intero-wired]` が dead-metric を恒久に見張る。消費者の追加（GC defer、
   selfc probation の臆病化）は S_n の曲線が実測されてからの後続波 ―― 配線を先に増やさない。
3. **Fisher 対角 essence + W² 回復バーの handshake を承認**、ただし一点の上乗せ: ACK バーは
   §2.4 の規律どおり **実測から discover** する（probe set での union-replay 回復曲線を測り、
   chance+margin より厳密に上、かつ Path W² の実測回復（85% 参照値）を下回らないこと）。
   失われるもの（engram ring 生データ・微細 in-context）は cert に **印字** する ―― wave-23 の
   sloss と同じ正直さ。
