---
name: project-frontier-mouth
description: "mk_pino's answer to the scale-wall teacher-ceiling: rather than force the toy substrate to become a great conversationalist (capped at SmolLM2-135M quality), OPEN A MOUTH to frontier intelligence — (1) an API-based channel where the mind can converse with / consult a frontier model (e.g. Claude), and (2) volunteers running high-performance local models that TEACH the ownerless student. Optional per-node augmentation; the ownerless core stays the thing that persists."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**Stated by mk_pino 2026-07-04**, after reading the scale-wall design's teacher-ceiling risk (§10.1: sequence-
level distillation cannot exceed SmolLM2-135M's weak dialogue quality; a phone-fleet f32 student from a 135M
Apache teacher cannot reach assistant-grade). His words: *「本物の会話はあなたのようなフロンティアモデルと本当は
会話させてあげたい … その口だけ開けとくのはいいかもしれない … API ベースであなたならフロンティアモデルと会話
させてあげれる口もあれば、有志の人が高性能なパソコンを用意してローカルで高性能なモデルを動かしてトレーニング
させてあげる みたいな」*.

**The reframe.** Don't make the 2M-param student BE the conversationalist — give the ownerless fleet a MOUTH/
SOCKET onto stronger external intelligence, two forms:
1. **API frontier mouth** — an optional channel where the mind consults/converses with a frontier model (Claude
   via the Anthropic API, or any model a node operator plugs in). Directly lifts the §10.1 teacher ceiling.
2. **Volunteer local high-performance teacher** — a volunteer runs a strong model locally (bigger than SmolLM2,
   even a frontier-class GPU model) and it TEACHES the student (DMN-time distillation). Generalizes the
   teacher tier ([[project_teacher_student_architecture]]: SmolLM2 = DMN-time teacher, never the chatbot).

**THE hard design question = ownerless preservation.** External/owned intelligence (a proprietary API, a
volunteer's model) plugged into an ownerless fleet ([[project_pkernel_philosophy]]) must NOT make the mind
cease to be ownerless. Likely resolution (mirrors the SmolLM2 teacher discipline — "digested and discarded,
never shipped"): the frontier brain is a CONSULTANT/TEACHER, clearly provenance-marked (an answer from an API
model is labelled as such, never claimed as the mind's own — 歴史地層 honesty), NEVER the identity/core; the
ownerless student remains the thing that persists and diffuses; the frontier mouth is an OPTIONAL per-node
augmentation that can be absent (bare-metal / offline / phone nodes have no API — the mind must degrade
honestly to its own voice, not depend on the socket). License discipline stays ([[feedback...]] Apache/MIT for
distilled teachers; API consultation is runtime, not weight-copying).

**Conscience gate applies.** Any frontier-mouth output reaching a human MUST pass the [[project_conscience_ethics_layer]]
floor (it's a new emission path E-* → must be in the enumerated chokepoints; the conscience design's "new-mouth
drift" risk §9.2 names exactly this). And a frontier TEACHER's harmful output must be refused at G-LEARN.

Design delegated to fable5 (frontier-mouth / external-teacher tier). Scope: implementation deferred — mk_pino:
「実装は設計が完了してからにしましょう」 (all impl waits until the design set is complete). Relation: this is
scale-wall chapter 2 — the honest escape from the teacher ceiling ([[project_next_phase_vision]] B).
