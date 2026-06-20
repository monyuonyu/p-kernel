# Thread T — teacher→student teaching over the mesh: implementation plan (cert-first)

> **Status:** DESIGN PLAN by an automated design-harden workflow on trunk `799a293b`, awaiting commander review + a separate impl→audit cycle.
> Implementer ≠ auditor ≠ commander. This doc is the contract; it must not be self-approved.

---

## 0. Which design won, and why

Three angles were designed and independently critiqued: **text-level** (byte-CE lesson packs), **logit-align** (soft-target KL with a BPE→byte projection), and **lesson-pack** (p-fs body + KDDS beacon, byte-CE).

| angle | reaches_student | cert_falsifiable | nocentral | builds_on_reality | score | killer objection |
|---|---|---|---|---|---|---|
| **text-level** | ✅ | ❌ | ✅ | ✅ | **6.5** | held-out split is positional; cert can fail with teaching **ON** |
| logit-align | ✅ | ❌ | ✅ | ✅ | 5.5 | soft channel is redundant given co-shipped bytes; cert can't isolate it |
| lesson-pack | ✅ | ❌ | ✅ | ✅ | 5.5 | imports an R3 masked-fact cert the **byte** student has no analog for |

**Winner: the text-level design, with the lesson-pack design's transport grafted in.**

All three pass `reaches_student ∧ nocentral ∧ builds_on_reality`. None passed `cert_falsifiable` as written — but the text-level design has the **highest score (6.5)** and the **weakest, most surgically-fixable killer objection**: its cert is broken only in *how it defines the held-out probe*, not in its mechanism, its transport, or its honesty. The fix is local (separate a trained lesson copy from a never-trained probe occurrence) and is spelled out by its own salvage note. Plainly: **text-level wins on simplicity + reality.** Its distill path is byte-identical to what `distill_proof.c` already certifies weight-resident (held-out 5.53→1.79 nats real vs 5.53→5.48 scrambled), it adds zero new math, and a lesson-less node trains exactly as today.

The other two each propose a *second* mechanism (a `q`-target KL loss; an R3 masked-fact recall) whose marginal value their own certs admit they cannot isolate. Per memory's audit doctrine, a mechanism whose cert can't falsify it is not allowed to ship claiming it works. So we ship the **byte form first** (the cheapest thing that is provably load-bearing) and reserve soft targets behind a wire-compatible `fmt` byte for a later wave.

### What we graft from the runners-up

- **From lesson-pack (verbatim transport):** the **KDDS `CRADLE_TEACH` beacon + p-fs `pfs_dag_save`/`pfs_dag_read` lesson body** carried over `gl_pfs_publish`/`gl_pfs_fetch` (gossip_learn.c). This is a more concrete, already-proven carrier than text-level's hand-waved "KDDS beacon + p-fs WANTs," and it pins the `vocab_fp` poison guard (`MT_WIRE_VER_VOCAB`, dtr.h) so a foreign byte-keying teacher can't corrupt a child.
- **From lesson-pack & logit-align (identical, and correct):** the **N-2b teacher-selection adaptation** — bit 1 of the existing `SWIM_GOSSIP_EVT.capability`. All three designs converged on this independently; it is the strong, unanimous part.
- **From logit-align (deferred but reserved):** the `LESSON_FMT_SOFT` header value and the `(p − q)·invN` generalization of `st_backward` — **named, not built.** Its own critique shows the four-arm cert can't yet prove soft adds anything over the bytes; it ships only when a paired `[ct-soft-vs-byte]` cert (q is the *only* supervision, bytes withheld) passes. **DEFERRED, see §6.**

---

## 1. THE CERT (lead, falsifiable, production self-test) — `[cradle-teach]`

This is the spine. It is written before the mechanism because the mechanism exists only to make this cert pass — and pass *for the right reason*.

### 1.1 The killer-objection fix (why the naïve cert is wrong)

