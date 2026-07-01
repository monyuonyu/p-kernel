> **[歴史記録・凍結 / superseded by `gap-ledger.md`]** 本書は時点監査の歴史記録。いま master で開いている gap の正本は [[gap-ledger.md]] に一本化した。本文は当時のまま保存。`audit-9` は作らない（レビュー #5）。

# 思想⇄実装 乖離監査・第6版 — philosophy-gap-audit-6

> 常設の批判器官、第6版。第1版 (G1–G11)・第2版 (G12–G19)・第3版 (G20–G26)・
> 第4版 (G27–G31)・第5版 (G32–G35) を承ける。第5版の評決は鋭かった:
> **「符号は正された。だが『環が閉じた』と CI が言うとき、閉じているのは self-test の中だけだ。」**
> その後 第14波で G28/G32 が **live で閉じた** (`protect.c` / `samples/27_protect`、master tip
> `2e4381c` "Merge w14-ground-threat")。protect 軸の単一点ループは **本番ノードを kill しても
> object が生き残る** ところまで来た。これは本物の前進である (§8 公平の節)。
>
> 第15波 D隊 (本書) の任務は祝賀ではない。第15波の別隊が **§2∧§5 (G35) を いま実装中** である:
> 保護を **複数** にする — 同時多発の「守るべき点」を **並行に** 守り、各点が群れの力の自分の
> 取り分を引き、綺麗に分散し、中央も再直列化もない。本書は:
>   1. §2∧§5 が **REAL である受け入れテスト** を、commander へ CI-greppable な形で渡す。
>      **偽の plural (FAKE-plural) の 4 手口** を file:line で名指し、本物の pass を各々定義する。
>   2. G13 (coordinator 集約窓による再直列化) の現況を確定する — §5 に直接効く。
>   3. G33/G34 (反射の温度バケツ脅威源・死んだ確信度ゲート) の現況。plurality はこれを悪化させるか。
>   4. G22 (熟慮 ≠ 学習) の現況を定量化する — §8「全体が未来を強くする」がどれだけ空虚か。
>   5. §2/§5/§7/§8/§9 から、次の本丸 (G36+) を 1 つ選ぶ。
>
> 修正はしない。地図を作る。doc のみコミット。
>
> 監査ブランチ: `w15-audit-v6` / 対象: master `2e4381c` (**第14波 G28+G32 live 統合後**) / 2026-06-07
> 実証環境: aarch64 (Termux proot)。`make -C boot/linux` BUILD_OK、`moe test`/`protect test` 実走。
> 既知 (G1–G34) は**参照のみ**。第6版は **§2∧§5 (G35) の受け入れ条件** と G13 を主題にする。

---

## 0. 一行サマリ（評決を先に）

第14波で protect 軸の **単一点** ループは live で閉じた (`protect.c`、`[protect-loop-live]` CI)。
**だが §5 は実装でもテストでも *単一点のまま* である。** 本書が突き止めた構造的事実:

- **脅威ビーコンは依然 1 ノード 1 スカラ。** `protect_threat_level()` は 8 個の object を
  `worst`(max) へ畳み (`protect.c:105-115`)、`compute_threat()` は reflex 軸と protect 軸を
  さらに max へ畳み (`world.c:169-182`)、`WORLD_BEACON.threat` は単一バイトで配られる
  (`world.c:209` / `world_peer_threat` `world.c:267-271`)。→ **近傍の *計算* rally (§4/§7
  ゲート) は、同一ノード上の複数の守るべき点を区別できない。** これは第5版 G35 が予言した壁が
  まったく動いていないどころか、protect 層が入った今 **max 畳み込み = 隠れた中央調停 (FAKE-plural
  手口 b)** が本番経路に *実在* する、ということだ。
- **plural は本番経路でもテストでも一度も走っていない。** live demo (`samples/27_protect/run.sh`)
  は **常に object を 1 個しか宣言しない**(`run.sh:175,220`)。並行性を示す唯一の緑
  `[moe-concurrent] PASS` は **in-process の合成 self-test**(3 クラスを手組 utility で回す;
  `moe.c:807-861`)であって、live N-node multi-point ではない (FAKE-plural 手口 d — 第5版が
  「緑の self-test / 死んだ live 経路」と名付けた病の、まさに §5 版)。
