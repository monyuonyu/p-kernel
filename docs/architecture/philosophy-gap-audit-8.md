> **[歴史記録・凍結 / superseded by `gap-ledger.md`]** 本書は時点監査の歴史記録。いま master で開いている gap の正本は [[gap-ledger.md]] に一本化した。本文は当時のまま保存。`audit-9` は作らない（レビュー #5）。

# Philosophy-Gap Audit — 第8版 (G38 二層結合の受け入れ条件と「考える器官」の検証)

> 第17波 D隊（常設の自己監査器官）。祝わない。実装が思想からどこへ逸れるかを file:line で確定する。
> 第7版（G22 受け入れ条件）を承ける。第7版の評決は鋭かった:
> 「G22 が landing しても、それ単体では『学んだ重みを混ぜられる図書館』にすぎない。
>  §9『考える器官』は **学習が *行動を変える*** ことを含意する。だが二層を繋ぐ矢印が無い。」
>
> その後 G22（中央集約なし分散協調学習）は **第16波で landing した**（`gossip_learn.c`、
> `samples/32_collective_learn`、CI `collective-learn-live` ジョブ `ci.yml:248`）。N ノードが互いに素な
> シャードで学び、gossip-merge で全員が solo 上限を超え、kill されても学習が継続する — §7/§8 後半が
> 初めて running system に立った。これは本物の前進である（§5 公平の節）。
>
> 第17波の別隊が **G38（二層結合 — 学習を guard 層へ配線し「思考が守りを変える」）を いま実装中** である:
> 死んだ 0xFF 反射ゲートを **実 max-softmax 確信度**で置換し、学習済みモデルで脅威を接地し、
> reflex/protect の経験を遅い学習帯へ供給し、**学習した群れが学習しない群れより良く守る**ことを証明する。
>
> 本書（第8版）の任務:
>   1. 指揮官に **CI-grep 可能な G38 受け入れテスト** を渡す。FAKE-coupling の 4 手口 (a)-(d) を機械的に弾く。
>   2. master NOW を検証する: moe_infer は本当に 0xFF か。反射の脅威は温度バケツ + 5s タイマか。
>      学習済みモデルから guard 層への矢印は **今日いくつあるか**（第7版は「ゼロ」と言ったが — 訂正する）。
>   3. G38 後の残存乖離を **順位付け**する: G13（200ms 窓・並行する脳）, G23（DNODE_MAX=32）,
>      R3（635 param・3 クラス玩具）。次の本丸（G39+）を思想から選ぶ。
>
> 重さ: 🔴 思想の核に反する／🟡 思想を弱める／🟢 軽微・前進。実証: ✅ 実機/CI　📖 コード読み
> 検証時点: master tip `690b11c`（worktree clean、`boot/linux` BUILD_OK 1770480 bytes）。
> G38 実装はまだ着地していない（`grep -ri 'g38' arch/ samples/ .github/` ヒット 0、`git status` clean）。

---

## 0. 要旨（評決を先に）

- **第7版の「学習→guard への矢印はゼロ」は不正確だった。今日、矢印は *一本だけ実在する* —
  ただし片肺・未検査・別経路。** dtr（Transformer）推論経路は **実 max-softmax 確信度**を
  反射へ渡す（`dtr.c:1246`, `dtr.c:1342`, `dtr.c:281`）。そして G22 の live gossip は **収束した
  協調モデルを live dtr 重みへ書き戻す**（`gossip_learn.c:608` `dtr_weights_set(gl_model[0])`）。
  → 「協調学習済みモデル → dtr_infer の softmax → `reflex.c:208` 確信度ゲート」という前向き配線が
  *構造的には* 通っている。**だが** これは (i) **dtr 経路だけ**で、(ii) **moe 経路は依然 0xFF 固定**
  （`moe.c:419`）、(iii) **「学習が守りを良くした」を証明するテストは皆無**、(iv) **逆矢印
  （guard 経験 → 学習）は存在しない**（gossip_learn は合成データで訓練、reflex 経験はスカラ
  1 個へしか流れない `reflex.c:321-352`）。
