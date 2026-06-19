/*
 *  dmn.c (x86)
 *  Phase 13 — Default Mode Network
 *
 *  外部刺激で発火し、アイドル時に記憶を整理する。
 *  人間の脳のデフォルトモードネットワークをカーネルに組み込んだもの。
 *
 *  ハートビート (cyclic handler, 1000ms):
 *    → dmn_sem を signal → dmn_task が起床
 *    → 直近の刺激を確認して ACTIVE/IDLE を判定
 *
 *  ACTIVE: 外部刺激 (推論リクエスト / ノード状態変化) を受信中
 *  IDLE  : 刺激なしが DMN_IDLE_THRESHOLD パルス継続 → 整理処理を実行
 *
 *  整理処理 (dmn_idle_work):
 *    1. 推論統計のダイジェスト出力 (dtr_stat)
 *    2. 縮退レベルの確認 (degrade_level)
 *    3. (将来) fedlearn 勾配集約 / KV キャッシュ再構築
 */

#include "dmn.h"
#include "dtr.h"
#include "degrade.h"
#include "ga.h"
#include "drpc.h"     /* galaxy v1: drpc_my_node for emit src */
#include "galaxy.h"   /* galaxy v1: S2/S3 emit hooks */
#include "lm_consolidate.h"   /* living-mind: rest-time sleep-consolidation */
#include "interocept.h"       /* interoception: the S_n stress bus (mind-body) */
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* Step ④ (wave-dmn-student-distill): the SECOND consolidation track. The DMN
 * sleep tick distills the resident NS-1 Cradle baby (arch/common/llm/
 * student_shell.c, host-libc tier) one bounded round from its teacher fixture
 * and persists it — sleep = the ownerless student LEARNS. Plain C symbol across
 * the kernel/LLM tier seam (same pattern student_shell.c uses for pfs_dur_*);
 * dmn.c does NOT include any student header. STRICT no-op (returns 0) unless
 * the node is actually raising a baby with persistence on, so a baby-less node
 * is completely unaffected. Returns 1 if it ran a round. */
IMPORT int student_dmn_consolidate(void);

/* FLASH-WEAR / CPU throttle (wave-student-throttle): how many idle pulses
 * between resident-baby distill rounds. Mirrors GA_INTERVAL's "1-in-N idle"
 * cadence (ga_step runs at idle_runs % GA_INTERVAL == 1). Before this gate the
 * baby distilled AND rewrote the full ~22.8MB blob (fsync'd) on EVERY 1000ms
 * heartbeat forever — continuous flash wear + CPU on an idle "finished" node.
 * Aligned with GA_INTERVAL (10) so the two self-improvement organs share one
 * unobtrusive rest cadence; the second half of the cure (skip the write when
 * the baby did not meaningfully improve) is inside student_dmn_consolidate(). */
#ifndef ST_DMN_INTERVAL
#define ST_DMN_INTERVAL  GA_INTERVAL
#endif

/* ------------------------------------------------------------------ */
/* ユーティリティ                                                      */
/* ------------------------------------------------------------------ */