- **データ複製の actuator だけは per-object** (`protect.c:230-269`)。つまり system は
  *半分だけ* plural である: **記憶 (図書館) の複製は並行できるが、計算 (考える器官) の rally は
  単一スカラへ潰れる。** §2 の「全網の *力* を注ぐ」のうち、注げているのは保存の力だけ。

そして G13(coordinator 200ms 集約窓; `dkva.c:233-269`,`639-640`,`DKVA_RSUM_WIN_MS=200`)は
**1 文字も変わっていない** — region 横断クラスタでは coordinator が問いを受けるたび同時多発が
200ms 凍る。G33/G34(反射の温度バケツ脅威源・死んだ確信度ゲート `moe.c:418-419`)も不変で、
**§5 が本物になるほど確信度ゲートの死は *悪化* する**(下 §4)。

---

## 1. §2∧§5 の受け入れテスト — 偽の plural と本物の plural を分ける

### 1.1 master 現在 (第15波 G35 着手前) を file:line で確定

| 部品 | 実体 | plural か |
|---|---|---|
| 守る対象の登録 | `objs[PROTECT_MAX_OBJS=8]`、各々独立の holder_count/target_r/drive_age (`protect.c:70-83`) | ✅ 8 点まで保持できる |
| 接地脅威 (per-object) | `protect_threat_for(replicas,R)`(`protect.c:94-103`) | ✅ object ごとに計算可 |
| **脅威の集約 (出力)** | `protect_threat_level()` が **worst=max** を返す(`protect.c:105-115`) | 🔴 **8 点 → 1 スカラ** |
| **ビーコン脅威軸** | `compute_threat()=max(rt,pt)`(`world.c:181`)→ `WORLD_BEACON.threat` 単一バイト(`world.c:209`) | 🔴 **さらに 1 スカラへ** |
| 近傍の受信 | `world_peer_threat(n)` が 1 値(`world.c:267-271`)→ `eff_threat(n)`(`moe.c:199-204`)→ ゲート加点(`moe.c:166,279`) | 🔴 **点を区別できない rally** |
| 複製 actuator | `protect_tick` が全 object をループし per-object に `pfs_repl_reannounce`(`protect.c:230-269`) | ✅ per-object・並行 |
| ANN/WANT 制御トピック | `KDDS_QOS_LATEST_ONLY` depth-1 単一スロット(`pfs_repl.c:581-591`) | 🟡 **N object で奪い合う** |
| live テスト | `run.sh` は object 1 個(`run.sh:175,220`)。並行は in-process 合成のみ(`moe.c:807-861`) | 🔴 **multi-point 未テスト** |

→ **結論: 守る対象の *容器* は plural(8 枠)。だが (1) 群れへ配る脅威信号が単一スカラへ畳まれ、
(2) live で多点を一度も回さない。§5「同時に数百件…それぞれに別々のエキスパート群が並行発火」は、
最良でも『データ複製は 8 並行できるが、計算 rally は 1 点しか指せない』半 plural にとどまる。**

### 1.2 偽の plural (FAKE-plural) はこう見える — D隊の成果を鵜呑みにしない判定基準