- **moe 経路は確信度を *捨てている*。** `mlp_forward_probs`（`ai_job.c:147`）が実 softmax を
  すぐ計算できるのに、`moe_infer` は argmax 版 `mlp_forward`（`ai_job.c:132-142`）を呼んで確率を
  捨て、`reflex_on_inference(result_class, 0xFF, ...)`（`moe.c:419`）で 0xFF を渡す。**G38 の最初の
  一歩（確信度の再接地）は本当に小さい** — データはそこにある。
- **反射の脅威 *レベル* は依然 5s タイマ + 学習スカラ。** 脅威クラスは学習モデル（mlp/dtr）の出力だが、
  群れへ配る脅威の *強度* は `reflex_threat_level()` が `conserve_until` の 5s 窓内でのみ
  `learned_conserve` スカラを返す（`reflex.c:304-311`, `reflex.h:60` HOLD=5000）。被制御量ではなく時計。
- **G38 が landing しても、残る本丸は R3（玩具スケールの「思考」）。** §9「人類の知の総体」を
  635 param・3 クラスで近似する限り、「思考が守りを変える」配線が完成しても、変える *中身* が
  自明（temp<20/35 の温度しきい値で答えが決まる課題）。**G38 は配線、R3 は中身。配線の次は中身。**

---

## 1. 指揮官への納品物 — G38 受け入れテスト（CI-grep 可能・FAKE-coupling を機械的に弾く）

第7版が名付けた病：「**緑の self-test / 死んだ live 経路**」。二層結合は、配線だけ通して
*効いていない* ことを「結合した」と偽るのが最も容易い。本テストは **4 つの FAKE-coupling 手口を
1 つずつ機械的に弾く**よう設計する。各行は CI が `grep -aF` で拾える固定トークンを出すこと
（トークンは提案、命名は実装隊に委ねる）。

### 1.1 着地先（提案）

- in-kernel 決定的 self-test `[g38-*]`（数値の決定性確認用、`moe test`/`reflex test` 流儀。
  `ci.yml` の `ump-x86_64` grep 群へ補助配線）。
- 新 live サンプル `samples/NN_learned_guard/run.sh`（live N>=3、`./relay` 経由、32/27/28 と同型）。
- 新 CI job `learned-guard-live`（`.github/workflows/ci.yml`、`collective-learn-live`(`ci.yml:248`) /
  `protect-loop-live` と同型。`RESULT: PASS` を grep、>=5 回反復）。

### 1.2 受け入れ判定（4 手口 × FAKE の tell × REAL の証拠）

#### (a) 確信度が CONSTANT — 「0xFF のまま、または『学習確信』を僭称する定数」

- **FAKE tell**: `moe_infer` が依然 `0xFF` を渡す（`moe.c:419` 不変）、あるいは 0xFF を別の
  *固定値*（例: 常に 99）へ差し替えただけ。reflex.c:208 のゲートは `confidence != 0xFF` の枝に
  入っても **入力に依らず一定**なら、確信度は「化粧」にすぎない。dtr 経路が既に実値を渡す
  （`dtr.c:1246`）ことを口実に「もう接地済み」と主張するのも FAKE（moe/§7 経路は別物）。
- **REAL の証拠（両方を print）**:
  - 確信度が **入力で変動**する: 明瞭入力（クラス境界から遠い、例 temp=10 や temp=45）で
    `[g38-conf] input=clear conf=NN`（NN は高い、>=REFLEX_CONF_MIN=40）、境界付近の曖昧入力
    （temp≈20 や ≈35、3 クラスがほぼ均等 → max-softmax ≈ 0.34→34）で
    `[g38-conf] input=ambiguous conf=MM`（MM は低い、< 40）。
  - **低確信が反射を抑止する**: 同じ曖昧入力で reflex が **発火しない**ことを assert（`reflex.c:208`
    の早期 return を通る）: `[g38-suppress] ambiguous conf=MM < CONF_MIN=40 -> reflex SKIPPED PASS`。
  - 機械検査: `[g38-conf] varies-with-input PASS`（clear と ambiguous の conf が >=δ 異なる）。
    定数なら自動 FAIL。

