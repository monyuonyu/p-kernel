# 思想⇄実装 乖離監査・第5版 — philosophy-gap-audit-5

> 常設の批判器官、第5版。第1版 (G1–G11)・第2版 (G12–G19)・第3版 (G20–G26)・
> 第4版 (G27–G31) を承ける。第4版の評決は鋭かった:
> **「load については環が閉じた。threat については、環が閉じる以前に『脅威』が存在しない。
> いま running system が注いでいるのは、一点ではなく温度バケツであり、向きは逆である。」**
>
> 第13波で G20(符号の倒錯)が**コードに着地した**(`f31716c`/`ec02df3`)。第14波 D隊
> (本書)の任務は祝賀ではない。第14波の別隊が G28(脅威の接地)を**いま実装中**である。
> 本書は:
>   1. G28 が **本物になる受け入れテスト** を、commander へ CI-greppable な形で渡す。
>      偽の close(タイマを「複製完了」に化粧する等)と本物の close を分ける。
>   2. G29(学習の符号)が第13波の rename で**本当に**正されたか、潜在の逆符号経路が
>      残っていないかを file:line で検証する。
>   3. G30/G22/G23/G31 の現況を確定する。
>   4. 次に弱い接合部 **G32+** を、すべて file:line で。次波の本丸を 1 つ選ぶ。
>
> 修正はしない。地図を作る。
>
> 監査ブランチ: `w14-audit-v5` / 対象: master `f289cf7`(**第13波 G20 統合 + ARK 統合後**) / 2026-06-07
> 実証環境: aarch64 (Termux proot)。コード読み + CI 設定の精査。
> 既知 (G1–G31) は**参照のみ**。第5版は G28 受け入れ条件と G32+ を主題にする。

---

## 0. 一行サマリ（評決を先に）

第13波で **G20 は本物にコードへ着地した** — `expert_utility` は load を引き threat を**足す**
二符号になり(`moe.c:165-166`)、CONSERVE は `compute_pressure` から外され脅威軸へ移った
(`world.c:150-155` / `compute_threat` `world.c:168-171`)。`[moe-protect] PASS` が CI 配線
された(`ci.yml:63,102`)。G29 の逆符号経路も**閉じた**(下 §2: rename は意味論ごと反転した)。

**だが第4版の根の指摘 — 脅威がそもそも環境から接地されていない(G28)— は master で 1 文字も
変わっていない。** threat の唯一の源はいまも温度バケツ(`moe.c:107-114`)、threat を縮める
actuator は無く、threat は **5 秒タイマ** で消える(`conserve_until`, `reflex.c:283-289`,
`REFLEX_HOLD_MS=5000`)。そして本書が新たに突き止めた最重要点は: **「環が閉じた」と CI が
証明している `[moe-protect]`/`[reflex-fb]` は、本番が走らせる経路ではなく、self-test 内の
合成プラント `lp_run` を証明している(G32)。** G20 の符号反転は正しいが、**符号が正しい
だけのループは、いまだ本番では閉じていない。** さらに本番の dwell ホメオスタットは、出力が
被制御量に効かない**開ループのまま MAX へ rail する**(G33)。G20 を正した結果、確信度ゲートの
死(G30)は**より危険になった**(G34): 誤発火が一点へ逃げる(自損)のではなく、群れを偽の一点へ
**集束させる**(集団誤配分)ようになった。

---

## 1. G28 の受け入れテスト — 偽の close と本物の close を分ける

### 1.1 master 現在(第14波 G28 着手前)を file:line で確定

- **脅威の源 = 温度バケツ。** `gate_predict` は温度を固定しきい値で 3 区間に割るだけ
  (`moe.c:107-114`)。moe_infer はこの結果クラスをそのまま reflex へ渡す(`moe.c:419`)。
  reflex はこの「温度バケツ」を「脅威レベル」と再解釈する(`act_table`, `reflex.c:53-57`)。
  → **第4版 G28 のまま。脅威はセンサに接地していない。**
- **守るべき一点(protected object)はオブジェクトとして不在。** `protect` の grep は
  `MOE_PROTECT_*`(効用係数)・`u_protected`(self-test 内のローカル変数)・`memory protect`
  だけ(§本書 grep)。**第一級の「守る対象」型は存在しない。**
