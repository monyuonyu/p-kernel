# 思想⇄実装 乖離監査 — philosophy-gap-audit

> 監査隊の地図。**掲げる設計思想と実装の乖離点・実装が弱い点**を、コードの
> 行レベル証拠つきで洗い出す。修正はしない。
>
> 照らした正典: `survival-network.md`（§2 中央の不在 / §5 同時多発 / §7 分散ゲート /
> §8 二層時定数 / §10 古さの明示）、`regions.md`、`reflex-deliberation.md`、
> `death-piercing.md`（正直さ規約）、`decentralized-lookup.md`、`relay-ha.md`、
> ルート `README.md`（正直4分割）、`docs/architecture/README.md`（唯一の不変条件）。
>
> 既知の穴（`assessment_structural_gaps.md` ①〜⑥、relay-ha.md の pmesh ルート不形成、
> ベアメタル guard 捕捉側未実装、selfc 無署名コード実行）は**再掲しない**。
> ここに書くのは、それらに**無い**乖離である。
>
> 監査ブランチ: `w-audit-gap` / 対象: master `c01d075` / 最終更新 2026-06-06
> 実証環境: aarch64 (Termux proot)。`boot/linux` ビルド成功、`hrw`/`hrw-l1` PASS、
> 単体 `moe` がローカルフォールバックすることを実機確認。

---

## 0. 検証可能な不変条件リスト（正典から抽出）

監査の物差し。各条は doc の主張を「コードで守られているか確認できる形」に落としたもの。

| # | 不変条件 | 出典 |
|---|---|---|
| **I1** | 集約専用ノード／特権ノードが構造的に存在しない（中央の不在） | survival §2, arch/README §1 |
| **I2** | region coordinator = 決定的最小 ID であり**特権ではない** | arch/README §1 |
| **I3** | 推論の起点（requester）に特権はない | death-piercing |
| **I4** | 群れが縮んだまま完遂したら**必ず degraded を明示**、黙って成功しない | death-piercing §3 |
| **I5** | 古さ・不完全さを明示する（stale/unknown を隠さない） | world.h, survival §10 |
| **I6** | 複数の問いが**同時並行**で発火できる（一点集中ではない） | survival §5 |
| **I7** | ゲーティングは局所勾配のみ。全網の真実を**集約する場所が無い** | survival §7 |
| **I8** | 反射（速い時定数）と熟慮（遅い時定数）を**混ぜない** | survival §8, reflex |
| **I9** | SOLO／孤立でも脳である（単体経路が常に残る） | regions §4 不変条件 |
| **I10** | region は論理層。relay 無し・素メッシュでも動く | regions §4 |
| **I11** | 異種 ABI を跨いで成立（cross-ABI 決定性） | regions §4, p-fs |
| **I12** | 中央索引なしに名前→所在を引く（HRW） | decentralized-lookup |
| **I13** | 解決不能は「未発見」を返す。**嘘の成功を返さない**（窓は有界） | decentralized-lookup §4.2 G2 |
| **I14** | 固定上限を溢れたとき、黙って壊れない（正直に縮退する） | regions §1.2 / 監査要件 |
| **I15** | 「守る」: ノード間の通信経路は認証される | relay-ha, phase_b_relay |

---

## 1. 乖離表

重さ: 🔴 思想の核に反する／設計変更級　🟡 思想を弱める／部分的に裏切る　🟢 軽微・自己申告済みに近い
実証: ✅ 実機/コードで確認　📖 コード読みのみ

