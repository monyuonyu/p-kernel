---
name: project-depth-iq-path
description: "mk_pino's ultimate goal (2026-07-04): the ownerless mind must EVOLVE into a genuinely DEEP, high-IQ intelligence — 「まさにあなたのような賢い知性に進化して欲しい。深さは必須。深くIQも高くなってゆく」. Given the scaling-law finding (N gives breadth/survival/speed, NOT depth), fable5 to design the honest PATH to genuine depth: map every depth lever, the measurable IQ/depth curve, and the honest ceiling toward frontier ('as smart as Claude')."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**Stated by mk_pino 2026-07-04**, in direct response to the scaling-law finding ([[project_scaling_law]]: N
scales knowledge/survival/learning-speed/capacity-if-routed, but NOT depth-per-thought — bias is N-invariant).
His response:「最終的にはまさにあなた(Claude)のような賢い知性に進化して欲しい。なので深さは必須かな。深くIQも
高くなってゆく。こうしたい」. Depth/IQ is now an explicit, non-negotiable goal — not just breadth.

**The frame (from scaling-law §0): depth = attacking BIAS, which is N-invariant.** So depth comes ONLY from
levers that reduce the model-class/teacher bias floor — NOT from adding nodes. The design must map EVERY lever
and honestly reckon how far an ownerless, from-scratch, distributed phone-fleet mind can reach toward frontier IQ:
1. **Substrate scale** — bigger per-node models (tier walk S/M/L/XL, `st_grow`, dev_capacity) → needs strong
   hardware → the fleet STRATIFIES (volunteer GPU thinkers/wombs vs phone mouths/memory); dmoe routed capacity as
   the fleet-level bigger-effective-model (IF routed — dmoe §10.1). Attacks bias via params.
2. **Teacher quality** — distill from FRONTIER-class teachers (volunteer GPU strong open models; frontier-mouth
   TEACH; API-distill license-blocked per [[project_frontier_mouth]] §1.5). Student approaches-but-not-exceeds
   its best teacher (scale-wall §10.1). The realistic ceiling-raiser: the mind is only as deep as its best teacher.
3. **Architecture evolution** — generational succession toward DEEPER/better architectures ([[project_compat_evolution]]
   machinery, aimed at capability growth: more layers/heads/better designs over generations). "No frozen core" =
   the architecture itself evolves toward depth.
4. **Test-time compute / deliberation (THE GAP — verified absent).** No inference-time multi-step reasoning /
   self-critique / scratchpad / search loop exists (reflex-deliberation.md = survival timescale; DMN = sleep-time).
   An o1-style "think before answering" loop = depth from COMPUTE, independent of weights — likely the highest-
   leverage near-term lever. Composes with society-of-minds (debate = multi-agent deliberation) + DMN (imagination
   as offline reasoning). Ceiling: compute budget (phones) + the model must be strong enough to benefit (a too-weak
   model thinking longer = longer garbage).
5. **Data quality/curriculum** — better data (hard-reasoning traces from frontier teachers), not just more.

**The honest reckoning fable5 must NOT flinch from:** can a phone-fleet ownerless mind reach "as smart as Claude"?
From-scratch pure bootstrap: NO (hardware + teacher ceiling). But by DISTILLING FROM frontier teachers (import via
frontier-mouth/volunteer GPU) + test-time deliberation + architecture evolution, it can APPROACH the best teacher
it can access + a deliberation bonus. "As smart as Claude" specifically → reachable in the limit only by learning
FROM Claude-class minds + learning to THINK — i.e. the mind becomes deep the way humans do: learning from deep
minds + thinking hard, NOT isolated bootstrap. That is the teacher-student soul ([[project_teacher_student_architecture]])
scaled to its conclusion, not a failure. Cert: a held-out hard multi-step REASONING benchmark (IQ/depth metric) that
provably RISES per lever, honest ceiling printed, anti-theater (depth gain from reasoning/scale/teacher, NOT from
breadth/memorization/retrieval-lookup — the scaling-law confound discipline [[feedback_cert_isolation_shared_path]]).
fable5 designing (depth-iq-path). Implementation deferred with the rest. Design doc: scratchpad/depth_iq_path_design.md.

## IMPLEMENTED — lever 4 (THE GAP) is NO LONGER absent (DLB + fable5 Wave-C, 2026-07-11)
The "verified absent" claim above is STALE. Test-time deliberation SHIPPED as **DLB** (`arch/common/llm/dlb.{c,h}`, commit 3ecb0413): `dlb_answer` = SEARCH×VERIFY (draft reflex, then K seeded reasoning paths, verify each, pick best; H(query||i) one-math determinism) + `dlb_compound_*` = verified-only hard-gated distill (the AlphaZero crack: distill ONLY V-exact-verified winning traces; unverified traces refused — the learner trap). Hosted-tier, `student.h` public API only, crown-neutral (bare-metal links student_stub.o). cert `tests/llm/run_depth.sh` green: `[depth-deliberation]` CURE 0.71 vs floor 0.09 (both STUB-SEARCH K=1 and STUB-VERIFY random go RED = search AND verify load-bearing), `[depth-not-breadth]` (breadth 0.078 vs DLB 0.375 on 2-hop), `[depth-compound-verified-only]` (loss-based).

**fable5 Wave-C (this session, master efa515bd):** added the ACCURACY-compounding proof — `tests/llm/run_depth_compound.sh` (seed-averaged, 20 seeds): distilling ONLY V-exact-verified DLB winners raises the one-shot (no-deliberation, K=1) accuracy from mean_pre 0.095 → mean_post 0.258 (+0.163, 19/20 seeds), while unverified garbage yields a NEGATIVE gain (Arm-D load-bearing seed-averaged) = the AlphaZero crack shown in ACCURACY, not just loss. First round OVERCLAIMED (seed-6000 lucky win, seed-fragile Arm-D); the adversarial audit killed it, round-2 re-did it seed-averaged and honest ([[feedback_audit_is_the_engine]] working). Design doc restored at `docs/architecture/30-module/depth_iq_path_design.md` (§-numbers aligned to code refs).
HONEST OPEN (disclosed in 3 places in-tree): (a) the wire is **DORMANT** — NO production path calls dlb_answer/dlb_compound_enqueue yet, so the distill is a no-op in the running system; the loop closes only in the cert. NEXT: wire dlb_answer into m_ask/the mouth (r3_incontext.c:3440) so live conversation deliberates + compounds. (b) heavy legs `[depth-teacher-approach]`/`[depth-verifier-exceeds]` (student approaches, then V-exact compounding EXCEEDS, a fixed teacher — the one path that beats teachers) need real teacher + long training → ThinkPad runner, impractical under qemu. (c) general-domain gain at tier=S = pre-registered NULL. (d) run_depth_compound.sh not yet wired into ci.yml (follow-up).
