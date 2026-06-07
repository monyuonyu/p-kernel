# 思想⇄実装 乖離監査・第4版 — philosophy-gap-audit-4

> 常設の批判器官、第4版。第1版 (G1–G11)・第2版 (G12–G19)・第3版 (G20–G26) を承ける。
> 第3版の評決は明快だった: **「環は load について閉じたが threat については閉じていない。
> G20(符号の倒錯) は晴れず、むしろ実証された。第13波の本丸。」**
> 第13波 A隊がいま G20 を実装中(`pressure` を負荷軸/脅威軸へ分離)。第4版の任務は
> celebration ではない。次の問いに答える:
>   1. **G20 の実装は本物になりうるか** — 変数を rename しただけの偽修正と本物の
>      修正を分ける、A隊の成果を検証する具体的な受け入れテストは何か。
>   2. **検証器そのものを検証する** — `tools/sim/survival_gating_sim.py` は
>      moe.c が出荷する系を検証しているのか、それとも別の系を検証しているのか。
>   3. **次に弱い接合部** (G27+) を、すべて file:line で。
>
> 修正はしない。地図を作る。
>
> 監査ブランチ: `w13-audit-v4` / 対象: master `81168b9`(**第12波統合後・第13波 G20 着手前**) / 2026-06-07
> 実証環境: aarch64 (Termux proot)。コード読み + CI 設定の精査。
> 既知 (G1–G26) は**参照のみ**。第4版は G20 の検証法と G27+ を主題にする。

---

## 0. 一行サマリ（評決を先に）

第12波で **load 軸の環は本物に閉じた**(`reflex.c:544-596` の `lp_run` は仕組まれた式
でなく実因果の負帰還)。だが思想の最独自点 **§2「守るべき一点へ全網の力を注ぐ」は、
コードでは依然 (a) 対象オブジェクト不在、(b) 力学が逆符号** のまま。第13波の G20 修正は
正しい方角だが、**本物になる条件は厳しい**(下 §1)。そして本書が新たに見つけた最重要の
緊張は: **「脅威」がそもそも環境から接地されていない**(温度バケツの言い換え, 無アクチュ
エータ; G28) ことと、**唯一動いているオンライン適応(G18 learned_conserve)が逆符号の逃走
ループのゲインを最適化している**(G29) こと。**G20 の符号を正しく反転させても、接地され
ていない脅威・アクチュエータの無いプラントに対して反転するだけ**になりうる。

---

## 1. G20 の検証 — 偽修正と本物の修正を分ける受け入れテスト

### 1.1 master 現在の符号(A隊 着手前)を file:line で確定

`expert_utility` は単一スカラ `pressure` を **一律ペナルティ**として引く:

```
arch/common/moe.c:155-163  expert_utility()
  moe.c:160   u -= (W)((eff_pressure * MOE_PRESS_NUM) / MOE_PRESS_DEN);   /* 一律減点 */
```

その `pressure` に、脅威観測時の reflex CONSERVE が **自ノードの値を底上げ**する:

```
arch/common/world.c:134-156  compute_pressure()
  world.c:149   UW p = (UW)base + (UW)fires * 12u;
  world.c:154   p += (UW)reflex_pressure_bias();   /* CONSERVE 中=脅威観測中に自圧↑ */
arch/common/reflex.c:301-308  reflex_pressure_bias() → learned_conserve (既定 40)
```

帰結(第3版 G20 のまま、master で不変):**脅威を観測したノードは自分の pressure を上げ、
`expert_utility` で自他から避けられる**。`select_expert` は self も候補に含む
(`moe.c:244-248`)ので、threatened node は**自分の緊急推論を遠方へ送り出す**
(`moe.c:365-384` の remote 経路、§8 が禁じる光速遅延を足す)。**「注ぐ」の真逆 = 「逃がす」。**

> したがって master `81168b9` 時点で **G20 は 1 文字も修正されていない**。pressure は
> 単軸、符号は逆。第13波の作業はこれを変える。

### 1.2 偽修正(FAKE)はこう見える — A隊の成果を鵜呑みにしない判定基準