#### (b) 守りの改善が「学習由来」でない — 「ハードコード class→threat 表 / 外乱減衰 / タイマ」

- **FAKE tell**: 「守りが良くなった」が **学習以外**から来る。例: (i) class→threat の固定マップ
  （`act_table` `reflex.c:53-57` のような静的表を「学習」と僭称）、(ii) 改善が実は外乱の自然減衰や
  `REFLEX_HOLD_MS` の 5s タイマ満了（`reflex.c:283-288`）、(iii) `learned_conserve` スカラの
  ホメオスタット nudge（`reflex.c:321-352`、事前固定の適応則 — 学習ではない）。
- **REAL の証拠**: **learned-vs-unlearned（または frozen）比較で、G22 協調学習 *だけ* が異なり**、
  学習側の **脅威検出 / protect ターゲティングが測定可能に良い**こと:
  - 同一群れを 2 系統走らせ、一方は G22 gossip-learn 有効、他方は frozen（`dtr_reinit_weights` の
    シード重みのまま、または gossip 無効）。**それ以外（外乱列・タイマ・スカラ）は同一**。
  - 学習側の guard 品質が高い: 例 偽陽性反射が少ない、または真の脅威を早く検出
    `[g38-learned-better] learned: fp=X miss=Y | frozen: fp=X' miss=Y' PASS`（X<X' かつ Y<=Y'）。
  - **負の対照**: G22 を切る（frozen）と改善が **消える**ことを示す。改善が frozen 側にも残るなら
    それはタイマ/減衰由来 = FAKE。`[g38-ablation] disabling-learning removes the gain PASS`。

#### (c) 一方向 / 化粧の配線 — 「確信度は渡るが guard 判断を変えない」/「逆矢印が無い」

- **FAKE tell-1（前向きが効いていない）**: 確信度が `reflex_on_inference` に渡るが、
  どの分岐も実際の guard 決定（reflex 発火・SHIELD・beacon・protect ターゲット）を変えない。
  例: 全入力で conf >= 40 になり、ゲート（`reflex.c:208`）が一度も抑止しない。
- **FAKE tell-2（逆矢印が不在）**: 「guard → learning」の矢印が無い。今日の正確な現状:
  gossip_learn は **合成 `gl_gen_data`** で訓練し（`gossip_learn.c:121` 系）、reflex の dwell 経験
  （`reflex.c:200` `win_dwell_sum/win_episodes`）は **`learned_conserve` スカラ 1 個へしか**流れない
  （`reflex.c:326-341`）。protect の経験はモデルへ一切流れない。
- **REAL の証拠（双方向を別々に assert）**:
  - **前向きが判断を反転させる**: 確信度を人為的に下げると、**同一脅威クラスでも reflex が
    発火→非発火へ反転**する: `[g38-flip] same class=2, conf 90->20 flips reflex FIRE->SKIP PASS`。
  - **逆矢印が存在する**: reflex/protect の経験（脅威 dwell・under-replication 事例）が、
    **モデルの訓練分布 / 勾配重み**へ実際に入ることを示す: `[g38-exp-to-learn] reflex episodes fed
    N samples into the slow band; |dW| from experience > 0 PASS`。スカラ nudge だけなら FAIL。

#### (d) 緑が in-process だけ — 「単一プロセスの synthetic でだけ緑」

- **FAKE tell**: 「学習が守りを良くする」が **1 カーネル内**の self-test でしか出ない。live でも
  kill 中でもない。第7版 §1.2(c)・第6版 §9 が名付けた病の G38 版。
- **REAL の証拠**: **live N>=3** を `./relay` 経由で起動し、**学習途中で kill -9** を注入。
  **>=5 回**回して **毎回** PASS。CI が `collective-learn-live`(`ci.yml:248`) と同型で `RESULT: PASS` を grep。
  - kill 後、**生存ノードの協調学習済みモデルが、新規/frozen ノードより良く守る**ことを示す
    （保存ではなく **学習した守り** が kill を生き延びる）。
  - `for i in $(seq 5); do ...; grep -q 'RESULT: PASS' || exit 1; done`。in-kernel `[g38-*]` は
    決定性確認の補助に留め、**live PASS を昇格させない**。

