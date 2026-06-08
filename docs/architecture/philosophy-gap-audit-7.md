# Philosophy-Gap Audit — 第7版 (G22 受け入れ条件と cross-node 学習の検証)

> 第16波 D隊（常設の自己監査器官）。祝賀しない。実装が思想からどこへ逸れるかを file:line で確定する。
> 第6版 (G35/G36/G37, §5 plural) を承ける。第6版の評決は鋭かった:
> 「§5 が完璧に plural な *保護* を達成しても、それは『並行する図書館』にすぎない。
>  ノード間を渡って未来を強くする学習は **0**(FedAvg `E_NOSPT`、熟慮はスカラ 1 個)。」
>
> 第16波の別隊が **G22（中央集約器なしの分散協調学習）を いま実装中** である:
> N ノードが互いに素なシャードで学習し、gossip+merge でモデルを混ぜ、
> どのノードも自分の単独シャード上限を超え、kill されても学習が継続する。
>
> 本書（第7版）の任務:
>   1. 指揮官に **CI-grep 可能な G22 受け入れテスト** を渡す。FAKE-learning の手口を機械的に列挙する。
>   2. master NOW で fedlearn が本当に E_NOSPT / b3 のみか証明する（cross-node weight delta = 0 を示す）。
>   3. G13（coordinator 200ms 集約窓の再直列化）の現況。協調学習は遅い帯域 — この窓と衝突するか。
>   4. G33/G34（反射の温度バケツ脅威源・死んだ確信度ゲート）の現況。G22 後に「熟慮→学習→反射の再接地」の道はできるか。
>   5. G23（federation 依然 doc-only・DNODE_MAX=32）。協調学習が乗ると 32 上限が「全体の総合力」を縛るか。
>   6. 次の本丸（G38+）を思想から選ぶ。
>
> 重さ: 🔴 思想の核に反する／🟡 思想を弱める／🟢 軽微・前進。実証: ✅ 実機/CI　📖 コード読み
> 検証時点: master tip `ed18b8a`（worktree clean）。G22 実装はまだ master に着地していない（`git status` clean、`samples/` に collective-learn 系なし）。
>
> **[訂正 第8版より]** 本書 §4.3 / §8 の「学習→反射への矢印は無い／二層は完全に分離」は **dtr 経路を
> 見落としていた**。dtr 推論経路は実 max-softmax 確信度を反射へ渡し（`dtr.c:1246/1342`）、G22 の live
> gossip は収束モデルを live dtr 重みへ書き戻す（`gossip_learn.c:608`）— 前向き矢印の半本は既に実在する。
> 死んでいるのは moe §7 経路の 0xFF（`moe.c:419`）と、逆矢印（guard 経験→学習）と、実証。詳細は
> philosophy-gap-audit-8.md §2.3。

---

## 0. 要旨（先に結論）

- **fedlearn は master NOW で「b3 バイアス 6 個だけ・単一ノードだけ」が真。** 局所学習は
  635 パラメータ中 `delta_w1/w2/w3` を **確保して 0 のまま**放置し、有限差分ループは
  `b3` のみ回す（`fedlearn.c:91-96, 99-119`）。cross-node FedAvg は `E_NOSPT`
  （`fedlearn.c:165-183`）。**今日の cross-node weight delta = 0 を証明済み**（§2）。
- **G13・G33・G34・G23 はいずれも第6版から 1 文字も動いていない。** 200ms 同期窓
  （`dkva.c:252-269`）、5s タイマ解除（`reflex.h:60`）、死んだ確信度ゲート（`moe.c:419`）、
  DNODE_MAX=32（`drpc.h:35`）。
- **G22 が landing しても、それ単体では §9「考える器官」にならない。** 熟慮（学習）と反射
  （今を守る）の二層（§8）が **互いに一切触れない**。次の本丸（G38）はその二層結合。

---

## 1. 指揮官への納品物 — G22 受け入れテスト（CI-grep 可能・FAKE 手口を機械的に弾く）

第6版が名付けた病：「**緑の self-test / 死んだ live 経路**」。協調学習は同じ病に最も罹りやすい。
in-process の synthetic で「collective > solo」を一度出すのは容易い（R3b `breathe` が既にそれをやっている。
§1.5 参照）。本テストは **4 つの FAKE 手口を 1 つずつ機械的に弾く**よう設計する。