| # | 偽修正の手口 | 見破り方 |
|---|---|---|
| F-a | `pressure` を `load_pressure` + `threat_pressure` に **rename** するが、`expert_utility` は両方を同符号で引く(`u -= load - threat` でなく `u -= (load+threat)`) | live 経路 `moe.c:160` を読む。**減算が 1 本のまま**なら偽。本物は load を引き threat を**加える**(あるいは別経路で threatened node を recipient に昇格) |
| F-b | threat 軸を足すが、それが効くのは **threatened node 自身の self-utility だけ**で、他ノードが threatened node へ**寄る**ルーティングは変わらない | 「他ノードの選択が threatened node へ集束するか」を測るテスト(下 Test-2)。self だけ動く実装は Test-2 で落ちる |
| F-c | 緑が **sim 由来**(別の系, §2)か、**外乱の自然減衰**由来(G21)で、live routing が証明していない | テストが**本番 `expert_utility`/`select_expert` を直接呼ぶ**こと(既存 self-test の作法, `moe.c:593` `expert_utility` 直呼び)。sim の PASS は受け入れない |
| F-d | doc に「G20 FIXED」と書くが、`moe.c:160` の `-= pressure` は手つかず。closed-loop の `lp_run`(`reflex.c:544`)も load プラントのまま | doc でなくコード。`lp_run` に「**helper が寄って P の backlog を引き下げる(rally)**」項が入ったかを読む。入っていなければ閉じたのは依然 load 環 |

### 1.3 本物(REAL)の必要条件と、A隊が通すべき具体テスト

**前提(API)**: 効用関数を **load 軸(混雑→減点)と threat 軸(危険→加点 or recipient 昇格)
の二入力**にし、live `select_expert` と self-test が**同一関数**を呼ぶ(重複定義を作らない —
既存 `expert_utility`/`ewma_step`/`deadband_pick` と同じ規律, `moe.c:200-213` のコメント)。

CI が grep する `[g20-*] PASS` 行として、以下を `moe_self_test` に足すこと(`moe test` 経由)。

- **Test-1 反対符号(核)**: 同 acc/RTT/region の 2 候補 A,B。A は load=0, threat=T。B は
  load=T, threat=0(同じ大きさ)。「群れの応援(reinforcement)を誰へ注ぐか」の決定は **A を
  選ぶ**こと。同時に「自分の余剰を誰へ逃がすか」の決定は **B を避ける**こと。
  → 単軸実装(F-a)では A と B が同値になり **A を選べず FAIL**。これが rename-only 検知。
- **Test-2 単調集束(力が脅威へ注ぐ)**: 保護点 P を固定し、threat_P を 0→大へ掃引(load_P
  固定)。P へ reinforcement を回す helper 数(or P の help-utility 順位)が threat_P に対し
  **単調非減少**であること。現行/逆符号コードでは**非増加**になるので、**符号が反転した事実**
  を数で示す。
- **Test-3 §8 局所性ガード**: threatened node P が**自分の**緊急推論を遠方へ offload しない
  こと。self の threat を上げても self の remote-offload 確率が**上がらない**。
  → `moe.c:244-248`(self も候補)+ `world.c:154`(自己観測に CONSERVE 反映) の病理を捕える。
  現行コードは CONSERVE→自圧↑→self utility↓→**遠方 offload** で FAIL。
- **Test-4 閉ループのプラント符号(rally vs flee)**: `reflex.c:544` の `lp_run` を拡張し、
  threat 下では**近傍の余力が P へ流れ込んで** P の backlog(脅威源)を引き下げる項を持つ。
  受け入れ: threatened-but-not-overloaded の P の dwell が**「helper が来たから」**縮む(P が
  自分の仕事を隣へ捨てたからではない)。同時に load だけ高い対照ノードは**動員されない**。
  → 「閉じた環が制御するのは threat、actuator は rally」を初めて数で示す(G21-for-threat を晴らす)。
- **Test-5 CI 配線**: 新 `[g20-*] PASS` を `.github/workflows/ci.yml:52-60` の grep 群へ
  追加。**doc/sim だけの修正は受け入れない**。