The text-level design's original cert said: *append the probe fact into the lesson ring, assert pre-loss == chance, run `student_dmn_consolidate()`, assert held-out loss on the probe drops.* **This is structurally broken**, confirmed against the real tree at `799a293b`:

- `window()` reads one contiguous buffer: `dst[i] = corpus[(off+i) % n]` (`student_shell.c:187-188`).
- The train/held split is **purely positional**: `trainw = total*3/4`, `train_end = trainw*ST_DMN_SEQLEN` (`student_shell.c:441-444`).
- `sleep_rounds` only iterates `w ∈ [0, trainw)` (`student_shell.c:213`).

So a probe appended to a `≤4096 B` ring lands either (a) in the trained 3/4 — then the loss drop is rote memorization of bytes the optimizer *directly saw*, exactly what `distill_proof.c`'s scrambled control rules out as "not generalization"; or (b) in the held-out tail 1/4 — then the optimizer **never touches** those positions, the gradient is identically zero, and the probe loss **cannot drop** even with teaching fully ON. **As literally specified the cert can fail with teaching ON**, inverting the falsification logic. It must not ship.

### 1.2 The corrected cert (what we actually build)

`cradle_teach_self_test()` — a **production self-test** exercising the **real** `student_dmn_consolidate()` corpus seam (NOT a private copy), modeled on `tests/llm/distill_proof.c` and registered in the hosted self-test list alongside `swim_cap_gossip_self_test`. Two in-process nodes: **A** (teacher-capable, has the GGUF/fixture-derived generator) and **B** (student-only, fresh `g_student` at `STUDENT_SEED 0x0BABE`).

The cert **separates the trained lesson copy from a never-trained held-out probe occurrence**:

1. **Lesson construction.** A composes a lesson body where a coined fact (e.g. byte string `"zorblax is a blue fox"`, absent from `TEACHER_FIXTURE`) appears in the **train region** — the first `trainw` windows that `sleep_rounds` actually iterates. A **distinct held-out occurrence** of the same fact — a *paraphrase or a continuation* that the optimizer **never trains on** — is placed in the **tail held region** (`≥ train_end`). The cert computes `train_end = trainw*ST_DMN_SEQLEN` from the **live ring length**, exactly as `student_dmn_consolidate` does, so the two regions are defined by the production split, not a test-private one.
2. **BEFORE.** B's held-out loss on the probe occurrence `== chance ≈ ln(256) = 5.545 nats` (assert). B's next-byte completion of the probe prefix is wrong (assert argmax ≠ fact byte).
3. **TEACH.** A `pfs_dag_save`s the body, beacons `seq=1` on `cradle/teach`; B pulls it via the **real** beacon + p-fs path into `g_lesson_ring`, then runs the **production** `student_dmn_consolidate()` for N ticks (via `dmn distill N`).
4. **AFTER.** B's held-out loss on the **never-trained** probe occurrence drops measurably (`assert post < pre − 0.5 nats`) AND B's next-byte completion of the probe prefix now matches the taught continuation above chance. Because the probe occurrence was never in `[0, trainw)`, a drop there is **generalization**, not rote copy — the same standard wave-24/wave-26 held for the R3 handoff, now made well-defined for the byte student.

### 1.3 Falsification arms (all mandatory; cert PASSES only if every arm holds)

- **Arm A — teaching OFF (mesh discriminator).** Gate the beacon-pull behind `g_cradle_enabled`. With it off, B's ring stays empty, B falls back to `TEACHER_FIXTURE`, never sees the fact → probe loss **stays at chance** (`post ≈ pre`). Proves the signal **rode the mesh**, not a local fixture. *Cert FAILS if a teaching-disabled B still learns the fact.*
- **Arm B — scrambled bytes (sequence discriminator).** A publishes a body where the lesson bytes are **randomized** (LCG, mirroring `distill_proof.c:60` `rng=rng*1664525u+1013904223u`), same length / same update count → probe loss **stays at chance**. Proves the fact lives in the **byte sequence**, not in byte statistics. *NOTE: use random-byte scramble (as distill_proof does), **not** a same-byte shuffle of the lesson — a shuffle leaves answer-byte frequencies intact and would fail to falsify statistics-vs-sequence (lesson-pack critique).*
- **Arm C — survives teacher death (weight-residency).** After the AFTER assertion, kill A; B re-runs the probe from its persisted weights → **still answers**. Proves the gain is weight-resident (`st_adam_step` mutated `m->w`, persisted at `student_shell.c:466-479`), not in-context. B has no context window holding the lesson at answer time; the ring is consumed during sleep.