### 1.1 着地先（提案）

- 新サンプル `samples/NN_collective_learn/run.sh`（live N-node、`./relay` 経由、27/28 と同型）。
- 新 CI job `collective-learn-live`（`.github/workflows/ci.yml`、`protect-loop-live` / `plural-protect-live`
  と同型。`RESULT: PASS` を grep）。
- in-kernel の決定的補助 self-test `[collective] ...`（数値の決定性確認用、live の代わりにはしない）。

### 1.2 受け入れ判定（4 手口 × FAKE の tell × REAL の証拠）

各行は **CI が `grep -aF` で拾える固定トークン**を出すこと。トークンは提案（命名は実装隊に委ねる）。

#### (a) COPY not COMBINATION — 「一ノードの重みを複製しただけ」（= 魂デモ／同一コピー）

- **FAKE tell**: merge 後、全ノードが **同一の重み**を持つ（= ある 1 ノードの重みのコピー）。
  これは survival-network.md §4 が名指しした「混合ではなく同一コピーが N 個」そのもの。
  master には既にこの基準点がある — `spec.c` の **control 行**
  「all-same-weights, audit §4 old state」（`spec.c:317-324`、`r_copies`）= 64.4%。
  協調学習が control と同じ accuracy しか出さない、あるいは merged の content-hash が
  どれか 1 ノードの重みと一致するなら **COPY であって COMBINATION ではない**。
- **REAL の証拠（両方を print）**:
  - 各ノードの **単独シャードのみ**の accuracy が **FULL タスク上で低い**ことを示す：
    `[collective] node<k> solo-shard FULL=XX.X%`（k=0..N-1。互いに素なシャードなので各々低い）。
  - gossip 学習後のモデルの **FULL タスク** accuracy が **どのノードの solo 上限も超える**：
    `[collective] merged FULL=YY.Y%`、続けて
    `[future-stronger] merged(YY.Y) > max_solo(ZZ.Z) PASS`（YY > すべての solo の最大なら PASS）。
  - merged != copy の構造証拠：`[collective] weights pairwise-distinct PASS`（全 N ノードの
    最終重みが互いに content-hash 相違、かつ merged が **どの単独入力とも一致しない**）。
- **なぜ spec.c では不足か**: `breathe` は 1 カーネル内で 4 エキスパートを **別 seed・別シャード**で
  学習し join で上がるが（`spec.c:317-324, 375-376`）、(i) ノード間を渡る学習ではなく、(ii) CI 非配線
  （`ci.yml` に `breathe` の grep 無し）、(iii) sparse 発火（推論）の話であって weight の cross-node
  merge ではない。**G22 は「別ノードが学んだ重みが gossip で混ざって全員が上限を超える」を要求する。**

#### (b) HIDDEN CENTRAL AGGREGATOR — 「1 ノードが全員を集めて平均する」

- **FAKE tell**: ノード 0（または固定の coordinator）が全ノードの delta を集めて平均し配り返す。
  これは master の現 fedlearn コメントが自認する「demo 簡易 = 中央集約器」（`fedlearn.c:130-138`）で
  あり、§7「中央ゲートを置けば潰された瞬間に全体が止まる」に正面から反する。
  DKVA の `coordinator_aggregate`（`dkva.c:233-288`）も region 内 1 点へ集める形 = この手口の同類。
- **REAL の証拠**: peer-symmetric。各ノードは **自分の近傍のモデルだけ**を読み、サーバを持たない。
  - `[collective] node<k> merged from neighbours {a,b}`（k ごとに **異なる近傍集合**、全集合ではない）。
  - **構造保証**: どの単一ノードも全 N モデルを受信していないことを log から機械検査
    `[no-central] no node received all N models PASS`。
  - **kill 検査**（(c) と重複・最強の証拠）: 「集約器役」になりそうな node0 を kill しても学習が
    **止まらない**（残りが互いの近傍から merge し続け、上限超えを維持）。これが真に「中央が無い」
    ことの唯一の決定的証拠。`[no-central] killed node0 mid-learn; remaining still > solo PASS`。

