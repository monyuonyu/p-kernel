# 思想⇄実装 乖離監査・第2版 — philosophy-gap-audit-2

> 常設の批判器官、第2版。第1版 (`philosophy-gap-audit.md`) で G1〜G11 を地図化し、
> 第10波で G1/G2/G3/G4/G5/G7/G8/G9 が FIXED された。**穴が塞がった今こそ、より
> 高い視座で問う**:
>   1. その修正自体が、思想を**別の形で**裏切っていないか（副作用）。
>   2. 「閉じた生存ループ」は本当に端から端まで**連続して**閉じているか。
>   3. survival-network.md の核で、まだ**コードに無い**章はどれか。
>
> 修正はしない。地図を作る。全指摘 file:line つき。
> 既知 (G6 動的ID / G10 heir drift / G11 時定数理論 / follow-up の rx-replay /
> usermain G7 skip) は**参照のみ・再掲しない**。
>
> 監査ブランチ: `w11-audit2` / 対象: master `50173da`（第10波反映済み）/ 2026-06-06
> 実証環境: aarch64 (Termux proot)。`boot/linux` ビルド成功。
> `moe test` ALL PASS、`samples/13_survival_loop/concurrent_infer.sh` シナリオ A+B PASS を実機確認。

---

## 0. 不変条件の再確認（I1–I15 ＋ 第2版で追う新規 I16–I18）

第1版の物差し I1–I15 はそのまま有効（`philosophy-gap-audit.md §0`）。
第10波で「正直さ(I4)」「同時多発(I6)」「認証(I15)」が前進したぶん、その**前進が
新たに要求する**不変条件を 3 つ足す。

| # | 不変条件 | 出典 | なぜ第2版で必要か |
|---|---|---|---|
| **I16** | degraded(k/n) の k と n は**実寄与/実期待**の数であり、観測タイミング (gossip 鮮度) の関数では*ない* | death-piercing §3, survival §10 | G2 が degraded を gossip 由来の期待集合で作るようになったため (下 G12) |
| **I17** | 生まれた／芽吹いたノードは反射・熟慮・world・guard の全器官に**完全参加**する（半身で生まれない）。行動(reflex)はすべての推論経路と**自律的取り込み経路**を覆う | survival §3, §8 | reflex の行動が一部の経路にしか配線されていない (下 G16/G17) |
| **I18** | 「守る対象（守るべき一点）」と「群れの学習を反射へ戻す熟慮ループ」が**コードに実体として存在**する | survival §2, §8, §9 | §2/§8/§9 の核がまだ配管されていない (下 G18) |

---

## 1. 第10波修正の副作用テーブル

第10波は穴を塞いだ。だが**塞ぎ方そのもの**が、思想を別の角度で弱めた箇所がある。
これは「修正が悪い」のではなく「修正が新しい緊張を生んだ地点」の地図である。

重さ: 🔴 思想の核に反する／🟡 思想を弱める／🟢 軽微・自己申告済みに近い
実証: ✅ 実機/CI で確認　📖 コード読み

| 修正 (第10波) | 生んだ緊張 | 証拠 (file:line) | 重さ | 実証 |
|---|---|---|---|---|
| **G2/G8**: degraded を他 region まで数える期待集合に拡張 | **degraded(k/n) の分母が world-gossip の鮮度に依存し、数字自体が嘘になりうる** → **G12** | `dkva.c:384-390`, `world.c:235-240`, `concurrent_infer.sh:125-126` | 🔴 | ✅(前提) |
| **G1**: per-origin Q + responder 時間多重で同時多発を解禁 | **multi-region では coordinator_aggregate(200ms)が responder ループを止め、G1 が戻した同時性を再び直列化** → **G13** | `dkva.c:248-269`, `dkva.c:575-576`, `dkva.c:579-592` | 🟡 | 📖 |
| **G1**: 単一スロット resp/&lt;me&gt; をラウンドロビン時間多重 | **同時多発が単一スロットの再発行帯域 (≈1件/10ms) に上限づけられ、§5「数百件同時」は「数個」が天井**。status の「真の同時多発」は過大 | `dkva.h:55-59`, `dkva.c:582-592` | 🟡 | ✅(挙動) |
| **G4**: 受信 HMAC 検証を実装 | **認証が単一グローバル静的 PSK に依存（鍵配布＝新たな中央/静的設定）＋ 既定は v1 無認証で許容** → **G15** | `net_relay.c:505,524-532`, `:531-542,661-665` | 🟡 | 📖 |
| **G3**: §7/§8 の核に性質テスト＋CI grep | **テストは純ヘルパーのみ。本番 select_expert 統合は無検証。CI は kill_one+moe testのみで G1/G2/loop demo を回さない** → **G19** | `moe.c:516-532`, `moe.c:203-293`, `.github/workflows/ci.yml:44-52,132` | 🟢 | ✅(CI内容) |