- **脅威を縮める actuator が無い。** reflex の三行動はどれも環境/センサを変えない:
  SHIELD=取込拒否(`reflex.c:296-302`)、CONSERVE=脅威軸を立てる(`reflex.c:304-312`)、
  BEACON=警報発行(`reflex.c:139-156`)。threat の解除は**唯一 `conserve_until` の
  タイマ満了**(`reflex.c:283-289`, `REFLEX_HOLD_MS=5000`, `reflex.h:60`)。
  → **threat ループは構造的に開いている(G21 の threat 版・未解消)。**
- **複製はあるが、複製数を origin が検証する経路が無い。** `pfs_repl.c` は
  announce→want→block 転送 + 受信側の content-hash 検証(`pfs_repl.c:372-388`)を持つが、
  `send_block_to` は **fire-and-forget の unicast**(`pfs_repl.c:235-278`)。受信側は store
  しても **origin へ ACK を返さない**。ACK/replica-count/quorum/R 閾値は grep でゼロ
  (`pfs_repl.{c,h}`)。→ **「R 個の耐久コピーに到達した」を測る術が現状コードに無い。**

### 1.2 偽の close(FAKE)はこう見える — D隊の成果を鵜呑みにしない判定基準

| # | 偽の手口 | 見破り方 |
|---|---|---|
| F-a | **タイマを「複製完了」に化粧。** threat の解除を `conserve_until` の満了のまま残し、ログだけ「object replicated → threat cleared」に変える | `reflex.c:283-289` の解除条件が **`now >= conserve_until` のまま**なら偽。本物は「**R 個の neighbour に検証済みコピーが在る**」が解除条件 |
| F-b | **protected-object = 自ノードの既存の仕事の名前替え。** `my_total`/backlog を `protected_object` に rename しただけ | object が **content-addressed で neighbour へ転送・再ハッシュ検証され、kill 後に別ノードから復元できる**(= pfs の id 検証, `pfs_repl.c:372`)ことを要求。ローカル変数の改名は kill-test で落ちる |
| F-c | **複製数が neighbour で検証されていない。** origin が「announce を出した=複製された」と仮定して count を自前で増やす | `send_block_to` は ACK を返さない(`pfs_repl.c:235-278`)。**受信側からの確認(ACK か、kill 後の実復元)**で R を数えること。送信回数で数えるのは偽 |
| F-d | **緑が self-test の合成プラント由来(G32)。** `lp_run` の `L` を新 actuator で削って `[g28-*] PASS` を出すが、本番 `world.c`/`reflex.c` 経路は不変 | テストが **kill した本物のノードに対し、本物の `pfs_repl`/`world`/`reflex` を回す**こと(下 Test-L)。`lp_run` 内の数値証明は受け入れない |
| F-e | **actuator-off の対照が無い。** ON だけ緑にして「閉じた」と主張 | actuator を切ると **object が失われ / threat が下がらない**ことを同じハーネスで示す(負の対照)。対照無しの緑は偽 |

### 1.3 本物(REAL)の必要条件と、commander へ渡す具体テスト

**前提**: §2 の「守る対象」を**第一級オブジェクト**にし、rally の力が**その object を
neighbour へ複製**し、**>=R 個の耐久コピーに検証到達したとき初めて threat が下がる**こと。
threat の解除条件を「タイマ満了」から「object が安全(R 到達)」へ**置換**する(タイマは
上限のフェイルセーフとしてのみ残してよいが、それは解除の*理由*ではない)。

CI が grep する PASS 行として、以下を要求する(2 系統: 純ローカル self-test と **live kill-test**)。

- **Test-1 接地(`[g28-grounded] PASS`)**: threat の源が温度バケツ(`gate_predict`)から
  **protected-object の危険度**へ替わったこと。同一温度でも、保護対象が「未複製(危険)」なら
  threat>0、「R 到達(安全)」なら threat==0 になることを数で示す。温度だけで threat が決まる
  実装(現行)は FAIL。
- **Test-2 actuator が脅威源を削る(`[g28-actuator] PASS`)**: rally の力で object が
  neighbour へ複製され、**複製数 r が増えるにつれ threat が単調非増加**。r>=R で threat==0。
  → 「rally すると(温度ではなく)**保護対象が安全側へ動く**」を初めて数で示す。
