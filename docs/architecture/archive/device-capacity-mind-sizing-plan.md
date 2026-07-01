# Device-capacity mind-sizing — measure the device, auto-fit the mind (HARDENED DESIGN)

> **ARCHIVED 2026-07-01 (doc-hygiene wave 2).** 正準は [[../device-capacity.md]]（§0.5 に DEVFIT-1 の SHIPPED 要約＝機構/ファイル/CI を畳んだ）。本文は DEVFIT-1 の設計記録＝年輪。

> **Status: SHIPPED (DEVFIT-1) — reconciled 2026-07-01.** The "DESIGN ONLY (no kernel
> code touched)" line below is STALE: the mind-sizing half shipped as
> `arch/common/llm/dev_capacity.c` (+ `student.c`), certified by `tests/llm/run_devfit.sh`
> with a load-bearing falsifier (`-DDEVFIT_IGNORE_MEASURE`), **now wired into default CI**
> (`.github/workflows/ci.yml:1258`). The cores half (SMP-AUTODETECT) had already shipped.
> Read the rest as the design record.

**Status (historical): DESIGN ONLY. Base commit `e83ebf3e`. Branch
`device_capacity_design`.** Honest > green. Every claim below is grounded in `file:line`.

> mk_pino's explicit direction (verbatim): **「デバイスのスペックを測って自動で合わせたい」**
> — measure the device's RAM/cores and **auto-fit** the mind's size to it. The **cores half**
> of this already shipped: SMP-AUTODETECT reads `GICD_TYPER` bits[7:5] at boot and wakes
> exactly that many CPUs (`docs/architecture/device-autodetect-plan.md`, commit `d31f7457`).
> This document designs the **named other half**: sizing the *mind* (the Cradle student) to
> the device, the deferred sibling that `device-autodetect-plan.md:25-35,297-343` and
> `device-capacity-verdict.md` both point at.

This is **not** an un-defer of the `device-capacity.md` continuous-`device_score`-into-
`capacity_score()` vision — that thread stays where the verdict left it. This doc designs
the **smallest discrete, byte-safe, falsifiable** way to make a device pick its **student
tier** (S/M/L), and states exactly how that reconciles with SS-4's fleet-driven expert
growth and with the one-mind crown.

---

## 0. Ground truth (cited — what EXISTS, so we do not reinvent)

### 0.1 The student is ALREADY tier-parameterized; nothing measures the device to pick a tier

The Cradle baby `st_model` is the ONLY substrate where size may vary
(`device-capacity-verdict.md:17-25`). The S/M/L tier machinery is **already built**:

- Tier dims live in a const table `ST_TIERS[ST_NTIER]` keyed by `ST_TIER_S/_M/_L`
  (`student.c:220-227`). Each tier is a `{d, dff, nlayer, nexpert}` 4-tuple
  (`student.c:222`).
- `st_init_tier(m, seed, tier)` copies that tuple into the runtime fields
  `m->d / m->dff / m->nlayer / m->nexpert` (`student.c:235-248`) and lays out a **heap**
  arena from them (`malloc`, `student.c:268-271`). The math body reads the runtime dims via
  `ST_DIMS(m)` (`student.c:124`, `:249`), so the same code runs any tier.
- **Every stack scratch array is bounded by the FIXED `ST_*_MAX` (== L-tier)**, never by the
  runtime `m->*` (`student.h:94-118`), so a tier is **not a VLA** (the no-VLA gate the
  verdict demands, `device-capacity-verdict.md:48-50`).
- The tier values (`student.h:76-99`): **S** = {d 64, dff 128, nlayer 2, nexpert 2}
  (`:77-80`); **M** (default, byte-identical to the legacy baby) = {128, 256, 4, 4}
  (`:83-86`); **L** = {256, 512, 6, 8} (`:89-92`). `ST_E_MAX == ST_E_L == 8`
  (`student.h:92,100`), `ST_D_MAX == ST_D_L` (`student.h:97`).