---

## 2. ループ連続性の検査（端から端まで閉じているか）

第三レビューの「閉じた生存ループ」:
書く(selfc) → 配る(p-fs gossip) → 発芽(genome) → 記憶で考える(retrieval) →
行動(reflex) → 死を生き残る(guard隔離+死の貫通) → (一周)。
各**つなぎ目**を疑う。

| つなぎ目 | 閉じているか | 証拠 / 所見 |
|---|---|---|
| selfc(書く) → p-fs(配る) → genome(発芽) | **○ (実体あり)** | `genome.c` は manifest を p-fs named ref に書き、sprout が walk。weights/code は p-fs 複製に相乗り。`sprout.sh` が manifest 到着→weight 復元→bit 一致 eval→**カーネル内コード実行**→banner を実証。 |
| 発芽したノード → 全器官への完全参加 | **△ (器官はカーネル側なので参加するが、未検証)** | reflex/world/moe/dkva の各 task は boot (cmd_net) で全ノード対称起動 = sprout は「模型(weights/code)」を得るので**器官の半身欠けは無い**。ただし sprout.sh は eval 一致までしか見ず、芽吹いた node が reflex BEACON / world map / dkva 応答に**実際に参加するか**はアサートしない (I17 未検証)。 |
| retrieval(記憶で考える) → forward | **○** | `dtr.c:556,807` が softmax 直前に `ret_blend` を呼び、p-fs engram の票を logits に加える。`ret_avail`→`pfs_dag_read` で出所は常に p-fs (`retrieval.c:163-176`)。 |
| forward → reflex(行動) | **△ (片肺)** | `dtr.c:281,1319` の dtr 経路だけが `reflex_on_inference` を呼ぶ。**`moe_infer`（§4/§7 の MoE ゲーティング中核）は `world_note_firing` のみで reflex を呼ばない** (`moe.c:332-369`) → **G17**。 |
| reflex(行動) → 次の推論への帰還 | **△ (一方向の鎖。閉ループではない)** | CONSERVE は `reflex_pressure_bias`→`world.c:148` の pressure→`moe.c:163` のゲート勾配 = 行動が**次の推論の経路選択**を変える。しかし**脅威の知覚そのものは変えない**（SHIELD/CONSERVE はモデルの threat_class を下げない）= 知覚→行動を繰り返す**フィードフォワードの鎖**であって、脅威を解消する負帰還ループではない。 |
| reflex(行動) → 自律的取り込みの防御 | **✗ (手動経路のみ)** | SHIELD は対話 shell の `selfc`/`genome` だけを拒否 (`usermain.c:551-569`)。**PKERNEL_SPROUT 自動発芽・p-fs gossip でのコード移動は SHIELD で塞がない** → 実フリート(自動発芽前提)では「本物の防御行動」が自律取り込みを覆わない → **G16**。 |
| 死を生き残る (guard隔離 + 死の貫通) | **○** | `kill_one.sh` A/B/C が推論中のノード kill→degraded 完遂、起点 kill→生存者が同問完遂を実アサート。CI 常駐 (`ci.yml:132`)。 |
| 複合シナリオ（一周を一度に） | **✗ (未検証)** | 各 leg は別々の手動デモ。**「書く→配る→発芽→記憶で考える→殺されても回る」を一つの連続実行で回す script は存在しない**。CI は kill_one.sh + moe test のみで、concurrent_infer.sh / sprout.sh / run_memory_thought.sh / reflex_demo.sh は**回さない** (`ci.yml:44-52,117-132`) → **G19**。 |