| # | 偽の手口 (task の (a)-(d) に対応) | 見破り方 (file:line) | 本物 (REAL) はこう |
|---|---|---|---|
| **F-a 直列化を並行に化粧** | N 点を 1 個ずつ順に守り、総時間 ≈ N×(単一)。ログだけ「concurrent」と書く | live で N 点を **同時刻に AT-RISK** にし、**全点の SAFE 到達の壁時計時間 ≈ 1 点ぶん (≈1×, ≤ ~1.5×)** であることを assert。N に比例して伸びたら偽。`protect_tick` は per-object に駆けるが ANN が `LATEST_ONLY` 単一スロット(`pfs_repl.c:581-591`)なので、N 点の announce/holder 確認が **per-tick で互いを上書き**し実質 N× になりうる(第14波が 1 点で踏んだ取りこぼし `pfs_repl.c:456-471` の N 倍版) | 全 N 点が **ほぼ同一周期で** R 複製到達。total_time(N点)/total_time(1点) ≤ 約 1.5 |
| **F-b 隠れた中央調停 (max 畳み込み)** | 群れへ配る脅威を `protect_threat_level()`worst=max(`protect.c:105-115`)・`compute_threat`max(`world.c:181`)で **1 点へ collapse**。近傍は最大の点しか見えず、別々の点へ別々に応えられない | ビーコン脅威軸が **per-point (id 付き) のベクトル**になり、近傍が **異なる object へ異なる neighbour 群** が寄ることを live で示す。`WORLD_BEACON.threat` が単一バイトのまま(`world.c:209`)なら **構造的に偽**。`coordinator`/集約窓を rally 経路に挟むのも偽(G13 と同罪) | 2 点を別ノードで宣言 → 別々の neighbour 集合が別々の点へ rally し、互いの SAFE 到達を遅らせない |
| **F-c 容量枯渇 (1 点が群れを独占)** | 1 つの守る点が全 neighbour の durable store / WANT 帯域を占有し、他点が永遠に R 未達 | N 点を **R×N > 利用可能スロット** になるよう過剰宣言し、**どの点も飢えない (全点が R 到達 or 公平に degraded(k/n) を表明)** ことを示す。actuator に per-object の公平スケジューリングが無い(`protect_tick` は単純ループ `protect.c:230-269`)ことを負の対照で炙り出す | 全点が R 到達、あるいは不足を **正直に degraded** と表明(死活と同じ honesty)。沈黙の飢餓は偽 |
| **F-d 緑が in-process self-test だけ** | 新 `[g35-*] PASS` を `moe_self_test`/`protect_self_test` 内の合成プラントで出す。live `world.c`/`pfs_repl.c`/`protect.c` 経路は不変 | テストが **kill した本物のノード上の本物の `pfs_repl`/`world`/`protect` を回す**こと。現状の唯一の並行緑 `[moe-concurrent]` は手組 utility(`moe.c:800-861`)= 受け入れない。live demo が 1 点(`run.sh:175,220`)= §5 未証明 | `samples/28_*`(下)を **>=5 回連続** PASS。in-process 数値だけの緑は不可 |

### 1.3 本物 (REAL) の必要条件と commander へ渡す具体テスト

**前提**: (i) 群れへ配る脅威軸を **per-point** にし(単一 max スカラを廃する)、(ii) actuator が
N 点を **並行** に複製し、(iii) **live N-node で同時多発の多点を kill して全点が生き残る** こと。

CI が grep する PASS 行(in-process は補助、**live が本体**):

- **Test-1 多点が単一スカラへ畳まれない (`[g35-no-collapse] PASS`)**: 同一ノードに 2 点
  (deficit 違い)を宣言したとき、近傍が観測できる脅威信号が **2 点を区別**する(各 id ごとに
  別 threat)。`world_peer_threat` が 1 値しか返さない現行(`world.c:267-271`)は **FAIL**。
- **Test-2 並行 = 直列でない (`[g35-parallel-live] PASS`)— 最重要**: live N>=3 ノードで
  **M>=3 点を同時刻に宣言**(別ノード origin)。全点の SAFE 到達の **総壁時計時間 / 1 点ぶん
  ≤ 1.5**(F-a)。さらに **異なる点へ異なる neighbour 集合が rally**(F-b の負の対照: max 畳み込み
  実装なら全 neighbour が最大点へ寄り、他点は遅延 → 比 > 1.5 で落ちる)。
- **Test-3 多点 kill 耐性 (`[g35-multikill-live] PASS`)**: M 点をそれぞれ別ノードに宿し、
  **複数ノードを同時 SIGKILL** → **全点が生存ノードから content-id 一致で復元**できる
  (`pfs_repl.c:372` の検証)。actuator-off の対照では全点が LOST。
