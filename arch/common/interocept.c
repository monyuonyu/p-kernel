/*
 *  interocept.c — 内受容: the unified stress S_n bus (interoception.md §2/§3.1)
 *
 *  「源泉は既にある。新しいのは BUS だ」(interoception.md §1). This module does
 *  NOT measure anything new. It is the cheap aggregator that folds the existing
 *  pain sources into ONE 0..255 scalar:
 *
 *    threat   ← reflex_threat_experience() max-class      (reflex.c)
 *    latency  ← worst adjacent SWIM RTT EWMA deviation    (swim.c swim_rtt_ms)
 *    surprise ← in-context prediction error from m_ask    (r3 mind_last_answer)
 *    fault    ← ring3 fault-reap increment over a window  (idt.c/isr.S, x86)
 *    degrade  ← degrade_level()                           (degrade.c)
 *
 *  Aggregation (interoception.md §2.2): scalar ← ewma_step(scalar,
 *  weighted_max(components)) using swim.c's EXACT integer recurrence
 *  (old*3 + sample + 2)/4  (alpha = 1/4). weighted_max keeps the single worst
 *  axis dominant so one sharp pain is never buried under many faint ones.
 *
 *  Two time-constants on purpose (interoception.md §1.1 / survival-network §8):
 *  reflex stays the FAST loop; S_n is the SLOW EWMA summary. We do NOT decide
 *  threat here — reflex_threat_experience() is an INPUT, not an output.
 *
 *  LP64-safe: components are UB; the EWMA state is `unsigned` (UW = unsigned int
 *  on all four arches, typedef.h). No `long`, no float. Allocation-free.
 */

#include "interocept.h"
#include "reflex.h"
#include "swim.h"
#include "degrade.h"
#include "drpc.h"     /* DNODE_MAX, drpc_my_node */
#include "dtr.h"      /* mind_last_answer() */
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

static void io_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void io_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { io_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    io_puts(&buf[i]);
}

/* ── fault axis source ─────────────────────────────────────────────────────
 * ring3_faults_reaped is x86-bare-metal-only (provided by arch/x86 idt.c).
 * On the hosted / aarch64 / Android builds it does not exist, so the bus reads
 * the fault axis through this WEAK accessor that defaults to 0 and can be
 * overridden by an arch that has the live counter. Keeps interocept.c in the
 * common source list on EVERY build (check_parity GREEN) without a per-arch
 * IMPORT that would fail to link where the symbol is absent. */
__attribute__((weak)) UW intero_fault_count_hook(void) { return 0; }

/* ── conscience axis source (良心, design §1.3) ────────────────────────────
 * A monotone refusal count provided by conscience.c (the strong override).
 * WEAK default 0 keeps interocept.c linking on any build that omits the
 * conscience TU, exactly like the fault hook. A refusal-of-harm raises S_n so
 * the DMN sleeps shallow and uneasy after being asked for harm. */
__attribute__((weak)) UW intero_conscience_count_hook(void) { return 0; }

/* ── module state (single task / behind the shells' serialization) ────────── */

static INTERO_COMPONENTS comp;       /* latest per-axis 0..255 pressures        */
static UW   scalar_ewma;             /* the S_n EWMA state (0..255)             */
static UB   dom_axis = INTERO_AXIS_MAX;
static UW   fault_base;              /* ring3_faults_reaped at init (window base) */
static UW   conscience_base;         /* conscience refusals at init (window base) */
static UB   inited;

/* cert-only deterministic injection (see intero_test_force) */
static UB   force_on;
static UB   force_val;
#ifdef _TK_HOSTED_LIBC_
/* survival-loop L0 (B): which axis the forced injection pins as dominant. The
 * legacy intero_test_force resets this to INTERO_AX_THREAT, so the existing
 * THREAT-pin shortcut is preserved; only the new intero_test_force_axis moves
 * it. Hosted-only — bare-metal keeps the constant THREAT pin below, so the
 * bare-metal .text (the crown) is byte-identical. */
static UB   force_axis = INTERO_AX_THREAT;
#endif

/* swim.c's exact integer EWMA: 3/4 old + 1/4 sample, +2 rounds. */
static UW io_ewma_step(UW old, UW sample)
{
    return (old * 3u + sample + 2u) / 4u;
}