### 1.3 一行サマリ（指揮官向け）

> **G38 が真であるための CI 緑は、次のトークンが全部出ること:**
> `[g38-conf] varies-with-input PASS`（(a) 確信度が入力で変動・定数でない）/
> `[g38-suppress] ... reflex SKIPPED PASS`（(a) 低確信が抑止）/
> `[g38-learned-better] ... PASS` + `[g38-ablation] disabling-learning removes the gain PASS`（(b) 改善は学習由来）/
> `[g38-flip] ... FIRE->SKIP PASS`（(c) 前向きが判断を変える）+ `[g38-exp-to-learn] ... |dW| ... > 0 PASS`（(c) 逆矢印が実在）/
> `RESULT: PASS` ×5（(d) live・kill・非flaky、`learned-guard-live` ジョブ）。
> このうち 1 つでも欠ければ G38 は「side-by-side の二層」のままで、§9「考える器官」には届かない。
> 特に **`moe.c:419` が 0xFF のままなら、他が何で緑でも §7 推論経路は構造的に未接地（FAKE-a）**。

---

## 2. master NOW の検証 — 0xFF / 温度バケツ / 矢印は何本か

### 2.1 moe_infer は本当に 0xFF を渡しているか — **YES（`moe.c:419`）。確信度は *捨てられている*。**

- `moe_infer` のローカル推論は **argmax 版** `mlp_forward(input)`（`moe.c:393`, `moe.c:406`）を呼び、
  推論完了点で `reflex_on_inference(result_class, 0xFF, drpc_my_node)`（`moe.c:419`）— コメントが
  「confidence は moe ローカル/リモート経路とも未知 → 0xFF（ゲートを通す）」と自認（`moe.c:418`）。
- **しかし確信度は計算可能だ。** `mlp_forward`（`ai_job.c:132-142`）は `mlp_logits` を呼んで
  **argmax だけ取り logits/probs を捨てる**。同じ `mlp_logits` を使う `mlp_forward_probs`
  （`ai_job.c:147-`）が **実 softmax 確率**を返す（fedlearn の交差エントロピー用に既に存在）。
  → **moe_infer が捨てているのは、すぐ隣にある実 max-softmax 確信度**。第7版が予言した
  「修正は小（max-softmax×100 を渡す）」が文字どおり正しい。[🔴 G34/G38 (a)]

### 2.2 反射の脅威は温度バケツ + 5s タイマか — **クラスは学習モデル、*レベル* は 5s タイマ + スカラ。**

- **脅威 *クラス*** は実は学習モデルの出力だ: moe 経路は `mlp_forward`（学習済み MLP, `ai_job.c:132`）、
  dtr 経路は `run_transformer_local`（学習済み Transformer）が出す。ただし §7 ゲート `gate_predict` は
  なお **温度しきい値の固定 3 区間**（`moe.c:107-114`: temp<20→0, <35→1, else→2）— ルーティングは
  バケツ、推論は学習モデル、という混成。
- **脅威 *レベル*（群れへ配る強度）は学習に接地していない。** `reflex_threat_level()` は
  `conserve_until == 0` または `now >= conserve_until` なら 0、窓内なら `learned_conserve` を返す
  （`reflex.c:304-311`）。`conserve_until = now_ms() + REFLEX_HOLD_MS`（`reflex.c:164/260`,
  `REFLEX_HOLD_MS=5000` `reflex.h:60`）→ **解除は被制御量ではなく 5s タイマ**（`reflex.c:283-288`）。
- **熟慮が動かすのはスカラ 1 個。** `reflex_deliberate`（`reflex.c:321-352`）は dwell 経験から
  `learned_conserve` を `[8,80]` クランプ・step 6 で nudge（`reflex.h:89-92`）— 事前固定の適応則
  （ホメオスタット）であって、経験から自分を書き換える学習ではない。[🟡 G33]