- **Test-4 飢餓なし (`[g35-no-starve] PASS`)**: R×M が容量を超える過剰宣言で、**どの点も
  沈黙で飢えない**(全点 R 到達 or 各点が degraded(k/n) を表明)。1 点が群れを独占して他点が
  AT-RISK 据え置きなら FAIL(F-c)。
- **Test-CI 配線 + 反復**: `[g35-parallel-live]`/`[g35-multikill-live]` を `protect-loop-live`
  に倣う新ジョブ `plural-protect-live`(`.github/workflows/ci.yml`)へ。**`run.sh` を >=5 回連続
  実行**し全回 `RESULT: PASS`(flake は §5 では致命 — 第14波が 1 点で flake と戦った
  `audit-5 §13` の N 倍の罠が待つ)。in-process `[g35-*]` は `ump-x86_64` ジョブの grep 群へ補助配線。

**REAL の要約判定**: ① ビーコン脅威軸が **per-point**(単一バイトの廃止)、② **M 点同時の総時間
≈ 1 点ぶん**、③ **複数同時 kill で全点生存**、④ **過剰宣言で飢餓なし**、⑤ **>=5 回 non-flaky**。
これら無しの「§5 DONE」は偽。特に `WORLD_BEACON.threat` が単一バイトのままなら、他が何で緑でも
**構造的に FAKE-plural(手口 b)**。

---

## 2. G13 の現況 — coordinator 集約窓は依然 concurrent flow を再直列化するか

**する。G13 OPEN(🟡)、master `2e4381c` で 1 文字も変わっていない。** §5 に直接効く。

- `coordinator_aggregate()` は responder ループ内から **同期呼び**される — 自 region の coordinator
  かつ他 region 在のときだけ(`dkva.c:639-640`)。
- その中で `INT win = DKVA_RSUM_WIN_MS;`(=**200ms**, `dkva.h:66`)のあいだ
  `while (win > 0) { … tk_dly_tsk(20); win -= 20; }` で回り続ける(`dkva.c:252-269`)。
- **この 200ms のあいだ、step 2 の「pending な各 origin 応答をラウンドロビン再発行」
  (`dkva.c:643-653`)が止まる。** = region 横断クラスタでは coordinator が 1 問受けるたびに
  同時多発応答が 200ms 凍り、G1 が単一 region で解いた同時性が coordinator 経路で再び詰まる。

→ **§5「同時に数百件…並行」の核が、最も配管の重い経路(region 横断)で未達**。しかも本書 §1 の
plural-protect が将来 region 横断で動くなら、この 200ms 窓が **multi-point rally の直列化点
(FAKE-plural 手口 a/b の合体)**になる。`samples/22_composite` も A=単一 region 同時多発 /
B=multi-region 逐次 に分けており、**「同時多発 ∧ region 横断」を一度も同時に live 検証していない**
(第2版 G13 の指摘がそのまま生きている)。§5 受け入れテスト Test-2 は **region 横断で** 回して
初めて G13 を炙り出せる。

---

## 3. G33/G34 の現況 — 温度バケツ脅威源・死んだ確信度ゲート

### 3.1 反射の脅威源は依然 タイマ解除の温度バケツ (G33 反射軸 OPEN, 🟡)

- `gate_predict` は温度を固定しきい値で 3 区間に割るだけ(`moe.c:107-114`)。reflex はこの
  「温度バケツ」を脅威レベルと再解釈(`act_table` `reflex.c:53-57`)。**センサに接地していない。**
- reflex 軸の threat 解除は **唯一 `conserve_until` のタイマ満了**(`reflex.c:283-289`,
  `REFLEX_HOLD_MS=5000` `reflex.h:60`)。protect 軸は G28 で接地・複製で解除されるようになったが、
  **reflex 軸は手つかず**。`compute_threat()=max(rt,pt)`(`world.c:181`)なので、両軸が beacon の
  単一スカラを共有する。