#### (c) GREEN ONLY IN-PROCESS — 「単一プロセスの synthetic でだけ緑」

- **FAKE tell**: 「collective > solo」が **1 カーネル内**の self-test でしか出ない（spec.c `breathe`、
  あるいは `[moe-concurrent]` 型の手組 utility）。live multi-node でも kill 中でもない。第6版 §9 が
  「緑の self-test / 死んだ live 経路」と名付けた病の協調学習版。
- **REAL の証拠**: **live N-node**（N>=3）を `./relay` 経由で起動し、**学習の途中で kill -9** を注入。
  **>=5 回**回して **毎回** PASS（non-flaky）。CI が `protect-loop-live`/`plural-protect-live` と同型で
  `RESULT: PASS` を grep。
  - `run.sh` 内: `for i in $(seq 5); do ... ; grep -q 'RESULT: PASS' || exit 1; done`。
  - kill タイミングは **学習が上限を超える前**（mid-learning）に注入し、残りノードで上限超えに
    到達することを示す（保存ではなく **学習が** kill を生き延びる）。
  - in-kernel `[collective] ...` self-test は決定性確認の補助に留め、**live PASS を昇格させない**。

#### (d) BODY-FROZEN — 「b3 バイアスしか動かない」（今日の fl_local_train そのもの）

- **FAKE tell**: 重み本体（w1/w2/w3）が 1 つも動かず、最終層バイアス `b3` だけ有限差分で動く。
  これは master NOW の **正確な現状**（`fedlearn.c:91-96` で `delta_w1/w2/w3` を 0 に初期化し、
  `fedlearn.c:99-119` のループは `b3` のみ）。
- **REAL の証拠**: weight body が **ノードをまたいで**更新される。各ノードが層ごとの delta の
  L2 ノルムを print し、**w1/w2/w3 すべて非ゼロ**であることを assert：
  - `[collective] node<k> |dW1|=.. |dW2|=.. |dW3|=.. |db3|=..`
  - `[body-moves] |dW1|>0 && |dW2|>0 && |dW3|>0 PASS`（b3 だけが 0 でない、を機械的に弾く）。

### 1.3 一行サマリ（指揮官向け）

> **G22 が真であるための CI 緑は、次の 5 トークンが live job `collective-learn-live` で全部出ること:**
> `[future-stronger] ... PASS`（(a) 上限超え）/ `[collective] weights pairwise-distinct PASS`（(a) 非コピー）/
> `[no-central] ... PASS`（(b) 近傍のみ + node0 kill 後も継続）/ `RESULT: PASS` ×5（(c) live・kill・非flaky）/
> `[body-moves] |dW1|>0 && |dW2|>0 && |dW3|>0 PASS`（(d) 本体が動く）。
> このうち 1 つでも欠ければ G22 は「並行する図書館」のままで、§9「考える器官」には届かない。

---

## 2. master NOW の検証 — fedlearn は本当に E_NOSPT / b3 のみか（cross-node delta = 0 の証明）

**真。第6版から不変。** 証拠:

- **局所学習は b3 のみ。** `fl_local_train` は w1/b1/w2/b2/w3/b3 の delta を引数に取るが、
  `fedlearn.c:91-96` で **6 本すべてを 0 に初期化**し、その後の有限差分ループ
  （`fedlearn.c:99-119`）は **`for (j = 0; j < MLP_OUT; j++)` で `b3[j]` だけ**を ±FL_EPS して
  `delta_b3[j]` を埋める。**`delta_w1/delta_b1/delta_w2/delta_b2/delta_w3` は 0 のまま返る。**
  → weight body（w1/w2/w3）の局所勾配は **存在しない**。
- **cross-node 集約は E_NOSPT。** `dtk_fl_aggregate`（`fedlearn.c:140-183`）:
  - `aggregator_node == drpc_my_node || drpc_my_node == 0xFF`（= 単一ノード）のときだけ
    自分の `delta_b3` を `b3` に足して `E_OK`（`fedlearn.c:146-163`）。**ここでも b3 のみ。**
  - それ以外（= 実 cross-node）は **`return E_NOSPT`**（`fedlearn.c:181-182`）。コメントが
    正直に「分散 FedAvg は NOT implemented」「averaged weights ever return が無い」と明記
    （`fedlearn.c:165-180`）。