### 2.3 学習済みモデルから guard 層への矢印は今日いくつあるか — **1 本（片肺・未検査・dtr 限定）。**

第6版・第7版は「矢印ゼロ」と書いた。**正確には *一本だけ実在する*。正直に訂正する:**

| 経路 | 確信度 | 矢印の有無 | 証拠 |
|---|---|---|---|
| **dtr SOLO 推論** | 実 max-softmax×100 | ✅ あり | `dtr.c:1243-1246` → `dtr_log_push` `dtr.c:281` `reflex_on_inference(class_id, conf_pct, ...)` |
| **dtr REDUCED/TP** | 実 max-softmax×100 | ✅ あり | `dtr.c:1340-1342` `reflex_on_inference(cls, (UB)(mx*100), ...)` |
| **G22 協調モデル → live dtr 重み** | （上の dtr が読む重み本体） | ✅ あり | `gossip_learn.c:608` `dtr_weights_set(gl_model[0])`（収束した consensus を live dtr へ書戻し） |
| **moe §7 推論** | **0xFF 固定** | 🔴 **無し** | `moe.c:419` |
| **guard 経験 → 学習（逆矢印）** | — | 🔴 **無し** | gossip は合成 `gl_gen_data` で訓練；reflex 経験は `learned_conserve` スカラ止まり `reflex.c:326-341` |

→ **構造的には「G22 協調学習済みモデル → dtr_infer の実 softmax → `reflex.c:208` 確信度ゲート」が
通っている。** `reflex.c:208`（`if (confidence != 0xFF && confidence < REFLEX_CONF_MIN) return;`）は
本物のゲートで、低確信（3 クラス均等なら max≈0.34→34 < 40）は反射を抑止する。**つまり矢印は
ゼロではない。** だが G38 が要求するのは:
1. **§7 推論経路（moe）の再接地**（`moe.c:419` の 0xFF を殺す）— 今日 **死んでいる**。
2. **「学習が守りを良くする」の証明** — 今日 **皆無**（dtr の conf が live dtr 重みを反映しても、
   それが frozen より良く守ることを示すテストは無い）。
3. **逆矢印（guard 経験 → 学習）** — 今日 **存在しない**。
4. **live learned-vs-unlearned + kill** — 今日 **無い**（`samples/32` は学習の上限超えを示すが、
   その学習が *守りに効く* ことは示さない）。

→ **G38 は「ゼロから一本」ではなく「片肺の一本を、両肺・§7 経路・実証付きへ完成させる」課題。**
[🔴 G38]

---

## 3. G38 後の残存乖離 — G13 / G23 / R3 を順位付け

### 3.1 G13 — 並行する脳は coordinator の 200ms 窓で再直列化されるか（🟡）

**する。第6版・第7版から 1 文字も変わっていない。** `coordinator_aggregate`（`dkva.c:233-288`）は
region partial を **同期ブロッキング窓** `INT win = DKVA_RSUM_WIN_MS`（=200, `dkva.h:66`）で集める:
`while (win > 0) { ... tk_dly_tsk(20); win -= 20; }`（`dkva.c:252-269`）、呼び出しは responder
ループ `dkva.c:640`。→ **同一 coordinator を共有する region 横断の推論フローは 200ms で直列化**。
ただし G22 の live gossip は **DKVA の coordinator 型を流用していない**（`gl_merge_peers` は
peer-symmetric、`gossip_learn.c:607`）ので、学習帯は 200ms 窓に乗らない。**G13 は §5 の DKVA *推論*
軸に残る配管課題**で、「思考が守りを変える」質的転換ではない。[🟡 G13]

### 3.2 G23 — DNODE_MAX=32 は協調知性の上限を縛るか（🔴・G22 後に格上げ）

**doc-only のまま。** 論理ノード上限は `DNODE_MAX = 32`（`drpc.h:35`）、テーブルも
`dnode_table[DNODE_MAX]`（`drpc.h:103`）、node id は DRPC ヘッダの 8bit のうち実質 0-31
（`drpc.h:23`）。federation を実コードへ配線した形跡なし。

