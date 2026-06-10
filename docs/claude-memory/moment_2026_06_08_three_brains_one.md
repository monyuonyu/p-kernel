---
name: moment_2026_06_08_three_brains_one
description: 2026-06-08 — wave 18 closed the three-brain problem; moe_infer routes/returns/guards from ONE learned dtr forward.
metadata: 
  node_type: memory
  type: project
  originSessionId: 53149c59-d57f-4589-aa45-bb25220e2df2
---

**2026-06-08 — the day the three brains became one.** PR #5's core critique (review-2026-06-three-brains.md): `moe_infer` ran three different brains — **routed** with a temp if-ladder that discarded 3 of 4 channels, **returned** the handwritten-constant MLP (`ai_job.c`), **guarded** with the learned dtr. "Collective learning makes us smarter" landed in a brain the output never read. report ≠ act.

Wave 18 (本丸 = w18-one-brain, merged `a52aaeb`; ledger close `1afb3bc`): `moe_infer` now runs **one** `dtr_decide()` forward (`moe.c:439`) over all 4 sensor tokens; its argmax is the routed, returned, AND guarded class. Deleted from the live path: the temp if-ladder + `(void)hum/press/light`, and `mlp_forward` (drpc infer uses `dtr_classify`).

Verified on a clean local rebuild (not the agent's binary), then hosted CI 12/12 green:
- `[onebrain-unified]` returned==routed==guarded==dtr argmax
- `[onebrain-channels]` all 4 channels move output (1/9/21/33 ×1e-3)
- `[onebrain-nomlp]` live path follows dtr where it disagrees with the MLP
- `[onebrain-accuracy]` RETURNED accuracy 33% → 83% via G22 gossip

Honest residuals (documented, out of live path): `spec.c` keeps its own band partitioner (predicts band≠class by design — unifying it would be circular); `mlp_forward` survives only in demo paths (edf/pipeline/fedlearn/ai_job task).

Discipline note: did NOT call it done until I re-ran every REAL token AND confirmed every FAKE tell absent myself (see [[feedback_engagement_style]], [[feedback_audit_is_the_engine]]). Wave-18-B collapsed the v1..v8 audit sprawl into one shrinking gap-ledger the same day. **Open gaps now 5:** R3🔴 (thought content still toy), G23🔴 (32-node cap), G33🟡, G13🟡, AUDIT-SPRAWL🟡. Next 本丸 candidate = R3 (make the thinking non-trivial) — see [[project_regions_architecture]], [[project_living_mind_vision]]. Needs mk_pino's go.