- **Test-3 タイマでないことの対照(`[g28-not-timer] PASS`)**: actuator を**off**にして
  同じ時間を回すと、threat は **REFLEX_HOLD_MS では下がらない**(= 現行のタイマ解除を**負の
  対照**として明示)。actuator on で**HOLD_MS より早く** r>=R 到達と同時に threat が落ちる。
  → F-a(タイマ化粧)を機械的に弾く。
- **Test-L 本番経路の kill-test(`[g28-rally-live] PASS`)— 最重要、G32 を晴らす**:
  `samples/13_survival_loop` / `22_composite` と同型の **live N>=3 ノードクラスタ**で、
  (a) ノード P に protected-object を生成、(b) **P を SIGKILL**、(c) **object が別ノードから
  復元できる**(content-id 一致, `pfs_repl.c:372` の検証)こと、(d) **actuator-off の対照run
  では object が失われる**ことを示す。これは `lp_run`(self-test)でも温度バケツでもない、
  **本番 `pfs_repl`/`world`/`reflex` が走る** ことの証明。
- **Test-CI 配線**: `[g28-grounded]/[g28-actuator]/[g28-not-timer]` を `ci.yml:52-67` の
  grep 群へ。`[g28-rally-live]` は survival-loop 系ジョブ(`ci.yml:135-156`)へ。
  **doc/`lp_run`-only の緑は受け入れない。**

**REAL の要約判定**: threat の解除条件が**タイマから「object が R 個の検証済み耐久コピーに
到達」へ置換**され、**本番ノードを kill しても object が生き残り**、**actuator-off の対照run
が object を失う** こと。これら無しの「G28 DONE」は偽。

---

## 2. G29 の検証 — rename は逆符号経路を残したか

**残していない。逆符号経路は閉じた(G29 CLOSED, 🟢)。** 第13波は変数を rename しただけでなく
**意味論ごと反転**させた。file:line で確定する:

- `learned_conserve` が流れる先は **threat 軸だけ**になった。`reflex_threat_level()` が
  `learned_conserve` を返し(`reflex.c:311`)、それを `compute_threat()` が拾い
  (`world.c:168-171`)、`WORLD_BEACON.threat` として配り(`world.c:198`)、moe ゲートで
  **加点**される(`moe.c:166` `u += threat*…`; self は `moe.c:279` で直読)。
- **逆符号の旧経路(`compute_pressure` への CONSERVE 加算)は除去された。** `world.c:150-155`
  にその旨の明示コメントがあり、CONSERVE はもう pressure(load 軸, `moe.c:165` で**減点**)
  へは載らない。第4版が名指した `reflex_pressure_bias()`(自圧↑→自他から回避)は **grep で
  消滅**(本書 grep: 該当ゼロ)。
- したがって `reflex_deliberate` の nudge 方向が**正しい符号になった**。dwell が長い(=応援が
  足りない)→ `learned_conserve` を**上げる**(`reflex.c:331-335`)= threat 軸を強める = **rally を
  強める**方向。第4版が懸念した「学習が逃走ゲインを最適化」は、`learned_conserve` の意味が
  「逃走強度」から「rally の受け入れ強度(脅威軸ゲイン)」へ反転したことで**整合した**。

> **判定(G29)**: rename は latent 逆符号経路を**残していない**。符号は thread 全経路で一貫して
> 「脅威→加点→寄る」。**ただし**これは「符号が正しい」を意味するだけで、「ループが本番で
> 閉じている」ではない(下 G32/G33)。`reflex_deliberate` が**正しい符号で**最適化していても、
> その出力(threat ゲイン)が本番で被制御量(dwell)に効かなければ、正しい方向へ**空回り**する。

---

## 3. G30 の現況 — confidence ゲートは moe 経路で死んでいるか

**いまも死んでいる(G30 OPEN, 🟡)。** `moe_infer` は reflex を**必ず `confidence=0xFF` で
呼ぶ**(`moe.c:419`、コメント `moe.c:418` も「未知 → 0xFF(ゲートを通す)」)。reflex 側の
`REFLEX_CONF_MIN=40` ゲート(`reflex.h:68`, `reflex.c:208`)は `0xFF` を**素通し**させる設計
なので、**moe 経路の全推論が confidence に関わらず反射を発火**する。