- **shell 経路でこれを観測できる。** `arch/x86/shell.c:1077` が `fl_local_train` を呼び、
  `shell.c:1100` で `dtk_fl_aggregate(0, flat_delta, 6, 3000)`。**単一ノード**（drpc_my_node==0/0xFF）
  なら `[FL] aggregate OK`（`shell.c:1101-1104`）— だが適用されたのは **b3 だけ**。
  **実 cross-node**（drpc_my_node != 0）なら `E_NOSPT` → `[FL] aggregate failed`
  （`shell.c:1105-1108`）。

→ **今日、ノード間を渡って適用される weight delta は文字通り 0。** cross-node 経路は何も適用せず
E_NOSPT を返すからゼロ、単一ノード経路すら b3 しか触らないから body はゼロ。
**§8「全体が未来を強くする」の cross-node 実装 = 0 個のパラメータ**、を第6版の主張どおり再確認した。
[🔴 G22]

---

## 3. G13 — coordinator の 200ms 同期窓は依然 concurrent flow を再直列化するか

**する。G13 OPEN（🟡）、master `ed18b8a` で第6版から 1 文字も変わっていない。**

- `coordinator_aggregate`（`dkva.c:233-288`）は region partial を **同期ブロッキング窓**で集める:
  `INT win = DKVA_RSUM_WIN_MS;`（=200, `dkva.h:66`）→ `while (win > 0) { ... tk_dly_tsk(20); win -= 20; }`
  （`dkva.c:252-269`）。呼び出しは responder ループ `dkva.c:640`。
- 効果: **同一 coordinator を共有する region 横断フローは、この 200ms 窓で直列化される**
  （第2版 G13 の指摘が生きている）。

**協調学習（遅い帯域）との衝突:** §8 は「熟慮層＝深い判断・学習・設計更新は時間をかけてよい」と
定義する。G22 の gossip+merge はまさにこの **遅い帯域**である。問題は、もし G22 の集約/同期点が
DKVA と同じ「窓で集める coordinator」型を流用すると、**遅い学習トラフィックが 200ms 窓に乗って
速い反射経路（§8 第1層）と同じ直列化点を共有**しかねないこと。受け入れテスト §1.2(b)/(c) は
これを構造的に弾く（peer-symmetric・近傍のみ・集約器なし・node0 kill 後も継続）— G22 が DKVA の
coordinator 型を流用しない限り、G13 と新たに衝突する筋はない。**G13 自体は §5 の DKVA 推論軸に
残る課題**で、G22 はそれを踏まないこと、が受け入れ条件側の含意。[🟡 G13]

---

## 4. G33/G34 — 反射の温度バケツ脅威源・死んだ確信度ゲート、そして G22 後の再接地

### 4.1 反射の脅威は依然 5s タイマ解除（G33 反射軸 OPEN, 🟡）

- engage したアクションは **固定 `REFLEX_HOLD_MS = 5000`**（`reflex.h:60`）で解除される。
  `conserve_until = now_ms() + REFLEX_HOLD_MS`（`reflex.c:260`）、解除は
  `if (conserve_until && now >= conserve_until)`（`reflex.c:283-288`）。**被制御量ではなく時計で出る。**
- 熟慮 `reflex_deliberate`（`reflex.c:321-352`）が動かすのは **スカラ `learned_conserve` 1 個**
  だけ（`[REFLEX_CONSERVE_MIN=8, MAX=80]` クランプ・step 6、`reflex.h:89-92`）。
  これは観測駆動のホメオスタット（事前固定の適応則の実行）であって学習ではない（G22 の系）。

### 4.2 確信度ゲートは moe 経路で依然 死んでいる（G34 OPEN, 🔴 候補のまま）

- moe 推論完了点は **`reflex_on_inference(result_class, 0xFF, drpc_my_node)`**（`moe.c:419`）—
  confidence を **0xFF 固定**で渡す（コメント `moe.c:418` が「未知 → 0xFF (ゲートを通す)」と自認）。