**REAL の要約判定**: live の `select_expert` 経路の効用に **load と threat の反対符号の二項**が
あり、threatened node が **本番関数を呼ぶ CI テストで reinforcement の recipient として
選ばれ**、§8 self-offload 病理が消えていること。これら無しの「G20 DONE」は**偽**。

---

## 2. 検証器を検証する — sim は kernel と同じ系か？（モデル⇄成果物の不一致）

`tools/sim/survival_gating_sim.py` は commit `81168b9` で **"G20 design oracle"** と銘打って
追加された。だが定数とダイナミクスを突き合わせると、**sim は moe.c が出荷する系を検証して
いない**。これは「モデルと成果物が食い違う」典型的リスクである。

### 2.1 定数・制御則の対応表(実コード)

| 項目 | sim (`survival_gating_sim.py`) | kernel (`moe.c`/`moe.h`/`reflex.h`) | 一致? |
|---|---|---|---|
| pressure の定義 | `load_i - cap_i`、**非有界**(line 122) | 抽象スカラ **0..100 にクランプ**(`world.c:155`)。load−cap ではない | ✗ |
| cap / k ゲイン | cap=10(line 94), 拡散ゲイン k∈{0.05,0.16,0.55}(line 290) | **対応物なし**。kernel に拡散ゲイン k は存在しない | ✗ |
| 制御則 | **連続ラプラシアン拡散**: `p_i += k·Σ(p_j−p_i)`(line 118-137)。連続場の負荷を勾配で流す | **離散 argmax + ヒステリシス**: 1 推論ごとに最良 utility のノードへ委譲(`moe.c:222-312`) | ✗ |
| ヒステリシス/デッドバンド | **なし**(素の拡散) | `MOE_SWITCH_MARGIN=12`(`moe.h:117`), EWMA α=1/4(`moe.h:116`) | ✗ |
| ダンピング | k を下げる(連続) | `recent_pick`(自己仮想負荷 +25/pick, 減衰 2/3; `moe.h:96-98`) | ✗ |
| 発振の機序 | **拡散オーバーシュート**(k 過大, line 289) | **群れ argmax 殺到**(`moe.c:707-765` `st_herd`)。kernel の発振は sim に無い機序 | ✗ |
| accuracy / RTT / region 項 | なし | utility の主要項(`moe.h:57-81`) | ✗ |

### 2.2 二つの致命的な不一致

1. **別の力学系である。** sim は「連続負荷場のラプラシアン拡散(ゲイン k)」、kernel は
   「個別推論の argmax-with-hysteresis ルーティング」。sim の k は moe.c に対応物が無く、
   kernel の `MOE_SWITCH_MARGIN`/`MOE_PICK_LOAD`/EWMA/accuracy/RTT は sim に無い。**sim が
   「収束する/発振する」と示しても、それは kernel の収束/発振の証明にならない**(機序が違う)。
   kernel の発振は `st_herd`(殺到 argmax)由来で、sim の拡散オーバーシュートとは別物。

2. **そもそも threat 軸をモデルしていない = G20 oracle として的を外している。** sim が流す
   のは pressure(=load−cap)の勾配を下る**負荷だけ**(line 129 `pres[j] < pi`「自分より楽な隣へ」)。
   これは第3版が「**load については既に環が閉じている**」と認めた、まさにその load 軸の再現で
   ある。**sim は threatened point へ力が注がれる挙動(§2 = G20 の核心)を一切モデルしていない。**
   よって `81168b9` の commit message「G20 design oracle」は**誤称**で、実体は §6/§7 の
   **負荷分散オラクル**。G20 が争っている脅威軸については、sim は沈黙している。

> **判定(sim vs kernel 定数ドリフト)**: sim は kernel と**別の制御則・別の pressure 定義・
> 別の発振機序**を持ち、しかも **G20 の対象である脅威軸を含まない**。sim の緑は kernel の
> G20 修正の証拠に**ならない**。A隊の G20 は §1.3 の **本番関数を直叩きする in-kernel テスト**
> でのみ検証すべきで、sim を oracle にしてはならない。sim は「負荷拡散の直観を絵で見る」
> 研究補助として価値はあるが、**"design oracle" の看板は降ろすべき**(doc/コメントの是正で足りる)。

---

