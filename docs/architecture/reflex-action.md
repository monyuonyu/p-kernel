# 反射の行動 — 思考に手足を付ける（推論結果 → 局所即時防御）

> 第9波・配線②。`arch/common/reflex.c` / `arch/common/include/reflex.h`。
> 関連: [[survival-network.md]] §2（守る単位と守る力の分離）・§8（反射＝速い時定数・局所閉ループ）、
> [[reflex-deliberation.md]]（反射層／熟慮層の時定数分離）。

最終更新: 2026-06-06（第9波②着地）／ デモ: `samples/15_reflex/reflex_demo.sh`

---

## 1. 解こうとした穴 — 第三レビューの配線②（逐語）

> 「推論結果が class ラベルを吐いて、どこにも行かない。『守る力』は計算するが、
> 目の前の一点を守る（行動する）手が無い。**思考に手足が付いていない。**
> §8反射層の『即収縮・遮蔽・回避』に繋げ。」

それまで `infer`／`dtr` は class ラベル（0=normal / 1=alert / 2=critical）を
**print して終わり**だった。脳が「危険だ」と判断しても、その判断が何も動かさない。
[[survival-network.md]] §8 が要求する反射層 ——「危険を感じたら即収縮・遮蔽・回避。
今を守る」—— の **行動する手** が存在しなかった。reflex はその手である。

---

## 2. §2／§8 との対応 — なぜ「反射」なのか

| survival-network.md | reflex.c での実体 |
|---|---|
| §2 守る単位（ローカル）と守る力（全体）の分離 | 守る単位＝この1ノードの即時行動。守る力＝隣へ流す BEACON は**情報**であって命令ではなく、受け手が自分の反射表で判断する（中央なし） |
| §8 反射層＝近傍・低遅延・即応 | `reflex_on_inference` は推論完了の瞬間に発火（要求駆動、実質ゼロ遅延）。局所状態だけで閉じる |
| §8「即収縮・遮蔽・回避」 | CONSERVE（収縮）／ SHIELD（遮蔽）／ BEACON（警報＝回避を促す情報） |
| §8 発振への処方（二層・ヒステリシス） | **速く入り、ゆっくり出る**（`REFLEX_HOLD_MS=5s` のヒステリシス）。受信反射は減衰（CONSERVE どまり・hop=1 で打ち止め）し、群れが一斉に痙攣しない |

反射は「正しい判断」ではなく「間に合う判断」を担う（§8）。だから玩具で良いが、
**配線は本物**でなければならない —— 飾りの print ではなく、実在するシステム操作を動かす。

---

## 3. アクション表 — class（脅威レベル）→ action（データ駆動）

class ラベルを「脅威レベル」と解釈し、小さな静的表で応答する（`reflex table` で閲覧）。

| class | 意味 | action |
|---|---|---|
| 0 | normal（脅威なし） | （なし）— 通常 class では何もしない |
| 1 | alert（警戒） | `CONSERVE` + `BEACON` |
| 2 | critical（危険） | `SHIELD` + `CONSERVE` + `BEACON`（SHIELD は連続 critical ≥2 が条件） |

追加ゲート:
- **確信度**: `confidence < REFLEX_CONF_MIN(=40)` の推論では行動しない（不明=0xFF は通す）。単発の低確信で身を固めない。
- **連続性**: 最も重い SHIELD は `REFLEX_SHIELD_STREAK(=2)` 回連続 critical を要求。誤推論1発で未知コード取り込みを止めない。

---

## 4. 行動の実在性 — 何が「本当に」変わるか

3つとも**ローカル閉ループ・中央なし**。飾りの print ではない。

### SHIELD（遮蔽）— 未知コードを取り込まない
- `reflex_is_shielded()` が真のあいだ、usermain は新規 `selfc`（カーネル内 C コンパイル＝
  未知コードの発芽）を**拒否**する。攻撃下で自分の実行基盤に新コードを取り込まない。
- 実在の効果: shell で `selfc demo` を打っても
  `[reflex] SHIELD active — refusing new selfc germination` で弾かれる（デモでアサート）。
- 編集境界: `selfc.c` は触らず、reflex 側が公開する `reflex_is_shielded()` を
  usermain のコマンド分岐が参照する形（同 API を将来 genome 発芽側も参照しうる）。

### CONSERVE（収縮）— 受援不要・応援に出ない
- `reflex_pressure_bias()`（CONSERVE 中は `+40`）を `world.c` の `compute_pressure()` が
  ビーコンの pressure に上乗せする。
