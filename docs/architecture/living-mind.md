# living-mind — an ownerless mind that learns from conversation and sleeps to remember

> Status: **design + acceptance test** (written before implementation, like R3 / wave-18).
> Owner of the *first slice*: the next wave (separate implementer + auditor).
> Builds ON: R3 (in-context recall capacity certificate, **closed**) and G22/G38
> (decentralized gossip learning + the two-layer couple, **closed**).
> Sequencing gate (from `project_living_mind_vision`): "after G38/R3" — both are
> closed in `gap-ledger.md`, so this is unblocked.

This document is two parts, like [survival-network.md](survival-network.md):

- **Part I — the north star.** The design, grounded in existing p-kernel pieces,
  honest about what is hard. No new subsystem is invented where one already exists.
- **Part II — the first slice.** ONE concrete, falsifiable proof-of-principle that
  gets implemented next wave: a rest-time ("sleep") consolidation that replays
  stored engrams and distills them into `dtr` weights via G22-style gossip, so the
  mind learns from a *stream* of tasks **without catastrophic forgetting**,
  decentralized, surviving node death + rejoin.

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

## Part II — the first slice: DMN consolidation (a toy proof-of-principle)

This is what the next wave implements. It mirrors `r3-nontrivial-thought.md`'s
rigor: a claim, a falsifiable certificate with numeric thresholds, a named honest
bound, a hard anti-fork constraint, and an explicit "what this does NOT prove."

### II.1 The claim to prove

> A rest-time ("sleep") consolidation process that **replays stored engrams** and
> **distills them into `dtr` weights via G22-style gossip**, such that the mind
> learns continuously from a **STREAM of tasks WITHOUT catastrophic forgetting**,
> **decentralized** (no central consolidator), and **surviving node death + rejoin**
> (engrams are durable + replicated in p-fs).

The disease (catastrophic forgetting) must be **demonstrated first** — exactly as R3
makes `handif` come out near chance to prove the bar is real. If the no-replay
baseline does *not* forget, the test is vacuous and FAILS.

### II.2 The task — a continual stream of tasks

Reuse the deterministic synthetic sensor generator family already in
`gossip_learn.c` / `dtr_train.c` (latent variable → 3 classes, with distractor
channels and label noise so a fixed threshold cannot hit 100%). A **task** is one
labelling of that space. The **stream** presents `T` tasks **sequentially**, each as
an online run of episodes; the learner sees task `t+1` only after task `t` is done.

Two honest ways to instantiate distinct-but-comparable tasks (implementer picks one,
reports which):

- **(A) Class-permutation tasks.** Task `t` applies a fixed permutation `π_t` of the
  3 class labels to the same generator. Training task 1 after task 0 drives the
  weights to a contradictory mapping → measurable forgetting of task 0.
- **(B) Region-shift tasks.** Task `t` shifts the latent decision boundaries
  (the `gl_uniform` band edges). Sequential training overwrites earlier boundaries.

Both keep the **input/output dims frozen** (`DTR_SEQ_LEN`=4 in, `DTR_OUT_DIM`=3 out)
so the **635-param `dtr` body is reused unchanged** — no architecture change (that is
the Evolution layer, future). `T` small (e.g. 3) and compiled-in, consistent with
`dtr.h` scale.

### II.3 The engram replay buffer (on p-fs, content-addressed)

The "hippocampus." A bounded ring of past episodes, stored durably in p-fs so it
survives power loss and replicates across the region.

- **One engram** (tiny, packed, the size class of `DTR_LOG_ENTRY` = 8 bytes):
  ```
  struct LM_ENGRAM {
      B   input[DTR_SEQ_LEN];   /* the episode's sensor input (int8[4])     */
      UB  label;                /* its target class                          */
      UB  task_id;              /* which task in the stream produced it       */
      UB  salience;             /* replay priority (from reflex threat exp.)  */
      UB  _pad;                 /* (or pack seq/timestamp if a slot is free)  */
  }   /* 8 bytes; many fit one PFS block (PFS_BLOCK_MAX) */
  ```
- **Where it lives.** A small ring of engram blocks under a p-fs name, e.g.
  `lm/eng/<k>` (≤ `PFS_NAME_MAX`=16 chars), saved with `pfs_dag_save` and read with
  `pfs_dag_read` — the **same** durable, versioned, P1-replicated store G22 uses for
  weight blobs. NOTE the constraint `PFS_REF_MAX`=8 named objects per node: keep the
  engram ring to a *few* named blocks (e.g. one block holding a fixed-size array of
  `B_RING` engrams), not one ref per engram.