- 反射側は `if (confidence != 0xFF && confidence < REFLEX_CONF_MIN) return;`（`reflex.c:208`）—
  **0xFF は素通し**。= 低確信の誤推論も反射を発火させる。第6版が「§5 で plural 倍悪化する」と
  🔴 へ格上げ推奨したまま。

### 4.3 G22 後に「熟慮→学習→反射の再接地」の道はできるか — **構造的にはできる。だが G22 単体では繋がらない。**

これが本書の核心的観察である。**§8 は二層が *結合* していることを要求する**:
「近傍が今を守り、**全体が未来を強くする**」。今日、二層は完全に分離している:

- 第1層（反射）の脅威源は **温度バケツ + 5s タイマ**（§4.1）、確信度は **0xFF 固定**（§4.2）。
- 第2層（熟慮）が動かすのは **`learned_conserve` スカラ 1 個**だけ（`reflex.c:321-352`）。
- 学習されるモデル本体（MLP の w1/w2/w3）は **どちらの層にも繋がっていない**（§2: cross-node delta=0）。

G22 が landing すれば、初めて **「経験から書き換わるモデル本体」**が running system に現れる。
その瞬間、**2 本の配線が原理上可能になる**:

1. **熟慮（学習済みモデル）→ 反射の確信度ゲート再接地**: `moe_infer` の 0xFF（`moe.c:419`）を、
   協調学習済みモデルの **実 softmax 確信度**（max-prob×100 等）に置き換えられる。これは G34 を
   殺す。死んだゲートが「学習で得た確信」で初めて生きる。
2. **反射の経験 → 熟慮（学習）への観測供給**: 反射が積む dwell 経験（`reflex.c:193-202`、
   `win_dwell_sum/win_episodes`）が、今は `learned_conserve` スカラ 1 個にしか流れない
   （`reflex.c:326-341`）。これを協調学習のシャード/勾配の **重み付け信号**に流せば、
   「近傍が今 守った経験」が「全体の未来の学習」を形作る = §8 の二層結合。

→ **G22 は前提条件を作るが、それ自体は再接地しない。** G22 の受け入れテスト（§1）は学習が
本物であることだけを検査し、二層結合は検査しない。**この結合こそ次の本丸（§6 / G38）。**
[🟡 G33 / 🔴 G34]

---

## 5. G23 — federation 依然 doc-only か。協調学習が乗ると 32 上限が「全体の総合力」を縛るか

**doc-only のまま（G23 OPEN, 🔴）。** 証拠:

- **論理ノード上限は `DNODE_MAX = 32`**（`drpc.h:35`）、テーブルも `dnode_table[DNODE_MAX]`
  （`drpc.h:103`）。federation を実コードへ配線した形跡なし
  （`arch/common/*.c` で federation 機構の grep ヒット 0、`federation.md` は doc）。
- node id は DRPC ヘッダの **8bit のうち実質 0-31** に縛られる（`drpc.h:23, 35`）。

**協調学習が乗ると何が起きるか — 上限が思想に直撃する:**

§2 は「一隻を守るために、宇宙全体に散らばった **全ネットワークの総合力**を呼ぶ」と言う。
§8 は「**全体が**未来を強くする」と言う。**G22 が本物になった瞬間、「全体」の大きさ = 学習に
参加できるノード数が、そのまま「総合力」の上限になる。** 今日それは **32**。

- 保存（protect/p-fs）の段階では、32 は「同時に守れる点の数」の上限にすぎず、思想的には
  「図書館の棚数」程度の制約だった。
- だが **学習が cross-node になると、32 は「人類全体の脳に参加できるニューロン数」の上限**になる。
  §9「人類の知の総体」を 32 ノードで近似することになり、**§2「全ネットワークの総合力」の
  *全* が、ハードコードされた 32 で頭打ち**になる。UMP フリート（Android 各インストール=1ノード、
  メモリ `project_ump_android_node`）の思想 —「every install becomes a node」— と真っ向から
  矛盾する: 33 台目の install は脳に参加できない。

→ **G22 が landing するほど、G23（32 上限）の 🔴 度は上がる。** 保存の時代は静かな制約だったが、
学習の時代には「全体の総合力」を直接縛る壁になる。**G22 の受け入れテストは N>=3 で緑にできるが、
思想（§2/§8）が要求する「全体」は 32 では足りない。** [🔴 G23]