- 実在の効果: 上がった逼迫度が world ビーコンで**ゴシップ**され、近傍ノードの
  `moe` ゲート（`utility = acc − rtt − pressure`）が当ノードへの委譲を避ける
  ＝「血流を自分に温存する」（§6 応援・受援の勾配を局所に注入）。解除でバイアスは 0 に戻る。

### BEACON（警報）— 隣が知る（命令ではなく情報）
- per-source topic `reflex/alarm/<node>` へ `{class, confidence, hop}` を即時 publish。
- 実在の効果: 隣のノードがそれを受信する。ただし**中央指令ではない** —— 受信側は
  自分の反射表で「構えるか」を判断する（NO-CENTRAL 不変条件）。

---

## 5. 時定数と発振防止（§8）

| 定数 | 値 | 役割 |
|---|---|---|
| 入り | 推論完了の即時 | 反射は速く入る（要求駆動） |
| `REFLEX_HOLD_MS` | 5000ms | ヒステリシス: エンゲージしたアクションを5秒保持してから解除（**ゆっくり出る**）。入力がチラついても高速 on/off の発振をしない |
| `REFLEX_POLL_MS` | 100ms | アラーム取り込み＋解除チェック周期（≤200ms の閉ループ） |
| `REFLEX_MAX_HOP` | 1 | アラーム中継は1ホップで打ち止め（空間の減衰） |

**伝播の二段減衰（群れの一斉痙攣を防ぐ）**:
1. *強度の減衰* — 受信した危険では SHIELD まで行かず **CONSERVE どまり**。完全な遮蔽は
   自分で観測した危険にだけ値する。
2. *空間の減衰* — `hop>0` のときだけ `hop-1` で1回中継。`MAX_HOP=1` なので連鎖は隣の隣で止まる。
   受信は seq で重複排除（同じ警報を二度処理しない）。

---

## 6. 配線（フック点）

```
dtr_infer (forward)                         reflex.c
  ├─ SOLO / FULL(DKVA) ─▶ dtr_log_push ──┐
  │                                       ├─▶ reflex_on_inference(class, conf, node)
  └─ REDUCED (TensorPar) ────────────────┘        │
                                                   ├─ act_table[class] → SHIELD/CONSERVE/BEACON
                                                   ├─ engage(*_until = now + HOLD_MS)   [速く入る]
                                                   └─ emit_beacon → reflex/alarm/<node>

reflex_task (全ノード対称・中央なし)
  ├─ poll reflex/alarm/<peer> ─▶ on_alarm() ─▶ 減衰 CONSERVE + hop-1 中継
  └─ check_release() ─▶ now ≥ *_until で解除                              [ゆっくり出る]

reflex_is_shielded()   ◀── usermain: selfc 発芽の可否
reflex_pressure_bias() ◀── world.c compute_pressure(): moe ゲートへ局所勾配を注入
```

推論完了点は dtr の2経路から1行で呼ぶ（`dtr_log_push` が SOLO/FULL を、TP 完了点が
REDUCED を担う）。dkva の集約ロジック・selfc.c・genome 系は未編集。

---

## 7. デモ（`samples/15_reflex/reflex_demo.sh`）

2ノードが REDUCED でメッシュ（`infer` はテンソル並列、反射フックは dtr 完了点から発火）。

1. node1 に critical 入力（`infer 120 5 0 90`）を連続注入 → 1発目 CONSERVE+BEACON、
   2発目以降 SHIELD+CONSERVE+BEACON（連続ゲート）。
2. SHIELD 中に `selfc demo` → **拒否**される（実在の遮蔽）。
3. node2 がアラームを受信 → **自分の判断で**減衰 CONSERVE（SHIELD はしない＝痙攣しない）。
4. 入力停止 → 5秒のヒステリシス後、両ノードが自動解除。

9アサート（発火・遮蔽・伝播・減衰・無痙攣・自動解除）。失敗時 exit 非0。

生ログ例:
```
node1: [dtr] TP(REDUCED): class=2 (critical) scores=[0.00 0.00 0.99]
       [reflex] FIRE class=2 -> SHIELD CONSERVE BEACON (hold 5s)
       [reflex] BEACON class=2 (critical) seq=2 hop=1 -> reflex/alarm
       [reflex] SHIELD active — refusing new selfc germination
       [reflex] SHIELD released (hysteresis 5s elapsed)
node2: [reflex] heard alarm from node0 class=2 (critical) -> attenuated CONSERVE (no SHIELD; my own judgement)
       [reflex] CONSERVE released (hysteresis 5s elapsed)
```

