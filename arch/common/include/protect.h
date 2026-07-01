/*
 *  protect.h — the protected-object registry (survival §2, G28).
 *
 *  設計: docs/architecture/00-concept/survival-network.md §2
 *        「守る単位 (protected UNIT) と守る力 (protecting POWER) の分離」
 *        「全網の力を一点へ注ぐ (concentrate the network's force on the unit)」
 *        docs/architecture/philosophy-gap-audit-4.md G28
 *
 *  ── なぜ存在するか (wave 14: 開いたループを閉じる) ───────────────────────
 *    wave 13 で moe ゲートに *脅威軸* が入り、近傍は脅威ノードへ *寄る*
 *    (rally) ようになった。しかしループは開いたままだった:
 *      - 脅威の唯一の源は gate_predict (温度バケツ) → reflex CONSERVE。
 *        「温度計が 35 を超えた」という、何の実体にも接地していない信号。
 *      - 脅威を下げる *行動* が無い。アクチュエータは固定タイマ
 *        (conserve_until) で、応援の力は「温度計の読み」へ注がれ、ループは
 *        どこにも閉じていなかった。
 *      - §2 の「守る対象」に一級の対象 (first-class object) が無かった。
 *
 *    protect.c はこの 3 つを埋める:
 *      1. 守る対象 = 名前付き p-fs オブジェクト (content-addressed block) を
 *         「これは生き延びねばならない」と宣言する登録簿 (= 守る UNIT)。
 *      2. 接地した脅威  = 宣言された対象が、まだ >=R 近傍へ複製されていない
 *         (under-replicated) あいだ脅威は HIGH。複製が進むと DROP する。
 *         脅威は局所/ゴシップ状態だけを読む (§7 中央 argmax なし) — 具体的
 *         には「その対象を保持していると *announce してきた* 別ノードの数」。
 *      3. アクチュエータ = 脅威が立つあいだ、対象を近傍の永続ストアへ複製/
 *         退避させる (pfs_repl 経由で re-announce → WANT → 転送 → 永続化)。
 *         複製が >=R に達すると *測られた脅威が落ちる* — タイマではなく実在の
 *         行動による負帰還。これがループを閉じる。
 *
 *  ── 守る UNIT と守る POWER の分離 (§2) ───────────────────────────────────
 *    守る対象 (block) は所有ノードが「静かに」保持する (protect 宣言時の put は
 *    通常の P1 ambient announce を抑止する)。複製を駆動するのは protect の
 *    アクチュエータだけ = 守る *力* は対象とは別物として明示的に働く。これに
 *    より「アクチュエータを止めると対象は退避されず、所有ノードが死ねば失わ
 *    れる」という対照 (control) が嘘なく成立する。
 *
 *  NO-CENTRAL 不変条件: 登録簿も holder カウントも各ノードのローカル状態。
 *  集約専用ノードは無い。holder は「announce を聞いた」ゴシップから数える。
 */

#pragma once
#include "kernel.h"
#include "pfs_block.h"

/* ------------------------------------------------------------------ */
/* tunables                                                            */
/* ------------------------------------------------------------------ */

#define PROTECT_MAX_OBJS      8     /* registry capacity (per node)         */
#define PROTECT_DEFAULT_R     2     /* desired durable replicas (neighbours) */
#define PROTECT_THREAT_STEP  20     /* threat points per missing replica     */
#define PROTECT_THREAT_MAX   80     /* clamp (<=100; aligns w/ reflex range)  */
#define PROTECT_REANNOUNCE_MS 400   /* actuator re-drive interval while at-risk */
                                    /* (the actuator pushes the unit DIRECTLY to */
                                    /* non-holder neighbours, so one drive places */
                                    /* all replicas; this cadence only re-pushes  */
                                    /* to recover a rare dropped UDP chunk).      */
#define PROTECT_DRIVE_SPACING_MS 60 /* gap between consecutive point announces */
                                    /* (> pfs poll 50ms so each clean, no      */
                                    /* clobber; lets ALL at-risk points kick   */
                                    /* off within one tick = parallel, §5)     */

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Clear the registry and register the pfs_repl announce-hook so heard
 * announces feed the holder count. Call once at boot after pfs_repl_init(). */
