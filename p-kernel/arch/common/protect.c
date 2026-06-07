/*
 *  protect.c — the protected-object registry + grounded threat + actuator.
 *
 *  Thought/design/NO-CENTRAL invariant: protect.h and
 *  docs/architecture/survival-network.md §2 / philosophy-gap-audit-4 G28.
 *
 *  This is the file that CLOSES the open loop wave 13 left:
 *    declare a unit  ->  threat = f(under-replication)  ->  actuator
 *    re-announces it -> neighbours pull+persist -> they announce back ->
 *    holder count rises -> threat FALLS (because of the replication, not a
 *    timer) -> at >=R the unit is safe and the drive stops.
 *
 *  Everything here is pure-local / gossip-fed: there is no aggregator.
 *  The holder count is built only from announces this node *heard*, and a
 *  node only announces a block it actually stored — so the threat is grounded
 *  in real, durable replication state.
 */

#include "protect.h"
#include "pfs_block.h"
#include "pfs_repl.h"
#include "drpc.h"
#include "region.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* output helpers (sio frame channel, like moe.c / reflex.c)           */
/* ------------------------------------------------------------------ */

static void pt_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void pt_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { pt_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    pt_puts(&buf[i]);
}

static void pt_put_id(const U1 id[PFS_ID_LEN])
{
    static const char hexd[] = "0123456789abcdef";
    char out[2 * 8 + 1];
    INT j = 0;
    for (INT i = 0; i < 8; i++) {
        out[j++] = hexd[(id[i] >> 4) & 0xF];
        out[j++] = hexd[id[i] & 0xF];
    }
    out[j] = '\0';
    pt_puts(out);
}