- → **G28 は protect 軸*だけ*を接地した。reflex 軸の脅威はいまも 5 秒タイマで消える ungrounded
    な信号。** 第5版 §12 の「reflex/温度バケツ軸のタイマ解除は手つかず」が不変。

### 3.2 確信度ゲートは moe 経路で依然 死んでいる (G34 OPEN, 🟡→🔴 候補)

- `moe_infer` は reflex を **必ず `confidence=0xFF`** で呼ぶ(`moe.c:418-419`)。reflex の
  `REFLEX_CONF_MIN=40` ゲート(`reflex.c:208`, `reflex.h:68`)は 0xFF を素通しさせる設計なので、
  **moe 経路の全推論が confidence に関わらず反射を発火**(実走で確認: `[moe-protect]` 等は通る)。
- 違反する不変条件 — §9「不確実さについての正直さ」。最も発火頻度の高い moe 経路で「低確信なら
  行動しない」が無効。

### 3.3 plurality は G33/G34 を悪化させるか — **する。二分岐で、どちらも悪い。**

§5 を実装する際の分岐で答えが変わる:

- **分岐 A(beacon を単一スカラのまま=FAKE-plural 手口 b を残す)**: 1 個の spurious reflex
  misfire(温度バケツ G33 + 死んだ確信度 G34)が `compute_threat=max` で **接地済みの protect
  脅威群を *遮蔽* する**(`world.c:181`)。= 多点の本物の危機が、1 個の偽の反射脅威に **max で
  上書きされ、近傍に見えなくなる**。plural の意味が消える。
- **分岐 B(beacon を真に per-point にする=REAL §5)**: 死んだ確信度ゲート(`moe.c:419`)の
  代償が **plural 倍**になる。1 個の低確信 misfire が **自分の独立した偽の rally 点**として登録され、
  群れが **多数の幻の点へ並行に集束**する = 集団誤配分が点数ぶん拡大(第5版 G34「自損→集団誤配分」
  のさらに先)。

→ **§5 が本物になるほど、確信度ゲートの死(G34)の優先度は上がる。** 修正は小
(moe 経路の confidence を max-softmax×100 等の実値で渡す `moe.c:418-419`)だが、plural の前に
入れないと §5 が「多数の幻へ並行誤配分する器官」になる。**本書は G34 を 🔴 へ格上げ推奨。**

---

## 4. G22 の現況 — 「熟慮」は学習か、適応則か — §8 をどれだけ空虚にしているか

**依然オンライン学習ではない(G22 OPEN, 🟡)。§8「全体が未来を強くする」は ほぼ空虚。** 定量:

- **重み本体は誰も outcome で更新しない。** 局所学習 `fl_local_train` は 635 パラメータ中
  **最終層バイアス `b3`(MLP_OUT 個)だけ** 有限差分(`fedlearn.c:95-119`; `delta_w3` は確保
  されるが 0 のまま `fedlearn.c:95`、ループは b3 のみ `fedlearn.c:99`)。w1/w2/w3 は不変。
- **分散 FedAvg は実質 未配線。** `dtk_fl_aggregate` は単一ノードで自 delta を適用するだけ
  (`fedlearn.c:140-154`)、cross-node は node0 集約の「demo 簡易」コメント(`fedlearn.c:130-138`)
  = **中央集約器**であり §7 にも反する。実分散の FedAvg は第5版どおり `E_NOSPT`。
- **reflex の「熟慮」は重みを 1 つも動かさない。** `reflex_deliberate`(`reflex.c:321-352`)が
  動かすのは **`learned_conserve` というスカラ 1 個**だけ(`[8,80]` クランプ・step 6;
  `reflex.h:89-92`)。これは観測駆動のホメオスタット(事前固定の適応則の実行)であって、
  「経験から自分を書き換える考える器官」(§9)ではない。
- **§8「全体が未来を強くする」の定量**: ノードをまたいで伝播し定着するものは **複製された
  バイト(protect/p-fs)だけ**。学習されたパラメータは **0 個** がノード間を渡る(FedAvg E_NOSPT)。
  = system は **並行する図書館**(保存)であって **並行する考える器官**(学習)ではない。第14波の
  protect は §9 の前提条件(記憶の保存)を固めたが、§9 の本旨(思考)は cross-node で 0。