| # | 不変条件 | 乖離の内容 | 証拠 (file:line) | 重さ | 実証 |
|---|---|---|---|---|---|
| **G1** | I6, I7, I1 | **`Status: FIXED` (wave 10, `w10-concurrent`)** — Q を per-origin `dtr/dkva/q/<origin>` 化し、起点ごとに独立ラッチ。RESP_PKT に `origin` を足して取り違えを消し、responder が pending 応答を resp/<n> へ時間多重で再発行 → 複数起点が同時完遂 (`samples/13_survival_loop/concurrent_infer.sh` シナリオA: 2/3起点同時, fp 一致)。<br>**DKVA の Query は単一の共有ラッチトピック**。`resp/<n>`・`rsum/<rid>` は per-source 化されたのに、`Q` だけは網全体で1スロット。異なる起点ノードが同時に推論を発行すると、後の Q が前の Q を**上書き**し、上書きされた側の responder は応答しない（LATEST_ONLY ラッチ）。= 網全体で**同時に in-flight な問いは1つ**。§5「同時に数百件…並行して立ち上がる」の真逆で、推論の**隠れた直列化点＝隠れた中央**。 | `dkva.h:31` (`DKVA_TOPIC_Q "dtr/dkva/q"` 単一名)、`dkva.c:550-553`(pub/sub とも単一)、`dkva.c:291`(全起点が同一トピックへ pub)、`dkva.c:488`(responder は最新 Q のみ sub)。対比: `dkva.h:32-38` が resp を per-source 化した理由を明記しているのに Q は据え置き。**修正: `dkva.h`/`dkva.c` per-origin Q + 時間多重, `kdds_open_poll_scoped`** | 🔴→✅ | ✅ |
| **G2** | I4 | **`Status: FIXED` (wave 10)** — 期待集合に他 region の coordinator を追加 (world gossip の `region_id` で remote ノードを region 単位に束ね、未着なら保守的に各ノードを数える)。欠けた region は degraded(k/n) に計上、SWIM DEAD は待たないが黙って成功にしない (`concurrent_infer.sh` シナリオB: 他 region 全滅で `degraded (2/3)`)。<br>**degraded(k/n) は自 region しか数えない**。`expect[]` は `region_is_member` の peer だけに立ち、`exp_cnt0` も同 region のみ。他 region の `rsum` が（coordinator 死亡・パケット欠損で）届かなくても、`exp_got<exp_cnt0` は偽のままなので **degraded が付かず**、縮んだ群れを黙って成功扱いにする。早期確定も `remote_alive==0` の時だけで、remote_alive>0 のまま rsum が来ないと 600ms 待って**無音で**部分結果を返す。death-piercing §3「群れが縮んだ事実は結果と同じ重さの情報／黙って成功にしない」に反する。 | `dkva.c:348-355`(expect は同 region 限定、remote は数えるだけ)、`dkva.c:377-390`(rsum 収集に期待集合・完全性の追跡なし)、`dkva.c:414-429`(旧: degraded 判定は `exp_got<exp_cnt0` のみ)。**修正: `rc_expect[]` で他 region coordinator を期待集合化, `world_peer_region()` 追加** | 🔴→✅ | ✅ |
| **G3** | I6, I7, I8 | **§7/§8 の核（moe.c）に自動テストが1本も無い**。CI は `pfs`/`hrw`/`hrw-l1`/relay 6本/survival-loop のみ。NO-CENTRAL ゲーティング不変条件（I7）も、二層時定数のローパス性（I8）も、§5 同時多発も、**性質を守るテストが存在しない**。reflex D0（単一時定数の発振をホストで再現）は doc 上 **DESIGNED のまま**で、ヒステリシスが効いていることを「数で」言えない（doc 自身が認める）。最も思想的に中心のコードが最も無検証。 | `.github/workflows/ci.yml:40-46,107-122`(テスト対象に moe/region/world/dkva無し)、`reflex-deliberation.md:256`(D0=DESIGNED)、`arch/README.md:142`(発振の定量観測「まだ」) | 🔴 | ✅ (CI 内容確認) |
| **G4** | I15, I1 | **relay クライアントの受信経路が HMAC を検証しない**。`net_relay_recv` は magic 4バイトしか見ず、v2 の MAC を**素通し**（"We trust the relay to MAC-verify before forwarding; the client side only checks structure"）。relay の src アドレス（既知・固定）を詐称して UDP を撃てる者は、任意フレームをカーネルの netstack へ注入できる。relay-ha.md は「wire protocol (v2 HMAC) は不変」と謳うが、**エンドツーエンドの真正性はクライアント側で破れている**。「守るための OS」の無防備面。 | `arch/linux/aarch64/net_relay.c:495-516`(magic のみ確認、MAC 検証なし)、同 `:495` のコメント | 🟡 | 📖 |
| **G5** | I4, I1 | **federated 集約の分散経路が偽の E_OK を返す**。`dtk_fl_aggregate` の distributed 分岐は delta を `arg0..2` に詰めた後 `(void)` で捨て、`dtk_cre_tsk` を投げるだけで**集約も適用もせず** `E_OK` を返す。FedAvg は実際には起きていないのに成功を名乗る（嘘の OK）。README §4.2 は loss スタブを認めるが、この**集約 API の沈黙の成功**は別件。 | `arch/common/fedlearn.c:165-184`(`(void)r;(void)arg0..2;` の直後 `return E_OK;`)、ヘッダ `fedlearn.c:8`「aggregator node (node 0)」 | 🟡 | 📖 |
| **G6** | I1, I12 | **ノード ID と relay 順序が静的トポロジ**。UMP は `PKERNEL_NODE_ID` env（1..255）と `PKERNEL_RELAY` の**順序付き静的リスト**で同一性とトポロジを固定する。decentralized-lookup/survival は「ID は churn で動的に割り当て」を前提にするが、ID は人間が外から固定。relay-ha は「全ノードが同一順序リストを持つ純関数」で収束＝**静的設定が疑似中央**（リスト先頭=暗黙の優先点）。動的 ID 割り当て・自己編成トポロジは未実装。 | `arch/linux/aarch64/net_relay.c:388-395`(env ID)、`:96-98,426`(`relay_list[]` 順序＋`cur_relay=0` 先頭優先)、`relay-ha.md:9-15` | 🟡 | 📖 |
| **G7** | I14 | **node ID > DNODE_MAX は黙って群れから消える**。`drpc` 内部 ID は `mac[5]-1`（mac[5]≤DNODE_MAX のときだけ `drpc_init`）。ID が 33+ のノードは relay には REGISTER できる（relay 側 `NODE_MAX=256`）が、`drpc_init` が走らず "single-node mode only" になり、ピア側も `src_node>=DNODE_MAX` で破棄。**トランスポートには居るのにクラスタ論理には不在**という割れ方をする。`usermain` は1行メッセージを出すので「完全な無音」ではないが、relay 越しの相手からは理由不明の不参加に見える。 | `arch/linux/aarch64/usermain.c:200-208`(mac[5]≤DNODE_MAX 判定)、`relay/relay.c:39`(`NODE_MAX 256`)、`pmesh.c:245`/`swim.c:224`(`>=DNODE_MAX` で破棄) | 🟡 | 📖 |
| **G8** | I2, I1 | **`Status: FIXED` (最小, wave 10)** — coordinator の死＝その region の欠損として G2 の degraded 分母に正直計上 (rsum が来なければ degraded)。ストレッチ (次点が in-flight rsum を肩代わり) は未実装で残置。<br>**rsum 集約は coordinator に実質的特権がある**。region 要約 `rsum/<rid>` を出せるのは region coordinator（最小 ID）だけ。死ねば次の最小 ID が引き継ぐが、**in-flight な推論の最中に coordinator が死ぬと、その region の寄与は失われ G2 により無音**。death-piercing は「起点に特権なし」(I3) は潰したが、**coordinator の rsum 役割**という別種の特権は残置。survival kill_one はこれを踏まない（単一 region 構成なので coordinator 死＝起点死に縮退）。 | `dkva.c:213-267`(coordinator のみ `coordinator_aggregate`)、`dkva.c:521`(`region_coordinator()==drpc_my_node` ゲート)、`dkva.c:377`(旧: requester は他 region coordinator を待つが期待集合に入れない)。**修正: coordinator 欠損を degraded 分母へ (G2 と一体)** | 🟡→✅ | ✅ |
| **G9** | I4 | **kdds_pub の fanout は配送ではなく試行を数える**。REGION/GLOBAL 配送ループは `pmesh_send` の戻り値を見ずに `fanout++`。`pmesh_send` が no_route で落ちても（`pmesh_stats.no_route++` するだけ）、kdds 側は送ったことにする。`kdds_pub_fanout()` を観測指標に使うと、実際に届いた数を過大報告する（無音 drop）。 | `kdds.c:236-242`(`pmesh_send(...)` 戻り値無視、無条件 `fanout++`)、`pmesh.c:320-322`(no_route で `-1` 返却) | 🟢 | 📖 |
| **G10** | I2, I1 | **heal の後継選出が局所ビュー依存で割れうる**。heir = 各ノードの**ローカル** `dnode_table` で見た ALIVE 最小 ID。eventual-consistent な membership がずれている窓では、複数ノードが「自分が heir」と判断（二重起動）または誰も heir にならない可能性。decentralized-lookup §1.2 が HRW で扱ったビュー drift 問題を、heal は素朴な最小 ID で踏む（drift 緩和なし）。 | `heal.c:114-126`(局所 `dnode_table` 走査・最小 ID)、`heal.c:111`(`heal_triggered[]` も局所) | 🟢 | 📖 |
| **G11** | I8 | **時定数・閾値の根拠が思想に接続されていない**。world pressure の基線 60/35/15、moe の各定数、DKVA `INFER_TMO 600`/`RSUM_WIN 200`、relay-ha `5s/3s/10s` 等が散在し、§8 の「反射 tick:熟慮 tick の比」理論（reflex §7-5 が解析的裏付け無しと自認）と数値が結ばれていない。発振の安定限界はシミュレーション未実施（G3）なので、いずれも経験則。 | `world.c:131-135`(60/35/15)、`dkva.h:44-45`(600/200)、`net_relay.c:83-85`(5000/3000/10000)、`reflex-deliberation.md:292-295`(比の決め方=未解決) | 🟢 | 📖 |