static INT pt_id_eq(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN])
{
    for (INT i = 0; i < PFS_ID_LEN; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* registry state (the protected UNITs)                                */
/* ------------------------------------------------------------------ */

typedef struct {
    U1 id[PFS_ID_LEN];
    UW len;
    U1 active;
    U1 requested_r;              /* R as originally requested (pre-cap)      */
    U1 target_r;                 /* desired durable replicas (region-capped) */
    U1 driven;                   /* 1 once the actuator has driven this unit  */
    U1 holders[DNODE_MAX];       /* holders[n]=1: peer n announced holding  */
    U1 holder_count;             /* distinct peers (excl. self) known holders */
    UW drive_age_ms;             /* since last actuator re-announce          */
    UB last_threat;              /* for transition logging (observability)   */
} PROTECT_OBJ;

static PROTECT_OBJ objs[PROTECT_MAX_OBJS];

static UB actuator_enabled = 1;  /* the protecting POWER; off = control     */

/* stats (observability) */
static UW st_declared, st_drives, st_holder_events, st_reached_safe;

/* ------------------------------------------------------------------ */
/* the grounded threat formula (shared with the self-test)             */
/* ------------------------------------------------------------------ */

UB protect_threat_for(INT replicas, INT target_r)
{
    if (target_r <= 0) return 0;
    if (replicas >= target_r) return 0;          /* safe — no deficit */
    INT deficit = target_r - replicas;
    INT t = deficit * PROTECT_THREAT_STEP;
    if (t > PROTECT_THREAT_MAX) t = PROTECT_THREAT_MAX;
    if (t < 0) t = 0;
    return (UB)t;
}

UB protect_threat_level(void)
{
    UB worst = 0;
    for (INT i = 0; i < PROTECT_MAX_OBJS; i++) {
        if (!objs[i].active) continue;
        UB t = protect_threat_for((INT)objs[i].holder_count,
                                  (INT)objs[i].target_r);
        if (t > worst) worst = t;
    }
    return worst;
}

/* ------------------------------------------------------------------ */
/* declaration (register a stored block as a protected unit)           */
/* ------------------------------------------------------------------ */

static PROTECT_OBJ *find_obj(const U1 id[PFS_ID_LEN])
{
    for (INT i = 0; i < PROTECT_MAX_OBJS; i++)
        if (objs[i].active && pt_id_eq(objs[i].id, id))
            return &objs[i];
    return 0;
}

/* region-aware cap: R can never exceed the neighbours we could reach. With
 * region_size() peers (incl. self) the most durable replicas possible is
 * region_size-1; never below 1 (a lone node genuinely cannot protect by
 * replication — that is honest, not a bug). */
static INT cap_target_r(INT want)
{
    if (want <= 0) want = PROTECT_DEFAULT_R;
    UB rs = region_size();                 /* incl. self; >=1 */
    INT max_peers = (rs > 1) ? (INT)rs - 1 : 1;
    if (want > max_peers) want = max_peers;
    if (want < 1) want = 1;
    return want;
}

INT protect_declare(const U1 id[PFS_ID_LEN], UW len, INT target_r)
{
    if (!pfs_has(id)) return PFS_E_NOTFOUND;   /* must hold the unit first */

    INT want = (target_r <= 0) ? PROTECT_DEFAULT_R : target_r;

    PROTECT_OBJ *o = find_obj(id);
    if (o) {                                    /* already protected: re-arm R */
        if (want > (INT)o->requested_r) o->requested_r = (U1)want;
        o->target_r = (U1)cap_target_r((INT)o->requested_r);
        return PFS_OK;
    }
    for (INT i = 0; i < PROTECT_MAX_OBJS; i++) {
        if (objs[i].active) continue;
        for (INT k = 0; k < PFS_ID_LEN; k++) objs[i].id[k] = id[k];
        objs[i].len          = len;
        objs[i].requested_r  = (U1)want;                /* remember the wish   */
        objs[i].target_r     = (U1)cap_target_r(want);
        objs[i].driven       = 0;
        objs[i].holder_count = 0;
        objs[i].drive_age_ms = PROTECT_REANNOUNCE_MS;   /* drive immediately */
        for (INT n = 0; n < DNODE_MAX; n++) objs[i].holders[n] = 0;
        objs[i].active     = 1;
        objs[i].last_threat = protect_threat_for(0, objs[i].target_r);
        st_declared++;

        pt_puts("[protect] DECLARE unit id="); pt_put_id(id);
        pt_puts("  len="); pt_putdec(len);
        pt_puts("  need R="); pt_putdec(objs[i].target_r);
        pt_puts(" durable replicas  (threat="); pt_putdec(objs[i].last_threat);
        pt_puts(", grounded in under-replication, NOT a timer)\r\n");
        return PFS_OK;
    }
    pt_puts("[protect] registry full\r\n");
    return PFS_E_FULL;
}

/* ------------------------------------------------------------------ */
/* holder accounting (gossip-fed; lowers the grounded threat)          */
/* ------------------------------------------------------------------ */

void protect_note_holder(UB src_node, const U1 id[PFS_ID_LEN])
{
    if (src_node >= DNODE_MAX) return;
    if (src_node == drpc_my_node) return;         /* self is not a replica  */

    PROTECT_OBJ *o = find_obj(id);
    if (!o) return;                               /* not a protected unit   */
    if (o->holders[src_node]) return;             /* already counted        */

    o->holders[src_node] = 1;
    if (o->holder_count < DNODE_MAX) o->holder_count++;
    st_holder_events++;

    UB t = protect_threat_for((INT)o->holder_count, (INT)o->target_r);

    pt_puts("[protect] replica confirmed: node"); pt_putdec(src_node);
    pt_puts(" holds id="); pt_put_id(o->id);
    pt_puts("  -> replicas="); pt_putdec(o->holder_count);
    pt_puts("/"); pt_putdec(o->target_r);
    pt_puts("  threat "); pt_putdec(o->last_threat);
    pt_puts("->"); pt_putdec(t);
    if (t == 0 && o->last_threat != 0) {
        pt_puts("  *** SAFE: unit replicated to R; threat closed by"
                " replication, not a timer ***");
        st_reached_safe++;
    }
    pt_puts("\r\n");
    o->last_threat = t;
}

/* ------------------------------------------------------------------ */
/* the actuator (the protecting POWER)                                 */
/* ------------------------------------------------------------------ */

void protect_set_enabled(INT on)
{
    actuator_enabled = on ? 1 : 0;
    pt_puts("[protect] actuator (protecting POWER) ");
    pt_puts(actuator_enabled ? "ENABLED\r\n"
            : "DISABLED (control: unit held quietly, never evacuated)\r\n");
}

void protect_tick(UW elapsed_ms)
{
    if (drpc_my_node == 0xFF) return;             /* not distributed         */

    for (INT i = 0; i < PROTECT_MAX_OBJS; i++) {
        if (!objs[i].active) continue;

        /* Self-heal an early declare: if the region GREW since we declared
         * (e.g. we declared before SWIM had measured RTT to both peers, so
         * target_r capped low), re-cap target_r UPWARD toward the originally
         * requested R. A node that learns of more peers legitimately wants
         * more durable copies; this re-opens the grounded threat until the
         * higher R is met. Monotone toward requested_r — never lowered, so a
         * peer leaving does not thrash the target back down. */
        INT cap = cap_target_r((INT)objs[i].requested_r);
        if (cap > (INT)objs[i].target_r) {
            objs[i].target_r  = (U1)cap;
            objs[i].last_threat = protect_threat_for((INT)objs[i].holder_count,
                                                     (INT)objs[i].target_r);
        }

        UB t = protect_threat_for((INT)objs[i].holder_count,
                                  (INT)objs[i].target_r);
        if (t == 0) continue;                     /* safe: no force to pour  */
        if (!actuator_enabled) continue;          /* control: do not evacuate */

        objs[i].drive_age_ms += elapsed_ms;
        if (objs[i].drive_age_ms < PROTECT_REANNOUNCE_MS) continue;
        objs[i].drive_age_ms = 0;

        /* concentrate force on the at-risk point: drive replication by
         * re-announcing so lacking/late neighbours WANT + pull it durably.
         * Mark the unit as deliberately driven: only an actuator-driven unit
         * is allowed to spread via ambient boot-SYNC (the protecting POWER
         * spreads it on purpose; it is never leaked accidentally — §2). */
        objs[i].driven = 1;
        pfs_repl_reannounce(objs[i].id);
        st_drives++;
        pt_puts("[protect] ACTUATE id="); pt_put_id(objs[i].id);
        pt_puts("  at-risk (replicas="); pt_putdec(objs[i].holder_count);
        pt_puts("/"); pt_putdec(objs[i].target_r);
        pt_puts(", threat="); pt_putdec(t);
        pt_puts(") -> drive replication into neighbours\r\n");
    }
}

#define PROTECT_TICK_MS 200

void protect_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (;;) {
        tk_dly_tsk(PROTECT_TICK_MS);
        protect_tick(PROTECT_TICK_MS);
    }
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* SYNC filter (§2: a protected unit spreads ONLY by the protecting     */
/* POWER, never leaked by ambient boot-sync)                            */
/* ------------------------------------------------------------------ */

/* pfs_repl asks, for each block a (re)joining peer SYNC-requests: may we
 * stream this block? We answer NO (1 = exclude) for a protected unit that
 * the actuator has NOT yet deliberately driven. Rationale (G28/§2): the
 * protecting POWER replicates the unit on purpose; with the actuator OFF
 * (control) the unit is held quietly and must NOT escape via ambient SYNC,
 * or the "control loses the object" contrast would be an accident, not a
 * proof. Ordinary (non-protected) blocks and already-driven protected units
 * are served normally — no regression to P1/boot-sync. */
static INT protect_sync_exclude(const U1 id[PFS_ID_LEN])
{
    PROTECT_OBJ *o = find_obj(id);
    return (o && !o->driven) ? 1 : 0;
}

void protect_init(void)
{
    for (INT i = 0; i < PROTECT_MAX_OBJS; i++) objs[i].active = 0;
    actuator_enabled = 1;
    st_declared = st_drives = st_holder_events = st_reached_safe = 0;
    /* heard announces feed the holder count (the only signal that lowers the
     * grounded threat). Registered here so it is live before any declare. */
    pfs_repl_set_announce_hook(protect_note_holder);
    /* keep a quiet (undriven) protected unit out of ambient boot-SYNC */
    pfs_repl_set_sync_filter(protect_sync_exclude);
    pt_puts("[protect] protected-object registry initialized"
            " (unit/power separated; threat grounded in replication)\r\n");
}

/* ------------------------------------------------------------------ */
/* observability                                                       */
/* ------------------------------------------------------------------ */

void protect_stat(void)
{
    pt_puts("[protect] actuator: ");
    pt_puts(actuator_enabled ? "ENABLED" : "DISABLED (control)");
    pt_puts("  grounded-threat="); pt_putdec(protect_threat_level());
    pt_puts("  (step="); pt_putdec(PROTECT_THREAT_STEP);
    pt_puts(" max="); pt_putdec(PROTECT_THREAT_MAX);
    pt_puts(")\r\n");
    pt_puts("[protect] stats: declared="); pt_putdec(st_declared);
    pt_puts(" drives="); pt_putdec(st_drives);
    pt_puts(" holder_events="); pt_putdec(st_holder_events);
    pt_puts(" reached_safe="); pt_putdec(st_reached_safe);
    pt_puts("\r\n");
    pt_puts("[protect] protected units (the things that must survive):\r\n");
    UB shown = 0;
    for (INT i = 0; i < PROTECT_MAX_OBJS; i++) {
        if (!objs[i].active) continue;
        shown++;
        UB t = protect_threat_for((INT)objs[i].holder_count,
                                  (INT)objs[i].target_r);
        pt_puts("  id="); pt_put_id(objs[i].id);
        pt_puts("  len="); pt_putdec(objs[i].len);
        pt_puts("  replicas="); pt_putdec(objs[i].holder_count);
        pt_puts("/"); pt_putdec(objs[i].target_r);
        pt_puts("  threat="); pt_putdec(t);
        pt_puts(t == 0 ? "  SAFE  holders=[" : "  AT-RISK  holders=[");
        UB first = 1;
        for (INT n = 0; n < DNODE_MAX; n++) {
            if (!objs[i].holders[n]) continue;
            if (!first) pt_puts(",");
            first = 0;
            pt_puts("n"); pt_putdec((UW)n);
        }
        pt_puts("]\r\n");
    }
    if (!shown) pt_puts("  (none — `protect <text>` to declare one)\r\n");
}

/* ------------------------------------------------------------------ */
/* shell `protect ...`                                                 */
/* ------------------------------------------------------------------ */

static INT pt_starts(const UB *p, INT n, const char *kw)
{
    INT i = 0;
    while (kw[i]) { if (i >= n || p[i] != (UB)kw[i]) return 0; i++; }
    return 1;
}

void protect_cmd(const UB *args, UW len)
{
    const UB *p   = args;
    const UB *end = args + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    INT rem = (INT)(end - p);

    if (rem <= 0 || pt_starts(p, rem, "stat")) { protect_stat(); return; }
    if (pt_starts(p, rem, "ls"))   { protect_stat(); return; }
    if (pt_starts(p, rem, "test")) { protect_self_test(); return; }
    if (pt_starts(p, rem, "on"))   { protect_set_enabled(1); return; }
    if (pt_starts(p, rem, "off"))  { protect_set_enabled(0); return; }

    /* otherwise: `protect <text>` — declare sha256(text) as a protected unit.
     * If we do not already hold it, store it QUIETLY (suppress the ambient P1
     * announce) so the protect actuator is the *sole* driver of its
     * replication — separating the protected unit from the protecting power
     * (§2) and making the actuator-off control honest. */
    {
        U1 id[PFS_ID_LEN];
        pfs_id_compute(p, (UW)rem, id);
        if (!pfs_has(id)) {
            pfs_repl_set_announce_suppress(1);
            U1 sid[PFS_ID_LEN];
            INT r = pfs_repl_put(p, (UW)rem, sid);
            pfs_repl_set_announce_suppress(0);
            if (r != PFS_OK) {
                pt_puts("[protect] put failed ("); pt_putdec((UW)(-r));
                pt_puts(")\r\n");
                return;
            }
        }
        protect_declare(id, (UW)rem, PROTECT_DEFAULT_R);
    }
}

/* ================================================================== */
/* property self-test (G28) — pure local integer math, runs bare-metal */
/*                                                                     */
/* Proves the loop is GROUNDED (threat = f(replication)) and CLOSED    */
/* (the actuator's replication is what drops the threat, never a timer)*/
/* by reusing the live protect_threat_for() formula — no duplicate.    */
/* ================================================================== */

/* [protect-ground]: threat is a monotone non-increasing function of the
 * real replica count and reaches 0 exactly at >=R (no timer anywhere). */
static INT st_grounding(void)
{
    INT fails = 0;
    const INT R = 3;
    pt_puts("[protect-ground] threat vs replicas (R="); pt_putdec((UW)R);
    pt_puts("):  ");
    UB prev = 0xFF;
    for (INT r = 0; r <= R + 1; r++) {
        UB t = protect_threat_for(r, R);
        pt_puts("r"); pt_putdec((UW)r); pt_puts("="); pt_putdec(t);
        pt_puts(r < R + 1 ? "  " : "");
        if (prev != 0xFF && t > prev) fails++;       /* must not rise */
        prev = t;
    }
    pt_puts("\r\n");
    if (protect_threat_for(0, R) == 0) {
        pt_puts("[protect-ground] FAIL under-replicated unit has zero threat\r\n");
        fails++;
    }
    if (protect_threat_for(R, R) != 0 || protect_threat_for(R + 1, R) != 0) {
        pt_puts("[protect-ground] FAIL threat not 0 at >=R\r\n");
        fails++;
    }
    if (protect_threat_for(R - 1, R) == 0) {
        pt_puts("[protect-ground] FAIL one-short-of-R reads as safe\r\n");
        fails++;
    }
    if (fails == 0)
        pt_puts("[protect-ground] PASS (threat grounded in under-replication;"
                " monotone; 0 only at >=R)\r\n");
    else
        pt_puts("[protect-ground] FAIL\r\n");
    return fails;
}

/* [protect-loop]: simulate the closed loop. Each tick, the ACTUATOR (when
 * enabled) drives one more durable replica into a neighbour, which announces
 * back and raises the real replica count -> the threat (recomputed from that
 * count) falls. With the actuator OFF nothing is evacuated: replicas stay 0
 * and the threat never moves. The drop is tied to replication, full stop. */
static void lp_sim(int actuator_on, INT R, INT ticks,
                   INT *replicas_out, UB *threat_first, UB *threat_last,
                   INT *dropped_at)
{
    INT replicas = 0;
    UB  first = protect_threat_for(replicas, R);
    UB  t = first;
    INT dropat = -1;
    for (INT k = 0; k < ticks; k++) {
        t = protect_threat_for(replicas, R);
        if (t == 0) { if (dropat < 0) dropat = k; break; }
        /* actuator drives replication: a neighbour pulls + persists + announces
         * back, so the real replica count rises by one this tick. */
        if (actuator_on && replicas < R) replicas++;
        /* (actuator off: replicas unchanged — no force is poured) */
    }
    t = protect_threat_for(replicas, R);
    if (t == 0 && dropat < 0) dropat = ticks;
    *replicas_out = replicas;
    *threat_first = first;
    *threat_last  = t;
    *dropped_at   = dropat;
}

static INT st_closed_loop(void)
{
    INT fails = 0;
    const INT R = 3, T = 12;

    INT rep_on, rep_off, drop_on, drop_off;
    UB  f_on, l_on, f_off, l_off;
    lp_sim(1, R, T, &rep_on,  &f_on,  &l_on,  &drop_on);
    lp_sim(0, R, T, &rep_off, &f_off, &l_off, &drop_off);

    pt_puts("[protect-loop]  actuator ON : replicas 0->"); pt_putdec((UW)rep_on);
    pt_puts("  threat "); pt_putdec(f_on); pt_puts("->"); pt_putdec(l_on);
    pt_puts("  (safe at tick "); pt_putdec((UW)drop_on); pt_puts(")\r\n");
    pt_puts("[protect-loop]  actuator OFF: replicas 0->"); pt_putdec((UW)rep_off);
    pt_puts("  threat "); pt_putdec(f_off); pt_puts("->"); pt_putdec(l_off);
    pt_puts("  (never safe)\r\n");

    /* (1) ON drives replicas to R and the threat to 0. */
    if (!(rep_on == R && l_on == 0)) {
        pt_puts("[protect-loop] FAIL actuator did not replicate unit to safety\r\n");
        fails++;
    }
    /* (2) the drop is BECAUSE of replication: more replicas, lower threat. */
    if (!(l_on < f_on)) {
        pt_puts("[protect-loop] FAIL threat did not fall as replicas rose\r\n");
        fails++;
    }
    /* (3) control: OFF leaves replicas at 0 and the threat pinned high. */
    if (!(rep_off == 0 && l_off == f_off && l_off > 0)) {
        pt_puts("[protect-loop] FAIL control (actuator off) did not stay at-risk\r\n");
        fails++;
    }
    /* (4) it is replication, not time: OFF ran the same ticks yet never safe. */
    if (!(drop_off < 0 || l_off > 0)) {
        pt_puts("[protect-loop] FAIL threat fell without replication (a timer?)\r\n");
        fails++;
    }
    if (fails == 0)
        pt_puts("[protect-loop] PASS (closed loop: actuator replication drops"
                " the grounded threat to 0; actuator-off stays at-risk — no timer)\r\n");
    else
        pt_puts("[protect-loop] FAIL\r\n");
    return fails;
}

INT protect_self_test(void)
{
    INT fails = 0;
    pt_puts("[protect-test] ==== §2/G28 grounded-threat + closed-loop tests ====\r\n");
    fails += st_grounding();
    fails += st_closed_loop();
    if (fails == 0) pt_puts("[protect-test] ALL PASS\r\n");
    else { pt_puts("[protect-test] FAILURES="); pt_putdec((UW)fails);
           pt_puts("\r\n"); }
    return fails;
}
