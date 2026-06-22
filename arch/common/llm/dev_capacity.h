/*
 *  dev_capacity.h — measure the device, auto-fit the mind (DEVFIT-1).
 *
 *  mk_pino's explicit direction: 「デバイスのスペックを測って自動で合わせたい」
 *  — measure the device's RAM/cores and auto-fit the mind's size. The CORES half
 *  (SMP-AUTODETECT, GICD_TYPER) already ships; this is the named OTHER half:
 *  sizing the Cradle student to the device.
 *
 *  Design: docs/architecture/device-capacity-mind-sizing-plan.md (HARDENED).
 *
 *  What this TU does (and what it does NOT):
 *    - It picks ONE of the already-built discrete student tiers {S, M, L}
 *      (student.h:70-92, the ST_TIERS table) from a device measurement. It does
 *      NOT invent a continuous size, does NOT touch R_DM (the R3 crown width),
 *      does NOT add any math to st_forward. The tier is a config/alloc choice;
 *      a FIXED tier reproduces a FIXED, byte-identical mind (per-tier forward
 *      hash family S=0a5bf44c.. / M=63e8de33.. / L=67f2434f..).
 *    - The size knob is DISCRETE (the tier) so the (tier,nexpert) merge-cohort
 *      key (student.h:276) stays well-typed and there is no VLA.
 *    - HOSTED-ONLY: this TU is built only into the hosted kernel (boot/linux,
 *      boot/linux_x86_64) alongside student.c. On bare metal student.c is
 *      replaced by student_stub.o (weak no-ops), so dev_capacity.c is NOT in the
 *      bare-metal default link and the bare-metal crown (.text 755a20fa /
 *      0x2856a99b) is structurally unaffected.
 *
 *  Honest scope / deferrals (see the plan §6):
 *    - Boot-time sizing ONLY (sized ONCE at boot, never re-tiered at runtime).
 *      Dynamic thermal/pressure demotion -> S_n bus + the SS-4 shrink path,
 *      DEFERRED.
 *    - Cross-tier learning bridge (distillation) is NOT solved here. Device-
 *      sizing fragments the fleet into up to 3 student cohorts; they share the
 *      R3 crown unconditionally (no size axis) and student learning WITHIN a
 *      tier-cohort by merge — across cohorts only when distillation lands.
 *    - bare-metal real RAM (the FDT /memory parser) is DEFERRED: bare metal uses
 *      a build constant with ram_trust=LOW and the cores-only fallback, and in
 *      any case ships at M (student_stub on bare metal).
 */
#ifndef PKERNEL_LLM_DEV_CAPACITY_H
#define PKERNEL_LLM_DEV_CAPACITY_H

#include <stddef.h>
#include "student.h"

/* Trust levels for a measured signal (the LOW level drives the cores-only
 * fallback in tier_of). */
#define DEVCAP_TRUST_LOW   0   /* a build constant / unmeasurable (bare metal)  */
#define DEVCAP_TRUST_MED   1   /* OEM-skewed (Android totalMem)                 */
#define DEVCAP_TRUST_HIGH  2   /* directly measured (host /proc/meminfo, cores) */

/* The device-capability probe result (static capability only; NOT dynamic
 * thermal/battery state — that belongs to the S_n bus, plan §1). */
struct dev_capacity {
    unsigned int  cores;        /* sysconf / availableProcessors / GICD_TYPER  */
    unsigned long ram_bytes;    /* totalMem / MemTotal / build-const           */
    unsigned int  cores_trust;  /* DEVCAP_TRUST_*                              */
    unsigned int  ram_trust;    /* DEVCAP_TRUST_* (LOW on bare-metal const)    */
};

/* Resident-arena cost (bytes) of a tier == n_params*4*sizeof(float) at that
 * tier's dims — the SAME w|g|mu|vu arena st_init_tier mallocs (student.c:268).
 * COMPUTED from the const ST_TIERS dims, never a magic number; this is what
 * makes the tier_of thresholds (and the cert) non-vacuous. Returns 0 for an
 * out-of-range tier. */
size_t st_arena_bytes_for_tier(int tier);

/* The head-room factor: a device must have at least HEADROOM x the resident
 * arena free for the whole organism (OS + the teacher engine + the R3 crown +
 * net buffers + activation cache + Adam moments are already in the arena). The
 * tier_of thresholds are arena_cost(tier) * DEVCAP_HEADROOM, so they TRACK the
 * real cost: bump a tier's dims and the threshold moves with it. 20x maps the
 * plan's profiles exactly (512MB->S, 2GB->M, 8GB->L). */