---

## 6. 次の本丸 — G38「§8 二層結合（熟慮→反射の再接地）」を思想から推す

第6版は本丸を G22（cross-node 学習）に置いた。第16波がそれを実装中。**その *次* の本丸は、
G22 が作る「学習済みモデル本体」を **反射層へ結線して二層を閉じる**こと（G38）。** 論拠は backlog
でなく **思想（§8/§9）**から:

1. **§8 は二層の *結合* がアーキテクチャの定義そのもの。** 「近傍が今を守り、**全体が未来を
   強くする**」（survival-network.md:216）。第14波は第1層（保存=今を守る）を、第15波は plural を、
   第16波は第2層（学習=未来を強くする）を立てる。だが **二層を繋ぐ矢印が無い**: 学習済みモデルは
   反射の確信度ゲートに繋がらず（`moe.c:419` の 0xFF）、反射の経験は学習に繋がらない
   （`reflex.c:326-341` でスカラ止まり）。**二層が並ぶだけでは「未来を強くした学習が今を守る反射を
   良くする」が起きない。** §8 は「未来を強くする」を「今を守る」の *ため* と位置づけている以上、
   結合されない二層は §8 の半分でしかない。

2. **§9「考える器官」は学習が *行動に効く* ことを要求する。** 学習がモデルの数字を上げるだけで、
   その学習が **反射の判断（守るか・どこへ rally するか・どれだけ構えるか）を変えない**なら、
   それは「賢くなったが何も変わらない図書館」。G34（死んだ確信度ゲート）はちょうどこの欠落の
   症状: 学習で得られるはずの確信が `moe.c:419` で 0xFF に潰され、反射は学習を **知らない**。
   G38 = 「学習済みモデルの確信が反射ゲートを生かし（G34 を殺す）、反射の経験が学習の重み付けを
   形作る」双方向結線。**G34 の小修正は G38 の最初の一歩**として自然に吸収される。

3. **§7（中央なし）の規律が、二層結合にもそのまま再利用できる。** rally の局所勾配・per-source
   gossip（`world.c`/`moe.c:241-337`、第6版 §6 が NO-CENTRAL を確認）は既に分散。同じ substrate で、
   学習済み確信を局所信号として反射へ流せば、**結合も中央なしで閉じる**。中央 confidence サーバを
   置く誘惑（§7 が禁じる）を避けられる。

> 対案として G23（32 上限の撤廃）や G13（200ms 窓）を本丸にする手もある。だが G23 は「全体を
> 大きくする」スケールの話、G13 は「速く綺麗にする」配管の話で、**どちらも「学習が今を守る行動に
> 効く」質的転換ではない**。§9 の言葉に立ち返れば、希少なのは「保存ではなく **思考する** 器官」。
> G22 で思考（学習）が立つなら、その次は **思考が行動を変える**こと = 二層結合（G38）。
> ただし G23 は §6.5 のとおり G22 が本物になるほど 🔴 度が上がる **並走する壁**であり、
> G38 の後（または並行）で必ず晴らすべき第二の本丸。

### 新規・残存乖離 G38（§5+ 表に追記する形）

| # | 不変条件 | 乖離の内容 | 証拠 (file:line) | 重さ | 実証 |
|---|---|---|---|---|---|
| **G38** | §8, §9 | **二層（反射/熟慮）が結合していない。** 学習済みモデルの確信は反射ゲートに届かず（`moe.c:419` 0xFF 固定 → `reflex.c:208` 素通し）、反射の dwell 経験は学習でなくスカラ 1 個に流れる（`reflex.c:326-341`）。反射の脅威源は依然 5s タイマ解除（`reflex.h:60`, `reflex.c:283-288`）。= 「全体が未来を強くする」が「今を守る」に効かない。G22 が学習を立てても、この矢印が無いと §9「考える器官」は行動に届かない。 | `moe.c:418-419`(0xFF), `reflex.c:208`(素通し), `reflex.c:321-352`(熟慮=スカラ), `reflex.c:326-341`(経験→スカラのみ), `reflex.h:60`(5s タイマ) | 🔴 | 📖 |
| **G23↑** | §2, §8, §9 | **DNODE_MAX=32 が、学習が cross-node になった瞬間「全体の総合力」の上限になる。** 保存時代の静かな制約が、学習時代には「脳に参加できるニューロン数」の壁へ昇格。UMP「every install = node」と矛盾。 | `drpc.h:35`(=32), `drpc.h:103`(table), `drpc.h:23`(8bit), federation はコード未配線 | 🔴(格上げ) | 📖 |