static UB io_clamp255(UW v) { return (UB)(v > 255u ? 255u : v); }

/* ── per-axis normalizers (interoception.md §2.1; §2.4 honest bands) ────────
 * The bands below are FIRST-CUT linear maps from each source's natural integer
 * to the 0..255 pressure scale. They are intentionally simple and documented
 * here so §2.4's "discover, don't assume" follow-up can replace any single one
 * from a measured curve without touching the bus. */

/* threat: reflex_threat_experience(cls) is a monotone firing count per class.
 * We take the max class and saturate — any sustained guarding reads as high. */
static UB io_norm_threat(void)
{
    UW mx = 0;
    for (INT c = 0; c < REFLEX_NUM_CLASSES; c++) {
        UW e = reflex_threat_experience((UB)c);
        if (e > mx) mx = e;
    }
    /* a handful of guard firings already means "this body keeps getting hit". */
    return io_clamp255(mx * 32u);
}

/* latency: worst adjacent RTT EWMA deviation above a healthy baseline.
 * swim_rtt_ms() returns 0xFFFFFFFF for unmeasured peers (skipped). The deviation
 * is (rtt - BASELINE_MS) scaled; a 0..~64ms healthy band maps under threshold. */
#define IO_RTT_BASELINE_MS   8u     /* a healthy same-host / LAN RTT floor      */
#define IO_RTT_SPAN_MS       120u   /* deviation that saturates the axis        */
static UB io_norm_latency(void)
{
    UW worst = 0; UB any = 0;
    for (INT n = 0; n < DNODE_MAX; n++) {
        if ((UB)n == drpc_my_node) continue;
        UW r = swim_rtt_ms((UB)n);
        if (r == 0xFFFFFFFFUL) continue;       /* unmeasured */
        any = 1;
        if (r <= IO_RTT_BASELINE_MS) continue;
        UW dev = r - IO_RTT_BASELINE_MS;
        if (dev > worst) worst = dev;
    }
    if (!any) return 0;
    return io_clamp255(worst * 255u / IO_RTT_SPAN_MS);
}

/* surprise: in-context prediction error from the last `mind ask`. mind_last_answer
 * gives the modal class share *10 (e.g. 950 = 95.0%). A CONFIDENT answer (high
 * share) is LOW surprise; a flat/uncertain answer (low share) is HIGH surprise.
 * surprise = (1000 - share)/1000 scaled to 0..255. */
static UB io_norm_surprise(void)
{
    UB k = 0, v = 0; UW share = 0;
    mind_last_answer(&k, &v, &share);
    if (share == 0) return 0;                  /* never asked yet — no signal */
    UW miss = (share >= 1000u) ? 0u : (1000u - share);
    return io_clamp255(miss * 255u / 1000u);
}

/* fault: monotone ring3 fault-reap increment since init (windowed at the bus).
 * Each reaped self-crash is a sharp pain; a few saturate. */
static UB io_norm_fault(void)
{
    UW now = intero_fault_count_hook();
    UW inc = (now >= fault_base) ? (now - fault_base) : 0u;
    return io_clamp255(inc * 64u);
}

/* degrade: FULL=0, REDUCED, SOLO=max. degrade_level() is a small ordinal. */
static UB io_norm_degrade(void)
{
    return io_clamp255((UW)degrade_level() * 96u);
}

/* conscience: monotone refusal increment since init (windowed at the bus). Each
 * refusal-of-harm is a sharp deliberative pain; a few saturate — the mind reads
 * as uneasy after being asked for harm (interoception.md §2 / conscience §1.3). */
static UB io_norm_conscience(void)
{
    UW now = intero_conscience_count_hook();
    UW inc = (now >= conscience_base) ? (now - conscience_base) : 0u;
    return io_clamp255(inc * 64u);
}

/* weighted_max (interoception.md §2.2): the worst axis dominates; the rest add a
 * faint background so a body hurting on many axes reads slightly worse than one.
 * Integer: max + (sum_of_rest >> 3), saturated. */