**違反する不変条件 — §9「不確実さについての正直さ」。** §9 は「記憶に接地して考える器官」を
謳い、§10 は staleness/uncertainty を隠さないことを要求する。最も発火頻度の高い moe 経路で
「低確信なら行動しない」が無効である以上、システムは**自分の推論の不確実さを行動の手前で
表現していない**。低確信の誤推論が CONSERVE/threat を立て、そのまま行動へ抜ける。
(`dtr` 経路では confidence が実値で渡るので §8 慎重化は効くが、moe 経路では効かない。)

→ 第4版から不変。**しかも G20 後はこの死がより高くつく(下 G34)。**

---

## 4. G22 の現況 — 「熟慮」はオンライン学習か、適応則か

**依然オンライン学習ではない(G22 OPEN, 🟡)。** master `f289cf7` で:

- 分散 FedAvg は依然 `E_NOSPT`(`fedlearn.c` の `fl_distributed_*` は正直な未実装表明を返す)。
- 局所学習 `fl_local_train` は依然 **635 パラメータ中 b3 バイアス分のみ有限差分**
  (`fedlearn.c` の `for j < MLP_OUT` 周辺)。重み本体 w1/w2/w3 は touch しない。
- `reflex_deliberate`(`reflex.c:321-352`)は**重みを 1 つも動かさない**。動かすのは
  `learned_conserve` という**反射ゲインのスカラ 1 個**(`REFLEX_LEARN_STEP=6` 刻み,
  `[MIN,MAX]=[8,80]` クランプ, `reflex.h:89-92`)。これは「経験から自分を書き換える考える
  器官」(§9)ではなく、**観測駆動のホメオスタット(事前固定の適応則の実行)**。

→ **G22 は学習でなく適応則。** G28 が脅威を接地し threat→actuator→dwell の本番ループが
**実際に閉じれば**、`reflex_deliberate` が回す `learned_conserve` は初めて「実 outcome から
ゲインを調律する」**本物の online 適応**になりうる(現状は被制御量が出力に効かず空回り; G33)。
だが重み本体を outcome で更新する経路は G28 後も無く、§9 の「学習」へはなお一歩足りない。

---

## 5. G23 の現況 — federation はコードに配線されたか

**doc-only のまま(G23 OPEN, 🔴)。** master で確認:

- `DNODE_MAX = 32` は不変(`arch/common/include/drpc.h:35`)。node_id も 0–31 前提
  (`drpc.h:23`)。
- `arch/` に federation / 複合 ID(region_id, local_id) / 上位メッシュ / coordinator-mesh の
  **配線は皆無**(grep: `federation`/`composite id`/`coordinator mesh` 該当ゼロ)。`region.c` は
  依然 32 を**分割**するだけ。
- `docs/architecture/federation.md` は設計図のまま。

→ **「数千ノード」の看板に対し、論理層は絶対上限 32。橋は設計図だけで、橋桁は無い。**
§2 の「守る力 = ネットワーク**全体**」は、全体が高々 32 ノードに留まる限り原理的に小さい。

---

## 6. G31 / G25 の現況 — §4 の核心は計測されたか・durable/ARK/locality は CI に在るか

**§4 の核心(光速/遅延/実 joule/実機)は依然未計測。durable/ARK/locality デモは local-only で
CI 非配線(G31 OPEN, 🟡)。**

- **CI に durable/ARK/locality は無い。** `ci.yml` 全文を grep して `durable`/`ark`/`locality`
  /`23_`/`24_`/`25_`/`26_` は**ゼロ**。CI が回すのは moe/reflex/pfs/hrw self-test(`ci.yml:52-67`)、
  relay 6 シナリオ(`:122-133`)、survival-loop(`:135-156`)、composite(bonus, `:158-183`)のみ。
  → 第12波 F隊が出した locality の 2.9× 削減も、第12/13波の ARK/durable backend も、**回帰で
  守られていない**(local 手動実行のみ)。
- **§4 の latency(光速の壁)は未測定。** RTT zone は region 形成のため観測 RTT を水増しする
  だけで実遅延を注入しない(第4版 §5 の locality.md 自認, 不変)。