---

## 2. 逆に、思想に忠実にできている点（公平のために）

過去レビュー文化に倣い、**守れている不変条件**も明記する。これらは「謳い文句」ではなく
コードで実際に守られているのを確認した。

- **I1/I7 — world.c の NO-CENTRAL が本物**: 集約専用ノードの概念がコードに無く、`world_task`
  は全ノードで対称。各ノードは「自分が受け取ったビーコンだけ」から world-table を作り、
  `world_peer_pressure()` は局所テーブルを読むだけで全網の真実を集約する場所が無い
  (`world.c:8-14, 208-227`)。moe の `select_expert` は broadcast score を「負荷の真実」として
  使わず world ビーコンの局所勾配＋自己観測 `recent_pick[]` を読む (`moe.c:217-219`)。
- **I3 — 起点の特権除去が実装されている**: `dkva_infer` はノード ID 前提を持たず、`dkva_cmd`
  は引数から決定論的に Q を合成するので「どのノードから発行しても同じ問い」になる
  (`dkva.c:649-658`)。survival kill_one シナリオ B が「起点を殺す→生存者が同じ問いを完遂」を
  実アサート (`kill_one.sh:147-166`)。
- **I4 — 同 region 内の degraded は誠実**: G2 の穴はあるが、**自 region に閉じた範囲**では
  fan-out 時の期待集合を取り、DEAD 遷移を期待から外し、欠けたまま完遂すれば `degraded (k/n)`
  を必ず出す (`dkva.c:333-429`)。CI の survival-loop が毎 push これを回す。