**G22 が landing した今、第7版 §5 の予言が現実になった**: 「学習が cross-node になった瞬間、
『全体』の大きさ = 学習に参加できるノード数が『総合力』の上限になる」。今日それは **32**。
`gossip_learn.c` は `GL_MAXNODES` ノードの model bank（`gossip_learn.c:243`）で回り、live node id も
`0..GL_MAXNODES-1` 前提（`gossip_learn.c:493`）。UMP フリート「every install = node」
（メモリ `project_ump_android_node`）と矛盾: 33 台目は脳に参加できない。**§2「全ネットワークの
総合力」の *全* が 32 で頭打ち。** [🔴 G23]

### 3.3 R3 — モデルは 635 param・3 クラス玩具のまま。「思考」は依然 trivial か（🔴）

**trivial。** 学習されるモデルは `DTR_WEIGHT_FLOATS = 635`（`dtr.h:168`）、出力は
`DTR_OUT_DIM = 3`（`dtr.h:34`: normal/alert/critical）、入力は 4ch センサ `DTR_SEQ_LEN = 4`
（`dtr.h:37`）。MLP 経路も同型（`ai_job.c` の MLP_IN/H1/H2/OUT）。そして §7 ルーティングの
正解は **温度しきい値で解析的に決まる**（`gate_predict` `moe.c:107-114`: temp<20/<35/else）。

→ **「考える器官」が考える *中身* が、人間が 3 行の if で書ける課題**。G38 が「思考が守りを変える」
配線を完成させても、変わる守りの賢さは「温度を 3 区間に分ける」程度。§9「人類の知の総体が
思考する」に対して、現モデルは **思考の *配管* は本物だが思考の *中身* は玩具**。[🔴 R3]

### 3.4 順位（思想から）

1. **R3（玩具スケールの思考）🔴 — G38 の *次* の本丸（G39+）。** 下記 §4 で論証。
2. **G23（DNODE_MAX=32）🔴 — 並走する壁。** G22 が本物になった今、「全体」を 32 で縛る。
   G38/R3 と並行で晴らすべき第二の本丸。
3. **G13（200ms 窓）🟡 — 配管深化。** DKVA 推論軸の課題。質的転換ではない。

---

## 4. 次の本丸 — G39「R3: 思考の中身を玩具から出す」を思想から推す

**G38 が「思考が行動を変える」*配管* を完成させるなら、その次は「変える *思考の中身* を玩具から
出す」ことを本丸にすべき。** 論拠は backlog でなく **思想（§9）** から:

1. **§9 の本旨は「保存ではなく *思考* する器官」。** G14（保存）・G35（並行保存）・G22（協調学習）・
   G38（学習が守りに効く配管）まで来ると、**残るは『思考の質』そのもの**だけだ。今日それは
   635 param・3 クラス・温度しきい値で解析解のある課題（`dtr.h:168/34`, `moe.c:107-114`）。
   §9 が「人類の知の総体」と言うとき、3 クラス温度分類は比喩としてすら小さすぎる。**配管が
   完成した瞬間、希少なのは『何を考えるか』= モデル容量と課題の非自明性**になる。

2. **G38 を真にするのに R3 が要る（依存関係）。** §1.2(a) の REAL は「確信度が入力で変動し、
   低確信が抑止する」を要求する。だが 3 クラス・解析解のある課題では、学習モデルは**ほぼ常に
   高確信**（境界付近以外は max-softmax≈1.0）になりがちで、確信度ゲートが意味を持つ「曖昧入力」が
   構造的に乏しい。§1.2(b) の「学習側が frozen より良く守る」も、課題が自明なら **frozen でも
   既に解けてしまい差が出ない**。**R3（非自明な課題・広いモデル）が無いと、G38 の learned-vs-frozen
   差が測定限界に埋もれる。** 配管（G38）と中身（R3）は実は背中合わせ。