The cert FAILS loudly if a teaching-disabled / scrambled / no-fetch B learns the fact (signal leaked elsewhere) **or** if a teaching-enabled B does not. This is the gate the implementer must make pass and the auditor must independently re-run with each arm sabotaged.

### 1.4 Honest bound carried INTO the cert

The cert certifies exactly: *"a teacher's mesh-delivered byte sequence made a held-out fact weight-resident in the byte student; neither teaching-off nor scrambled-bytes reproduces it; it survives the teacher's death."* It does **NOT** certify closed-book novel-fact recall in the R3 sense, nor any soft-target / logit fidelity. We do not import the R3 masked key/value cert (`r3_incontext.c`) — that lives on a *different* substrate (`rw[]`, `R_VALV` vocab, key/val tables) and has no analog in `student.c` (`ST_VOCAB=256`, `m->w`, no key/value structure). Claiming it for the byte student would be a lie the substrate can't back (lesson-pack killer objection).

---

## 2. Minimal spec

### 2.1 The lesson wire format + mesh transport (grafted from lesson-pack)

**Beacon — `CRADLE_TEACH_PKT`** on KDDS singleton topic `cradle/teach` (`LATEST_ONLY`, region-scoped, same slot class as `MIND_TEACH_TOPIC`):

```
{ UW  magic;
  UB  teacher_node;            /* origin id (== region_teacher() on a healthy mesh) */
  UW  seq;                     /* per-teacher monotonic high-water           */
  UB  body_ref[PFS_ID_LEN];    /* content-id of the lesson body in p-fs       */
  UH  body_len;
  UB  fmt;                     /* LESSON_FMT_BYTE now; LESSON_FMT_SOFT reserved */
  UW  vocab_fp; }              /* byte-keying fingerprint; refuse-on-mismatch  */
```
≈192 B beacon. Declared in `dtr.h` beside `MIND_TEACH_TOPIC`; noted in the `kdds.h` singleton-topic budget.

**Body** — `≤4096 B`, content-addressed via `pfs_dag_save` under ref `cradle/lesson/<teacher>/<seq>`, carried by the existing `gl_pfs_publish`/`gl_pfs_fetch` pattern. **Length-prefixed binary, NOT a C-string** — `tok_decode` emits arbitrary UTF-8 including NUL/control bytes; `TEACHER_FIXTURE` was hand-cleaned to ASCII but live lessons cannot be. The ring and `window()` are already `uint8_t`-clean, so this is wire-format discipline, not a math risk.

**Child pull (NO-OP-safe):** at the **top** of `student_dmn_consolidate()`, before windowing, `cradle_poll_and_pull()` does `kdds_sub(timeout=0)` on `cradle/teach`; if `seq > g_lesson_hw[teacher]` and `vocab_fp` matches, `pfs_dag_read` the body into `g_lesson_ring`, bump the high-water. **Gated by `g_cradle_enabled`** and **must never block the sleep**: no relay / no p-fs → fall back to `TEACHER_FIXTURE` (preserving the strict no-op contract at `student_shell.c:430-438`).

### 2.2 The distill integration on B's DMN sleep tick (text-level, zero-regress)

The **only** change to the math path is the **corpus SOURCE**. The genuinely zero-regress seam (text-level salvage):