- **I5 — 古さの明示が徹底**: world map は age>STALE を `stale(Ns)`、SWIM DEAD を `DEAD`、
  pressure を `[ unknown ]` で描き「this view is local & may be stale/incomplete by design」と
  自己申告 (`world.c:388-420`)。lookup キャッシュも TTL で stale を捨て純 HRW に戻る
  (`lookup.c:325-379`)。
- **I9 — SOLO 経路常在**: `moe_infer` は expert==自分 or node id 未確定でローカル `mlp_forward`
  にフォールバック (`moe.c:335-339`)。単体実機で確認（`moe ...` → `reflex local-only` → `[local]`）。
- **I11/I12/I13 — HRW が cross-ABI 決定的で嘘をつかない**: `lookup.c` は digest 先頭8バイトを
  明示バイト取りで固定幅化し、`_Static_assert` で LP64 罠を塞ぐ (`lookup.c:45-89`)。
  実機で `hrw`/`hrw-l1` PASS、既知ベクタ `4 7 3 1 2 6 0 5`、一ノード drift で top-2 が 16/16 交差を確認。
  decentralized-lookup §4.2 G2「嘘の成功は返さない」の精神どおり、解決経路は所在のみ返す設計。
- **I10 — region は論理層**: `region.c` は SWIM の ALIVE 集合を RTT で部分集合化するだけで
  relay/物理トポロジに依存しない (`region.c:45-71`)。
- **lockstep の対称性**: `arch/linux/{aarch64,x86_64}/net_relay.c`・`net_unix.c`・`net_dispatch.c`
  はヘッダコメント1行を除き **diff ゼロ**（実測）。片肺実装の兆候なし。
- **relay v2 のセキュリティは本物**: HMAC-SHA256＋nonce sliding window＋鍵なし起動拒否、
  drop を全種ログ (`relay/relay.c:379-453`)。G4 はその**クライアント受信側**の話であって relay 本体は堅い。

---

## 3. 推奨順位（修正提案は各1行・実装はしない）

1. **G2（cross-region degraded の無音）🔴** — `rsum` にも期待集合を持たせ、欠けたら degraded に
   算入する（k/n の分母に生存他 region を含める）。**正直さ規約 I4 の穴で最優先**。
2. **G3（§7/§8 の無テスト）🔴** — reflex D0 のホスト純シミュレーションを1本足し、NO-CENTRAL と
   発振抑制を CI で数値アサートする。思想の核を無検証のまま置かない。
3. **G1（単一 Q トピック）🔴** — Q を per-source（`dtr/dkva/q/<origin>`）化し §5 の同時多発を
   構造的に解禁する（resp で既に証明済みの手法の横展開）。
