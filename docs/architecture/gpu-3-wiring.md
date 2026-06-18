# GPU-3 — wiring the Vulkan matmul into the mind (design + verdict)

**Status: design + adversarial critique done 2026-06-18. Decision: DEFER implementation.**
This document is the recorded analysis (年輪) and the gate that any future GPU-3
implementation must pass. No code is wired yet — and that is the correct state.

## What exists today (honest baseline)
- A **real, complete** Vulkan f32 matmul backend ships in the APK:
  `android/app/src/main/cpp/gpu/gpu_vk.c` (dlopen libvulkan, device/queue, SPIR-V
  `matmul_f32` shader, pipeline, dispatch+readback; `gpu_available()` gates on `G.enabled`).
- **Nothing in the mind calls it.** `moe.c` / `dtr.c` / `r3_incontext.c` have zero GPU
  references. All inference and training run on the CPU.
- The product is **honest about this**: the engineer page reads
  `… available · enabled · not yet used for inference (CPU)` and the toggle handler
  notes *"GPU-3 wires the matmul. We do NOT touch the inference path."*

## The proposed design (sound in approach)
- **Single choke point.** Every forward/backward matmul funnels through
  `dt_linear(W,b,x,y,M,N)` at `arch/common/dtr.c:149` — byte-for-byte the shader
  contract — so one gated branch inside `dt_linear` can route to `gpu_matmul_resident`.
- **CPU-authoritative determinism.** The CPU `dt_linear` (`-ffp-contract=off`) stays
  THE canonical math. The GPU is a tolerance-bounded accelerator allowed on **exactly one
  path** (the pure inference forward) and **forbidden** on anything that writes `rw[]`/`W_*`
  (all training / DMN distillation), because the GPU's reassociated tree-reduction differs
  from the CPU left-fold — the exact rounding-order divergence class the **salty bug
  (wave-49)** proved breaks on-device training convergence.
- **Two certs, two contracts.** The bit-exact fpdet golden-logit cert
  (`0xe2391516`, cross-arch) stays authoritative and is computed **GPU-off**. The GPU
  forward gets a *separate, weaker, falsifiable* cert: **argmax-stable + |Δlogit| within
  `allclose` tolerance** (reusing `gpu_test.c`'s rule). Bit-equality is required of the CPU
  path; behavioral stability of the GPU path.
- **Size gate.** GPU fires only above `DT_GPU_MIN_MACS` (default ~262144).

## Why DEFER (the adversarial critique — verdict: needs-work)
1. **The safety boundary is NOT structural.** `train_forward` (dtr.c:747) and `r_forward`
   (r3_incontext.c:191) are the **same functions** on the read AND gradient paths. "GPU is
   inference-only" rests entirely on a hand-maintained, **fail-OPEN** runtime flag
   (`dt_in_training`, which does not exist yet): forget to set it at any present/future
   training entry point and a gradient forward silently goes to the GPU → salty-bug class
   re-opened. The project's constitution prefers structural guards over discipline.
2. **Zero speedup today, provably.** Largest matmul in the shipped core is R3 48×48 =
   2304 MACs; the gate is 262144 → the GPU branch **never fires**. `gpu_test.c` records the
   CPU beating the GPU even at 4096² on the S25 Adreno for this shader. Real payoff needs the
   quantized MoE (Q8_0/Q4_0) — which has **no GPU kernel yet**.
3. **Cross-node merge hazard.** `mw_fold_region` / `gl_merge` / Fisher / Path-W² average
   `rw[]` across nodes. A GPU-produced state entering the merge makes per-node GPU/CPU
   divergence **accumulate** across rounds — violating "one mind, one math" by accumulation,
   not by a single forward.

## The gate — must hold BEFORE any GPU-3 implementation
- [ ] Make the GPU branch **opt-IN from the read entry points** (`dtr_classify`/`moe_infer`/
      `r_recall`/`r_answer`), not opt-OUT — i.e. **fail-CLOSED to CPU**. No runtime
      `dt_in_training` flag as the safety mechanism.
- [ ] Drive `dt_gpu_invalidate_all()` from the ONE weight-write choke point
      `r3_weights_set` (r3_incontext.c:601) + `dtr_weights_set`, not an enumerated list
      (merge/DMN/Fisher all write `rw[]`). Assert no `rw[]`/`W_*` write bypasses `*_weights_set`.
- [ ] **Fence GPU state out of cross-node merge**: declare the GPU forward a same-device-only
      optimization; the merge path must assert it reads a CPU-authoritative `rw[]`.
- [ ] Harden the tolerance cert for **R3's 64-class readout** (tighter decision boundaries
      than dtr's 3 classes) and **poison-test a forced NaN/Inf** (allclose's `denom=fabs(ref)`
      can let a NaN slip through).
- [ ] Add a concurrency mutex around `dispatch_matmul` (shared `G.queue`/`G.cmd_pool`) before
      any concurrent ring3 minds ship.
- [ ] Frame it as **determinism-boundary infrastructure**, never as "faster" — until the
      quantized MoE lands, "GPU enabled" must not read to users as a speedup it does not deliver.

## Recommendation
Keep today's honest status. Revisit GPU-3 when the heavy quantized MoE exists (that is the
first matmul large enough to cross the GPU threshold). At that point, implement against the
gate above — opt-in/fail-closed, invalidation at the `*_weights_set` choke point, GPU fenced
out of merge, R3+NaN-hardened cert.
