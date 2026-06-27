/*
 *  arch/linux/x86_64/supernode_autopromote.c
 *
 *  Measured-capability supernode auto-promotion (N-2d).
 *  Design: docs/architecture/supernode-autopromote.md.
 *
 *  A node AUTO-PROMOTES its own supernode capability bit when it MEASURES
 *  that it is actually a good supernode (relay-reachable / public-or-cone /
 *  stable / not metered / low stress / not isolated), and AUTO-DEMOTES when
 *  it isn't. The election (region.c::supernode_select), the capability
 *  gossip (swim.c::cap_self), and the N-2c forwarding plane are UNCHANGED:
 *  this TU only AUTO-SETS super_capable[self] via the EXISTING public setter
 *  region_set_super_capable(self, TRUE/FALSE).
 *
 *  HOSTED-ONLY (arch/linux/<arch>/, never linked into a bare-metal image), so
 *  the CROWN .text is byte-identical by construction (§D). The ONLY write into
 *  shared state is region_set_super_capable(drpc_my_node, ...). The teacher bit
 *  is OUT of scope (§C.6) and is never touched here.
 *
 *  The aarch64 twin (arch/linux/aarch64/supernode_autopromote.c) is kept
 *  lockstep-identical except the path in this banner.
 */

#include "kernel.h"
#include "region.h"      /* region_set_super_capable / region_is_super_capable; drpc_my_node, DNODE_MAX (via drpc.h) */
#include "interocept.h"  /* intero_scalar()  — CROWN read-only (§C.1) */
#include "degrade.h"     /* degrade_level()  — CROWN read-only (§C.1) */

/* libc bits we need without dragging in <unistd.h>/<time.h> (same discipline
 * net_relay.c uses: this TU is built under the T-Kernel runtime). */
extern char  *getenv(const char *);
extern long   time(long *);
extern long   write(int, const void *, unsigned long);

/* ---- reflexive-address signal (NEW hosted shim in net_relay.c, §C.1.a) ---- */
extern int  net_relay_contacted(void);          /* relay answered us (SLICE 4) */
extern void net_relay_reflexive_probe(void);     /* fire a REFL1 keepalive sweep */
extern int  net_relay_reflexive_count(void);     /* # vantage points reporting   */
extern int  net_relay_reflexive_classify(void);  /* 0 UNKNOWN / 1 CONE / 2 SYM   */
extern int  net_relay_reflexive_public(void);    /* reflexive IP == net_my_ip    */

/* NAT class codes — numerically identical to supernode.h NP_NAT_* so this
 * hosted re-implementation of supernode.c:768's rule is cert-pinned equivalent
 * to np_classify (§B.2.2). UNKNOWN fails closed (§C.4). */
#define SAP_NAT_UNKNOWN    0
#define SAP_NAT_CONE       1
#define SAP_NAT_SYMMETRIC  2

/* ---- thresholds + dwell windows (§C.2/§C.4) ------------------------------- */
#define SAP_STRESS_MAX   200    /* interoception pressure 0..255; >= => don't volunteer */
#define SAP_DEGRADE_MAX    2    /* DEGRADE_SOLO; degrade >= SOLO => don't volunteer      */
#define SAP_PROMOTE_K_S   60    /* promote only after 60 s CONTINUOUS fitness_good       */
#define SAP_DEMOTE_K_S    30    /* demote only after 30 s CONTINUOUS !fitness_good        */

/* ---- verdicts ------------------------------------------------------------ */
#define SAP_HOLD     0
#define SAP_PROMOTE  1
#define SAP_DEMOTE   2

/* The signals gathered once per evaluator tick (§C.1). */
typedef struct {
    int relay_contacted;   /* 0/1 — a working bidirectional path exists       */
    int refl_count;        /* # distinct vantage points reporting our mapping */
    int refl_classify;     /* SAP_NAT_UNKNOWN / CONE / SYMMETRIC              */
    int refl_is_public;    /* 0/1 — reflexive IP == our local bind IP         */
    int metered;           /* 0/1 — metered/cellular link                     */
    int stress;            /* 0..255 interoception pressure                   */
    int degrade;           /* DEGRADE_FULL..SOLO                              */
} SAP_SIGNALS;

/* Anti-flap dwell state (§C.4). good_since/bad_since are the monotonic-second
 * timestamps the current good/bad run started, or -1 when not in that run. */
typedef struct {
    long good_since;
    long bad_since;
    int  adopted;          /* the capability bit this evaluator currently holds */
} SAP_STATE;

static SAP_STATE g_state = { -1, -1, 0 };

/* ====================================================================== *
 *  PURE fitness core — integer-only, deterministic, arch-uniform.
 *  This is what the cert (§E.1) drives + the falsifier sabotages.
 * ====================================================================== */