- The merge cohort key today is the **tier** (`student.h:276`: "S averages with S, M with M,
  L with L — a cross-tier blob is REFUSED"), fail-closed via `st_blob_tier_ok`
  (`student.h:280-285`, `student.c:1844`, `:1768`).

**The gap this design fills, stated precisely:** the tiers exist, but **no caller selects a
non-default tier from a device measurement.** `st_init` always uses M (`student.c:229-233`);
the only non-M callers are the SS-6 self-test fixture (`student_shell.c:765`,
`SS6LIVE_TIER`) and a peer mirror that copies `g_student.tier` (`student_shell.c:885`). The
production student boots M everywhere. **So the tier knob is real and proven; the missing
piece is the boot-time `tier = tier_of(device_measurement)` decision.** That is the whole of
this slice. (This is exactly the verdict's blessed "fold the capacity meter in as
observability" SLICE 0 (`device-capacity-verdict.md:63-72`) **extended by one step**: the
meter now also *picks the tier*.)

### 0.2 SS-4 sizes the EXPERT COUNT by FLEET N, within a tier

`SS-4` (`ss4-function-preserving-growth-plan.md`, `student.c:320-460`) grows
`m->nexpert` from `cap_experts_of(N)` where **N = alive fleet node count**
(`degrade.c:156-160,186`). Key facts that constrain us:

- `cap_experts_of(N) = clamp(N, 1, CAP_E_MAX)`, `CAP_E_MAX = 16` (`degrade.c:156-161`,
  `degrade.h:40`). The function's own honest label: it "does NOT size the router / the
  student's expert table … every model is fixed compile-time today" — SS-4 is the wave that
  makes it size the router (`degrade.c:148-159`).
- Growth ADDS **DEAD experts** (router-row=0, W2=0, `alive[e]=0` mask) that **provably never
  fire** ⇒ EXACT, byte-identical output for all inputs (`ss4-...-plan.md:100-148`). Turning
  one ON (resurrection) is deliberately ε-perturbing and gated by a *different* cert
  (`ss4-...-plan.md:152-200`).
- **SS-4 grows E only within `[ST_TOPK, ST_E_MAX]`** — open-risk #7
  (`ss4-...-plan.md:407-411`) explicitly reconciles the two ceilings: the router-sizing curve
  must clamp E to **`ST_E_MAX` (the tier's max, =8 for L)**, NOT `CAP_E_MAX` (=16, the
  display/degrade number). **This is the seam this design plugs into** (§3).
- The cohort key is extended `tier → (tier, nexpert)` (`ss4-...-plan.md:213-226`,
  `RULE [grow-cohort]`). Different `(tier, nexpert)` = different merge-island, bridged by
  distillation, never by ill-typed `gl_merge`.
- The single fleet-wide mind stays the R3 `rw[]` crown, which **has no expert axis at all**
  and is byte-identical regardless of student size (`ss4-...-plan.md:228-235`).

### 0.3 The crown is the R3 mind, has no size axis

The `[smp-one-mind]` crown `0x2856a99b…` / commit `755a20fa` hashes `r_forward` (R3,
`r3_incontext.c`), whose flat `rw[R_NP=21568]` layout is dense Embed+MHSA+FFN+Cls with
**no `nexpert`, no tier, no router-over-experts** (`ss4-...-plan.md:336-344`,
`device-capacity-verdict.md:11-16`). `gl_merge` exchanges exactly `R_NP` floats with a
fail-closed shape guard (`device-capacity-verdict.md:14-16`). **Making R3 per-device variable
would break the merge and is the one thing we MUST NOT do** (`device-capacity-verdict.md:17`).
The capacity surgery that raised comfortable-N 4→16 moved the **R3 attention width**
(`R_DM` 32→48, lock-stepped with vocab — `r3_vocab.h:6,32`); `R_DM` is a known size lever but
it is a **fleet-wide compile-time** dimension of the *crown*, not a per-device knob. **This
design does NOT touch `R_DM` or any R3 dim** (see §5, LENS A).

### 0.4 What can be measured, per target (cited)

| signal | bare-metal aarch64 | Linux/x86_64 host | Android (the fleet) |
|---|---|---|---|
| **cores** | `GICD_TYPER` bits[7:5] **SHIPPED** (`device-autodetect-plan.md:75-110`); RPi3 = build-constant 4 (BCM2837 is not a GIC, `device-autodetect-plan.md:124-132`) | `sysconf(_SC_NPROCESSORS_ONLN)` — **proven in tree** (`pk_parallel.c:110-112`) | `Runtime.availableProcessors()` — **already on the engineer page** (`LogActivity.kt:272,338`) |
| **RAM total** | DTB `/memory` node (needs the deferred FDT parser, `device-autodetect-plan.md:136-147`) **OR** the linker `SYSTEMAREA_END` / `CFN_REALMEMEND` build constant (`utk_config_depend.h:61`) | `/proc/meminfo` `MemTotal` (host) — not yet wired, `device-capacity.md:280-282` | `ActivityManager.MemoryInfo.totalMem` — not yet wired (`device-capacity.md:75,280`) |
| **RAM free / pressure** | none (no allocator instrumentation exposed) | `/proc/meminfo` `MemAvailable` | `MemoryInfo.availMem` / `lowMemory` flag |
| **storage** | none (no block device on QEMU virt) | `statvfs` | `StatFs` (deferred — not load-bearing for mind size) |

**Honest measurement gaps (LENS C, §-final):** bare-metal has NO portable RAM number without
the FDT parser; QEMU virt's `-m` is invisible to the kernel except via the (clobbered, then
deferred) DTB pointer (`device-autodetect-plan.md:69-73`). So **on bare-metal the RAM input is
a build-constant** until the FDT follow-up lands — mirroring the RPi3 core-count precedent.
Android `totalMem` is OEM-reserved-skewed (`device-capacity.md:75`, 中 reliability). The
**one cross-OEM-trustworthy, host-and-Android-portable, hardware-independent signal already in
the tree is `sysconf`/`availableProcessors` cores** (`pk_parallel.c:110`, `LogActivity.kt:272`).
This drives the conservative default (§2.3).

---

## 1. What is measured (the device-capability probe)

A boot-time, **fixture-injectable** probe (the verdict's mandated env hook
`PKERNEL_DEVICE_TIER`, `device-capacity-verdict.md:71`) produces a small struct. This is the
SLICE-0 capacity meter the verdict already blessed, plus one new consumer (the tier pick).

```c
/* Proposed: arch/common/llm/dev_capacity.{c,h} (NEW TU, student-adjacent).
 * NOT in degrade.c — degrade.c is FLEET-N capacity (§0.2); this is the LOCAL
 * DEVICE. The two are orthogonal axes (device-capacity.md:58-60). */
struct dev_capacity {
    unsigned int  cores;        /* sysconf / availableProcessors / GICD_TYPER (§0.4) */
    unsigned long ram_bytes;    /* totalMem / MemTotal / build-const (§0.4 gaps)      */
    unsigned int  cores_trust;  /* 0..2: high everywhere                              */
    unsigned int  ram_trust;    /* 0..2: high host, med Android, LOW bare-metal const */
};
```

Two honest splits, taken straight from `device-capacity.md:80-86`:

- **Static capability** (cores, RAM total) = the device's *potential* ceiling → the **size
  material** (this slice consumes it). Measured once at boot.
- **Dynamic state** (thermal headroom, battery, memory pressure) = *current* head­room →
  the `S_n` interoception bus, NOT the size knob (`device-capacity.md:84-86`). **This slice
  does NOT use thermal/battery.** Dynamic demotion (an L device that gets hot shrinking to S)
  is the SS-4-shrink + `S_n`-damp problem, explicitly **deferred** (§6, and
  `ss4-...-plan.md:262-281`, `device-capacity-verdict.md:60-61`).

**What CANNOT be measured where (honest, restated):** bare-metal RAM (FDT deferred) → use a
build-constant `ram_bytes` with `ram_trust=LOW`; QEMU-virt RAM (DTB clobbered) → same; Android
device-wide CPU% (SELinux-denied, `LogActivity.kt:320-327`) → not needed for sizing. The probe
**never fabricates precision**: a LOW-trust signal is flagged and the mapping (§2.3) degrades
to the cores-only conservative path.

---

## 2. What "mind size" maps to + the mapping function `tier_of(ram, cores)`

### 2.1 The size knob IS the existing tier — discrete, not continuous

`device-capacity.md` proposed a **continuous** `device_score` multiplied into
`capacity_score()`. **This design deliberately does NOT do that** — it is the part the verdict
flagged as high-risk and unverified (`device-capacity-verdict.md:27-44`). Instead the device
measurement picks one of the **already-built discrete tiers** `{S, M, L}` (`student.c:223-226`).
Discrete is the right primitive because:

- **Mergeability requires discrete shapes** — the cohort key is `(tier, nexpert)`
  (`student.h:276`, `ss4-...-plan.md:213-226`). A continuous per-device width fractures the
  fleet into N singleton merge-islands (the verdict's relabeling critique,
  `device-capacity-verdict.md:31-33`). Discrete tiers give a *bounded* number of cohorts
  (≤3), each mergeable internally.
- **The math already supports exactly these three** (`ST_TIERS`, `student.c:223`) with the
  no-VLA guarantee; a continuous d_model would re-open the VLA risk
  (`device-capacity-verdict.md:38-40`).

So: **device measurement → tier byte ∈ {S, M, L} → `st_init_tier(m, seed, tier)`.** The tier
sets `(d, dff, nlayer, nexpert_base)`. That is the entire surface this slice touches in the
mind.

### 2.2 Why the tier (not `R_DM`)

The capacity-surgery memory says `R_DM` (R3 attention width) is "a known size lever" — true,
but `R_DM` is a **crown** dimension: it is fleet-wide, compile-time, and lock-stepped with the
R3 vocab (`r3_vocab.h:6,32`). Per-device variation of `R_DM` would (a) break `gl_merge`'s
`R_NP` shape guard (`device-capacity-verdict.md:14-16`) and (b) change the crown hash for every
device → the catastrophic "different size = different mind, no merge" failure
(`device-capacity-verdict.md:17`). **The student tier is the correct lever** because the
student is heap-allocated, already tier-variable, and explicitly the "ONLY place variability
may live" (`device-capacity-verdict.md:17-25`). `R_DM` stays fixed; the student tier moves.

### 2.3 The mapping `tier_of(ram, cores)` — bottleneck, conservative, fixture-discoverable

Memory is the **bottleneck**: a strong CPU cannot run a model that does not fit in RAM
(`device-capacity.md:92-104` — capacity is bottleneck-dominated, the `⊗` not `+`). So the tier
is the **min** of the per-axis tiers, and we **never round up past what RAM allows**:

```c
/* dev_capacity.c — the proposed mapping. Thresholds are PROPOSALS to be
 * DISCOVERED from a measured device histogram (device-capacity.md:124-131,
 * the validator-trap discipline), printed in the cert, NOT frozen as gospel. */
int tier_of(const struct dev_capacity *c)
{
    /* RAM tier: the HARD ceiling (the model must FIT). The bytes below are the
     * resident-arena cost of each tier == n_params*4*sizeof(float)
     * (student.c:268: w+g+mu+vu) at that tier's dims — COMPUTED, not guessed,
     * by st_arena_bytes_for_tier(t) so the threshold tracks the real cost. */
    int ram_tier;
    if      (c->ram_bytes >= L_FIT_BYTES) ram_tier = ST_TIER_L;
    else if (c->ram_bytes >= M_FIT_BYTES) ram_tier = ST_TIER_M;
    else                                  ram_tier = ST_TIER_S;

    /* cores tier: a SOFT preference (parallelism head-room). */
    int core_tier;
    if      (c->cores >= 6) core_tier = ST_TIER_L;
    else if (c->cores >= 3) core_tier = ST_TIER_M;
    else                    core_tier = ST_TIER_S;

    /* BOTTLENECK: the smaller wins — never pick a tier RAM can't hold. */
    int t = ram_tier < core_tier ? ram_tier : core_tier;

    /* TRUST degrade: if RAM is a LOW-trust build constant (bare-metal, §1),
     * IGNORE it and fall back to the cores-only tier (the trustworthy signal).
     * This is the "どの端末でも壊れない > どの端末でも精密" rule
     * (device-capacity.md:330-334). */
    if (c->ram_trust == 0) t = core_tier;

    return t;   /* ST_TIER_S / _M / _L */
}
```

Key properties (each is a cert obligation, §4):

