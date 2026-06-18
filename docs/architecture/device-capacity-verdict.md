# Device-capacity / native-student — design + verdict

**Status: design + adversarial critique done 2026-06-18. Decision: DEFER tier/sizing;
record the (correct) interop architecture + gate. Fold the capacity METER into the
living-body inspector as honest observability.**

Companion drafts: `device-capacity.md`, `native-student.md` (both design DRAFT 実装前).
This doc is the audited verdict + the gate that any tier/sizing work must pass.

## The one correct, verified architecture (keep this)
Two model substrates need OPPOSITE treatment:
- **R3 mind `rw[R_NP]` — STAYS FIXED fleet-wide.** It is the shared core every node
  averages into "one mind" (wave-41/42/44). Verified in code: `gl_merge`
  (gossip_learn.c:69) is a flat element-wise fold over a single length `n` with a hard
  `GL_MERGE_MAXFLOATS==21568` ceiling; `r3_weights_get/set` exchange exactly `R_NP` floats
  (r3_incontext.c:597-601); `R3_WP_HDR.r_np` (line 709) fail-closes on shape mismatch.
  Making R3 per-device variable would break the merge and is the one thing we must NOT do.
- **The Cradle baby `st_model` (arch/common/llm/student.{c,h}) — the ONLY place variability
  may live.** It is already a malloc'd arena (`student.h:62-79`: runtime `n_params`, runtime
  `o_*` offsets); only the 5 dims (`ST_DMODEL/ST_NLAYER/ST_NEXPERT/ST_TOPK/ST_DFF`) are
  `#define`. Runtime sizing = move those into the struct + discrete tiers (S/M/L) for
  mergeability (same-tier byte-identical shape → one merge-cohort).

This isolation (baby weights NEVER enter the R3 `gl_merge`) is what preserves the crown
result. It must be a CI tripwire, not an assumption.

## Why DEFER (adversarial critique — verdict: needs-work)
1. **The baby is TEST-ONLY today.** No Makefile / no boot / no persistence (`st_save`/
   `st_load` absent) / no p-fs transport / no cohort merge / no distill loop — all greenfield.
   "90% there" is false; SLICES 1-4 assume infrastructure that must be built from zero.
2. **"One mind" gets quietly relabeled.** Heterogeneous sizing = "one shared 21568-float
   core + N disjoint per-tier sub-collectives." A weak (S) node **cannot** receive a strong
   (L) node's baby-resident learning by merge — by construction.
3. **The only cross-tier bridge is distillation, which is vapor.** No distill loop in
   student.c; native-student.md marks it NS-2 design-DRAFT; its certs are admittedly
   UNVERIFIED (commit 2978c529). The whole justification for heterogeneity rests on it.
4. **Runtime dims → VLA stack-overflow risk.** ~11 stack-local arrays in student.c
   (`chosen[E]`, `tmp[D]`, `moe[D]`, `g_logit[V]`, `g_oin[D]`, `g_fin[D]`, `g_gate[E]`,
   `gw_chosen[K]`, `g_eo[D]`, `g_eh[DFF]`, `g_ain[D]`) become VLAs in a libc-free kernel —
   exactly the hosted-relay stack-overflow class the project keeps getting bitten by.
5. **Helps nothing now.** The current M baby (D=128) runs on a watch-class device; no fleet
   device can't run it, so S/L tiers are inert until the baby is much larger.
   `device-capacity.md §0` admits this ("効果は大きいモデルと共に来る").

## The gate — must hold BEFORE any tier/sizing work
- [ ] **Build the baby as a REAL substrate first**: wire it into boot+build, give it a
      persistence header AND a p-fs publish/fetch path for variable-length blobs (today
      `gl_blob` is a fixed-size R3-class struct). SLICE −1.
- [ ] **No VLAs**: bound the ~11 stack arrays to the MAX tier's dims as fixed scratch, or
      move them into the malloc'd arena. Revise the "one-TU mechanical edit" claim.
- [ ] **Gate the cross-tier claim on working distillation**: build + verify
      `[distill-loss-drops]`/`[distill-grounded]` FIRST, or downgrade the headline to
      "tiers share only the 21568-float core; strong-node learning stays in-cohort."
- [ ] **`[baby-merge-isolation]` CI tripwire**: prove baby weights can NEVER reach
      `gl_merge`/`gl_merge_w` of `rw[]` (R3 merge inputs are only `R_NP`-sized, only from
      `r3_weights_get`).
- [ ] **Sequence tiers AFTER a bigger baby exists** (plumbing for absent water otherwise).
- [ ] **Cohort-minimum / singleton fallback**: a 1-node tier is a merge-island; tie tier
      count to the measured device histogram + a fallback to the next populated cohort.
- [ ] **Thermal demotion data-loss**: an L→S demotion currently discards the saved baby
      (header-refuse guard); needs a function-preserving shrink or an explicit policy.

## What CAN ship now (honest, low-risk)
**The capacity METER (SLICE 0) — as observability, folded into the living-body inspector.**
Detect real device signals (cores via `Runtime.availableProcessors`, RAM via
`/proc/meminfo`/`ActivityManager.MemoryInfo`, an OEM-independent CPU microbench running the
real `st_forward`) and surface them at `/capability.json` + the inspector, labeled
**"担当キャパシティ (capacity), not 賢さ (smartness)"** — an honest meter, no fake progress
bar, and it changes NO model behavior (truthfully informational). This is the device's
"substrate" vital alongside the mind's organs. The probe must be fixture-injectable
(`PKERNEL_DEVICE_TIER` env) so CI without real hardware can still cert it (salty-bug lesson).

## Recommendation
Defer tiers/sizing until a bigger baby + working distillation exist. Keep R3 fixed. When the
time comes, implement against the gate above. Meanwhile, fold the honest capacity meter into
the living-body inspector wave.
