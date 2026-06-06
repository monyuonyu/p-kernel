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
 *    CONSERVE : reflex_pressure_bias() を world ビーコンの pressure に上乗せ
 *               し、moe ゲートの局所勾配へ「受援不要・応援に出ない」を伝える。
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
#define REFLEX_HOLD_MS       5000   /* エンゲージしたアクションの保持時間   */
#define REFLEX_POLL_MS       100    /* アラーム取り込み + 解除チェックの周期 (≤200ms) */

/* SHIELD は「危険 class が連続したら」発火する。単発の誤推論で未知コード
 * 取り込みを止めてしまわないための、最も重い行動への追加ゲート。 */
#define REFLEX_SHIELD_STREAK 2      /* SHIELD 発火に要する連続 critical 回数 */

/* 確信度が低い推論では行動しない (confidence==0xFF=不明は許可)。 */
#define REFLEX_CONF_MIN      40     /* 行動に要する最小 confidence (0..100) */

/* CONSERVE 中に world ビーコンへ上乗せする pressure バイアス (0..100)。
 * これが moe の局所勾配に乗り、近傍ゲートが当ノードへの委譲を避ける。 */
#define REFLEX_CONSERVE_PRESSURE 40

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

/* 推論完了フック: 推論結果 (脅威レベル class) を反射アクションへ変換する。
 * dtr の推論完了点 (dtr_log_push) から 1 行で呼ばれる。
 *   threat_class : 0..2 (推論 class)
 *   confidence   : 0..100 (max softmax×100), 不明なら 0xFF
 *   src_node     : 観測ノード (drpc_my_node, 単機なら 0xFF) */
void reflex_on_inference(UB threat_class, UB confidence, UB src_node);

/* SHIELD 照会 — usermain が新規 selfc 実行 / genome 発芽の前に参照する。
 * TRUE のあいだ未知コードを取り込まない (攻撃下の遮蔽)。 */
BOOL reflex_is_shielded(void);

/* CONSERVE 照会 — world.c の compute_pressure() がビーコンへ上乗せする
 * pressure バイアス (0..REFLEX_CONSERVE_PRESSURE)。CONSERVE 非発火なら 0。 */
UB reflex_pressure_bias(void);

/* shell `reflex [on|off|table|stat]`:
 *   (引数なし)/stat — 現在の反射状態とアクション表と統計を表示
 *   on / off        — 反射層の有効/無効 (デフォルト有効)
 *   table           — class→action のアクション表を閲覧 */
void reflex_cmd(const UB *args, UW len);