- **How replay samples it.** During rest, sample a minibatch from the ring and
  interleave it into SGD. Sampling may be uniform or **salience-weighted** — and the
  salience-weighting mechanism *already exists*: reuse `gl_build_weighted` /
  `reflex_threat_experience` (G38 arrow-2) so danger-class engrams are rehearsed more
  (the DMN "imagination/threat rehearsal" function, I.3).
- **Bound (the honesty knob, see II.6).** The ring holds `B_RING` engrams **per
  task** with `B_RING ≪ |task data|`. This is what makes it *consolidation*
  (compression) and not "store everything + joint retrain" (which would be trivial).

### II.4 Fast layer vs slow layer (wiring)

- **Fast layer (in-context, R3 substrate).** The R3 path (`r3_incontext.c`) holds
  the current conversation/context with **zero weight update** — stability for free.
  In the first slice the fast layer is *cited and reused as-is*; the falsifiable
  certificate centers on the slow layer (consolidation). The fast→slow handoff is
  described here but not the thing measured (honest scoping, II.6).
- **Slow layer (consolidated weights).** The 635-param `dtr` body. Changed **only**
  during DMN rest, via replay-SGD + gossip merge. This is the layer the forgetting
  test measures.

### II.5 The falsifiable acceptance test (the certificate)