- **エネルギーは byte-proxy であって joule ではない。** 実電力計は無い(同上)。
- **実機ゼロ。** bare-metal ジョブは **build-only**(`ci.yml:106-120`)。全 survival 計測は
  localhost。RPi/ベアメタルの実走は依然監査対象外。

→ G31 は第4版から不変。**監査の最深部(遅延・実機・joule)と回帰防御の穴は残る。**

---

## 7. 新規・残存乖離 G32+

重さ: 🔴 思想の核に反する／🟡 思想を弱める／🟢 軽微・前進。実証: ✅ 実機/CI　📖 コード読み

| # | 不変条件 | 乖離の内容 | 証拠 (file:line) | 重さ | 実証 |
|---|---|---|---|---|---|
| **G32** | (メタ) I7, §2 | **「環が閉じた」と CI が証明しているのは本番経路ではなく self-test 内の合成プラント `lp_run` である。** `[moe-protect]`/`[reflex-fb]` の PASS は、手で組んだ utility 配列と `lp_run` 内部の backlog `L` に対し本番効用関数を呼ぶ(`reflex.c:561-621`)。だが**本番の threat 経路** `compute_threat→reflex_threat_level→learned_conserve`(`world.c:170`)→`beacon.threat`→`eff_threat`→`expert_utility` には、**neighbour が rally したとき何かを減らす actuator が無い**。本番で threat が下がる唯一の道は `conserve_until` タイマ満了(`reflex.c:283-289`, `HOLD_MS=5000`)。= **CI 緑は「threat が接地していれば rally するはず」を証明するが、shipped system の threat は 5 秒タイマで消える。** 検証器が、出荷物が走らせない性質を証明している(G27 の親戚: あちらは sim≠kernel、こちらは self-test plant≠live plant)。 | self-test: `reflex.c:561-621`(`lp_run` 合成 L), `moe.c:863-971`(`st_test_protect` 手組 utility) vs 本番: `world.c:168-171,198`, `moe.c:279`, `reflex.c:283-289,304-312`。CI: `ci.yml:63,66`(self-test のみ) | 🔴 | ✅(CI/コード) |
| **G33** | I18, §8/§9 | **本番の dwell ホメオスタットは、出力が被制御量に効かない開ループのまま MAX へ rail する。** `reflex_deliberate` は「平均脅威 dwell > target(=3)」なら `learned_conserve` を上げる(`reflex.c:331-335`)。`lp_run`(self-test)では gain↑→rally→`L↓`→dwell↓ と**閉じる**(`reflex.c:594-603`)。だが**本番には L を削る actuator が無い**(G32)ので、危機が続く限り dwell は HOLD_MS いっぱい続き常に target=3 を超える → `learned_conserve` は**単調に MAX=80 へ飽和**(`reflex.h:90`)、過剰防御を抑える下げ枝(`reflex.c:336-343`)はほぼ発火しない。= 本番の「学習」は setpoint へ調律しておらず、railする。G29 で**符号は正しくなった**が、正しい符号のまま空回りする。 | `reflex.c:321-343`(nudge 規則), `reflex.h:89-92`(MIN8/MAX80/TARGET3/STEP6), 本番に actuator 不在(`reflex.c:283-289` のタイマ解除), self-test だけ閉路(`reflex.c:594-603`) | 🔴 | 📖 |
| **G34** | I4, §2 | **G30(confidence 死)は G20 後に *より危険* になった。** moe 経路は依然 `confidence=0xFF` 固定(`moe.c:419`)で、低確信の誤推論も reflex を発火させる。G20 *以前* は誤発火が自ノードの pressure を上げ「自分が遠方へ逃げる」自損(影響は自ノードに局在)だった。G20 *以後* は同じ誤発火が threat 軸を立て、moe ゲートが**近傍を偽の一点へ rally させる**(`moe.c:166` 加点)= **群れの計算資源を危険でないノードへ集団誤配分**。符号を正した(G20)結果、確信度ゲートを死なせたまま(G30)にする代償が**自損から集団誤配分へ拡大**した。修正は小(moe 経路の confidence を max-softmax×100 等の実値で渡す)。 | `moe.c:418-419`(0xFF 固定), `moe.c:166`(threat 加点=rally), `reflex.c:208`(0xFF 素通し), `reflex.c:304-312`(threat 軸立て) | 🟡 | 📖 |
| **G35** | §5, §2 | **脅威は 1 ノード 1 スカラなので、§5「同時多発・並行分散」と §2 が同時に成立しない。** `WORLD_BEACON.threat` は単一バイト、`world_peer_threat` は 1 値を返す(`world.c:256-260`)。reflex も `conserve_until`/`learned_conserve` を 1 組しか持たない(`reflex.c:78,103`)。= 1 ノードが同時に表現できる「守るべき一点」は**高々 1 個**。§5 が要求する「無数の入力それぞれに別々のエキスパート群が同時並行で発火」「数百件の危機が同時進行」は、**保護対象ごとの threat 軸が無い**ため畳まれて表現できない。survival-network.md の対応表自身が §5 を**未実装**と認める(`survival-network.md:304`)。G28 が**1 点**を接地しても、この壁は残る。 | `world.c:256-260`(threat 単値), `arch/common/include/world.h`(WORLD_BEACON.threat 単バイト), `reflex.c:78-79,103-106`(単一 conserve/learned 状態), `survival-network.md:304`(§5 未実装) | 🟡 | 📖 |