- Add file-static state next to `TEACHER_FIXTURE`: `g_lesson_ring[CT_RING_BYTES]`, `g_lesson_len`, `g_lesson_hw[DNODE_MAX]` (`student_shell.c:84`).
- `window()` reads from `g_lesson_ring` when `g_lesson_len >= 4*ST_DMN_SEQLEN`, else falls back to `TEACHER_FIXTURE` (`student_shell.c:185-189`). **A node with no lessons trains byte-identically to today.**
- `sleep_rounds`, `st_zero_grad`/`st_forward`/`st_backward`/`st_adam_step` — **UNCHANGED** (`student_shell.c:206-222`). They fold whatever `window()` yields.
- The held-out throttle math (`trainw`, `train_end`, `heldout_loss`) recomputes from `g_lesson_len` when the ring is live — and the cert in §1 relies on exactly this `train_end` boundary.

This is the proven `sleep_rounds → st_backward → st_adam_step` loop that `distill_proof.c` certifies weight-resident; the lesson bytes enter it unchanged. The DMN sleep tick (`dmn.c:265`) is the **only** distill driver; the fetch seam is *inside* `student_dmn_consolidate()`, so `dmn.c` itself is untouched.

### 2.3 NOCENTRAL teacher selection (unanimous N-2b reuse) — wire-compat choice pinned

**WIRE-COMPAT CHOICE = Option A: bit 1 of the existing `capability` byte.** Explicitly **NOT** Option C (new field → packet-size bump → `SWIM_VERSION` bump → old nodes drop packets, mesh interop breaks).

- `SWIM_GOSSIP_EVT` stays **4 B**, `SWIM_PKT` stays **24 B** — the static asserts at `swim.h:93-96` (`_Static_assert(sizeof(SWIM_GOSSIP_EVT)==4 ...)`, confirmed live) stay true. `SWIM_VERSION` stays 1. Old nodes emit `capability=0` → read as `(supernode=0, teacher=0)` → safe degrade.
- **Self-authoritative origination:** add `teacher_self()` mirroring `cap_self()` (`swim.c:113-117`), true iff this node holds a successfully `lm_load`'ed teacher GGUF — a verifiable runtime property, **not an env decree** (a lying node could otherwise self-elect; opt-in `PKERNEL_TEACHER=1` is *gated by GGUF presence*, never sufficient alone).
- **Bit-pack at both origination sites:** self-beacon (`swim.c:525`) and self-refutation (`swim.c:197`) emit `(cap_self()<<0) | (teacher_self()<<1)`.
- **Apply under the SAME LWW gate:** extract `teacher_in = (capability>>1)&1` at `swim.c:181`; call `region_set_teacher_capable(nid, ...)` in lock-step with `region_set_super_capable` at transitive-discovery (`swim.c:234`), the anti-stale `(incarnation,state)` gate (`swim.c:256`), and the rx-marks-alive path. Converges epidemically, no registrar, no vote.
- **Deterministic selector:** add `teacher_capable[DNODE_MAX]` table + `teacher_select()` / `region_teacher()` in `region.c`, mirroring `supernode_select()` / `region_supernode()` (`region.c:132-148`, confirmed live) = lowest-id node that is BOTH `member[]` AND `teacher_capable[]`, `0xFF` if none (degrade: no teacher this region → child keeps its ring / falls back to fixture). Every child recomputes the same id locally → no vote. **Survives teacher death by recomputation** (kill teacher → `member[]=0` → next teacher-capable id wins → beacon continues), identical to supernode handoff.
- **Selection cert** `swim_teacher_gossip_self_test()` mirrors `swim_cap_gossip_self_test` (`swim.c:829-982`): `[teacher-converge]` / `[teacher-selector]` / `[teacher-determinism]` / `[teacher-staleness]` (a stale lower-incarnation rumor cannot flip teacher-capability).

---

## 3. EXACT file:line touchpoints