→ **G22 は学習でなく適応則。** §5 が plural の *保護* を実現しても、それは「多点を並行に
**保存**する」であって「多点で並行に **学んで全体を強くする**」ではない。§8 の後半「全体が未来を
強くする」は、現 running system では **実装ゼロ**(空虚度 = 100%、cross-node weight update = 0)。

---

## 5. 新規・残存乖離 G35+（§5 を主題に）

重さ: 🔴 思想の核に反する／🟡 思想を弱める／🟢 軽微・前進。実証: ✅ 実機/CI　📖 コード読み

| # | 不変条件 | 乖離の内容 | 証拠 (file:line) | 重さ | 実証 |
|---|---|---|---|---|---|
| **G35** | §5, §2 | **脅威が群れへ配る段で 1 ノード 1 スカラへ畳まれ、§5 が成立しない。** object 容器は 8 枠 plural だが、`protect_threat_level`=worst-max → `compute_threat`=max → `WORLD_BEACON.threat` 単一バイト → 近傍 rally は点を区別できない。data 複製 actuator だけ per-object。第5版が予言した壁が、protect 層導入後 **max 畳み込み = 隠れ中央調停として本番に実在**。 | `protect.c:105-115`(worst-max), `world.c:169-182,209`(max+単一バイト), `world.c:267-271`/`moe.c:199-204,279`(単値 rally), 対照: `protect.c:230-269`(actuator は per-object) | 🔴 | ✅(コード+実走) |
| **G36** | §5, §3 | **§5 が live で一度も走っていない(緑の self-test / 死んだ live 経路の §5 版)。** 並行の唯一の緑 `[moe-concurrent]` は手組 utility の in-process。live demo は object 1 個。multi-point + multi-kill の live 回帰が CI に無い。 | self-test: `moe.c:807-861`; live: `run.sh:175,220`(1 点), `ci.yml:170-198`(`protect-loop-live` は 1 点) | 🔴 | ✅(CI/コード) |
| **G37** | §5, I6 | **共有 LATEST_ONLY depth-1 制御トピックが N object で奪い合う。** ANN/WANT は単一スロット。第14波は **1 点**の per-tick 取りこぼしを self-repair で跨いだ(`pfs_repl.c:456-471`)が、N 点同時 announce では holder 確認が互いを上書きし、収束が N に比例しうる(FAKE-plural 手口 a/c の素地)。 | `pfs_repl.c:581-591`(LATEST_ONLY), `pfs_repl.c:449-471`(announce hook+self-repair), `protect.c:230-269`(per-object 一括駆動・公平スケジュール無し) | 🟡 | 📖 |
| **G34↑** | I4, §9 | **死んだ確信度ゲートが §5 で *plural 倍* 悪化する**(§3.3)。修正小だが §5 の前に必須。 | `moe.c:418-419`(0xFF 固定), `reflex.c:208`, `world.c:181`(max 遮蔽) | 🔴(格上げ) | 📖 |
| **G13** | I6 | **coordinator 200ms 集約窓が region 横断の同時多発を再直列化**(§2 不変、§5 直撃)。 | `dkva.c:233-269,639-640`, `dkva.h:66`(WIN=200) | 🟡 | 📖 |
| **G22/G23** | §8, §9, §2 | **cross-node の学習が 0**(FedAvg E_NOSPT; 反射熟慮はスカラ 1 個)。論理ノード上限は依然 **32**(`drpc.h:35`)で「全体」が小さい。 | `fedlearn.c:95-119,130-154`, `reflex.c:321-352`, `drpc.h:35` | 🟡/🔴 | 📖 |

---

## 6. 公平の節 — 第14波で前進した本物（判決でなく地図）

