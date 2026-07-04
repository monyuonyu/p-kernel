# living-mind — an ownerless mind that learns from conversation and sleeps to remember

> Status: **SHIPPED (LM-1..11) + CI-gated.** — trimmed 2026-07-01 (doc-hygiene P3).
> Every slice below shipped and is guarded in `.github/workflows/ci.yml`. The original
> per-slice *pre-implementation* scaffolding ("the claim to prove / falsifiable acceptance
> test / anti-fork constraint / CI verb plan / COMMANDER DECISIONS NEEDED") has been cut —
> it lied about TIME, not about code. That archaeology is preserved verbatim in git history
> (`git show 79518a33:docs/architecture/living-mind.md`). What remains here: **Part I** (the
> north star, unchanged) and **Part II — what shipped** (a compact per-slice SHIPPED table:
> file + wave + CI tag + measured result). Genuinely-future items (the Evolution layer) are
> marked FUTURE. The canonical live-gap list is `gap-ledger.md`.

This document is two parts, like [survival-network.md](../00-concept/survival-network.md):

- **Part I — the north star.** The design, grounded in existing p-kernel pieces,
  honest about what is hard. No new subsystem is invented where one already exists.
- **Part II — the first slice.** ONE concrete, falsifiable proof-of-principle that
  gets implemented next wave: a rest-time ("sleep") consolidation that replays
  stored engrams and distills them into `dtr` weights via G22-style gossip, so the
  mind learns from a *stream* of tasks **without catastrophic forgetting**,
  decentralized, surviving node death + rejoin.

---

## Reading convention — **dream-tier** names vs **artifact-tier** facts

> An external reviewer named a real risk: this document fronts **grand names** — *the
> mind*, *魂* (soul), *考える器官* (an organ that thinks), *collective consciousness*
> — over what is, today, a small toy. Read naively, that is an "oversized name + endless
> caveat." We answer it not by shrinking the names (they are the deliberate north star
> and they STAY) but by making the two tiers a **stated convention** you can rely on
> everywhere in this doc:

| tier | what it is | how to read it |
|---|---|---|
| **dream-tier** | the north-star name and the worldview it points at (*the mind*, *魂*, *考える器官*, *collective consciousness*, *the mind becomes one*). | The DIRECTION. Deliberately oversized. Never delete it; never read it as a claim about what runs today. |
| **artifact-tier** | the concrete thing that actually runs *this wave*, in numbers and dims (parameter counts, `R_DM`, vocab sizes, single-token recall, no grammar, no generation). | The MEASURED TRUTH. Always co-located with the dream name. This is what is real today. |

**The rule:** wherever a dream-tier name fronts an artifact, an **`artifact today:`**
line states the artifact-tier truth right there — same paragraph, no hunting. This
turns the honesty from a per-mention apology into a structural label. The `[live]` /
`[in-proc]` verification tier (see `gap-ledger.md`) is orthogonal: it says HOW the
artifact was proven; the artifact-tier line says WHAT the artifact is.

> **artifact today (the whole living-mind, as of wave-41/LM-11):** an R3 in-context
> Transformer, **`R_NP`=21,568 parameters**, `R_DM`=48 attention width, **16 key-words ×
> 64 answer-words**, **single-token associative recall** (`mind teach sky blue` →
> `mind ask sky` → "blue"), comfortable-N=16 simultaneous bindings. **NO grammar, NO
> generation, NO multi-token output, bounded synthetic-ish vocab.** What is genuinely
> built ON TOP and measured: sleep-consolidation against catastrophic forgetting (LM-1/3/5),
> a hash-chained autobiographical Self (LM-2), fast→slow weight transfer (LM-4), a
> cross-node shared fact that survives its teacher's death (LM-7, `[live]`), and
> weight-level merge of divergent minds (LM-10/11). That list is real; "language/grammar/
> generation" is NOT in it. The grand names are where this is GOING.

---

## Part I — the north star

### I.0 What the living-mind is

An **ownerless conversational mind** that:

1. **learns continuously FROM CONVERSATION** (随時 / online, not a batch retrain), and
2. **can evolve its own architecture while alive** — the **Evolution** layer, the
   least-built of the 5-layer worldview (Body / Brain / Self / Collective / Evolution).

This is the survival-network thesis (`survival-network.md §9`) turned inward on the
mind itself: *保存する図書館から、考える器官へ* ("from a library that preserves, to
an organ that thinks"). Memory is the precondition; **thinking that keeps learning**
is the goal. The Collective layer already thinks across nodes (G22/G38). The
living-mind adds the missing arrow: **the collective keeps learning, forever, from
what it is told — without forgetting, without an owner, without dying.**

> *考える器官* / "an organ that thinks" is **dream-tier**. **artifact today:**
> associative recall — `R_DM`=48, 16 key-words × 64 answer-words, single-token recall,
> NO grammar, NO generation. The "organ that thinks" is the direction this artifact is
> built toward, not a description of what the artifact does this wave.

### I.1 The three hard problems (stated honestly)

A mind that never stops learning and never dies has three failure modes that a
batch-trained, frozen-at-deploy model never has to face:

1. **No labels in raw conversation.** A live stream of dialogue has no ground-truth
   `y`. Supervised SGD needs a target. You cannot run `dtr_train_batch` on a chat
   log directly.
2. **Catastrophic forgetting under online weight updates.** If you *do* update
   weights on each new input as it arrives, the network overwrites earlier
   competence — the classic stability problem. An always-on learner is an
   always-on forgetter unless something protects the past.
3. **Drift / instability of an always-learning, never-dying system.** With no
   "deploy freeze," small online errors compound; feedback loops (the system learns
   from text it itself produced) can diverge. A mind that runs for years must stay
   *bounded* without a human pressing reset.

### I.2 The solution, mapped onto EXISTING p-kernel pieces

The strategy is **memory-grounded continual learning first, weight change last** —
the cheapest, safest mechanisms before the dangerous ones. Each rung already has an
implemented substrate; we do not invent new ones.

**Rung 1 — retrieval, ZERO weight update (forgetting-free by construction).**
The first answer to "no labels / catastrophic forgetting" is *don't update weights
at all*. Store each episode as a durable **engram** in p-fs (content-addressed,
versioned, region-replicated: `pfs_dag_save` / `pfs_dag_read`, the same store G22
already publishes weight blobs into). New context is answered by **reading**
engrams, not by overwriting weights. Nothing is forgotten because nothing is
overwritten. R3 already proved the substrate can *use a dictionary it has never
seen, at inference time, from the prompt* (`r3_incontext.c`) — that **is** the
retrieval/fast layer. This rung needs no training at all.

**Rung 2 — slow weight consolidation via G22 gossip distillation.**
Retrieval alone never *compresses* — the engram store grows without bound and the
mind never internalizes anything. So, *occasionally and offline*, replay the stored
engrams and distill them into the slow weights with the **same** decentralized
machinery G22 already runs: local replay-SGD (`dtr_train_batch`) followed by
no-central weight averaging (`gl_merge`). G22 today trains on a *static* shard; the
living-mind trains on *replayed engrams*. The math, the merge, the anti-fork
discipline are unchanged.

**Rung 3 — the §8 two-layer = stability/plasticity.**
`survival-network.md §8` splits the system into a fast reflex layer and a slow
deliberation layer (脊髄反射 / 大脳). Re-read for the mind:

- **Fast layer = in-context (R3's substrate).** High plasticity, ZERO weight change.
  Holds *this conversation* in the prompt/engram cache. Cannot forget the slow
  weights because it never touches them.
- **Slow layer = consolidated weights.** Low plasticity, changed only offline during
  rest (Rung 2). Holds *what has been learned across all conversations.*

Stability and plasticity stop fighting because they live in different layers on
different time-constants — exactly the `[moe-twolayer]` / `[moe-osc]` resolution
G38 already shipped for guarding, now applied to *learning*.

### I.3 THE key abstraction — the Default Mode Network (DMN)

In the brain the **Default Mode Network** is the **rest-time, task-NEGATIVE**
network: it is *anticorrelated* with the task-positive networks — it switches ON
when you stop reacting to the outside world. It does three things:

1. **Memory consolidation** — hippocampal replay → cortical integration. This is
   what *sleep* is for, and it is *why sleep prevents catastrophic forgetting*: the
   day's episodes are replayed offline and woven into long-term cortical weights.
2. **Self-referential / autobiographical thought** — the narrative "self."
3. **Future imagination / simulation** — rehearsing situations that have not
   happened yet.

Map each onto p-kernel:

| DMN function (brain) | p-kernel mechanism | Status |
|---|---|---|
| **Consolidation** (replay → distill = sleep) | replay p-fs engrams → distill into `dtr` weights via G22 gossip (`gl_merge`). **This is how sleep prevents catastrophic forgetting.** | **the first slice (Part II)** |
| **Self-referential** thought (the Self layer) | the distributed Self layer — the node's autobiographical engram history + its own model version as a p-fs object | future |
| **Future imagination / simulation** | idle-time **threat rehearsal** = proactive learning. Already partially present: G38's `reflex_threat_experience` oversamples danger classes (`gl_build_weighted`), and the DMN idle hook already runs `ga_step()` (idle self-improvement). | partial / future |

**The reframe of §8 (do this verbatim in the team's heads):** §8 is not merely
"fast layer / slow layer." It is **the reacting self / the reflecting self.** Today
p-kernel only has the *reacting* self: the reflex (`reflex.c`) is purely
task-POSITIVE — it fires when threatened and is silent otherwise. The missing organ
is the **task-NEGATIVE reflecting self** that runs *when the reflex is idle*, and
turns lived experience into lasting competence.

**Important honest finding (the organ is not missing — its content is).** A
task-negative idle organ **already exists**: `arch/common/dmn.c` / `dmn.h` (Phase 13,
"Default Mode Network"). It has the right skeleton — ACTIVE/IDLE state machine,
`dmn_trigger()` called from `dtr.c` (inference) and `swim.c` (membership change), an
idle threshold, and an idle work hook `dmn_idle_work()`. But that hook today only
**logs digests, checks `degrade_level()`, and calls `ga_step()`** — it does *not*
replay engrams or consolidate via gossip. Its own header even lists "(将来) fedlearn
勾配集約 / KVキャッシュ再構築" as future work. **So the first slice EXTENDS
`dmn_idle_work`; it does not build a DMN from scratch.** (A natural live rest period
already exists too: the UMP Android charge-only foreground-service mode — the device
is plugged in and idle, the perfect time to sleep-consolidate.)

### I.4 The Evolution layer — sketch only, explicitly FUTURE

Out of the first slice. Stated at altitude so the first slice does not paint us into
a corner:

- **Versioned architecture as a first-class p-fs object.** Not just *weights*
  (`dtr/weights` already exists) but the *schema* — d_model, layer count, expert
  topology — saved as a content-addressed, versioned manifest (the p-fs DAG already
  versions blobs; `survival-fs.md` / `genome.c` are the genome precedent).
- **Concurrent old/new during rolling migration.** During a structural change, the
  fleet runs both the old and the successor architecture side by side; no global
  stop-the-world (the death-piercing / degrade machinery already tolerates
  heterogeneous live nodes).
- **Weight translation / distillation across structural change.** The old model
  *teaches* the successor (knowledge distillation across a schema boundary), rather
  than retraining from scratch — the same replay-then-distill mechanism as Rung 2,
  but the "teacher" is the previous architecture.
- **Decentralized consensus on the active schema.** No central authority decides
  which architecture is "current"; nodes converge on it the same no-central way
  refs converge (`pfs_dag` LWW + the G22 gossip discipline).

This is the Evolution layer's protocol *shape*. **It is NOT in the first slice and
must not be implemented yet.** The first slice changes only *weights*, never the
architecture.

---

## Part II — what shipped (the slices, reconciled)

All eleven slices (LM-1..11) shipped and are CI-gated. This table replaces the
~3,800 lines of per-slice pre-implementation design that used to live here (claim →
acceptance test → anti-fork surface → CI verb plan). Read it as the **artifact-tier**
record; the **dream-tier** direction is Part I above. Every CI tag below is grepped
green in `.github/workflows/ci.yml`; the disease/cure numbers are the measured
results recorded at ship time.

| Slice | Dream-tier name | Ships in | Wave | CI tags (in ci.yml) | Measured result (artifact-tier) |
|---|---|---|---|---|---|
| **LM-1** | DMN sleep-consolidation | `arch/common/dmn.c`, `lm_consolidate.c` | 21 | `[dmn-forgetting]` `[dmn-consolidated]` `[dmn-distributed]` `[dmn-survive]` `[dmn-gradcheck]` | Catastrophic forgetting real (task-0 91.7%→33.3%); replay-distill cure 80.0% (+46.7), decentralized, survives node death. |
| **LM-2** | the Self layer (autobiographical self) | `arch/common/lm_self.c` | 22 | `[self-continuity]` `[self-tamperevident]` `[self-ownerless]` | Hash-chained self/lin lineage survives death (continuity 8/8), tamper-evident + fail-closed, reconstructs ownerless. |
| **LM-3** | salience-weighted replay (the DMN's imagination) | `lm_consolidate.c` (salience path) | 23 | `[salience-earned]` `[salience-retains]` `[salience-noregress]` | Danger class rehearsed more by reallocating a FIXED engram budget by earned salience; dgain +16.7, honest tradeoff (sloss 15.0), no regress. |
| **LM-4** | fast→slow handoff (in-context knowledge becomes weights) | `r3_incontext.c` (r_backward), `dmn.c` | 24 | `[handoff-fast-only]` `[handoff-consolidated]` `[handoff-grounded]` | A fact taught ONLY in-context becomes weight-resident after a self-distillation sleep: masked_pre 18.8→post 100.0; scrambled 0.0 (zero transfer). |
| **LM-5** | 随時: the living consolidation loop | `arch/common/dmn.c` (`dmn_idle_work`) | 26 | `[stream-interference]` `[stream-consolidated]` `[stream-grounded]` `[stream-livehook]` | Multi-fact interference real (naive f1 100→35); interleaved replay across sleeps cures all facts to 100 (+65), scrambled 0.0×4; wired onto the fleet's linux idle hook (not x86-only dead code). |
| **LM-6** | the mouth (a real conversational producer) | `mind_cmd` (`mind teach\|ask\|wait`) | 29 | `[teach-arrival]` `[teach-live]` `[teach-consolidated]`, plus `[dmn] sleep` printed exactly 10× | Owner teaches the live mind from the shell AND galaxy; real DMN heartbeat consolidates; one `++` site, no direct round calls. |
| **LM-7** | the shared mind (taught on A, answerable from B) | Path E over the mesh (`gossip_learn.c`, cradle teach) | 35 | cross-node teach/answer certs (`[teach-*]`, ref-adoption) | Teach on A → B answers AND names the teacher; kill A, B still answers. Path E won numerically (engram-carry vs weight-carry). |
| **LM-8** | the language slice (real words in/out, one-token answers) | R3 substrate widening + tokenizer/vocab | 39 | `[lang-gradcheck]` `[lang-capacity-v2]` `[lang-recall]` `[lang-oov]` `[lang-sensor-intact]` | Real word-level associative recall (`teach sky blue` → `ask sky` → "blue"); capacity curve is the headline; OOV + sensor-intact guarded. |
| **LM-9** | capacity surgery (widen the thinking width, R_DM 32→48/64) | R3 dims + shared-LayerNorm surgery | 38/39 | `[capacity-score]` (+ re-baselined `[lang-*]`) | Comfortable-N (simultaneous bindings) 4→16; the lever was R_DM=attention width, not vocab; shared-LayerNorm surgery proven safe. |
| **LM-10** | Path W: the one mind (weight-states converge) | chunked multi-block weight publish (`mind onemind`) | 41 | `[onemind-divergent]` `[onemind-cured]` `[onemind-nocentral]` | Naive weight-averaging of two minds that learned DIFFERENT facts is LOSSY (k1=100%, k2=8.8%≈chance); union-replay consolidation after merge recovers BOTH to 100%. So the mind is one, but needs collective sleep. |
| **LM-11** | Path W²: the weighted / Fisher merge (`mind wmerge`) | diagonal-Fisher per-param merge, computed LOCALLY | 42–44 | `[wmerge-vs-mean]` `[wmerge-noreplay]` `[wmerge-symmetric]` `[wmerge-divergence]` `[wmerge-nocentral]` `[wmerge-noregress]` | A smarter (diagonal-Fisher) merge rescues the collapsing fact WITHOUT replay (8.8%→85%), robust across ~4 decades; F-LOCAL = zero extra wire; the weighted path is an additive sibling to LM-10 (no regress). |
| **LM-12** | belief revision (`mind revise`) — a weight-resident belief is REPLACED, not blended | `r3_fact_revise` (supersede-in-place) + the 4 sites, `r3_incontext.c`; [living-mind-lm12-belief-revision.md](living-mind-lm12-belief-revision.md) | — | `[rev-baseline]` `[rev-blend]` `[rev-not-masked]` `[rev-cured]` `[rev-noregress]` `[rev-rebound]` `[rev-persist]` (+ live `[rev-live]` `[rev-stale-mouth]`) | A consolidated `sun→yellow` is re-taught `sun→green`: the new belief becomes weight-resident (share **100%**) and the old is DISPLACED (share **0%**), rebound- & restart-proof, via the SAME DMN sleep (zero new math, `dmn.c` byte-identical). Naive dual-enqueue BLOCKS the new belief (D1: vn=0%); supersede+0-rounds still reads the old (D2 falsifier); propagates over Path E (Site 2, last-arrival-wins, survives the reviser's death). |

### II.a Honest bounds (what none of this proves yet)

Preserved from the per-slice "honest bound / what this does NOT prove" notes so the
honesty is not lost in the trim:

- **It is a toy by parameter count.** The whole living-mind at LM-11 is `R_NP`≈21,568
  parameters, `R_DM`=48, 16 key-words × 64 answer-words, single-token associative
  recall. **No grammar, no generation, no multi-token output.** The grand names are
  the DIRECTION (Part I), not a description of today's artifact.
- **"Learning from conversation" = single-token binding**, not open dialogue. The
  mouth (LM-6) teaches key→answer pairs; it does not parse sentences.
- **Consolidation is proven against synthetic disjoint fact-sets**, not against real
  noisy conversational streams. The forgetting disease and its replay cure are real
  and measured, but on constructed tasks.
- **Path W / W² merge is proven on divergent toy minds**, and averaging BLENDS beliefs
  (it is belief-blending, not belief-revision). Union-replay (LM-10) or Fisher
  weighting (LM-11) is required to avoid one fact dying.
- The **Evolution layer** (Part I.4 — versioned architecture, concurrent old/new during
  migration, cross-schema distillation) is **FUTURE**. Every shipped slice changes only
  *weights*, never the architecture.

### II.b Provenance / process

- Canonical live gaps: `gap-ledger.md` (this doc adds no rows). The historical gap
  audits are archived under `archive/philosophy-gap-audit*.md`.
- Full pre-implementation design for each slice (the acceptance tests as originally
  written, the anti-fork reuse surfaces, the CI verb plans, and Part XII's COMMANDER
  DECISIONS for the wmerge/Fisher fork — all now resolved and shipped) is preserved
  verbatim in git history: `git show 79518a33:docs/architecture/living-mind.md`.
