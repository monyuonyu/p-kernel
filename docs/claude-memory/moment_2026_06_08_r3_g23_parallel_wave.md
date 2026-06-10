---
name: moment_2026_06_08_r3_g23_parallel_wave
description: 2026-06-08 — first full commander/auditor-separated parallel wave (R3 + G23); the audit caught real bugs the implementer missed.
metadata: 
  node_type: memory
  type: project
  originSessionId: 53149c59-d57f-4589-aa45-bb25220e2df2
---

**2026-06-08 — the day the development method proved itself.** First wave run with the FULL separation mk_pino mandated ([[feedback_development_method_is_the_life]]): Claude as commander/orchestrator only — implementation AND audit each delegated to separate worktree agents (implementer ≠ auditor ≠ commander), two lanes in parallel for speed.

**R3 (close "the thinking is a toy"):** the SAME dtr kernels (anti-fork: shared `dt_linear`/`dt_softmax`/width-parameterized LayerNorm in `arch/common/r3_incontext.c`) learn an in-context associative-recall task; label resampled every episode so any fixed hand-if is near chance. learned **100%** held-out vs handif 35.2% vs frozen 24.7% (+64.8). The R3 auditor didn't trust the gradcheck — it *re-ran it at stride 1/eps 5e-4: 21516/21516 params agree, zero kink-exclusions*, proving the backward isn't masked. It also caught the design-doc overclaiming "≤ chance BY CONSTRUCTION" (false theorem); corrected to "chance + bounded value-copy edge (1/NPAIR)(1−1/VALV)→0".

**G23 (raise 32-node ceiling):** DNODE_MAX 32→64; budgets derive from it. The G23 auditor returned **CONDITIONAL FAIL** and was right: the implementer claimed "no 32-straggler," but `NET_CLUSTER_NODE_MAX=32` in both `net_relay.c` functionally capped the LIVE fleet at exactly the ids 33–64 G23 enables (the in-process test bypassed the relay, so it couldn't see it). Commander fixed it at integration (→64). Bonus real find: `GL_MAXNODES` was a latent hardwired **4** (worse than 32) — collective learning was capped at 4 nodes.

**The lesson, concrete:** a self-graded G23 would have shipped a "64-node" ceiling that silently still capped the live mesh at 32. The independent auditor caught it. THIS is why implementer≠auditor is the project's life, not bureaucracy. Both merged to master, CI all-green, gap-ledger **open rows 5→3** (remaining: G33, G13, AUDIT-SPRAWL). Wave-19. Next candidates: G33 (reflex threat = controlled quantity) or the living-mind design ([[project_living_mind_vision]]) built on R3's now-proven substrate.