void protect_init(void);

/* Periodic task: drives the actuator (protect_tick). Symmetric on every
 * node; create in cmd_net after drpc_my_node is set. */
void protect_task(INT stacd, void *exinf);

/* ------------------------------------------------------------------ */
/* the registry (the protected UNIT)                                   */
/* ------------------------------------------------------------------ */

/* Declare a stored block-id as a protected unit ("this must survive").
 * target_r <= 0 selects PROTECT_DEFAULT_R; it is region-capped so it can
 * never exceed the neighbours actually reachable. Returns 0 (PFS_OK) on
 * success, negative on error (block not held / registry full). */
INT  protect_declare(const U1 id[PFS_ID_LEN], UW len, INT target_r);

/* ------------------------------------------------------------------ */
/* the grounded threat (§2 / §7)                                      */
/* ------------------------------------------------------------------ */

/* This node's grounded threat (0..PROTECT_THREAT_MAX): the worst deficit
 * over all protected objects. 0 when every protected object is safely
 * replicated to >= its target_r neighbours. world.c folds this into the
 * beacon threat axis so neighbours rally toward an at-risk unit. NO timer:
 * the value is purely a function of the real replication state. */
UB   protect_threat_level(void);

/* Pure formula shared by the live path and the self-test (no duplicate
 * definition): threat for ONE object holding `replicas` durable copies
 * against `target_r`. Monotonically non-increasing in replicas; 0 at >=R. */
UB   protect_threat_for(INT replicas, INT target_r);

/* G35/§5: number of DISTINCT protected points that are at-risk right now
 * (0..PROTECT_MAX_OBJS). world.c carries this in the beacon's spare byte so
 * neighbours perceive PLURALITY — that this node defends MANY simultaneous
 * points — which the single aggregate threat scalar folds away. Local-only. */
INT  protect_atrisk_count(void);

/* ------------------------------------------------------------------ */
/* holder accounting (fed by gossip — §7 no central)                  */
/* ------------------------------------------------------------------ */

/* A peer `src_node` announced it holds `id`. If `id` is one of our
 * protected objects, mark src as a holder and recount. This is the only
 * input that lowers the grounded threat — and it is real (a peer only
 * announces a block it actually stored, durably). */
void protect_note_holder(UB src_node, const U1 id[PFS_ID_LEN]);

/* ------------------------------------------------------------------ */
/* the actuator (the protecting POWER)                                */
/* ------------------------------------------------------------------ */

/* Drive replication of every still-at-risk protected object: re-announce
 * it to the region so lacking/late neighbours WANT + pull it into their
 * durable store. Negative feedback: as holders reach target_r the object
 * stops being at-risk and the drive ceases. Gated by protect_set_enabled. */
void protect_tick(UW elapsed_ms);

/* Enable/disable the actuator (the protecting POWER). Default ON.
 * Disabled = control experiment: the protected unit is held quietly and
 * never evacuated, so it stays at-risk and dies with its owner. */
void protect_set_enabled(INT on);

/* ------------------------------------------------------------------ */
/* observability + shell + self-test                                  */
/* ------------------------------------------------------------------ */

void protect_stat(void);

/* shell `protect ...`:
 *   protect <text>     declare sha256(text) as protected (quiet put if new)
 *   protect ls         list protected objects + holder counts + threat
 *   protect stat       full status (registry + actuator + grounded threat)
 *   protect on|off     enable/disable the actuator (control experiment)
 *   protect test       run the grounding + closed-loop self-test          */
void protect_cmd(const UB *args, UW len);

/* Property self-test (host + bare-metal; pure local integer math):
 *   [protect-ground] threat is a monotone function of replicas, 0 at >=R.
 *   [protect-loop]   ACTUATOR ON drives replicas up -> threat falls to 0;
 *                    ACTUATOR OFF leaves replicas at 0 -> threat stays high.
 *                    The drop is tied to replication, never to a timer.
 * Returns 0 on all-PASS. */
INT  protect_self_test(void);