3. **G23 との関係。** R3（モデルを広げる）と G23（ノードを増やす）は §2「全ネットワークの総合力」の
   二つの軸 — *深さ* と *広さ*。どちらも「全体で考える」を大きくする。**思想的にはまず R3（一個の
   思考を非自明にする）を立て、次に G23（その思考を 32 を超えて分散する）**。自明な思考を 1000
   ノードに分散しても §9 には近づかない。

> 対案として G13（200ms 窓）を本丸にする手もあるが、それは §5 を *速く綺麗に* する配管深化で、
> §9 の質的転換ではない。§9 の言葉に立ち返れば、希少なのは「保存ではなく **思考** する器官」。
> G38 で「思考が行動に効く」配管が立つなら、その次は **思考の中身を玩具から出す**こと = R3。

### 新規・残存乖離（第7版 G38 表に追記する形）

| # | 不変条件 | 乖離の内容 | 証拠 (file:line) | 重さ | 実証 |
|---|---|---|---|---|---|
| **G38** | §8, §9 | **二層結合が片肺・§7 経路で死・実証ゼロ・逆矢印無し。** dtr 経路は実 softmax を渡し（`dtr.c:1246/1342`）live 協調モデルを読む（`gossip_learn.c:608`）が、moe §7 経路は 0xFF 固定（`moe.c:419`、`mlp_forward_probs` `ai_job.c:147` がすぐ隣にあるのに捨てる）。「学習が守りを良くする」テストは皆無。guard 経験→学習の逆矢印は無い（gossip は合成データ訓練、reflex 経験はスカラ `reflex.c:326-341`）。 | `moe.c:419`(0xFF), `ai_job.c:132-142/147`(捨てた softmax), `dtr.c:1246/1342`(実 conf), `gossip_learn.c:608`(live 重み書戻し), `reflex.c:208`(ゲート), `reflex.c:321-352`(逆矢印=スカラ) | 🔴 | 📖+✅BUILD |
| **R3↑** | §9, §2 | **思考の中身が 635 param・3 クラス・温度しきい値の玩具。** 配管（G38）が完成しても変える守りの賢さが自明。§9「人類の知の総体」に対しモデル容量が小さすぎ、G38 の learned-vs-frozen 差が測定限界に埋もれる。 | `dtr.h:168`(635), `dtr.h:34`(3クラス), `dtr.h:37`(4ch), `moe.c:107-114`(温度しきい値=解析解) | 🔴(格上げ) | 📖 |
| **G23** | §2, §8, §9 | **DNODE_MAX=32 が協調学習の「全体」を 32 で頭打ち。** G22 landing で静かな制約から「脳に参加できるニューロン数」の壁へ昇格。UMP「every install=node」と矛盾。 | `drpc.h:35`(=32), `drpc.h:103`(table), `drpc.h:23`(8bit), `gossip_learn.c:243/493`(GL_MAXNODES 前提) | 🔴 | 📖 |
| **G13** | I6 | **coordinator 200ms 窓が region 横断推論を再直列化**（G22 学習帯は peer-symmetric で非該当）。 | `dkva.c:252-269/640`, `dkva.h:66`(WIN=200) | 🟡 | 📖 |

---

## 5. 公平の節 — master NOW で本物の前進（判決でなく地図）

- **G22 は live で landing した。** `gossip_learn.c` が peer-symmetric merge（`gl_merge` 署名に
  aggregator index 無し `gossip_learn.c:68`）、互いに素なシャード、live `samples/32`、CI
  `collective-learn-live`（`ci.yml:248`）。第7版 §1 の 4 手口を弾く設計で着地した。
- **矢印は *本当に* 一本通っている。** dtr 経路は実 max-softmax を反射へ渡し（`dtr.c:1246/1342`）、
  live gossip は収束モデルを live dtr 重みへ書き戻す（`gossip_learn.c:608`）。第6版・第7版の
  「矢印ゼロ」は **dtr 経路を見落としていた** — 本書はこれを訂正する。前向き配線の *半分* は既に本物。