## 3. G22 の現況 — 「熟慮」はオンライン学習か、適応則か

**依然オンライン学習ではない(G22 そのまま OPEN, 🟡)。** master `81168b9` で:

- 分散 FedAvg は依然 `E_NOSPT`(`arch/common/fedlearn.c:180-184`、正直な未実装表明)。
- 局所学習 `fl_local_train` は依然 **635 パラメータ中 b3 バイアス 6 個**のみ有限差分
  (`fedlearn.c:99-120` の `for j < MLP_OUT`)。重み本体 w1/w2/w3 は touch しない。
- G18 の `reflex_deliberate`(`reflex.c:317-348`)は**重みを 1 つも動かさない**。動かすのは
  `learned_conserve` という**反射ゲインのスカラ 1 個**(`reflex.c:327-339`、`REFLEX_LEARN_STEP=6`
  刻み, `[MIN,MAX]=[8,80]` クランプ)。これは「経験から自分を書き換える考える器官」(§9)では
  なく、**観測駆動のホメオスタット(事前固定の適応則の実行)**。第12波評決の「事前適応則の
  実行に近いが観測駆動である点は本物」という評価は妥当だが、**§9 の学習ではない**。

→ **G22 は学習でなく適応則。`learned_conserve` という 1 スカラの自動調律に留まる。** さらに
これは下記 G29 の温床になる(調律対象が逆符号ループのゲイン)。

---

## 4. G23 の現況 — federation は コードに配線されたか

**doc-only のまま(G23 OPEN, 🔴)。** `docs/architecture/federation.md` は第12波 G隊が
**設計だけ**を書いた(自身が「1 行のコードも変えていない」と明記, `federation.md:284-292`)。
master で確認:

- `DNODE_MAX = 32` は不変(`arch/common/include/drpc.h:35`)。
- `arch/common/*.c` に federation/複合 ID/上位メッシュ/coordinator-mesh の**配線は皆無**
  (grep: 該当ゼロ)。`region.c` は依然 32 を**分割**するだけ。
- 複合 ID (region_id, local_id)、葉間 rsum 昇格、疎構造化 — **すべて未実装**。

→ **「数千ノード」の看板に対し、論理層は絶対上限 32。橋は設計図だけで、橋桁は無い。**
federation.md は良い地図だが、F0(コード不変の足場確認)すら**コードには痕跡が無い**。

---

## 5. G25 の現況 — 監査の死角は閉じたか（locality 計測の評価）

第12波 F隊が `samples/24_locality/` + `docs/benchmarks/locality.md` で **G25 を部分的に閉じた。**
正直に分けて評価する。

**閉じた部分(本物・🟢)**: §4「locality = 遠方通信を減らす」の**前半**を初めて数で出した。
クラスタ総 kdds メッセージ ~1630(ON) vs ~4677(OFF) ≈ **2.9× 削減**、relay DATA ~0.96MB vs
~2.0MB ≈ **2.1× 削減**(`locality.md §2b`)。これは「精度と生存だけ」だった監査に**初めて
trafficの数字**を入れた。しかも benchmark 自身が「合否でなく計測」「主張が偽でも数字を出す」
という death-piercing 精神を守り、**(C) 要求ノード単体は中立**という非自明な負の所見まで
正直に記録している(`locality.md:75-81`)。

**まだ閉じていない部分(🟡)**:

1. **§4 の核心(光速/遅延)は未測定。** RTT zone ペナルティは region 形成のため**観測 RTT を
   水増しするだけ**で実遅延を注入しない(`locality.md:83-87` が自認)。§4 の存在理由「光速の
   壁への答え」の **latency 半分は本ハーネスでは検証されていない**。
2. **エネルギーは byte-proxy であって joule ではない**(`locality.md:93-110`)。`K=5` は order-
   of-magnitude 仮定。実電力計は無い。
3. **実機ゼロのまま。** 全計測が **N=4 localhost**。RPi/ベアメタルの survival 実走は依然
   監査対象外。CI の bare-metal は build-only(`ci.yml:14,96`)で、locality benchmark も
   **CI に配線されていない**(`ci.yml` に locality の grep 無し = 回帰防御ゼロ)。