**結論（ループは本当に閉じているか）**: 個々の配線は実体を持ち、各 leg は緑。だが
**「閉じた制御ループ」ではなく「個別に緑の鎖」**である。三つの切れ目がある —
(a) 行動が一方の推論経路(dtr)・手動取り込みにしか配線されていない (G16/G17)、
(b) 網規模の熟慮=学習が空 (G18)、(c) 一周を連続実行・回帰防御する仕組みが無い (G19)。
そして行動は**知覚を変えない**ので、厳密には負帰還の閉ループではなく繰り返すフィードフォワード。

---

## 3. 新規・残存乖離 G12+

| # | 不変条件 | 乖離の内容 | 証拠 (file:line) | 重さ | 実証 |
|---|---|---|---|---|---|
| **G12** | I16, I4 | **degraded(k/n) の数字が world-gossip の鮮度に依存する**。requester は応答すべき他 region の期待集合 `rc_expect[]` を **world ビーコンの region_id** から組む。gossip 未着なら「remote ノード 1 個 = 1 region」とみなして保守的に数える (`dkva.c:386-390`)。だが**複数ノードから成る remote region**で coordinator のビーコンがまだ届いていないと、その region の各メンバを別々の期待 region として `rc_cnt0` に積む一方、実際に `rsum` を出すのは coordinator 1 台だけ → `rc_got < rc_cnt0` → **実際は全寄与が畳めていても degraded を誤って出す／分母が過大**。逆に gossip が確定した後でないと「黙って成功にしない」保証も「正しい k/n」も成立しない。`concurrent_infer.sh` 自身が FULL 到達後に **4 秒の sleep で「SWIM RTT + world region beacons converge」を待ってから**測っている (`:125-126`) ことが、honesty 数字が gossip-timing 依存である何よりの証拠。death-piercing §3 が要求する「縮んだ事実は結果と同じ重さの**正確な**情報」を、過渡期には満たせない。※テスト topology は remote region が単一ノード=自身が coordinator なので**この穴を踏まない**（fallback と実態が偶然一致）。 | `dkva.c:384-390`(gossip 由来の期待集合)、`world.c:235-240`(`world_peer_region` は valid な beacon のみ)、`concurrent_infer.sh:125-126`(収束待ち) | 🔴 | 📖 |
| **G13** | I6 | **multi-region では coordinator_aggregate が responder の同時多発を再直列化する**。G1 は単一 region の同時多発を解いたが、coordinator かつ他 region 在のときだけ `coordinator_aggregate` を responder ループ内で**同期呼び**する (`dkva.c:575-576`)。その中で `DKVA_RSUM_WIN_MS`(200ms) のあいだ `tk_dly_tsk(20)` で回り続け (`dkva.c:253-269`)、その間 step 2 の「pending な各 origin 応答をラウンドロビン再発行」(`dkva.c:579-592`) が**止まる**。= region をまたぐクラスタでは coordinator が問いを受けるたび 200ms 応答多重が凍り、G1 が戻したはずの同時性が coordinator 経路で再び詰まる。`concurrent_infer.sh` は A=単一 region で同時多発、B=multi-region で**逐次**1問ずつに分け、**「同時多発 ∧ region 横断」を一度も同時に検証していない** (`:130-215`)。§5「同時に数百件…並行」の核が、最も配管の重い経路で未達かつ未検証。 | `dkva.c:248-269`(200ms 同期窓)、`dkva.c:575-576`(responder ループ内同期呼び)、`dkva.c:579-592`(その間 RR 停止)、`concurrent_infer.sh:130-215`(A/B 分離) | 🟡 | 📖 |
| **G14** | I6 | **G1 の同時多発は単一スロットの再発行帯域に上限づけられた「数個」**。各 responder の応答は per-source の**単一スロット** `resp/<me>` を、1 反復に**1 件だけ**ラウンドロビン再発行する時間多重 (`dkva.c:582-592`, 反復 ≈10ms)。同時起点が増えるほど各起点の応答がスロットに現れる周期が伸び、`DKVA_INFER_TMO`(600ms) 窓内に間に合う起点数は構造的に上限を持つ。`dkva.h:58` のコメント自身が「同時起点が **≤ 数個**でも」と認める。第10波 status 表の「真の同時多発」は §5「数百件同時」に対しては**過大評価**で、実態は「ハードな単一 Q 直列化 → ソフトな単一スロット帯域上限」への置換（隠れた直列化点を消したのではなく緩めた）。 | `dkva.h:55-59`(ANSWER_ITERS と「≤数個」の自認)、`dkva.c:582-592`(1スロット・1件/反復)、`dkva.c:65`(TMO=600) | 🟡 | ✅ |
| **G15** | I15, I1, I16 | **認証が単一グローバル静的 PSK に依存し、既定は無認証**。`PKERNEL_RELAY_KEY` は全ノードが共有する**唯一の平文 32B 鍵**を env から読む (`net_relay.c:505,524-529`)。鍵が無ければ `wire_version=V1`（**無認証**）に落ち (`:531-532`)、`PKERNEL_RELAY_STRICT` 未設定なら受信側も v1 を**許容**する (`:541-542,661-665`)。G4 で受信 HMAC が本物になったぶん、「守るための OS」の信頼基盤が **(1) 単一共有秘密＝一点突破で網全体が開く中央的トラストアンカー、(2) 鍵配布が env 経由の静的外部設定 (G6 と同型)、(3) 既定オープン**という三点に集約した。中央/特権ノードを構造的に排した思想 (I1) を、トラスト層では単一鍵が裏切る。鍵ローテーション/ノード別資格は未実装。 | `net_relay.c:505,524-532`(単一 env PSK→V2)、`:531-532`(鍵無し=V1無認証)、`:541-542,661-665`(STRICT 未設定で v1 許容) | 🟡 | 📖 |
| **G16** | I17, I15 | **reflex の「本物の防御行動」が自律的コード取り込みを覆わない**。SHIELD は対話 shell の `selfc`/`genome` 入力のみを拒否する (`usermain.c:551-569`)。だが survival §3 が前提する実フリートは **PKERNEL_SPROUT による自動発芽**と **p-fs gossip でのコード/manifest 伝播**で増える。これらの自律取り込み経路は reflex を一切経由しない (`genome.c` の sprout 経路・p-fs 複製に SHIELD 照会なし)。= 攻撃下で「未知コードを取り込まない」防御が、人間がコンソールで叩く経路にしか効かず、群れが自分で増える経路には効かない。「考える器官」が自律する場面で行動の核が空く。 | `usermain.c:551-569`(SHIELD は shell verb のみ)、`genome.c:14-20`(自動 sprout 経路に reflex 照会なし) | 🟡 | 📖 |
| **G17** | I8, I17 | **二つの推論経路のうち、行動(reflex)に配線されているのは一方だけ**。`dtr.c:281,1319` の Transformer 経路は `reflex_on_inference` を呼ぶが、**§4/§7 の中核である `moe_infer`（MoE ゲーティング推論）は `world_note_firing` を呼ぶだけで reflex を一切叩かない** (`moe.c:332-369`)。= 思想的に最も中心の「分散ゲーティングで選んだ expert が出した判断」が、行動層へ流れない。survival §8「危険を感じたら即収縮・遮蔽・回避」は dtr 経路でしか起動しない片肺。 | `dtr.c:281,1319`(dtr→reflex)、`moe.c:332-369`(moe_infer に reflex 呼びなし) | 🟡 | 📖 |
| **G18** | I18, I8 | **§8 熟慮層の「網規模の学習を反射へ戻すループ」が空**。`dtk_fl_aggregate` の分散集約は G5 で**正直に `E_NOSPT`** を返す＝**実装されていない** (`fedlearn.c:165-183`)。動くのは単一ノードの局所 delta 適用だけ (`fedlearn.c:146-163`)。よって「遅い時定数で深い判断・学習・設計更新を全体で担い、反射層へ戻す」(survival §8 第2層 / §9 考える器官) は**一台の中では閉じるが、網としては未着地**。moe の二層 (反射 tick / 熟慮 tick) は**選択の時定数分離**であって**学習**ではない (`moe.c:436-468` は score gossip と accuracy 再計算のみ)。第三レビューが指摘した「熟慮層が空」は、選択のローパス化では埋まらず、分散学習の不在として残る。§2「守る対象＝守るべき一点」も同様にコード上の実体 (protected target オブジェクト) を持たず、reflex の行動は抽象的（SHIELD=コード拒否、CONSERVE=pressure 加算）で「一点を守る」対象がない。 | `fedlearn.c:165-183`(分散=E_NOSPT)、`fedlearn.c:146-163`(局所のみ)、`moe.c:436-468`(熟慮=学習でない)、`reflex.c:51-55`(act_table に「守る対象」概念なし) | 🟡 | 📖 |
| **G19** | I7, I4, I6 | **G3 の性質テストは純ヘルパー止まり＋ CI が思想中核の新修正を回さない**。`moe_self_test` は本番の `expert_utility`/`ewma_step`/`deadband_pick` を呼ぶが、**ライブ `select_expert` 統合経路（world gossip / region / recent_pick を読む本体, `moe.c:203-293`）は通らない**。select_expert が中央ソースを読むよう退行しても性質テストは落ちない。さらに CI は `kill_one.sh` と `moe test` のみ (`ci.yml:44-52,132`)。**第10波で最も思想中核の G1/G2 を検証する `concurrent_infer.sh`、loop を成す `sprout.sh`/`run_memory_thought.sh`/`reflex_demo.sh` は CI 外**＝手動一回限りで回帰防御が無い。最も中心の修正が最も回帰に弱い。 | `moe.c:516-532`(test は helper を呼ぶ)、`moe.c:203-293`(統合は無検証)、`.github/workflows/ci.yml:44-52,117-132`(CI 対象) | 🟢 | ✅ |