---

## 8. 公平の節 — 第13波で前進した本物

判決でなく地図。コードで実際に前進した点を明記する。

- **G20 は本物にコードへ着地した。** 効用関数は load を引き threat を足す二符号
  (`moe.c:165-166`)、CONSERVE は load 軸から脅威軸へ移った(`world.c:150-155,168-171,198`)。
  `[moe-protect]` は本番 `moe_expert_utility`/`deadband_pick` を直叩きし(`moe.c:909-963`)、
  「P は仕事を保持(hold)・近傍は寄る(rally)、naive(threat==load)は flee/avoid」を数で示し、
  **CI 配線済み**(`ci.yml:63,102`)。第4版 §1.3 Test-1/2 の精神を満たす。
- **G29 は閉じた。** 逆符号の `reflex_pressure_bias()` は除去され(grep 消滅)、学習対象の
  意味論が rally ゲインへ反転した(§2)。
- **G27 は部分的に晴れた。** `reflex test` が CI へ配線され(`ci.yml:66-67,103-104`)、第4版が
  名指した「最重要 self-test が回帰の外」は解消。
- **NO-CENTRAL は依然本物。** `select_expert` は局所 world-table と自己観測のみ読む
  (`moe.c:241-337`)。per-source topic で単一集約点を作らない設計が threat 軸でも一貫
  (`world.c:102-116` の per-source beacon)。
- **正直な未実装表明が一貫。** FedAvg `E_NOSPT`、federation.md「1 行も変えていない」、
  locality の latency 未測定の自認。death-piercing 精神は健在。
- **ARK が p-fs の耐久 backend になった(第13波 白眉)。** content-addressed/log-structured/
  crash-safe(`91dd05a`)。§9「記憶の保存は思考の前提」の土台が一段固くなった(ただし CI 非配線, G31)。

---

## 9. 推奨順位（各1行・実装はしない）

1. **G28(脅威の接地)🔴 — 第14波 本丸(進行中)。** §1.3 の Test-1/2/3/L を受け入れ条件に。
   特に **threat 解除を「タイマ満了」から「object が R 個の検証済み耐久コピー到達」へ置換**し、
   **live kill-test(`[g28-rally-live]`)**で証明。`lp_run`/温度バケツの緑は不可。
2. **G32(self-test plant ≠ live plant)🔴 — 次波の本丸(下 §10)。** G28 を**本番経路で**
   閉じ、CI を `lp_run` から **live kill-test** へ昇格。これが片づけば G33 は構造的に消える。
3. **G34(confidence 死が rally で増悪)🟡** — moe 経路の confidence を実値で渡す(小修正)。
   G20 を正した今こそ優先度が上がった。
4. **G35(threat 単スカラ→§5 と §2 が両立しない)🟡** — protected-object を**集合**にし、
   object ごとの threat 軸へ。G28(1 点接地)の**直後**の深い壁。
5. **G31(durable/ARK/locality が CI 非配線・§4 核心未測定)🟡** — 23/24/25/26 を CI へ。
   relay v2 タイムスタンプで latency 実測。