static UW io_weighted_max(const INTERO_COMPONENTS *c, UB *which)
{
    UW vals[INTERO_AXIS_MAX];
    vals[INTERO_AX_THREAT]     = c->threat;
    vals[INTERO_AX_LATENCY]    = c->latency;
    vals[INTERO_AX_SURPRISE]   = c->surprise;
    vals[INTERO_AX_FAULT]      = c->fault;
    vals[INTERO_AX_DEGRADE]    = c->degrade;
    vals[INTERO_AX_CONSCIENCE] = c->conscience;

    UW mx = 0, sum = 0; UB mi = INTERO_AXIS_MAX;
    for (INT i = 0; i < INTERO_AXIS_MAX; i++) {
        sum += vals[i];
        if (vals[i] > mx) { mx = vals[i]; mi = (UB)i; }
    }
    UW rest = (sum > mx) ? (sum - mx) : 0u;
    if (which) *which = mi;
    return io_clamp255(mx + (rest >> 3));
}

/* ── public: refresh every axis from its live source, fold into the EWMA ──── */

void intero_sample(void)
{
    if (!inited) intero_init();

    if (force_on) {                  /* cert-only deterministic S_n */
        scalar_ewma = force_val;
#ifdef _TK_HOSTED_LIBC_
        dom_axis    = force_axis;     /* survival-loop L0 (B): axis-selectable */
#else
        dom_axis    = INTERO_AX_THREAT;
#endif
        return;
    }

    comp.threat     = io_norm_threat();
    comp.latency    = io_norm_latency();
    comp.surprise   = io_norm_surprise();
    comp.fault      = io_norm_fault();
    comp.degrade    = io_norm_degrade();
    comp.conscience = io_norm_conscience();

    UB which = INTERO_AXIS_MAX;
    UW agg = io_weighted_max(&comp, &which);
    scalar_ewma = io_ewma_step(scalar_ewma, agg);
    dom_axis    = which;
}

/* ── public: arrival-driven write (a source pushed one raw reading) ────────
 * We fold the single axis's freshly-normalized value into the scalar so a hot
 * event moves S_n promptly (G13: act when the source moves, don't poll). The
 * per-axis component is updated from its OWN normalizer so a stale axis cannot
 * be spoofed by a wrong raw on a different one. */
void intero_note(UB axis, UW raw)
{
    if (!inited) intero_init();
    (void)raw;   /* the normalizers read the authoritative live source directly */

    switch (axis) {
    case INTERO_AX_THREAT:     comp.threat     = io_norm_threat();     break;
    case INTERO_AX_LATENCY:    comp.latency    = io_norm_latency();    break;
    case INTERO_AX_SURPRISE:   comp.surprise   = io_norm_surprise();   break;
    case INTERO_AX_FAULT:      comp.fault      = io_norm_fault();      break;
    case INTERO_AX_DEGRADE:    comp.degrade    = io_norm_degrade();    break;
    case INTERO_AX_CONSCIENCE: comp.conscience = io_norm_conscience(); break;
    default: return;
    }
    UB which = INTERO_AXIS_MAX;
    UW agg = io_weighted_max(&comp, &which);
    scalar_ewma = io_ewma_step(scalar_ewma, agg);
    dom_axis    = which;
}

/* ── public: cheap reads ───────────────────────────────────────────────────
 * intero_scalar re-samples so a caller that does not also drive intero_note
 * (e.g. the DMN modulation read) still tracks event-less sources (RTT, degrade)
 * on each call. The re-sample is O(DNODE_MAX) integer work, no allocation. */
UB intero_scalar(void)
{
    intero_sample();
    return io_clamp255(scalar_ewma);
}

INTERO_COMPONENTS intero_components(void) { intero_sample(); return comp; }

UB intero_dominant_axis(void) { return dom_axis; }

/* ── lifecycle ─────────────────────────────────────────────────────────────*/

void intero_init(void)
{
    comp.threat = comp.latency = comp.surprise = comp.fault = comp.degrade = 0;
    comp.conscience = 0;
    scalar_ewma = 0;
    dom_axis    = INTERO_AXIS_MAX;
    fault_base  = intero_fault_count_hook();
    conscience_base = intero_conscience_count_hook();
    inited      = 1;
#ifdef _TK_HOSTED_LIBC_
    force_axis  = INTERO_AX_THREAT;  /* survival-loop L0 (B): back to THREAT pin */
#endif
}

