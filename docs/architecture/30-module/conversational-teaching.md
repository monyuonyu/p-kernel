# conversational-teaching — a live teacher node educates the child over the network

> **Status: SHIPPED (cradle live L1/L2/L3, wave 2026-06-26) — reconciled 2026-07-01.**
> The live-teacher wire is real: `arch/common/cradle_net.c` publishes a **40-byte**
> `CRADLE_TEACH_PKT` (`dtr.h:446-460`) on the region-scoped `cradle/teach` topic, a child
> pulls the p-fs lesson body and trains during DMN sleep; cert `tests/llm/run_cradle_teach.sh`
> (`[cradle-teach]` + structural `[cradle-nocentral]`). The "ROADMAP / pre-impl" framing
> and the "192-B beacon" wire below are STALE — the real packet is 40 B and its fields are
> reconciled in §6.1. Read the rest as the design record.

**Status (historical): ROADMAP (design, pre-impl). 2026-06-19.** This is the concrete mechanism for mk_pino's education 考え方
(`project_education_via_conversation`, conversation.md §A4/§3.6/§3.7): *you cannot copy
weights into the special distributed dynamic-MoE student — you educate it by a high-spec
TEACHER node conversing with the CHILD a lot, and the dialogue is distilled into the child
during sleep.* It generalizes today's **committed-fixture** distill into a **live,
peer-sourced** one, without breaking determinism, the DMN budget, or NOCENTRAL.