---

## 7b. G38 — 二層結合（wave 17）: 思考が守りを変える

> 関連: [[survival-network.md]] §8（近傍が今を守り **全体が未来を強くする**）・§9（考える器官）、
> [[archive/philosophy-gap-audit-7.md]] §12、`arch/common/gossip_learn.c` の `[g38-*]`、
> デモ: `samples/34_twolayer/run.sh`。

audit-7 §4.3 の核心的指摘: G22（協調学習）が landing しても、**学習されるモデル本体が
反射層へ一本も配線されていない** —「二層は並んでいるだけで結合していない」。G38 は 2 本の
矢印を引いた。

**主アロー（学習 → 守る）。** `moe_infer` は反射へ渡す確信度を **`0xFF` 固定**で殺していた
（G34: 確信度ゲートが常に素通り = 低確信の誤推論でも反射が暴発）。いまは協調学習される
Transformer（dtr; G22 が全網平均する本体）に同じ入力を通し、その **実 max-softmax** を確信度、
`argmax` を脅威クラスとして反射へ渡す（`moe.c`）。発火判定は `reflex_would_fire()` に一元化
（本番と self-test が同じゲートを使う）。**結果: 低確信（未学習/曖昧）な入力は反射を発火させず、
高確信の脅威クラスだけが決然と発火する。** 同一の critical 入力が、UNLEARNED モデルでは
`cls=0 conf=50% → fire=no`、協調学習後は `cls=2 conf=97% → fire=YES` に反転する。

**第二アロー（守る → 学習）。** 反射が「危険」と判断して発火したクラス別経験
（`reflex_threat_experience()`）を、協調学習が **優先度**として読み、守りが要ったクラスを
重点学習する（遅い熟慮帯域、`gl_run_gossip_weighted`）。守った経験が全体の未来の学習を形作る。

**数（正直）。** 同じ反射ゲートを学習モデルで駆動した held-out 守りスコア:
**UNLEARNED 33.3% → LEARNED 93.3%**（threat-detect **0% → 95%**）。改善は学習のみに由来する
（確信度は実 softmax、ゲートは同一）。LIVE（`samples/34_twolayer`、relay 経由 3 ノード・x86_64）
でも各ノードの LEARNED 守りスコアが UNLEARNED ベースライン（33%）を超え（例: 88%/83%）、
**ノードを kill -9 しても残りが守り続ける**。CI: 自己テスト `[g38-*]` ＋ live `twolayer-couple-live`。

**残（正直）。** 確信度ゲートと「脅威クラスの判断」は学習モデルへ接地したが、CONSERVE の
**保持時間**は依然 `REFLEX_HOLD_MS=5s` のタイマ（脅威軸の温度バケツ・ヒステリシスは健在 —
G33 反射軸は部分的に残る）。第二アローの定量効果は、長い協調学習が ~100% に飽和すると
plain と weighted の差が消えるため、短スケジュールでのみ headroom があり「飽和域では marginal」
と正直に報告する（`[g38-guard-feeds-learning]` はアローの **存在と機能**を構造的に保証する）。

---

## 8. 限界（正直に）

- **行動が3種だけ**（SHIELD / CONSERVE / BEACON）。実機の「回避」（物理アクチュエータ）は無い。
  このOSの「世界」は今はネットワークと自分自身なので、行動もそこに閉じている。
- **脅威モデルが玩具**。class ラベルを脅威レベルと素朴に同一視しているだけで、
  本当の攻撃検知ではない。誤推論が反射を誤発火させうる（確信度・連続ゲートで緩和するのみ）。
- **自エコーの受信**。origin は隣が中継した自分の警報（src が中継者・seq が別）を
  自分の発火と区別できず、減衰 CONSERVE を再エンゲージしうる（無害・dedup で有界だが純粋ではない）。
- **時定数は経験則**。`HOLD_MS=5s`・`MAX_HOP=1` は survival §8 / reflex §7-5 の未解決
  （層をまたぐ発振の定量設計）に未着手のままの当面値。
- **熟慮層との和解は未実装**（reflex §7-1/§7-2 OPEN）。反射は「今を守る」だけで、
  熟慮（学習・設計更新）が反射を上書き・抑制する権限分散はまだ無い。
- **REDUCED/TP 以外の dtr 完了点の網羅**: SOLO/FULL は `dtr_log_push`、REDUCED は
  TP 完了点でカバー。pipeline-parallel フォールバック経路は元々ログを残さず、反射も未接続。