---

## 7. 公平の節 — master NOW で本物の honesty（判決でなく地図）

- **fedlearn は嘘をつかない。** cross-node は `E_NOSPT` を返し、round counter を進めない
  （`fedlearn.c:181-182`）。コメントが「averaged weights ever return が無い・false success だった
  過去」を逐語で残す（`fedlearn.c:165-180`）。第10波の honesty が健在。
- **R3b `breathe` は「join で賢くなる」を *in-kernel で* 数で示し、その範囲も正直。** control 行
  （`spec.c:317-324`）が「同一コピー = 監査 §4 旧状態」を 64.4% で晒し、join が 1<2<4 で超える
  （`spec.c:375-376`）。そして breathe.sh が「Real distributed inference over the relay is
  intentionally NOT used here」と自認。**これは G22 の前哨であって G22 ではない、を実装側が認めている。**
- **NO-CENTRAL は rally 配管では本物。** `select_expert` は局所 world-table と自己観測のみ
  （`moe.c:241-337`）。G22 の受け入れ §1.2(b) はこの規律を学習にも要求する。

---

## 8. 評決（G22 後、system は「考える器官」か、まだコピーする図書館か・正直に）

> **正直に言えば、G22 が landing しても、それ単体では system は「考える器官」にならない。
> せいぜい「学んだ重みを混ぜられる図書館」になるだけだ。** §9 が言う「考える器官」は、
> 学習が *行動を変える* ことを含意する。だが今日、学習されるモデル本体は反射層に **一本も
> 配線されていない** — 反射の脅威は温度バケツ + 5s タイマで出（`reflex.h:60`, `reflex.c:283-288`）、
> 確信度は学習を知らずに 0xFF へ潰され（`moe.c:419`）、熟慮はスカラ 1 個を nudge するだけ
> （`reflex.c:321-352`）。G22 が「全体が未来を強くする」を本物にしても、その「未来」が
> 「今を守る」反射に届く矢印が無い限り、§8 の二層は **並んでいるだけで結合していない**。
>
> しかも master NOW の cross-node weight delta は **文字通り 0**（`fedlearn.c` cross-node=`E_NOSPT`、
> 単一ノードすら b3 6 個のみ `fedlearn.c:91-119`）。第6版が「並行する図書館」と評した状態から、
> コードは 1 行も進んでいない。第16波が G22 を landing させる *まで*、system は「学習する器官」
> ですらない。
>
> そして G22 が landing しても、(i) DNODE_MAX=32 が「全体の総合力」を 32 ニューロンで頭打ちにし
> （`drpc.h:35`、§6.5）、(ii) 二層が結合せず学習が行動に効かない（G38）。**最も深い正直: 第16波が
> §1 の 5 トークンを全部緑にしても、それは「N ノードが互いに素なシャードで学んで全員が上限を
> 超えた」を証明するだけで、「その学習が、今 一点を守る反射を賢くした」は証明しない。** 後者こそ
> §8/§9 が要求する「考える器官」の本旨だ。
>
> —— 迷ったら survival-network.md §8（近傍が今を守り **全体が未来を強くする** = 二層は手段と目的の
> 関係）と §9（保存ではなく **思考する** = 思考が行動に効く）へ戻る。注ぐ向きは正しい。G22 で
> 「全体で学ぶ」が初めて立つ。だが「学んで、今を守る」— 二層が結合して初めて器官になる — は、
> G22 の *次*（G38）に、まだ self-test の外で待っている。**第16波の G22 を、§1 の 4 手口を全部
> 弾く live テストで landing せよ。そして祝わずに、次は二層を結べ。**

---

