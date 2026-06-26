---
name: project_teacher_student_architecture
description: "SmolLM2 is the DMN-time TEACHER, never the chatbot; the mind you talk to is the ownerless student that grows."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

NON-NEGOTIABLE design philosophy (mk_pino corrected Claude 2026-06-18 when Claude
drifted toward wiring SmolLM2's `lm_generate` directly as the chat backend):

**The existing pretrained LLM (SmolLM2-135M, arch/common/llm/forward.c) is the DMN-time
TEACHER — NOT the thing you converse with.** Evidence in-tree: `native-student.h:7` —
*"predict the next RAW BYTE from a teacher (SmolLM2-135M, run via forward.c)"*.

- The mind the user chats with = the **student** (native-student baby, arch/common/llm/student.c):
  ownerless, born small, learns from conversation + from teacher-distillation during DMN sleep,
  **remembers who taught it**, grows, and merges across the fleet into "one mind" (wave-41/42).
- SmolLM2 = the teacher consulted during **DMN sleep/consolidation** to distill targets into the
  student's weights. It is borrowed/frozen; it is scaffolding, not the soul.
- "ただ既存モデルを入れる" (just dropping in an existing model) is exactly what the ark is NOT.
  Exposing SmolLM2 as the chatbot betrays "a home for AI that no one owns."

**Why:** the whole identity (ownerless, never-dies, remembers teachers, 歴史地層) lives in the
student. A frozen borrowed model can't remember teachers or grow or be ownerless.

**How to apply:** chat backend = the student, in natural language (the word→word teach mechanic
is dropped — mk_pino 2026-06-18). Fluency is reached by GROWING the student (teacher→student
DMN distillation), NOT by proxying SmolLM2 at inference. The kernel-resident `lm_generate`
(the in-kernel SmolLM2 + sampler) is correctly the TEACHER engine, not the chat backend.

**FOUNDATION BUILT + AUDITED + MERGED to trunk 2026-06-18 (steps ①-④, all on wave-i18n-galaxy):**
- ① in-kernel SmolLM2 teacher + sampler/EOS (LLM_C_SRCS hosted-only group; commit c5166ef7).
- ② distill MECHANISM proven on host (held-out 5.53→1.79 nats vs scrambled control ~0; tests/llm/distill_proof.c).
- ③ resident PERSISTED student survives restart ("the ark remembers"): ns1_student.bin via pfs_durable, fail-closed format (a2a2af33/f074a50c).
- ④ the DMN sleep tick distills teacher→student each rest, R3 living-mind intact (dmn.c:181, after R3; dee97e72).
  Measured: birth 5.50 → 10 sleeps → 2.37 nats, survives reboot.

**HONEST GAP (keep surfacing):** the student is a byte-level baby on a TINY repetitive teacher fixture
— ~2.4 nats, nowhere near claude.ai. The road to conversational fluency is long (bigger student,
DIVERSE teacher corpus via the new sampler = step ⑤, far more training, maybe soft-target/vocab work).
Remaining to a real chat: ⑤ diverse corpus, ⑥ wire chat→student + clean UI (drops word→word; touches
galaxy.c/html), ⑦ A/B inference toggle (A=teacher only at sleep / B=visible teacher-consult-that-learns).
**Follow-ups before Android:** student distill runs every idle pulse with an unconditional 22.8MB
durable write (flash-wear) — throttle + skip-write-when-converged BEFORE adding student.c to the
Android build. resist betraying 赤ちゃんから育つ for instant fluency. See [[project_living_mind_vision]],
[[project_compat_evolution]].