/* reachable_good (§C.2): relay answered AND (public OR cone) AND NOT symmetric.
 * UNKNOWN reachability (no echo yet / <2 vantage points) makes both the
 * public and the cone clauses false -> NOT good (fail-closed, §C.4). */
static int sap_reachable_good(const SAP_SIGNALS *s)
{
    if (!s->relay_contacted) return 0;
    if (!(s->refl_is_public || s->refl_classify == SAP_NAT_CONE)) return 0;
#ifndef SAP_NO_SYMBLOCK
    /* HARD BLOCK (§C.3): a symmetric-NAT node is unpunchable, never a supernode.
     * The TOOTHFUL FALSIFIER (-DSAP_NO_SYMBLOCK) removes EXACTLY this clause so
     * a symmetric node wrongly promotes and the cert's case B flips RED. */
    if (s->refl_classify == SAP_NAT_SYMMETRIC) return 0;
#endif
    return 1;
}

/* fitness_good (§C.2): reachable AND not metered AND low stress AND not SOLO. */
static int sap_fitness_good(const SAP_SIGNALS *s)
{
    return sap_reachable_good(s)
        && s->metered == 0
        && s->stress  <  SAP_STRESS_MAX
        && s->degrade <  SAP_DEGRADE_MAX;
}

/* The dwell engine. Returns SAP_PROMOTE / SAP_DEMOTE / SAP_HOLD and mutates
 * *st (the two dwell timers + the adopted bit). Symmetric is a hard block
 * INSIDE fitness_good, so the good-dwell can never accumulate while symmetric
 * (§C.3). Demote is faster than promote (30 vs 60) — the hysteresis band. */
int sap_step(SAP_STATE *st, const SAP_SIGNALS *s, long now_s)
{
    int good = sap_fitness_good(s);

    if (good) {
        st->bad_since = -1;
        if (st->good_since < 0) st->good_since = now_s;
    } else {
        st->good_since = -1;
        if (st->bad_since < 0) st->bad_since = now_s;
    }

    if (!st->adopted && good && st->good_since >= 0 &&
        (now_s - st->good_since) >= SAP_PROMOTE_K_S) {
        st->adopted = 1;
        return SAP_PROMOTE;
    }
    if (st->adopted && !good && st->bad_since >= 0 &&
        (now_s - st->bad_since) >= SAP_DEMOTE_K_S) {
        st->adopted = 0;
        return SAP_DEMOTE;
    }
    return SAP_HOLD;
}

/* ====================================================================== *
 *  LIVE wrapper — gathers real signals, applies the verdict via the
 *  EXISTING setter. Folded into net_heartbeat_task at the 5 s cadence.
 * ====================================================================== */

/* ---- cert mock seam (compiled OUT in production -> .text unchanged, §E.1) - */
#ifdef SAP_CERT
static int        sap_use_mock   = 0;   /* 1 = divert clock+signals, no sockets */
static long       sap_mock_now   = 0;   /* mock monotonic seconds               */
static int        sap_mock_forced= 0;   /* mock PKERNEL_SUPERNODE=1              */
static SAP_SIGNALS sap_mock_sig;        /* cert-controlled signal vector        */
static int        sap_step_calls = 0;   /* observability: # sap_step invocations */
#define SAP_NOW()  (sap_use_mock ? sap_mock_now : (long)time((long *)0))
#else
#define SAP_NOW()  ((long)time((long *)0))
#endif

/* FORCE > measured (§C.5): PKERNEL_SUPERNODE=1 stays ALWAYS-capable. Read the
 * env ONCE. A forced node skips fitness entirely and is never demoted. */
static int sap_forced_cached = -1;
static int sap_is_forced(void)
{
#ifdef SAP_CERT
    if (sap_use_mock) return sap_mock_forced;
#endif
    if (sap_forced_cached < 0) {
        const char *e = getenv("PKERNEL_SUPERNODE");
        sap_forced_cached = (e && e[0] == '1') ? 1 : 0;
    }
    return sap_forced_cached;
}

/* metered link -> never promote (§C.1). Android sets it on cellular. */
static int sap_metered_cached = -1;
static int sap_metered(void)
{
    if (sap_metered_cached < 0) {
        const char *e = getenv("PKERNEL_METERED");
        sap_metered_cached = (e && e[0] == '1') ? 1 : 0;
    }
    return sap_metered_cached;
}

static void sap_gather(SAP_SIGNALS *s)
{
#ifdef SAP_CERT
    if (sap_use_mock) { *s = sap_mock_sig; return; }
#endif
    s->relay_contacted = net_relay_contacted() ? 1 : 0;
    s->refl_count      = net_relay_reflexive_count();
    s->refl_classify   = net_relay_reflexive_classify();
    s->refl_is_public  = net_relay_reflexive_public() ? 1 : 0;
    s->metered         = sap_metered();
    s->stress          = (int)intero_scalar();
    s->degrade         = (int)degrade_level();
}