**Teacher selection (T-fix):**
- `arch/common/include/swim.h:59-74` — document `capability` bit 1 = teacher-capable (no layout change; asserts at `:93-96` stay).
- `arch/common/swim.c:113-117` — add `teacher_self()` mirroring `cap_self()`.
- `arch/common/swim.c:197, 525` — bit-pack `(cap_self()<<0)|(teacher_self()<<1)` in self-refutation + self-beacon.
- `arch/common/swim.c:181, 234, 256` — extract `teacher_in` and `region_set_teacher_capable()` under the existing LWW gate (and the rx-marks-alive `gossip_add` site).
- `arch/common/region.c:50` — add file-static `UB teacher_capable[DNODE_MAX]`.
- `arch/common/region.c:132-148` — add `teacher_select()` + `region_teacher()` mirroring `supernode_select()`/`region_supernode()`.
- `arch/common/include/region.h:62-67` — declare `region_teacher()`, `region_set_teacher_capable()`, `region_is_teacher_capable()`.
- `arch/common/swim.c:829-982` (mirror) — add `swim_teacher_gossip_self_test()` (converge / selector / determinism / staleness).

**Lesson transport + distill (T-1):**
- `arch/common/include/dtr.h` (near `MIND_TEACH_TOPIC`) — add `CRADLE_TEACH_TOPIC "cradle/teach"` + `CRADLE_TEACH_PKT` + `LESSON_FMT_BYTE` / reserve `LESSON_FMT_SOFT`; gate on `vocab_fp` mirroring `MT_WIRE_VER_VOCAB`.
- `arch/common/include/kdds.h:37` — note `cradle/teach` in the singleton-topic budget.
- `arch/common/llm/student_shell.c:84-141` — keep `TEACHER_FIXTURE[]` as the fallback corpus (no removal; zero-regress).
- `arch/common/llm/student_shell.c:84` (new statics) — `g_lesson_ring[CT_RING_BYTES]`, `g_lesson_len`, `g_lesson_hw[DNODE_MAX]`, `g_cradle_enabled`.
- `arch/common/llm/student_shell.c:185-189` — `window()` reads `g_lesson_ring` when `g_lesson_len >= 4*ST_DMN_SEQLEN`, else `TEACHER_FIXTURE`.
- `arch/common/llm/student_shell.c:440` — at top of `student_dmn_consolidate()`: `cradle_poll_and_pull()` (NO-OP-safe, `g_cradle_enabled`-gated) before the existing windowing; `trainw`/`train_end` already recompute from the live corpus length.
- `arch/common/llm/student_shell.c:206-222` — `sleep_rounds` UNCHANGED.
- `arch/common/gossip_learn.c:139-159` — reuse `gl_pfs_publish`/`gl_pfs_fetch` (`pfs_dag_save`/`pfs_dag_read`) carrier for the lesson body.
- `arch/common/dmn.c:265` — UNCHANGED; the teacher node's tick also emits a lesson via a new `cradle_teach_tick()` guarded by `region_teacher()==drpc_my_node` (deterministic seed pinned, see §5).
- `arch/common/llm/student_shell.c` (new symbols) — `cradle_lesson_ingest(const uint8_t*, int)` + `cradle_teach_self_test()`.
- `arch/common/llm/student_stub.c:42` — extend the weak stub so a student-less build links `cradle_teach_self_test` as a no-op.
- `tests/llm/` (new) `cradle_teach_proof` harness — the `[cradle-teach]` cert of §1, modeled on `tests/llm/distill_proof.c` (note: that file's `sleep_rounds` already takes a `scramble` arg using the LCG above — reuse that scramble, not a shuffle).

---

## 4. Honest about the BPE↔byte bridge (what the teacher CAN'T transfer)

The teacher's path is **BPE tokens → `tok_decode` → raw UTF-8 bytes** (the `lm_generate_sampled` → `tok_decode` step that *already* produced `TEACHER_FIXTURE` via `student_harvest_diverse.c`). The student is byte-native (`ST_VOCAB=256`), so a byte stream **is** the training signal — no token alignment, no logits on the wire.

What the bridge **CANNOT** transfer, stated plainly (per conversational-teaching.md §3.3):

1. **The teacher's logit/soft-target distribution.** Byte-CE fits the teacher's emitted **TEXT** distribution, not its full next-token probability mass. The student learns *what the teacher said*, not *how confident the teacher was between alternatives*. We do **not** claim soft-target / logit-KL distillation. The `fmt` byte reserves `LESSON_FMT_SOFT` so a future wave adds it without a wire break — but until that wave's paired cert passes, soft is vapor.
2. **The teacher's subword-space reasoning** (attention, MoE routing, inter-token structure above the byte level) is collapsed at `tok_decode`. Whether byte-CE captures the teacher's reasoning *faithfully* is **unproven** — an honest open gap (conversational-teaching.md §3.3, BACKLOG.md Gap 2), not a solved problem.
3. **Information lost in `token→byte` for multi-byte / discriminative choices** — e.g. the teacher's preference *between* long tokens sharing a first byte would be lost even under the deferred soft projection (logit-align's own admission). For ASCII-heavy English the soft channel is thin; another reason to ship byte first and prove soft adds something before believing it does.