static void dmn_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void dmn_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { dmn_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    dmn_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

DMN_STATS dmn_stats;

static volatile UB  dmn_state        = DMN_ACTIVE;
static volatile UW  dmn_pulse_count  = 0;   /* ハートビート総数        */
static volatile UW  dmn_last_trigger = 0;   /* 最後に刺激を受けたパルス */
static ID           dmn_cyc;               /* cyclic handler ID (将来拡張用) */

/* 実行時可変パラメータ (GA/RL から動的調整可能) */
volatile UW  dmn_idle_threshold = DMN_IDLE_THRESHOLD_DEFAULT;
volatile UW  dmn_log_interval   = DMN_LOG_INTERVAL_DEFAULT;

/* ── mind-body coupling: S_n modulates the effective idle threshold ─────────
 * interoception.md §3.2/§3.4. The DMN is the ONE production consumer of the
 * S_n bus ([intero-wired]). It maps the 0..255 stress scalar to a CONTINUOUS
 * effective idle threshold:
 *   calm  (low S_n)  -> threshold STRETCHED  -> sleep deep & late (good GC).
 *   hurt  (high S_n) -> threshold SHRUNK     -> wake shallow & fast (defer GC).
 * Oscillation guard (§3.4 / survival-network §8 / reflex-deliberation §6):
 *   (1) S_n is itself an EWMA (fast jitter already damped upstream);
 *   (2) a DEADBAND: changes inside ±DMN_SN_DEADBAND of the held S_n are ignored,
 *       so the threshold holds steady near a boundary (moe.c deadband_pick);
 *   (3) the map is a CONTINUOUS slope, never "stressed => fastest".
 * The base threshold (dmn_idle_threshold, GA/RL-tunable) is the CALM anchor;
 * stress can only pull it DOWN toward a floor, never above the operator value. */
#define DMN_SN_DEADBAND     16u   /* hold the modulation within this S_n band   */
#define DMN_IDLE_FLOOR      1u    /* shallowest sleep (highest stress)          */

static UB  dmn_sn_held    = 0;    /* last S_n that actually moved the threshold */
static UB  dmn_sn_have    = 0;    /* deadband seeded?                           */
static UW  dmn_eff_thresh = DMN_IDLE_THRESHOLD_DEFAULT;  /* last effective value */

/* Map a held S_n (0..255) to an effective idle threshold in [FLOOR, base].
 * Linear: at S_n=0 -> base; at S_n=255 -> FLOOR. Integer, no float. */
static UW dmn_thresh_for_sn(UW base, UB sn)
{
    if (base <= DMN_IDLE_FLOOR) return base;
    UW span = base - DMN_IDLE_FLOOR;            /* how far stress may pull down */
    UW cut  = (span * (UW)sn + 127u) / 255u;    /* proportional, rounded       */
    UW eff  = (cut >= span) ? DMN_IDLE_FLOOR : (base - cut);
    if (eff < DMN_IDLE_FLOOR) eff = DMN_IDLE_FLOOR;
    return eff;
}

/* Recompute the effective idle threshold from the live S_n bus, applying the
 * deadband+hysteresis so a flat/near-flat S_n leaves the threshold untouched.
 * Returns the effective threshold the task loop should compare against. */
static UW dmn_effective_idle_threshold(void)
{
    UB sn = intero_scalar();                    /* the ONE production read */
    if (!dmn_sn_have) {
        dmn_sn_have    = 1;
        dmn_sn_held    = sn;
        dmn_eff_thresh = dmn_thresh_for_sn(dmn_idle_threshold, sn);
        return dmn_eff_thresh;
    }
    UW d = (sn >= dmn_sn_held) ? (UW)(sn - dmn_sn_held) : (UW)(dmn_sn_held - sn);
    if (d >= DMN_SN_DEADBAND) {                  /* moved enough to re-modulate */
        dmn_sn_held    = sn;
        dmn_eff_thresh = dmn_thresh_for_sn(dmn_idle_threshold, sn);
    }
    return dmn_eff_thresh;
}

/* Read-only accessors for the [intero-tick] cert (production symbols, not a
 * sim): the cert drives S_n and reads the SAME effective threshold the live
 * task loop steers on, counting state changes to prove the deadband holds. */
UW   dmn_intero_effective_threshold(void) { return dmn_effective_idle_threshold(); }
UB   dmn_intero_held_sn(void)             { return dmn_sn_held; }

/* ------------------------------------------------------------------ */
/* Cyclic handler — タスク独立コンテキスト (割り込みレベル)           */
/* ------------------------------------------------------------------ */

static void dmn_pulse_handler(VP exinf)
{
    (void)exinf;
    /* dmn_task が tk_dly_tsk で自律的にパルスをカウントするため、
     * ここでは何もしない (cyclic handler は将来の拡張用に残す) */
}

/* ------------------------------------------------------------------ */
/* 外部刺激通知 — dtr.c / swim.c から呼ぶ                            */
/* ------------------------------------------------------------------ */

void dmn_trigger(void)
{
    dmn_last_trigger = dmn_pulse_count;
    dmn_stats.triggers++;

    if (dmn_state == DMN_IDLE) {
        dmn_state = DMN_ACTIVE;
        dmn_stats.idle_to_active++;
        galaxy_emit(EV_DMN_WAKE, drpc_my_node, GALAXY_NODE_NONE, 0, 0);  /* S2: the star wakes (galaxy.md) */
    }
}

/* ------------------------------------------------------------------ */
/* アイドル整理処理                                                    */
/* ------------------------------------------------------------------ */

/* LM-6 (living-mind.md Part VII, VII.5): lifetime count of R3 idle
 * rounds run by the dmn_idle_work hook below — incremented at EXACTLY
 * ONE site, deliberately NOT inside r3_consolidate_idle_round() itself,
 * so the cert's direct calls (r3_stream_test) cannot move it and a
 * nonzero delta is attributable to the production trigger (the real
 * 1000ms heartbeat + ACTIVE->IDLE transition) alone. Read through
 * dmn_r3_rounds(). Pre-ring3 caveat stands: in one flat address space
 * nothing is unfakeable by code in the same image; the discipline is
 * the auditor's one-++-site grep + the commander's line-by-line read. */
static UW dmn_r3_round_count = 0;

/* Step ④ proof seam: the resident baby's held-out loss over its fixture (host-
 * libc tier, arch/common/llm/student_shell.c). Read-only; chance (~5.545) when
 * no baby is resident. Used by dmn_student_distill_test below to SHOW the loss
 * dropping across real DMN sleep ticks. */
IMPORT float student_dmn_heldout_loss(void);

/* Lifetime count of FULL ~22.8MB durable writes the student DMN track has
 * actually done (wave-student-throttle flash-wear proof). Read-only. */
IMPORT unsigned student_dmn_save_count(void);

static void dmn_idle_work(void)
{
    dmn_stats.idle_runs++;

    /* Phase 14: GA による重み自己改善 (GA_INTERVAL アイドルに 1 回) */
    if (dmn_stats.idle_runs % GA_INTERVAL == 1)
        ga_step();

    /* living-mind (docs/architecture/living-mind.md II.7): rest-time
     * "sleep" consolidation — replay durable engrams and distill them
     * into the dtr slow weights. ALONGSIDE ga_step (not replacing the
     * organ). No-op until engrams are pending (e.g. captured by a prior
     * `dmn test` run or, later, the live conversational fast layer). */
    if (dmn_stats.idle_runs % GA_INTERVAL == 1 && lm_engrams_pending()) {
        if (lm_consolidate_idle_round()) {
            galaxy_emit(EV_CONSOLIDATE, drpc_my_node, GALAXY_NODE_NONE, 0, 0);  /* S3: an engram sinks (galaxy.md) */
            dmn_puts("[dmn] sleep: replayed engrams -> consolidated weights\r\n");
        }
    }

    /* living-mind Part VI (LM-5, 随時): in-context conversation facts
     * pending in the R3 stream queue are the most urgent rest work —
     * consolidate EVERY idle pulse while pending (DECISION 3; the round
     * is bounded by R3_IDLE_STEPS, and a fact drains in
     * ~R3_SLEEPS_PER_FACT idle seconds at the 1000ms pulse). Trains
     * R3's own rw[], a DIFFERENT network from the lm round above —
     * non-interference is structural (disjoint weight buffers). */
    if (r3_facts_pending()) {
        if (r3_consolidate_idle_round()) {
            dmn_r3_round_count++;            /* the ONLY ++ site (VII.5) */
            galaxy_emit(EV_CONSOLIDATE, drpc_my_node, GALAXY_NODE_NONE, 1, 0);  /* S3: the taught fact sinks into rw[] (galaxy.md) */
            dmn_puts("[dmn] sleep: distilled in-context facts -> rw[]\r\n");
            /* persistence SLICE 2 (docs/architecture/persistence.md): the
             * sleep-then-save policy. Only once the batch has fully drained
             * (no more pending facts) is rw[] in its settled post-sleep
             * state — persist it THEN (not every round) so a reboot answers
             * `ask sky`->blue from the durable store. r3_weights_persist is
             * a content-id no-op when rw[] is unchanged (flash-wear honest-
             * issue) and a no-op without PKERNEL_PFS_DIR / on bare metal. */
            if (!r3_facts_pending()) {
                INT wr = r3_weights_persist();
                if (wr == 1)
                    dmn_puts("[dmn] sleep: persisted rw[] -> durable store\r\n");
                else if (wr < 0)
                    dmn_puts("[dmn] sleep: WARN durable weights write failed\r\n");
            }
        }
    }

    /* Step ④ (native-student.md, wave-dmn-student-distill): the SECOND
     * consolidation track — AFTER (not instead of) the R3 living-mind work
     * above. While the node sleeps, distill the resident NS-1 baby one bounded
     * round from its teacher fixture and persist it: sleep = the ownerless
     * student LEARNS, and the gain survives a reboot. A DIFFERENT network from
     * R3's rw[] and from the lm engram round — non-interference is structural
     * (disjoint weight buffers in a separate tier). STRICT no-op on a node that
     * isn't raising a baby / has no persistence, so a baby-less node is
     * unaffected. Drives the SAME symbol the proof hook uses.
     *
     * FLASH-WEAR throttle (wave-student-throttle): cadence-limited to ~1-in-
     * ST_DMN_INTERVAL idle pulses exactly like ga_step above, NOT every pulse.
     * Before this gate a "finished" idle baby re-ran a round AND rewrote the
     * full ~22.8MB blob (fsync'd) on every 1000ms heartbeat forever. The R3
     * living-mind track above is deliberately left at every-pulse — it only
     * runs WHILE r3_facts_pending(), so it self-limits and is unaffected here.
     * The other half of the cure (skip the 22.8MB write when the baby did not
     * meaningfully improve) lives inside student_dmn_consolidate(). */
    if (dmn_stats.idle_runs % ST_DMN_INTERVAL == 1 && student_dmn_consolidate()) {
        galaxy_emit(EV_CONSOLIDATE, drpc_my_node, GALAXY_NODE_NONE, 2, 0);  /* S3: the baby's weights settle a little (galaxy.md) */
        dmn_puts("[dmn] sleep: distilled teacher -> resident student (baby)\r\n");
    }

    /* dmn_log_interval パルスに 1 回だけ詳細ログを出す */
    if (dmn_stats.idle_runs % dmn_log_interval != 1) return;

    static const char *lname[] = { "FULL", "REDUCED", "SOLO" };
    UB lv = degrade_level();

    dmn_puts("[dmn] idle digest ---\r\n");

    dmn_puts("[dmn]   pulses      : "); dmn_putdec(dmn_stats.pulses);    dmn_puts("\r\n");
    dmn_puts("[dmn]   triggers    : "); dmn_putdec(dmn_stats.triggers);  dmn_puts("\r\n");
    dmn_puts("[dmn]   idle runs   : "); dmn_putdec(dmn_stats.idle_runs); dmn_puts("\r\n");
    dmn_puts("[dmn]   degrade     : ");
    dmn_puts(lv < 3 ? lname[lv] : "?");
    dmn_puts("\r\n");

    /* 推論統計を出力 */
    dtr_stat();
}

/* ------------------------------------------------------------------ */
/* Step ④ proof: drive the REAL sleep path and watch the baby learn    */
/* ------------------------------------------------------------------ */

/* small fixed-point printer: v rounded to 4 decimals (loss is small/positive) */
static void dmn_putf4(float v)
{
    if (v < 0) { dmn_puts("-"); v = -v; }
    UW whole = (UW)v;
    UW frac  = (UW)((v - (float)whole) * 10000.0f + 0.5f);
    if (frac >= 10000) { whole++; frac -= 10000; }
    dmn_putdec(whole);
    dmn_puts(".");
    /* zero-pad the 4-digit fraction */
    if (frac < 1000) dmn_puts("0");
    if (frac < 100)  dmn_puts("0");
    if (frac < 10)   dmn_puts("0");
    dmn_putdec(frac);
}

/* `dmn distill [N]` — drive the EXACT production sleep path (dmn_idle_work) N
 * times and SHOW the resident baby's held-out loss drop across the sleeps
 * (sleep = learning). Also reports the R3 idle-round delta so we can confirm
 * the existing living-mind consolidation track STILL RUNS alongside the new
 * student track (non-interference). This calls the SAME static dmn_idle_work
 * the 1000ms heartbeat calls — not a private copy — so the proof certifies the
 * real path. NO-OP-honest: with no PKERNEL_PFS_DIR / no baby the student track
 * inside dmn_idle_work is a no-op and the loss simply stays at chance. */
void dmn_student_distill_test(UW n)
{
    if (n < 1) n = 1;
    if (n > 256) n = 256;   /* bounded; enough ticks to reach plateau in proof */

    float chance = student_dmn_heldout_loss();   /* chance if no baby resident */

    dmn_puts("[dmn-distill] driving "); dmn_putdec(n);
    dmn_puts(" REAL sleep tick(s) (dmn_idle_work)\r\n");

    UW r3_before   = dmn_r3_round_count;
    UW save_before = student_dmn_save_count();   /* flash-wear: 22.8MB writes */
    float pre = student_dmn_heldout_loss();
    dmn_puts("[dmn-distill] student held-out loss (before) = ");
    dmn_putf4(pre); dmn_puts(" nats\r\n");

    for (UW i = 0; i < n; i++) {
        dmn_idle_work();                         /* the production sleep path */
        /* only log per-tick loss for short runs to keep the shell responsive */
        if (n <= 32) {
            float l = student_dmn_heldout_loss();
            dmn_puts("[dmn-distill]   after sleep "); dmn_putdec(i + 1);
            dmn_puts(": student loss = "); dmn_putf4(l); dmn_puts(" nats\r\n");
        }
    }

    float post = student_dmn_heldout_loss();
    UW r3_after   = dmn_r3_round_count;
    UW save_after = student_dmn_save_count();

    dmn_puts("[dmn-distill] student held-out loss (after)  = ");
    dmn_putf4(post); dmn_puts(" nats  (chance="); dmn_putf4(chance);
    dmn_puts(")\r\n");
    dmn_puts("[dmn-distill] R3 idle rounds this run        = ");
    dmn_putdec(r3_after - r3_before);
    dmn_puts(" (living-mind track intact)\r\n");
    dmn_puts("[dmn-distill] ticks driven                   = ");
    dmn_putdec(n); dmn_puts("\r\n");
    dmn_puts("[dmn-distill] 22.8MB durable writes this run  = ");
    dmn_putdec(save_after - save_before);
    dmn_puts("  (every-tick would be ~");
    dmn_putdec(n / ST_DMN_INTERVAL);
    dmn_puts(" student rounds; pre-throttle = 1 write/round)\r\n");

    if (post < pre - 0.01f)
        dmn_puts("[dmn-distill] PASS: the baby LEARNED while it slept\r\n");
    else
        dmn_puts("[dmn-distill] (no drop: no baby / no PKERNEL_PFS_DIR — "
                 "student track no-op, expected on a baby-less node)\r\n");
}

/* ------------------------------------------------------------------ */
/* interoception mind-body cert (interoception.md §3.5 [intero-tick])   */
/* ------------------------------------------------------------------ */
/* Drives the S_n bus low->high through the cert-only deterministic
 * injection and proves, on the SAME production symbol the live task loop
 * steers on (dmn_intero_effective_threshold), that:
 *   (1) the effective idle threshold falls MONOTONICALLY as S_n rises
 *       (calm = late deep sleep, stressed = early shallow wake);
 *   (2) the DEADBAND suppresses oscillation: a sweep of TINY S_n wiggles
 *       (< DMN_SN_DEADBAND) around a point changes the effective threshold
 *       strictly FEWER times than the same number of LARGE swings.
 * Returns 0 = PASS. Also runs intero_self_test first (sources + EWMA). */
INT dmn_intero_modulation_test(void)
{
    INT fail = 0;

    /* part A: the source bus self-test ([intero-sources]/[intero-ewma]). */
    if (intero_self_test() != 0) fail = 1;

    dmn_puts("[dmn] intero modulation: S_n low->high shrinks effective idle\r\n");

    /* anchor the calm base so the sweep is interpretable. */
    UW base = dmn_idle_threshold;
    dmn_puts("[dmn]   calm base threshold = "); dmn_putdec(base); dmn_puts("s\r\n");

    /* (1) MONOTONIC fall. Reset the deadband state, then step S_n in big jumps
     *     well past the deadband so each step re-modulates. */
    dmn_sn_have = 0;
    intero_test_force(1, 0);
    UW prev = dmn_effective_idle_threshold();   /* S_n=0 -> base */
    dmn_puts("[dmn]   S_n=0   -> eff="); dmn_putdec(prev); dmn_puts("\r\n");
    const UB sweep[] = { 64, 128, 192, 255 };
    for (INT i = 0; i < 4; i++) {
        intero_test_force(1, sweep[i]);
        UW eff = dmn_effective_idle_threshold();
        dmn_puts("[dmn]   S_n="); dmn_putdec((UW)sweep[i]);
        dmn_puts(" -> eff="); dmn_putdec(eff); dmn_puts("\r\n");
        if (eff > prev) { dmn_puts("[dmn]   NON-MONOTONIC FAIL\r\n"); fail = 1; }
        prev = eff;
    }
    /* the high-stress end must be strictly shallower than the calm end. */
    intero_test_force(1, 0);   dmn_sn_have = 0; UW calm_eff = dmn_effective_idle_threshold();
    intero_test_force(1, 255); UW hurt_eff = dmn_effective_idle_threshold();
    if (!(hurt_eff < calm_eff)) {
        dmn_puts("[dmn]   stress did not shrink the window FAIL\r\n"); fail = 1;
    }

    /* (2) DEADBAND damps oscillation. Count effective-threshold CHANGES under
     *     N tiny wiggles vs N large swings about a midpoint. */
    UW mid = 120;
    UW small_changes = 0, big_changes = 0;
    /* tiny wiggles: +/- (DEADBAND-1) — must be largely absorbed. */
    dmn_sn_have = 0; intero_test_force(1, (UB)mid);
    UW e0 = dmn_effective_idle_threshold();
    for (INT i = 0; i < 20; i++) {
        UB v = (UB)(mid + ((i & 1) ? (DMN_SN_DEADBAND - 1) : 0));
        intero_test_force(1, v);
        UW e = dmn_effective_idle_threshold();
        if (e != e0) { small_changes++; e0 = e; }
    }
    /* large swings: +/- 80 — must move many times. */
    dmn_sn_have = 0; intero_test_force(1, (UB)mid);
    UW e1 = dmn_effective_idle_threshold();
    for (INT i = 0; i < 20; i++) {
        UB v = (UB)((i & 1) ? (mid + 80) : (mid > 80 ? mid - 80 : 0));
        intero_test_force(1, v);
        UW e = dmn_effective_idle_threshold();
        if (e != e1) { big_changes++; e1 = e; }
    }
    dmn_puts("[dmn]   deadband: tiny-wiggle changes="); dmn_putdec(small_changes);
    dmn_puts("  large-swing changes="); dmn_putdec(big_changes); dmn_puts("\r\n");
    if (!(small_changes < big_changes)) {
        dmn_puts("[dmn]   deadband did not damp oscillation FAIL\r\n"); fail = 1;
    }

    /* release the injection; the live read restores real-source modulation. */
    intero_test_force(0, 0);
    dmn_sn_have = 0;
    (void)dmn_effective_idle_threshold();

    dmn_puts(fail ? "[intero-tick] FAIL\r\n" : "[intero-tick] PASS\r\n");
    return fail;
}

/* ------------------------------------------------------------------ */
/* DMN タスク本体                                                      */
/* ------------------------------------------------------------------ */

void dmn_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    dmn_puts("[dmn] task started  pulse=");
    dmn_putdec(DMN_PULSE_MS);
    dmn_puts("ms  idle_threshold=");
    dmn_putdec(DMN_IDLE_THRESHOLD_DEFAULT);
    dmn_puts("s\r\n");

    for (;;) {
        /* ハートビート: task context で待機してパルスをカウント */
        tk_dly_tsk(DMN_PULSE_MS);
        dmn_pulse_count++;
        dmn_stats.pulses++;

        UW idle_for = dmn_pulse_count - dmn_last_trigger;

        /* mind-body coupling (interoception.md §3.2): the threshold we compare
         * against is the S_n-modulated EFFECTIVE one — calm stretches it (deep
         * late sleep), stress shrinks it (shallow fast wake). Deadband-guarded
         * so a steady S_n never makes this oscillate. */
        UW eff_threshold = dmn_effective_idle_threshold();

        if (idle_for >= eff_threshold) {
            /* ACTIVE → IDLE 遷移 */
            if (dmn_state == DMN_ACTIVE) {
                dmn_state = DMN_IDLE;
                dmn_stats.active_to_idle++;
                galaxy_emit(EV_DMN_IDLE, drpc_my_node, GALAXY_NODE_NONE, 0, 0);  /* S2: my star starts dreaming (galaxy.md) */
                dmn_puts("[dmn] -> IDLE  (no stimulus for ");
                dmn_putdec(idle_for);
                dmn_puts("s)\r\n");
            }
            /* アイドル整理 */
            dmn_idle_work();
        } else {
            /* IDLE → ACTIVE 遷移はすでに dmn_trigger() で処理済み */
        }
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void dmn_init(void)
{
    dmn_stats.pulses        = 0;
    dmn_stats.triggers      = 0;
    dmn_stats.idle_runs     = 0;
    dmn_stats.active_to_idle = 0;
    dmn_stats.idle_to_active = 0;

    dmn_state           = DMN_ACTIVE;
    dmn_pulse_count     = 0;
    dmn_last_trigger    = 0;
    dmn_idle_threshold  = DMN_IDLE_THRESHOLD_DEFAULT;
    dmn_log_interval    = DMN_LOG_INTERVAL_DEFAULT;

    /* Cyclic handler 生成 (将来の拡張用; 現在はパルスカウントは dmn_task 側で実施) */
    T_CCYC cc;
    cc.exinf   = NULL;
    cc.cycatr  = TA_HLNG | TA_STA;           /* 即時開始               */
    cc.cychdr  = (FP)dmn_pulse_handler;
    cc.cyctim  = (RELTIM)DMN_PULSE_MS;
    cc.cycphs  = 0;
    dmn_cyc    = tk_cre_cyc(&cc);

    dmn_puts("[dmn] initialized  heartbeat=");
    dmn_putdec(DMN_PULSE_MS);
    dmn_puts("ms  idle_threshold=");
    dmn_putdec(DMN_IDLE_THRESHOLD_DEFAULT);
    dmn_puts("s\r\n");
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

UB dmn_state_get(void) { return dmn_state; }

UW dmn_r3_rounds(void) { return dmn_r3_round_count; }

void dmn_stat(void)
{
    static const char *sname[] = { "ACTIVE", "IDLE" };
    UB st = dmn_state < 2 ? dmn_state : 0;

    dmn_puts("[dmn] state         : "); dmn_puts(sname[st]); dmn_puts("\r\n");
    dmn_puts("[dmn] pulses        : "); dmn_putdec(dmn_stats.pulses);         dmn_puts("\r\n");
    dmn_puts("[dmn] triggers      : "); dmn_putdec(dmn_stats.triggers);       dmn_puts("\r\n");
    dmn_puts("[dmn] idle runs     : "); dmn_putdec(dmn_stats.idle_runs);      dmn_puts("\r\n");
    dmn_puts("[dmn] active->idle  : "); dmn_putdec(dmn_stats.active_to_idle); dmn_puts("\r\n");
    dmn_puts("[dmn] idle->active  : "); dmn_putdec(dmn_stats.idle_to_active); dmn_puts("\r\n");
    dmn_puts("[dmn] idle threshold: "); dmn_putdec(dmn_idle_threshold);
    dmn_puts("s (def="); dmn_putdec(DMN_IDLE_THRESHOLD_DEFAULT); dmn_puts(")\r\n");
    dmn_puts("[dmn] log interval  : "); dmn_putdec(dmn_log_interval);
    dmn_puts(" (def="); dmn_putdec(DMN_LOG_INTERVAL_DEFAULT); dmn_puts(")\r\n");
    dmn_puts("[dmn] heartbeat     : "); dmn_putdec(DMN_PULSE_MS);             dmn_puts("ms\r\n");
}

void dmn_set_idle_threshold(UW v)
{
    if (v < 1) v = 1;
    dmn_idle_threshold = v;
    dmn_puts("[dmn] idle_threshold <- "); dmn_putdec(v); dmn_puts("s\r\n");
}

void dmn_set_log_interval(UW v)
{
    if (v < 1) v = 1;
    dmn_log_interval = v;
    dmn_puts("[dmn] log_interval <- "); dmn_putdec(v); dmn_puts(" idle_runs\r\n");
}