/* Low-stack constant-string logger (write(2), never glibc stdio on a 4 KB
 * task stack — the hosted-relay stack lesson). The live harness greps the
 * exact substrings "autopromote: PROMOTE" / "autopromote: DEMOTE". */
static void sap_log(const char *s)
{
    unsigned long n = 0; while (s[n]) n++;
    (void)write(2, s, n);
}

/* The evaluator tick (called from net_heartbeat_task every 5 s). */
void sap_evaluate(void)
{
    /* Fire a fresh reflexive measurement so next tick has up-to-date echoes
     * (real path only; the cert feeds mock signals directly). */
#ifdef SAP_CERT
    if (!sap_use_mock)
#endif
        net_relay_reflexive_probe();

    /* FORCE > measured: a forced node is never demoted and skips fitness. */
    if (sap_is_forced()) return;

    UB me = drpc_my_node;
    if (me == 0xFF) return;     /* no cluster id yet — nothing to set */

    SAP_SIGNALS s;
    sap_gather(&s);

#ifdef SAP_CERT
    sap_step_calls++;
#endif
    int v = sap_step(&g_state, &s, SAP_NOW());
    if (v == SAP_PROMOTE) {
        region_set_super_capable(me, TRUE);
        sap_log("autopromote: PROMOTE (measured fitness: relay-reachable, "
                "public/cone, low stress)\n");
    } else if (v == SAP_DEMOTE) {
        region_set_super_capable(me, FALSE);
        sap_log("autopromote: DEMOTE (measured fitness lost: reachability/"
                "stress/isolation)\n");
    }
}

/* ====================================================================== *
 *  IN-PROC cert — `autopromote test` (mock signals, NO sockets, §E.1)
 *  Drives the SHIPPED wrapper sap_evaluate()/sap_step() against a mock clock
 *  and mock SAP_SIGNALS, then asserts the REAL setter effect by reading
 *  region_is_super_capable(self). drpc_my_node is impersonated, saved/restored.
 * ====================================================================== */
#ifdef SAP_CERT

static int sap_fail;

static void sap_check(void (*pr)(const char *), int ok, const char *d)
{
    pr(ok ? "[autopromote]   PASS " : "[autopromote]   FAIL ");
    pr(d); pr("\r\n");
    if (!ok) sap_fail = 1;
}

static void sap_state_reset(void)
{
    g_state.good_since = -1;
    g_state.bad_since  = -1;
    g_state.adopted    = 0;
}

/* Build a "perfect" promotion-favorable signal vector. */
static void sap_sig_good(SAP_SIGNALS *s)
{
    s->relay_contacted = 1;
    s->refl_count      = 2;
    s->refl_classify   = SAP_NAT_CONE;
    s->refl_is_public  = 1;
    s->metered         = 0;
    s->stress          = 10;
    s->degrade         = 0;       /* DEGRADE_FULL */
}