What the bridge **CAN** transfer is exactly what the `[cradle-teach]` cert measures and what `distill_proof.c` proves: the teacher's emitted byte sequence, made weight-resident, generalizing to held-out occurrences.

---

## 5. Sequencing — small falsifiable waves

### Wave **T-fix** — teacher selection + the bridge decision
- Ship Option A teacher-capable bit (§2.3, §3), `region_teacher()`, and `swim_teacher_gossip_self_test()`.
- **Decision recorded:** the BPE↔byte bridge ships as **`LESSON_FMT_BYTE` (byte-CE) only**; soft targets deferred with the `fmt` byte reserved. No soft-target claim is permitted until §6's paired cert passes.
- **Gate:** `[teacher-gossip]` PASS (converge/selector/determinism/staleness) + the existing `[cap-gossip-*]` suite still green (no SWIM wire regression; static asserts hold).

### Wave **T-1** — the 2-node teach demo cert
- Ship the lesson wire (`CRADLE_TEACH_PKT` + p-fs body), the `g_lesson_ring` corpus-source seam, and `cradle_poll_and_pull()`.
- **Gate:** `[cradle-teach]` PASS — the corrected cert of §1 with **all three falsification arms** (teaching-off, scrambled-bytes, teacher-death) and the train-copy/held-probe separation. **The auditor independently re-runs each arm sabotaged and confirms the cert fails when it should.** Plus: existing `distill_proof.c` unchanged and green (zero-regress for lesson-less nodes).
- **Determinism precondition (carry from risk):** pin `lm_generate`'s RNG seed per `(curriculum-prompt, seq)` and assert two successive HRW/min-id-elected teachers emit **byte-identical** lessons for the same prompt, else divergent corpora silently poison reproducibility. The cert teaches a **deterministic probe fact**, never relying on sampler output matching.

---

## 6. DEFERRED / OUT OF SCOPE

