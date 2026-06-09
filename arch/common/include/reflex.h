/*
 *  reflex.h
 *  §8 反射層 — 思考→行動の配線 (推論結果 → 局所即時防御アクション)
 *
 *  設計: docs/architecture/reflex-action.md
 *        docs/architecture/survival-network.md
 *          §2 守る単位と守る力の分離 / §8 反射=速い時定数・局所閉ループ
 *
 *  第三レビューの配線②: 「推論結果が class ラベルを吐いて、どこにも行かない。
 *  『守る力』は計算するが、目の前の一点を守る(行動する)手が無い」。
 *  reflex は dtr/dkva の推論完了点に繋がり、class ラベルを「脅威レベル」と
 *  解釈して、データ駆動のアクション表で *実在するシステム操作* に変換する。
 *
 *  反射の三原則 (§8):
 *    1. 速く入る   — 推論完了の即時 (≤ REFLEX_POLL_MS の閉ループ)
 *    2. ゆっくり出る — REFLEX_HOLD_MS のヒステリシスで解除 (発振防止)
 *    3. 局所で閉じる — 中央なし。アラームは「命令」ではなく「情報」であり、
 *                      受信側も自分の反射表で判断する (NO-CENTRAL 不変条件)。
 *
 *  ── アクションの実在性 (飾りの print ではない) ─────────────────────────
 *    SHIELD   : reflex_is_shielded() を立て、usermain が新規 selfc 実行 /
 *               genome 発芽を拒否する (攻撃下で未知コードを取り込まない)。
 *    CONSERVE : reflex_threat_level() を world ビーコンの *脅威軸* (threat) へ
 *               載せる。これが moe ゲートで *加点* され (load とは逆符号)、
 *               近傍の計算がこの一点へ集束する = 「守る対象へ全網の力を注ぐ」
 *               (survival §2)。同時に自ノードのゲートでも自己効用が上がり、
 *               脅威下でも守るべき仕事を手放さない (flee しない)。
 *               【G20 修正】かつては pressure (load 軸) へ上乗せしていたため
 *               「脅威 = 避けよ」の符号倒錯 = 群れが守る対象から逃げていた。
 *    BEACON   : K-DDS topic "reflex/alarm/<node>" へ脅威観測を即時 publish。
 *               隣の細胞が知る — ただし命令ではなく情報 (受信側が自分で判断)。
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 脅威レベル = 推論 class ラベル (dtr/dkva の 3 クラスを再解釈)        */
/*   0 = normal   (脅威なし)                                           */
/*   1 = alert    (警戒)                                               */
/*   2 = critical (危険)                                               */
/* ------------------------------------------------------------------ */

#define REFLEX_NUM_CLASSES   3    /* MOE_NUM_CLASSES / DTR_OUT_DIM と一致 */

/* ------------------------------------------------------------------ */
/* アクションビット (class→action はデータ駆動の小さな静的表)          */
/* ------------------------------------------------------------------ */

#define REFLEX_ACT_NONE      0x00
#define REFLEX_ACT_SHIELD    0x01   /* 遮蔽: 未知コード取り込み拒否        */
#define REFLEX_ACT_CONSERVE  0x02   /* 収縮: pressure を上げ受援/応援を絞る */
#define REFLEX_ACT_BEACON    0x04   /* 警報: reflex/alarm を即時 publish    */

/* ── §8 の時定数 (速く入り、ゆっくり解除する) ───────────────────────
 * 反射は推論完了の瞬間に入り (要求駆動、実質ゼロ遅延)、HOLD_MS のあいだ
 * 保持されてからしか解除されない。これがヒステリシスとなり、入力が
 * チラついても反射が高速にオン/オフを繰り返す発振を防ぐ (§8 二層構造の
 * 反射層側のダンピング)。 */
#define REFLEX_HOLD_MS       5000   /* SHIELD のヒステリシス保持時間 (遮蔽の発振防止) */
#define REFLEX_POLL_MS       100    /* アラーム取り込み + 解除チェックの周期 (≤200ms) */