- **`L_FIT_BYTES`/`M_FIT_BYTES` are COMPUTED from the tier's real arena cost**
  (`n_params*4*sizeof(float)`, `student.c:266-268`) times a head-room factor, **not a magic
  number**. This makes the cert non-vacuous: the threshold *is* the thing that prevents OOM.
- **Fail-SAFE direction:** an out-of-range tier already clamps to M (the default,
  `student.c:239`), and the bottleneck-min means we **never oversize**. A device that
  mis-reports *high* RAM (LENS C) gets caught by the cores bottleneck and, if cores also lie,
  by the runtime malloc-fail fallback (§2.4).
- **Default M:** with no probe / LOW-trust everything / fixture absent, the result is M — the
  byte-identical legacy baby (`student.h:47-49`). The default fleet is unchanged.

### 2.4 OOM is refused, not crashed (the alloc is the truth)

`st_init_tier` already returns `ST_E_OOM` if the arena `malloc` fails (`student.c:270`). The
sizing wrapper uses this as the **final arbiter**: if `tier_of` picks L but the L arena does
not actually allocate, **step down a tier and retry** (L→M→S), so a lying RAM number degrades
to a *fitting* mind instead of a dead node. The measurement *proposes*; the allocator
*disposes*. This makes the cert's "small device refuses to OOM" leg true even if the RAM
signal is wrong (LENS C mitigation).

---

## 3. Reconciliation with SS-4 / fleet-sizing (the load-bearing rule)

**Both axes apply, and they compose without fighting. State the rule precisely:**

> **RULE [device-ceiling-fleet-grows-within]:** The **DEVICE** picks the **tier**
> (S/M/L via `tier_of`, §2). The tier sets the **local hard ceiling** on every dim,
> including the maximum expert count `ST_E_<tier>` (S=2, M=4, L=8 — `student.h:80,86,92`).
> **SS-4's `cap_experts_of(N)` then grows `m->nexpert` WITHIN that ceiling as the fleet
> grows**, clamped to the tier's `ST_E_<tier>`, **NOT** to the global `CAP_E_MAX`=16.
> Device = the ceiling; fleet = growth under it.

This is **exactly** the seam SS-4 open-risk #7 leaves open
(`ss4-...-plan.md:407-411`): *"The router-sizing curve must clamp E to `ST_E_MAX`, not
`CAP_E_MAX`."* Today SS-4 clamps to a single `ST_E_MAX`=8 (the L-tier) because every node is
M-tier. **Device-capacity makes `ST_E_MAX` per-node = the chosen tier's `ST_E_<tier>`:**

```c
/* the reconciled growth ceiling — the ONE change to SS-4's curve. */
int e_ceiling = ST_TIERS[m->tier].nexpert;        /* S=2, M=4, L=8 */
int e_target  = (int)cap_experts_of(alive_node_cnt);  /* SS-4, fleet N */
if (e_target > e_ceiling) e_target = e_ceiling;       /* device ceiling WINS */
/* st_grow_experts(m, e_target) — never exceeds the tier the device can hold. */
```

**Neither overrides the other; they intersect.** The device says "this phone can hold at most
L = 8 experts"; the fleet says "with N=5 alive, grow to 5 experts." An L-phone grows to 5 (≤8,
fine). An S-watch (ceiling 2) with N=5 alive stays at 2 — the device ceiling clamps it, and
**that is correct**: a watch must not try to host 5 experts it cannot fit.

### 3.1 The cohort rule still holds — a phone and a watch are different cohorts

The merge cohort key is `(tier, nexpert)` (`ss4-...-plan.md:213-226`, `student.h:276`). With
device-sizing:

- An **L-phone** and an **S-watch** have **different `tier`** ⇒ different cohort ⇒
  `st_blob_tier_ok` REFUSES a cross-tier blob (`student.h:280-285`) ⇒ they **never** enter
  `gl_merge`/`st_merge_cohort` together (ill-typed `n` is structurally impossible —
  `ss4-...-plan.md:208-211`). They are merge-islands by construction, exactly as today's
  hardcoded tiers would be if any node used them.
- They **still share the mind** — at the **R3 `rw[]` crown** (no expert axis, byte-identical
  fleet-wide, §0.3), and across student cohorts via **distillation** (the teacher path), NOT
  merge (`ss4-...-plan.md:228-235`). **This is the same honest "one mind at the crown,
  per-cohort student accelerators bridged by distillation" story SS-4 already states; device-
  sizing changes only *which* cohort a node lands in (now measured, not default-M).**

### 3.2 Honest consequence (do not hide it)

Today the whole fleet is one cohort (all M, `student.c:229-233`), so `gl_merge` of students
just works. **Device-sizing FRAGMENTS the fleet into up to 3 student cohorts.** A watch can no
longer receive a phone's *student-resident* learning by merge — only via the R3 crown
(byte-identical) and via distillation (the `ss4-...-plan.md` / `native-student.md` NS-2 gap,
admittedly unverified — `device-capacity-verdict.md:34-36`). **This is the verdict's exact
"one mind gets quietly relabeled" critique (`device-capacity-verdict.md:31-33`), and this
design does NOT pretend distillation is solved.** It ships the *tier-pick mechanism* and the
*cohort isolation* (both buildable + certifiable now); the cross-tier *learning bridge* stays
the deferred distillation gap. Honest headline: **"device-sized nodes share the R3 crown
unconditionally; student learning is shared within a tier-cohort by merge and across cohorts
only when distillation lands."**

---

## 4. The cert `[device-fit]` (falsifiable, non-vacuous)