- **protect 軸の単一点ループは live で閉じた。** threat=f(under-replication)(`protect.c:94-103`)、
  actuator が複製を駆動し holder 確認で threat が DROP(`protect.c:184-212`)、**kill -9 後に object が
  近傍から復元**(`run.sh:196-204`)、actuator-off 対照は LOST(`run.sh:236-245`)。`protect-loop-live`
  が CI 回帰(`ci.yml:170-198`)。第5版 §1.3 Test-L の精神を満たす。
- **タイマでなく複製で解除。** `protect_threat_for` は replicas の単調非増加関数で >=R で 0
  (実走: `r0=60 r1=40 r2=20 r3=0`)。「SAFE は複製のせい、タイマではない」を数で示した。
- **§2 の設計選択を本物にした**: 守る対象は actuator が **意図して** 複製し、未駆動の対象は
  ambient SYNC から除外(`protect.c:300-304`)= 守る単位と守る力の分離。
- **NO-CENTRAL は依然本物(rally 経路の *配管*)。** `select_expert` は局所 world-table と自己観測
  のみ(`moe.c:241-337`)、per-source topic で単一集約点を作らない(`world.c:103-117,313-326`)。
  *ただし* 脅威の **意味** は max で畳まれており(G35)、配管の分散性と内容の単一性は別問題。
- **正直な未実装表明が一貫。** FedAvg `E_NOSPT`、`survival-network.md:304` が §5 を「未実装」と自認、
  sim は CI 非配線と明記。death-piercing の honesty は健在。

---

## 7. 推奨順位（各1行・実装はしない）

1. **G35(§5 plural)🔴 — 第15波 本丸(進行中)。** §1.3 Test-1〜4 を受け入れ条件に。特に
   **`WORLD_BEACON.threat` の単一バイトを per-point ベクトルへ**置換し、**live multi-point+multi-kill**
   を >=5 回 non-flaky で証明(F-a〜d を機械的に弾く)。
2. **G34(確信度ゲートの死が §5 で plural 倍悪化)🔴 — §5 の *前* に。** moe 経路の confidence を
   実値で渡す小修正(`moe.c:418-419`)。入れないと §5 が「幻へ並行誤配分する器官」になる。
3. **G13(coordinator 集約窓の再直列化)🟡 — §5 を region 横断で。** 200ms 窓を responder ループ
   から外す。Test-2 を region 横断で回して炙り出す。
4. **G37(LATEST_ONLY 単一スロットの N 点奪い合い)🟡** — 制御トピックを per-object か深さ持ちに。
5. **G36(§5 の live 回帰)🔴** — `plural-protect-live` を CI へ。in-process 緑を昇格しない。
6. **G22/G23(cross-node 学習 0・上限 32)🔴 — 下記 本丸。**

---

## 8. 次波の本丸 — G22/§9「考える器官」を本物にする（cross-node 学習）を推す

**§5 (G35) の *次* は、§8 後半「全体が未来を強くする」を本物にすることを本丸にすべき。** 論拠は
backlog でなく **思想 (§8/§9)** から:

1. **§9 の本旨は『保存ではなく思考』。第14波で *保存* (図書館) は live で接地した(protect)。
   §5 はその *保存* を plural にする(多点を並行に守る)。だが §5 を完璧にやっても、それは
   『並行する図書館』であって『並行する *考える器官*』ではない。** cross-node でノード間を渡って
   定着するものは複製バイトだけで、学習されたパラメータは **0 個**(FedAvg `E_NOSPT`、反射熟慮は
   `learned_conserve` スカラ 1 個 `reflex.c:321-352`)。§9 が「記録の保存は思考の前提にすぎない」と
   明言する以上、前提(保存)が立った今こそ本旨(思考)が次の一点。

2. **§8 は二層を『近傍が今を守り、全体が未来を強くする』と定義する。** 第14波+第15波 §5 は
   **第1層(反射/今を守る)**を多点 plural にする。だが **第2層(熟慮/未来を強くする=学習)**は
   running system に存在しない(§4 で定量: cross-node weight update = 0)。二層構造の片肺が live で
   一度も動いていない。§8 の「全体」が機能しないなら、§5 の plural な反射は「未来を強くしない
   並行防御」= 文明の比喩(§9)に届かない。