6. **G23(federation 未配線)🔴** — F0(コード不変の足場)を実コードへ。§2 の「全体」を 32 超へ。
7. **G22(学習でなく適応則)🟡** — G28 で本番ループが閉じれば `reflex_deliberate` が初めて
   本物の online 適応になりうる。重み本体の outcome 更新はなお別課題。

---

## 10. 次波の本丸 — G32（本番経路でループを閉じ、live kill-test で守る）を推す

**G28 の次は G32 を本丸にすべき。** 論拠:

1. **この監査系列が毎版見つけ続ける病はただ一つ — 「緑の self-test、死んだ本番経路」。**
   第3版「環が逆符号で閉じる」、第4版「sim が別の系・別の軸を見ている(G27)」、本書「G20 の緑は
   `lp_run` を証明し本番 threat はタイマで消える(G32)」。**症状名は毎回変わるが病は同じ**:
   検証器が、出荷物が走らせない性質を証明する。G32 を本丸に据えるとは、この病を**器官の規律
   として恒久的に断つ**こと — §2 のあらゆる主張を、in-process oracle ではなく **running
   system の kill-test** で証明する体制へ移す。

2. **G28 は G32 無しでは「もう一つの `lp_run`」になりうる。** D隊が actuator を実装しても、
   それを**また self-test 内のプラントへ**埋めて `[g28-*] PASS` を出すことは容易い(F-d)。
   G32 を本丸にする = G28 の actuator が **本番 `pfs_repl`/`world`/`reflex` に在り、本物の
   ノードを kill して object が生き残る** ことを CI が要求する、という受け入れ規律を恒久化する。
   §1.3 Test-L はその第一インスタンスである。

3. **依存順序が正しく、波及が広い。** G32(本番でループを閉じ live で守る)が片づけば、
   **G33(ホメオスタットの空回り)は構造的に消える**(出力が被制御量に効くようになる)、
   **G22 は本物の online 適応の入口に立つ**、そして §2 の「注ぐ先が実在する一点」が初めて
   running system で真になる。G34 は小修正、G35/G23 は G32 が立った**後**の深い壁として
   自然に積み上がる。

> 対案として G35(§5 多点)を本丸にする手もあるが、G35 は「単一点のループが本番で閉じている」
> ことを前提にする深化であり、**まだ単一点すら本番では閉じていない**(G32)以上、時期尚早。
> 深さでなく**接地の順序**で選べば **G32**。

---

## 11. 評決（running system は §2 にどれだけ近いか・正直に）

> **符号は正された。だが「環が閉じた」と CI が言うとき、閉じているのは self-test の中だけだ。**
> 第13波で G20 は本物にコードへ着地し(`moe.c:165-166`)、G29 の逆符号経路は意味論ごと反転して
> 閉じた(§2)。これは飾りではない実コードの前進で、第4版が「1 文字も直っていない」と断じた
> 状態からは確かに進んだ。
> **しかし §2 の最独自点 — 守るべき一点へ全網の力を *注ぐ* — は、running system では依然
> 接地していない。** 脅威の源はいまも温度バケツ(`moe.c:107-114`)、threat を縮める actuator は
> 無く、threat は 5 秒タイマで消える(`reflex.c:283-289`)。`[moe-protect]`/`[reflex-fb]` の緑は、
> 本番が走らせる経路ではなく合成プラント `lp_run` を証明している(G32)。本番の dwell
> ホメオスタットは出力が被制御量に効かないまま MAX へ rail する(G33)。そして符号を正した
> 結果、確信度ゲートの死(G30)は自損から**集団誤配分**へと**増悪した**(G34)。
> **正直に言えば、前版より §2 に近づいたとは言いがたい。** 第4版は「向きが逆」と言った。本書は
> 「向きは直った、が、その向きはまだ本番では*どこへも向いていない*」と言わねばならない —
> 注ぐ先(protected object)が running system に存在せず、注いだ結果(脅威の鎮静)を本番で測る
> 経路も無いからだ。G20 の符号反転は**必要条件の一つを満たしただけ**で、§2 の接地は G28 が
> 本番経路で(G32 の規律で)閉じて初めて始まる。
> —— 迷ったら `survival-network.md §2`(守るべき**一点**へ群れの力を**注ぐ**)へ戻る。
> いま running system は、注ぐ向きこそ正したが、注ぐ先も注いだ証も、self-test の外にはまだ無い。