---

## 4. 公平の節 — 第10波で思想に前進した点

判決ではなく地図。第10波が**実際にコードで**思想を前進させた点を、確認したうえで明記する。

- **I4 同 region の正直さは本物（実証）**: `concurrent_infer.sh` シナリオ B を実機実行し、
  他 region (node3) を SIGKILL 後の推論が `[dkva] degraded (2/3)` を明示して完遂、
  E_TMOUT ゼロを確認 (`/tmp/ci13_B_node1.log`)。黙って成功にしない規約が動いている。
- **I6 同時多発は単一 region で成立（実証）**: シナリオ A で node1+node2+node3 が同フレームで
  発火し、各々が自分のクリーン baseline と**同一 fp**で完遂（取り違えゼロ）。per-origin Q (G1) が
  効いている (`concurrent_infer.sh:147-184`)。G14 はその**上限**の話であって、解禁自体は本物。
- **I7/I8 ゲーティングの性質が数で守られる（実証）**: `moe test` ALL PASS を実機確認 —
  `[moe-nocentral] PASS`（3 観測ノードが局所勾配で別 expert: 中央 argmax への退行なし）、
  `[moe-twolayer] PASS`（step63 fast=4 / slow=40 tick、振動 pp fast=14 / slow=0 のローパス）、
  `[moe-osc] PASS`（naive 28 切替 → 安定 4 切替）、`[moe-concurrent] PASS`。
  しかも本番の `ewma_step`/`deadband_pick` を共用するので、ヒステリシス削除や中央 argmax 退行を
  即検知する設計 (`moe.c:516-532`)。G19 はカバレッジの話であって、この 4 本の価値は本物。