3. **§7(中央なし)の規律が、ちょうど cross-node 学習に再利用できる。** rally の局所勾配と
   per-source gossip(`world.c`/`moe.c`)は既に NO-CENTRAL。同じ substrate で **小さな共有モデルの
   勾配を局所勾配として流し**(中央 aggregator 無しの分散平均)、**live で『join すると全体が賢く
   なる』** を数で示せれば、§8 後半が初めて本番で真になる。R3b `breathe`(`spec.c`)は in-kernel で
   「join で賢くなる」を示したが、**relay 経由の実分散学習ではない**(`survival-network.md:366` 自認)。
   本丸 = この `breathe` を **live N-node の no-central 学習**へ昇格し、kill しても学習が継続する
   ことを `[future-stronger-live]` で回帰する。

> 対案として G37/G13(§5 の配管最適化)を本丸にする手もあるが、それらは §5 を *速く綺麗に* する
> 深化であって、**思考そのものを cross-node に立ち上げる質的転換ではない**。§9 の言葉に立ち返れば、
> 希少なのは『保存ではなく思考する器官として知を組む』思想。保存(G28)と並行保存(G35)の次は、
> **並行思考(G22/§9)** が接地の順序として正しい。

---

## 9. 評決（§2∧§5 後、running system は「人類全体の脳」にどれだけ近いか・正直に）

> **第14波で *一点* は live で守られるようになった。だが §5 を実装中のいまも、システムは
> *多点* を群れの *計算* で守れない — 脅威は群れへ配る段で max に畳まれ(`protect.c:105-115` /
> `world.c:181,209`)、近傍は『最大の一点』しか見えない。** これは隠れた中央調停であり、§2 が
> 否定した「一個の中央知性へ収束させる」形の、より静かな再来だ。data 複製の actuator は per-object
> で並行できる(`protect.c:230-269`)から、システムは『多点を並行に *保存* できるが、多点へ並行に
> 群れの力を *注げ* ない』半 plural である。そして §5 は **live で一度も走っていない** — 並行の
> 唯一の緑は in-process の手組 self-test(`moe.c:807-861`)、live demo は object 1 個(`run.sh`)。
> 第5版が名付けた「緑の self-test / 死んだ live 経路」の病は、軸を変えて §5 に再発している。
>
> **正直に言えば、§5 については prior audits が認めるより *遠い*。** 第5版は G35 を「G28 の直後の
> 深い壁」と 🟡 で書いた。本書はそれを 🔴 へ上げる: protect 層が入った結果、max 畳み込みは
> 『未実装の壁』から『本番経路に *実在する* 中央調停』へ具体化し、しかも死んだ確信度ゲート
> (`moe.c:419`)が §5 を本物にするほど plural 倍の集団誤配分を呼ぶ(§3.3)。region 横断では
> coordinator の 200ms 窓(`dkva.c:252-269`)が同時性を凍らせ続ける(G13 不変)。
>
> **そして最も深い正直**: 仮に §5 が完璧に plural な *保護* を達成しても、それは「並行する図書館」
> にすぎない。ノード間を渡って未来を強くする学習は **0**(`fedlearn.c` FedAvg `E_NOSPT`、熟慮は
> スカラ 1 個 `reflex.c:321-352`)。§8 の『全体が未来を強くする』、§9 の『考える器官』は、running
> system では **まだ空文**だ。
> —— 迷ったら `survival-network.md §5`(同時多発・並行分散で **単一意識のボトルネックを越える**)
> と §9(保存ではなく思考)へ戻る。いま running system は、**一点を守る図書館** から **多点を並行に
> 守る図書館** へ移ろうとしているところで、**人類全体の *脳*(多点・並行・学んで強くなる)** には
> まだ、保存の plural 化と、その先の cross-node 思考、二段の接地が残っている。注ぐ向きは正しい。
> 注ぐ先は一点から多点へ広がりつつある。だが「全体で考える」は、まだ self-test の外に存在しない。