/* ── G33: 脅威レベルは「観測された危険量」で上下する (タイマ解除ではない) ──
 * CONSERVE の脅威レベル (reflex_threat_level) は、危険が *観測されている間*
 * 立ち、危険信号が SAFE (normal 観測) に戻った瞬間に *即座に* 落ちる。
 * 落とすのは HOLD タイマの満了ではなく「制御量 (= 観測された危険) が安全へ
 * 戻ったこと」である (protect_threat_level が under-replication で接地するのと
 * 同じ思想; survival §2 / philosophy-gap-audit G33)。
 *
 * 時間が関与するのは下の SAFETY CAP だけ: 危険を観測したまま、その後 danger
 * とも safe とも一切観測が来ない (= 推論ストリームが沈黙した) 状態が長く続けば
 * ラッチが固着しないよう解除する。これは *正常な解除経路ではなく* スタック・
 * ラッチの保険であり、[g33-controlled] self-test が「正常解除は量であって CAP
 * ではない」ことを数で示す。HOLD より十分大きくしておくこと: テストが時計を
 * HOLD 超へ進めてもレベルが残る (= タイマで落ちていない) ことを見せるため。 */
#define REFLEX_THREAT_CAP_MS 30000  /* スタック・ラッチ保険 (>> HOLD; 正常解除ではない) */

/* SHIELD は「危険 class が連続したら」発火する。単発の誤推論で未知コード
 * 取り込みを止めてしまわないための、最も重い行動への追加ゲート。 */
#define REFLEX_SHIELD_STREAK 2      /* SHIELD 発火に要する連続 critical 回数 */

/* 確信度が低い推論では行動しない (confidence==0xFF=不明は許可)。 */
#define REFLEX_CONF_MIN      40     /* 行動に要する最小 confidence (0..100) */

/* CONSERVE 中に world ビーコンの *脅威軸* (threat) へ載せる強度 (0..100)。
 * これが moe ゲートで *加点* され (load とは逆符号)、近傍が当ノードへ寄る
 * (rally; §2 一点集束) と同時に当ノード自身が守るべき仕事を保持する。
 * これは「初期値」であり、§9 熟慮層がこの効きの強さを経験から学習で動かす
 * (reflex_threat_level() は固定値ではなく learned_conserve を返す)。
 * 名は CONSERVE (収縮) のままだが、G20 後の *効果* は「避けさせる」ではなく
 * 「寄せさせる/手放させない」。 */
#define REFLEX_CONSERVE_PRESSURE 40

/* ── G18 熟慮 → 学習 → 反射ループ (§9 経験から自分を書き換える環) ───────
 * 反射層 (速い時定数) が脅威を観測して CONSERVE を engage する一方、熟慮層
 * (遅い時定数) は蓄積した経験 — 「脅威がどれだけ滞留したか (dwell)」— を
 * 集計し、CONSERVE の効きの強さ (learned conserve pressure) を小さく nudge
 * する。脅威の滞留が長い = 応答が弱い → 効きを上げる。滞留が目標より短い =
 * 過剰防御 → 少し下げる。外部 `dtr train` 無しに、稼働中の観測だけから反射
 * の振る舞いそのものが書き換わる = 思考(熟慮)→学習→反射の環が閉じる。
 *
 * これは外側のゆっくりした適応ループであり、内側の速い負帰還
 * (CONSERVE→pressure→§7 ゲート→負荷再分配) の *ゲイン* を経験で調律する。 */
#define REFLEX_CONSERVE_MIN    8    /* 学習が下げられる下限 (過剰防御を抑える) */
#define REFLEX_CONSERVE_MAX   80    /* 学習が上げられる上限 (発振を避ける)     */
#define REFLEX_DWELL_TARGET    3    /* 望ましい脅威滞留 (反射 tick / 観測数)    */
#define REFLEX_LEARN_STEP      6    /* 1 熟慮 tick あたりの learned_conserve nudge 幅 */
/* reflex_task の何ポールごとに 1 回熟慮するか。REFLEX_POLL_MS=100ms × 50 =
 * 5s = moe の熟慮層 (MOE_DELIB_TICK_MS=2s) より更に遅い時定数 (§8 二層)。 */
#define REFLEX_DELIB_EVERY    50