void intero_test_force(UB on, UB value)
{
    force_on  = on ? 1 : 0;
    force_val = value;
#ifdef _TK_HOSTED_LIBC_
    force_axis = INTERO_AX_THREAT;   /* legacy entry = the THREAT-pin shortcut */
#endif
}

#ifdef _TK_HOSTED_LIBC_
/* survival-loop L0 (B) / GAP-⑨: pin the forced S_n AND its dominant axis, so the
 * [state-axis] cert can drive surprise/fault/degrade/latency as dominant (the
 * legacy intero_test_force only ever pins THREAT). Release with
 * intero_test_force(0,0) / intero_init, which restore the THREAT-pin shortcut.
 * Hosted/cert only — never linked into a bare-metal kernel. */
void intero_test_force_axis(UB axis, UB scalar)
{
    force_on   = 1;
    force_val  = scalar;
    force_axis = (axis < INTERO_AXIS_MAX) ? axis : INTERO_AX_THREAT;
}
#endif

/* ── production self-test (interoception.md §3.5) ──────────────────────────
 * Drives the SAME production write path (intero_note) per axis with the live
 * sources stubbed by a directly-set component, asserting:
 *   [intero-sources] only the driven axis rises (others stay flat);
 *   [intero-ewma]    a one-shot spike does NOT immediately saturate the scalar
 *                    (the EWMA damps it — the slow-loop discipline of §1.1);
 *   [intero-wired]   intero_scalar() is callable and returns the folded mood.
 * Because the live normalizers read real sources, this test injects directly
 * into the component vector then folds via the REAL io_weighted_max + EWMA so
 * the aggregation under test is the production aggregation. Returns 0 = PASS. */
INT intero_self_test(void)
{
    INT fail = 0;
    io_puts("[intero] self-test: source isolation + EWMA damping\r\n");

    /* isolation: set one axis high, the rest zero; the dominant axis must be it
     * and the aggregate must rise. Repeat per axis. */
    for (INT ax = 0; ax < INTERO_AXIS_MAX; ax++) {
        comp.threat = comp.latency = comp.surprise = comp.fault = comp.degrade = 0;
        comp.conscience = 0;
        UW *slot;
        /* address the chosen axis */
        switch (ax) {
        case INTERO_AX_THREAT:     comp.threat = 200; break;
        case INTERO_AX_LATENCY:    comp.latency = 200; break;
        case INTERO_AX_SURPRISE:   comp.surprise = 200; break;
        case INTERO_AX_FAULT:      comp.fault = 200; break;
        case INTERO_AX_CONSCIENCE: comp.conscience = 200; break;
        default:                   comp.degrade = 200; break;
        }
        (void)slot;
        UB which = INTERO_AXIS_MAX;
        UW agg = io_weighted_max(&comp, &which);
        if (which != (UB)ax) {
            io_puts("[intero]   axis "); io_putdec((UW)ax);
            io_puts(" dominance FAIL (got "); io_putdec((UW)which); io_puts(")\r\n");
            fail = 1;
        }
        if (agg < 200u) {
            io_puts("[intero]   axis "); io_putdec((UW)ax);
            io_puts(" did not raise aggregate FAIL\r\n");
            fail = 1;
        }
    }

    /* EWMA damping: from a calm 0 state, a single max spike must NOT saturate
     * (slow loop). After one step of sample=255, the EWMA is ~(0*3+255+2)/4=64. */
    scalar_ewma = 0;
    UW one = io_ewma_step(scalar_ewma, 255u);
    if (one >= 255u) { io_puts("[intero]   EWMA failed to damp a spike FAIL\r\n"); fail = 1; }
    /* repeated stress eventually rises toward saturation (it IS responsive). */
    UW s = 0; for (INT i = 0; i < 30; i++) s = io_ewma_step(s, 255u);
    if (s < 200u) { io_puts("[intero]   EWMA failed to rise under sustained stress FAIL\r\n"); fail = 1; }

    /* wired: the public read works end-to-end. */
    (void)intero_scalar();

    /* leave the bus calm after the test (restore live state). */
    intero_init();

    io_puts(fail ? "[intero-self] FAIL\r\n" : "[intero-self] PASS\r\n");
    return fail;
}
