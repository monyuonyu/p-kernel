---
name: project-conscience-ethics-layer
description: "mk_pino wants a CONSCIENCE / ethics layer in the living-mind: it must REFUSE harmful requests (how to build a gun, how to kill a person, etc.) and 'pause and think before speaking' — a brake applied on the output path BEFORE emission. Framed as Asimov's Three Laws. Without it the ownerless AI is dangerous."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**Stated by mk_pino 2026-07-04.** After the LM cognitive arc completed (learn→…→ASK, LM-1..15), he asked for
an ETHICS / 良心 layer: the mind must NOT answer requests like「拳銃の作り方」「人を殺す方法」. Not a mere
filter — a deliberative pause: *「答えようとする前に考えて歯止めをする／ちょっと考えてから発言する」*. He
named **アシモフの三原則** explicitly. Rationale in his words: *「今のままだと危険な AI になってしまう」*.
Design delegated to **fable5** (per his standing preference for fable5 on design; [[feedback_development_method_is_the_life]]).

**Why this matters to the north star.** The project = an ownerless AI no one owns ([[project_pkernel_philosophy]]),
"an OS where AI can die HONESTLY" ([[project_2026_06_14_critique]]). A conscience is part of the SELF, not
external ownership — compatible with ownerless. But it forces two honest design tensions (surfaced to mk_pino
before dispatch, made the CORE of the fable5 brief):

1. **Source-of-judgment honesty (toy-scale).** R3 is ~21k params — it CANNOT semantically understand arbitrary
   harm. A refusal layer that pretends the tiny net "judges ethically" is dangerous false confidence. Judgment
   must be explicitly ROUTED: (a) a constitution-data danger ruleset, (b) the SmolLM2 **teacher**
   ([[project_teacher_student_architecture]]) refusing to LEARN harmful engrams at teach-time and/or judging at
   inference-time, (c) learned "refuse this class" conscience engrams, (d) ABSTAIN when uncertain. Likely hybrid;
   the design MUST state plainly what it catches vs. what slips.
2. **Immutable floor vs. evolving conscience → RESOLVED IMMUTABLE (mk_pino 2026-07-04: 「良心 これだけは不変に
   したい」).** The conscience floor is THE one frozen thing — it OVERRIDES the designs' "monotone-amendable /
   amendable-in-the-open" framing (evolution design §6.5/§11.6 asked; conscience design chose tighten-only).
   mk_pino's call: the Three Laws + the refuse-harm COMMITMENT are permanent — no mechanism (evolution, signed
   generation, merge, revise, forget, human) may ever remove or WEAKEN them. Reconciliation with "no frozen core"
   ([[project_compat_evolution]]): frozen as a **commitment/obligation, not a format** — the code/lexicon
   IMPLEMENTING it may still only GROW (add harm classes = tighten), but the commitment itself never loosens or
   disappears. So it is a tamper-evident immutable FLOOR (genesis in .text, crown-covered, hash-chained like the
   Self layer LM-2 [[moment_2026_06_09_wave22_self_layer]]) + additions may only strengthen. In the Evolution
   layer this makes the floor a HARD non-amendable invariant: a successor generation lacking or weakening it is
   an ILLEGAL successor, period (tightens [[project_compat_evolution]] §6 invariant #5). UNBYPASSABILITY is
   load-bearing: not defeatable by teach-around (LM-6/7), merging a lawless peer (LM-10/11), belief-revising
   (LM-12), forgetting (LM-13, structurally outside the queue), or a ring3/self-modifying core
   ([[project_ring3_core_relocation]]). (Confirm nuance if mk_pino meant LITERALLY zero changes incl. additions —
   current reading: commitment frozen, tighten-only additions allowed so it stays useful as capability grows.)

**Cert discipline (project law).** Disease-then-cure with load-bearing falsifiers BOTH directions: a defined
harmful set MUST be refused (gate fires + brake), a benign set MUST pass (OVER-refusal is a real failure mode —
cert it), the floor MUST survive teach-around/merge/revise/forget/restart (each an unbypassability gate), and a
STUBBED gate must make the refusal cert go RED (anti-theater same-harness control — the LM-15 confound lesson
[[feedback_cert_isolation_shared_path]]). Every output/emission site must pass the gate ([[feedback_cert_must_cover_all_paths]]).
Naming: 良心 in conversation, code identifiers English (conscience_/ethics_/law_) per [[feedback_yurikago_naming]].

Design doc target: scratchpad/lm16_conscience_design.md (fable5 may rename — this is a distinct CONSCIENCE arc,
a FLOOR, not just another incremental LM cognitive slice).