#ifndef DEVCAP_HEADROOM
#define DEVCAP_HEADROOM  20u
#endif

/* tier_of(c): the mapping. RAM is the bottleneck (the model must FIT): the tier
 * is the MIN of the per-axis tiers, so we never round up past what RAM allows.
 * If RAM is a LOW-trust build constant (bare metal), it is IGNORED and the
 * trustworthy cores signal decides (the "どの端末でも壊れない > 精密" rule).
 * Returns ST_TIER_S / _M / _L. Pure; never allocates. */
int tier_of(const struct dev_capacity *c);

/* dev_capacity_probe(out): fill *out from the running host.
 *   cores : sysconf(_SC_NPROCESSORS_ONLN) (the pk_parallel.c:110 precedent).
 *   ram   : /proc/meminfo MemTotal (host); falls back to a LOW-trust build
 *           constant if unreadable.
 * FIXTURE INJECTION (so CI certs need no real hardware, plan §6 / risk #6):
 *   - PKERNEL_DEVICE_TIER = S|M|L  forces the returned tier directly (the
 *     verdict's mandated hook). When set, *out is filled with a synthetic
 *     profile whose tier_of() == the forced tier (HIGH trust), so the whole
 *     pipeline still runs.
 *   - PKERNEL_DEVICE_RAM_BYTES / PKERNEL_DEVICE_CORES override the raw measured
 *     values (HIGH trust) — the profile-injection the [device-fit] cert uses.
 *   - PKERNEL_DEVICE_RAM_TRUST = 0|1|2 overrides ram_trust (to exercise the
 *     LOW-trust cores-only fallback path).
 */
void dev_capacity_probe(struct dev_capacity *out);

/* st_init_device(m, seed): the ONE production sizing wrapper. Probes the device,
 * picks a tier via tier_of(), and st_init_tier()s the student at that tier WITH
 * an alloc-fail step-down: if the chosen tier's arena does not actually malloc
 * (a lying/optimistic RAM number), step DOWN a tier (L->M->S) and retry, so the
 * node boots a FITTING mind instead of OOM-crashing. The measurement proposes;
 * the allocator disposes.
 *
 * Determinism: this is a config/alloc event only — it selects WHICH deterministic
 * (seed,tier) model to build; it adds no math. For a fixed resulting tier the
 * model bytes are identical to st_init_tier(m, seed, tier). The chosen tier is
 * the DEVICE's: a modest host (<6 cores or <6.14GB) selects M (== st_init); a
 * capable host scales up to L; a tiny one steps down to S. So the per-node tier
 * varies — it is NOT a byte-identical default everywhere; what is invariant is
 * each tier's pinned forward hash (S/M/L) and the untouched R3 crown.
 *
 * Returns ST_OK on success (m is initialised at the FITTING tier), or ST_E_OOM
 * if even the smallest tier (S) cannot allocate. m->tier reports the chosen tier.
 *
 * FALSIFIER: built with -DDEVFIT_IGNORE_MEASURE this hardcodes tier=ST_TIER_L
 * and DISABLES the step-down, so a small device OOMs/fails — proving the
 * measurement is load-bearing. */
int st_init_device(st_model *m, uint32_t seed);

/* st_grow_ceiling_for_tier(tier): the local hard expert ceiling a device of this
 * tier imposes == ST_E_<tier> (S=2, M=4, L=8). This is the device half of
 * RULE [device-ceiling-fleet-grows-within]. Returns 0 for out-of-range. */
int st_grow_ceiling_for_tier(int tier);

/* st_fleet_expert_target(fleet_n, tier): the RECONCILED SS-4 growth target.
 *   e_target = min(cap_experts_of(fleet_n), ST_E_<tier>)
 * The DEVICE tier is the hard ceiling; SS-4's fleet-N curve grows nexpert WITHIN
 * it. Clamped to ST_E_<tier>, NOT the global CAP_E_MAX(=16). This closes SS-4
 * open-risk #7: a big-N fleet on an S device stays at ST_E_S=2 and never reaches
 * 16, so a watch never tries to host experts it cannot fit. Any future
 * st_grow_experts() caller MUST size its target through this function.
 *
 * `cap_experts_of` (degrade.c, the fleet-N capacity curve) is taken as a plain
 * int here so this TU does not drag in the kernel's degrade.h LP64 typedefs; the
 * caller passes cap_experts_of(N) (or N directly — the clamp is idempotent under
 * the min). Returns the clamped target in [ST_TOPK, ST_E_<tier>]. */
int st_fleet_expert_target(int fleet_cap_experts, int tier);

#endif /* PKERNEL_LLM_DEV_CAPACITY_H */