void sap_self_test(void (*pr)(const char *))
{
    sap_fail = 0;
    pr("[autopromote] measured-capability supernode auto-promotion cert (N-2d)\r\n");

    UB saved_node = drpc_my_node;
    const UB ME   = 3;
    drpc_my_node  = ME;

    sap_use_mock    = 1;
    sap_mock_forced = 0;
    const long T0   = 1000;

    /* ---- CASE A: CONE/public + stable -> promotes within the 60 s window --- */
    sap_state_reset();
    region_set_super_capable(ME, FALSE);
    sap_sig_good(&sap_mock_sig);
    int a_before = 0;
    for (long t = 0; t <= 55; t += 5) {       /* 0..55 s < SAP_PROMOTE_K_S */
        sap_mock_now = T0 + t;
        sap_evaluate();
        if (region_is_super_capable(ME)) a_before = 1;
    }
    sap_check(pr, !a_before && region_is_super_capable(ME) == FALSE,
              "A: HOLD before 60 s (super_capable[self] still 0)");
    sap_mock_now = T0 + 60;                    /* >= SAP_PROMOTE_K_S */
    sap_evaluate();
    sap_check(pr, region_is_super_capable(ME) == TRUE,
              "A: PROMOTE at >=60 s of continuous fitness (super_capable[self] set)");

    /* ---- CASE B: SYMMETRIC-NAT node NEVER promotes ------------------------- *
     * "everything else perfect" INCLUDING refl_is_public=1, so the ONLY thing
     * keeping it out is the symmetric HARD BLOCK (§C.3). The falsifier
     * (-DSAP_NO_SYMBLOCK) drops that clause -> this node promotes -> RESULT: FAIL. */
    sap_state_reset();
    region_set_super_capable(ME, FALSE);
    sap_sig_good(&sap_mock_sig);
    sap_mock_sig.refl_classify = SAP_NAT_SYMMETRIC;
    sap_mock_sig.refl_is_public = 1;           /* isolate the symblock clause */
    int b_ever = 0;
    for (long t = 0; t <= 10 * SAP_PROMOTE_K_S; t += 5) {
        sap_mock_now = T0 + t;
        sap_evaluate();
        if (region_is_super_capable(ME)) b_ever = 1;
    }
    sap_check(pr, !b_ever && region_is_super_capable(ME) == FALSE,
              "B: SYMMETRIC never promotes (hard block, independent of dwell)");

    /* ---- CASE C1: a good blip < 60 s then bad -> never promotes ------------ */
    sap_state_reset();
    region_set_super_capable(ME, FALSE);
    int c1_promoted = 0;
    for (long t = 0; t <= 30; t += 5) {        /* 30 s good (< 60) */
        sap_sig_good(&sap_mock_sig);
        sap_mock_now = T0 + t;
        sap_evaluate();
        if (region_is_super_capable(ME)) c1_promoted = 1;
    }
    for (long t = 35; t <= 120; t += 5) {      /* then bad */
        sap_sig_good(&sap_mock_sig);
        sap_mock_sig.relay_contacted = 0;      /* reachability lost */
        sap_mock_now = T0 + t;
        sap_evaluate();
        if (region_is_super_capable(ME)) c1_promoted = 1;
    }
    sap_check(pr, !c1_promoted && region_is_super_capable(ME) == FALSE,
              "C1: <60 s good blip does NOT promote");

    /* ---- CASE C2: while promoted, a bad blip < 30 s then good -> no demote -- */
    sap_state_reset();
    region_set_super_capable(ME, FALSE);
    for (long t = 0; t <= 60; t += 5) {        /* earn the bit (>=60 s good) */
        sap_sig_good(&sap_mock_sig);
        sap_mock_now = T0 + t;
        sap_evaluate();
    }
    int c2_was_capable = region_is_super_capable(ME);
    int c2_demoted = 0;
    for (long t = 65; t <= 85; t += 5) {       /* 20 s bad blip (< 30) */
        sap_sig_good(&sap_mock_sig);
        sap_mock_sig.relay_contacted = 0;
        sap_mock_now = T0 + t;
        sap_evaluate();
        if (!region_is_super_capable(ME)) c2_demoted = 1;
    }
    for (long t = 90; t <= 160; t += 5) {      /* good again */
        sap_sig_good(&sap_mock_sig);
        sap_mock_now = T0 + t;
        sap_evaluate();
        if (!region_is_super_capable(ME)) c2_demoted = 1;
    }
    sap_check(pr, c2_was_capable && !c2_demoted && region_is_super_capable(ME) == TRUE,
              "C2: <30 s bad blip does NOT demote (still capable)");

    /* ---- CASE D: PKERNEL_SUPERNODE=1 forces capable despite all-bad -------- *
     * region_super_init would have set the bit; the evaluator must LEAVE it,
     * skip fitness (sap_step NEVER called), and never issue set(self,FALSE). */
    sap_state_reset();
    region_set_super_capable(ME, TRUE);        /* simulate the env one-shot */
    sap_mock_forced = 1;
    sap_step_calls  = 0;
    sap_sig_good(&sap_mock_sig);
    sap_mock_sig.relay_contacted = 0;          /* all-bad */
    sap_mock_sig.refl_classify   = SAP_NAT_SYMMETRIC;
    sap_mock_sig.refl_is_public  = 0;
    sap_mock_sig.metered         = 1;
    sap_mock_sig.stress          = 255;
    sap_mock_sig.degrade         = 2;
    int d_dropped = 0;
    for (long t = 0; t <= 200; t += 5) {
        sap_mock_now = T0 + t;
        sap_evaluate();
        if (!region_is_super_capable(ME)) d_dropped = 1;
    }
    sap_check(pr, !d_dropped && region_is_super_capable(ME) == TRUE && sap_step_calls == 0,
              "D: PKERNEL_SUPERNODE=1 forces capable, fitness skipped (force>measured)");
    sap_mock_forced = 0;

    /* restore production state */
    region_set_super_capable(ME, FALSE);
    sap_state_reset();
    drpc_my_node  = saved_node;
    sap_use_mock  = 0;

    if (sap_fail) pr("[autopromote] RESULT: FAIL\r\n");
    else          pr("[autopromote] RESULT: 6/6 PASS\r\n");
}
#endif /* SAP_CERT */