4. **G8（coordinator の rsum 特権）🟡** — coordinator 死亡時に次点が in-flight rsum を再発行する
   縮退経路を足す（decentralized-lookup §2.2 の縮退経路と同型）。G2 と一体で設計。
5. **G4（クライアント HMAC 未検証）🟡** — `net_relay_recv` で受信フレームの v2 MAC を検証する。
6. **G5（fedlearn 偽 OK）🟡** — 分散集約が未実装なら `E_OK` ではなく `E_NOSPT` を返す。
7. **G6/G7（静的トポロジ・ID 溢れ）🟡** — 動的 ID 割り当ては R3/Phase D の本丸として明示し、
   ID>DNODE_MAX は relay REGISTER 段で拒否して理由を返す。
8. **G9/G10/G11 🟢** — fanout を配送成功で数える／heir 選出に HRW を流用／時定数比を D0 で実測。

---

> この監査は乖離の**地図**であって判決ではない。守れている点（§2）の厚さが示すとおり、
> 骨格は思想に忠実である。残る乖離の多くは「配管は通ったが、正直さ（I4）と同時多発（I6）と
> 検証（性質テスト）が水量（R3）を待たずに先回りで破れている」一点に収斂する。
> —— 迷ったら `survival-network.md` へ戻る。

---

## 修正ステータス（2026-06-07 第10波）

監査の置き土産を並列ワークフローで一掃。各 G の現在地（master 反映済み分）:

| # | 重さ | 状態 | 修正 |
|---|---|---|---|
| **G3** | 🔴 | **FIXED** | §7/§8 の核に4本の性質テスト（`moe test`）+ CI grep。NO-CENTRAL（各ノードが局所勾配で別 expert を選ぶ）/ 二層時定数のローパス性 / **発振=単一時定数28〜39切替 vs ヒステリシス4〜5切替**を数で実証。reflex-deliberation.md D0 を DESIGNED→DONE。 |
| **G4** | 🟡 | **FIXED** | `net_relay_recv` が受信フレームの v2 HMAC を定数時間比較で検証→不正は破棄（`mac drop` カウンタ）。正規トラフィックは無損失。`run_relay_forgery.sh` で偽造注入→破棄を実証。 |
| **G5** | 🟡 | **FIXED** | fedlearn の distributed 集約が偽 `E_OK` をやめ `E_NOSPT` を返す（嘘の成功を消す）。 |
| **G7** | 🟡 | **FIXED** | ID 範囲外・`>DNODE_MAX` の無音脱落を診断ログで可視化（relay 側 + `net_relay_init`）。※カーネル側 usermain の skip は follow-up。 |
| **G9** | 🟢 | **FIXED** | `kdds_pub` の fanout は `pmesh_send`==0 のときだけ加算。no_route は別カウンタへ。観測指標が配送数を過大報告しない。 |
| **G1** | 🔴 | **FIXED** | 真の同時多発: Q を per-origin `dtr/dkva/q/<origin>` 化、RESP_PKT に origin、responder が時間多重で再発行 → 2〜3起点が同時完遂（`concurrent_infer.sh` A）。 |
| **G2** | 🔴 | **FIXED** | 全 region を数える正直な degraded: 期待集合に他 region coordinator を追加（`world_peer_region()`）、欠損は `degraded (k/n)` に計上（同 B: 他 region 全滅で `degraded (2/3)`）。 |
| **G8** | 🟡 | **FIXED**(最小) | coordinator 死＝その region 欠損として G2 の degraded 分母へ正直計上。次点の rsum 肩代わりはストレッチで残置。 |
| **G6** | 🟡 | 据え置き | 動的 ID 割当・自己編成トポロジは R3/Phase D の本丸として明示。 |
| **G10/G11** | 🟢 | 据え置き | heal の後継選出 drift / 時定数の理論接続。後続。 |

### follow-up（第10波で表面化、未統合）

- **G4+ 受信側リプレイ窓**: honesty は受信 HMAC を検証するが、**正規フレームの再送（replay）** は素通る（MAC は再送でも有効）。並列で走った重複隊 `w10-honest-defense` (branch 温存) が per-source 64-packet スライディング nonce 窓を受信側に実装済み（`rx_nonce_max/win/armed[256]`、relay.c の `replay_check_and_update` と同一ロジック）。honesty の上に小さく移植して取り込む価値あり。監査 G リスト外のボーナス発見。
- **G7 残**: カーネル側 `usermain.c` の `mac[5] <= DNODE_MAX` 無音 skip（net_relay_init では可視化済みだが usermain は別経路）。