- **I4 配送の正直さ（G9）**: `kdds_pub` の fanout が `pmesh_send==0`（リンク層に渡った）でのみ
  加算、no_route は別カウンタへ (`kdds.c:236-242`, `pmesh.c:317-322`)。観測指標が嘘をつかない。
- **I15 受信認証が本物（G4）**: `net_relay_recv` が v2 frame の HMAC-SHA256 を定数時間比較し、
  偽造/改竄/注入を破棄 (`net_relay.c:642-666`)。G15 は**鍵配布モデル**の批判であって、検証経路自体は堅い。
- **正直な未実装の表明（G5）**: 分散 FedAvg を偽 `E_OK` でなく `E_NOSPT` で返す
  (`fedlearn.c:165-183`)。嘘の成功を消したのは death-piercing 精神の正しい適用。G18 は
  「だから熟慮ループはまだ空」という続きであって、表明自体は誠実。
- **記憶が源であることの徹底（retrieval）**: `ret_blend` は p-fs に engram が無ければ票ゼロ、
  キャッシュは高速化に過ぎず出所は常に p-fs (`retrieval.c:163-176,193-197`)。
  confidence gate (1-pmax)² は 3 条件の精度で実測して選んだと注記 (`retrieval.c:198-224`)。

第10波は骨格をさらに思想へ寄せた。残る乖離の多くは「配管が通ったぶん、**正直さの数字が
過渡条件に依存し (G12)**、**同時性が帯域上限に化け (G13/G14)**、**行動と学習の帰還が一部
未配線 (G16/G17/G18)**、**回帰防御が中核に届いていない (G19)」——「水量(R3)を待たずに先回りで
解禁した配線の、つなぎ目とトラスト層と熟慮ループ」へ収斂する。