All emitted as printed numbers **then** a canonical `[tag] PASS/FAIL` line, the R3 /
g22 way (numbers are the honest evidence; the verdict greps in CI). `chance = 33.3%`
(3 classes). Thresholds below are **proposed bars**; the implementer reports the
**actual** measured numbers (like R3's "report the actual number").

1. **`[dmn-forgetting]` — the disease is real (precondition).**
   Train the stream task-by-task **WITHOUT** replay. Record task-0 accuracy *right
   after* training task 0 (`acc0_fresh`, expect ≥ chance+50) and *after the whole
   stream* (`acc0_end`). PASS requires the drop to be large **and** the end-state
   collapsed:
   - `acc0_fresh − acc0_end ≥ 25 pts` (forgetting actually happened), AND
   - `acc0_end ≤ chance + 15` (it really collapsed toward chance).
   If this FAILS, the tasks don't conflict and the whole certificate is vacuous —
   fix the task generator, do not weaken the bar.

2. **`[dmn-consolidated]` — replay cures it (the headline).**
   Train the same stream **WITH** DMN replay-consolidation (interleave engrams of
   earlier tasks while learning later ones). PASS requires all three:
   - `acc0_end_replay ≥ chance + 30` (early task retained), AND
   - `acc0_end_replay − acc0_end_noreplay ≥ +25 pts` (the cure beats the disease by
     a wide, non-flaky margin — the analog of R3's +30), AND
   - `acc_lastTask_replay ≥ chance + 30` (plasticity NOT sacrificed: it still learns
     the newest task; otherwise "retention" is the trivial win of never learning).

3. **`[dmn-distributed]` — no central consolidator.**
   The consolidation merge is peer-symmetric. Reuse the **exact** `[g22-no-central]`
   discipline: run `gl_merge` over the consolidated models in node-0 order vs
   reverse order; `|fwd − rev|` must be O(1e-6) rounding, not an O(1) structural
   privilege; single-model merge is identity. PASS = same structural test as G22,
   applied to the consolidated weights.

4. **`[dmn-survive]` — engrams outlive a node.**
   Kill a node mid-stream; its engrams persist as durable, region-replicated p-fs
   blocks. On rejoin/respawn the node reloads them (`pfs_dag_read` /
   `pfs_durable_restore`) and reconsolidates. PASS:
   `acc0_after_rejoin ≥ chance + 25` (the past was reconstructed from p-fs, not RAM).
   (In-process self-test form for CI; the heavier live N≥3 `./p-kernel` + `relay` +
   kill form mirrors the existing `collective-learn-live` job — recommended but
   in-process is the CI gate, as with G22/G23.)

5. **`[dmn-gradcheck]` — the gradients are real.**
   The replay path trains via `dtr_train_batch` (already analytic-backprop and
   already grad-checked for the sensor model). Re-assert it on a **replayed engram**
   with the existing `dtr_grad_check` discipline: max rel err `< 0.05`. This forbids
   a "consolidation" that is secretly a no-op or a fitted artifact.

### II.6 The honest bound (do NOT repeat R3's "by construction" sin)

R3's doc once overclaimed "≤ chance BY CONSTRUCTION" and had to be corrected to a
*measured* bound. We name the trivial-shortcut bounds **explicitly** here:

- **The trivial way to "retain" everything is joint training** (keep all data, retrain
  from scratch each step). That is *not* continual learning. The certificate forbids
  it structurally: the learner only ever streams the **current** task online; the only
  past data it may touch is the **bounded** engram ring (`B_RING ≪ |task data|`,
  compiled-in and printed). Retention achieved with `B_RING` a small fraction of the
  data is the real claim; with `B_RING` ≈ full data the test degenerates to joint
  training and is meaningless — so `B_RING` is a *named, small* constant and the
  ratio `B_RING / |task data|` is **printed**.
- **The honest claim is RETENTION, not "no forgetting."** Replay *reduces*
  forgetting; it does not eliminate it. We claim a numeric retention margin
  (`acc0_end_replay ≥ chance+30` and `≥ noreplay + 25`), **never** "zero forgetting."
- **The disease must be real or the cure is vacuous** — that is exactly why
  `[dmn-forgetting]` is itself a gated precondition with its own threshold (the
  analog of R3 requiring `handif` to actually land near chance).
- **A frozen/random model's task-0 accuracy = chance** is the floor; "retained" must
  beat that floor by the stated margin, measured on **held-out** episodes of task 0
  (fresh draws, never the replayed engrams themselves — the analog of R3's "held-out,
  not training episodes").

No false theorem is asserted. Every bound above is a printed runtime number, not a
belief.

### II.7 Anti-fork constraint (HARD) — exact reuse surface

The implementation **MUST** reuse the existing shared kernels and the G22 gossip
machinery. **NO forked or duplicated math.** The first slice calls:

**Training / eval / weights (from `dtr.h`, the SAME brain R3 and G22 use):**
- `dtr_train_batch` — the one analytic-backprop SGD step (replay-SGD runs *through*
  this; the replayed minibatch is just its `X,y` argument).
- `dtr_eval_batch` — held-out accuracy for every `[dmn-*]` number.
- `dtr_reinit_weights` — fresh/unlearned baselines (shared seed `GL_INIT_SEED`-style).
- `dtr_weights_get` / `dtr_weights_set` — swap weight sets to simulate N nodes
  in-process (the exact G22 pattern).
- `dtr_forward_probs` — if a per-input probability is needed (salience / guard tie-in).
- `dtr_grad_check` — the `[dmn-gradcheck]` token.
- Underlying kernels stay shared if any custom forward is ever needed (it should not
  be, since `dtr_train_batch` is called directly): `dt_linear`, `dt_softmax`,
  `dt_relu`, `dt_sqrt`, `dtr_ln_fwd_cache`, `dtr_ln_bwd`, `dtr_logf`/`dtr_expf`.

**Decentralized distillation (from `gossip_learn.h`, the G22 machinery):**
- `gl_merge` — no-central weight averaging (the consolidation merge IS this).
- `gl_accumulate` / `gl_scale` — the merge primitives, if a weighted distill is used.
- `gl_pfs_publish` / `gl_pfs_fetch` — publish/fetch a model body over p-fs.
- Reuse the **pattern** of `gl_fold_cached_peers` / `gl_run_gossip` for the live
  multi-node consolidation loop (do not re-implement the merge loop — factor/extend).

**Salience / threat-rehearsal (from `reflex.h`, G38 arrow-2, optional but in-scope):**
- `gl_build_weighted` (oversample by class) and `reflex_threat_experience` — to make
  replay salience-weighted (the DMN "imagination" function) without new math.

**Durable engram store (from `pfs_dag.h`):**
- `pfs_dag_save` / `pfs_dag_read` — content-addressed, versioned, P1-replicated
  engram blocks; `pfs_durable_restore` for the `[dmn-survive]` reload.

**The DMN organ (from `dmn.h`, EXTEND not fork):**
- Extend `dmn_idle_work()` (in `dmn.c`) so the live idle path *also* runs a
  consolidation round when engrams are pending — alongside the existing `ga_step()`
  call, not replacing the organ.

Recommended file layout: put the **algorithm + self-test** in a new
`arch/common/lm_consolidate.c` (keeps `dmn.c` the scheduler, not the algorithm);
call it from `dmn_idle_work()` (live) and from the new shell verb (CI).

### II.8 Distribution & death

- **Decentralized, no central buffer.** Each node holds **its own** engrams in p-fs.
  Consolidation = local replay-SGD on local engrams → `gl_merge` of the resulting
  weight bodies with peers (no aggregator). This is byte-for-byte the G22 topology;
  only the *training data* changed from a static shard to replayed engrams.
- **Survives kill + rejoin.** Engrams are durable content-addressed blocks,
  region-replicated by P1 (`pfs_repl`). A killed node's engrams remain reachable; a
  respawned/rejoining node restores them and reconsolidates — mirroring the existing
  G22 "survives kill+rejoin" live demo (`32_collective_learn` /
  `collective-learn-live`).

### II.9 CI integration plan (specify only — do NOT edit ci.yml)

The next wave should:

1. Add a shell verb. Recommended: **`dmn test`** (extends the existing `dmn` verb,
   which today only does `dmn_stat`), dispatching into `lm_consolidate.c`'s self-test.
   (`lm test` is an acceptable alternative if the team prefers a fresh namespace; the
   bracket tags below use `dmn-` regardless.)
2. Emit these bracket-tagged lines (PASS/FAIL), greppable like the rest of the suite:
   - `[dmn-forgetting] PASS`     — the disease is real (II.5 #1)
   - `[dmn-consolidated] PASS`   — replay cures it (II.5 #2, the headline)
   - `[dmn-distributed] PASS`    — no central consolidator (II.5 #3)
   - `[dmn-survive] PASS`        — engrams outlive a node (II.5 #4)
   - `[dmn-gradcheck] PASS`      — real gradients on the replay path (II.5 #5)
3. Wire `dmn test` into the stdin verb sequence in `ci.yml` (the existing line that
   runs `... r3 test\nexit\n` — add `dmn test` before `exit`) and add five
   `grep -aF '[dmn-*] PASS' selftest.log` lines next to the R3 block.
   **This document does NOT modify `ci.yml`; the implementer wave does.**

### II.10 What this does NOT prove yet (honesty)

- **Not real language / not a tokenizer.** Episodes are the synthetic sensor task,
  same as R3 and G22. "Learns from conversation" is demonstrated only in the
  *structural* sense (a stream of unlabelled-in-the-wild episodes consolidated
  offline), not on natural language. Real text + a label-free objective is future.
- **Not the Evolution layer.** Only **weights** consolidate; the architecture
  (`DTR_EMBED_DIM` etc.) is frozen. Versioned-schema migration (I.4) is explicitly
  out of scope.
- **Toy scale.** 635-param body, `T`≈3 tasks, an engram ring of a few dozen — the
  `dtr.h` scale, by design (this is a capacity certificate, like R3, not a product).
- **Rehearsal, not the fancy methods.** The mechanism is **experience replay** of
  stored engrams — the simplest honest DMN analog. It is *not* generative replay, and
  *not* synaptic-importance methods (EWC and friends). We claim replay works at this
  scale; we do not claim it is the best anti-forgetting method.
- **Not a live-sensor swap.** Like R3, this is a CI-enforced **capacity certificate**
  for the substrate. The live thermostat/reflex path is unchanged. The conversational
  mind is built *on top of* this certificate, next.
- **Fast→slow handoff is described, not measured.** The certificate measures
  slow-layer retention (the consolidation = sleep). The fast layer (R3) is reused and
  cited; a falsifiable test of the live fast→slow conversational handoff is a later
  slice.

---

## Part III — the Self layer (first slice): a distributed autobiographical self

> Status: **design + acceptance test** (written before implementation, like Part II /
> R3). Owner of *this* slice: the next wave (separate implementer + separate auditor).
> Builds ON: **G24** (durable content-addressed blocks), **P2 pfs_dag** (the manifest
> hash-chain), **G22** (no-central gossip), **LM-1 / DMN** (the `LM_ENGRAM` episode
> unit). All four are closed in `gap-ledger.md`, so this is unblocked.

The 5-layer worldview is Body / Brain / Self / Collective / Evolution. The DMN slice
(Part II) built the **consolidation** arm of the Default Mode Network (I.3, row 1). The
DMN's **second** function — *self-referential / autobiographical thought* (I.3, row 2,
"the distributed Self layer — the node's autobiographical engram history + its own
model version as a p-fs object") — is the **least-built layer of the five**. This Part
specs its first FALSIFIABLE slice. It mirrors Part II's rigor exactly: a claim, a
certificate with numeric/boolean thresholds and bracket tags, a named honest bound (no
false "by construction" theorem), a HARD anti-fork constraint naming exact existing
functions, an explicit "what this does NOT prove yet," and a CI verb plan.

### III.1 The claim to prove

> A **distributed autobiographical self** — a per-node, hash-chained NARRATIVE LINEAGE
> (an ordered record of the node's own experiences + its own model versions) that
> (1) **survives the node's DEATH** (reconstructs from the durable/gossiped store, not
> RAM), (2) is **TAMPER-EVIDENT** (any alteration of a committed entry is detectable —
> a forged/edited history fails closed), (3) **reconstructs from ANY peer subset with
> NO central owner** (kill the origin, the self still serves), and (4) is **CONTINUED
> by a successor** (after a respawn the lineage links forward — the identity persists
> *through* death, not merely the bytes).

This is "ownerless + never-dies identity" made falsifiable. The self is not a blob that
happens to survive (that is G24); it is an *ordered, origin-stamped, hash-linked thread
of a particular node's life* that no one owns and that cannot be silently rewritten.

### III.2 What this is NOT — the precise G24 / G22 delta (read this first)

This slice is **not durable blobs again** and **not collective weights again**. State
the delta explicitly so the auditor can hold the line:

| Closed gap | What it proves | What it does NOT give — and this slice adds |
|---|---|---|
| **G24** (ARK FS / durable blocks) | *bytes* survive power loss; content-addressed; crash-safe | no ORDER, no ORIGIN identity, no continuity-through-death, no accept/reject identity semantics. G24 makes a byte immortal; it never says *whose life, in what order*. |
| **P2 `pfs_dag`** (manifest hash-chain) | a NAME has a versioned history; `{prev,content,seq,origin}`; forks preserved; LWW ref gossip | it is the generic substrate. It is not *used as an identity*: no "reconstruct excluding the origin" claim, no "a tampered version is rejected as a forged self" claim, no successor-links-forward-through-death claim. This slice puts **identity semantics** ON pfs_dag. |
| **G22** (collective weights) | the SHARED brain converges, no-central | it is the *Collective* layer (one brain across nodes). The Self layer is the orthogonal axis: the *per-node thread* — WHICH node lived WHICH experiences and ran WHICH model versions. |

**One-line delta:** G24 makes *bytes* immortal and G22 makes the *collective brain*
converge; the Self layer makes an *identity* — an origin-stamped, ordered, hash-chained
narrative of experiences + model versions — **immortal, ownerless, and tamper-evident**,
reusing both as substrate and adding the identity semantics neither has.

### III.3 The self-record data structure (built ON pfs_dag — NO new hash)

The lineage is **one p-fs named object**, `self/lin` (8 chars ≤ `PFS_NAME_MAX`=16,
costs ONE of the `PFS_REF_MAX`=8 named-object slots). Each *self-narrative version* is
one `pfs_dag_save("self/lin", …)`; the pfs_dag manifest chain (`prev-manifest-id`) is
the lineage at the manifest level — durability, region replication, LWW-ref gossip and
fork-preservation come **for free** from P2/P1. The version *content* is a packed entry:

```c
/* one self-narrative version. Fixed-width / packed / _Static_assert'd,
 * per the LP64 wire discipline (feedback_lp64_typedef_trap). ~112 B,
 * one entry fits one p-fs block easily. */
typedef struct {
    U4  magic;                  /* LM_SELF_MAGIC                              */
    U4  version;                /* LM_SELF_VER                               */
    U1  self_id;                /* ORIGIN node identity (drpc_my_node)        */
    U1  _pad0; U2 _pad1;
    U4  seq;                    /* 1-based position on the chain              */
    U4  age_ms;                 /* coarse timestamp (tk tick / get_otm)       */
    U1  prev_entry[PFS_ID_LEN]; /* content-id of prev LM_SELF_ENTRY (chain)   */
    U1  eng_digest[PFS_ID_LEN]; /* sha256 of this period's LM_ENGRAM ring img */
    U1  model_ver [PFS_ID_LEN]; /* content-id of current "dtr/weights" blob   */
} __attribute__((packed)) LM_SELF_ENTRY;   /* genesis: prev_entry = all-zero */
```

- **`self_id`** = the origin node (`drpc_my_node`, the same id the `WORLD_BEACON` and
  SWIM table use) — the *whose-life* stamp, carried down the whole chain.
- **`eng_digest`** = `pfs_id_compute()` over the packed `LM_ENGRAM` ring image for the
  period (reuse Part II's episode unit and the existing sha256 content-address path —
  **no new hash**). It records *which experiences* this version summarizes.
- **`model_ver`** = the content-id of the node's current `dtr/weights` blob (reuse the
  **genome** versioned-object pattern: `GENOME_WEIGHTS_REF` = `"dtr/weights"`). It is
  the "own model version" of I.3's Self row — the self knows which brain it was running.
- **`prev_entry`** = the content-id of the previous `LM_SELF_ENTRY`, computed with
  `pfs_id_compute`. This is the **content-level** chain link (see III.4 for why it is
  duplicated alongside the manifest-level link).

**Build / read / walk — exact pfs_dag calls (all verified present):**

- *append a version*: `pfs_dag_save((UB*)"self/lin", 8, &entry, sizeof entry)` — creates
  the content block + a manifest whose `prev` is the previous head; the ref bumps; P1
  replicates. (`pfs_dag_save` internally uses `pfs_repl_put`, so region replication is
  automatic.)
- *read the head version*: `pfs_dag_read((UB*)"self/lin", 8, &entry, sizeof entry)`.
- *walk back to verify the chain*: `pfs_get(entry.prev_entry, &prev, sizeof prev)` then
  `pfs_id_compute(&prev, sizeof prev, id)` and require `id == entry.prev_entry`; repeat
  to genesis (`prev_entry` all-zero).
- *restore after death*: `pfs_dag_restore()` (reloads the ref table) **after**
  `pfs_durable_restore()` (reloads the content+manifest blocks); then `pfs_dag_read`
  returns the head and the walk above replays the chain.

### III.4 IMPORTANT — there is NO public manifest-prev-chain walker (commander, decide)

The pfs_dag manifest chain has the link we want (`prev-manifest-id`), but **the walk is
not exposed**: `load_manifest()`, `dag_log()`, `dag_cat()` are all `static` in
`pfs_dag.c`, and `pfs_dag_read()` returns **only the HEAD content**. There is no public
"read version @seq into a buffer" or "walk prev" accessor that returns data (the shell
`pfs log/cat @seq` only *prints*). A prior wave shipped a wrong assumption an auditor
had to catch; this is flagged so the commander corrects the plan **before** coding.

Two honest options:

- **(Recommended) walk at the CONTENT level.** Embed `prev_entry` (a content-id, via
  `pfs_id_compute`) inside `LM_SELF_ENTRY` as above and walk with `pfs_get` +
  `pfs_id_compute`. This needs **no pfs_dag API change**, makes tamper-evidence
  self-contained (recompute each entry's id, compare to the successor's stored
  `prev_entry`), and still rides the manifest chain for durability/gossip/fork-preserve.
- **(Alternative) extend pfs_dag.** Add one small *public* accessor —
  `pfs_dag_read_at(name, seq, buf, max)` or `pfs_dag_walk(name, cb)` — promoting the
  existing static walk. This is an EXTENSION (not a fork) but touches shared P2 code and
  needs its own audit. The recommended option avoids it.

The certificate below assumes the **content-level walk**, so it runs with zero changes
to `pfs_dag.c`.

### III.5 The falsifiable acceptance test (the certificate)

In-process self-test form (the aarch64-PRoot HOST crashes on cross-node LIVE p-fs
tests, so — as with G22 / G23 / DMN — the CI gate is in-process; an N≥3 live form is a
recommended extra, not the gate). N (chain length) small and compiled-in (e.g. `N=8`).
Each check prints its evidence then a canonical `[self-*] PASS/FAIL` line (greppable).

1. **`[self-continuity]` — the identity survives death and continues.**
   Build a lineage of `N` entries on one node. **Drop RAM**: clear the in-RAM ref table
   + engram ring (force reconstruction from the persisted store only). Reconstruct via
   `pfs_dag_restore()` + `pfs_durable_restore()`; walk the chain. PASS requires ALL:
   - the reconstructed chain has length `N` (`recovered_len == N`, printed), AND
   - it equals the pre-death chain **hash-for-hash** (every entry's recomputed
     `pfs_id_compute` matches the `prev_entry` its successor stored, and the recovered
     head id == the pre-death head id) — boolean, AND
   - a NEW entry appends and links forward: `seq == N+1`, its `prev_entry` == the
     restored head id (the **successor continues the identity through death**) — boolean.

2. **`[self-tamperevident]` — a forged/edited self is detected, FAIL-CLOSED.**
   Take a valid chain; flip ONE byte in one entry's stored bytes (or splice a forged
   entry with a wrong `prev_entry`). Re-walk. PASS requires BOTH:
   - the break is DETECTED — `pfs_id_compute(tampered) != prev_entry` held by its
     successor (or the head id mismatches), and the walker **rejects** the chain
     (default verdict on a non-verifying chain is REJECT — fail closed), AND
   - a clean chain still verifies (no false positive).
   This is the non-trivial teeth: a committed self cannot be silently altered. (Honest
   bound on its REACH: III.6.)

3. **`[self-ownerless]` — reconstruct from a peer subset that EXCLUDES the origin.**
   Simulate ≥2 nodes' block stores in-process (the G22 in-process pattern). The origin's
   content+manifest blocks were replicated to a peer (P1 region replication — this is
   what `pfs_dag_save`/`pfs_repl_put` already do). **Empty the origin's store entirely**;
   reconstruct the head + the full length-`N` chain, hash-for-hash, from the peer's
   blocks ONLY. PASS = boolean: full chain served with the origin store empty (kill the
   origin, the identity is still served — no central owner).

4. **(optional) `[self-other]` — lineages are distinguishable by origin.**
   Build two lineages with different `self_id`; assert each entry down each chain carries
   its own `self_id` and the two heads are distinct. **Stated plainly: this is
   near-trivial** — it asserts an origin STAMP is present and consistent, not any learned
   or semantic distinctiveness. Include it only as a cheap consistency guard; it is NOT
   evidence of a "self-model." (Drop it rather than oversell it.)

### III.6 The honest bound (do NOT assert a false theorem)

Name each trivial shortcut and how the test structurally forbids it:

- **"keep it all in RAM" = not survival.** Forbidden by `[self-continuity]`, which
  **drops RAM** (clears in-RAM refs + ring) and reconstructs from the persisted store
  only. A RAM-only design recovers length 0 and FAILS.
- **"trust the origin" = not ownerless.** Forbidden by `[self-ownerless]`, which
  **empties the origin store** and rebuilds from a peer subset. An origin-anchored
  design serves nothing and FAILS.
- **"no hash" = not tamper-evident.** Forbidden by `[self-tamperevident]`: a one-byte
  flip must be DETECTED via the content-address (`pfs_id_compute`) chain, and the walker
  must FAIL CLOSED. A no-hash / trust-the-bytes design cannot detect it and FAILS.

**What is真 (claimed):** an *identity* — an ordered, origin-stamped, hash-chained record
of a node's own experiences (`eng_digest`) and model versions (`model_ver`) — that
**survives the node's death, is ownerless (reconstructs without the origin), and is
tamper-evident (any alteration of a committed entry is detectable, fail-closed), and is
continued by a successor.**

**What is NOT claimed (no overclaim):**

- **Structural narrative, NOT a learned/semantic self-model.** The chain records *which*
  experiences and *which* brain version, in order — it does not mean the node
  *understands* itself. This is **not introspection and not consciousness.** A
  learned/semantic self-model (the node reasons about its own history) is a later slice.
- **Tamper-EVIDENT, NOT tamper-PROOF / unforgeable.** There is **no signature primitive**
  in the tree (genome.h states the same limit verbatim: "there is no signature /
  verification on the manifest or its entries"). So a malicious node that controls its
  own store can author a *fresh, internally-consistent* fake lineage from genesis. The
  teeth we DO claim: you cannot **alter or splice an already-committed entry** without
  the content-address chain breaking, and reconstruction needs no owner. Per-manifest
  **signatures** (a node keypair) to get true unforgeability/Sybil-resistance are named
  here and explicitly **deferred** to a later slice.
- **Toy scale / not real language.** `eng_digest` summarizes the synthetic-sensor
  `LM_ENGRAM` episodes of Part II, not natural-language autobiography. Same scope caveat
  as Part II and R3: this is a capacity certificate for the substrate, not a product.

No false "by construction" theorem is asserted. Every PASS bound above is a printed
runtime boolean/number, not a belief (the R3/DMN discipline).

### III.7 Anti-fork constraint (HARD) — exact reuse surface

The implementation **MUST** reuse the existing substrate. **NO new hash, NO forked
merkle/crypto, NO duplicated gossip loop.** Verified-present functions the implementer
calls (all confirmed by reading the headers):

**Content-address / hash-chain (the ONE sha256 path — `pfs_block.h`):**
- `pfs_id_compute(buf, len, id_out)` — THE sha256 content-address; every chain link
  (`prev_entry`, `eng_digest`, `model_ver`) is this. Do **not** fork crypto.
- `pfs_get(id, buf, max)` — fetch a content block by id (the in-process chain walk).
- `pfs_has(id)` — presence check before a walk step.

**Durable, versioned, replicated lineage (`pfs_dag.h` / `pfs_repl.h` / `pfs_block.h`):**
- `pfs_dag_save(name, nlen, buf, len)` — append a self-narrative version (manifest
  chain + content block; replicates via `pfs_repl_put` for free).
- `pfs_dag_read(name, nlen, buf, max)` — read the HEAD self-entry.
- `pfs_dag_restore()` — reload the ref table after death.
- `pfs_durable_restore(emit)` — reload content+manifest blocks after death (call FIRST).
- `pfs_repl_put` / `pfs_repl_want` — block put + pull (used indirectly via pfs_dag).

**Identity stamp + distribution (`world.h` / `swim.h` / `drpc.h`):**
- `drpc_my_node` — the `self_id` stamp (same id as `WORLD_BEACON.node_id` / SWIM).
- World self-beacon path (`WORLD_BEACON_TOPIC_PFX` `"world/beacon/<node>"`) +
  P1/P2 ref gossip (`PFSD_TOPIC_REF` `"pfs/ref"`) — the existing distributed-identity
  broadcast; the lineage head rides the ref-gossip already running in `pfs_dag_task`.
  Do **not** add a new gossip loop.

**Genome versioned-object precedent (`genome.h`):**
- `GENOME_WEIGHTS_REF` (`"dtr/weights"`) — the "own model version" object whose
  content-id becomes `model_ver`. Reuse the pattern; do not invent a new weights object.

**Episode unit (`lm_consolidate.h`):**
- `LM_ENGRAM` — the experiences `eng_digest` summarizes. Optionally trigger a
  `self/lin` append from `lm_consolidate_idle_round()` (when a sleep round commits new
  weights, the self gains a new chapter) — reuse the DMN cadence, do not add a timer.

**FLAGGED — names that do NOT exist as public API (commander must resolve, III.4):**
- *No public manifest-prev-chain walker.* `load_manifest` / `dag_log` / `dag_cat` are
  `static`; `pfs_dag_read` returns only the head. → use the content-level walk
  (`pfs_get` + `pfs_id_compute`), OR add a public `pfs_dag_read_at` / `pfs_dag_walk`
  (an extension needing its own audit). The cert assumes the content-level walk.
- *No signature / keypair primitive exists* (confirmed: genome.h states it). → tamper-
  EVIDENT only; signatures deferred (III.6).

### III.8 Distribution & death

- **Decentralized, no central owner.** Each node owns its `self/lin` and saves it with
  `pfs_dag_save`; the manifest + content blocks are content-addressed and P1-replicated
  region-wide, and the head ref gossips on the existing `"pfs/ref"` topic. No aggregator,
  no coordinator — byte-for-byte the topology G24/P2 already run.
- **Reconstruct without the origin.** Because every block is content-addressed and
  replicated, a peer that holds the replicated blocks can serve the full chain after the
  origin is gone — exactly what `[self-ownerless]` exercises in-process and what an N≥3
  live run (`./p-kernel` + `relay`, kill the origin) would show.
- **Continuity through death.** A respawn restores the chain (`pfs_dag_restore` +
  `pfs_durable_restore`) and appends a new entry whose `prev_entry` is the restored head
  — the lineage links forward across the death boundary (`[self-continuity]` clause 3).

### III.9 CI verb plan (specify only — do NOT edit ci.yml)

The next wave should:

1. Add a shell verb **`self test`** dispatching into a new `arch/common/lm_self.c`'s
   `lm_self_test()` (keep `lm_consolidate.c` the DMN-consolidation module; the Self
   layer is a sibling). Register it in `arch/x86/shell.c`'s command table (mirroring
   `cmd_dmn` → `lm_test` at `shell.c:488`) and the equivalent in the three
   `usermain.c`. (`lm self` is an acceptable alternative namespace; tags stay `self-`.)
2. Emit these bracket-tagged lines (greppable like the rest of the suite):
   - `[self-continuity] PASS`      — survives death + continues (III.5 #1)
   - `[self-tamperevident] PASS`   — a forged/edited self is detected, fail-closed (#2)
   - `[self-ownerless] PASS`       — reconstruct excluding the origin (#3)
   - `[self-other] PASS`           — *optional*, origin-distinguishable (#4; near-trivial)
3. Wire `self test` into the **native** stdin verb sequence at `ci.yml:57` (after
   `dmn test`, before `exit`) and add the matching `grep -aF '[self-*] PASS'
   selftest.log` lines next to the `[dmn-*]` block (`ci.yml:142-146`). Note: the
   aarch64-qemu sequence (`ci.yml:172`) is a SUBSET that already omits `dmn test` /
   `r3 test`, so `self test` goes only on the native line — consistent with DMN.
   **This document does NOT modify `ci.yml`; the implementer wave does.**

### III.10 Provenance / closes-on (Self layer)

Design only. This slice closes when `[self-continuity]`, `[self-tamperevident]`,
`[self-ownerless]` (and, if kept, `[self-other]`) are green on a clean rebuild AND
CI-enforced (all native targets), audited by a **separate** agent on the **commander's**
binary — not the implementer's. Per `gap-ledger.md` discipline: when it ships and is
CI-enforced it earns ONE epitaph line; it does not lengthen the ledger and spawns no
`philosophy-gap-audit-9`. The audit makes the acceptance test; the commander reads the
gate formula line-by-line.

---

### Provenance / closes-on

This is design only. The first slice closes when `[dmn-forgetting]`,
`[dmn-consolidated]`, `[dmn-distributed]`, `[dmn-survive]`, `[dmn-gradcheck]` are all
green on a clean rebuild AND CI-enforced (all targets), audited by a separate agent
on the commander's binary — not the implementer's. Per `gap-ledger.md` discipline:
when it ships and is CI-enforced, it earns ONE epitaph line; it does not lengthen the
ledger. No `philosophy-gap-audit-9`. The audit makes the acceptance test; the
commander reads the gate formula line-by-line.