4. **規模が toy。** N=4 では capacity(N)・O(N²)→O(region²) のスケール曲線は出ない
   (`locality.md:136-138` が TODO 列挙)。

→ **G25 は traffic/energy-proxy について部分的に閉じた。だが §4 の核心(光速)・実機・joule・
規模は依然未測定。** 監査の死角は「広さ」が縮んだが「最も深い穴(latency と実機)」は残る。

---

## 6. 新規・残存乖離 G27+

重さ: 🔴 思想の核に反する／🟡 思想を弱める／🟢 軽微。実証: ✅ 実機/CI　📖 コード読み

| # | 不変条件 | 乖離の内容 | 証拠 (file:line) | 重さ | 実証 |
|---|---|---|---|---|---|
| **G27** | (メタ) I7 | **検証器が別の系を検証している + 唯一の "G20 oracle" が脅威軸を含まない。** sim(`survival_gating_sim.py`)は連続ラプラシアン拡散(ゲイン k, pressure=load−cap 非有界)で、kernel の離散 argmax+ヒステリシス(MOE_SWITCH_MARGIN=12, EWMA, recent_pick, accuracy/RTT)とは**別の力学系・別の発振機序**。しかも sim は load 勾配しか流さず **threat 軸を一切モデルしない**ので、"G20 design oracle"(commit `81168b9`)は誤称(実体は §6/§7 負荷分散 oracle)。さらに **`reflex test`(G17/G18 の閉ループを"証明"する self-test)は CI で一度も呼ばれない**(`ci.yml:52` の printf は `moe test` のみ)。= 「環が閉じた」と謳う最重要テストが回帰防御の外。 | sim: `survival_gating_sim.py:118-137`(拡散), `:122`(load−cap), `:290`(k 値) vs `moe.c:155-163,222-312`/`moe.h:96-98,116-117`。CI: `.github/workflows/ci.yml:52`(reflex test 不在), `:57-60`(moe のみ grep) | 🔴 | ✅(CI/コード) |
| **G28** | I19, I20, I21 | **「脅威」が環境から接地されていない — センサもアクチュエータも無い。** threat_class の唯一の源は `gate_predict`(`moe.c:107-114`)= **温度を固定しきい値で 3 バケツに割るだけ**。reflex はこの「温度バケツ」を「脅威レベル」と再解釈する(`reflex.h:33-37`)。一方、脅威を**縮める actuator は存在しない**(SHIELD=取込拒否, CONSERVE=負荷偏向, BEACON=警報 — どれもセンサ/環境を変えない)。よって **threat ループは構造的に開いている**(G21 の threat 版)。§2 の「守るべき一点(protected unit)」もオブジェクトとして不在(grep: protect は memory-protect のみ)。**G20 で符号を正しく反転させても、群れの力は「温度計が >35 を指したノード」へ注がれるだけ**で、何かを守る一点へ注がれるのではない。 | `moe.c:107-114`(threat=温度バケツ), `moe.c:394`(reflex へ result_class を渡す), `reflex.c:159-223`(act_table の行動は入力を変えない), `reflex.c:269-287`(解除=タイマのみ, G21 のまま threat 側で OPEN) | 🔴 | 📖 |
| **G29** | I18, I19 | **唯一動くオンライン適応が、逆符号の逃走ループのゲインを最適化している。** G18 `reflex_deliberate` が学習(nudge)する `learned_conserve`(`reflex.c:317-348`)は、CONSERVE の効き = **自ノードの pressure 上昇量**(`world.c:154` で加算)。脅威が長く滞留すると効きを**上げる**(`reflex.c:327-331`)= threatened node を群れから**より強く避けさせる**方向(G20 の逆符号を**強化**)。「考える器官の学習」(§9)が、§2 と**逆向きの目的関数(逃走をうまくやる)**を最適化している。A隊が G20 の符号を反転しても、`reflex_deliberate` の nudge 方向(dwell が長い→効きを上げる)が同じままなら、**学習器が修正を押し戻す**。G20 修正は `learned_conserve` の意味論再設計と**同時に**行わねば整合しない。 | `reflex.c:317-348`(dwell↑→conserve↑), `world.c:154`(conserve→自圧↑), `moe.c:160`(自圧→自他から回避), `reflex.h:71-87`(学習対象が反射ゲイン 1 スカラ) | 🔴 | 📖 |
| **G30** | I4 | **confidence ゲートが §4/§7 中核経路で死んでいる。** `moe_infer` は reflex を **必ず confidence=0xFF で呼ぶ**(`moe.c:394`、コメントも「未知 → 0xFF(ゲートを通す)」)。reflex 側の `REFLEX_CONF_MIN=40` ゲート(`reflex.c:205`)は 0xFF を素通しさせる設計なので、**moe 経路の全推論が confidence に関わらず反射を発火**する。「低確信では行動しない」(§8 慎重化)は dtr 経路でしか効かず、最も発火頻度の高い moe 経路では無効。低確信の誤推論が CONSERVE/BEACON を起こし、G20 の逆符号病理を**確信度に関係なく**撒く。 | `moe.c:394`(0xFF 固定), `reflex.c:204-205`(0xFF は素通し), `reflex.h:62-63`(CONF_MIN の意図) | 🟡 | 📖 |
| **G31** | (メタ) I21 | **G25 の部分閉鎖が回帰防御されておらず、§4 核心は未測定のまま。** locality benchmark は traffic/energy-proxy を数で出した(本物, §5)が、(a) §4 の核心 **latency/光速は未測定**(RTT は水増しのみ, `locality.md:83-87`)、(b) energy は **joule でなく byte-proxy**(`locality.md:93-110`)、(c) **実機ゼロ・N=4 localhost**、(d) **CI 非配線**(`ci.yml` に locality grep 無し)。= 監査の死角(G25)は「広さ」が縮んだが「最深部(遅延と実機)」は残り、しかも縮んだ分すら回帰で守られていない。 | `docs/benchmarks/locality.md:83-87,93-110,127-139`, `samples/24_locality/`, `.github/workflows/ci.yml`(locality 不在), `ci.yml:14,96`(bare-metal=build only) | 🟡 | ✅(CI/コード) |