---

## 5. 推奨順位（各1行・実装はしない）

1. **G12（degraded 数字の gossip 依存）🔴** — 期待集合に「region_id 未確定の remote」を別 region として
   数えない（保守加算は OK だが、coordinator 未知の region は `unknown` 軸として degraded とは別に明示）。
   I4 honesty の核なので最優先。`world_peer_region` が未確定の間は degraded の k/n を確定値として出さない。
2. **G13（multi-region で同時性が再直列化）🟡** — `coordinator_aggregate` の 200ms 集約を responder ループから
   切り離す（別タスク／非同期収集）か、集約中も RR 再発行を続ける。`concurrent_infer.sh` に「同時多発 ∧ 横断」
   シナリオ C を足して回帰に入れる。§5 の核。
3. **G17（moe 経路が reflex に未配線）🟡** — `moe_infer` の結果クラスから `reflex_on_inference` を呼ぶ。
   行動層を両推論経路で対称にする（loop の片肺を塞ぐ）。
4. **G16（SHIELD が自律取り込みを覆わない）🟡** — 自動 sprout / p-fs コード受領経路でも `reflex_is_shielded()`
   を照会し、攻撃下の自律取り込みを止める。「本物の防御行動」を自律経路へ拡張。
5. **G15（単一静的 PSK）🟡** — 鍵配布を per-link / 回転可能にする方向を docs に明示し、既定 v1 オープンを
   せめて警告から「STRICT 既定」へ寄せる検討。トラスト層の脱・中央。
6. **G18（網規模の熟慮=学習が空）🟡** — fedlearn の分散経路（FL_UDP_PORT バルク delta + aggregator FedAvg +
   平均重みの broadcast）を R3/Phase D の本丸として明示。§2「守る対象」をコードの実体（protected target）として置く。
7. **G19（テスト/CI カバレッジ）🟢** — `concurrent_infer.sh`/`sprout.sh`/`run_memory_thought.sh`/`reflex_demo.sh`
   を CI に入れ、ライブ `select_expert` の NO-CENTRAL を 2 ノード実機で1本数値アサート。
8. **G14（同時性の帯域上限）🟢** — status/docs の「真の同時多発」を「数個の同時多発（単一スロット時間多重）」と
   正直に修飾。複数スロット化は R3 水量を待って。

---

> 第2版の結論: 第10波で骨格はさらに思想へ寄った。だが「閉じた生存ループ」は
> **個別に緑の鎖**であって、一周を連続実行する**閉じた制御ループ**ではない。
> 最も効く副作用は **G12** ——「黙って成功にしない」ための G2 が、過渡期には
> degraded の数字自体で嘘をつきうる。次点は **G13/G17** —— 同時性と行動の核が、
> 最も配管の重い経路で半分しか配線されていない。
> —— 迷ったら `survival-network.md §2/§5/§7/§9` へ戻る。