- **`LESSON_FMT_SOFT` (soft-target / logit-KL distillation) — DEFERRED to a CT-4-class wave.** The `(p − q)·invN` generalization of `st_backward` (`student.c:1159-1161`) and a `st_backward_soft` sibling are *named, not built*. Ship ONLY behind a **paired `[ct-soft-vs-byte]` cert** where `q` is the **only** supervision (completion bytes **withheld**, loss = pure `KL(q‖p)`, no onehot term) and `q`'s held-out drop on a **discriminative** probe (a prefix where two next-bytes are plausible and the teacher prefers one) **strictly exceeds** the byte-CE baseline. If `q` is scrambled or projected to noise it must provably fail (no byte fallback). The four-arm cert with bytes co-shipped does NOT falsify the soft claim (logit-align killer objection) — do not ship soft on it.
- **R3 closed-book novel-fact argmax recall on the byte student — OUT OF SCOPE.** No production analog exists in `student.c`; the masked key/value cert lives only in `r3_incontext.c` on the dtr substrate. If true fact-recall is wanted, route the lesson into the R3 in-context path — but then it is no longer "byte-resident in the student" and the Gap-2 honesty note becomes the headline, not a footnote.
- **CT-3 salience-weighted lesson rehearsal + DMN-budget cert** — needs T-1; the ring is bounded (`CT_RING_BYTES`), so multi-fact interference (wave-26 disease) can recur and needs the interleave/salience cure applied to the **ring**, not just the fixture.
- **In-kernel live teacher harvest (CT-2 full)** — T-1 demonstrates the path with an in-process/host-harvester teacher; a node that `lm_load`s a real GGUF in-kernel and the full M1d tokenizer→byte detail is a follow-on.
- **Ops enforcement that exactly one node sets `PKERNEL_TEACHER=1`** — convergence is safe with duplicates (lowest-id wins; dedupe by content-id `seq`), but single-teacher enforcement is deferred to ops/CI, same boundary as N-2b's supernode.

---

## 7. OPEN RISKS (every unresolved killer_objection carried forward)

1. **[CARRIED — text-level killer]** The naïve positional held-out cert can fail with teaching ON. **Mitigation in plan:** §1.2 separates a trained lesson copy from a never-trained held-out probe occurrence, computed from the production `train_end`. The auditor must confirm the cert (a) fails teaching-off, (b) fails scrambled, (c) **passes** teaching-on — the third is the inversion guard.
2. **[CARRIED — lesson-pack killer]** A scramble that *shuffles the same lesson bytes* leaves answer-byte frequencies intact and fails to falsify statistics-vs-sequence. **Mitigation:** Arm B uses **random-byte** scramble (the LCG already in `distill_proof.c:60`), not a shuffle.
3. **[CARRIED — lesson-pack killer]** Importing an R3 masked-fact cert onto the byte student is unimplementable/unprovable. **Mitigation:** §1.4 + §6 — the cert claims only held-out byte-CE transfer + survival; R3 recall is out of scope.
4. **[CARRIED — logit-align killer]** A soft cert with co-shipped bytes cannot isolate the soft channel; the first-byte projection (8000→256) likely carries ~zero marginal information for ASCII English. **Mitigation:** §6 — soft deferred behind a bytes-withheld paired cert; no soft claim until it passes.
5. **BPE→byte fidelity (Gap 2) stays OPEN.** Whether byte-CE captures the teacher's reasoning is unproven, honestly scoped (§4). Not blocking T-fix/T-1; blocking any "the student learned to *reason* like the teacher" claim.
6. **Teacher-determinism / corpus divergence.** Two teachers with different seeds emit different lessons. **Mitigation:** §5 T-1 precondition — pin the seed, assert byte-identical lessons across successive elected teachers; cert teaches a deterministic probe.
7. **`teacher_self()` truth-source.** Must key off a genuinely-loaded GGUF, not an env decree, or a lying node self-elects (degrade, not crash). **Mitigation:** §2.3 gates the bit on `lm_load` success; the cert's BEFORE arm must run where `region_teacher() != self`.
8. **Lesson-ring eviction starves a slow-pulling child** of a fact taught long ago — no provenance-outlives-eviction yet (same open follow-up as wave-35). LATEST_ONLY beacon means a child sleeping through several seqs gets only the newest lesson (acceptable for a babbling baby, lossy vs a full curriculum).
9. **Network dependency on the DMN tick.** The pull must stay strictly NO-OP-safe (no relay → fall back to fixture, never block sleep), preserving the no-op contract at `student_shell.c:430-438`.
10. **Live lessons are binary, not ASCII.** `tok_decode` emits NUL/control bytes; the wire must be length-prefixed binary. The ring/`window()` are `uint8_t`-clean, so this is wire discipline — but a regression to C-string handling silently truncates lessons.