## 12. G38 — 二層結合（wave 17）: THINKING CHANGES GUARDING — RESOLVED（🟢）

第11版が「次の本丸」と名指した G38（二層結合）を第17波で着地させた。§4.3 が
「構造的にはできる。だが G22 単体では繋がらない」と予言した **2 本の配線**を実装し、
「学習が、今 一点を守る反射を賢くした」を **数で** 証明した。

### 12.1 主アロー（学習 → 守る）— G34 は死んだ

- `moe.c` の死んだ `reflex_on_inference(result_class, 0xFF, …)`（旧 `moe.c:419`）を、
  **協調学習される 635-param Transformer（dtr; G22 が全網平均する本体）の実出力**へ置換:
  同じ入力を `dtr_forward_probs` に通し、`argmax` を脅威クラス、`max-softmax × 100` を
  **実在の確信度**として反射へ渡す。確信度は 0xFF 固定ではなく入力ごとに変わる
  （held-out で [49%..97%] のように動く）。
- 反射ゲートは `reflex_would_fire`（`reflex.c`）に一元化（`deadband_pick` と同じ思想 —
  self-test が本番と同じゲートを叩く）。低確信（未学習/曖昧）は発火せず、高確信の脅威
  クラスだけが決然と発火する。**`[g38-confidence-live]`**: 同一の critical 入力が
  UNLEARNED では `cls=0 conf=50% fire=no`、LEARNED では `cls=2 conf=97% fire=YES` —
  学習がこの一点の守りを「素通り」から「確信を持った反射発火」へ反転させた。

### 12.2 第二アロー（守る → 学習）

- 反射が「危険」と判断して発火したクラスごとの経験（`guard_class_exp[]`、
  `reflex_threat_experience()`）を、協調学習が **優先度**として読む（遅い熟慮帯域で
  オーバーサンプリング、`gl_run_gossip_weighted`）。**`[g38-guard-feeds-learning]`** が
  「守った経験 → 学習優先度」が配線され機能していることを構造的に検査する。

### 12.3 数（学んだら守りが良くなったか）— 正直な値

- in-process **`[g38-learning-improves-guarding]`**: 同じ reflex ゲートを学習モデルで
  駆動した held-out 守りスコア = **UNLEARNED 33.3% → LEARNED 93.3%**
  （threat-detect **0% → 95%**）。改善は学習のみに由来する（確信度は実 softmax、
  ゲートは同一）。
- LIVE（`samples/34_twolayer/run.sh`、relay 経由 3 ノード・互いに素シャード・x86_64）:
  各ノードの LEARNED 守りスコアが自分の UNLEARNED ベースラインを超え、**ノードを
  kill -9 で殺しても残りが守り続ける**。CI: `twolayer-couple-live`。

### 12.4 残課題（正直）

- **脅威軸の温度バケツは残る**: `gate_predict`（温度バケツ）と `REFLEX_HOLD_MS=5s` の
  ヒステリシスは健在。G38 は確信度ゲートと「脅威クラスの判断」を学習モデルへ接地した
  が、CONSERVE のヒステリシス解除そのものは依然タイマ。学習信号が **発火するか/何を
  守るか** を支配する一方、**どれだけ保持するか** はまだ時計（G33 の反射軸は部分的に
  残る）。
- **第二アローの定量効果は限定的**: 守り経験の重み付け学習は配線され「守りを上げる」
  ことは示せるが、長い協調学習が ~100% に飽和すると plain と weighted の critical-detect
  差は消える（短スケジュールでのみ headroom）。`[g38-guard-feeds-learning]` は **アローの
  存在と機能**を構造的に保証し、定量差は正直に「飽和域では marginal」と報告する。
- **DNODE_MAX=32**（G23）は不変 — 「全体」の大きさの上限は別件として残る。

→ **§8 の二層は、もはや「並んでいるだけ」ではない。** 学習が守りのゲートを開閉し
（主アロー）、守った経験が学習を方向づける（第二アロー）。`[g38-confidence-live]` /
`[g38-learning-improves-guarding]` / `[g38-guard-feeds-learning]` が CI で機械的に守る。
[🟢 G38] / [🟡 G33 反射軸（保持時間のタイマ）部分的に残存]