Mirrors SMP-AUTODETECT's "**same binary**, `-smp 2/4/8`, adapts" proof
(`device-autodetect-plan.md:244-293`) — here the axis is a **simulated device profile**,
fixture-injected (`PKERNEL_DEVICE_TIER` + a RAM/cores fixture), so CI needs no real hardware
(the salty-bug lesson, `device-capacity-verdict.md:71`).

**Claim.** The **SAME student binary**, given different injected device profiles, sizes its
student to the matching tier and **refuses to OOM** a small device.

**Observable PASS conditions (in-process, no network — the SS-3/SS-6 cert style):**

| injected profile | `tier_of` result | `m->tier` after init | `m->nexpert` ceiling | arena fits |
|---|---|---|---|---|
| `ram=8GB, cores=8` | L | `ST_TIER_L` | ≤ 8 | yes |
| `ram=2GB, cores=4` | M | `ST_TIER_M` | ≤ 4 | yes |
| `ram=512MB, cores=2` | S | `ST_TIER_S` | ≤ 2 | yes |
| `ram=512MB, cores=8` | **S** (RAM bottleneck wins over cores) | `ST_TIER_S` | ≤ 2 | yes |
| `ram=LOW-trust const, cores=2` | S (cores-only fallback) | `ST_TIER_S` | ≤ 2 | yes |