---

## 7. 公平の節 — 第12波で前進した本物

判決でなく地図。コードで実際に前進した点を明記する。

- **load 環は本物に閉じた。** `lp_run`(`reflex.c:544-596`)は外乱→知覚→本番 §7 ゲート
  (`moe_expert_utility`)→負荷 offload(`L -= LP_SHED`)→減衰、という**実因果の負帰還**で、
  仕組まれた式ではない。G21 は **load について晴れた**(actuator=offload が実在)。
- **NO-CENTRAL は依然本物。** `select_expert` は局所 world-table と自己観測のみを読み、
  全網集約点が無い(`moe.c:222-312`)。per-source topic(world/moe/reflex/dkva)で単一スロット
  集約点を作らない設計が一貫。
- **正直な未実装表明が一貫。** FedAvg `E_NOSPT`(`fedlearn.c:180-184`)、locality benchmark の
  「(C) 中立」「(D) latency 未測定」の自認(`locality.md:75-87`)、federation.md の「1 行も
  変えていない」(`federation.md:284-292`)。death-piercing 精神は健在。
- **G25 を初めて数で攻めた。** §4 の traffic 主張に 2.9× の実数を与えた(§5)。
- **二層の時定数分離は数で守られている。** `[moe-twolayer] PASS`(`moe.c:637-696`)は本番
  `ewma_step` のローパス性を検証し、CI で grep(`ci.yml:58`)。

---

## 8. 推奨順位（各1行・実装はしない）

1. **G20(符号の倒錯)🔴 — 第13波 本丸(進行中)。** §1.3 の Test-1〜5 を**受け入れ条件**に。
   特に **live `expert_utility` の二符号化**と **`reflex_deliberate` の nudge 方向再設計(G29)**を
   同時に。doc/sim だけの緑は不可。
2. **G28(脅威の未接地)🔴 — 次波の本丸候補(下 §9)。** §2 の protected-unit をオブジェクト化し、
   threat に actuator(rally で脅威源 backlog を実際に下げる閉路)を一つでも持たせる。
3. **G29(学習が逆符号を最適化)🔴** — `learned_conserve` の意味論を「逃走ゲイン」から
   「rally の受け入れ強度」へ反転。G20 と不可分。
