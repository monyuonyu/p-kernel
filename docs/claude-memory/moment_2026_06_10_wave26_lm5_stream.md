---
name: moment_2026_06_10_wave26_lm5_stream
description: "wave-26 — LM-5 随時 ships: a STREAM of in-context facts consolidated across MANY sleeps on the REAL dmn idle hook, without destroying earlier facts. Also: the DMN was x86-only dead code — this wave wired sleep onto the fleet's linux nodes for the first time."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**2026-06-10, wave-26.** The living-mind's **fifth** slice ships: **LM-5 — 随時, the living
consolidation loop** (living-mind.md Part VI). Builds directly on LM-4's one-fact-one-sleep
handoff ([[moment_2026_06_09_wave24_lm4_handoff]]) toward the north star's "learns continuously
from conversation" ([[project_living_mind_vision]]).

**Proven:** F=4 disjoint fact-sets arriving over time (frozen FAST layer reads each at arrival →
fact-engrams, bounded FIFO queue) are consolidated by bounded sleep rounds into `rw[]` via
`r_backward`. Multi-fact interference is a REAL disease (naive sequential consolidation: fact-1
100→35.0, drop 65.0) and the cure is LM-1's interleave discipline transplanted to R3 (replay end
100.0 on ALL facts, cure +65.0, newest 100.0). Grounded (scrambled-arrival → 0.0×4). The
livehook gate is structural: the cert consolidates ONLY via `r3_consolidate_idle_round()` — the
exact symbol `dmn_idle_work` calls (G33: the test drives the production formula; the naive
control is just the `with_replay=0` knob).

**The fleet discovery (design-wave finding, verified):** `dmn_init`/`dmn_task` were created ONLY
in `arch/x86/usermain.c` — **the DMN (sleep) was dead code on every hosted linux node** since
LM-1. This wave wired it into BOTH linux usermains (prio 13, 8KiB, mirroring x86); live idle
pulses now genuinely consolidate (observed `[dmn] sleep: distilled in-context facts -> rw[]`
end-to-end), and the prio-13 task does NOT perturb the deterministic certs (byte-identical).

**Audit notes worth remembering:**
- Two self-flagged implementer deviations were judged HONEST with reasoning: (1) the SDICT value
  table was re-derived because naive DSTAR reuse violated the GATED ≤33 precondition (fact 4
  pre-known at 44.5); same table reaches 100 in the cure → the collapse is interference, not
  impossibility. (2) 5-read-majority arrival engrams ≤ LM-4's effective teacher; scrambled still 0.
- The evicted-fact "decay" print (100→53.5) is really an ACTIVE re-bind by a 5th fact reusing
  keys (8-key vocab forces reuse) — printed not gated, disclosed.
- No conversational producer exists yet: the cert is the only `r3_fact_learn` caller, so the live
  `[dmn] sleep` print can't fire outside tests today. The next lm slice likely = a real producer
  (conversation → facts) or belief revision (contradicting facts — VI.2 named it a future slice).

Merged `8f3d9f9`, epitaph `d4d78d2` (wave-26). 49/49 CI greps; cross-arch byte-identical.
Method held: separate design (Part VI agent) + implement + audit, commander read the four gate
`if`s directly. Same day as wave-25 ([[moment_2026_06_10_wave25_ring3_survival]]) — two lanes
shipped in parallel.