Procedure: for each profile, inject the fixture → call the sizing wrapper →
`st_init_tier(m, SEED, tier_of(...))` → ASSERT `m->tier` and `m->d/dff/nlayer/nexpert`
equal the expected `ST_TIERS[tier]` tuple → ASSERT the arena allocated (no `ST_E_OOM`). Print
the measured raw (`ram_bytes`, `cores`, trust) AND the chosen tier AND the computed
`L_FIT_BYTES/M_FIT_BYTES` thresholds, per profile (the validator-trap "print the numeric
basis" discipline, `device-capacity.md:316-318`).

**Monotonicity leg `[device-fit-monotone]`:** sweep RAM (and cores) monotonically up ⇒
`tier_of` is **non-decreasing** (saturates at L, never inverts) — the analogue of
`[capacity-mono]` (`degrade.c:285-313`).

**THE FALSIFIER (load-bearing, a real runnable control — `feedback_cert_must_cover_all_paths`):**
build `-DDEVFIT_IGNORE_MEASURE`, which **hardcodes `tier = ST_TIER_L`** and ignores
`tier_of`. Then:

- under the `ram=512MB` profile: the hardcoded-L build tries to `malloc` the L arena
  (`~4× n_params` floats at d=256) on a device that cannot hold it → `st_init_tier` returns
  `ST_E_OOM` (`student.c:270`) **and the step-down retry is disabled in the falsifier** →
  the node **fails to bring up its student** → `[device-fit] FAIL`. **This proves the
  measurement is load-bearing**: ignore it and a small device OOMs/refuses the big mind.
- under the `ram=8GB` profile the falsifier "passes" (L fits) — which is WHY the small-RAM
  profile must be in the cert; a falsifier that only ran the big profile would be vacuous.

This is the same falsifier discipline as SMP-AUTODETECT's `-DSMP_FORCE_NCPU=4`
(`device-autodetect-plan.md:272-284`) and SS-4's `-DSS4_GROW_NAIVE`
(`ss4-...-plan.md:305-312`): a build flag that breaks the one mechanism under test and proves
the green is not vacuous.

**Non-vacuity guard:** the cert ALSO asserts that the `tier_of` thresholds are the *computed*
arena costs (§2.3), so a degenerate `tier_of` that always returns M would FAIL the L and S
profiles (it would mis-size, the assertion on `m->tier` catches it).

**`[device-fit-ceiling]` (reconciliation, §3):** with `m->tier = S` injected and a large
fleet `alive_node_cnt = 10`, ASSERT SS-4 growth clamps `m->nexpert ≤ ST_E_S` (=2), **not**
`cap_experts_of(10)`=10. Falsifier: clamp to `CAP_E_MAX` instead of `ST_E_<tier>` → the S
node grows past its arena ceiling → OOM/overrun. (Covers SS-4 open-risk #7.)

---

## 5. Byte-identity / crown gating (LENS A + LENS B)

### 5.1 The crown stays byte-identical for the shipped size — by construction

The crown `0x2856a99b…` (commit `755a20fa`) hashes `r_forward` (R3), which has **no expert/
tier axis** (§0.3, `ss4-...-plan.md:336-344`). Device-sizing touches **only** (a) a new
`dev_capacity.{c,h}` TU and (b) the **caller** of `st_init_tier` (picking the tier byte). It
does **not** edit `r_forward`, `r3_onemind_forward_hash`, `gl_merge`, `R_DM`, `R_NP`, or any
file/dimension the crown hashes. **The crown does NOT need re-pinning** — same argument SS-4
makes (`ss4-...-plan.md:336-344`).

### 5.2 Per-size determinism + a FAMILY of crowns (LENS A)

**Does picking a size at runtime perturb the math for a FIXED size?** No. `st_init_tier`
already produces a deterministic model for a given `(seed, tier)` (`student.h:205-207`:
"same tier reproduces the same weights deterministically"), and the forward reads the runtime
dims via `ST_DIMS` with **no float-contraction** (`-ffp-contract=off`, the salty-bug law).
The sizing decision is a **config/alloc event** that selects which deterministic model to
build — it adds no math to `st_forward`. For a FIXED tier, the bytes are identical with or
without the probe.

**A different-size mind IS a different mind — so what is "byte-identity"?** Each tier has its
**own** forward-hash. SS-6 already pins **M = `63e8de333e995913`** and **L =
`67f2434f50e791b6`** (`ss4-...-plan.md:46,348`) — a **family of crowns**, one per tier. This
design's obligation: **the M-tier hash must remain `63e8de333e995913` after the sizing wrapper
lands** (the M path is byte-untouched — the wrapper only *chooses* M, it does not change how M
is built), and the S-tier needs its own pinned hash added (S has no SS-6 hash yet — that is a
named impl deliverable). The single fleet-wide *shared* identity remains the R3 crown
(`0x2856a99b…`), unchanged. So:

> **Crown story (per-size):** ONE shared crown (R3 `rw[]`, `0x2856a99b…`) across ALL sizes
> (no size axis). PLUS a per-tier student forward-hash family (M=`63e8…`, L=`67f2…`, S=TBD).
> Device-sizing must (1) leave the R3 crown untouched, (2) leave the M-tier hash unchanged
> (the default path is byte-identical), (3) pin the S-tier hash it newly exercises. If the
> M hash moves after the edit, **STOP** (the "re-prove 755a20fa or STOP" discipline,
> `ss4-...-plan.md:346-350`).

### 5.3 The gating (LENS B — sizing is config/alloc, not a math change)

- The default build (no `dev_capacity` consumer wired, or fixture absent) selects **M** ⇒
  byte-identical to today (`student.c:229-233`, `student.h:47-49`). The shipped kernel + the
  existing crowns are **unaffected** unless a device explicitly measures a non-M tier.
- The new TU `dev_capacity.c` is **off the R3/crown link path entirely** (student-adjacent,
  like the SS-4 edit `ss4-...-plan.md:351-353`). No default-linked R3 function is edited
  (contrast ②.2b-ii, which had to edit `knl_make_ready`).
- **Bare-metal gating:** on a board with only a build-constant RAM (LOW trust), the mapping
  falls back to the cores-only tier (§2.3). For the **shipped bare-metal kernel** the simplest
  honest default is to keep selecting **M** (the crown's shipped size) unless the board's
  build constant + GICD_TYPER cores clearly indicate otherwise — i.e. **the bare-metal crown
  ships at M and stays byte-identical**; device-sizing is exercised first on the host/Android
  fleet (where RAM is trustworthily measurable). State this so nobody reads "device-sizing
  shipped" as "the bare-metal crown changed size."

---

## ADVERSARIAL self-hardening

### LENS A — crown / determinism (expanded)

1. **Runtime size ≠ per-size math perturbation.** Verified above (§5.2): `st_init_tier` is
   deterministic per `(seed, tier)`; `st_forward` adds no math from the sizing event;
   `-ffp-contract=off` holds per tier. PASS by construction, gated by the per-tier hash
   re-verification (§5.2).
2. **Family of crowns, each pinned.** The risk is shipping a tier whose forward-hash is NOT
   pinned (a silent-drift hole). Mitigation: the cert **refuses to size to a tier that has no
   pinned SS-6 hash** — so S must get a pinned hash before S is selectable in production. M and
   L are already pinned (`ss4-...-plan.md:46`). This makes the crown family **complete before
   use**, not retrofitted.
3. **The M default must not move.** The single biggest determinism risk: the sizing wrapper
   accidentally perturbs the M path (e.g. by re-seeding, by a different malloc order). Gate:
   re-assert M = `63e8de333e995913` (and the R3 crown `0x2856a99b…`) **after** the wrapper
   lands, for a fixture that selects M, or STOP (`ss4-...-plan.md:346-350`).

### LENS B — reconciliation correctness (can device-ceiling and fleet-growth fight?)

- **The interaction is an INTERSECTION, not a tug-of-war** (§3). `e_target = min(cap_experts_of(N),
  ST_E_<tier>)`. They cannot fight because the device ceiling is a hard `min`; the fleet can
  only request growth *up to* it. There is no path where the fleet forces a node past its
  arena.
- **Graceful degradation, not OOM:** if the fleet wants more experts than the tier's ceiling,
  the node simply **does not grow** past the ceiling (it stays at `ST_E_<tier>`). No alloc is
  attempted beyond the tier's arena, so **no OOM** — the alloc was sized once at `st_init_tier`
  to the tier's max; growth within the tier is a `realloc`-class reshard bounded by the same
  ceiling (`ss4-...-plan.md:79-98`). Covered by `[device-fit-ceiling]` (§4).
- **Tie to SS-4 grow/shrink + cohort rule:** growth stays the cheap, EXACT direction
  (`ss4-...-plan.md:256-260`); a node that *changes tier at runtime* (the dynamic-demotion
  case) is the **shrink** path (data-loss, `[grow-shrink-fold]`), explicitly **DEFERRED**
  (`ss4-...-plan.md:262-281`, §6). **This slice sizes the tier ONCE at boot and does NOT
  change it at runtime** — so the hard shrink/cohort-migration problem does not arise here.
  The cohort rule (`(tier,nexpert)`, §3.1) holds unchanged: a boot-time S node is simply in
  the S cohort for its lifetime.

### LENS C — measurement trust + failure

- **A device that mis-reports RAM (high):** caught by the cores bottleneck-min (§2.3) and, as
  the final backstop, the **alloc-fail step-down** (§2.4) — the model that actually fits is
  the one that boots. A device reporting *low* RAM under-sizes (safe; smaller mind, no crash).
- **A measurement that changes mid-run (Android memory pressure):** **NOT used by this slice.**
  Static capability is measured once at boot (§1); dynamic pressure belongs to `S_n`
  (`device-capacity.md:84-86`) and the deferred shrink path. So a memory-pressure spike does
  **not** resize the mind mid-run here — it would (in a later slice) damp the `effective_budget`
  via `S_n`, never re-tier in place. Honest bound: **this slice cannot follow a device that
  gains/loses RAM after boot** — that is the deferred dynamic thread.
- **Bare-metal DTB-memory-node absent:** mirrors SMP-AUTODETECT's RPi3 precedent exactly
  (`device-autodetect-plan.md:124-132`) — the count/size becomes a **build constant** with
  `ram_trust=LOW`, and the mapping degrades to cores-only (§2.3). The shipped bare-metal kernel
  ships at **M** (§5.3) so the crown is unaffected. The FDT-parser follow-up
  (`device-autodetect-plan.md:136-147`) is the path to a real bare-metal RAM number, and is
  **deferred** here.
- **Capability spoofing (a node lies about its tier to grab more experts):** out of scope for
  a *self-sizing local* slice — there is no advertisement here (no SWIM `device_score` field;
  that is `device-capacity.md:185-197` DEVCAP-2, deferred). A node sizes ITSELF; it cannot
  oversize itself past its own alloc (§2.4). The cross-node spoofing threat
  (`device-capacity.md:350-353`) only arises once tiers are *advertised*, which this slice
  does not do.

---

## The cert + falsifier (restated, crisp)

- **`[device-fit]`:** same student binary + injected device profiles → student sizes to the
  matching tier and refuses to OOM the small device (§4 table). **Falsifier
  `-DDEVFIT_IGNORE_MEASURE`** (hardcode L, ignore the measurement) → the 512MB profile OOMs /
  fails to bring up the student → cert FAILS, proving the measurement is load-bearing.
- **`[device-fit-monotone]`:** RAM/cores swept up ⇒ tier non-decreasing.
- **`[device-fit-ceiling]`:** S-tier node + large fleet ⇒ `nexpert ≤ ST_E_S`, NOT
  `cap_experts_of(N)` — the SS-4 reconciliation (open-risk #7).
- **Non-vacuity:** `L_FIT_BYTES/M_FIT_BYTES` are the *computed* per-tier arena costs
  (`student.c:266-268`), printed in the cert; a degenerate always-M `tier_of` fails the L/S
  profiles.

## Byte-identity / crown gating (restated)

ONE shared R3 crown (`0x2856a99b…`, no size axis) across all sizes; PLUS a per-tier student
forward-hash family (M=`63e8de333e995913`, L=`67f2434f50e791b6`, **S=TBD — must be pinned
before S is selectable**). Obligations: leave the R3 crown + the M-tier hash byte-identical
(the wrapper only *chooses* M, never changes how M is built — re-prove or STOP); pin the
S-tier hash; bare-metal ships at M.

## Reconciliation rule with SS-4 (restated)

**RULE [device-ceiling-fleet-grows-within]:** DEVICE picks the tier (S/M/L) = the local hard
expert ceiling `ST_E_<tier>`; SS-4's `cap_experts_of(N)` grows `nexpert` WITHIN that ceiling
as the fleet grows (`e_target = min(cap_experts_of(N), ST_E_<tier>)`, clamped to `ST_E_<tier>`
NOT `CAP_E_MAX`). Different tiers are different `(tier,nexpert)` cohorts (phone ≠ watch),
isolated by `st_blob_tier_ok`, sharing the mind at the R3 crown + across cohorts by
distillation, never by ill-typed merge. This closes SS-4 open-risk #7
(`ss4-...-plan.md:407-411`).

## Honest scope / deferrals

- **DEFERRED — continuous `device_score`** (`device-capacity.md`'s multiply-into-
  `capacity_score()`): we use **discrete tiers** instead (mergeable, no VLA). The continuous
  path stays where the verdict left it.
- **DEFERRED — dynamic re-tiering / thermal demotion:** boot-time sizing only; runtime
  pressure → `S_n` (later), not in-place resize. The shrink data-loss hole
  (`ss4-...-plan.md:262-281`, `device-capacity-verdict.md:60-61`) is untouched.
- **DEFERRED — cross-tier learning bridge (distillation):** unverified NS-2 gap
  (`device-capacity-verdict.md:34-36`). We ship cohort *isolation* + the R3-crown share; the
  distillation bridge is NOT claimed solved.
- **DEFERRED — tier advertisement / heterogeneous fleet sharding** (SWIM `device_score`
  field, DEVCAP-2/3, `device-capacity.md:320-325`): a node self-sizes locally; no advertise.
- **DEFERRED — bare-metal real RAM** (FDT `/memory` parser, `device-autodetect-plan.md:136-147`):
  build constant + cores-only fallback for now.
- **HONEST benefit bound:** like SS-4, the *visible* payoff is small until a bigger baby exists
  (`ss4-...-plan.md:370-375`, `device-capacity.md:251-270`) — today's M baby (d=128) runs on a
  watch already, so S/L tiers are mostly inert until SS-7 grows the baby. **The value shipped
  now is the mechanism + the EXACT cert + the cohort isolation**, proving "measure the device,
  fit the mind" is real and crown-safe — not an accuracy jump.

## Open risks for the implementer

1. **(biggest) The M default must stay byte-identical.** The sizing wrapper must select M
   without perturbing how M is built. Re-assert M=`63e8de333e995913` AND R3 crown
   `0x2856a99b…` after the edit, both arches, `-ffp-contract=off`, `-O1`, or STOP
   (`ss4-...-plan.md:346-350`, the salty-bug discipline).
2. **Pin the S-tier forward-hash BEFORE S is selectable.** L+M are pinned; S is not. A device
   sizing to S without a pinned S-crown is a silent-drift hole (LENS A.2). Add the S hash to
   the SS-6 self-test first.
3. **`L_FIT_BYTES`/`M_FIT_BYTES` must be COMPUTED from `n_params*4` (`student.c:266-268`),
   not magic numbers** — else the cert is vacuous and the OOM-refusal is luck. Add
   `st_arena_bytes_for_tier(tier)`.
4. **The SS-4 ceiling clamp must read `ST_E_<m->tier>`, NOT `CAP_E_MAX`** (open-risk #7).
   Every SS-4 growth site (`st_grow_experts` callers) must clamp to the per-tier ceiling, not
   the global 16. Enumerate them (the cohort-guard discipline,
   `feedback_cert_must_cover_all_paths`).
5. **Alloc-fail step-down (§2.4) must be REAL**, not assumed — wire L→M→S retry on
   `ST_E_OOM` so a lying RAM number degrades to a fitting mind. The falsifier disables it to
   prove it is load-bearing.
6. **Measurement must be fixture-injectable** (`PKERNEL_DEVICE_TIER` + a RAM/cores fixture) so
   CI certs without real hardware (`device-capacity-verdict.md:71`). Do NOT make the cert
   depend on real device RAM.
7. **No VLA in `dev_capacity.c` or the wrapper** — it does no per-tier stack arrays
   (it only picks a byte); the student's no-VLA guarantee (`student.h:94-118`) is preserved
   because the tier still binds scratch to `ST_*_MAX`.
8. **Honest cohort fragmentation** — document in the readout that device-sizing splits the
   fleet into up to 3 student cohorts (§3.2); do NOT let a UI imply a watch and a phone merge
   their student learning directly.

## First IMPL slice recommendation (the smallest real device-fit cert to ship first)

**DEVFIT-1 — single-node, host-only, fixture-injected, no fleet, no distillation, no advertise:**

1. Add `arch/common/llm/dev_capacity.{c,h}`: the `struct dev_capacity` probe (cores via the
   already-proven `sysconf(_SC_NPROCESSORS_ONLN)` `pk_parallel.c:110`; RAM via `/proc/meminfo`
   `MemTotal` on host; **fixture override via `PKERNEL_DEVICE_TIER` / env RAM+cores**), plus
   `st_arena_bytes_for_tier(tier)` (computed from `n_params*4`) and `tier_of()` (§2.3).
2. Wire ONE caller: a sizing wrapper `st_init_device(m, seed)` = `st_init_tier(m, seed,
   tier_of(probe()))` with the L→M→S alloc-fail step-down (§2.4). Leave `st_init` (M-default)
   untouched so the default fleet is byte-identical.
3. Cert `[device-fit]` + falsifier `-DDEVFIT_IGNORE_MEASURE` (§4) — the three-row profile table
   (S/M/L), in-process, host, fixture-injected. Add the S-tier SS-6 forward-hash pin (risk #2).
4. **Explicitly OUT of DEVFIT-1:** the SS-4 ceiling reconciliation cert
   `[device-fit-ceiling]` (do it in DEVFIT-2, *after* SS-4 lands its growth code — they both
   touch the expert-count path and must be sequenced, `ss4-...-plan.md:376-380`); Android RAM
   wiring; advertisement; dynamic re-tiering.

Rationale: DEVFIT-1 is the **smallest thing that makes "measure the device → fit the mind"
TRUE and FALSIFIABLE** (a small fixture sizes to S, a big one to L, hardcoding-L OOMs the small
one), on the host, with zero new distributed-systems surface and zero crown risk (M path
byte-identical, R3 untouched). It is the exact mind-sizing analogue of SMP-AUTODETECT's
"same binary, `-smp 2/4/8`" proof — measurement drives size, the same binary adapts.

---

*Grounding index (file:line):* tiers exist `student.c:220-227,235-248`; runtime dims +
heap arena `student.c:266-271`, `ST_DIMS` `student.c:124,249`; no-VLA scratch bound to MAX
`student.h:94-118`; tier values `student.h:56-99` (`ST_E_S/M/L=2/4/8`, `ST_E_MAX=ST_E_L=8`
`:92,100`); default M `student.c:229-233`, `student.h:47-49`; only non-M callers are fixtures
`student_shell.c:765,885`; cohort key = tier `student.h:276`, `st_blob_tier_ok`
`student.h:280-285`, `student.c:1768,1844`; OOM return `student.c:270`. SS-4: `cap_experts_of`
`degrade.c:156-161,186`, honest label `:148-159`, `CAP_E_MAX=16` `degrade.h:40`; DEAD-expert
EXACT growth `ss4-...-plan.md:100-148`; cohort `(tier,nexpert)` `ss4-...-plan.md:213-226`;
ceiling open-risk #7 `ss4-...-plan.md:407-411`; crown untouched `ss4-...-plan.md:336-344`;
SS-6 tier hashes M/L `ss4-...-plan.md:46,348`. Crown: R3 no size axis
`device-capacity-verdict.md:11-17`; `R_DM`/vocab lock-step `r3_vocab.h:6,32`. Measurement:
cores `pk_parallel.c:110-112`, `LogActivity.kt:272,338`, GICD_TYPER `device-autodetect-plan.md:75-110`;
RAM gaps `device-capacity.md:75,280-282`, `utk_config_depend.h:61`, FDT defer
`device-autodetect-plan.md:136-147`; Android CPU SELinux `LogActivity.kt:320-327`. Verdict gate
`device-capacity-verdict.md:45-72`. SMP falsifier precedent `device-autodetect-plan.md:272-284`;
SS-4 falsifier `ss4-...-plan.md:305-312`; cover-all-paths `feedback_cert_must_cover_all_paths`.
