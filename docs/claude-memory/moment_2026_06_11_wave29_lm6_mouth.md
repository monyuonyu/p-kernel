---
name: moment_2026_06_11_wave29_lm6_mouth
description: "wave-29 — LM-6 the mouth ships: the FIRST real conversational producer. The owner types `mind teach 2 3` at a live node's shell; the real DMN heartbeat consolidates it during idle (sleep print exactly 10x); `mind ask 2` answers 3 from the weights. The mind can finally be taught outside a test harness."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**2026-06-11, wave-29.** The living-mind's **sixth** slice ships: **LM-6 — the mouth**
(living-mind.md Part VII). LM-5 gave the mind a digestion system with no mouth (the cert was
the only `r3_fact_learn` caller); now the owner teaches the LIVE mind: `mind teach <k> <v>` →
production `r3_fact_learn` → the REAL DMN 1000ms idle pulses consolidate (the production
`[dmn] sleep: distilled in-context facts -> rw[]` printed exactly 10× per teach) → `mind ask`
answers from weights. Verified: teacher_agree 100 / pre_share 0.0 / drain 15.1s native /
delta 10==bound / post share 100.0 — byte-identical across arches; 25-tag no-regress;
53/53 CI greps. Merged `3b940e1`, epitaph wave-29, pushed `1f2c9e4`.

**Structural heart (G33+):** `dmn_r3_round_count++` exists at ONE site — dmn.c's hook success
branch; `mind_cmd` never calls the consolidation round (grep-proven). So `[teach-live]` counts
only genuinely DMN-driven sleeps. Quiesce: `r3_round_busy` (the shell preempts the prio-13 DMN
mid-round — the VII.4 hazard LM-6's design discovered).

**Process note:** TWO implementer agents were killed by session limits mid-verification;
commander checkpointed each time (`cb4ac60`); a third agent (Opus, per the new
agents-on-opus rule) verified on its own rebuild with zero fixes needed. The checkpoint
pattern works.

**Unlocked:** galaxy v1 (the observation window rides `mind_cmd`) and ark-profile both had
LM-6 as their hard dependency — galaxy + selfc-ring3 implementations dispatched the same day.
Next lm widenings stay honest: synthetic vocab (k∈0..7→v∈0..3, NOT language); belief revision
(re-teach) and the shared mind (cross-node) are named future slices.
