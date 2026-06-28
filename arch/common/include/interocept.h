/*
 *  interocept.h — 内受容: the unified stress S_n bus (interoception.md §2)
 *
 *  The SOURCES of pain already exist (reflex threat, SWIM RTT EWMA, in-context
 *  surprise, ring3 fault reaps, degrade level). This module is the cheap BUS
 *  that aggregates them into ONE scalar 0..255 "mood" any organ can read for
 *  free. No new sensor, no new float smoother — the EWMA is swim.c's exact
 *  integer recurrence (old*3 + sample + 2)/4 (alpha = 1/4).
 *
 *  Discipline (interoception.md §1.1): S_n is the SLOW deliberative summary,
 *  NOT a replacement for the fast reflex loop. reflex_threat_experience() is an
 *  INPUT to S_n, never its output. Two time-constants on purpose (§II-3 / §8).
 *
 *  LP64 trap (memory feedback_lp64_typedef_trap): components are UB (uint8),
 *  the EWMA state is held in a plain `unsigned` (UW = unsigned int on all four
 *  arches per typedef.h) — no `long`, so the same 0..255 math on every ABI.
 */
#ifndef _INTEROCEPT_H_
#define _INTEROCEPT_H_

#include "typedef.h"

/* The pain axes. Order is the bus index; intero_note(axis, raw) writes one.
 * Keep INTERO_AXIS_MAX last. */
#define INTERO_AX_THREAT    0   /* reflex_threat_experience max-class pressure */
#define INTERO_AX_LATENCY   1   /* worst adjacent SWIM RTT EWMA deviation      */
#define INTERO_AX_SURPRISE  2   /* in-context CE / prediction error from m_ask */
#define INTERO_AX_FAULT     3   /* ring3 fault-reap increment over a window    */
#define INTERO_AX_DEGRADE   4   /* degrade_level() (FULL=0 .. SOLO=max)        */
#define INTERO_AXIS_MAX     5

/* Component vector (interoception.md §2.1): each 0..255 dimensionless pressure.
 * Read by galaxy/diagnostics; the scalar is what organs steer on. */
typedef struct {
    UB threat;
    UB latency;
    UB surprise;
    UB fault;
    UB degrade;
} INTERO_COMPONENTS;

/* ── read side (cheap, O(1)) ─────────────────────────────────────────────── */

/* 0..255 aggregated "mood" — one EWMA over the weighted-max of the live axes.
 * Recomputed lazily from the latest sampled axes on each read (no polling). */
UB                intero_scalar(void);

/* The per-axis breakdown for galaxy / the `intero` cert. */
INTERO_COMPONENTS intero_components(void);

/* The dominant (largest) axis index at the last scalar update — for galaxy's
 * me.s_axis hint. Returns INTERO_AXIS_MAX when nothing has been sampled. */
UB                intero_dominant_axis(void);

/* ── write side (sources push raw readings; normalization is internal) ────── */

/* A source notes a fresh raw reading on one axis (arrival-driven, G13). raw is
 * the source's natural integer (ms, count, percent, level); intero.c maps it to
 * the 0..255 band. Cheap; safe to call from any task context. */
void              intero_note(UB axis, UW raw);

/* Pull-mode refresh: re-sample every axis from its live source and fold the
 * weighted-max into the scalar EWMA. The production consumer (DMN) calls this
 * once per modulation read so S_n tracks sources that don't emit events. */
void              intero_sample(void);

/* ── lifecycle / cert ────────────────────────────────────────────────────── */

void              intero_init(void);

/* Cert-only: force the scalar EWMA to a known value so an acceptance test (the
 * DMN [intero-tick] modulation cert) can drive S_n deterministically without a
 * live source. The NEXT intero_sample()/intero_scalar() re-folds live sources,
 * so this is a one-shot injection the production read overwrites. Use ONLY from
 * certs. Pass force=1 to pin, force=0 to release. */
void              intero_test_force(UB on, UB value);

/* Cert-only (survival-loop L0 (B) / GAP-⑨): like intero_test_force but ALSO pins
 * the DOMINANT AXIS, so the [state-axis] cert can inject surprise/fault/degrade
 * as dominant (legacy intero_test_force always pins INTERO_AX_THREAT). Hosted /
 * cert only — never on the live path. Release via intero_test_force(0,0) or
 * intero_init, both of which reset the forced axis back to the THREAT-pin
 * shortcut so the legacy behaviour is byte-for-byte preserved. */
#ifdef _TK_HOSTED_LIBC_
void              intero_test_force_axis(UB axis, UB scalar);
#endif

/* Production self-test (interoception.md §3.5 [intero-sources]/[intero-wired]):
 * drives EACH axis in isolation and asserts only that axis moves; confirms the
 * scalar EWMA damps; prints "[intero-self] PASS/FAIL". Calls the SAME production
 * symbols (intero_note/intero_scalar) the live path uses. Returns 0 on PASS. */
INT               intero_self_test(void);

#endif /* _INTEROCEPT_H_ */