4. **G27(検証器の不一致)🔴** — sim を G20 oracle から降ろす(doc 是正)。`reflex test` を
   CI(`ci.yml:52`)へ配線。G20 の新テストも CI grep へ。
5. **G23(federation 未配線)🔴** — F0(コード不変の足場)を実コードに刻む第一歩を R3/Phase D で。
6. **G31(G25 残)🟡** — relay v2 タイムスタンプで latency を実測(§4 核心)、locality を CI 配線。
7. **G30(confidence 死)🟡** — moe 経路の confidence を実値(max softmax×100)で渡す。
8. **G22(学習でなく適応則)🟡** — fl_local を b3 6 個から実重みへ、または status で正直に修飾。

---

## 9. 次波の本丸 — G28（脅威の接地）を推す

**G20 の次は G28 を本丸にすべき。** 論拠:

1. **G20 は G28 無しでは「絵に描いた符号反転」になる。** A隊が `expert_utility` を二符号化
   しても、threat の唯一の源は温度バケツ(`moe.c:107-114`)で、threat を縮める actuator は無い
   (`reflex.c` の三行動はどれも入力を変えない)。**群れの力は「温度計が >35 のノード」へ
   注がれ、そこに注いでも脅威は減らない**(開ループ)。§2 の「守るべき一点へ注ぐ」が意味を
   持つには、(a) **守る対象 = protected-unit のオブジェクト**、(b) **threat を実際に下げる
   閉路(rally で backlog/危険源を引き下げる)**が要る。これは第3版が「§2 だけが接地せず、
   しかも逆を向いている」と名指した核(`audit-3 §4.1`)の、**接地側**の本丸。

2. **依存順序が正しい。** G20(符号)→ G29(学習の符号)→ G28(接地)は同じ §2 の三面。符号を
   反転(G20)し、学習を整合(G29)させた**直後**に、注ぐ先を実在の対象にし actuator を付ける
   (G28)のが自然な積み上げ。G23(federation)や G31(計測)は重要だが §2 の核心ではなく、
   後回しでよい。

3. **「全部緑」を勝利と誤認しない歯止めになる。** G28 を本丸に据えると、次波は「protected-unit
   へ群れの力が集束し、その結果**脅威源が実際に縮む**」を §1.3 Test-4 形式の閉ループで
   数証明することになる。これは sim(別系)でも温度バケツでもない、§2 そのものの初めての接地。

> 対案として G27(検証器)を本丸にする手もあるが、G27 は G20 の**受け入れ条件の一部**として
> 第13波内で片付くべき配管であり、単独の波を割く重さではない。深さで選べば **G28**。

---

## 10. 評決（running system は思想にどれだけ近いか・正直に）

> **load については環が閉じた。threat については、環が閉じる以前に「脅威」が存在しない。**
> 第12波で `lp_run` の負帰還は本物になり、NO-CENTRAL・二層時定数・正直な未実装表明・§4 の
> 初めての数(traffic 2.9×)— これらは実コードの前進で、飾りではない。
> だが思想の最独自点 **§2** は依然、**(a) 対象オブジェクト不在・(b) 力学が逆符号(G20)・
> (c) 脅威そのものが温度バケツで未接地(G28)・(d) 唯一の学習が逆符号ゲインを最適化(G29)** の
> 四重に未達。そして**それを検証するはずの sim は別の力学系で、G20 の脅威軸を含まない(G27)**。
> 第13波の G20 は正しい方角だが、**符号を反転するだけでは、アクチュエータの無い開ループに
> 対して、温度計の読みへ向けて反転するだけ**になりうる。
> 前三版より楽観的になる理由は無い。むしろ第4版は、第12波の「全部緑」が **load の勝利を
> threat の勝利と取り違えさせる**こと、そして**検証器自身が間違った系・間違った軸を見ている**
> ことを、新たに突き止めた。
> —— 迷ったら `survival-network.md §2`(守るべき**一点**へ群れの力を**注ぐ**)へ戻る。
> いま running system が注いでいるのは、一点ではなく温度バケツであり、向きは逆である。
</content>
</invoke>