It is the missing **step ⑤** named in `student_shell.c:40` ("Live-teacher harvesting is
step ⑤") and the live half of conversation.md §3.7's minimal loop
`(a) run teacher → soft targets, (b) distill into the native student, (c) diffuse`.

---
## 0. What is and is NOT being designed

- **IS:** the wire + selection + child-side distill that lets a teacher node's generated
  dialogue reach a child node and be folded into the child's student weights at sleep.
- **IS NOT:** the SmolLM2 *soft-target* distillation math (that is a separate within-node
  wave — today the child trains on teacher *bytes* via cross-entropy, see §3.3 honesty);
  the dynamic-size / cross-node-MoE student (special-structure-mind.md SS-2..SS-7); the
  KV cache. This design is transport + selection + corpus-folding; it composes with those.

The crown invariant it must not break: **R3 `rw[]` stays the small shared weight-merged
core** (FedAvg of identical-dims bodies, gossip_learn.c `gl_merge`); the student is taught,
never weight-merged across tiers (§4, the one-mind resolution).

---
## 1. Current vs target (verified on trunk)

| Piece | Reality on `wave-i18n-galaxy` | This design |
|---|---|---|
| Teaching signal | static committed `TEACHER_FIXTURE[]` string (`student_shell.c:71`) | LIVE bytes a teacher node generated with `lm_generate` (forward.c) |
| Distill driver | `student_dmn_consolidate()` windows the fixture, `sleep_rounds` → `st_forward/backward/adam` (`student_shell.c:363,141`) | SAME driver; windows a **received lesson ring** in addition to / instead of the fixture |
| Teacher engine | `lm_load`/`lm_forward`/`lm_generate` exist (forward.c) but are NOT wired to the consolidation loop | a teacher task harvests prompt→completion examples and publishes them |
| Transport for facts | region-scoped KDDS topic `mind/teach`, 53-B `MT_TEACH_PKT` (R3 engrams, dtr.h:383) | NEW p-fs lesson-pack object family `cradle/lesson/<t>/<seq>` (≤4096-B blocks) + a 192-B KDDS beacon `cradle/teach` |
| Teacher selection | n/a | deterministic-from-SWIM: teacher-CAPABLE ∧ min-id (or HRW) region member; survives teacher death |

The student is **byte-level** (`ST_VOCAB=256`, `student.h:56`), so a "teaching example" is
simply a run of bytes — the most mergeless, OOV-free, ownerless unit (conversation.md
§3.7's byte-256 decision). This is what makes the wire trivial: a lesson is just bytes.

---
## 2. mechanism — teacher generates, child distills at sleep

### 2.1 The teacher side (high-spec node only)
The premise (`project_education_via_conversation`): the teacher is heavy, so it lives on a
beefy volunteer machine, NOT every phone. A teacher-capable node is one that has actually
`lm_load`'ed a permissive-licensed GGUF (conversation.md §3.6 gate 1) — the SmolLM2 engine
in forward.c. On such a node a low-priority **`cradle_teach_task`** runs in the seconds-band
(the same cadence as gossip_learn's `GL_SLOW_BAND_MS=2000`, gossip_learn.c:995):

1. Pick a **prompt** from a deterministic curriculum seed (a fixed list of seed byte-strings,
   advanced by a counter — reproducible, so two teachers on the same seq agree). The "lots
   of conversation" is the teacher *generating* over many prompts across many ticks.
2. Run the teacher: `lm_generate` produces a completion (today: greedy ids → bytes; the
   tokenizer-to-bytes detail is the teacher node's, conversation.md §B). The pair
   `(prompt-bytes, completion-bytes)` is one **teaching example**.
3. Pack `LESSON_PACK_MAX` examples into a **lesson pack** (a content-addressed p-fs object,
   ≤4096 B = `PFS_BLOCK_MAX`) and `pfs_dag_save` it under `cradle/lesson/<teacher>/<seq>`
   (the same publish path gossip_learn uses, gossip_learn.c:139 `gl_pfs_publish`).
4. Bump the beacon: publish a 192-B `CRADLE_TEACH_PKT` on the region-scoped LATEST_ONLY
   KDDS topic `cradle/teach` carrying `{teacher_node, latest_seq, tier, vocab_fp, lic_id}`
   (announce-only, like `mind/teach`'s re-drive, r3_incontext.c:2249).

The teacher NEVER pushes into a child; it publishes content + a beacon. Children pull. This
keeps it peer-symmetric and lets late-joining / NAT'd children catch up by content.

### 2.2 The child side (every node raising a baby)
The child already has `student_dmn_consolidate()` driven by the DMN sleep tick
(dmn.c:206). We add a **pull-before-distill** step, gated exactly like today's no-op rules
(persistence on AND a resident/saved baby):

1. On a sleep tick, poll `cradle/teach`. If a teacher is advertising a `latest_seq` higher
   than the child's high-water for that teacher, **WANT** the missing
   `cradle/lesson/<teacher>/<seq>` packs (the content-driven pull, identical to
   `gl_merge_peers`'s `gl_pfs_fetch` retry loop, gossip_learn.c:960).
2. Each arrived pack is unpacked into a bounded **lesson ring** (file-static, never the
   task stack — the hosted-relay stack-overflow lesson, `feedback_hosted_relay_stack_overflow`).
3. `student_dmn_consolidate()` now `window()`s from the lesson ring (live teacher bytes)
   instead of `TEACHER_FIXTURE`, then runs the IDENTICAL `sleep_rounds` + skip-write
   throttle. Sleep = the child folds the teacher's conversation into its weights.

So the only new code on the hot path is "fill the corpus pointer from the ring instead of
the fixture, if the ring is non-empty." Everything downstream (the math, the determinism,
the flash-wear throttle, the held-out-loss proof) is unchanged.

### 2.3 Which transport carries what — and WHY

| Datum | Size | Transport | Why |
|---|---|---|---|
| teacher beacon `{node,seq,tier,vocab_fp,lic}` | ≤192 B | **KDDS topic `cradle/teach`** (region LATEST_ONLY) | tiny, latest-only, region-local discovery; same slot class as `mind/teach` (uses the `KDDS_SINGLETON_TOPICS` headroom, kdds.h:37) |
| lesson pack (N examples of bytes) | ≤4096 B | **p-fs DAG** `cradle/lesson/<t>/<seq>` | content-addressed, replicated by `pfs_repl`, relay-friendly, pull-by-WANT; the proven `gl_pfs_publish/fetch` path |
| (NAT-crossing) | — | **relay v2** (HMAC, ≤1380-B payload) | both KDDS and p-fs already tunnel through the relay; lesson packs >1380 B fragment over the existing pfs block transport, not here |
| student weights | ~22.8 MB | **NEVER on the wire** | tiers can't merge; only the *dialogue* crosses nodes (§4) |

The lesson **bytes** are what travels, exactly as conversation.md §3 mandates: "教えた言葉
が伝播 ... 借り物 base = 食べ物（教師）, 学び = 魂." The teacher's GGUF weights never leave
the teacher node; only the conversation it produced does.

---
## 3. child_distill — folding the lesson into the baby

### 3.1 Generalize the corpus source (one small change)
Today `window()` (`student_shell.c:120`) indexes `TEACHER_FIXTURE`. We introduce a
`g_corpus` pointer + length that defaults to the fixture and is rebound to the lesson ring
when packs have arrived. `heldout_loss`, `sleep_rounds`, `student_birth_warmup` all read
through `g_corpus`. A node with no lessons yet falls back to the fixture (so a fresh phone
still babbles and grows — the birth-warmup arc is preserved, `student_shell.c:285`).

### 3.2 DMN budget + wave-23 salience (no starvation)
Two consolidation tracks already share the sleep tick: R3's `rw[]` (runs only while
`r3_facts_pending`, self-limiting) and the student track (cadence-limited to
`1-in-ST_DMN_INTERVAL`, dmn.c:206). The lesson-fold stays **inside the existing student
track** and inherits its throttle — it does NOT add a third tick. The wave-23 salience idea
(reallocate a FIXED engram budget by EARNED salience) maps cleanly: a lesson pack carries a
`salience` byte (teacher's confidence / a danger-class flag) so the child rehearses
high-salience windows more *within the same bounded round count* — same budget, earned
allocation. This is the special-structure-mind.md gate-6 "DMN-budget cert across BOTH
tracks": the cert drives `dmn distill` and asserts the R3 idle-round delta is unchanged
while the student loss drops (the non-interference proof dmn.c already prints).

### 3.3 Honesty: bytes now, soft targets later
Today the child trains on the teacher's **bytes** via cross-entropy (`st_backward`), not on
the teacher's **soft-target logit distribution**. True logit-KL distillation (the dense
teacher's full next-byte distribution → student) is more sample-efficient and is the real
conversation.md §3.7(a) "soft target" — but it needs the teacher to ship a 256-float
distribution per position (a bigger lesson pack and a KL loss in student.c). This design
ships the **byte** form first (cheapest, proven by `distill_proof.c`) and leaves a `format`
byte in the pack header so a future `LESSON_FMT_SOFT` can carry quantized logits without a
wire break. Do not claim soft-target distillation until that wave lands.

---
## 4. one_mind_resolution — teaching IS the cross-tier bridge

State it plainly (resolves special-structure-mind.md CRITIQUE GATE 2 and §3.5's "one-mind
contradiction"):

- **Heterogeneous students DO NOT weight-merge.** `gl_merge` is a flat element-wise fold to
  one length (gossip_learn.c:69; `GL_MERGE_MAXFLOATS`); different tiers (S/M/L) have
  different dims, so a strong node's expert learning can NEVER reach a weak node by merge.
  Same-tier nodes may merge their students (special-structure-mind.md SS-3 cohorts), but
  there is no fleet-wide cross-tier student average — by construction.
- **The shared fleet-wide "one mind" stays R3's `rw[]`** — the small, identical-shape,
  FedAvg-merged core (the wave-41/42/44 crown). Heterogeneous students never enter
  `gl_merge` of `rw[]` (the mandated `[baby-merge-isolation]` tripwire, SS-0).
- **Therefore cross-tier / cross-node knowledge transfer = TEACHING (this document):** a
  strong teacher node *converses*, the dialogue (bytes) crosses the network, and a weaker
  child *distills* it at sleep. The collective conversational mind is achieved by teaching,
  not by averaging incompatible weights. This is conversation.md §A4 made into a wire.

So "心をひとつに" has two layers, each with its own correct mechanism: the **shared core**
is one by FedAvg (`rw[]`); the **diverse bodies** become one in capability by teaching
(this design). No contradiction, no fork.

---
## 5. teacher_selection — NOCENTRAL, deterministic, survives death

No election, no Raft (the forbidden central coordinator). Selection is a **local function of
local SWIM state + a verifiable local capability**, exactly the region-coordinator
philosophy (region.c:83 min-id; lookup.h HRW).

A node is a **candidate teacher** iff BOTH hold, locally checkable:
1. **Capability:** it has actually `lm_load`'ed a permissive teacher GGUF (the high-spec
   premise + conversation.md §3.6 gate-1 license filter — a verifiable property, not a
   decree). A phone with no GGUF is simply never a candidate.
2. **Liveness:** it is `DNODE_ALIVE` in the local SWIM view.

Among candidates in the child's region, the **active teacher is deterministic-from-state**:
the HRW-rendezvous winner of `hash(region-epoch, candidate-id)` over the alive candidate set
(or, simplest first, the **min candidate-id**, reusing region.c's min-id pattern). Every
child computes the same answer locally from its own SWIM view — no broadcast vote, no
registrar. A child PULLS from whichever teacher is currently beaconing; if two briefly
disagree during churn, the child just pulls from both teachers' lesson families (harmless —
more dialogue, deduped by `(teacher,seq)` high-water).

**Teacher death = automatic handoff.** When the active teacher dies, SWIM reaps it
(`DNODE_DEAD`), its `cradle/teach` beacon goes stale (LATEST_ONLY, no re-drive), and the
next candidate by the SAME deterministic rule becomes active and starts beaconing. Children
keep their already-pulled lesson ring (the teacher's death does not erase what it taught —
the survival-network property). No interregnum stalls learning; at worst a gap until the
successor's first lesson pack lands. This is the same "deterministic-from-membership +
re-home on death + safe fallback" contract special-structure-mind.md §6 uses for expert
placement.

**Volunteer model (conversation.md §3.7):** "誰でも教師を提案できる" = anyone may run a
teacher (load a GGUF → become a candidate). "滋養で選ぶ" = the no-regress DMN gate (a lesson
that does not improve the child's held-out loss is simply not persisted, student_shell.c's
skip-write). "進化で伝える" = good students propagate by same-tier merge / by becoming
teachers themselves. "来歴で覚える" = the lesson pack carries the teacher's node + GGUF
content-id + license, recorded in the Self-lineage (special-structure-mind.md §7).

---
## 6. protocol — wire format, topics, cadence

### 6.1 The beacon (KDDS topic `cradle/teach`, region-scoped, LATEST_ONLY)

SHIPPED layout — the real `CRADLE_TEACH_PKT` (`arch/common/include/dtr.h:446-460`) is
**40 bytes**, not the 192-B sketch this doc originally proposed:
```
CRADLE_TEACH_PKT  (packed, 40 B = 4+1+1+2+4+4+16+8, <= KDDS_DATA_MAX)
  UW  magic                    CRADLE_MAGIC
  U1  teacher_node             origin id (== region_teacher() when healthy)
  U1  fmt                      LESSON_FMT_BYTE now; _SOFT reserved
  U1  _pad0, _pad1             alignment
  UW  seq                      per-teacher monotonic lesson high-water
  UW  body_len                 lesson byte length (<= CT_LESSON_MAX)
  U1  body_ref[CRADLE_REF_LEN=16]  p-fs DAG object name of the lesson body
  U1  vocab_fp[MT_VOCAB_FP_LEN=8]  byte-keying fingerprint (refuse-on-mismatch)
```
The body is a length-prefixed BINARY blob fetched from p-fs by `body_ref` (NOT inlined in
the beacon), so the beacon stays tiny and latest-only. The originally-sketched
`tier`/`wire_ver`/`gguf_id`/`lic_ok` fields were dropped in the shipped design.
Reserved at boot via `kdds_open_poll_scoped(..., KDDS_SCOPE_REGION)` in the same place
`mind_net_open` reserves `mind/teach` (r3_incontext.c:2156), using the
`KDDS_SINGLETON_TOPICS` headroom — it must be reserved BEFORE dkva pre-opens saturate the
topic table (the wave-48 lesson, kdds.h:44).

### 6.2 The lesson pack (p-fs DAG object `cradle/lesson/<teacher>/<seq>`)
```
LESSON_PACK_HDR  (object body <= PFS_BLOCK_MAX = 4096 B)
  UW  magic            "CLSN" LE
  UW  seq              matches the beacon's latest_seq high-water
  U1  teacher_node
  U1  fmt              LESSON_FMT_BYTES
  U1  n_examples       count of (prompt,completion) byte runs that follow
  U1  salience         wave-23 rehearsal weight (0=normal, higher=rehearse more)
  ... then n_examples records: { U1 prompt_len; U1 comp_len; bytes[prompt][comp] }
```
Object name fits `PFS_NAME_MAX=16` (e.g. `cl/<tt>/<ssss>`). Published with `pfs_dag_save`,
pulled with `pfs_dag_read` + WANT (the `gl_pfs_fetch` retry loop). Bytes-only format is
deterministic and ABI-independent (the byte-256 decision); `vocab_fp`/`fmt` reserve the path
to soft-target packs without a wire break.

### 6.3 Cadence
- **Teacher:** one generate+publish per seconds-band tick (`GL_SLOW_BAND_MS`); a new lesson
  pack every few ticks (batched to amortize the 4096-B publish). Bounded curriculum cursor.
- **Child:** poll the beacon and WANT missing packs once per DMN sleep tick
  (`ST_DMN_INTERVAL` idle pulses, dmn.c:206) — the SAME cadence that already gates the
  student distill, so no new heartbeat and no extra flash wear. The skip-write throttle
  still decides whether the post-fold weights are worth persisting.

### 6.4 Request/receive (child)
Pure content-pull, no RPC to the teacher: the child issues p-fs WANTs for the seq range
`(my_high_water+1 .. beacon.latest_seq)`, bounded per tick (e.g. ≤4 packs/tick), with the
`gl_merge_peers` retry/wait discipline so a 4096-B pack that lagged still lands next tick.
Idempotent: dedup by `(teacher_node, seq)`.

---
## 7. sequencing — cheapest first, prerequisites honest

1. **CT-0 — beacon + lesson-pack wire, NO teacher engine (loopback).** Define
   `CRADLE_TEACH_PKT` + `LESSON_PACK_HDR`; reserve `cradle/teach` at boot; teach task
   publishes packs whose examples are drawn from the EXISTING `TEACHER_FIXTURE` (no GGUF
   yet). Child pulls + folds. Cert `[ct-wire-roundtrip]`: a pack published on node A is
   pulled and unpacked byte-identical on node B; `[ct-fold-equiv]`: folding a
   fixture-derived pack lowers held-out loss the same as the in-process fixture distill.
   **No prerequisite beyond the relay/p-fs already shipped.** This is the safe first slice.
2. **CT-1 — deterministic teacher selection (§5).** Candidate = ALIVE ∧ has-GGUF; active =
   min-id (then HRW). Cert `[ct-teacher-deterministic]` (all children agree from the same
   SWIM view), `[ct-teacher-handoff]` (kill the teacher → successor beacons, child keeps its
   ring). NOCENTRAL preserved. Needs CT-0.
3. **CT-2 — live teacher harvest.** Wire `lm_generate` (forward.c) into `cradle_teach_task`:
   real generated completions replace fixture-derived examples on a node that has a GGUF.
   Cert `[ct-live-teach]`: a child with NO fixture knowledge of a teacher-only prompt
   answers/continues it better after sleeping over live packs (held-out on teacher-sourced
   windows). **Prereq: a node that can actually `lm_load` a GGUF (the high-spec premise) +
   the M1d tokenizer-to-bytes detail; honest that phones are children, not teachers.**
4. **CT-3 — salience-weighted lesson rehearsal (§3.2) + DMN-budget cert.** Carry/honor the
   `salience` byte; re-certify the wave-23 budget across both tracks (gate-6). Cert
   `[ct-salience]` (high-salience windows rehearsed more within the fixed budget),
   `[ct-dmn-budget]` (R3 idle-round delta unchanged). Needs CT-0.
5. **CT-4 — soft-target lessons (§3.3).** `LESSON_FMT_SOFT`: teacher ships a quantized
   next-byte distribution; child adds a KL term in student.c. Cert `[ct-soft-equiv]`
   (KL-distill ≥ byte-CE on held-out at equal packs). **Prereq: student.c KL loss; bigger
   packs; the real soft-target win conversation.md §3.7 wants — sequenced last, biggest
   lift.**

Waves needing the bigger premise first: **CT-2** (a real teacher node with a GGUF) and
**CT-4** (soft-target math + bandwidth). CT-0/CT-1/CT-3 deliver the live-teaching substrate
on the current small byte baby with only the already-shipped relay/p-fs/SWIM.

---
## 8. honest risks / open questions

- **Bandwidth & cold-start.** Lesson packs are bytes, cheap (≤4 KB), but a child starting
  from babble needs MANY ticks of teacher dialogue to move — conversation.md §3.6's "赤子は
  ゆっくり育つ." The UX must show this as growth, not breakage (product-soul).
- **Byte-CE is not soft-target distillation.** §3.3 — the cheap form first; the real
  efficiency win is CT-4 and must not be claimed early.
- **Determinism of the teacher's output.** `lm_generate` greedy is deterministic per
  (GGUF, prompt); but two teachers with DIFFERENT GGUFs produce different lessons. That is
  fine (a child may pull from several) but the curriculum cursor must be reproducible so a
  successor teacher can resume coherently. Sampling (non-greedy) teachers need a seeded RNG
  in the pack for replay.
- **Poison teacher.** A teacher could ship plausible-but-wrong dialogue that passes the
  no-regress gate yet bends the child (conversation.md §3.6 open threat). Mitigation is the
  same as the live fleet: no-regress DMN gate + 来歴 (recorded provenance) + "audit is the
  engine," not prevention. Selection's license/capability gate filters *unlicensed*, not
  *malicious*.
- **Tier compatibility of byte lessons.** Byte-256 is tier-invariant (vocab is fixed all
  tiers, student.h:56), so a lesson pack is foldable by ANY tier — good. But a lesson tuned
  to an L-tier teacher's style may be too hard for an S-tier child; the `tier` field lets a
  child prefer same/lower-tier teachers. Open: whether to enforce or merely prefer.
- **Region locality.** The beacon is region-scoped (low RTT), so children learn from a NEAR
  teacher. A child in a teacher-less region learns nothing until a teacher joins or the
  scope is widened — an honest gap, not a stall (the child keeps its existing ring).