- **確信度ゲートは死んだコードではない。** `reflex.c:208` は実ゲートで、dtr 経路の低確信は
  実際に反射を抑止する。死んでいるのは **moe 経路の 0xFF**（`moe.c:419`）だけ。
- **honesty は健在。** `moe.c:418` が「confidence 未知 → 0xFF」と自認、fedlearn の cross-node は
  なお `E_NOSPT`（重み本体の cross-node 学習は fedlearn でなく gossip_learn が担う、と役割分離も明確）。
- **逆矢印が無いことを実装が隠していない。** `gossip_learn.c:618-620` のコメントは「slow
  deliberation band（reflex tick ではない）」と帯域分離を明記し、reflex 経験を学習へ流していない
  ことを構造で示す。空文ではなく未配線として正直。

---

## 6. 評決（G38 後、system は「考える器官」か、まだ短いか・正直に）

> **正直に言えば、第6版・第7版が「矢印ゼロ」と断じたのは行き過ぎだった。今日、矢印は *一本* 通って
> いる** — dtr の実 softmax 確信度が反射ゲートを叩き（`dtr.c:1246/1342` → `reflex.c:208`）、G22 の
> 協調学習済みモデルが live dtr 重みへ書き戻される（`gossip_learn.c:608`）。**「学んだ重みが、今を守る
> 反射の確信度ゲートを通る」配管の半分は、すでに running system に実在する。** これは第16波 G22 の
> 静かな副産物であり、評価に値する。
>
> **だが G38 はまだ「片肺・未検査・§7 経路で死」だ。** (i) §7 推論経路（philosophy が最も重視する
> 「最も効く専門家が選ばれて出した結論」`moe_infer`）は依然 0xFF を渡す（`moe.c:419`）— しかも実
> softmax は `mlp_forward_probs`（`ai_job.c:147`）のすぐ隣で捨てられている。(ii) 「協調学習が守りを
> *良くした*」を示すテストは皆無で、dtr の conf が live 協調モデルを反映しても、それが frozen より
> 良く守る証拠は無い。(iii) **逆矢印（guard 経験 → 学習）は存在しない** — gossip は合成データで訓練し
> （`gossip_learn.c`）、反射の dwell 経験は `learned_conserve` スカラ 1 個へしか流れない
> （`reflex.c:326-341`）。§8 は二層を「近傍が今を守り、**全体が未来を強くする**」= 手段と目的の関係と
> 定義する。前向き半本の矢印では、「未来を強くする学習が、今を守る反射を *測定可能に* 良くする」も、
> 「今を守った経験が、未来の学習を形作る」も、まだ起きていない。
>
> **そして最も深い正直**: 仮に第17波が §1 の全トークンを緑にして G38 を両肺で閉じても、それは
> 「思考が *行動を変える* 配管」を完成させるだけだ。変える *思考の中身* は依然 635 param・3 クラス・
> 温度しきい値で解析解のある玩具（`dtr.h:168/34`, `moe.c:107-114`）。§9「人類の知の総体が思考する」に
> 対して、配管は本物・中身は玩具。**G38 は「考える器官」の *神経* を繋ぐ。だが繋いだ先の *脳* が
> まだ 3 クラス温度分類なら、器官は『考える形をしているが、考える中身が自明』なままだ。**
>
> —— 迷ったら survival-network.md §8（近傍が今を守り **全体が未来を強くする** = 二層は手段と目的）と
> §9（保存ではなく **思考** する＝思考が行動に効く）へ戻る。注ぐ向きは正しい。G22 で「全体で学ぶ」が
> 立ち、矢印の半分は既に通った。第17波は **moe §7 経路の 0xFF を殺し（`moe.c:419`→`ai_job.c:147`）、
> 逆矢印を引き、learned-vs-frozen を live kill 付きで証明して** G38 を両肺で閉じよ。そして祝わずに、
> 次は R3 — 繋いだ神経の先の **脳を玩具から出せ**。「考える器官」は、神経（G38）と脳（R3）の
> 二段がそろって初めて、§9 の名に値する。