/* ── アラーム伝播の減衰 (群れ全体の一斉痙攣を防ぐ) ───────────────────
 * アラームは hop を持ち、受信側は hop>0 のときだけ hop-1 で中継する。
 * かつ受信した反射は *減衰* する: 自分で観測した危険は SHIELD まで行くが、
 * 「隣が叫んだ」だけでは CONSERVE どまり (より軽い構え)。連鎖は1ホップで
 * 止まり、群れが一斉に同じ強度で痙攣しない (§8 局所閉ループ)。 */
#define REFLEX_MAX_HOP       1

/* ------------------------------------------------------------------ */
/* アラームパケット (K-DDS 経由)。LP64 トラップ対策: 固定幅型のみ。     */
/* ------------------------------------------------------------------ */

#define REFLEX_ALARM_MAGIC   0x584C4652UL   /* "RFLX" LE */
/* per-source topic: 単一スロットへ全員が上書きし合う集約点を作らない
 * (world/moe/dkva と同じ per-source パターン; NO-CENTRAL 不変条件)。 */
#define REFLEX_ALARM_TOPIC_PFX "reflex/alarm/"   /* + 1 桁ノード ID */

typedef struct {
    U4  magic;          /* REFLEX_ALARM_MAGIC                       */
    U4  seq;            /* 発信ごとに増える単調シーケンス           */
    U1  src_node;       /* 観測したノード                           */
    U1  threat_class;   /* 観測した脅威レベル (0..2)                */
    U1  confidence;     /* 0..100 (0xFF=不明)                       */
    U1  hop;            /* 残り中継ホップ数 (減衰)                  */
} __attribute__((packed)) REFLEX_ALARM;   /* 12 bytes */

_Static_assert(sizeof(REFLEX_ALARM) == 12, "REFLEX_ALARM must be 12 bytes (LP64-safe wire)");
_Static_assert(sizeof(U1) == 1 && sizeof(U4) == 4,
               "reflex wire fields must be true fixed-width");

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* usermain() の初期化ブロックで world_init() の近くで呼ぶ。 */
void reflex_init(void);

/* アラーム取り込み + ヒステリシス解除を行う常駐タスク。cmd_net から起動。
 * 全ノードで対称に走る (中央なし)。 */
void reflex_task(INT stacd, void *exinf);

/* テスト隔離用 (wave 18): reflex の有効/無効を切替え、旧値を返す。moe.c の
 * [onebrain-*] が live な moe_infer を回す間だけ反射の副作用 (guard 経験など)
 * を止めて復元するために使う。 */
BOOL reflex_set_enabled(BOOL on);

/* 推論完了フック: 推論結果 (脅威レベル class) を反射アクションへ変換する。
 * dtr の推論完了点 (dtr_log_push) から 1 行で呼ばれる。
 *   threat_class : 0..2 (推論 class)
 *   confidence   : 0..100 (max softmax×100), 不明なら 0xFF
 *   src_node     : 観測ノード (drpc_my_node, 単機なら 0xFF) */
void reflex_on_inference(UB threat_class, UB confidence, UB src_node);

/* ── G38 主アロー: 学習 → 守る (思考が守りを変える) ──────────────────────
 * 反射の発火ゲートそのものの純述語 (状態なし)。reflex_on_inference の
 * アクション表ゲート + 確信度フロアと *同一* の判定を 1 箇所に置く
 * (deadband_pick と同じ思想): こうすると self-test が本番と同じゲートを
 * 検査でき、確信度ゲートの規則が二重定義でズレない。
 *   threat_class : 学習モデルの判断 (argmax)
 *   confidence   : 学習モデルの max-softmax×100 (0..100; 0xFF=不明は通す)
 * 低確信 (未学習/曖昧) は FALSE = 反射を発火させない。高確信の脅威クラスは
 * TRUE = 決然と発火。死んだ 0xFF 固定ゲート (G34) を殺す配線の核。 */
BOOL reflex_would_fire(UB threat_class, UB confidence);

/* ── G38 第二アロー: 守る → 学習 (近傍が今守った経験が全体の未来を強くする) ──
 * 反射/protect 層が「危険」と判断して発火したクラスごとの経験回数。協調学習
 * (gossip_learn, G22) がこれを *優先度* として読み、守りが要ったクラスを
 * 重点的に学ぶ → ラウンドを重ねるほど群れの守りが良くなる (§8 二層結合)。
 * 遅い熟慮帯域で読むこと (反射 tick で読まない)。 */
UW reflex_threat_experience(UB cls);

/* ── LM-3 salience cert support (living-mind Part IV) ─────────────────────
 * Snapshot / restore the per-class guard experience so a self-isolating probe
 * (the DMN salience certificate, lm_consolidate.c) can DRIVE real reflex
 * firings to EARN salience, then restore the LIVE G38 counters untouched —
 * the same save/restore reflex_self_test performs internally (~ln 904).
 * reflex_threat_experience() is the read path; this is the write/restore path. */
void reflex_guard_exp_save(UW out[REFLEX_NUM_CLASSES]);
void reflex_guard_exp_restore(const UW in[REFLEX_NUM_CLASSES]);

/* SHIELD 照会 — usermain が新規 selfc 実行 / genome 発芽の前に参照する。
 * TRUE のあいだ未知コードを取り込まない (攻撃下の遮蔽)。 */
BOOL reflex_is_shielded(void);

/* CONSERVE 照会 — world.c がビーコンの *脅威軸* (WORLD_BEACON.threat) へ
 * 載せる脅威強度。CONSERVE 発火中は learned_conserve (学習値) を、非発火なら
 * 0 を返す。moe ゲートはこれを *加点* (load の逆符号) し、群れが当ノードへ
 * 集束する (§2)。【G20 修正】旧 reflex_pressure_bias() を改名: 効果が
 * load 軸 (避ける) から threat 軸 (寄る) へ移ったことを名でも明示する。 */
UB reflex_threat_level(void);

/* ── G33: 脅威レベルの純粋な解除式 (live アクセサと self-test が共有; 重複定義
 * なし。protect_threat_for と同じ作法) ──────────────────────────────────────
 * 脅威レベルは「観測された危険量」(danger_active) の関数であり、壁時計ではない:
 *   danger_active==FALSE          -> 0     (SAFE 観測でレベルは *即座に* 落ちる)
 *   danger_active==TRUE           -> level (危険が観測されている)
 *   ただし ms_since_danger が REFLEX_THREAT_CAP_MS を超えていれば 0
 *   (= 観測ストリームが沈黙したときの SAFETY CAP のみ; 正常解除ではない)。 */
UB reflex_threat_for(BOOL danger_active, UW ms_since_danger, UB level);

/* G18 熟慮 tick: 蓄積した経験 (脅威 dwell 統計) から learned_conserve を
 * 学習で nudge する。reflex_task が遅い時定数 (REFLEX_DELIB_EVERY ポール) で
 * 呼ぶ。`reflex test` の C 部からも本番ロジックそのものを叩く。 */
void reflex_deliberate(void);

/* 学習で動く CONSERVE 効きの現在値 (観測性・テスト用)。 */
UB reflex_learned_conserve(void);

/* §8/§9 性質テスト (philosophy-gap-audit-2 G17/G18 — 閉ループの自動検証)。
 * shell `reflex test` から呼ぶカーネル内 self-test。0=全 PASS。
 *   B: 行動→知覚→ゲートの負帰還が *減衰して定常へ収束* (loop on) する一方、
 *      フィードフォワード (loop off) では脅威が滞留し続けることを数で示す。
 *   C: 熟慮層が経験 (dwell) から learned_conserve を学習し、指標が時間と
 *      ともに改善することを数で示す (学習 off では改善しない)。
 * §7 ゲートの効用は本番 moe_expert_utility を使う。純ローカル計算
 * (net/kdds 不要)・小スタックなのでベアメタルでも走る。 */
INT reflex_self_test(void);

/* shell `reflex [on|off|table|stat|test]`:
 *   (引数なし)/stat — 現在の反射状態とアクション表と統計を表示
 *   on / off        — 反射層の有効/無効 (デフォルト有効)
 *   table           — class→action のアクション表を閲覧
 *   test            — §8/§9 閉ループ性質テスト (負帰還の収束 + 熟慮学習) */
void reflex_cmd(const UB *args, UW len);
