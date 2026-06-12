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

---

## Part IV — salience-weighted replay (the DMN's imagination)

> Status: **design + acceptance test** (written before implementation, like Part II /
> III / R3). Owner of *this* slice: the next wave (separate implementer + separate
> auditor). Builds ON: **LM-1 / DMN** (Part II, `arch/common/lm_consolidate.c`, **closed**
> — the engram ring + uniform replay + the held-out generator), **G38** (`reflex.c`
> arrow-2, `reflex_threat_experience`, **closed**), **G22** (`gl_build_weighted`
> class-oversample *pattern*, **closed**). All three closed in `gap-ledger.md`, so this is
> unblocked.

The DMN's third function — *future imagination / threat rehearsal* (I.3, row 3) — is the
last of the three to be specced. LM-1 deliberately shipped **uniform** replay and wrote
the honest reason verbatim in `lm_ring_capture` (`lm_consolidate.c` ~ln 213): *"Salience-
weighted replay (II.3) is OPTIONAL … in this self-contained certificate the live reflex
has met no danger yet, so we do honest UNIFORM replay and set salience=1."* This Part
makes replay **salience-weighted**: during DMN sleep, the danger class — the one the
**reflex actually guarded** — is rehearsed MORE, turning consolidation from a passive
even-handed rewind into a **proactive, survival-biased** rewind. It mirrors Part II/III's
rigor exactly: a claim stated as a tradeoff, a falsifiable certificate with numeric
thresholds + bracket tags, a named honest bound (no false "by construction" theorem), a
HARD anti-fork surface naming exact functions, an explicit "what this does NOT prove,"
and a CI verb plan.

### IV.1 The claim to prove

> Under a **FIXED replay budget** (`B_RING` unchanged), weighting the engram ring by
> **EARNED salience** — the per-class guard experience `reflex_threat_experience(cls)`
> that accrued from the reflex actually firing on a danger class — **retains the
> high-salience (danger) class better than uniform replay at the SAME budget.** This is a
> real **reallocation** of fixed replay capacity toward survival-critical memory.

Stated as a **tradeoff, not a free lunch** (IV.6): the danger class goes up *because* the
fixed slots stolen from the low-salience class make its retention go down (or no better).
The headline is the *net survival-favoring* reallocation, measured on held-out, never the
replayed engrams.

The honesty crux — and the by-construction trap this Part exists to avoid — is that
salience must be **EARNED from real reflex experience, not hand-set.** The analog of R3's
"`handif` must land near chance" and DMN's "`[dmn-forgetting]` must be real": if salience
is a hard-coded weight, the certificate proves nothing. So the cert **drives real reflex
guard firings** on a chosen danger class through the public inference hook, shows the
experience **accrued** in `guard_class_exp` (read back via `reflex_threat_experience`),
and shows the ring's `salience` field **derives from that count** (not the constant `1`).

### IV.2 What changes vs LM-1 (the precise delta)

LM-1's `lm_ring_capture(t)` walks the per-task train set with a uniform stride and writes
`B_RING` engrams with `salience = 1`. The classes land round-robin (`lm_ds_init` generates
episodes `i % LM_NCLASS`), so the ring is ~balanced 1/3 per class. The slow weights are
distilled by interleaving earlier tasks' rings into later tasks' SGD (`lm_train_task`).

Part IV changes **only the capture allocation**, not the ring size, not the SGD path, not
the merge:

- **Earn a per-class weight.** `classw[c]` is derived from `reflex_threat_experience(c)`
  using the **same clamp formula** G22 already uses (IV.7) — `classw[c] = 1 + (exp[c]·
  (WMAX−1) + mx − 1)/mx`, clamped `1..WMAX`, where `mx = max_c exp[c]`. With **no earned
  experience** every `classw[c] == 1` → identical to LM-1 uniform (the no-regress hinge,
  IV.5 #3).
- **Reallocate the FIXED `B_RING` slots by class weight.** The ring still holds **exactly
  `B_RING` engrams per task** (budget unchanged — *not* a bigger ring). Salience changes
  *which* engrams occupy the slots: the danger class gets a larger share, the low-salience
  class a smaller share, in proportion to `classw`. `salience` per engram is set to its
  class weight (a real number from the reflex, not `1`).
- **Replay is otherwise byte-identical.** `lm_train_task` / `lm_reconsolidate_from_engrams`
  / `lm_consolidate_idle_round` consume the ring exactly as today; they never learn that
  the allocation changed. (This is why the no-regress hinge is clean.)

> **Design decision — reallocate the fixed slots; do NOT inflate the minibatch.** There
> are two ways to "oversample by class weight." (a) *Slot reallocation* (chosen): keep the
> ring at `B_RING`, change the class mix of which engrams are stored. (b) *Minibatch
> inflation* (the literal shape of `gl_build_weighted`): keep a balanced ring but `rep`-eat
> danger engrams when building the SGD batch, growing the effective replay count. We pick
> (a) because the **budget must stay fixed** (IV.6) — inflation (b) silently spends more
> replay than uniform and would make any "retention gain" the trivial result of *more*
> rehearsal, not *reallocated* rehearsal. We reuse `gl_build_weighted`'s **weight formula**
> (IV.7), not its inflation loop.

### IV.3 Driving earned salience (the public accrual path — verified)

The cert needs a public way to make `guard_class_exp[danger]` accrue **before** capture.
It exists and is already exercised by a sibling self-test:

- **Accrue:** call `reflex_on_inference(danger_cls, conf, drpc_my_node)` with
  `danger_cls ∈ {1,2}` (`act_table[1]=CONSERVE|BEACON`, `act_table[2]=SHIELD|CONSERVE|
  BEACON`, both non-`NONE`) and `conf ≥ REFLEX_CONF_MIN` (40) or `0xFF`. This passes
  `reflex_would_fire` and hits `guard_class_exp[danger_cls]++` (`reflex.c` ln 240–241,
  gated on `threat_class ≥ 1`). Repeat it `K` times to build a clear gap over the safe
  class. **Precedent:** `reflex_self_test` (`reflex.c` ~ln 904–922) drives
  `reflex_on_inference(2,100,…)` / `(0,100,…)` and **save/restores `guard_class_exp`**
  around the probe — the cert should follow the same save/restore discipline so it does not
  pollute the live G38 experience counters, then *re-drive* its own accrual for the
  measurement.
- **Read back:** `reflex_threat_experience(danger) > reflex_threat_experience(safe)` —
  printed; this is the EARNED evidence, not a literal.
- **Side-effects to manage (honest note):** `reflex_on_inference` also moves
  `danger_streak`, `threat_run`, sets `danger_active` / `shield_until`, and would
  `emit_beacon` (in-process `h_pub < 0`, so BEACON is a print no-op). After accrual the
  cert should observe SAFE (`reflex_on_inference(0,100,…)`) to release the CONSERVE latch,
  and **must run with `enabled == TRUE`** (the default; `reflex_set_enabled` toggles it).

> Mapping note: the LM task's class index and the reflex threat class share the same
> 3-class space (`DTR_OUT_DIM == REFLEX_NUM_CLASSES == 3`), so "danger = class `d`" is the
> same `d` for the ring and the reflex. See IV.6 for *which* `d` to pick so the win is not
> trivial.

### IV.4 A per-class held-out metric is needed (small extension, flagged)

LM-1 measures **per-task** accuracy only (`lm_acc(t)` → `dtr_eval_batch` over
`lm_tex[t]`/`lm_tey[t]`). Salience is **per-class**, so Part IV needs a **per-class**
held-out accuracy. Add a small helper inside `lm_consolidate.c` (no new organ): either
(a) filter the held-out episodes of class `c` into a sub-array and call the existing
`dtr_eval_batch`, or (b) loop `dtr_forward_probs` + argmax over held-out, counting hits
per class. Both reuse shared kernels only. Measure the danger class **aggregated across
all tasks** (the salience weight is class-global), on **fresh** held-out (`lm_tex`/`lm_tey`),
never the replayed engrams. *This helper does not exist yet — implementer adds it; it is
an extension of LM-1, not a fork.*

### IV.5 The falsifiable acceptance test (the certificate)

All emitted as printed numbers **then** a canonical `[tag] PASS/FAIL` line (the R3 / DMN
way; numbers are the honest evidence, the verdict greps in CI). `chance = 33.3%`.
Thresholds are **proposed bars**; the implementer reports the **actual** measured numbers
and, per the audit-is-the-engine rule, may only *lower* a bar to the measured value minus a
flake margin — never inflate to make green.

1. **`[salience-earned]` — salience is EARNED, not hand-set (the anti-by-construction
   gate; the precondition).**
   Drive `K` real reflex guard firings on the danger class via
   `reflex_on_inference(danger,100,…)` (IV.3). PASS requires ALL:
   - `reflex_threat_experience(danger) > reflex_threat_experience(safe)` — experience
     **accrued from the reflex**, both counts printed, AND
   - the danger class's derived `classw[danger] > 1` while `classw[safe] == 1` (the ring
     weight **derives from** the reflex count, NOT the constant `1`) — printed, AND
   - the captured ring holds **strictly more** danger-class engrams than the uniform ring
     would at the same `B_RING` (`n_danger_salience > n_danger_uniform`, both printed) —
     the reallocation actually happened in the fixed budget.
   If this FAILS, salience is fake and the rest of the certificate is vacuous — fix the
   accrual path, do **not** hand-set the weight.

2. **`[salience-retains]` — earned salience retains the danger class better, at EQUAL
   budget (the headline; a tradeoff, not a free lunch).**
   Run the full LM-1 stream twice from the **same** shared seed and the **same** `B_RING`:
   once with uniform capture, once with salience-weighted capture. Measure per-class
   held-out accuracy (IV.4) on **fresh** episodes. Let
   `dgain = acc_danger_salience − acc_danger_uniform` and
   `sloss = acc_safe_uniform − acc_safe_salience` (sign: positive `sloss` = the safe class
   got worse). PASS requires ALL:
   - `dgain ≥ +5 pts` (the danger class is retained measurably better — a real, non-flaky
     reallocation effect; **not** R3/DMN's +25/+30, which were disease-vs-cure, whereas
     this is a subtler within-budget reallocation — report the actual number), AND
   - the reallocation is **net survival-favoring**: `dgain ≥ sloss` (the danger gain is at
     least the non-danger loss — honest, since slots are conserved), AND
   - macro (mean per-class) held-out does **not collapse**: `macro_salience ≥ macro_uniform
     − ε` with `ε = 3 pts` (the tradeoff stays bounded; salience does not wreck the whole
     model to pump one class).
   **Both `dgain` and `sloss` are printed** so the tradeoff is visible, not hidden behind a
   one-sided number. (If `sloss ≤ 0`, say so plainly — it would be a *near* free lunch at
   this toy scale, which is fine to report but not what we claim.)

3. **(optional) `[salience-noregress]` — the uniform DMN path is unchanged.**
   With `guard_class_exp` all-zero (no earned experience), assert every `classw[c] == 1`
   and the salience capture is **byte-identical** to LM-1 uniform capture (same engrams in
   the same slots). PASS = boolean. This is the safety hinge that keeps all `[dmn-*]` green
   when no danger has been met (the honest default: no experience ⇒ uniform).

### IV.6 The honest bound (do NOT assert a false theorem)

- **It is a REALLOCATION of a fixed budget → a tradeoff, NOT free retention.** Danger up,
  low-salience possibly down. The budget knob `B_RING` is **unchanged** and printed; the
  ring is **not** enlarged and the SGD is **not** given more replay (that is exactly why we
  reallocate slots rather than inflate the minibatch, IV.2). Anyone who "improves
  retention" by spending more replay has changed the budget and proved nothing.
- **Salience must be EARNED.** Hand-setting class weights is the by-construction sin (the
  R3/DMN/Self lesson). `[salience-earned]` (IV.5 #1) is a gated precondition exactly so the
  weight cannot be a literal: it must trace to `guard_class_exp` accrued by real
  `reflex_on_inference` firings.
- **Measure on held-out, never the replayed engrams.** Per-class accuracy is on fresh
  `lm_tex`/`lm_tey` draws (the R3 "held-out, not training episodes" discipline).
- **The danger class must NOT be trivially easiest** (else the gain is saturation, not
  retention). LM-1's generator places class centers at `0 / 14 / 28` (+ per-task region
  offset): the **extreme** classes (0, 2) overlap on one side only and are *easier*; the
  **middle** class (1) overlaps on both sides and is the *hardest* (Bayes-error > 0, by
  design). Two honest ways to keep the win real (implementer picks one, reports which):
  **(a)** choose the **middle/hardest class as the danger class** (drive guard firings on
  reflex class 1 = *alert*, whose `act_table[1]` fires), so retaining it better is a
  genuine win on the hard class, **or** **(b)** symmetrize the generator so all three
  classes carry equal Bayes error, then any class may be "danger." Either way the cert
  **prints the danger class's uniform-replay accuracy** and requires it to have **headroom**
  (e.g. `acc_danger_uniform ≤ 90%`) so the claimed `dgain` is real improvement, not noise
  on a saturated metric.
- **No false "by construction" theorem.** Every PASS bound is a printed runtime number, not
  a belief.

### IV.7 Anti-fork constraint (HARD) — exact reuse surface

The implementation **MUST** reuse the existing substrate. **NO forked weighting math, NO
new organ, NO bigger ring.** Verified-present functions the implementer calls:

**Earned salience (from `reflex.h`, G38 arrow-2 — all confirmed public):**
- `reflex_threat_experience(cls)` (`reflex.h:180`, def `reflex.c:382` → `guard_class_exp[cls]`)
  — THE earned-salience source. Read, never recompute.
- `reflex_on_inference(threat_class, confidence, src_node)` (`reflex.h:162`) — the **public
  accrual entry** that increments `guard_class_exp` (IV.3). Confirmed public; precedent
  `reflex_self_test` `reflex.c:907`. Follow its **save/restore of `guard_class_exp`** so the
  cert does not corrupt live G38 counters.
- `reflex_would_fire(cls, conf)` (`reflex.h:173`) — optional, to assert the gate the firing
  passes (so the accrual is the *same* gate G38 / production use).

**The class-weight PATTERN — MIRROR, do NOT call (decision + rationale):**
- `gl_build_weighted(k, classw)` lives at `gossip_learn.c:574` and is **`static`
  (file-private)** — it is **not** in `gossip_learn.h`. It also operates on gossip_learn's
  **shard** arrays (`sh_x`/`sh_y`), not on `LM_ENGRAM` rings, and its body is a
  **minibatch-inflation** loop (`rep`-eat up to `GL_TRAIN·GL_WMAX`) — which would violate
  the fixed-`B_RING` budget if copied. **Decision: MIRROR its class-weight FORMULA, do not
  call it.** Reuse the exact clamp shape used at `gossip_learn.c:635`
  (`classw[c] = 1 + (exp[c]·(WMAX−1) + mx − 1)/mx`, clamp `1..WMAX`) and apply it to
  **fixed-slot reallocation** in `lm_ring_capture`. Rationale: (1) it is private — calling
  it would require promoting it to `gossip_learn.h`, a shared-P2 change needing its own
  audit, against minimal-touch; (2) wrong data source (shards, not engrams); (3) its
  inflation loop breaks the fixed budget. Mirroring the *formula* (a one-liner) is the
  smallest honest reuse. `WMAX` mirrors `GL_WMAX = 3` (`gossip_learn.c:567`, also private)
  as a local `LM_WMAX = 3`.

**Training / eval / weights (from `dtr.h`, the SAME brain LM-1/G22 use):**
- `dtr_train_batch` / `dtr_eval_batch` / `dtr_reinit_weights` / `dtr_weights_get` /
  `dtr_weights_set` — reuse exactly as LM-1 does; the SGD/merge path is untouched.
- `dtr_forward_probs` (`dtr.h:137`) — for the per-class argmax held-out helper (IV.4),
  if option (b) is taken.

**EXTEND `lm_consolidate.c` (do NOT fork):**
- `LM_ENGRAM.salience` (`lm_consolidate.h:37`, UB) — already present; now **derived** from
  the reflex count, not the constant `1`.
- `lm_ring_capture` (`lm_consolidate.c:213`) — the ONE function whose allocation changes
  (uniform stride → class-weighted fixed-slot reallocation). `lm_ds_init` / `lm_gen` /
  `lm_tex`/`lm_tey` / `lm_train_task` / `lm_run_replay` / `lm_run_noreplay` reused as-is.
- `lm_test` (`lm_consolidate.c:391`) — extended to emit the three new tags after the
  `[dmn-*]` block (IV.8).

**FLAGGED — names that are NOT public / do NOT exist (commander resolves before coding):**
- `gl_build_weighted` and `GL_WMAX` are **`static` in `gossip_learn.c`** — not callable
  from `lm_consolidate.c`. → MIRROR the formula (above), do not call. (This is the one the
  prompt asked to confirm.)
- **No per-class held-out accuracy helper exists in LM-1** (only per-task `lm_acc`). →
  implementer adds a small one inside `lm_consolidate.c` (IV.4) — an extension, not a fork.
- The public reflex accrual path **does** exist (`reflex_on_inference`) — *no* missing
  entry to flag here; the G38 arrow-2 wiring is complete.

### IV.8 CI verb plan (specify only — do NOT edit `ci.yml`)

The next wave should:

1. **Extend the existing `dmn test` verb** (`lm_test()` in `lm_consolidate.c`) to also emit
   the salience tags — keep them in the **same** self-test so the salience path is gated on
   the same binary as `[dmn-*]`, and so `[salience-noregress]` can assert the uniform path
   it shares is untouched. (A separate `dmn salience` verb is acceptable; **decision: extend
   `dmn test`**, because the no-regress claim and the uniform/salience comparison are most
   honestly made side-by-side in one run. Tags stay `salience-` regardless.)
2. Emit these bracket-tagged lines (PASS/FAIL), greppable like the rest of the suite:
   - `[salience-earned] PASS`     — salience is earned, not hand-set (IV.5 #1)
   - `[salience-retains] PASS`    — earned salience retains the danger class better at equal
     budget; tradeoff printed (IV.5 #2, the headline)
   - `[salience-noregress] PASS`  — *optional*, uniform DMN path unchanged when no
     experience exists (IV.5 #3)
3. **Wire into the native sequence** at `ci.yml:57` — `dmn test` already runs there, so **no
   sequence edit is needed**; add three matching `grep -aF '[salience-*] PASS' selftest.log`
   lines next to the `[dmn-*]` block (`ci.yml:142-146`). The aarch64-qemu subset
   (`ci.yml:172`) already omits `dmn test`, so the salience tags ride only the native line —
   consistent with DMN. In-process gate (the aarch64-PRoot host crashes on cross-node live
   p-fs). **This document does NOT modify `ci.yml`; the implementer wave does.**

### IV.9 What this does NOT prove yet (honesty)

- **Prioritized experience replay, NOT generative imagination.** This rehearses *stored*
  danger engrams more; it does **not** synthesize counterfactual / not-yet-happened episodes
  (true DMN "future simulation"). Generative replay is a later slice.
- **"Danger" is the synthetic sensor class, not a real threat.** Same scope caveat as Part
  II / R3: a capacity certificate for the substrate, not a product. The reflex firing is
  driven by the cert (`reflex_on_inference`), not by a live thermostat under attack.
- **Toy scale.** 635-param body, `T ≈ 3` tasks, `B_RING ≈ 24` engrams/task, `WMAX = 3`.
- **A tradeoff at toy scale, not a universal anti-forgetting law.** We claim a measurable,
  earned, budget-conserving reallocation toward the danger class — not that salience replay
  dominates uniform on every metric, nor that it is the best prioritization scheme (PER /
  importance-sampling theory is out of scope).
- **Not real language.** Episodes are the LM-1 sensor stream, not natural-language
  experience.

### IV.10 Provenance / closes-on

Design only. This slice closes when `[salience-earned]`, `[salience-retains]` (and, if kept,
`[salience-noregress]`) are green on a clean rebuild AND CI-enforced (native targets),
audited by a **separate** agent on the **commander's** binary — not the implementer's. Per
`gap-ledger.md` discipline: when it ships and is CI-enforced it earns ONE epitaph line; it
does not lengthen the ledger and spawns no new `philosophy-gap-audit`. The audit makes the
acceptance test; the commander reads the gate formula line-by-line.

---

## Part V — the fast→slow handoff (in-context knowledge becomes weights)

> Status: **design + acceptance test** (written before implementation, like Part II / III /
> IV / R3). Owner of *this* slice: the next wave (separate implementer + separate auditor).
> Builds ON: **R3** (`arch/common/r3_incontext.c`, in-context associative recall, **closed**
> — the FAST layer) and **LM-1 / DMN** (`arch/common/lm_consolidate.c`, replay→distill rest-
> time consolidation, **closed** — the SLOW-layer *pattern*). Both closed in `gap-ledger.md`,
> so this is unblocked.

Part II.10 left this as explicit homework: *"Fast→slow handoff is described, not measured.
The certificate measures slow-layer retention. The fast layer (R3) is reused and cited; a
falsifiable test of the live fast→slow conversational handoff is a later slice."* **This Part
makes that handoff falsifiable.** It is the §8 two-layer (I.2 Rung 3) turned into a measured
mechanism: a fact the mind learns ONLY in-context (FAST layer, frozen weights, the fact lives
in the prompt) is **transferred into the WEIGHTS** by DMN sleep-consolidation, so that AFTER
sleep the mind answers it **without the prompt** — the literal mechanism of *随時 会話から学ぶ
that sticks.* It mirrors Parts II/III/IV's rigor exactly: a claim, a falsifiable certificate
with numeric thresholds + bracket tags, a named honest bound (no false "by construction"
theorem), a HARD anti-fork surface naming exact functions (and flagging names that do not
exist), an explicit "what this does NOT prove yet," and a CI verb plan.

### V.0 IMPORTANT — read this BEFORE coding: the two organs are DIFFERENT networks

The wave brief said "wire the two existing organs together: R3 (fast) + lm_consolidate
(slow)." **Reading the code shows that is not literally possible, and the commander must
correct the plan before implementation.** R3 and the DMN consolidate **different neural
networks with different input/output spaces:**

| | FAST organ (R3) | SLOW organ (DMN / lm_consolidate) |
|---|---|---|
| file | `arch/common/r3_incontext.c` | `arch/common/lm_consolidate.c` |
| network | its own Transformer, `rw[R_NP]` (`R_DM=32`, `R_SEQ=9`, key→value recall) | the **dtr sensor body**, `DTR_WEIGHT_FLOATS=635` (`DTR_SEQ_LEN=4` in, 3 classes out) |
| input | 8 dict tokens + 1 query token (key/value vocab) | 4 int8 sensor channels |
| training | its own static `r_backward`/`r_train_epoch` | `dtr_train_batch` (dtr.h) |

`dtr_train_batch` / `dtr_weights_get|set` / `gl_merge` (the lm_consolidate slow path) **train
the dtr sensor body, which has no in-context mechanism at all** — it cannot ingest R3's 9-token
prompt, and R3's recall task cannot be expressed in 4 sensor channels. Feeding R3's in-context
output into `dtr_train_batch` would be a fork of the task, not reuse.

**The faithful handoff therefore lives WITHIN the R3 model.** R3 *is* both layers: in **fast
mode** it answers a query by reading a dictionary from the prompt (frozen `rw[]`, zero weight
change — exactly what R3 already proved); the **slow layer is R3's own weights `rw[]`**,
consolidated by R3's own already-grad-checked `r_backward`. The reuse of "lm_consolidate's
consolidation" is the **DMN cadence/discipline** (engram-replay → distill, the bounded rest-
time round, the held-out eval rule, the `[tag] PASS/FAIL` print convention) — **not** a call
into `lm_test`/`dtr_train_batch` (wrong network). This is the single biggest correction to the
wave framing; it is honest reuse (no new math — R3's forward/backward already exist) and it is
the only construction where "the SAME organ moves a fact from prompt-resident to weight-
resident" is true rather than asserted.

### V.1 The claim to prove

> A fact the mind learned **ONLY in-context** — a fixed key→value dictionary `D*` presented in
> the prompt, answered by R3's FAST layer with **frozen weights** — is **transferred into the
> WEIGHTS** by a DMN-style sleep-consolidation round, such that **after sleep** the mind answers
> `D*` queries **with the prompt support REMOVED** at ≥ chance + margin, while **before sleep
> (or with frozen weights and the support removed) it is at chance.** The handoff is real and
> measured: the only place `D*` ever lived was the in-context computation; consolidation
> distills that computation **into** the weights.

The disease/precondition must be **real** — exactly as R3 makes `handif` land near chance and
DMN makes `[dmn-forgetting]` collapse to chance. Here the precondition is that **weights alone
cannot answer `D*`**: if the frozen R3 weights already answered `D*` with the support removed,
nothing was learned in-context and the whole certificate is vacuous.

### V.2 The task — a fixed dictionary `D*` (one conversation's fact)

R3's generator (`gen_episode`, `r3_incontext.c:123`) **resamples** the dictionary every episode
on purpose, so no single binding persists. The handoff needs the opposite: **one fixed fact-set
to be taught.** Define `D*`: a fixed map of each of the `R_KEYV=8` keys to a value in
`{0..R_VALV-1}` (`R_VALV=4`). `D*` is "what this conversation told the mind" — 8 bindings. The
implementer adds a small **fixed-dictionary** prompt builder (a variant of `gen_episode`,
reusing the identical token layout — NO new math) with three modes:

- **SUPPORT** — the normal R3 prompt: 8 dict tokens carry `D*`'s real bindings, query = a chosen
  key, label = `D*[key]`. This is the FAST layer's input; frozen R3 reads `D*` and answers.
- **MASKED** (support removed) — same query token, but the dict value slots carry `R_UNK` (no
  binding is visible). The only path to the answer is the **weights**. Arrangement (key order,
  query position, distractor padding) varies, so there is a genuine held-out distribution.
- **SCRAMBLED** (the by-construction control, V.5) — SUPPORT-shaped, but the visible dictionary
  is a *different* random `D'` ≠ `D*`. A teacher reading this prompt does NOT know `D*`.

`chance = 100/R_VALV = 25%`. The R3 substrate is first trained to its in-context-competent state
**exactly as `r3_test` already does** (`r_train_epoch` over resampled dictionaries) — `D*` is
just one of the combinatorially many dictionaries, so `D*` is provably **not** baked into the
trained weights (that is what `[handoff-fast-only]` measures).

### V.3 How the in-context (fast) knowledge becomes the slow-layer target (the circularity, honestly)

This is the crux the wave asked to nail. **The teacher is the FAST layer itself** (self-
distillation), and we address the circularity head-on:

1. **Teacher = frozen R3 + SUPPORT prompt.** For each key `k`, run the frozen, in-context-
   competent R3 on the SUPPORT prompt for `(D*, k)`; its argmax is the **teacher label** `ŷ_k`.
   This is the in-context computation, the only place `D*` is readable.
2. **Student = the SAME R3 weights, MASKED prompt.** The consolidation SGD (R3's own
   `r_backward`/update, the **bounded rest-time round** of the DMN pattern) trains `rw[]` on
   `(MASKED prompt for k → ŷ_k)` over the keys of `D*`, fresh arrangements. The weights memorize
   an 8-entry key→value table (an associative memory baked into the embedding/readout).
3. **Eval = MASKED prompt, scored against GROUND-TRUTH `D*`, on a DISJOINT held-out stream.**
   The headline accuracy is **vs the oracle `D*`**, never vs the teacher, on `R_SEED_HELD`
   arrangements that are never among the `R_SEED_TRAIN` consolidation items.

Why this is not circular / not leakage:
- The training signal comes from the **fast layer's in-context output**, not from the oracle and
  not from the eval set. We train on `ŷ_k` (teacher) but **grade on `D*[k]`** (oracle) on a
  separate stream. If the teacher is wrong on some key, the weights memorize the wrong value and
  the oracle-graded eval **shows it** — the test cannot hide a bad teacher.
- The teacher's competence is **bounded by R3's measured in-context accuracy** (the
  `[r3-incontext-learned]` number). We **print teacher-vs-oracle agreement** so the ceiling is
  visible, not assumed.
- The by-construction guard (`[handoff-grounded]`, V.4 #3): consolidating from a **SCRAMBLED**
  teacher (reads `D'`≠`D*`) must give **no** gain on `D*`. Since the architecture has no prior
  toward `D*`, it memorizes whatever the teacher says — so a real gain on `D*` is attributable
  *only* to the fast layer having genuinely read `D*`. This is the analog of "`[dmn-forgetting]`
  must be real" / "salience must be EARNED": it forbids the trap where *any* training pumps the
  metric.

### V.4 The falsifiable acceptance test (the certificate)

All emitted as printed numbers **then** a canonical `[tag] PASS/FAIL` line (the R3 / DMN way;
numbers are the honest evidence, the verdict greps in CI). `chance = 25%`. Thresholds are
**proposed bars**; the implementer reports the **actual** measured numbers and, per the audit-
is-the-engine rule, may only *lower* a bar to the measured value minus a flake margin — never
inflate to make green.

1. **`[handoff-fast-only]` — the fact is FAST-only (precondition / the disease).**
   With the in-context-competent but `D*`-naive frozen R3 weights, print BOTH:
   - SUPPORT accuracy on `D*` queries `acc_support` (in-context recall works), AND
   - MASKED accuracy on the SAME `D*` queries `acc_masked_pre` (weights alone).
   PASS requires ALL:
   - `acc_support ≥ chance + 25` (the fast layer can answer `D*` *with* the prompt), AND
   - `acc_masked_pre ≤ chance + 8` (weights alone are at/near chance — `D*` is NOT yet in the
     weights), AND
   - `acc_support − acc_masked_pre ≥ 25` pts (the knowledge demonstrably lives in the prompt,
     not the weights).
   If this FAILS, either in-context recall is broken or the weights already knew `D*` — fix the
   setup, do **not** weaken the bar.

2. **`[handoff-consolidated]` — sleep moves it into the weights (the headline).**
   Run the DMN-style consolidation round (V.3) distilling the fast layer's `D*` reading into
   `rw[]`. Measure MASKED accuracy on **held-out** `D*` queries (disjoint stream), scored vs the
   oracle `D*`. Let `acc_masked_post` be that number. PASS requires BOTH:
   - `acc_masked_post ≥ chance + 25` (after sleep the mind answers `D*` **without the prompt**),
     AND
   - `acc_masked_post − acc_masked_pre ≥ +20` pts (the gain over the pre-sleep no-prompt baseline
     is large and non-flaky — the handoff happened).

3. **`[handoff-grounded]` — the gain traces to the real in-context reading (the anti-by-
   construction gate).**
   Print teacher-vs-oracle agreement, then run a SCRAMBLED-teacher control. PASS requires ALL:
   - `teacher_agree ≥ chance + 25` (the consolidation targets really are the fast layer reading
     `D*`, not noise — printed), AND
   - the held-out eval is scored **vs oracle `D*`** on a stream **disjoint** from the
     consolidation items (printed: train seed ≠ eval seed; no shared arrangement), AND
   - SCRAMBLED control: after an identical consolidation round whose teacher read `D'`≠`D*`,
     MASKED accuracy vs `D*` stays `acc_masked_scrambled ≤ chance + 8` (a teacher that did not
     read `D*` produces **no** transfer to `D*` — so the headline gain is attributable to the
     genuine in-context computation, not to generic training).
   If this FAILS, the "handoff" could be leakage or a fitted artifact — fix it, do not relax.

(Considered and **dropped** as near-trivial, per the `[self-other]` lesson: a separate
`[handoff-gradcheck]` — the consolidation SGD is R3's own `r_backward`, **already** grad-checked
by `[r3-incontext-gradcheck]` on the same kernels, so a second gradcheck tag would be padding.
We cite the existing R3 gradcheck instead. Also **dropped**: a `[handoff-distributed]` tag —
`gl_merge` is generic over param count and *could* average R3 models, but distributing the
consolidated weights is a separate axis (G22 already owns "no-central"); adding it here would be
sprawl. The handoff is a within-node fast↔slow claim.)

### V.5 The honest bound (do NOT assert a false theorem)

**What is 真 (claimed):** a fact that is **provably answerable only in-context** (`acc_support`
high, `acc_masked_pre` at chance — the *same* frozen weights) becomes **weight-resident** after
a sleep-consolidation round, measured **without the prompt against ground truth** on a held-out
stream, AND a scrambled teacher yields **no** transfer (the gain traces to the genuine in-context
reading). Every PASS bound is a printed runtime number, not a belief.

**What is NOT claimed (no overclaim):**

- **Toy synthetic mapping, NOT natural language.** `D*` is an 8-key→4-value dictionary in R3's
  synthetic vocab — the R3 scope caveat. "Learns from conversation" is shown only structurally
  (a fact held in-context is consolidated into weights), not on natural-language dialogue.
- **Supervised SELF-distillation, NOT unsupervised label discovery.** The consolidation target
  is the **fast layer's own in-context prediction** `ŷ_k` (V.3). We do not discover labels from
  raw text; we distill the in-context *computation* into weights. The training signal comes from
  the fast layer, **not** from the oracle and **not** from the eval set — eval is oracle-graded
  on a disjoint stream so a wrong teacher is exposed, never hidden.
- **The teacher's competence is the ceiling.** A key the fast layer reads wrong is memorized
  wrong; `teacher_agree` is **printed** so this ceiling is visible. The handoff cannot exceed
  what the in-context layer actually knew.
- **"Held-out" is held-out ARRANGEMENT, not held-out FACTS.** `D*`'s 8 bindings ARE the fact
  being taught — you cannot hold out a fact you are deliberately memorizing (that is the nature
  of "learning what you were told"). The falsifiability is **not** generalization to unseen
  facts; it is that the **same** MASKED held-out distribution reads at **chance before sleep**
  and **well above chance after sleep**, scored vs the oracle, with a scrambled-teacher control
  showing no free transfer. Named explicitly so the auditor holds the line.
- **Within ONE frozen-architecture R3 model.** The slow layer is R3's own `rw[]`, not the dtr
  sensor body (V.0). Only weights change; the architecture is frozen (the Evolution layer, I.4,
  stays future).
- **Single fact-set, single sleep — not lifelong.** One `D*`, one consolidation round. A stream
  of conversational facts consolidated over many sleeps (and their interaction with DMN anti-
  forgetting, Part II) is future.
- **CI in-process certificate, not the live `dmn_idle_work` path.** Like R3/DMN, the gate is an
  in-process self-test; wiring the handoff into the live idle hook is future. (The handoff is
  within-node — no cross-node p-fs — so it is even simpler to gate than DMN.)

No false "by construction" theorem is asserted.

### V.6 Anti-fork constraint (HARD) — exact reuse surface (+ FLAGGED names)

The implementation **MUST** reuse R3's existing in-context forward/backward and the shared dtr
kernels. **NO forked math, NO new organ, NO second Transformer.**

**Shared kernels (from `dtr.h`, the SAME math R3 already composes):**
- `dt_linear` / `dt_softmax` / `dt_relu` / `dt_sqrt` / `dtr_ln_fwd_cache` / `dtr_ln_bwd` /
  `dtr_logf` — already used by `r_forward`/`r_backward`; reused transitively, never re-derived.

**R3 fast/slow path (INSIDE `r3_incontext.c` — see the FLAG below):**
- the in-context forward `r_forward` (`r3_incontext.c:143`) — the FAST teacher and the eval.
- the analytic backward + SGD update `r_backward` (`:212`) / the update loop in `r_train_epoch`
  (`:399`) — the SLOW-layer consolidation step (R3's own grad-checked gradients).
- the weight buffer `rw[R_NP]` (`:92`) + `r_init_weights` (`:332`) — the substrate to train to
  in-context competence, snapshot, and restore between the real-teacher and scrambled-teacher
  runs.
- the held-out discipline `R_SEED_TRAIN` / `R_SEED_HELD` (`:474-475`) — disjoint train/eval
  streams, reused verbatim.
- `r_grad_check` (`:437`) / `[r3-incontext-gradcheck]` — cited as the gradient guarantee (V.4).

**DMN slow-layer PATTERN (from `lm_consolidate.c` — mirror the discipline, do NOT call):**
- the engram-replay → distill cadence, the bounded rest-time round
  (`lm_consolidate_idle_round`), the `[tag] PASS/FAIL` print convention, the "measure on held-
  out, never the trained items" rule. Reuse the **shape**; the SGD is R3's, not `dtr_train_batch`.

**FLAGGED — names that do NOT exist as callable API (commander must resolve BEFORE coding):**
- **`r3_incontext.c` is SELF-TEST-ONLY — it exposes NO in-context hook.** Every function
  (`gen_episode`, `r_forward`, `r_backward`, `r_train_epoch`, `r_eval`, `r_init_weights`,
  `r_grad_check`) and both weight arrays (`rw[]`/`rg[]`) are `static`; the only public symbols
  are `r3_cmd` / `r3_test` (`dtr.h:273-274`). → **The handoff self-test MUST live INSIDE
  `r3_incontext.c`** (a sibling to `r3_test`, reaching the statics directly), exposing exactly
  ONE new public entry `r3_handoff_test()` (declared in `dtr.h` next to `r3_test`). No wide
  exposure of internals. The implementer also adds, **inside the file** (small, reusing the
  existing token layout / forward / backward — NO new math): (1) the fixed-`D*` prompt builder
  with SUPPORT/MASKED/SCRAMBLED modes; (2) a 1-episode argmax `predict`; (3) a consolidation
  loop over MASKED prompts with teacher labels; (4) `memcpy` snapshot/restore of `rw[]`.
- **`dtr_train_batch` / `dtr_weights_get|set` / `gl_merge` train the dtr SENSOR body (635
  params, 4-ch input), NOT the R3 model** — they **cannot** serve as the handoff's slow layer
  (V.0). The slow layer is R3's `r_backward`. (This is the framing correction the wave most
  needs.)
- **`LM_ENGRAM` (8 bytes, 4-ch sensor input, `lm_consolidate.h:33`) does NOT fit an R3 recall
  episode** (9 tokens, key/value vocab). Reuse the engram *concept/discipline*, not the struct;
  the handoff keeps its fixed-`D*` episodes in its own in-RAM representation.

### V.7 What this does NOT prove yet (honesty)

- **Not real language / not a tokenizer.** `D*` is the R3 synthetic key→value vocab, same scope
  caveat as R3, DMN, Self, salience.
- **Not the Evolution layer.** Only `rw[]` weights change; the R3 architecture is frozen.
- **Toy scale.** R3's `R_NP`-param Transformer, an 8-key dictionary, 4 values, one `D*`, one
  sleep round — a capacity certificate for the substrate, not a product.
- **One fact-set, one sleep — not a lifelong conversational stream.** Multi-fact, multi-sleep
  consolidation and its interaction with DMN anti-forgetting (Part II) is future.
- **Not distributed in this slice.** `gl_merge`-ing consolidated R3 weights across nodes is
  possible (it is generic over param count) but is the G22 axis; left out to avoid sprawl.
- **Not wired to the live `dmn_idle_work` hook.** Like R3/DMN the CI gate is in-process; live
  idle-time handoff is future.

### V.8 CI verb plan (specify only — do NOT edit `ci.yml`)

The next wave should:

1. Add a shell verb **`handoff test`** dispatching into `r3_handoff_test()` (which lives in
   `r3_incontext.c`, V.6). Register it in `arch/x86/shell.c`'s command table (mirroring `cmd_dmn`
   → `lm_test` at `shell.c:500` and `cmd_self` → `lm_self_test` at `:489`) and the equivalent in
   the three `usermain.c`. (Acceptable alternative — and the **only** change if a sequence edit
   is unwanted: extend the existing **`r3 test`** to also emit the handoff tags, since it already
   runs at `ci.yml:57` and shares the file; tags stay `handoff-` regardless. **Decision: a
   dedicated `handoff test` verb** for namespace clarity — it is a distinct claim from R3 recall.)
2. Emit these bracket-tagged lines (PASS/FAIL), greppable like the rest of the suite:
   - `[handoff-fast-only] PASS`    — the fact is fast-only; weights alone at chance (V.4 #1)
   - `[handoff-consolidated] PASS` — sleep moved it into the weights; answered without the prompt
     (V.4 #2, the headline)
   - `[handoff-grounded] PASS`     — gain traces to the real in-context reading; scrambled control
     gives no transfer; oracle-graded on a disjoint stream (V.4 #3)
3. Wire `handoff test` into the **native** stdin verb sequence at `ci.yml:57` (after `r3 test`,
   before `dmn test`/`exit`) and add three matching `grep -aF '[handoff-*] PASS' selftest.log`
   lines next to the `[dmn-*]`/`[salience-*]` block (`ci.yml:142-157`). The aarch64-qemu subset
   (`ci.yml:193`) already omits `r3 test`/`dmn test`, so the handoff tags ride only the native
   line — consistent with R3/DMN. In-process gate (no live p-fs; the handoff is within-node).
   **This document does NOT modify `ci.yml`; the implementer wave does.**

### V.9 Provenance / closes-on

Design only. This slice closes when `[handoff-fast-only]`, `[handoff-consolidated]`,
`[handoff-grounded]` are green on a clean rebuild AND CI-enforced (native targets), audited by a
**separate** agent on the **commander's** binary — not the implementer's. Per `gap-ledger.md`
discipline: when it ships and is CI-enforced it earns ONE epitaph line; it does not lengthen the
ledger and spawns no new `philosophy-gap-audit`. The audit makes the acceptance test; the
commander reads the gate formula line-by-line.

---

## Part VI — 随時: the living consolidation loop (a stream of facts, many sleeps, the real idle hook)

> Status: **design + acceptance test** (written before implementation, like Parts II–V).
> Owner of *this* slice: the next wave (separate implementer + separate auditor). Builds ON:
> **LM-4** (Part V, `r3_handoff_test()` in `arch/common/r3_incontext.c`, **closed** — ONE fact,
> ONE sleep, hand-called), **LM-1** (Part II, the engram-replay→distill cadence + the live
> `dmn_idle_work` hook pattern in `arch/common/lm_consolidate.c` / `arch/common/dmn.c`,
> **closed**) and the DMN organ itself (`dmn.c`). All closed in `gap-ledger.md`, so this is
> unblocked.

Part V.5/V.7 left this as explicit homework, in three named bounds: *"Single fact-set, single
sleep — not lifelong"*, *"no interaction with DMN anti-forgetting (Part II)"*, and *"not wired
to the live `dmn_idle_work` hook."* **This Part closes all three** — it is the 随時 of the
north star (I.1: *the mind learns continuously from conversation, 随時, not batch*) turned
into a measured mechanism: a STREAM of facts taught in-context at different times, consolidated
across MULTIPLE bounded sleep rounds **without destroying previously consolidated facts**,
driven by the SAME function the mind's own idle-time hook calls — not a hand-called harness.
It mirrors Parts II–V's rigor exactly: a claim, a falsifiable certificate with numeric
thresholds + bracket tags, named honest bounds, a HARD anti-fork surface flagging names that
do not exist, and a CI verb plan.

### VI.0 IMPORTANT — what the tree actually says (read BEFORE coding)

Three load-bearing facts from the code, two expected and one that corrects the wave framing:

1. **`r3_incontext.c` is still self-test-only.** Every function and both weight arrays are
   `static`; the only public symbols are `r3_cmd` / `r3_test` / `r3_handoff_test`
   (`dtr.h:273-275`). The whole stream machinery — queue, arrival, idle round, cert — must
   live INSIDE `r3_incontext.c`, exporting a minimal new surface (VI.8), exactly the V.6
   discipline that saved the LM-4 wave.
2. **The LM-1 idle-hook pattern exists and is the template.** `dmn_idle_work()` (`dmn.c:93`)
   already calls a consolidation round, gated by pending work:
   `if (dmn_stats.idle_runs % GA_INTERVAL == 1 && lm_engrams_pending()) {
   lm_consolidate_idle_round(); }` (`dmn.c:106-109`); the round is BOUNDED
   (`LM_IDLE_STEPS=4`, `lm_consolidate.c:865-888`) and the public pair
   `lm_consolidate_idle_round()`/`lm_engrams_pending()` (`lm_consolidate.h:56-59`) is the
   exact shape the R3 stream must mirror. Idle is decided in `dmn_task` (`dmn.c:144-166`):
   `idle_for = dmn_pulse_count - dmn_last_trigger >= dmn_idle_threshold` (pulse = 1000 ms,
   threshold = 5; `dmn.h:37-38`), and ACTIVE is re-entered by `dmn_trigger()` (`dmn.c:78-87`),
   called on inference requests (`dtr.c:1251`) and node-state changes (`swim.c:126`).
3. **The DMN task does not run on the binary CI actually tests.** `dmn_init()` and
   `create_task(dmn_task, DMN_PRIORITY=13, DMN_STACK=8192)` exist ONLY in
   `arch/x86/usermain.c:155,167` (constants at `:84-85`). The two hosted usermains that the
   CI native job drives (`ci.yml:57` runs `./p-kernel`) create net/swim/dtr/reflex/pfs/...
   tasks (`arch/linux/x86_64/usermain.c:259-352`) but **never the DMN task** — so today even
   LM-1's "live idle hook" is live only on bare-metal x86 and is dead code on the hosted
   fleet. Part VI's claim "driven by the mind's own idle-time" is hollow unless this is fixed
   → **COMMANDER DECISION 1** (VI.5; recommended: wire it in this wave).

### VI.1 The claim to prove

> A **stream** of facts, each taught ONLY in-context at a different time (arrival order, not a
> batch), is consolidated into the weights `rw[]` across **multiple bounded sleep rounds** —
> the SAME `r3_consolidate_idle_round()` the DMN idle hook calls — such that: (1) **naive**
> sequential consolidation demonstrably **destroys earlier facts** (multi-fact interference,
> the new disease, measured first); (2) **interleaved replay of retained fact-engrams** during
> each sleep cures it — after the full stream EVERY fact is answerable **with the prompt
> removed**, at ≥ chance + margin, while the newest fact still consolidates (plasticity); (3)
> the gain still traces to the genuine in-context reading (a stream of **scrambled** arrivals
> transfers nothing — the LM-4 grounding survives into the stream setting); (4) the pending
> queue is **budget-bounded** with honest, printed forgetting on eviction.

The disease must be **real** before the cure is credited — exactly the `[dmn-forgetting]` /
`[handoff-fast-only]` discipline. If naive sequential consolidation does NOT collapse earlier
facts, the implementer raises the pressure (steps / lr), never lowers the bar.

### VI.2 The stream — F disjoint fact-sets (and why disjoint, honestly)

Partition the `R_KEYV=8` keys into **F disjoint key subsets** `K_1..K_F`; fact `D_f` binds the
keys of `K_f` to values in `{0..R_VALV-1}` (chance stays 25%). **Why disjoint, named up
front:** two facts binding the SAME key to different values *contradict*; replay cannot retain
both — `lm_consolidate.c:14-26` chose region-shift over class-permutation for exactly this
reason (*"a replayed task-0 engram (X, y0) directly CONTRADICTS the current task's (same X,
y_t) — the model cannot satisfy both and replay cannot cure forgetting"*). Contradiction is
fact UPDATE / belief revision — a different, later slice (VI.7). This slice is the R3 analog
of LM-1's instantiation (B): disjoint regions of prompt space (here: of query-key space), so
an old fact's engram never contradicts a new fact's training signal — yet interference is
still expected and must be measured, because all facts share every weight.

Per-fact episodes extend the LM-4 prompt builder `h_build` (`r3_incontext.c:600`) — token
choice only, NO new math (`r_forward` unchanged):

- **SUPPORT(f, k∈K_f)** — keys in `K_f` show `D_f`'s real bindings; the OTHER keys' value
  slots show **fresh random values, resampled per episode** — i.e. exactly the pretraining
  distribution `gen_episode` (`r3_incontext.c:123`) produces, so the frozen teacher reads it
  in-distribution and the random fillers carry no persistent signal (they average out; the
  student never sees them — it trains on MASKED prompts). Query = `k`, oracle = `D_f[k]`.
- **MASKED(f, k∈K_f)** — the LM-4 MASKED mode unchanged: all dict value slots `R_UNK`; query
  restricted to `K_f`. The only path to the answer is the weights.
- **SCRAMBLED(f)** — SUPPORT-shaped but `K_f` shows `D'_f ≠ D_f` (every binding shifted
  `+1 mod R_VALV`, the `h_make_dstar` pattern, `r3_incontext.c:571-580`).

Note the end-state sanity check: with the recommended F=4×2 (DECISION 2), the union of all
facts is 8 bindings — exactly the LM-4 task size that `[handoff-consolidated]` already drove
to 100% masked. The cure asks the weights to hold no more than LM-4 proved they can hold;
what is new is *surviving the arrival ORDER*.

### VI.3 Fact arrival = engrams, not API calls

A "fact taught in conversation" becomes a pending-consolidation item at **arrival time**: the
new public entry `r3_fact_learn(keys[], vals[], n)` (FLAGGED — does not exist, VI.8) builds
SUPPORT prompts from what the conversation shows, runs the **frozen FAST layer** on each key
(`h_predict`, `r3_incontext.c:625`), and enqueues the readings as **fact-engrams** — then
calls `dmn_trigger()` (a conversation IS a stimulus, mirroring `dtr.c:1251`). The engram
stores the **teacher's in-context reading `ŷ_k`, NOT the oracle** — that is the hippocampal
trace of the conversation, and it preserves LM-4's grounding: the oracle is used only to
grade, never to train; a misread binding is memorized wrong and the eval shows it.

Minimal in-RAM representation (file-static in `r3_incontext.c`; `LM_ENGRAM` at
`lm_consolidate.h:33-39` is 4-channel sensor-shaped and does NOT fit — reuse the concept, not
the struct, the Part V flag re-affirmed):

    typedef struct {
        UB key[R_KEYV];   /* the fact's bound keys                  */
        UB yhat[R_KEYV];  /* the FAST layer's reading per key       */
        UB n;             /* bindings in this fact (<= R_KEYV)       */
        UB state;         /* R3F_PENDING -> R3F_RETAINED            */
        UB rounds_done;   /* idle rounds spent on this fact          */
        UB salience;      /* reserved, default 1 (see below)         */
        UW seq;           /* arrival order (the autobiographical when)*/
    } R3_FACT;            /* fixed size; _Static_assert it           */
    static R3_FACT r3_fq[R3_FQ_MAX];          /* the bounded queue   */

**Budget:** `R3_FQ_MAX = F` (4 under DECISION 2) fact-sets — the B_RING honesty knob applied
to facts: fixed, small, PRINTED at test time. **Eviction: FIFO** (oldest RETAINED evicted on
overflow; a PENDING fact is never evicted), and every eviction is **printed** — an evicted
fact stops being rehearsed and its weight trace may decay over later sleeps: honest, visible
forgetting, not silent loss. **Why FIFO and not LM-3 salience:** earned salience needs a real
accrual source, and `reflex_threat_experience` is per dtr **sensor class**, not per R3 key —
no conversational fact earns threat experience today. Deriving a salience for facts by hand
would be hand-set salience, the exact thing `[salience-earned]` forbids. The `salience` byte
is reserved at default 1 (the LM-3 no-regress hinge shape) so a later slice can earn it; this
slice does not force the tie-in (→ DECISION 4).

Honest note: **today the only caller of `r3_fact_learn` is the cert.** No live conversational
producer exists yet (`chat.c` feeds the dtr/chat path, not R3). The arrival API is the real
production entry; the conversation source is future (VI.7).

### VI.4 The cure — interleaved replay across sleeps (ONE mechanism, no new math)

The LM-1 pattern applied to R3's own consolidation, and nothing else: each sleep round runs
MASKED-student SGD (`r_forward`/`r_backward` + `rw -= lr*rg`, `r3_incontext.c:143/212/696-699`
— the V.0 correction stands: NOT `dtr_train_batch`/`gl_merge`, wrong network) over the
**interleaved union** of (the PENDING fact's engrams) + (ALL RETAINED facts' engrams), exactly
the `with_replay` discipline of `lm_train_task` (`lm_consolidate.c:384-402`) transplanted to
the `rw[]` slow layer. Training items are `MASKED(f,k) → stored ŷ_k` on fresh arrangements
from the train seed; no teacher re-run is needed for old facts — their reading was captured
at arrival (VI.3), which is precisely what an engram is.

**Bounded and incremental — a real idle round, not a batch job:**
`r3_consolidate_idle_round()` runs ONE bounded chunk (`R3_IDLE_STEPS`, the `LM_IDLE_STEPS`
analog; propose 4×`H_PER_ROUND`=256 interleaved steps) and returns 1; a PENDING fact flips to
RETAINED after `R3_SLEEPS_PER_FACT` chunks (propose 10, totalling the `H_ROUNDS×H_PER_ROUND`
= 40×64 budget LM-4 measured as sufficient, `r3_incontext.c:711-712`). So one fact genuinely
takes MULTIPLE sleeps — 随時 is literal, and the cert calling the same function in a loop is
not a simulation shortcut but the actual production cadence.

**The disease run (naive path)** is identical in every respect — same arrivals, same per-fact
step budget, same lr schedule — except the minibatch contains ONLY the pending fact's engrams
(the `with_replay=0` knob). The ONLY difference between disease and cure is the interleaving;
nothing else moves, so the cure is attributable.

### VI.5 The live idle hook — and the honest split

The wiring, 3 lines next to the LM-1 hook in `dmn_idle_work()` (`dmn.c:106-109`):

    if (r3_facts_pending()) {
        if (r3_consolidate_idle_round())
            dmn_puts("[dmn] sleep: distilled in-context facts -> rw[]\r\n");
    }

Gating: LM-1 runs 1-in-`GA_INTERVAL`(=10, `ga.h:39`) idle pulses; for pending conversation
facts we recommend **every idle pulse while pending** (a fresh fact is the most urgent rest
work; the round is bounded, and a fact drains in ~`R3_SLEEPS_PER_FACT` idle seconds at the
1000 ms pulse) → DECISION 3. No busy flag is needed: `dtr_ga_busy` (`dtr.c:267`, checked by
`dtr_infer` at `dtr.c:1248`) protects the dtr body, and **nothing live reads `rw[]`** — R3 is
self-test + this loop. If a later slice serves live R3 queries, an `rw[]` busy flag is added
then (named, not built).

**Interaction with Part II anti-forgetting (the LM-4 bound), answered honestly:** the two
consolidations now share one roof (`dmn_idle_work`) but train **different networks** —
`lm_consolidate_idle_round` distills engrams into the dtr sensor body; this round distills
fact-engrams into R3's `rw[]`. Non-interference is *structural* (disjoint weight buffers),
and the only shared resource is idle time, which both rounds bound. That is the whole claim;
no stronger interaction theorem is asserted (VI.7).

**The honest split (the G33 lesson: the test must drive the production formula):**

- **CI in-process cert** (`r3_stream_test()`): enters facts ONLY through `r3_fact_learn()`
  and consolidates ONLY through `r3_consolidate_idle_round()` — the exact symbols the dmn.c
  hook calls — looped deterministically until the queue drains. This is CI-gated.
- **Genuinely on the idle path**: the 3 wired lines above + the DMN task itself. NOT CI-gated
  on wall-clock timing (5 s idle threshold × ~40 sleeps would add minutes and flake); instead
  `[stream-livehook]` gates code-path identity structurally (VI.6 #4) and the commander reads
  the dmn.c wiring line-by-line (the standing rule). The live print `[dmn] sleep: ...` is
  observable in a manual run, like LM-1's (`dmn.c:108`).

**COMMANDER DECISION 1 — wire the DMN task into the hosted builds (recommended: YES, this
wave).** Add `dmn_init()` + `create_task((FP)dmn_task, 13, 8192)` (params from
`arch/x86/usermain.c:84-85,155,167`) to `arch/linux/{x86_64,aarch64}/usermain.c`. Without it,
"driven by the mind's own idle-time" is dead code on the binary CI tests (VI.0 #3) — and
LM-1's live hook has silently been x86-only too; this fixes both. Tradeoff: a new always-on
lowest-priority task on the hosted fleet (priority 13, below every existing 3–7 task at
`usermain.c:259-352`; heartbeat work is a few comparisons per second, the consolidation only
runs when facts/engrams are pending). Alternative: defer and demote the slice's claim to
"hand-callable idle round" — honest but it punts the very gap this Part exists to close.

### VI.6 The falsifiable acceptance test (the certificate)

All emitted as printed numbers **then** a canonical `[tag] PASS/FAIL` line. `chance = 25%`.
Thresholds are **proposed bars**; the implementer reports actuals and may only LOWER a bar to
measured-minus-flake-margin, flagged — never inflate to make green. Substrate discipline as
LM-4: pretrain once to in-context competence (`r_train_epoch` over resampled dicts,
`r3_incontext.c:727-733`), snapshot `rw[]`, restore before EACH of the three runs below and
at the end (no state leaks into later verbs; `r3_incontext.c:716,735,759,787` pattern). Seeds:
train/eval arrangement streams disjoint, the `H_SEED_TRAIN`/`H_SEED_HELD` discipline
(`r3_incontext.c:707-708`); per-fact masked eval queries only `K_f`, graded vs oracle `D_f`.

1. **`[stream-interference]` — the multi-fact disease is real (precondition).**
   From the frozen snapshot: print `acc_pre(f) ≤ 33` for EVERY fact (no fact is baked in —
   the `[handoff-fast-only]` precondition inherited per-fact). Arrive fact 1, consolidate to
   RETAINED, print `acc_f1_post ≥ 50`. Then arrive + consolidate facts 2..F **naively** (no
   interleave). PASS requires ALL:
   - every `acc_pre(f) ≤ chance + 8` (= 33), AND
   - `acc_f1_post ≥ chance + 25` (= 50: fact 1 was really consolidated), AND
   - `acc_f1_naive_end ≤ 40` AND `acc_f1_post − acc_f1_naive_end ≥ 25` pts
     (the `[dmn-forgetting]` bars, `lm_consolidate.c:565`: later sleeps ATE fact 1).
   If interference is not real, raise the pressure (steps/lr) — do NOT weaken the bar.

2. **`[stream-consolidated]` — interleaved sleeps retain the whole stream (the headline).**
   Restore the snapshot; same arrivals; every sleep interleaves retained engrams (VI.4);
   loop `r3_consolidate_idle_round()` until the queue drains. PASS requires ALL:
   - `min over f of acc_replay_end(f) ≥ chance + 25` (= 50: EVERY fact answerable with the
     prompt removed, after the full stream), AND
   - `acc_f1_replay_end − acc_f1_naive_end ≥ +20` pts (the cure over the measured disease),
     AND
   - `acc_fF_replay_end ≥ 50` (the NEWEST fact also landed — plasticity not sacrificed,
     the `[dmn-consolidated]` third clause).

3. **`[stream-grounded]` — the stream's gain traces to genuine in-context reading.**
   Restore the snapshot; identical stream, but every arrival's prompt shows `D'_f ≠ D_f`
   (SCRAMBLED). PASS requires ALL:
   - `teacher_agree(f) ≥ chance + 25` printed per fact on the REAL run (the ceiling visible,
     V.3 discipline), AND
   - train seed ≠ eval seed printed (disjoint arrangement streams), AND
   - `acc_scrambled_end(f) ≤ chance + 8` (= 33) for EVERY fact — arrivals that never showed
     the real bindings transfer nothing; the LM-4 control, stream-wide.

4. **`[stream-livehook]` — the loop is the production formula, bounded, budget-honest.**
   Structural gates, all printed:
   - facts entered ONLY via `r3_fact_learn()`; consolidation reached ONLY via
     `r3_consolidate_idle_round()` — the SAME public symbols `dmn_idle_work` calls (the cert
     cannot see the dmn.c call site from in-process; the PASS text says so, and the commander
     reads the 3 wired lines — stated, not overclaimed), AND
   - every round bounded: `steps_per_round == R3_IDLE_STEPS` printed; rounds-per-fact
     `== R3_SLEEPS_PER_FACT` printed; state flips PENDING→RETAINED only inside the round, AND
   - queue budget honest: arrive an `R3_FQ_MAX+1`-th fact → the OLDEST RETAINED fact is
     evicted, the eviction PRINTED, queue occupancy never exceeds `R3_FQ_MAX` (printed with
     the budget, the B_RING convention); the evicted fact's masked accuracy after further
     sleeps is PRINTED but not gated (honest forgetting is allowed to decay).

(Considered and **dropped**, the V.4/[self-other] lesson: a gradcheck tag — the SGD is the
already-certified `r_backward` (`[r3-incontext-gradcheck]`); a distributed tag — `gl_merge`
of `rw[]` is the G22 axis, sprawl here; a salience tag — no earned source exists for facts,
VI.3.) **No-regress is CI-level, not a new tag:** all existing 18 greps stay green
(`[r3-*]`×4, `[handoff-*]`×3, `[dmn-*]`×5, `[salience-*]`×3, `[self-*]`×3) — the stream code
extends `r3_incontext.c` without touching the LM-4 gates, and the snapshot/restore discipline
leaves `rw[]` as found.

### VI.7 The honest bound (what is NOT claimed)

- **Toy synthetic vocab, NOT natural language** — the standing R3 scope caveat; "learns from
  conversation 随時" is shown structurally on key→value facts, not on dialogue.
- **Facts are DISJOINT by construction.** Contradiction — re-teaching key `k` a NEW value
  (belief revision / fact update) — is explicitly out of scope, and naive replay provably
  cannot do it (the `lm_consolidate.c:14-26` trap). A future slice; named, not hidden.
- **The engram is the reading, not the conversation.** Arrival keeps `(k, ŷ_k)`; the teacher
  ceiling applies at arrival time and a misread binding is rehearsed wrong forever after
  (printed `teacher_agree(f)` keeps the ceiling visible).
- **FIFO forgetting, no earned salience.** Eviction is honest and printed but arbitrary in
  the salience sense; the LM-3 tie-in waits for a real accrual source.
- **"Live" means: the same function, wired at the idle path.** CI gates the formula and the
  bounds, NOT the wall-clock idle timing; and no conversational producer calls
  `r3_fact_learn` yet — the cert is its only caller today.
- **Part II interaction is structural non-interference only** (different weight buffers, both
  rounds bounded, one shared idle budget) — no starvation/ordering theorem is asserted.
- **Within ONE frozen-architecture R3 model, within ONE node.** Weights only (Evolution layer
  stays future); no `gl_merge` of `rw[]` (G22 axis).
- **Held-out is held-out ARRANGEMENT, not held-out facts** — the Part V bound stands,
  per-fact.

No false "by construction" theorem is asserted.

### VI.8 Anti-fork constraint (HARD) — exact reuse surface (+ FLAGGED names)

**Reuse (MUST — no forked math, no new organ, no second Transformer):**
- `r_forward` (`r3_incontext.c:143`) / `r_backward` (`:212`) / the SGD update shape
  (`:399`, `:696-699`) — the only training path for `rw[]`.
- `h_build` (`:600`) — EXTENDED in-file with the key-subset + random-filler SUPPORT mode and
  `K_f`-restricted queries (token choice only); `h_predict` (`:625`); `h_eval_mode` (`:639`)
  extended per-fact; `h_teacher_agree` (`:657`); `h_make_dstar`'s shift trick (`:571-580`)
  for the per-fact scramble; the snapshot/restore discipline (`:716,735,759,787`).
- Seed discipline: `R_SEED_TRAIN/HELD` (`:474-475`), `H_SEED_TRAIN/HELD` (`:707-708`).
- The DMN cadence as PATTERN (mirror, do NOT call): the bounded idle round
  (`lm_consolidate.c:865-888`), the `with_replay` interleave (`:384-402`), the pending/round
  public pair shape (`lm_consolidate.h:56-59`), the `dmn_idle_work` wiring (`dmn.c:106-109`),
  `dmn_trigger()` on arrival (`dmn.c:78`, mirroring `dtr.c:1251`).

**FLAGGED — names that do NOT exist as callable API (create exactly as scoped, nothing more):**
- `r3_fact_learn()`, `r3_facts_pending()`, `r3_consolidate_idle_round()`, `r3_stream_test()`
  — the ONLY new public symbols, declared in `dtr.h` next to `r3_handoff_test`
  (`dtr.h:273-275`). The queue (`r3_fq[]`), `R3_FACT`, and all stream helpers stay
  file-static in `r3_incontext.c`.
- The `handoff stream` sub-verb — the dispatchers accept only `test` today
  (`arch/linux/{x86_64,aarch64}/usermain.c:656-666`).
- The DMN task on hosted builds — `dmn_init`/`dmn_task` are created only at
  `arch/x86/usermain.c:155,167` today (DECISION 1).
- **Do NOT call:** `dtr_train_batch` / `dtr_weights_get|set` / `gl_merge` (the dtr sensor
  body — wrong network, the V.0 correction stands); `LM_ENGRAM` (4-ch sensor struct);
  `lm_consolidate_idle_round` (the dtr body's round); `lm_ring_capture` (file-static, dtr
  dataset). Reuse their DISCIPLINE, never their symbols.
- No `rw[]` busy flag exists and none is added (nothing live reads `rw[]`) — named for the
  slice that first serves live R3 queries.

### VI.9 CI verb plan (specify only — do NOT edit `ci.yml` in this Part)

1. **Sub-verb `handoff stream`** → `r3_stream_test()`, extending the existing `handoff`
   dispatcher (`usermain.c:656-666`: today only `test`). **Recommended over** (a) a new
   top-level verb — the stream IS the handoff grown to 随時, same namespace — and (b) folding
   into `handoff test` — a distinct claim deserves a distinct greppable verb, and LM-4's cert
   stays untouched for bisection.
2. stdin sequence at `ci.yml:57`: insert `handoff stream\n` after `handoff test`, before
   `dmn test`.
3. Four greps next to the `[handoff-*]` block (`ci.yml:154-156`):
   `[stream-interference] PASS`, `[stream-consolidated] PASS`, `[stream-grounded] PASS`,
   `[stream-livehook] PASS`. Native job only — the aarch64-qemu subset already omits
   `r3 test`/`dmn test`, consistent with R3/DMN/LM-4.
4. ALL existing 18 greps stay — the no-regress gate. **The implementer wave edits `ci.yml`,
   not this document.**

### VI.10 Provenance / closes-on

Design only. This slice closes when `[stream-interference]`, `[stream-consolidated]`,
`[stream-grounded]`, `[stream-livehook]` are green on a clean rebuild AND CI-enforced (native
targets), audited by a **separate** agent on the **commander's** binary — not the
implementer's. Per `gap-ledger.md` discipline: when it ships and is CI-enforced it earns ONE
epitaph line; it does not lengthen the ledger and spawns no new `philosophy-gap-audit`. The
audit makes the acceptance test; the commander reads the gate formula — and the 3 wired
`dmn.c` lines — line-by-line.

**COMMANDER DECISIONS NEEDED (recommended defaults):**
1. **Wire the DMN task into the hosted usermains** (VI.5) — recommend **YES, this wave**;
   else the live-hook claim must be demoted (tradeoff: one new priority-13 task on the fleet).
2. **Stream shape:** **F=4 facts × 2 keys (recommended)** vs F=2 × 4 keys. 4 arrivals/sleeps
   make 随時 and the interference test real (fact 1 must survive 3 later sleeps); the union
   (8 bindings) stays exactly the LM-4-proven capacity. Tradeoff: 2-key facts give the
   teacher an easier read but each fact a coarser per-query eval (mitigated by N≥200
   arrangement-varied eval episodes per fact).
3. **Idle gating for the fact round:** every idle pulse while pending (**recommended**;
   facts drain in ~10 idle seconds) vs LM-1's 1-in-`GA_INTERVAL` (uniform but ~100 s/fact).
4. **Eviction policy:** FIFO with printed forgetting (**recommended**) vs sizing
   `R3_FQ_MAX` so the cert never evicts (defers the budget-honesty sub-claim of
   `[stream-livehook]`). FIFO is recommended because a bounded mind that cannot say what it
   forgot is the AUDIT-SPRAWL failure mode applied to memory.

## Part VII — the mouth: a real conversational producer (the owner teaches the live mind)

> Status: **design + acceptance test** (written before implementation, like Parts II–VI).
> Owner of *this* slice: the next wave (separate implementer + separate auditor). Builds ON:
> **LM-5** (Part VI, `r3_fact_learn`/`r3_facts_pending`/`r3_consolidate_idle_round` in
> `arch/common/r3_incontext.c:918/975/1030`, the DMN task live on BOTH hosted usermains —
> **closed** wave-26) and the DMN organ (`dmn.c:118-120` already consolidates pending facts
> every idle pulse). All closed in `gap-ledger.md`, so this is unblocked.

LM-5's own audit named the hole this Part closes, verbatim in the VI.3 honest note and the
gap-ledger epitaph: *"no conversational producer yet (the cert is the only `r3_fact_learn`
caller today)"*. The mind has a digestion system but no mouth: the live
`[dmn] sleep: distilled in-context facts -> rw[]` print (`dmn.c:120`) can NEVER fire outside
a test, because nothing in production ever enqueues a fact. This Part gives the fleet the
smallest honest mouth: **the OWNER, at the shell prompt, teaches the mind a fact; with NO
further verbs, the mind's own idle pulses consolidate it; the owner asks and the mind answers
from its weights.** It is the north star's 随時 (I.1) finally fired by a human hand instead
of a harness — and it is deliberately NOT natural language (VII.8): the vocabulary is still
R3's synthetic 8 keys × 4 values. Own that bound loudly; do not dress it up.

### VII.0 IMPORTANT — what the tree actually says (read BEFORE coding)

Five load-bearing facts, two of which EXPIRE promises made by Part VI:

1. **`r3_fact_learn(const UB *keys, const UB *vals, INT n)` accepts `n = 1..R_KEYV`**
   (`r3_incontext.c:918-920`, decl `dtr.h:298`). The 2-keys-per-fact F-shape was CERT
   geometry (`R3_FKEYS = R_KEYV/R3_NFACTS = 2`, `:818`), not an API constraint. A single
   `teach k v` maps honestly onto a **singleton fact-set (n=1)** — no batching trick, no
   pairing heuristic, no second key invented to please the cert's shape (VII.2).
2. **At a fresh boot the mind has no brain.** `rw[]` is a zeroed static (`:93`); the
   substrate is pretrained ONLY inside the certs (`r_init_weights(0xA5A5)` + the 60-epoch
   recipe at `:1089-1095`, same at `:496/:504/:728`). A live `teach` against zero weights
   would have the "frozen FAST layer" read garbage. The mouth needs a substrate bootstrap
   (VII.3) — lazy, deterministic, printed.
3. **VI.8's "no `rw[]` busy flag is needed (nothing live reads `rw[]`)" expires NOW.** This
   slice is exactly "the slice that first serves live R3 queries" that VI.8 named. The
   hazard is real and one-directional: the DMN task is priority 13 — strictly LOWEST
   (`usermain.c:593`) — so it runs only while the shell task blocks, and the shell PREEMPTS
   it mid-`s_round` (`:987`) the moment input arrives. A `mind ask`/`teach` then runs
   `h_predict`→`r_forward` (`:626/:143`) over the SAME shared activation struct `rc`
   (`:110-115`), gradient buffer `rg` (`:94`) and RNG `r_rng` (`:116`) that the half-finished
   round will resume into — the resumed `r_backward` would push a corrupted gradient into
   `rw[]`. The named flag must now be BUILT (VII.4). The reverse direction is safe by
   construction: prio-13 can never preempt the shell, so a verb's own eval is atomic.
4. **Latency is already decided by shipped constants — design the wait around them, do not
   invent new ones.** Arrival calls `dmn_trigger()` (`r3_incontext.c:971`) → ACTIVE; idle
   re-entry needs `dmn_idle_threshold = 5` quiet pulses at `DMN_PULSE_MS = 1000`
   (`dmn.h:38-39`, `dmn.c:158`); then `dmn.c:118` runs one bounded round EVERY idle pulse
   while pending, and a fact flips RETAINED after `R3_SLEEPS_PER_FACT = 10` rounds (`:821`,
   `:1021`). Expected teach→consolidated wall clock ≈ **15 s**; that number sizes the cert
   timeout (VII.5).
5. **Cert verbs are amnesia bombs.** `r3_stream_test` restores `rw[]` to its OWN pretrain
   snapshot and wipes the queue on exit (`:1232-1233`); `r3_handoff_test` likewise (`:788`).
   Any later cert run erases everything the owner taught live. This slice does NOT fix that
   (it would mean persisting `rw[]`, a different axis); it PRINTS it (VII.8) and orders the
   CI verbs so the mind verbs come after all certs (VII.10).

### VII.1 The claim to prove

> An owner at the shell types `mind teach <k> <v>` — ONE verb, no harness. The fact enters
> the LIVE queue through `r3_fact_learn` (the frozen FAST layer reads a SUPPORT prompt at
> arrival; `teacher_agree` printed). With **NO further verbs**, the DMN task's own idle
> pulses — the real 1000 ms heartbeat, the real ACTIVE→IDLE transition, the real
> `dmn_idle_work` call site at `dmn.c:118-120` — consolidate the fact into `rw[]`, and the
> live print `[dmn] sleep: distilled in-context facts -> rw[]` fires **in production for the
> first time**. Afterwards `mind ask <k>` answers the fact from the weights on a MASKED
> prompt. The consolidation is attributable to the dmn.c call site by a counter that ONLY
> that call site increments — the cert never calls the round function itself.

This is strictly stronger than LM-5's `[stream-livehook]` ("the cert drives the production
FORMULA"): here the production TRIGGER fires, timer and all. The G33 rule is kept and
extended: the cert drives the production formula *through the production schedule*.

### VII.2 The verbs — `mind teach | ask | wait | (bare)` (the whole mouth)

One new top-level shell verb `mind` on BOTH hosted usermains (next to the `handoff` branch,
`arch/linux/{x86_64,aarch64}/usermain.c:672-683`), dispatching to ONE new public
`mind_cmd(const UB *args, UW len)` that lives in `r3_incontext.c` — so every helper it needs
(`s_build :896`, `h_predict :626`, `s_eval`-style loops, the queue) stays file-static, the
V.6/VI.8 discipline unchanged. No `mind` verb exists today (FLAGGED, VII.9). Sub-verbs:

- **`mind teach <k> <v>`** (`k` 0..7, `v` 0..3, decimal): the owner teaches one binding.
  Semantics, in order: (1) quiesce (VII.4); (2) substrate bootstrap if first use (VII.3);
  (3) **pre-sleep novelty read**: masked vote share of `v` for key `k` over N=100 held-out
  arrangements (`S_SEED_HELD` stream, the `s_eval_fact :1034` pattern) — printed as
  `pre_share`; (4) **refuse re-teach**: if `k` is already bound by any queued fact, print
  `[mind] key K already taught — re-teach is belief revision (future slice); refused` and
  return — replay provably cannot hold a contradiction (`lm_consolidate.c:14-26`, the VI.2
  bound); (5) call `r3_fact_learn(&k, &v, 1)` — a SINGLETON fact-set (VII.0 #1); a full-of-
  PENDING queue refuses loudly (`:932`), a full queue with RETAINED facts FIFO-evicts with
  the printed forgetting LM-5 shipped (`:925-944`); (6) print `teacher_agree` (= stored
  `yhat == v`, the majority-of-5 frozen read, `R3_TEACH_READS :824`) and the queue occupancy;
  (7) emit the `[teach-arrival]` line (VII.6). The verb does NOT consolidate anything —
  `r3_fact_learn` already calls `dmn_trigger()`; the sleep belongs to the DMN.
- **`mind ask <k>`**: quiesce, then majority vote + vote share over **N=40** MASKED held-out
  arrangements (same `S_SEED_HELD`-derived stream as the certs — asking does not consume the
  arrival/training stream `r3_s_rng :851`); print `pred=<v> share=<x>%`, and, when `k` is
  held by a RETAINED queued fact, compare `pred` against the stored engram `yhat` and emit
  `[teach-consolidated]` (VII.6). Asking is a read: it must NOT call `dmn_trigger()` —
  wait, it MUST: a question is a stimulus exactly as an inference is (`dtr.c:1251`), and
  skipping it would special-case the mouth. It does call `dmn_trigger()`; the cert sequences
  `ask` AFTER `wait`, so this costs nothing (VII.5).
- **`mind wait [secs]`** (default 120): poll `r3_facts_pending()` every 500 ms via
  `tk_dly_tsk` until 0 or timeout. The sleeping shell IS the idle window — priority 13 runs
  precisely while the waiter sleeps, so this verb does not merely observe the sleep, it
  *yields the machine to it*, which is honest (an idle organ needs idle). On drain: print
  elapsed seconds, the dmn-round delta (VII.5), and emit `[teach-live]`. On timeout: FAIL.
- **`mind`** (bare): status — substrate ready?, queue occupancy `n/R3_FQ_MAX` with per-fact
  `key/state/rounds_done`, lifetime `dmn_r3_rounds()`. Costs ~20 lines and is the owner's
  only window into the queue; included.

**Decision logic for singleton facts (#1 answered concretely):** `r3_fact_learn` takes
`n=1..8` (VII.0 #1), and a 1-key fact flows through `s_round` unchanged (`m=1` engram for
the pending fact, `:995-1004`; replay interleave unchanged). Per-fact budget stays
`R3_SLEEPS_PER_FACT × R3_IDLE_STEPS = 2560` steps — generous for one binding (LM-5 landed
2-key facts at 100% with the same budget), and NOT retuned this slice (no new constants, the
VI.4 budget argument inherited). The alternative — buffering teaches in pairs to mimic the
cert's 2-key shape — was rejected: it adds a hidden half-taught state ("your fact is waiting
for a sibling") that the owner cannot see, for zero mechanistic benefit.

### VII.3 The substrate bootstrap — lazy pretrain, printed, deterministic

First `mind teach`/`ask` checks a file-static `m_ready`; if unset it runs EXACTLY the cert
pretrain — `r_init_weights(0xA5A5)` + the 60-epoch `r_train_epoch(R_SEED_TRAIN, 192, lr)`
schedule, lifted verbatim from `:1089-1095` into ONE shared static `s_pretrain()` that
`r3_stream_test` is refactored to call too (in-file de-dup, not a public symbol; the recipe
already exists three more times at `:496/:504/:728` — consolidating r3_test/handoff onto it
is allowed but NOT required this slice, to keep the diff reviewable). Prints
`[mind] substrate pretrained (first use, seed 0xA5A5)` with elapsed time. Same seed + same
recipe = the same deterministic substrate the certs measure, byte-identical cross-arch
(LM-5's audited property), so the cert's chosen (k,v) bias measurement (VII.6) holds on the
live path. The alternative — a separate `mind init` verb — was rejected: a mouth that errors
with "run init first" is ceremony; lazy + printed is one fewer way to misuse it.

### VII.4 The quiesce handshake — the VI.8 named flag, now built

File-static in `r3_incontext.c`:

    static volatile UB r3_round_busy = 0;   /* set/cleared by s_round() */

`s_round` sets it on entry, clears on exit (`:987-1023`). Every `mind` sub-verb begins:

    while (r3_round_busy) tk_dly_tsk(20);   /* shell sleeps -> prio-13 finishes the round */

Bounded by construction: one round is ≤ `R3_IDLE_STEPS = 256` SGD steps (milliseconds), and
the shell sleeping is precisely what lets the lower-priority round complete — no deadlock,
no semaphore object needed, no change to the round's hot path beyond two byte-stores. The
same quiesce line is added at the TOP of `r3_stream_test`/`r3_handoff_test`/`r3_test`: with
live facts now possible, a cert that starts while a round is in flight would reset the queue
and pretrain `rw[]` under a half-finished SGD step (VII.0 #5 made certs amnesia bombs; this
keeps them at least ATOMIC bombs). It is a flag, not a lock: sufficient ONLY because of the
strict-priority argument in VII.0 #3 (verbs cannot be preempted by the round). If R3 queries
ever move off the shell task, this becomes a real mutex — named for that slice, not built.

### VII.5 The live proof — `dmn_r3_rounds` and the flakiness story (#3 answered)

**The counter.** A new static `UW dmn_r3_round_count` in `dmn.c`, incremented at EXACTLY ONE
site — inside the existing hook, beside the live print:

    if (r3_facts_pending()) {
        if (r3_consolidate_idle_round()) {
            dmn_r3_round_count++;                       /* the ONLY ++ site */
            dmn_puts("[dmn] sleep: distilled in-context facts -> rw[]\r\n");
        }
    }

read through a new public `UW dmn_r3_rounds(void)` (`dmn.h`, FLAGGED). It is NOT inside
`r3_consolidate_idle_round()` itself — so `r3_stream_test`'s 40+ direct calls (`:1141` etc.)
do not move it, and a nonzero delta is attributable to the dmn.c call site alone.
`mind teach` snapshots the counter at enqueue; `mind wait` prints the delta at drain and
gates `delta >= R3_SLEEPS_PER_FACT` (a fact cannot drain in fewer rounds, `:1021`).

**Fakeability, stated honestly:** in one flat address space nothing is unfakeable by code in
the same image (the standing pre-ring3 caveat — the REAL fix is the ring3/EL0 relocation
project, where the counter becomes kernel-owned). The discipline that makes it trustworthy is
the LM-5/G33 one, extended: (a) the auditor greps that `dmn_r3_round_count++` appears ONCE,
in `dmn.c`, inside the `r3_consolidate_idle_round()` success branch; (b) the auditor greps
that `mind_cmd`'s body contains NO call to `r3_consolidate_idle_round` or `s_round` (in
`r3_incontext.c` the round symbols stay referenced only by their definitions, the dmn hook,
and `r3_stream_test`); (c) the commander reads the `mind wait` loop and the dmn.c hook
line-by-line (the standing rule). Stated, not overclaimed.

**The flakiness story, confronted:** the gate is on END STATE within a BOUND, never on when
pulses land. (i) The weight math cannot flake: rounds consume the dedicated arrival stream
`r3_s_rng` (`:851`, saved/restored around every round `:1006/:1018`), so the post-sleep
`rw[]` is bit-identical whether the 10 rounds take 15 s or 90 s — only elapsed time varies,
and elapsed time is printed, not gated. (ii) Expected drain is ~15 s (VII.0 #4: 5 idle-
threshold pulses + 10 round pulses at 1000 ms); the cert timeout is **120 s = 8× margin**.
(iii) What could spend the margin: stray `dmn_trigger()`s during the wait (each costs ≤5 s
of idle re-entry). In the CI sequence the mind verbs run LAST (VII.10) — no net is up, no
inference traffic exists, stdin is the only stimulus source, and `mind wait` itself triggers
nothing. A pathological CI-runner stall >120 s fails the gate; that residual risk is the
same class as the existing `timeout 300` on the whole job (`ci.yml:57`) and is accepted, not
hidden. (iv) `dmn test`/certs earlier in the sequence leave no pending facts (`:1233`), so
the counter delta window is clean.

### VII.6 The falsifiable acceptance test (the certificate) — 3 tags

No new in-process cert FUNCTION exists for this slice — that is the point. The certificate
IS the production verbs, driven over stdin, each printing numbers then a canonical line.
The cert's chosen binding `(k*, v*)` must be measured OFF-BIAS by the implementer on the
0xA5A5 substrate (the frozen mind emits key-conditional bias on all-UNK prompts — the LM-5
SDICT note (a), `:866-880`); the doc proposes `teach 2 3` but the implementer substitutes
the measured pick and records it. Bars are proposals: report actuals; lower ONLY to
measured-minus-margin, flagged — never inflate.

1. **`[teach-arrival]` — the mouth genuinely feeds the queue** (printed by `mind teach`).
   PASS requires ALL, each printed: substrate ready (bootstrapped or already trained);
   `r3_fact_learn` returned 0 and `r3_facts_pending()==1` after; `teacher_agree == 100`
   (the majority-of-5 frozen read returned `v*` — deterministic at the fixed substrate; if
   the measured pick cannot reach 100, lower to ≥80 WITH FLAG); `pre_share <= 33` (chance
   25 + 8: the fact is NOT already in the weights — the disease half of the pair, inherited
   from `[handoff-fast-only]`). A production teach whose `pre_share > 33` prints
   `[teach-arrival] REDUNDANT (already leaning)` and still enqueues — honest three-way
   outcome, but the CERT pick must print PASS.
2. **`[teach-live]` — the production trigger fired** (printed by `mind wait 120`). PASS
   requires ALL: queue drained within the 120 s bound (elapsed printed);
   `dmn_r3_rounds() delta >= R3_SLEEPS_PER_FACT` (=10) since the teach snapshot — rounds
   that ONLY the `dmn.c:118` call site can count; the `[dmn] sleep: distilled in-context
   facts -> rw[]` live print observed in the same log (CI greps it — the print this whole
   Part exists to make real).
3. **`[teach-consolidated]` — the mind answers from weights** (printed by `mind ask <k*>`).
   PASS requires ALL: fact RETAINED; `pred == yhat` (and `yhat == v*` was already gated at
   arrival — the chain owner-value → teacher-reading → weight-answer is closed end-to-end);
   `share >= 75` over N=40 masked held-out arrangements (LM-5 measured 100.0 post-sleep on
   harder 2-key facts; 75 leaves flake margin for the 40-sample vote — lower only with
   flag). Before the sleep the same number was `pre_share <= 33` (tag 1): disease→cure on
   the live path, two prints apart.

(Considered and **dropped**: an eviction tag — `[stream-livehook]` already gates FIFO
eviction and re-gating it via 5 teaches would add ~75 s of waits to CI for no new mechanism;
a scrambled-teach tag — grounding is `[stream-grounded]`'s axis and the arrival path here is
byte-identical to the certified one; a multi-fact live tag — one fact proves the trigger,
N facts prove only patience.) **No-regress is CI-level:** all existing greps stay green; the
`handoff stream`/`handoff test` numbers must stay BYTE-IDENTICAL (the refactor in VII.3
moves the pretrain recipe without changing it; the quiesce lines are no-ops when no round is
in flight; the mind verbs run after all certs).

### VII.7 Cross-node teaching — examined and deferred (#2 answered: NO)

The plumbing would be genuinely cheap: `kdds_open("mind/teach", ...)` + `kdds_pub` of a
3-byte `(k, v, origin)` and a subscriber task calling `r3_fact_learn` — the K-DDS surface
(`kdds.h:117-160`) makes it an afternoon. It is still the wrong slice, for three reasons:
(1) **divergence without reconciliation** — each node's `rw[]` would drift with its own
arrival timing and interleave; merging consolidated `rw[]` across nodes is `gl_merge`-of-
`rw[]`, the G22 axis Parts V/VI explicitly fenced off, and shipping the gossip without the
merge gives the fleet inconsistent minds with no observability story; (2) **no provenance**
— any node could feed every mind; the Self layer is tamper-EVIDENT lineage with NO signing
primitive (III.6), so "who taught this" is unanswerable today — a mouth the whole network
can shout into needs an immune system first; (3) **the cert would need two nodes and real
timing**, exactly the flake surface VII.5 just spent a section draining. The future slice is
named: **"the shared mind" — `mind/teach` gossip + periodic `rw[]` merge (G22) + taught-fact
provenance via the Self lineage.** Within-node first. → COMMANDER DECISION 2.

### VII.8 The honest bound (what is NOT claimed)

- **Synthetic vocabulary, NOT language.** `teach 2 3` binds key 2 to value 3 in an 8×4 toy
  space. The MOUTH is real (a human, a prompt, no harness); the WORDS are not. Claiming
  "the mind learns from conversation" without this caveat would be the project lying to
  itself.
- **Owner-typed, not autonomous extraction.** Nothing parses dialogue; `chat.c` still feeds
  the dtr path, untouched. Extraction is a different organ.
- **Within ONE node** (VII.7). Single shell = single implicit owner; facts carry no
  identity, no provenance, no signature.
- **Re-teach refused** — belief revision stays the named future slice (VI.2/VI.7); the
  refusal is printed, not silent.
- **The budget is 4 FACTS, not 8 keys** (`R3_FQ_MAX = 4`, `:819`): with singleton teaches
  the owner holds at most 4 of the 8 keys before printed FIFO eviction. Not enlarged this
  slice (budget honesty; enlarging is a knob, not a mechanism). → COMMANDER DECISION 3.
- **Cert verbs erase the live mind** (VII.0 #5) — `handoff test|stream` after a live teach
  wipes it, printed nowhere today; the `mind` status verb at least makes the loss visible.
  Persistence of `rw[]`/queue across runs (p-fs) is a named non-goal here.
- **The live proof is wall-clock-BOUNDED, not wall-clock-gated** (VII.5) — and counter
  attribution rests on auditor greps + commander read, not memory protection, until the
  ring3 relocation puts the counter behind a privilege boundary.

No false "by construction" theorem is asserted.

### VII.9 Anti-fork constraint (HARD) — exact reuse surface (+ FLAGGED names)

**Reuse (MUST — no forked math, no new organ, no second queue):**
- The ENTIRE LM-5 live API as-is: `r3_fact_learn` (`:918`), `r3_facts_pending` (`:975`),
  `r3_consolidate_idle_round` (`:1030`), the queue/eviction/refusal logic (`:925-944`),
  `s_round` (`:987`) — **zero signature changes, zero constant changes** (`R3_FQ_MAX`,
  `R3_IDLE_STEPS`, `R3_SLEEPS_PER_FACT`, `R3_TEACH_READS`, `R3_STREAM_LR`, `:817-827`).
- `s_build` (`:896`) for the verb's masked eval prompts; `h_predict` (`:626`); the
  `s_eval_fact` eval-loop pattern (`:1034`) with `S_SEED_HELD`; the pretrain recipe
  (`:1089-1095`) hoisted to in-file `s_pretrain()`.
- The dmn hook stays the SAME three lines (`dmn.c:118-120`) plus the one counter increment
  (VII.5); `dmn_trigger` semantics untouched.
- Shell-verb shape: the `handoff` branch pattern (`usermain.c:672-683`), `starts_with` +
  manual arg scan like every existing verb.

**FLAGGED — names that do NOT exist (create exactly as scoped, nothing more):**
- `mind_cmd(const UB*, UW)` — the ONLY new public in `r3_incontext.c`, declared in `dtr.h`
  beside `r3_stream_test` (`dtr.h:301`). All sub-verb logic, `m_ready`, `r3_round_busy`,
  `s_pretrain` stay file-static.
- `dmn_r3_rounds(void)` + static `dmn_r3_round_count` — `dmn.c`/`dmn.h`, the only new dmn
  surface.
- The `mind` shell verb — exists on NO dispatcher today (both linux usermains get it;
  `grep` confirms no collision: no existing verb starts with "mind").
- **arch/x86 bare-metal shell: NOT this slice** (recommendation, with reason): the x86
  shell has NO r3 verbs today (`arch/x86/usermain.c` greps clean for r3/handoff), the x86
  boot CI job drives only `ring3 test|mind` (`ci.yml:313`), so a `mind` verb there would be
  permanently ungated surface — the AUDIT-SPRAWL failure mode. It arrives with the first
  wave that gates an x86-shell r3 verb. → COMMANDER DECISION 4.
- **Do NOT:** add a new pretrain variant (one recipe, one seed); call `lm_consolidate_*` /
  `dtr_train_batch` / `gl_merge` (wrong network, the standing V.0 correction); add kdds
  topics (VII.7); parse anything beyond two decimal ints; touch `r3_stream_test`'s gates or
  numbers.

### VII.10 CI verb plan (specify only — do NOT edit `ci.yml` in this Part)

1. stdin sequence at `ci.yml:57`: append, AFTER `self test` (so every amnesia-bomb cert has
   already fired, VII.0 #5) and before `exit`:
   `mind teach 2 3\nmind wait 120\nmind ask 2\n` (the implementer substitutes the measured
   off-bias `(k*, v*)`, VII.6).
2. **Raise the native job timeout** `300 → 420` (`ci.yml:57`): the wait contributes ~15 s
   nominal/120 s worst-case + lazy pretrain seconds; 300 was already snug with LM-5 aboard.
   State the measured end-to-end time in the PR.
3. Four greps next to the `[stream-*]` block (`ci.yml:167-170`):
   `grep -aF '[teach-arrival] PASS'`, `grep -aF '[teach-live] PASS'`,
   `grep -aF '[teach-consolidated] PASS'`, and — the line this Part exists for —
   `grep -aF '[dmn] sleep: distilled in-context facts -> rw[]'` (now a PRODUCTION
   occurrence, not a cert print; it appears only if the dmn task itself consolidated).
4. ALL existing greps stay (no-regress); the aarch64-qemu subset (`ci.yml:221`) continues to
   omit r3-family verbs — consistent with R3/LM-4/LM-5. **The implementer wave edits
   `ci.yml`, not this document.**

### VII.11 Provenance / closes-on

Design only. This slice closes when `[teach-arrival]`, `[teach-live]`,
`[teach-consolidated]` and the production `[dmn] sleep: distilled in-context facts -> rw[]`
grep are green on a clean rebuild AND CI-enforced (native job), audited by a **separate**
agent on the **commander's** binary. The audit makes the acceptance test; the commander
reads the gate formula — the `mind wait` loop, the dmn.c counter site, and the quiesce flag
— line-by-line. One epitaph line in `gap-ledger.md`; no new ledger entries.

**COMMANDER DECISIONS NEEDED (recommended defaults):**
1. **Re-teach policy** (VII.2 step 4): refuse with printed reason (**recommended**) vs
   evict-and-replace the same-key fact. Replace is tempting ("the owner corrects the mind")
   but is belief revision wearing eviction's clothes: the old weight trace persists and
   nothing measures whether the new binding actually overwrites it — an unmeasured claim.
   Refusal is honest; revision gets its own slice with its own disease/cure cert.
2. **Cross-node teaching** (VII.7): defer (**recommended NO**, within-node first) vs ship
   the `mind/teach` kdds topic now. Deferral names the follow-up slice ("the shared mind").
3. **Queue budget** (VII.8): keep `R3_FQ_MAX = 4` (**recommended**) vs raise to 8 for
   singleton teaches. Keeping it preserves LM-5's audited eviction numbers byte-identical
   and the budget-honesty story; 4 live facts are enough to prove a mouth.
4. **x86 bare-metal shell verb** (VII.9): not this slice (**recommended**) vs wire it
   ungated. Ungated surface on the least-tested shell is sprawl, not generosity.

## Part VIII — the shared mind: a fact taught on node A is answerable from node B

> Status: **design + acceptance test** (written before implementation, like Parts II–VII).
> Owner of *this* slice: the next wave (separate implementer + separate auditor). Builds ON:
> **LM-6** (Part VII, `mind teach|ask|wait` + `mind_cmd` at `r3_incontext.c:1619`, the live
> DMN consolidation, the ONE provenance write site `ark_prov_record` at `:1496`, the galaxy
> `EV_TEACH`/`EV_CONSOLIDATE` emissions, the web POST `/teach` bridge `galaxy.c:814`),
> **LM-5** (the bounded fact queue + `r3_fact_learn`/`r3_facts_pending`/
> `r3_consolidate_idle_round` at `r3_incontext.c:1006/1063/1120`), **p-fs P1**
> (region-scoped gossip replication, `pfs_repl.h`, `pfs_dag_save`/`pfs_dag_read`
> `pfs_dag.h:145/153`), **ark-profile** (`self/prof` 1188 B + `self/prov` 48 B objects that
> already P1-replicate, `ark_profile.h:71/97`), **region** (`region_id()`
> `region.c:77`; region = peers within `REGION_TAU_MS=50` ms RTT, `region.h:24`). All closed
> in `gap-ledger.md`, so this is unblocked.

LM-5's VI.3 honest note, LM-6's VII.7 ("cross-node teaching — examined and deferred: NO"),
and the gap-ledger all named the SAME hole and the SAME successor: *"the shared mind —
`mind/teach` gossip + taught-fact provenance via the Self lineage … within-node first."*
This Part closes the within-node bound: **a fact taught on node A becomes answerable from
node B, from B's OWN weights, with B's OWN DMN doing the consolidating, and with the human
who taught it remembered across the mesh.** It is the literal Collective layer applied to the
Self layer — 人類の記憶 going collective. It is deliberately **region-scoped** (VIII.7): the
region's shared mind, not the planet's; federation is a later slice. And it is still the
synthetic 8×4 vocabulary (VII.8 stands) — own that loudly. It mirrors Parts II–VII's rigor
exactly: a claim, a falsifiable certificate with bracket tags + numeric bars, named honest
bounds, a HARD anti-fork surface flagging names that do not exist, a 2-process live cert, and
a CI verb plan.

### VIII.0 IMPORTANT — what the tree actually says (read BEFORE coding)

Six load-bearing facts. The first three pre-decide **the W-vs-E fork below** by arithmetic;
the rest correct the wave framing.

1. **The R3 model is 7172 floats — 28 688 bytes.** `R_NP = 7172` (`r3_incontext.c:94`,
   layout `:77-94`; verify by build, do not trust this number blind). A fact-ENGRAM is the
   `R3_FACT` struct, **24 bytes** (`_Static_assert(sizeof(R3_FACT)==24)`, `:935`); the
   minimal taught binding on the wire is **3 bytes** `(key, val, origin)`. The ratio is
   **~9 500×** (weights) vs **~1×** (an engram fits any single packet).
2. **One p-fs block is 4096 bytes; one K-DDS payload is 192 bytes.** `PFS_BLOCK_MAX = 4096`
   (`pfs_block.h:28`), `KDDS_DATA_MAX = 192` (`kdds.h:49`). The G22 carrier `gl_pfs_publish`
   **statically asserts the whole blob fits ONE block** (`gossip_learn.c:94`, `GL_MAXFLOATS =
   DTR_WEIGHT_FLOATS = 635`, 2548 B — fits). **The R3 model does NOT fit:** 28 688 B is 7×
   `PFS_BLOCK_MAX`, so Path W cannot even reuse `gl_pfs_publish` as-is — it needs a NEW
   multi-block split / chunked transport (the `pfs_repl.c` 512 B `PFSR_CHUNK_SIZE` path
   generalizes, but that is new code on a hot, weight-corrupting axis). A fact-engram, by
   contrast, fits one K-DDS payload with **189 bytes to spare**.
3. **`self/prof` and `self/prov` ALREADY cross the mesh.** p-fs P1 is region-scoped gossip
   replication of EVERY content-addressed block over K-DDS `pfs/ann`+`pfs/want`+a private
   512 B-chunk UDP port, all `KDDS_SCOPE_REGION` (`pfs_repl.h:10-21,72-75`). The
   `ark_prov_record` write site (`r3_incontext.c:1496` → `pfs_dag_save("self/prov", …)`,
   `ark_profile.c:309`) and `ark_profile_save("self/prof", …)` (`:257`) therefore **already
   replicate to same-region peers today** — provenance crossing the mesh is, to first order,
   *free and already shipping*. What does NOT cross today is the FACT itself (the `(k, ŷ_k)`
   that drives B's training) and the SIGNAL that tells B to enqueue it.
4. **`r3_fact_learn` is the only production mouth into the queue, and it is the right one for
   a remote arrival too.** It takes `(const UB *keys, const UB *vals, INT n)` with `n=1..8`
   (`:1006`, VII.0 #1), runs the frozen FAST read, enqueues the engram, calls `dmn_trigger()`
   (`:1037-…`), and is FIFO-budget-bounded at `R3_FQ_MAX=4` with printed eviction
   (`:1013-1029`). A fact arriving from B's network must enter through THIS function — the G33
   rule (the production mouth, not a test injection) extended across nodes.
5. **`dmn_trigger()` from a network task is safe; nothing live reads `rw[]` off the shell.**
   VII.0 #3/VII.4 built the `r3_round_busy` quiesce flag precisely because the shell verb path
   and the prio-13 round share `rc`/`rg`/`r_rng`. A remote-arrival task that calls
   `r3_fact_learn` runs the frozen FAST read over the SAME shared scratch — so it MUST take
   the same `m_quiesce()` discipline (VIII.2). The arrival task is otherwise just another
   producer, exactly like the shell `mind teach`.
6. **Region is emergent, not configured.** `region_id()` returns the lowest-id ALIVE peer
   within `REGION_TAU_MS=50` ms RTT (`region.c:60-82`); region-scoped K-DDS reaches exactly
   that set. So "the region's shared mind" has a real, observable boundary: a node at >50 ms
   RTT is OUTSIDE and provably does NOT receive the fact — which is the honest negative half
   of the `[shared-grounded]` cert (VIII.6 #3), not a limitation to apologize for.

### VIII.1 The claim to prove

> An owner at node **A** types `mind teach <k> <v>` (LM-6 unchanged): the fact enters A's queue
> with consent + provenance (`ark_prov_record`, the human resolved to A's profile). Within a
> **bounded** time and with **NO operator action on B**, the fact crosses the **region** via
> existing replication machinery, enters **B's** live queue through **`r3_fact_learn`** (the
> production mouth, G33), and **B's OWN DMN idle pulses** consolidate it into **B's `rw[]`**.
> Afterwards `mind ask <k>` **on B** answers **v** from B's weights on a MASKED prompt — and
> can name **who** taught it (A's profile, replicated via `self/prof`). A node OUTSIDE the
> region does **NOT** receive it. Cross-arch (aarch64 + x86_64 in one region).

This is strictly stronger than LM-6's within-node claim: there, one mind learned from its
owner; here, **B's mind learns from A's owner** — the thread crosses the galaxy and a distant
star answers. The Collective layer (the mesh) now carries not telemetry but *taught knowledge*,
and the Self layer (the human chapter) rides with it.

### VIII.2 THE FORK — Path W (weights travel) vs Path E (engrams travel)

Both were investigated to the numbers (VIII.0 #1–#3). **Recommendation: Path E for v1.** Path W
is the deeper "the mind literally becomes one" and is named as a later slice with ITS OWN
disease/cure cert (VIII.7) — but it is gated on an **unanswered empirical question** that this
slice should NOT smuggle past its audit.

**Path E (engrams travel) — RECOMMENDED.** After A enqueues, gossip the tiny fact-engram
`(k, ŷ_k, origin, fact_seq)` to region peers; each peer calls its OWN `r3_fact_learn` and its
OWN DMN consolidates it with its OWN gradients. The fact arrives at B as a *thing to learn*,
not as a weight delta.
- **Wire cost: one packet.** 24 B engram (or 3 B minimal binding) ≤ `KDDS_DATA_MAX=192`
  (VIII.0 #1-2). No chunking, no new transport, no multi-block split.
- **Each mind stays self-consistent.** B trains on B's own `r_backward`; B's `rw[]` is always a
  coherent product of B's own SGD — never a foreign average. There is no "two minds that
  learned different facts got blended" hazard, because nothing is blended.
- **Provenance rides naturally** — the engram carries `origin` + `fact_seq`, and `self/prov` +
  `self/prof` already replicate (VIII.0 #3); B's `mind ask` resolves the human (VIII.4).
- **The queue/FIFO/refusal semantics already exist** — a remote arrival is just another
  `r3_fact_learn` caller (VIII.0 #4); budget honesty, eviction, re-teach refusal all inherit.
- **Cost, owned:** the region's minds are **N copies converging**, not one substrate. They can
  momentarily disagree (A has consolidated, B has not yet) — but they converge to the SAME
  binding, and the disagreement window is bounded and observable (VIII.7). Conflicts (same key,
  different values from different people) surface EARLIER than in Path W — and that is GOOD: we
  set the honest v1 rule now (VIII.3) instead of discovering it inside a weight average.

**Path W (weights travel) — DEFERRED, with its cert named.** After consolidation, `gl_merge`
the R3 `rw[]` across nodes (the G22 pattern applied to R3). Pros: rides the proven no-central
machinery; the mind becomes literally one substrate. **Cons that gate it:**
- **Wire cost ~28 KB/merge round** (VIII.0 #1) — 11× the dtr body — and `gl_pfs_publish` does
  not fit it in one block (VIII.0 #2): new chunked-publish code on the weight axis.
- **THE empirical crux, unanswered:** G22's `gl_merge` was certified averaging models trained
  on DISJOINT shards of the SAME task (`gossip_learn.c` header; `samples/32_collective_learn`).
  Here A and B learned **DIFFERENT facts** (k1 on A, k2 on B). Does averaging two minds that
  each consolidated a different binding **preserve both** facts, or **corrupt both**? LM-1
  averaged dtr weights trained on the same stream's shards; this is categorically different —
  and `lm_consolidate.c:14-26` already warns that contradictory engrams cannot both survive a
  naive merge. **No measurement of "average two R3 minds, ask both for BOTH facts" exists.**
  Until a disease/cure cert answers it — *teach k1 on A, k2 on B, merge; do BOTH nodes answer
  BOTH keys, at what accuracy, and does interleaved post-merge replay rescue what the raw
  average loses?* — shipping Path W would be asserting an unmeasured "the mind becomes one"
  claim, the exact AUDIT-SPRAWL failure mode the project forbids.

**Verdict:** E ships the buzz demo (teach on your phone → friend's node answers) at one packet,
with every mind self-consistent and provenance free; W is the more beautiful "one mind" but is
**its own slice with its own cert** (VIII.7). This fork is **not genuinely close** once the
numbers are in (9 500× wire, an unanswered averaging question on the weight path) — so it is a
*recommendation, not a COMMANDER DECISION*. The commander may still elect W-first; if so, the
averaging-of-divergent-minds cert (VIII.7) is mandatory and replaces VIII.6 wholesale.

### VIII.3 The transport (Path E) — which existing machinery carries the fact

Two honest sub-options for the engram hop; **recommend the K-DDS topic** (cheapest, matches the
"a fact is a small live message" shape), with the p-fs-object fallback named.

**Recommended: a region-scoped K-DDS topic `mind/teach`.** Open once at boot with
`kdds_open_poll_scoped("mind/teach", KDDS_QOS_LATEST_ONLY, KDDS_SCOPE_REGION)`
(`kdds.h:139`, the dkva poll pattern — no `CFN_MAX_SEMID` cost). The publisher is A's
`mind teach` AFTER a successful local enqueue; the payload is a fixed wire struct **well under
`KDDS_DATA_MAX=192`**:

    #define MT_MAGIC  0x4854454DUL          /* "METH"? -> pick a free LE magic, FLAGGED */
    typedef struct {
        UW magic;            /* MT_MAGIC                                      */
        UW fact_seq;         /* A's R3_FACT.seq — the autobiographical when    */
        U1 origin_node;      /* drpc_my_node of A (the teacher's node)         */
        U1 key, val;         /* the declared binding (n=1 singleton, VII.0#1)  */
        U1 src;              /* ARK_PROV_SRC_SHELL/WEB (carried for the prov)  */
        U1 prov_head[PFS_ID_LEN];  /* content-id of A's ARK_PROV (resolves the */
                             /* human via the already-replicated self/prov)    */
    } __attribute__((packed)) MT_TEACH_PKT;  /* 4+4+1+1+1+1+32 = 44 B          */

A subscriber **task on B** (a new low-priority `mind_net_task`, mirroring `pfs_repl_task`) polls
the topic, and for each unseen `(origin_node, fact_seq)` pair (the dkva.c-style
once-per-(src,seq) dedup, `pfs_repl.h:89-90`):
1. **dedup + loop-prevention:** ignore packets where `origin_node == drpc_my_node` (A's own
   fact gossiped back to A — the standing gossip loop trap); track a small per-origin
   last-seq high-water so a re-published LATEST_ONLY slot is acted on **once**.
2. **quiesce** (VIII.0 #5): `while (r3_round_busy) tk_dly_tsk(20);` — the VII.4 flag, now also
   guarding a network producer (the frozen read shares `rc`/`rg`/`r_rng`).
3. **arrival = `r3_fact_learn(&key, &val, 1)`** — THE production mouth (VIII.0 #4, G33). B's
   frozen FAST layer reads its OWN SUPPORT prompt and stores ŷ_k (B may read differently from
   A — honest: each mind memorizes its own reading; the engram is the reading, not the oracle,
   VI.3/VII.8). `r3_fact_learn` calls `dmn_trigger()` → B goes ACTIVE → idle → B's own pulses
   consolidate.
4. **provenance:** record B's view of the remote prov — `ark_prov_record(fact_seq, key, val,
   src)` with `prov_head` retained so `mind ask` on B resolves it to A's profile (VIII.4). (B
   does NOT re-author the human's consent; it records "node A's owner taught this, here is the
   pointer," the tamper-evident lineage stance of III.6.)
5. **budget honesty inherited:** B's `R3_FQ_MAX=4` is shared by local + remote facts; a remote
   flood FIFO-evicts the oldest RETAINED fact with the LM-5 printed forgetting (`:1013-1029`) —
   a remote node cannot silently evict B's locally-taught facts beyond the same honest, printed
   rule. **Rate rule:** at most one `r3_fact_learn` per polled `(origin, seq)`; the poll cadence
   (≥500 ms, `pfs_repl_task` style) bounds the arrival rate structurally — no remote node can
   spin B's queue faster than B polls.

**Fallback (named, not built): a p-fs object `mind/teach/<origin>`.** `pfs_dag_save` of the
44 B `MT_TEACH_PKT` would replicate identically to `self/prov` (VIII.0 #3), giving durable,
content-addressed, death-surviving delivery for free — at the cost of a per-origin ref and the
pfs_dag versioning ceremony. Recommended ONLY if the cert needs delivery to survive A's death
mid-gossip (out of v1 scope; the K-DDS LATEST_ONLY slot is sufficient for "A teaches, B answers"
while both live). **COMMANDER DECISION 1** (VIII.10).

**Loop prevention, stated once more because it is the classic trap:** every node both publishes
and subscribes `mind/teach`; the `origin_node == me` drop (step 1) is the only thing preventing
a fact from ping-ponging. The auditor greps for that guard explicitly (VIII.9).

### VIII.4 Provenance across the mesh — the star's name from another galaxy

The human is already on B. `ark_profile_save` writes `self/prof` (1188 B, one block,
`ark_profile.h:71-74`) and it P1-replicates region-wide (VIII.0 #3); `ark_prov_record` writes
`self/prov` carrying `profile_head[PFS_ID_LEN]` (`ark_profile.h:94`), the content-id of the
exact profile version in force. The `MT_TEACH_PKT` carries `prov_head` = the content-id of A's
`ARK_PROV` for this fact. So on B:
- `mind ask <k>` for a fact whose engram came from a remote origin prints, beyond `pred`/`share`,
  a provenance line: `taught by node <origin>` and, when B has received A's `self/prof` block
  (it has, region-replicated), the handle/name the human chose to disclose (or "anonymous" when
  `profile_head` is all-zero — the consent-without-disclosure case, `ark_profile.h:95-96`,
  honored honestly).
- The chain is end-to-end: A's owner → A's `ARK_PROV` (`prov_head`) → A's `ARK_PROFILE`
  (`profile_head`) → replicated to B → resolved at B's `mind ask`. **人類の記憶 going
  collective**, with the consent gate intact (B never invents a human; it points at A's
  consented record).

Honest bound: this is **tamper-EVIDENT**, not unforgeable — there is no signing primitive yet
(III.6); a malicious node could publish a `mind/teach` with a forged `prov_head`, and B would
resolve it to whatever profile that id names (or fail to resolve → "unknown teacher"). The
immune system (signed provenance) is named in VIII.7, not built — exactly VII.7's reason #2,
now scoped to a SINGLE region of mutually-RTT-close (≤50 ms) peers, a far smaller trust surface
than the planet.

### VIII.5 Conflict honesty — same key taught differently on two nodes

The belief-revision trap surfaces earlier here than within-node (VIII.2), so set the honest v1
rule explicitly:

> **v1 rule: LOCAL teach wins; a remote arrival for a key already QUEUED on B is REFUSED and
> printed.** This mirrors LM-6's within-node re-teach refusal (VII.2 step 4, `:1469-1473`)
> extended across nodes: if B already holds key `k` (locally taught OR a prior remote arrival),
> an incoming `mind/teach` for `k` with a different `val` prints
> `[mind] remote teach key K from node N refused — already bound here (belief revision is a
> future slice)` and does NOT enqueue. If the remote `val` MATCHES B's binding, it is a
> harmless duplicate (dedup drops it silently).

Rationale, the standing one: replay provably cannot hold a contradiction
(`lm_consolidate.c:14-26`), so "the network overwrites B's binding" would be an unmeasured
belief-revision claim. Refusal is honest and printed; **belief revision (local OR remote) stays
its own slice with its own disease/cure cert** (VIII.7). **What the galaxy shows:** the refused
remote teach still emits a galaxy event (a fact-particle that arrived but did not land — a
deflected ray), so the conflict is *observable*, not silent (VIII.9).

Region-scoped means conflicts are bounded to ≤50 ms-RTT peers; cross-region reconciliation is
federation (VIII.7). **COMMANDER DECISION 2** (last-writer-wins vs refuse) — recommend
**refuse**, for the same reason VII chose refuse over evict-and-replace.

### VIII.6 The falsifiable acceptance test (the certificate) — 4 tags

The cert is a **2-process region mesh** (A and B, mirroring `samples/32_collective_learn/run.sh`
and the `[galaxy-events]` 3-node pattern, `ci.yml:225`): both `./p-kernel` with `AUTONET`, same
relay, RTT < `REGION_TAU_MS` so they form ONE region; a third node placed OUTSIDE the region for
the negative half of tag 3. All numbers printed, then a canonical `[tag] PASS/FAIL` line.
`chance = 25%`. Bars are **proposals**; the implementer reports actuals and may LOWER only to
measured-minus-flake-margin, FLAGGED — never inflate. The cert's `(k*, v*)` is the LM-6
off-bias pick (VII.6), measured on the 0xA5A5 substrate.

1. **`[shared-arrival]` — B's queue gains A's fact via the REAL transport, not a test
   injection.** On A: `mind teach k* v*` (the unchanged LM-6 verb). On B, with NO operator
   action: within a bound (propose 30 s), `r3_facts_pending()` on B becomes 1 for a fact whose
   `origin_node == A` and `seq == ` A's printed seq. PASS requires ALL, printed: B received the
   `mind/teach` packet (poll log line); B's enqueue came through `r3_fact_learn` (the auditor
   greps that `mind_net_task` calls ONLY `r3_fact_learn`, never an internal queue poke); the
   loop-guard fired on A (A dropped its own echo: `origin==me` count ≥ 1). A node whose
   `origin==me` enqueues nothing (no self-teaching from the echo).

2. **`[shared-consolidated]` — B answers v from B's OWN weights after B's OWN dmn pulses.**
   Pre-state on B: `pre_share(k*) <= chance+8` (=33) — the fact is NOT already in B's weights
   (the disease half, inherited from `[handoff-fast-only]`). Then with NO operator action, B's
   DMN drains the fact: `dmn_r3_rounds()` on B increments by `>= R3_SLEEPS_PER_FACT` (=10) —
   rounds only B's `dmn.c:135` call site can count — and the live print
   `[dmn] sleep: distilled in-context facts -> rw[]` appears in **B's** log. Then `mind ask k*`
   on B: `share(k*) >= 75` over N=40 masked held-out arrangements (the LM-6 bar; LM-6 measured
   100 post-sleep). PASS requires ALL the above AND a printed
   **`teach@A=<t_A>  answer@B=<t_B>  delta=<s>`** wall line (A's teach time → B's answer time —
   the thread's flight time, bounded, printed not gated, the VII.5 flakiness discipline).

3. **`[shared-grounded]` — provenance resolves to A's profile; the region boundary is real.**
   ALL printed: on B, `mind ask k*` names the teacher — `taught by node A` and A's disclosed
   handle (or "anonymous" if A declined disclosure — print which, honestly), resolved through
   the region-replicated `self/prof`/`self/prov` (VIII.4); AND the **negative half**: a third
   node **C placed OUTSIDE the region** (RTT > `REGION_TAU_MS`, or simply not in A/B's
   region-scoped K-DDS fanout) has `r3_facts_pending()==0` for k* and `mind ask k*` answers
   only the substrate prior (`pre_share`-level) — the region-scope honesty made falsifiable: the
   shared mind is the REGION's, and a node outside does not receive it.

4. **`[shared-live]` — a 2-process mesh, kill-tolerant, galaxy-visible (the discipline tag).**
   Structural + live gates, printed: facts entered ONLY via `r3_fact_learn` on BOTH nodes (the
   auditor greps `mind_net_task` for the single `r3_fact_learn` call and NO direct queue/round
   poke); the loop-prevention guard present (`origin==me` drop, greppable); the conflict rule
   exercised (a remote teach of an already-bound key on B prints the refusal line, VIII.5); AND
   the galaxy shows the thread — `EV_REMOTE_TEACH` (VIII.9) emitted ONCE on B at the arrival
   site, observable in B's `/events` stream, with A's star and B's star both flashing in the
   same region (the `kill_one.sh`-style 2-process harness; killing A AFTER B consolidated does
   NOT un-teach B — B's `rw[]` already holds it, the Collective survives the source's death,
   §3). Budget honesty: a remote flood beyond `R3_FQ_MAX` FIFO-evicts with the LM-5 printed
   forgetting; printed, not gated.

(Considered and **dropped**: a weight-merge tag — that is Path W's `gl_merge`-of-`rw[]` axis,
explicitly deferred (VIII.2/VIII.7), and gating it here would smuggle the unanswered averaging
question past the audit; a multi-fact-flood tag — `[shared-live]` already gates eviction with
ONE extra fact, N facts prove only patience; a cross-region federation tag — VIII.7's slice.)
**No-regress is CI-level:** every existing grep stays green — the LM-6/LM-5 in-process certs
(`[teach-*]`, `[stream-*]`, `[handoff-*]`, `[dmn-*]`, `[salience-*]`, `[self-*]`, `[r3-*]`) are
UNTOUCHED; the shared-mind code ADDS a network task + one publish in `mind teach`, changing no
existing math, no existing constant, no existing number.

### VIII.7 The honest bound (what is NOT claimed)

- **Region-scoped, NOT planetary.** The shared mind is the set of peers within
  `REGION_TAU_MS=50` ms RTT (`region.c`). Cross-region propagation is **federation**
  (`docs/architecture/federation.md`) — a named future slice; this slice proves and BOUNDS the
  region's mind, and tag 3 makes the boundary falsifiable, not hidden.
- **Path W (one-mind weight merge) is DEFERRED with its cert stated.** The future "one
  substrate" slice is: `gl_merge` the R3 `rw[]` across nodes (G22 generalized off dtr's 635
  params to R_NP=7172), chunked over `PFSR_CHUNK_SIZE` since it exceeds one block. **Its
  acceptance test is mandatory and is the question this slice refuses to smuggle:** *teach k1
  on A, k2 on B, merge `rw[]`; gate that BOTH nodes answer BOTH k1 AND k2 at ≥ chance+margin
  (does raw averaging of two divergent minds preserve both facts, or corrupt both?), and that
  interleaved post-merge replay rescues whatever the raw average loses (the disease→cure shape,
  on the merge).* Until that cert is green, "the mind is literally one" is unproven.
- **Synthetic vocabulary, NOT language** (VII.8 stands) — `teach 2 3` is a key→value binding.
- **N converging copies, not one substrate (Path E).** A and B answer the same binding but
  have distinct `rw[]`; there is a bounded, observable disagreement window before B consolidates.
  Convergence to the SAME binding is the claim; weight-identity is NOT (that is Path W).
- **Each mind memorizes its OWN reading.** B's frozen FAST layer reads the SUPPORT prompt with
  B's substrate; a misread on B is rehearsed wrong on B (the VI.3/VII.8 ceiling, per-node).
- **Tamper-EVIDENT provenance, not unforgeable** (VIII.4) — no signing primitive (III.6); a
  region of ≤50 ms-RTT peers is a smaller trust surface, but the immune system (signed
  `mind/teach`) is named, not built.
- **No belief revision** (local OR remote) — same-key conflict is REFUSED and printed (VIII.5);
  revision is its own slice with its own cert.
- **Delivery is best-effort while both live** (K-DDS LATEST_ONLY); durable, death-surviving
  delivery is the p-fs-object fallback (VIII.3, DECISION 1), out of v1 scope.
- **Cert verbs remain amnesia bombs** (VII.0 #5); persisting `rw[]` across runs is a named
  non-goal here.

No false "by construction" theorem is asserted.

### VIII.8 The live idle path is unchanged (B uses the LM-6 machinery exactly)

The whole point of Path E is that **B's consolidation side is byte-identical to LM-6.** The
remote arrival enters B's queue through the same `r3_fact_learn`; B's `dmn_idle_work`
(`dmn.c:134-139`) consolidates pending facts every idle pulse with the same bounded round and
the same `dmn_r3_round_count++` at the same single site; `mind ask` reads B's `rw[]` with the
same masked vote. The ONLY new code is **the mouth that feeds B's queue from the network**
(`mind_net_task` + the `mind/teach` publish in A's `mind teach`) — the dmn.c hook, the round,
the counter, the queue, the eviction are all reused verbatim (VIII.9). This is why the slice is
small and why its no-regress story is strong: it adds a producer, not a second brain.

### VIII.9 Anti-fork constraint (HARD) — exact reuse surface (+ FLAGGED names)

**Reuse (MUST — no forked queue, no second consolidation path, no second Transformer):**
- The ENTIRE LM-6/LM-5 live API as-is: `r3_fact_learn` (`:1006`), `r3_facts_pending` (`:1063`),
  `r3_consolidate_idle_round` (`:1120`), the queue/eviction/refusal logic (`:1013-1029`),
  `mind_cmd`/`m_quiesce`/`m_masked_vote`/`m_find_key` (`:1619`/VII path) — zero signature
  changes, zero constant changes (`R3_FQ_MAX`, `R3_IDLE_STEPS`, `R3_SLEEPS_PER_FACT`).
- The dmn hook stays the SAME (`dmn.c:134-139`) + the SAME counter; `dmn_trigger` untouched.
- The transport machinery: `kdds_open_poll_scoped`/`kdds_pub`/`kdds_sub` (`kdds.h:139/154/160`),
  `KDDS_SCOPE_REGION`; the `pfs_repl_task` poll/dedup PATTERN (`pfs_repl.h`, mirror, do NOT
  fork its block plane); `region_id()` (`region.c:77`).
- Provenance as-is: `ark_prov_record` (`:1496`), `ark_profile_head`/`self/prof` replication
  (`ark_profile.c`), `prov_head`/`profile_head` resolution — record B's view, do NOT re-author
  consent.
- The galaxy hook PATTERN: one `galaxy_emit` at one site (mirror `dmn.c:137`, `r3_incontext.c:1509`).

**FLAGGED — names that do NOT exist as callable API (create exactly as scoped, nothing more):**
- `mind_net_task(INT, void*)` — the new region subscriber task (in `r3_incontext.c`, so its
  helpers stay file-static; declared in `dtr.h` beside `mind_cmd`). Created in BOTH hosted
  usermains next to `pfs_repl_task`/the DMN task. The ONLY new public callable for arrival.
- `MT_TEACH_PKT` + `MT_MAGIC` + the `"mind/teach"` topic name — new wire surface; pin its size
  with `_Static_assert(sizeof(MT_TEACH_PKT) <= KDDS_DATA_MAX)`; pick a free LE magic (grep the
  existing `*_MAGIC` set first — `GL_BLOB_MAGIC`, `KDDS_MAGIC`, `PFSR_*_MAGIC`, `ARK_*_MAGIC` —
  to avoid collision).
- `EV_REMOTE_TEACH` = **15** in `galaxy.h` (next after `EV_SUMMARY=14`; `a = origin_node`,
  `b = key`) — the ONE new galaxy event; emitted at EXACTLY ONE site, B's arrival in
  `mind_net_task`, the thread crossing the galaxy + the distant star flashing (VIII.10).
- The `mind/teach` PUBLISH line inside `mind teach` (`m_teach`, `:1446`): added AFTER the
  successful local enqueue + `ark_prov_record`, gated on `n==1` (singleton; the cert geometry),
  carrying `r3_fq[r3_fq_n-1].seq`/`yhat`-or-`val`/`drpc_my_node`/`prov_head`. **One publish,
  one site.**
- **Do NOT:** call `gl_merge`/`gl_pfs_publish` on `rw[]` (Path W, deferred — VIII.2/VIII.7);
  add a second queue or a second consolidation path; let `mind_net_task` call
  `r3_consolidate_idle_round`/`s_round` directly (arrival ONLY via `r3_fact_learn`; the round
  belongs to the DMN — the auditor greps this); re-author consent on B; parse natural language;
  touch any existing cert's numbers; open `mind/teach` GLOBAL-scoped (region-scope is the claim).

### VIII.10 The galaxy hook — the thread crossing the galaxy

ONE emission point: inside `mind_net_task`, at the moment a remote arrival is accepted into B's
queue (right after the `r3_fact_learn` returns 0), emit
`galaxy_emit(EV_REMOTE_TEACH, A_origin_node, drpc_my_node, (UH)key, (UH)val)`. In the galaxy UI
this is the literal buzz image: a thread leaves A's star, crosses the region, and B's distant
star flashes as the fact lands — `src = A` (where it came from), `dst = B = me` (where it
landed). A REFUSED remote teach (conflict, VIII.5) emits the same event with a deflect flag in a
spare bit (or simply is NOT emitted as ARRIVAL but logged) so the conflict is observable, not
silent. This is the only new event; `EV_TEACH`/`EV_CONSOLIDATE`/`EV_ASK` on B fire unchanged
from LM-6 (B's own consolidation and answer), so the full cross-node story is already visible:
`EV_TEACH`@A → `EV_REMOTE_TEACH`@B → `EV_CONSOLIDATE`@B → `EV_ASK`@B.

### VIII.11 CI verb plan (specify only — do NOT edit `ci.yml` in this Part)

1. A NEW live multi-node cert job `shared-mind-live` (mirror `collective-learn-live`
   `ci.yml:529-559` and `samples/32_collective_learn/run.sh`): a new
   `samples/NN_shared_mind/run.sh` that boots a relay + node A + node B in ONE region (RTT <
   `REGION_TAU_MS`) + node C OUTSIDE the region (tag 3 negative half); drives `mind teach k* v*`
   on A over stdin; polls B until `[shared-arrival]`/`[shared-consolidated]`/`[shared-grounded]`/
   `[shared-live]` print PASS; greps `RESULT: PASS`. Cross-arch via the existing
   `PKERNEL_BOOT_DIR`/`PKERNEL_WRAP` overrides (aarch64 A + x86_64 B in one region, the
   `moment_2026_05_22_xarch_mesh` property).
2. Timeout: `timeout 600` (the `survival-loop` budget, `ci.yml:459`) — the teach→arrive→
   consolidate→answer chain is ~30 s nominal (idle re-entry + 10 rounds + poll latency); 600 is
   ~20× margin and matches the existing live-mesh jobs.
3. Four greps in the new job: `[shared-arrival] PASS`, `[shared-consolidated] PASS`,
   `[shared-grounded] PASS`, `[shared-live] PASS`, plus the production
   `[dmn] sleep: distilled in-context facts -> rw[]` grep on **B's** log (the print this slice
   makes fire on a node that was never directly taught).
4. ALL existing greps stay (no-regress); the native in-process job (`ci.yml:67`) is UNCHANGED —
   the shared-mind cert is LIVE-only (two processes), like every other Collective-layer proof.
   **The implementer wave edits `ci.yml` + adds the sample, not this document.**

### VIII.12 Provenance / closes-on

Design only. This slice closes when `[shared-arrival]`, `[shared-consolidated]`,
`[shared-grounded]`, `[shared-live]` are green on a clean rebuild AND CI-enforced (the new live
2-process job), audited by a **separate** agent on the **commander's** binary — not the
implementer's. The audit makes the acceptance test; the commander reads the gate formula —
the `mind_net_task` arrival path (single `r3_fact_learn`, no direct round call), the loop-guard
(`origin==me` drop), the `mind/teach` publish site, and the ONE `EV_REMOTE_TEACH` emission —
line-by-line. One epitaph line in `gap-ledger.md`; no new ledger entries; no new
`philosophy-gap-audit`.

**COMMANDER DECISIONS NEEDED (recommended defaults):**
1. **Engram transport** (VIII.3): region-scoped K-DDS `mind/teach` topic (**recommended** —
   cheapest, "a fact is a live message") vs a p-fs `mind/teach/<origin>` object (durable,
   death-surviving, more ceremony). K-DDS is enough for "both live"; the p-fs fallback is the
   federation/durability slice.
2. **Same-key cross-node conflict** (VIII.5): LOCAL-wins + remote-refused-and-printed
   (**recommended**, mirrors VII's refuse) vs last-writer-wins. Refuse keeps belief revision a
   measured future slice; last-writer is an unmeasured overwrite claim.
3. **Path W vs E** (VIII.2): **Path E for v1 (recommended; not a close fork — 9 500× wire +
   an unanswered averaging-of-divergent-minds question gate W)**. Surfaced as a decision only
   because W is the deeper "one mind" north star; if the commander elects W-first, the
   averaging disease/cure cert (VIII.7) is MANDATORY and replaces VIII.6.
4. **The negative-control node C** (VIII.6 #3): include an out-of-region node to falsify the
   region boundary (**recommended** — it is the honest half of `[shared-grounded]`) vs a
   2-node cert only (cheaper CI, weaker claim). Recommend include; it is the difference between
   "B got it" and "the REGION'S mind got it, and only the region."

## Part IX — the language slice: REAL WORDS in, REAL WORDS out (one-token answers)

> Status: **design + acceptance test** (written before implementation, like Parts II–VIII).
> Owner of *this* slice: the next wave (separate implementer + separate auditor). Builds ON:
> **LM-7** (Part VIII, the shared mind: `mind_net_task` + the `MT_TEACH_PKT` wire `dtr.h:324`,
> `m_publish_teach`/`mind_net_open` `r3_incontext.c`, the region-scoped `mind/teach` topic),
> **LM-6** (Part VII, `mind teach|ask|wait` + `mind_cmd`, the ONE provenance write site
> `ark_prov_record`, the web POST `/teach`/`/ask` bridge `galaxy.c:815/849`, the `galaxy.html`
> teach/ask dropdowns), **LM-5** (the bounded fact queue + `r3_fact_learn`/`r3_facts_pending`/
> `r3_consolidate_idle_round`), **R3** (the in-context substrate `r_forward`/`r_backward`,
> `R_NP`/`R_KEYV`/`R_VALV`/`R_DM`/`R_NPAIR`, the `[r3-incontext-gradcheck]` discipline),
> **ark-profile i18n** (`ark_manifesto_*` — the 32-language audience). All closed in
> `gap-ledger.md`, so this is unblocked.

This is **the longest-named honest bound in the whole ledger.** EVERY living-mind epitaph since
wave-21 carries the same disclaimer, verbatim: *"toy R3 synthetic vocab (NOT natural language)"*
(LM-4), *"a teach is `k∈0..7 → v∈0..3`, NOT language"* (LM-6), *"synthetic vocab"* (LM-7). The
mouth is real (a human, a prompt, a region of machines). The WORDS are not. This Part moves the
mind **from `teach k∈0..7 → v∈0..3` toward REAL WORDS** — and does so by the smallest falsifiable
step that the substrate's measured capacity can actually carry, NOT the dream. The headline of the
certificate is **a number: how many real word-bindings the substrate honestly holds at ≥75%
recall** — the capacity curve IS the claim. It mirrors Parts II–VIII's rigor exactly: a claim, a
falsifiable certificate with bracket tags + numeric bars, a HARD anti-fork surface, named honest
bounds, a versioned wire, the i18n-honest UI, and a CI verb plan.

### IX.0 IMPORTANT — what the tree actually says (read BEFORE coding)

Seven load-bearing facts. The first four pre-decide **the (a)/(b)/(c) fork below by arithmetic**;
the rest correct the wave framing. **Verify every number by build; do not trust this doc blind.**

1. **The R3 substrate is 7172 floats, and its capacity is set by `R_DM`, NOT by vocab size.**
   `R_NP = 7172` (`r3_incontext.c:100`, layout `:82-100`). The parameter families are: embeddings
   **704 B** (`R_KEYV·R_DM + R_VALEMB·R_DM + R_SEQ·R_DM = 8·32 + 5·32 + 9·32`), attention **4096**
   (`3·R_NH·R_DH·R_DM + R_DM·R_DM`), FFN **2112**, LayerNorm **128**, classifier **132**
   (`R_VALV·R_DM + R_VALV`). The comment at `:76-77` is the load-bearing fact: *"8-way recall needs
   `R_DM=32` capacity; `R_DM=16`/`R_NH=2` stays at chance — measured, not assumed."* So the
   **associative-reasoning bottleneck is the attention width `R_DM`**, which scales the model
   quadratically; the **vocabulary** lives only in the embedding (`R_KEYV·R_DM`, `R_VALEMB·R_DM`)
   and classifier (`R_VALV·R_DM`) tables, which scale **linearly** and are independent lookups.
2. **Widening the vocabulary is cheap; widening the reasoning is expensive.** Measured `R_NP` by
   the layout formula (verify by build): `(R_KEYV=256, R_VALV=64, R_DM=32)` → **19 008 floats**
   (≈2.65× today, still one allocation of statics); `(256, 64, R_DM=48)` → **31 536**;
   `(256, 64, R_DM=64)` → **50 240**. The vocab knobs `R_KEYV`/`R_VALV` move `R_NP` by ~1–2 K each;
   the reasoning knob `R_DM` moves it by 12 K per step. **The honest v1 widens vocab at `R_DM=32`
   and lets the capacity cert MEASURE whether 32-wide attention disambiguates the words** — it may
   need `R_DM=48`; the cert decides, and the doc forbids guessing.
3. **The per-episode binding budget is `R_NPAIR`, NOT the vocabulary.** `gen_episode` (`:133`)
   places `R_NPAIR=8` distinct key→value pairs per prompt + 1 query; the model must read the ONE
   binding the query asks for. Vocabulary is **how many distinct words can be NAMED** across
   episodes; `R_NPAIR` is **how many bindings coexist in ONE prompt**. A 256-word vocabulary with
   `R_NPAIR=8` means "any of 256 words may appear, 8 at a time." Raising `R_NPAIR` lengthens
   `R_SEQ` and costs attention quadratically (fact #1); the cert below holds `R_NPAIR` and sweeps
   the **number of taught word-bindings carried in the live queue** (`R3_FQ_MAX`-bounded), which is
   the user-visible "how much can I tell it" number.
4. **The wire has 189 bytes to spare; token ids fit trivially.** `MT_TEACH_PKT` is **44 B**
   (`dtr.h:334`, `_Static_assert(sizeof(MT_TEACH_PKT) <= KDDS_DATA_MAX)` at `r3_incontext.c:38`,
   `KDDS_DATA_MAX=192`). Today `key`/`val` are `U1` (`dtr.h:329`). A 256-word vocabulary still fits
   `U1`; a >256-word vocabulary needs `U2` token ids (+2 B → 46 B, still 146 B to spare). **The
   wire change is a 2-byte field-width bump + a version field, NOT a new transport** — the Path E
   "one packet" property (VIII.0 #1) is preserved with enormous headroom.
5. **The tokenizer must be SHARED by the kernel and the UI, and must live where both can reach it.**
   The kernel maps a word → token id at `mind teach`/`mind ask`; the galaxy page maps the user's
   typed word → the same id (or shows the kernel's word back). The current dropdowns
   (`galaxy.html:383` `fill("tk",8);fill("tv",4)`) hard-code 8/4 as integers. A word list embedded
   like the manifestos (`ark_manifesto_*`, one byte image hashed by `pfs_id_compute`) is the
   established pattern — the vocab is a content-addressed asset, served to the UI by a new
   `GET /vocab` route, so **the UI and kernel provably agree by content-id** (a vocab mismatch is
   detectable, not silent). It lives in `arch/common/` (a new `r3_vocab.c` + `r3_vocab.h`),
   reachable by `r3_incontext.c`, `galaxy.c`, and the cert.
6. **OOV is an honest REFUSAL, not a guess.** A word not in the fixed list has no token id. The
   mouth must PRINT the refusal (`[mind] word "..." not in vocabulary (N words); refused`) exactly
   as re-teach is refused today (`r3_incontext.c:1618`). Silently hashing OOV words into collisions
   would manufacture fake bindings — the opposite of the project's honesty discipline.
7. **The certs are pinned to the OLD dims; widening forces a re-baseline that must be LOUD.**
   `r3_test`/`r3_handoff_test`/`r3_stream_test` all hard-code `R_KEYV=8`/`R_VALV=4` numbers (the
   `DSTAR`/`SDICT` tables `:668/:979` are 8 entries; gates like `acc_masked_pre <= 33` assume
   `chance = 100/R_VALV = 25%`). Changing `R_VALV` changes `chance` and every margin. **The certs
   either (a) compile against a frozen `R_VALV_LEGACY` view, or (b) are re-baselined with new
   printed numbers.** This Part RECOMMENDS (b) with a loud epitaph (IX.5) — a silent re-baseline
   that lowered a bar would be exactly the immune-system failure the audit exists to catch.

### IX.1 The claim to prove

> A person types **real words** in the galaxy page (or `mind teach sky blue` at the shell): the
> word "sky" maps to a vocabulary token, the word "blue" to an answer token; the fact enters the
> SAME bounded queue via `r3_fact_learn` (LM-5/6 unchanged in mechanism); B's — or the same node's
> — OWN DMN consolidates it during sleep; `mind ask sky` answers **"blue"** (a real word) from the
> weights on a MASKED prompt; the answer crosses the region over the **versioned** `mind/teach`
> wire (LM-7, token-id payload); and the certificate **MEASURES and PRINTS how many real
> word-bindings the substrate holds at ≥75% recall** — the capacity curve is the headline.

The task stays **associative recall** — the proven machinery (LM-4..7 all transfer unchanged in
structure) — but over **real word tokens** instead of `k∈0..7 → v∈0..3`. This is the honest
"the mind remembers what you tell it, in words." It is **NOT generation, NOT grammar, NOT chat,
NOT multi-token answers, NOT belief revision** (IX.7 owns each bound loudly).

### IX.2 THE FORK — (a) word-level recall vs (b) byte/char sequences vs (c) BPE generative LM

The wave must pick ONE. The arithmetic of IX.0 #1–3 pre-decides it.

- **(c) a real BPE tokenizer + a tiny GENERATIVE LM.** The full dream: subword tokenization, a
  causal LM that *produces* novel word sequences. **Capacity math says NO at 7 K params, and says
  so with a number.** A useful generative English LM needs an embedding table of (≥8 K subwords ·
  d_model) PLUS a reasoning stack deep/wide enough to model grammar — at `d_model=32` that
  embedding alone is **256 K floats** (>35× the whole current model), and the reasoning capacity
  (fact #1: grammar is not 32-wide-attention-shaped) is the harder wall. **What WOULD it take?** A
  conservative tiny-but-real generative LM (≈8 K-token BPE, `d_model=256`, 4 layers) is
  **~25–50 M params** — **~3 500–7 000×** today. That is not a knob; it is a different organism,
  and it ties directly to the fleet: such a model **cannot live in one node's `rw[]`** and would
  have to run **tensor-parallel across the region**, which is *exactly the machinery the dtr sensor
  side already proved* (DKVA / distributed KV attention over the relay, `gap-ledger.md`). **(c) is
  named here as the future arc and explicitly handed to the distributed-inference substrate**, not
  attempted in this slice. Naming the param budget IS the honest deliverable for (c).
- **(b) byte/char-level sequence memory.** Tokenizer = bytes (language-neutral, zero word list).
  But teaching arbitrary strings requires a **sequence OUTPUT** — multi-token answers — which is a
  **structural change to R3's single-class readout** (`r_forward` reads `R_VALV` classes from ONE
  query position, `:213-215`; `r_backward` `dlog[R_VALV]` `:230`). That means autoregressive
  decoding, a new loss over a sequence, and a new gradcheck surface. It is a real, honest slice —
  but it is a **bigger structural lift than the substrate's proven single-token recall**, and it
  buys multi-byte words at the cost of leaving the LM-4..7 machinery that all transfers. **Named as
  the slice AFTER (a): "the mind answers in a SHORT PHRASE, not one word."**
- **(a) WORD-level associative recall — THE RECOMMENDED v1.** A fixed small vocabulary of common
  words; `key` words = the things asked-about, `val` words = the one-word answers. The task stays
  **single-token associative recall** — `r_forward`/`r_backward` UNCHANGED in structure (only
  `R_KEYV`/`R_VALV`/the embedding-table sizes change, exactly the "legitimately task-specific"
  surface the anti-fork rule already blesses at `:16-18`). Real words in, **one real word out.**
  The wire carries token ids. The UI becomes text fields. The honest claim — *"the mind remembers
  what you tell it in words"* — is true, falsifiable, and small.

> **VERDICT: (a). RECOMMENDED, not a COMMANDER DECISION** — the capacity math forecloses (c) at
> 7 K params, and (b)'s multi-token readout is a strictly larger structural change for a phrase
> the user does not yet need. (b) and (c) are named successors. **The commander may still elect (b)
> first**; if so, the multi-token autoregressive readout + its gradcheck become MANDATORY and
> replace IX.6's single-token cert.

### IX.3 The tokenizer / vocabulary — exact mechanism (the (a) design)

**Mechanism: a fixed, embedded, content-addressed word list — NO hashing, NO BPE, NO OOV
collisions.** Two tables, because keys (questions) and values (answers) are different roles in the
substrate (mirroring `R_KEYV` vs `R_VALV` today):

- `r3_vocab_key[]`: the **key vocabulary** — the words a person can ask ABOUT. Size `R_KEYV` (the
  capacity cert sets this; start candidate 64–256, IX.4 measures).
- `r3_vocab_val[]`: the **answer vocabulary** — the one-word answers. Size `R_VALV` (candidate
  16–64). The classifier still reads `R_VALV` classes; each class IS an answer word.

Each list is **one embedded UTF-8 byte image** (newline-separated words), exactly like the
manifesto images (`ark_manifesto_at`, hashed by `pfs_id_compute`). Token id = the word's **0-based
line index**. The image has a **content-id** (`pfs_id_compute`), served to the UI via a new
`GET /vocab` route so the page and the kernel **provably share the same list** (a mismatch is a
detectable id disagreement, not a silent fake binding). Lookup is a linear scan (lists are small,
bounded, cold-path — `mind teach`/`ask` are human-paced).

- **OOV handling (IX.0 #6):** a typed word not on the list → **PRINTED refusal**, no token, no
  enqueue. `[mind] "<word>" not in key vocabulary (N=<R_KEYV> words); refused — vocab is fixed v1`.
  The cert gates that a known OOV word is refused (a positive falsification of "it would never just
  guess").
- **i18n honesty (the 32-language audience will type non-English).** Two options, weighed:
  - **English-only word list (v1 RECOMMENDED).** Smallest, cleanest, measurable; the cert's
    capacity number is unambiguous. The honest bound printed in the UI: *"this mind's words are
    English v1; your language is a future slice"* — said in the user's language via the existing
    i18n string table (`galaxy.html` `STR`). A non-English typer gets a clear refusal, not a
    mojibake binding.
  - **Byte-token language-neutrality** is the (b) path (multi-token), deferred.
  > **COMMANDER DECISION 1 — vocab language: English-only word list v1 (RECOMMENDED — one
  > measurable capacity number, honest refusal in the typer's language) vs a multilingual word
  > list (N× the table, blurs the capacity headline, and most non-English single "answer words"
  > want morphology the single-token readout cannot carry).** Recommend English-only v1; the
  > manifesto audience is told the bound truthfully in their language. The *manifesto* stays 32
  > languages (ark-profile unchanged); only the toy *vocabulary* is English v1.

### IX.4 The substrate widening — which knobs, the new `R_NP`, the gradcheck, the CI budget

**Knobs that change (and ONLY these — the anti-fork surface IX.9 pins it):** `R_KEYV` (key vocab),
`R_VALV` (answer vocab / output classes). **`R_DM` changes ONLY if the capacity cert proves 32-wide
attention cannot disambiguate the widened vocab** (IX.0 #2) — and that change is a measured
decision printed by the cert, never a guess. `R_NPAIR`/`R_SEQ` are **unchanged** (the per-prompt
binding budget is orthogonal to vocab size, IX.0 #3).

- New `R_NP` at the v1 candidate `(R_KEYV=256, R_VALV=64, R_DM=32)`: **≈19 008 floats** (verify by
  build) — 2.65× today, still a single block of statics, still well inside `DTR_LN_MAXW` widths
  (R_DM unchanged at 32).
- **Gradcheck stays MANDATORY and UNCHANGED in form.** `r3_grad_check` (`:447`) strides by 7 over
  `R_NP` with the ReLU-kink exclusion + absolute-floor disciplines; widening only grows the param
  count it sweeps (stride 7 still covers every weight family since gcd(7, R_DM=32)=1). `R_DM`
  unchanged ⇒ the `DTR_LN_MAXW` LayerNorm bound is untouched. `[r3-incontext-gradcheck]` must stay
  PASS at the new `R_NP` — the FIRST gate the cert prints.
- **CI time budget (the pretrain runs in CI — keep it bounded, print the time).** Cost model:
  pretrain is `R_EPOCHS·R_TRAIN_N` fwd+bwd passes; the dominant cost is attention + linear layers,
  which scale with `R_SEQ²·R_DM` and `R_SEQ·R_DM²` — **NOT with vocab size** (embeddings are O(R_SEQ·R_DM)
  lookups, classifier is O(R_VALV·R_DM) once per fwd). Estimated relative cost at `(256, 64, R_DM=32,
  R_SEQ=9)`: **≈1.03×** today (vocab adds ~3% via the larger classifier). At `R_DM=48` it is **≈2.0×**;
  at a longer `R_SEQ=13` (more bindings/prompt) **≈1.5×**. **So vocab widening at R_DM=32 is
  essentially free in CI time.** The cert PRINTS the measured pretrain seconds (the `m_boot`
  `tk_get_otm` pattern, `:1358`); the gate is *printed, not asserted* (host-speed-dependent), but
  the implementer wave must report it stays in the same order as LM-6's ~15 s native.

### IX.5 Compatibility — the LM-4..7 certs, re-baselined LOUDLY

`R_VALV` changing from 4 → 64 changes `chance = 100/R_VALV` from **25% → ~1.6%**, and every gate in
`r3_test`/`r3_handoff_test`/`r3_stream_test` that compares to `chance`/`33`/`25`. Two strategies:

- **(b) RE-BASELINE — RECOMMENDED.** The synthetic certs keep their STRUCTURE (the disease/cure/
  grounded shape is the proof, not the literal 25%) and re-print honest new numbers against the new
  `chance`. Every changed bar is called out in the epitaph and read line-by-line by the auditor.
  This is honest *because* it is loud: the cert's job is to certify the substrate as shipped, not a
  frozen historical view. **The recommended gate discipline: a re-baselined bar may only become
  STRICTER relative to the new chance, never looser — a gate that drops below its old margin-over-
  chance is a FAIL the auditor must catch (the validator-trap lesson).**
- **(a) frozen `*_LEGACY` compile-time path** (the certs run against a frozen 8×4 view via a
  separate small parameter block) keeps the old numbers byte-identical but **certifies a model that
  no longer ships** — a silent divergence between the certified and the live substrate. Rejected as
  the default for exactly that reason; named only as a fallback if re-baselining proves to destabilize.

> **COMMANDER DECISION 2 — cert compatibility: re-baseline the LM-4..7 certs against the new
> `R_VALV` with a LOUD epitaph and the stricter-only gate rule (RECOMMENDED) vs a frozen
> `*_LEGACY` 8×4 view (byte-identical history, certifies a non-shipping model).** Recommend
> re-baseline; the auditor reads every changed gate against the new chance.

### IX.6 The falsifiable acceptance test (the certificate) — the capacity curve IS the headline

Verb: `mind lang test` (or a `lang` cert verb). All numbers PRINTED, then canonical `[tag]
PASS/FAIL` lines, greppable like the rest of the suite. Pretrain via the SHARED `s_pretrain` recipe
(IX.0, the live mouth's path), re-baselined chance printed.

1. **`[lang-gradcheck]`** — the widened substrate's gradients are real. `r3_grad_check` at the new
   `R_NP`, max rel err `< 0.05` (the unchanged discipline). The FIRST gate — a widening that broke
   the gradient is rejected before any capacity claim.
2. **`[lang-capacity]` — THE HEADLINE.** Teach **N real word-bindings** (drawn from the embedded
   vocab) through the LIVE `r3_fact_learn` mouth across DMN sleeps, for N sweeping a printed ladder
   (e.g. N = 2, 4, 8, 12, 16, 24, 32, bounded by `R3_FQ_MAX` rehearsal + the cert's own larger
   sweep buffer); MEASURE masked held-out recall vs N; PRINT the curve. The **disease** = the old
   8-key ceiling (the substrate could only ever hold the synthetic 8). PASS requires the
   **measured-comfortable N** to be **≥ a bar the cert sets from the curve** (candidate: **≥16
   word-bindings at ≥75% recall** — the implementer reports the curve and the auditor sets the bar
   from it; **the number is discovered, not assumed** — IX.0 forbids guessing it). The printed curve
   is the deliverable even if the bar is renegotiated.
3. **`[lang-recall]`** — a SPECIFIC real binding round-trips in words. Teach `sky→blue`
   (real tokens); after sleep `mind ask sky` returns the token whose word is "blue", masked share
   `≥75%`, N=40 (the LM-6 `[teach-consolidated]` discipline, now over real words). The word, not
   the int, is printed.
4. **`[lang-oov]`** — OOV is refused, not guessed. A word provably absent from the list is REFUSED
   with the printed message; no queue entry is created (gated by `r3_facts_pending()` unchanged).
   The honest negative half: the mind does NOT invent a binding for a word it has no token for.
5. **`[lang-wire]`** (live, 2-process — rides the LM-7 path) — a real word taught on A is answered
   in words on B over the **versioned** wire. Teach `sky→blue` on A; `mind ask sky` on B returns
   "blue"; the `MT_TEACH_PKT` carried token ids with the version field set; an old-version node
   (or a malformed version) **drops the packet and prints it** (IX.0 #4, IX.7 mixed-version
   honesty). PASS requires B's answer word == A's taught word AND the version-mismatch drop is
   exercised + printed at least once.

### IX.7 The honest bound (what is NOT claimed)

- **NOT generation, NOT grammar, NOT chat.** One-token associative recall over a fixed vocabulary.
  "sky → blue" is a remembered binding, not a sentence the mind composed. No syntax, no novelty.
- **One-word answers only.** The readout is `R_VALV` single classes (IX.0 #3). Multi-token / phrase
  answers are the (b) successor slice.
- **Vocabulary is fixed and bounded** (the embedded list); growing it is a knob (re-embed + widen),
  not a mechanism, and OOV is an honest refusal (IX.6 #4), never a guess.
- **English-only v1** (COMMANDER DECISION 1); the manifesto stays 32-language, the toy vocab does
  not. A non-English typer is told the bound truthfully, in their language.
- **Belief revision still future.** Re-teaching a word a new answer is refused (the LM-6/7 rule
  unchanged). "sky → blue" then "sky → grey" is the belief-revision slice, not this one.
- **The capacity number is the substrate's, not language's.** "≥16 word-bindings" means the 7 K
  substrate, widened, holds ~16 real bindings at recall — it is NOT a claim about vocabulary
  *coverage* of a language. A 256-word list is still an infant's words.
- **Mixed-version region honesty.** A node on the old wire drops new-version packets (printed),
  and vice-versa; the region's shared mind is then *partitioned by version* — stated, gated
  (`[lang-wire]`), not hidden. The version field makes the partition observable.

### IX.8 The wire — `MT_TEACH_PKT` version bump (token ids)

Today `MT_TEACH_PKT.key`/`val` are `U1` carrying `k∈0..7`/`v∈0..3` (`dtr.h:329`). The language slice
carries **token ids**. The change (IX.0 #4):

- Add a `U1 wire_ver` field (or repurpose a reserved byte): `MT_WIRE_VER_LANG`. Old nodes set/expect
  the legacy version; a receiver whose `wire_ver` does not match its own **drops the packet and
  prints it** (the mixed-version honesty of IX.7). This is the ONE place the region partitions by
  version, and it is observable.
- Token-id width: if `R_KEYV ≤ 256` and `R_VALV ≤ 256`, `key`/`val` stay `U1` (no width change, only
  *meaning* changes — id-into-vocab, not a small synthetic int). If a future vocab exceeds 256,
  widen to `U2` (+2 B → 46 B, `_Static_assert(sizeof(MT_TEACH_PKT) <= KDDS_DATA_MAX)` still holds
  with 146 B to spare). **v1 recommends `R_KEYV ≤ 256`, `R_VALV ≤ 256` so the wire width is
  UNCHANGED — only the version field is new.**
- `prov_head`/`fact_seq`/`origin_node`/`src` UNCHANGED — provenance across the mesh (LM-7 VIII.4) is
  untouched; the teacher is still named.

### IX.9 Anti-fork constraint (HARD) — exact reuse surface (+ FLAGGED names)

The numerically-meaningful kernels stay the SAME ones `dtr.c`/`r3_incontext.c` already use (the
`:13-18` anti-fork rule). This slice's *only* legitimate task-specific surface is **(i) the size of
the embedding/classifier tables** (already blessed as "token lookup vs scalar projection … which is
legitimately task-specific") and **(ii) a new cold-path word↔id tokenizer**. EXPLICITLY:

- **REUSE, do not fork:** `r_forward`/`r_backward`/`r_grad_check`/`r_train_epoch`/`s_pretrain`/
  `r3_fact_learn`/`r3_consolidate_idle_round`/`m_masked_vote`/`mind_cmd`/`mind_net_task`/
  `m_publish_teach` — the math + the mouth + the wire path are UNCHANGED in structure. Only
  `R_KEYV`/`R_VALV`/the dependent offsets change.
- **NEW, file-static or one TU:** `r3_vocab.c`/`r3_vocab.h` (the embedded lists + `r3_vocab_id`/
  `r3_vocab_word` lookup, content-id), the `GET /vocab` route in `galaxy.c`, the `[lang-*]` cert.
- **FLAGGED names that do NOT exist yet (the implementer creates them; the auditor greps that NO
  other symbol was forked):** `r3_vocab_id`, `r3_vocab_word`, `r3_vocab_count`, `r3_vocab_id_blob`,
  `MT_WIRE_VER_LANG`, `R_VALV_LEGACY` (only if COMMANDER DECISION 2 picks the frozen path).
  **`dtr_train_batch`/`gl_merge`/`LM_ENGRAM` must NOT appear** (the wrong network — the V.0 rule;
  they may appear only inside a do-NOT-use comment).

### IX.10 The UI — galaxy teach/ask become text inputs

`galaxy.html:383` (`fill("tk",8);fill("tv",4)`) and the `/teach` POST `body:`k=${k}&v=${v}``
(`:396`) change minimally:

- The `tk`/`tv`/`ak` `<select>` integer dropdowns become **text `<input>`** fields (or a datalist
  populated from `GET /vocab` so the user sees the available words — RECOMMENDED, since the vocab is
  fixed and small, a datalist is honest about what words exist).
- `POST /teach` sends `k=sky&v=blue` (words); `galaxy.c`'s `/teach` bridge (`:815`) maps words→ids
  via `r3_vocab_id` BEFORE building the `mind teach` command, and returns a **403/refusal** for OOV
  (the existing 403-until-ack gate pattern extends to OOV). `mind ask` returns the answer WORD
  (`m_last_v` resolved through `r3_vocab_word`), shown in the page.
- **The i18n chrome** (`STR` table) gains the honest-bound string ("English words v1; your language
  is a future slice") in every existing language, and the OOV-refusal message. The note line
  (`galaxy.html:67`, *"this mind speaks 8 symbols today — it is an infant, truthfully shown"*)
  becomes *"this mind speaks N English words today — still an infant, truthfully shown."*

### IX.11 Tags + bars (summary), CI plan, provenance

**Tags (5):** `[lang-gradcheck]` (the widened gradients are real — the precondition),
`[lang-capacity]` (**the headline curve** — N word-bindings at ≥75%, disease = the 8-key ceiling),
`[lang-recall]` (a specific real binding round-trips in words — the cure), `[lang-oov]` (OOV
refused, not guessed — the honest negative), `[lang-wire]` (real words cross the region on the
versioned wire — the shared-discipline tag, live 2-process). Disease→cure→capacity→shared
discipline all covered.

**CI plan (specify only — do NOT edit `ci.yml` in this Part).** The implementer wave adds: a native
job running `mind lang test` and grepping `[lang-gradcheck] PASS`, `[lang-capacity] PASS`,
`[lang-recall] PASS`, `[lang-oov] PASS`; the live 2-process job (rides the LM-7 `[shared-*]`
harness) grepping `[lang-wire] PASS`. The re-baselined LM-4..7 greps (`[r3-incontext-*]`,
`[handoff-*]`, `[stream-*]`, `[teach-*]`, `[shared-*]`) must ALL stay green at the new dims — the
no-regress gate. **The implementer wave edits `ci.yml` + the sample, not this document.**

### IX.12 Provenance / closes-on

Design only. This slice closes when `[lang-gradcheck]`, `[lang-capacity]`, `[lang-recall]`,
`[lang-oov]`, `[lang-wire]` are green on a clean rebuild AND CI-enforced, the LM-4..7 certs are
re-baselined-and-green at the new dims, audited by a **separate** agent on the **commander's**
binary — not the implementer's. The audit makes the acceptance test; the commander reads the gate
formula — the widened `R_NP` (built, not trusted), the `[lang-capacity]` curve + the bar set FROM
it, the tokenizer's OOV-refusal path (no silent binding), the wire version-drop, and the re-baselined
gates (each stricter-only vs the new chance) — line-by-line. One epitaph line in `gap-ledger.md`;
the LM epitaph's longest-standing disclaimer ("synthetic vocab, NOT real language") is finally
**downgraded, honestly: real WORDS in/out, one-token, bounded vocab — NOT generation/grammar/chat.**

**COMMANDER DECISIONS NEEDED (recommended defaults):**
1. **Vocab language** (IX.3): **English-only word list v1 (RECOMMENDED** — one measurable capacity
   number; honest refusal in the typer's language; manifesto stays 32-language) vs a multilingual
   word list (N× the table, blurs the headline, morphology breaks single-token answers).
2. **Cert compatibility** (IX.5): **re-baseline LM-4..7 against the new `R_VALV` with a LOUD epitaph
   + stricter-only gate rule (RECOMMENDED)** vs a frozen `R_VALV_LEGACY` 8×4 view (byte-identical
   history but certifies a non-shipping model).
3. **The (a)/(b)/(c) fork** (IX.2): **(a) word-level single-token recall for v1 (RECOMMENDED;
   capacity math forecloses (c) at 7 K params — the future arc is ~25–50 M params, tensor-parallel
   over the region via the existing DKVA substrate; (b) multi-token phrases is the next slice).**
   Surfaced as a decision because (b)/(c) are the deeper "real language" north star; if the
   commander elects (b)-first, the autoregressive multi-token readout + its gradcheck become
   MANDATORY and replace IX.6.
4. **`[lang-capacity]` bar** (IX.6 #2): the comfortable-N bar is **set FROM the measured curve by
   the auditor, not assumed** (candidate ≥16 bindings at ≥75%). The commander reads the curve before
   ratifying the bar — the headline number must be discovered, never guessed.
5. **The `R_DM` widening** (IX.4): hold `R_DM=32` UNLESS `[lang-capacity]` proves 32-wide attention
   cannot disambiguate the widened vocab; any `R_DM` bump is a MEASURED decision printed by the
   cert (it ~doubles `R_NP` and CI time per step), never a guess.

## Part X — the capacity surgery: widen the mind's thinking width (R_DM 32→48/64)

LM-8 (Part IX) shipped REAL WORDS and then did the honest thing the audit demands: it **measured the
capacity wall instead of asserting it past**. The printed `[lang-capacity]` curve at `(R_KEYV=8,
R_VALV=32, R_DM=32)` was

```
N= 2 -> 100%    N= 4 -> 100%    N= 6 -> 63%    N= 8 -> 72%
comfortable-N (>=75% recall) = 4
```

So the mind holds **4 word-bindings comfortably** today — and recall has already collapsed below the
75% bar by N=6, *before* the R_KEYV=8 vocabulary ceiling is even reached. The dreamed 256/64 vocab
"collapsed to chance" (IX.0 #2). **The bottleneck is not the vocabulary tables; it is the 32-wide
attention** (`R_DM`) trying to disambiguate too many simultaneous key→value bindings in one prompt
(IX.0 #1: *"the associative-reasoning bottleneck is the attention width R_DM, which scales the model
quadratically"*). This Part performs the **measured `R_DM` surgery** Part IX named as the follow-up
(IX.4, COMMANDER DECISION 5) and the gap-ledger epitaph recorded — widen the thinking width so the
capacity curve rises, and PRINT the new comfortable-N superimposed on LM-8's.

### X.0 IMPORTANT — what the tree actually says (read BEFORE coding)

Eight load-bearing facts. The first three are the **safety crux** (the shared LayerNorm); the rest
size the surgery. **Verify every number by build; do not trust this doc blind.**

1. **`DTR_LN_MAXW` is a pure STACK-BUFFER CAPACITY CAP, not a behavioral constant — this is the
   whole safety question and the answer is GREEN.** Its ONLY runtime use anywhere in the tree
   (`grep -rn DTR_LN_MAXW arch/ boot/ android/`) is one line: `float dxh[DTR_LN_MAXW]` in
   `dtr_ln_bwd` (`dtr.c:731`). It is the size of a scratch array, indexed `dxh[0..n-1]` where `n` is
   the **runtime width argument**. The dtr SENSOR brain calls `dtr_ln_bwd(..., n=DM=8)` and touches
   only `dxh[0..7]`; R3 calls it with `n=R_DM`. **Raising the cap 32→64 makes the array longer but
   the sensor still writes/reads exactly its first 8 entries — its arithmetic is byte-for-byte
   identical.** A capacity cap, not a behavioral one (the IX-prose hedge "crashes as-is" is true and
   benign: at `R_DM=48` the R3 path would write `dxh[0..47]` into a `dxh[32]` array — a stack smash
   — which is *precisely* a buffer-size bug a size bump fixes, NOT a numerics change). **This is the
   crux the design exists to settle: the bump is FREE for the sensor brain. The cert proves it.**
2. **The dtr sensor brain's dims are `DM=8, FFN=16, SEQ=4, NH=2, DH=4` (`dtr.h:33-40`), 635 weights
   (`DTR_WEIGHT_FLOATS`).** None of these change. The sensor certs are `[onebrain-*]` (and `moe`/
   `dkva`), NOT `[r3-incontext-*]`/`[lang-*]`. The surgery touches `DTR_LN_MAXW` (a `dtr.h` constant)
   and the R3-private `R_DM`/`R_FFN`/`R_KEYV`/`R_VALV` — **never `DTR_EMBED_DIM` or any dtr dim.**
   The unaffected-sensor proof (X.3) is a cert requirement, not a hope.
3. **`dtr_ln_fwd_cache` has NO width-capped buffer** (`dtr.c:709`) — it writes `xh[]`/`y[]` provided
   by the caller, loops to `n`, allocates nothing. Only `dtr_ln_bwd`'s `dxh[DTR_LN_MAXW]` is capped.
   So the LayerNorm forward is already width-safe at any R_DM; the bump is a one-constant change.
4. **The capacity wall is the ATTENTION width, measured. LM-8's curve already collapses at N=6
   (63%) — before R_KEYV=8.** Widening `R_DM` alone is necessary but NOT sufficient to RAISE
   comfortable-N past 8: the `[lang-capacity]` ladder is bounded by `R_KEYV` (`LANG_NMAX = 8 =
   R_KEYV`, `:1462`; `lang_make_dict` draws N distinct keys from `R_KEYV`, `:1493`). To MEASURE a
   comfortable-N of 12 or 16 the cert must sweep N>8, which needs **both** (a) `R_DM` wide enough to
   disambiguate them AND (b) `R_KEYV` ≥ that N AND (c) the embedded `vk_img` word list grown to ≥
   that many key words (`r3_vocab.c:31` holds exactly 8 keys today; `R3_VOCAB_KEYS=8`,
   `r3_vocab.h:32`). **The headline gain needs a coordinated R_DM + R_KEYV + word-list widening,
   not R_DM alone.** The answer vocab `R_VALV=32` already has 32 real words (`vv_img`, 4 lines × 8),
   and can grow to 64 only if `vv_img` gains 32 more answer words.
5. **Every R3 activation buffer is a macro-sized STATIC, not a stack local — they grow automatically
   and safely with `R_DM`.** The forward cache `R_TC rc` (`:140-156`) and the `r_backward` scratch
   arrays (`dy2`/`dr2`/`dy1`/`dr1`/`dtok`/`dconcat`/`dQ`/`dK`/`dV`, declared `static`, `:259-261`)
   are all dimensioned `[R_SEQ][R_DM]`/`[R_SEQ][R_DH]` etc. Bumping `R_DM` resizes them at compile
   time into `.bss`, NOT onto the task stack — so the stack-overflow lesson
   (`feedback_hosted_relay_stack_overflow.md`, `feedback_validator_and_learner_traps.md`) does NOT
   bite here, *as long as the implementer keeps them `static`*. Measured `.bss` for `rc`+backward
   scratch: ~26.8 KB today (DM=32) → ~39.5 KB (DM=48, FFN=48, VALV=64) → ~52.1 KB (DM=64). The ONE
   thing to watch: `lang_make_dict`'s `static UB kpool[R_KEYV]` and the `[lang-recall]` `kp[R_KEYV]`
   (`:1672`) — these grow with R_KEYV, also static, fine. **No new stack locals; the auditor greps
   that the bumped arrays stayed `static`.**
6. **Gradcheck stride-7 stays valid at every candidate R_DM.** `r_grad_check` (`:481`) strides by 7
   over `R_NP` with ReLU-kink exclusion + absolute-floor (`:493` comment: *"gcd(7,R_DM)=1 so row/col
   positions rotate"*). `gcd(7,48)=1`, `gcd(7,64)=1`, and 7 is coprime to every family width that
   appears (`R_DH∈{12,16}`, `R_FFN∈{32,48,64}`, `R_VALV∈{32,48,64}`, `R_KEYV∈{12,16}`). **No stride
   change is needed** — the comment's coprimality argument holds at the new R_DM unchanged. The
   `:493` comment must be updated to name the new R_DM, but the stride is 7.
7. **`R_DH = R_DM/R_NH` must stay an integer.** `R_NH=4` today. `48/4=12 ✓`, `64/4=16 ✓`. If the
   commander elects to scale `R_NH` (X.2's third fork), only `R_NH∈{R_DM divisors}` are legal:
   `R_DM=48 → R_NH∈{1,2,3,4,6,8,12,16,24}`; `R_DM=64 → R_NH∈{1,2,4,8,16}`. v1 holds `R_NH=4` (the
   `:96-99` comment: *"8-way recall needs R_DM=32 capacity; R_DM=16/R_NH=2 stays at chance —
   measured, not assumed"* — head count was already tuned; widening per-head dim, not head count,
   is the conservative first move).
8. **The wire and `/vocab` follow the vocab dims, not R_DM.** `MT_TEACH_PKT.key`/`val` stay `U1`
   while `R_KEYV ≤ 256` and `R_VALV ≤ 256` (`dtr.h:345`; the `_Static_assert` at `:134`). Growing
   `R_KEYV` 8→16 and `R_VALV` 32→64 leaves the wire width UNCHANGED — only the embedded vocab images
   change content-id, which `GET /vocab` already serves to the UI (IX.10). A vocab-dim change is a
   `wire_ver` non-event (the version field already exists, `MT_WIRE_VER_LANG`); but it IS a
   `/vocab` content-id change, so the UI's datalist re-fetches — the design must state that honestly
   (X.4). **R_DM is invisible to the wire and the UI** (it changes only `rw[]`, never a packet).

### X.1 The claim to prove

> The mind's **thinking width** is widened (`R_DM` 32→the cut, the attention that disambiguates
> simultaneous bindings) so the `[lang-capacity]` curve **RISES**: where LM-8 collapsed below 75%
> recall by N=6 (comfortable-N = 4), the widened substrate holds **MORE real word-bindings at ≥75%
> recall** — the new comfortable-N is MEASURED, PRINTED, and superimposed on LM-8's curve, and the
> headline is the **capacity GAIN** (Δ comfortable-N) over LM-8's 4. The widening reuses the SAME
> shared LayerNorm kernels (the `DTR_LN_MAXW` cap raised — proven free for the dtr sensor brain,
> whose `[onebrain-*]`/moe/dkva numbers stay BYTE-IDENTICAL), the SAME `r_forward`/`r_backward`
> math (only the dims change), and the SAME gradcheck discipline (stride-7, now over the larger
> `R_NP`). The gain BUYS more words held at once — it does NOT buy grammar, generation, or
> multi-token answers (the IX.7 bounds stand, restated in X.5).

This is **the same associative-recall task, wider** — the legitimately-task-specific surface the
anti-fork rule already blesses (`r3_incontext.c:16-18`: embedding/readout dims may differ; the
kernels may not). It is NOT a new mechanism; it is the substrate finally given the width its own
LM-8 cert proved it lacked.

### X.2 The cut — `R_DM = 48` vs `64`, the knobs, the new `R_NP`, the expected vocab gain

**Knobs that change (and ONLY these — the anti-fork surface X.6 pins it):**

- `R_DM` (the surgery): **48 or 64** (the fork below). `R_DH = R_DM/R_NH` follows (12 or 16).
- `R_KEYV` (key vocab): **8 → 16** — REQUIRED to let the ladder sweep N>8 (X.0 #4). Needs `vk_img`
  grown to 16 key words + `R3_VOCAB_KEYS=16`.
- `R_VALV` (answer vocab / classes): **32 → 64** — to give the wider attention a wider answer space
  to separate (and to finally land the dreamed 64-answer vocab IX hoped for). Needs `vv_img` grown
  to 64 answer words + `R3_VOCAB_VALS=64`.
- `R_FFN`: **the third fork.** Today `R_FFN=32` is hard-coded (`:100`), independent of R_DM. Scaling
  it to R_DM (`R_FFN=R_DM`) gives the wider model proportional MLP capacity; holding it at 32 saves
  params. v1 RECOMMENDS `R_FFN = R_DM` (a wider attention deserves a wider MLP; the cost is small —
  see R_NP below) but flags it as a COMMANDER DECISION.
- `R_NPAIR`/`R_SEQ`: **UNCHANGED** (`R_NPAIR=8`, `R_SEQ=9`) — the per-prompt binding budget is
  orthogonal to thinking width (IX.0 #3). Widening R_DM lets the model disambiguate the 8 bindings
  it already carries; it does not add more per prompt.
- `DTR_LN_MAXW`: **32 → 64** (one constant in `dtr.h`, X.3) — sized to the largest R_DM the cut may
  ever reach, so a future R_DM=64 needs no second bump.

**The new `R_NP` (built, not trusted — the `_Static_assert` updates to the measured value):**

| dims `(R_KEYV, R_VALV, R_DM, R_FFN)` | `R_NP` | × LM-8 (8992) | CI pretrain cost vs LM-8 |
|---|---|---|---|
| LM-8 v1 `(8, 32, 32, 32)` | **8 992** | 1.00× | 1.0× (~2 s host) |
| **`(16, 64, 48, 48)` — v1 RECOMMENDED** | **21 568** | 2.40× | **≈2.0×** (~4 s) |
| `(16, 64, 48, 32)` (R_FFN held) | 20 016 | 2.23× | ≈2.0× |
| `(16, 64, 64, 64)` | 34 880 | 3.88× | **≈4.0×** (~8 s) |
| `(16, 64, 64, 32)` (R_FFN held) | 30 752 | 3.42× | ≈4.0× |

CI-cost model (IX.4, verified): attention + linears scale `R_SEQ²·R_DM + R_SEQ·R_DM²`, dominated by
the `R_DM²` term ⇒ **cost ∝ R_DM²**. `(48/32)² ≈ 2.25×`, `(64/32)² = 4×`. R_SEQ unchanged (9), vocab
adds only ~3% via the larger classifier. **Pretrain at R_DM=48 ≈ 4 s host (well inside LM-6's ~15 s
budget); at R_DM=64 ≈ 8 s** (still bounded, but 2× the 48 cost for a curve gain the cert must prove
is worth it). The cert PRINTS measured seconds (the `tk_get_otm` pattern, gate *printed not
asserted*).

**Expected vocab gain (the design EXPECTS; the cert DISCOVERS).** LM-8 collapsed at N=6 with R_DM=32.
Doubling the per-head dimension (R_DH 8→16 at R_DM=64, or 8→12 at R_DM=48) gives attention markedly
more room to keep 8+ key directions near-orthogonal. **Expectation: R_DM=48 lifts comfortable-N from
4 to ~8 (the full R_NPAIR=8 prompt cleanly recalled); R_DM=64 may reach 12–16** (the ladder swept to
16 once R_KEYV=16). **But IX.0's discipline binds: the bar is DISCOVERED from the printed curve by
the auditor, never asserted.** The headline is whatever Δ the curve shows — if R_DM=48 only reaches
6, that is the honest gain, printed.

> **THE CUT — RECOMMENDED `R_DM = 48`.** It is the measured sweet spot: 2.4× params / 2.0× CI for an
> expected comfortable-N of ~8 (doubling LM-8's 4) — a real, falsifiable gain at half the R_DM=64
> cost. `R_DM=64` is the reach option if the implementer's curve shows 48 still collapses before N=8
> (then 64 is justified BY THE MEASUREMENT, not the guess). **The design proposes 48; the
> implementer measures both behind a build flag (`R_DM` is one `#define`) and the auditor ratifies
> the smaller R_DM that clears the discovered bar — the stricter-is-cheaper rule.**

### X.3 The shared-LayerNorm surgery — the safety proof (the crux)

**Exactly what changes in `dtr.c`/`dtr.h`:** ONE line.

```
- #define DTR_LN_MAXW 32   /* max LayerNorm width across all dtr configs */
+ #define DTR_LN_MAXW 64   /* max LayerNorm width: R3 R_DM up to 64 (LM-9) */
```

`dtr_ln_bwd`'s `float dxh[DTR_LN_MAXW]` (`dtr.c:731`) becomes `float dxh[64]`. **Nothing else in
`dtr.c` changes.**

**Why the dtr SENSOR brain is provably unaffected (byte-identical `[onebrain-*]`/moe/dkva — a cert
requirement, not a hope):**

1. **It is a pure capacity cap (X.0 #1).** `dxh` is scratch, written `dxh[i]` and read `dxh[i]` for
   `i < n` only. The sensor passes `n = DM = 8` and touches `dxh[0..7]`. Enlarging the array to
   `[64]` leaves `dxh[0..7]` at the same offsets with the same values — the loop bounds, the
   arithmetic (`m1`, `m2`, `dx[i]`), and every float written are **identical bit-for-bit**. A larger
   uninitialized tail the sensor never reads cannot change a sensor output.
2. **No struct layout moves.** `DTR_LN_MAXW` sizes a *function-local* array, not any persisted
   struct, weight buffer, or wire field. `DT_TCACHE`, `DTR_WEIGHT_FLOATS=635`, the `.tdtr` checkpoint
   header — all unchanged. So no checkpoint, no wire, no replay re-baselines.
3. **The cert PROVES it (the X.7 `[lang-sensor-intact]` tag).** The acceptance suite runs the
   EXISTING `[onebrain-*]` (and moe/dkva) certs at the bumped `DTR_LN_MAXW` and greps they print
   their SAME PASS lines with their SAME numbers. The audit re-runs them on the commander's binary.
   **If any `[onebrain-*]` number moves by a single digit, the bump is rejected** — but X.0 #1's
   analysis says it cannot, and the cert exists to make that falsifiable, not assumed.

**The honest weighing (anti-fork vs safety) — and why the free path wins.** The alternative is an
**R3-local LayerNorm width** (an R3-private `dxh[R_DM]` that does NOT touch the shared cap). That
would isolate the sensor *by construction* — but it would mean R3 calling a kernel the sensor does
NOT, which is **exactly the fork the `:13-18` anti-fork rule forbids** (the kernels must be the SAME
functions). Since X.0 #1 proves the shared bump is already free for the sensor, **the shared-cap
bump is BOTH safer for anti-fork AND safe for the sensor** — there is no tension to trade. The
R3-local path is named here only to be rejected with its reason: it would manufacture a fork to
solve a problem the shared bump does not have. **RECOMMENDED: raise the shared `DTR_LN_MAXW` to 64;
the `[lang-sensor-intact]` cert makes the freeness falsifiable.**

> **COMMANDER DECISION C — LayerNorm surgery: raise the shared `DTR_LN_MAXW` 32→64 (RECOMMENDED —
> free for the sensor by X.0 #1, preserves anti-fork, cert-proven by `[lang-sensor-intact]`) vs an
> R3-local LayerNorm width (isolates the sensor but FORKS the shared kernel — rejected by the
> `:13-18` rule for a problem the shared bump does not have).**

### X.4 Re-baseline (again) — `R_VALV` 32→64 moves the bars; the wire/UI follow

LM-8 already re-baselined LM-4..7 to `R_VALV=32` (chance 25%→3.125%, IX.5). **Part X moves chance
AGAIN: `R_VALV` 32→64 ⇒ `chance = 100/R_VALV` 3.125% → 1.5625%.** The stricter-only rule (IX.5)
binds: every re-baselined gate may only get STRICTER relative to the new chance, never looser.

- **`H_CHANCE`/`chance`/`handif_gate`** auto-track `R_VALV` already (they are computed `100/R_VALV`
  and `(100/R_NPAIR)(1-1/R_VALV)`, not frozen constants — IX.5's deliberate design, `:633,648,693`).
  So the structural certs (`[r3-incontext-*]`, `[handoff-*]`, `[stream-*]`, `[teach-*]`) re-baseline
  by recompile; the auditor reads each printed gate against chance=1.56% and confirms each margin
  did not shrink. `H_VSPREAD = R_VALV/4` (`:700`) becomes 16 (spread {0,1,2,3}→{0,16,32,48}) — still
  mutually separated across the wider space, the SAME derivation discipline.
- **`R_CERTKEYS`/`R_NPAIR` unchanged** (8) — the fixed-fact cert working-key arrangement stays
  byte-identical (IX's R_CERTKEYS==R_NPAIR property), so the LM-4..7 cert STRUCTURE is untouched;
  only the chance/margins re-print.
- **The wire (`MT_TEACH_PKT`)**: `R_KEYV=16 ≤ 256` and `R_VALV=64 ≤ 256` ⇒ `key`/`val` stay `U1`,
  **wire width UNCHANGED** (X.0 #8). `MT_WIRE_VER_LANG` is unchanged (still token-ids semantics) —
  a same-version node decodes fine. **BUT** the vocab *content* grew, so a node with the OLD 8/32
  word list would resolve a token id 8..15 (a new key) or 32..63 (a new answer) to a DIFFERENT word
  or OOB. **Honesty:** the `/vocab` content-id (IX.3, `GET /vocab`) CHANGES (the images grew), and
  that is the detectable signal — a peer whose `/vocab` content-id differs is on a different
  vocabulary and the UI shows the mismatch. The design does NOT bump `wire_ver` for a vocab-size
  change (the wire FORMAT is identical); it relies on the content-id channel that IX.3 already built
  to make the vocab divergence observable. **The implementer must state this in the epitaph: a
  Part-X node and an LM-8 node share `wire_ver` but NOT `/vocab` content-id — the partition is by
  vocabulary, surfaced by content-id, not by a version drop.**
- **`/vocab` + the galaxy datalist** re-fetch the grown images (the datalist auto-populates from
  `GET /vocab`, IX.10) — the UI shows 16 key words / 64 answer words with no code change beyond the
  images. The note line ("this mind speaks N English words today") prints the new N.

> **COMMANDER DECISION D — vocab-divergence signal: rely on the existing `/vocab` content-id
> channel to make the Part-X-vs-LM-8 vocabulary partition observable (RECOMMENDED — the wire FORMAT
> is identical, only content grew; IX.3 already built content-id agreement) vs bump `wire_ver` to a
> new value (forces a hard drop between Part-X and LM-8 nodes — cleaner partition but discards
> interoperable same-format packets the receiver could still decode against ITS own vocab).**

### X.5 The honest bound (what is NOT claimed)

- **This BUYS more words held at once, NOT grammar/generation.** A higher comfortable-N means the
  mind disambiguates more simultaneous bindings — it is still **single-token associative recall over
  a fixed, bounded vocabulary** (IX.7 stands verbatim). No syntax, no novelty, no multi-token
  answers, no belief revision.
- **The generative LM still needs the DKVA substrate + millions of params** (IX.2's (c) arc:
  ~25–50 M params, tensor-parallel over the region). Widening R_DM 32→64 is a knob within the
  one-node `rw[]`, not a step toward (c) — it is the *recall* substrate getting wider, not the
  *generation* substrate appearing.
- **The capacity number is the substrate's, not the language's.** "comfortable-N = 8 (or 12/16)"
  means the widened ~21 K–35 K substrate holds that many real bindings at recall — NOT a claim about
  vocabulary coverage. 16 key words is still an infant's vocabulary.
- **The gain is MEASURED, and may be smaller than hoped.** If R_DM=48 only lifts comfortable-N to 6,
  that is the honest headline, printed; the dream (16) is not asserted. The curve is the deliverable
  even if the bar is renegotiated downward.
- **The sensor brain is untouched** — a cert requirement (`[lang-sensor-intact]`), not an assumption.

### X.6 Anti-fork constraint (HARD) — exact reuse surface (+ FLAGGED names)

The numerically-meaningful kernels stay the SAME ones `dtr.c`/`r3_incontext.c` already use (`:13-18`).
This Part's *only* legitimate change surface is **(i) the shared `DTR_LN_MAXW` capacity cap** (a
buffer size, not a kernel fork — X.3), **(ii) the R3-private dims `R_DM`/`R_FFN`/`R_KEYV`/`R_VALV`
and their dependent offsets** (the "legitimately task-specific" embedding/classifier sizes), and
**(iii) the grown `vk_img`/`vv_img` word images** (content, not mechanism).

- **REUSE, do not fork:** `r_forward`/`r_backward`/`r_grad_check`/`r_train_epoch`/`s_pretrain`/
  `dtr_ln_fwd_cache`/`dtr_ln_bwd`/`dt_linear`/`dt_softmax`/`r3_fact_learn`/`lang_*`/`mind_cmd`/
  `m_publish_teach` — UNCHANGED in structure. Only the dims and the one cap change.
- **CHANGED constants (the whole surgery — the auditor greps NOTHING ELSE forked):** `DTR_LN_MAXW`
  (`dtr.h`, 32→64), `R_DM` (`:97`, 32→the cut), `R_FFN` (`:100`, → R_DM or held), `R_KEYV` (`:84`,
  8→16), `R_VALV` (`:85`, 32→64), `R3_VOCAB_KEYS`/`R3_VOCAB_VALS` (`r3_vocab.h`, to match), the
  `lang_ladder`/`LANG_NMAX`/`LANG_LADDER_N` (`:1460-1462`, to sweep N>8), the `R_NP`
  `_Static_assert` (`:126`, to the new built value), the gradcheck `:493` comment (name the new
  R_DM; stride STAYS 7).
- **GROWN content:** `vk_img` (8→16 key words), `vv_img` (32→64 answer words) in `r3_vocab.c`.
- **MUST NOT appear (the wrong network — the V.0 rule):** `dtr_train_batch`/`gl_merge`/`LM_ENGRAM`,
  and **NO new R3-local LayerNorm function** (the X.3-rejected fork — its appearance is the anti-fork
  violation the auditor catches). **NO new stack locals in `r_forward`/`r_backward`** (X.0 #5 — the
  buffers stay `static`; a non-static `[R_DM]` array at R_DM=64 is the stack-smash regression).
- **FLAGGED names that do NOT exist yet (the implementer may create; the auditor greps no other
  symbol forked):** none required — the surgery is constants + grown images + the new cert tags
  below. (`R_DM_LEGACY` only if a frozen-dims fallback is elected, X.4's analog of IX's
  `R_VALV_LEGACY` — NOT recommended.)

### X.7 The falsifiable acceptance test (the certificate) — the capacity curve RISES

Verb: `mind lang test` (the EXISTING `r3_lang_test`, re-run at the widened dims — NOT a new verb).
All numbers PRINTED, then canonical `[tag] PASS/FAIL`, greppable. Six tags (the five LM-8 `[lang-*]`
tags re-measured at the new dims, PLUS the sensor-intact tag the shared-cap bump mandates):

1. **`[lang-gradcheck]`** — the WIDENED substrate's gradients are real. `r_grad_check` at the new
   `R_NP` (stride 7, ReLU-kink exclusion, absolute floor — unchanged), max rel err `< 0.05`. The
   FIRST gate: a widening that broke the gradient (e.g. a mis-sized offset) is rejected before any
   capacity claim. PRINTS `R_NP` and `R_DM`.
2. **`[lang-capacity]` — THE HEADLINE, v2.** Sweep N over a ladder extended past 8 (e.g.
   `{2, 4, 8, 12, 16}`, bounded by the new `R_KEYV=16`); consolidate N real word-bindings into
   `rw[]` each step (the SHARED LM-4 recipe); PRINT masked held-out recall vs N. **PRINT BOTH curves
   superimposed:** the LM-8 R_DM=32 curve (the recorded `N2→100 N4→100 N6→63 N8→72`, comfortable-N=4)
   and the new R_DM-widened curve, with the **Δ comfortable-N called out as the headline gain.** PASS
   requires the new comfortable-N **> LM-8's 4** (a strict gain — the bar is DISCOVERED from the new
   curve by the auditor; candidate ≥8, but the number is measured, IX.0 forbids guessing). Tag:
   **`[lang-capacity-v2]`** (distinct from LM-8's `[lang-capacity]` so the CI grep sees the new
   gate; the prose calls it "the curve at the new R_DM superimposed on LM-8's").
3. **`[lang-recall]`** — a SPECIFIC real binding still round-trips in words, now at the new
   comfortable-N. Teach `sky→blue`; after sleep `mind ask sky` returns "blue", masked share ≥75%,
   N=40 — but the surrounding dict is now sized to the MEASURED comfortable-N (was hard-coded to
   `LANG_CAP_BARN`), so the round-trip is scored in the wider regime the capacity curve proved.
4. **`[lang-oov]`** — OOV still refused, not guessed (unchanged mechanism; re-run because the vocab
   grew — a word absent from the *new* 16/64 list is still refused, no queue entry).
5. **`[lang-wire]`** (live, 2-process) — a real word taught on A answered in words on B over the
   versioned wire (unchanged format; re-run at the new vocab to confirm a token id 8..15 / 32..63
   round-trips). PLUS: a node with the OLD `/vocab` content-id is shown as a vocabulary partition
   (X.4 — content-id mismatch surfaced, not a silent wrong-word binding).
6. **`[lang-sensor-intact]` — THE SAFETY GATE (new this Part).** Run the EXISTING `[onebrain-*]`
   (and moe/dkva) sensor certs at the bumped `DTR_LN_MAXW=64` and assert they print their SAME PASS
   lines with their SAME numbers — the dtr sensor brain is BYTE-IDENTICAL after the shared-cap bump
   (X.3). The audit re-runs them on the commander's binary. **A single moved sensor digit fails this
   gate and rejects the bump.**

**CI plan (specify only — do NOT edit `ci.yml` in this Part).** The implementer wave: the native
`mind lang test` job greps `[lang-gradcheck] PASS`, `[lang-capacity-v2] PASS`, `[lang-recall] PASS`,
`[lang-oov] PASS`, `[lang-sensor-intact] PASS`; the live 2-process job greps `[lang-wire] PASS`; the
re-baselined LM-4..7 greps (`[r3-incontext-*]`, `[handoff-*]`, `[stream-*]`, `[teach-*]`,
`[shared-*]`) AND the sensor greps (`[onebrain-*]`, moe, dkva) must ALL stay green at the new dims —
the no-regress gate. Pretrain seconds PRINTED (gate not asserted). **The implementer edits `ci.yml`
+ the sample, not this document.**

### X.8 Provenance / closes-on

Design only. This slice closes when `[lang-gradcheck]`, `[lang-capacity-v2]` (comfortable-N strictly
> LM-8's 4), `[lang-recall]`, `[lang-oov]`, `[lang-wire]`, and **`[lang-sensor-intact]`** are green
on a clean rebuild AND CI-enforced; the LM-4..7 certs are re-baselined-and-green at the new
`R_VALV=64`; and the dtr sensor certs (`[onebrain-*]`/moe/dkva) are BYTE-IDENTICAL — audited by a
**separate** agent on the **commander's** binary, not the implementer's. The audit makes the
acceptance test; the commander reads the gate formula line-by-line — the bumped `DTR_LN_MAXW` (the
sensor-intact proof), the widened `R_NP` (built, not trusted), the `[lang-capacity-v2]` curve + the
Δ-comfortable-N bar set FROM it, the stride-7 coverage at the new `R_NP`, and the re-baselined gates
(each stricter-only vs the new chance) — line-by-line. One epitaph line in `gap-ledger.md`: LM-8's
disclaimer ("the R_DM=32 substrate's KEY recall ceiling is ~8 / comfortable-N=4") is **downgraded,
honestly: the thinking width is widened, comfortable-N rises to the measured value — still
single-token, still bounded vocab, NOT generation/grammar.**

**COMMANDER DECISIONS NEEDED (recommended defaults):**
A. **The R_DM cut** (X.2): **`R_DM=48` (RECOMMENDED** — 2.4× params / 2.0× CI for an expected
   comfortable-N ~8, double LM-8's 4) vs `R_DM=64` (3.9× / 4.0× CI, reach for comfortable-N 12–16).
   The implementer measures both behind the `R_DM` `#define`; the auditor ratifies the smaller R_DM
   that clears the discovered bar.
B. **`R_FFN` scaling** (X.2): **`R_FFN = R_DM` (RECOMMENDED** — proportional MLP for the wider
   attention; +~1.5 K params at R_DM=48) vs `R_FFN = 32` held (saves params; the cert measures if
   the MLP is the bottleneck — likely not, attention is, IX.0 #1).
C. **The LayerNorm surgery** (X.3): **raise the shared `DTR_LN_MAXW` 32→64 (RECOMMENDED** — free for
   the sensor by X.0 #1, preserves anti-fork, `[lang-sensor-intact]`-proven) vs an R3-local
   LayerNorm width (forks the shared kernel — rejected for a non-problem).
D. **Vocab-divergence signal** (X.4): **rely on the existing `/vocab` content-id channel
   (RECOMMENDED** — wire FORMAT identical, only content grew) vs bump `wire_ver` (hard partition
   between Part-X and LM-8 nodes — cleaner but discards decodable same-format packets).
E. **`R_KEYV`/`R_VALV` target** (X.2): **`R_KEYV=16`/`R_VALV=64` (RECOMMENDED** — lets the ladder
   sweep N to 16, lands the dreamed 64-answer vocab) vs a smaller bump if the curve shows R_DM=48
   still collapses before N=16 (then R_KEYV=12 matches the reachable N — don't ship an unreachable
   ladder).

## Part XI — Path W: the one mind (the weight-states literally converge)

> Status: **design + acceptance test** (written before implementation, like Parts II–X). Owner of
> *this* slice: the next wave (separate implementer + separate auditor). Builds ON: **LM-7**
> (Part VIII, the shared mind — `mind_net_task` + the region-scoped K-DDS topic `mind/teach`,
> the `MT_TEACH_PKT` wire `r3_incontext.c:39`, B's `r3_fact_learn` arrival mouth, the
> `EV_REMOTE_TEACH` galaxy emission `galaxy.h:55`, **Path E shipped**), **G22** (the no-central
> weight-averaging primitive `gl_merge`/`gl_accumulate`/`gl_scale` `gossip_learn.c:57-77`, the
> live `gl_merge_peers`/`gl_fold_cached_peers` round `gossip_learn.c:903`, the `[g22-no-central]`
> structural proof `gossip_learn.c:347`, the kill+rejoin `collective-learn-live` job
> `ci.yml:554`), **LM-5** (Part VI, `s_round(with_replay)` `r3_incontext.c:1204` — interleaved
> replay of RETAINED facts' engrams, the live `r3_consolidate_idle_round` `:1249`), **LM-9**
> (Part X, `R_NP = 21568` floats, `rw[R_NP]` `r3_incontext.c:127/137`, R3's own `r_backward`
> `:257`), **pfs_repl** (the 512 B chunked block transport `PFSR_CHUNK_SIZE`, `pfs_repl.c:293`).
> All closed in `gap-ledger.md`, so this is unblocked.

LM-7 explicitly **DEFERRED** this slice with its cert named (VIII.7): *"Path W (one-mind weight
merge) is DEFERRED with its cert stated … teach k1 on A, k2 on B, merge `rw[]`; gate that BOTH
nodes answer BOTH k1 AND k2 … (does raw averaging of two divergent minds preserve both facts,
or corrupt both?), and that interleaved post-merge replay rescues whatever the raw average loses
(the disease→cure shape, on the merge). Until that cert is green, 'the mind is literally one' is
unproven."* **This Part closes that homework.** Where Path E spreads *facts* (each node trains its
OWN weights on a gossiped engram, so the minds CONVERGE to the same bindings but keep distinct
`rw[]`), Path W merges the *weights* themselves: after consolidation, every node averages its
`rw[]` with its region peers' `rw[]` so the weight-states **literally become one substrate**.
mk_pino's words: 「心をひとつに」. It is the Collective layer made literal at the weight level — and
it is gated on an **empirical question the project refuses to assert past**: does the average of
two minds that learned DIFFERENT things answer BOTH, or NEITHER? The headline of this Part is the
disease/cure certificate that MEASURES the answer. It mirrors Parts II–X's rigor exactly: a claim,
a falsifiable certificate with bracket tags + numeric bars **discovered from measurement**, named
honest bounds, a HARD anti-fork surface flagging names that do not exist, a live multi-node cert,
and a CI verb plan.

### XI.0 IMPORTANT — what the tree actually says (read BEFORE coding)

Seven load-bearing facts. The first three set the transport by arithmetic; #4–#5 give the cure its
machinery for free; #6–#7 correct the wave framing.

1. **`gl_merge` is a plain unweighted mean, fully generic over param count.** `gl_merge(out,
   models, count, n)` (`gossip_learn.c:69`) zeroes `out[0..n)`, accumulates every model, scales by
   `1/count` — NO weighting, NO count-of-samples bias, NO param-count assumption. It already
   averages `DTR_WEIGHT_FLOATS=635` arrays in the live path; passing `n=R_NP=21568` is a **one-arg
   change, not a new averager** (the anti-fork mandate: reuse it, VIII/XI flagged names). The
   `[g22-no-central]` structural proof (`gossip_learn.c:347`: order-independence to float-rounding
   + single-model identity) is param-count-agnostic and transfers verbatim. **Confirm by build:**
   `gl_merge` has no `DTR_WEIGHT_FLOATS` baked into its body.

2. **The R3 model is 21568 floats = 86 272 bytes = 84.2 KB ≈ 22 p-fs blocks.** `R_NP=21568`
   (`_Static_assert`, `r3_incontext.c:127`); `rw[R_NP]` is 86 272 B. `PFS_BLOCK_MAX=4096`
   (`pfs_block.h`), so `rw[]` is **21.05 → 22 blocks** with the 8 B `GL_BLOB_HDR`. **`gl_pfs_publish`
   CANNOT carry it:** it `_Static_assert`s the whole blob ≤ one block (`gossip_learn.c:94`,
   `GL_MAXFLOATS=DTR_WEIGHT_FLOATS=635`, 2548 B). **`pfs_dag_save` CANNOT carry it either:** it
   returns `PFS_E_TOOBIG` for `len > PFS_BLOCK_MAX` (`pfs_dag.c:378`) — a DAG "object" is ONE
   content-addressed block, not a multi-block file. **So Path W needs chunked multi-block weight
   transport** — and it already exists: `pfs_repl.c`'s 512 B `PFSR_CHUNK_SIZE` in-order chunk path
   (`pfs_repl.c:293-440`) moves an arbitrary block; generalizing it to a 84 KB payload (or
   N×4 KB-DAG-chunks under one ref prefix) is the transport work. **Own the byte cost: ~84 KB per
   peer per merge round per node** (vs Path E's 44 B engram — a **~1900× wire premium**). This is
   the single biggest honest cost of Path W and the cert PRINTS it.

3. **The dtr 635-param body fit one block; that is WHY G22 never needed chunking.** The live
   `gl_merge_peers` (`gossip_learn.c:903`) pulls each peer's 2.5 KB model via `gl_pfs_fetch` (one
   `pfs_dag_read`) and folds it. R3's 84 KB does NOT fit that path — Path W is the FIRST consumer
   that forces multi-block weight transport into the gossip layer. Treat the new transport as a
   **hot, weight-corrupting axis** (a half-received 84 KB blob folded into `rw[]` is a corrupted
   mind): the chunk receiver must be all-or-nothing (the `pfs_repl.c:417` `chunk_idx==0` reinit +
   in-order `next_chunk` guard is the template; a partial blob is DROPPED, never merged).

4. **`s_round(with_replay=1)` is the cure mechanism, already built.** `r3_consolidate_idle_round`
   (`:1249`) calls `s_round(1)` (`:1204`), which interleaves the oldest PENDING fact's engrams with
   **ALL RETAINED facts' engrams** and runs R3's own `r_forward`/`r_backward`/`rw -= lr*rg`. Each
   node's RETAINED facts persist in `r3_fq[]` after consolidation, so **post-merge interleaved
   replay of a node's OWN facts is FREE local data** — no new engrams, no new math, no fork. If raw
   averaging is the disease, the cure candidate is: after folding the merged `rw[]` back, run a
   bounded burst of `s_round(1)` so each node re-grounds the blended weights on its own retained
   bindings (the LM-5 discipline applied fleet-wide). **This is the LM-1/LM-5 "sleep after a
   perturbation" pattern — Path W's merge IS a perturbation, and replay is how the DMN already
   heals perturbations.**

5. **`rw[]` is a file-static array with NO public accessor today.** `static float rw[R_NP]`
   (`:137`) — unlike dtr's `dtr_weights_get/set`, R3 exposes no get/set. Path W needs a **minimal
   FLAGGED pair** `r3_weights_get(float *out)` / `r3_weights_set(const float *in)` (R_NP floats,
   memcpy-equivalent, `m_quiesce()`-guarded per VII.4 so no round is mid-flight) — the dtr accessor
   mirror, NOT a new weight buffer. This is the ONLY genuinely new R3 surface; everything else
   reuses `gl_merge` + `s_round` + the LM-7 subscriber shape.

6. **No central consolidator, kill+rejoin — the template is `collective-learn-live`.** The merge
   MUST be peer-symmetric (every node folds {self} ∪ {peers it fetched}, no aggregator index —
   `gl_fold_cached_peers` `:885` is the exact pattern), order-independent (the `[g22-no-central]`
   proof), and survive a node kill+rejoin (the survivors hold the merged mind; a rejoiner pulls it).
   `samples/32_collective_learn/run.sh` (3-node disjoint-shard gossip, node kill -9'd mid-run,
   rejoin) is the integration template; `samples/41_shared_mind/run.sh` is the Path-E sibling.

7. **Region-scoped, version-honest, loop-guarded — inherit LM-7's discipline verbatim.** The merge
   is the REGION's (peers within `REGION_TAU_MS=50` ms, `region.c`); a node OUTSIDE the region does
   NOT merge (the honest negative half of the cert). Merging already-merged weights is a real
   hazard (a round-counter / merge-epoch prevents folding the same peer-state twice and lets the
   cert show convergence is monotone, not oscillating); the `origin_node==me` drop and a per-peer
   last-merge-epoch high-water are the loop guards (the LM-7 `:2463` pattern on the weight axis).

### XI.1 The claim to prove

> An owner teaches **k1→v1** on node **A** and a DIFFERENT binding **k2→v2** on node **B** (LM-6
> mouth unchanged); each node's OWN DMN consolidates its OWN fact into its OWN `rw[]` (LM-5/LM-6
> unchanged). Then, with **NO operator action and NO central aggregator**, each node publishes its
> consolidated `rw[]` (84 KB, chunked) to its region peers and **`gl_merge`s the set into a single
> shared weight-state**. Afterwards `mind ask k1` AND `mind ask k2` — **on BOTH A and B, from the
> MERGED `rw[]`, on MASKED prompts** — answer **v1 and v2** at a **measured** accuracy. The merge is
> **order-independent** (A-folds-B == B-folds-A to float rounding), **survives a node's death**
> (kill A after the merge; B still answers both), and a **rejoining node converges** to the shared
> state. A node OUTSIDE the region does **NOT** merge. Cross-arch (aarch64 + x86_64 in one region).
>
> **The honest accuracy expectation is a RANGE to be MEASURED, not asserted.** R3 facts are SPARSE
> (a binding lives in a small key-embedding + readout subspace; `O_WKE`/`O_WCLS` `:104/119`), which
> gives REASON to hope averaging preserves both (possibility (a), preservation) — but two minds that
> each ran SGD from the SAME seed toward DIFFERENT optima may have rotated their shared hidden
> subspaces into destructive misalignment (possibility (b), catastrophic averaging). **The cert
> measures which, and by how much; the design does not pre-decide it.**

This is strictly stronger than Path E's LM-7 claim. There, A and B converge to the same binding but
keep **distinct `rw[]`** ("N converging copies," VIII.7). Here the `rw[]` **are byte-converged into
one** (up to float rounding + per-node replay) — the Self/Collective boundary dissolves at the
weight level: there is no longer "A's mind" and "B's mind," there is the region's mind, instanced.

### XI.2 Path W vs Path E — complementary, not competing (and what ships HERE)

LM-7's VIII.2 recommended Path E for v1 *"not a close fork once the numbers are in,"* and shipped it.
Path W does NOT replace it — **they are complementary, and the framing that unifies them is the
beautiful one:**

> **Path E is the fleet's FAST fact-spread (the synapse); Path W is the fleet's DMN — periodic
> weight-merge = collective SLEEP/consolidation across devices (the night).** E carries a single new
> fact in one packet, immediately, so a friend's node can answer within seconds. W periodically
> reconciles the whole fleet's accumulated learning into one weight-state — the way a single brain's
> DMN reconciles a day's episodes into weights overnight (Part II), but now ACROSS bodies.

This is not decoration; it sets the **merge trigger** (XI.3): Path W's natural cadence is the
SLOW band (the G22 `GL_SLOW_BAND_MS=2000` deliberation tempo / a fleet idle-DMN pulse), NOT every
teach. Fast facts ride Path E; the fleet "sleeps together" on a slow timer and merges. **What ships
in THIS slice:** the divergent-minds disease/cure cert + the no-central + survive certs, on a
2-node-then-3-node region, with Path E LEFT RUNNING (the certs co-exist; a node may both gossip a
fact via E and merge weights via W). **What stays future:** federation (cross-region merge), the
`R_DM`-scale generative model, signed merge packets (the immune system), and a Fisher/weighted merge
if the cert shows plain mean is the bottleneck (XI.5 names it as the cure-escalation, not v1).

### XI.3 The transport — chunked multi-block weight publish (own the 84 KB)

**The fork the commander owns (DECISION 1):** how to move 84 KB of `rw[]` to region peers. Two
honest options, both reusing existing machinery — **recommend (T-a)**:

**(T-a) RECOMMENDED — N×4 KB DAG-chunk objects under a per-node ref prefix.** Publish `rw[]` as
`ceil(R_NP*4 / (PFS_BLOCK_MAX-8))` = **22 named DAG objects** `mind/w/<node>/<chunk>`, each a
`gl_pfs_publish`-shaped ≤4 KB blob (the EXISTING one-block `gl_blob` carrier, `gossip_learn.c:84`,
unchanged — just called 22 times with a chunk header `{epoch, chunk_idx, n_chunks}`). They P1-
replicate region-wide for free exactly as `self/prof` does (VIII.0 #3). The subscriber reassembles
all 22 chunks for a peer's current epoch (drop the peer for this round if any chunk is missing —
all-or-nothing, XI.0 #3) and only THEN folds. **Pro:** zero new transport code — it is the proven
DAG path, 22×; durable, content-addressed, death-surviving (a rejoiner pulls the chunks). **Con:**
22 refs/peer (the `PFS_REF_MAX=16` per-node ref budget, `pfs_dag.h:62`, is exceeded — so use ONE
ref `mind/w/<node>` holding a manifest of 22 content-ids, OR raise the budget — a sub-decision the
implementer measures).

**(T-b) — the `pfs_repl.c` 512 B unicast chunk path generalized.** Send `rw[]` as a 169-chunk
in-order stream over the private pmesh UDP port (`pfs_repl.c:293`, the `PFSR_BLK_PKT` machinery).
**Pro:** one transfer, no ref-budget pressure, the in-order `chunk_idx` guard already enforces
all-or-nothing. **Con:** unicast per-peer (N-1 sends/round), not the gossip-replicate-once model;
new code on the hot axis.

**Merge trigger (DECISION 2):** **(M-a) RECOMMENDED — a fleet-DMN slow pulse:** every node, on its
idle-DMN tick AND when the SLOW band has elapsed since its last merge, publishes its current `rw[]`
epoch and folds whatever peer-epochs are local — the "collective sleep" framing made literal. **(M-b)
on-demand `mind merge` verb** (a human/cert triggers a fleet merge) — recommended for the CERT and as
a debugging handle, likely BOTH ship (M-b drives the test, M-a is the production cadence).

**Version/loop honesty (XI.0 #7):** a `merge_epoch` counter per node, bumped each local
consolidation; the published blob carries it; a node folds a peer only if its epoch is NEWER than
the last one folded from that peer (the `pfs_repl.h:89` once-per-(src,seq) dedup on the weight axis).
This prevents re-folding a stale peer-state and makes "is convergence monotone or oscillating?" a
PRINTED, falsifiable quantity (XI.4 `[onemind-nocentral]` measures the inter-round delta shrinking).

### XI.4 The acceptance test (the certificate) — THE disease/cure headline

All numbers PRINTED, then canonical `[tag] PASS/FAIL`, greppable. The first two tags are the
headline (the question LM-7 refused to smuggle); the last two are the no-central + survival gates.
**Bars are DISCOVERED from the disease measurement, not guessed (IX.0 rule); lower-only-with-flag.**

1. **`[onemind-divergent]` — THE HEADLINE, the DISEASE measurement.** Teach k1→v1 on node 0, k2→v2
   on node 1 (DIFFERENT bindings); each node's DMN consolidates ITS OWN fact (assert each answers
   ITS OWN key ≥ a strong solo bar first — the disease is only meaningful if each mind learned its
   fact). Then `gl_merge` the two `rw[]` with **NO post-merge replay** (the naive control, exactly
   `s_round`'s `with_replay=0` analogue on the merge). **PRINT the 2×2 matrix:** node0-asks-k1,
   node0-asks-k2, node1-asks-k1, node1-asks-k2, MASKED, vs the pre-merge solo accuracies and vs
   chance (`100/R_VALV = 1.5625%`). **This tag does not assert preservation** — it RECORDS the
   number that decides possibility (a)/(b)/(c). PASS here means **the measurement ran and printed a
   well-formed matrix at a recorded merge_epoch** (the diagnostic gate); the SCIENCE gate is #2. The
   auditor reads this matrix and classifies: if both keys survive at ≥ chance+margin from the RAW
   merge, possibility (a) — the cure tag degenerates to "no cure needed, proven." If either key
   collapses toward chance, possibility (b)/(c) — the cure tag #2 becomes mandatory.

2. **`[onemind-cured]` — the CURE measurement (conditional on #1 showing degradation).** After the
   raw merge, run a bounded burst of post-merge **interleaved replay of each node's OWN RETAINED
   facts** (`s_round(1)`, XI.0 #4) on the merged `rw[]`, on BOTH nodes. **PRINT the same 2×2 matrix
   post-cure**, and the **gain (cured − naive)** per cell. PASS requires: (i) BOTH nodes answer BOTH
   keys at **≥ chance + a margin the auditor sets FROM the curve** (candidate ≥ 75% masked, but the
   bar is MEASURED — a fact that the cure cannot recover above chance FAILS, honestly), AND (ii) the
   cure does not destroy the OTHER node's fact (no catastrophic re-forgetting — the LM-1 disease must
   not reappear under the cure). **If #1 already shows preservation (possibility (a)), `[onemind-
   cured]` still runs and must show replay does not REGRESS the already-good matrix** (replay is then
   a no-op-or-better, never a harm). The disease→cure DELTA is the headline number; if the delta is
   ~0 because there was no disease, the prose says so loudly (preservation proven, cure unnecessary)
   — the honest both-ways result.

3. **`[onemind-nocentral]`** — the merge is order-independent + has no aggregator. Reuse the
   `[g22-no-central]` structural proof (`gossip_learn.c:347`) AT `n=R_NP`: merge {node0,node1}
   in forward vs reverse order, assert `|fwd−rev| max < 1e-4` (float-rounding only; an O(1) shift
   would mean a privileged position), and single-model-merge identity. PLUS the live half: print the
   inter-round max-`|rw|`-delta shrinking across ≥3 merge rounds (convergence is monotone, not
   oscillating — the merge_epoch honesty, XI.3). No node index is special; every node runs the same
   `gl_merge` over {self} ∪ {peers fetched}.

4. **`[onemind-survive]`** — kill a node, the merged mind persists. After a successful merge (BOTH
   facts answerable region-wide), `kill -9` node 0; assert node 1 STILL answers BOTH k1 AND k2 from
   its merged `rw[]` (the fact taught on the DEAD node lives on in the survivor's weights — the
   Collective outliving the Self at the weight level, strictly stronger than LM-7's `[shared-*]`
   teacher-kill which only proved B kept its OWN copy). PLUS rejoin: a fresh node joins and pulls the
   merged state (the `collective-learn-live` rejoin shape).

**Live multi-node cert** (`samples/42_one_mind/run.sh`, the 32_/41_ template): a 2-node region (then
3-node) over the relay, real chunked transport, teach-divergent → merge → ask-both-on-both, kill,
rejoin. Cross-arch (aarch64 + x86_64) — `gl_merge` is byte-portable (IEEE754 binary32, asserted
`gossip_learn.c:90`), and the chunk wire is fixed-endian.

**CI plan (specify only — do NOT edit `ci.yml` in this Part).** The implementer wave adds an
in-process native job grepping `[onemind-divergent] PASS`, `[onemind-cured] PASS`,
`[onemind-nocentral] PASS`; and a live job (the `collective-learn-live` shape, `ci.yml:554`)
running `samples/42_one_mind/run.sh` and grepping `[onemind-survive] PASS` + `RESULT: PASS` + the
production print a merge fires on a node never directly taught the other's fact. The LM-7
`shared-mind-live` + all `[g22-*]`/`[shared-*]`/`[teach-*]`/`[lang-*]` greps stay green — Path W
ADDS a path, regresses nothing. **The implementer edits `ci.yml` + the sample, not this document.**

### XI.5 Conflict semantics — averaging BLENDS (belief-blending, not belief-revision)

Path E REFUSES a same-key conflict (VIII.5: k taught v on A and v'≠v on B → the second is refused +
printed). **Path W cannot refuse — it AVERAGES.** If A consolidated k→v and B consolidated the SAME
k→v', the merged `rw[]` is the mean of two weight-states pulled toward different readout classes for
the same key. **The cert MEASURES what the blend answers** (a fifth diagnostic print under
`[onemind-divergent]`: teach k→v on A, k→v' on B, merge, ask k on both — record the argmax and its
confidence). The honest semantics, stated loudly: **this is belief-BLENDING — a categorically
different thing from belief-revision.** The merged mind may answer v, v', a third class entirely, or
low-confidence noise; whatever it does is RECORDED, not engineered. v1 does NOT promise a
conflict-resolution policy on the weight path — it ships the honest measurement and names
belief-revision-over-merge as its own future slice (the immune system + provenance-weighted merge,
where the higher-trust teacher's weights dominate — that is the Fisher/weighted-merge escalation,
NOT plain `gl_merge`). **COMMANDER DECISION 4.**

### XI.6 Anti-fork surface — reuse, do NOT fork

- **REUSE `gl_merge`/`gl_accumulate`/`gl_scale`** (`gossip_learn.c:57-77`) at `n=R_NP` — do NOT
  write a second averager. The auditor greps that the R3 merge call site passes `gl_merge`.
- **REUSE `s_round(1)`/`r3_consolidate_idle_round`** (`:1204/1249`) for the cure — do NOT write a
  second replay loop. The cure is the EXISTING interleaved-replay round run post-merge.
- **REUSE the LM-7 `mind_net_task` subscriber shape + `EV_REMOTE_TEACH` galaxy pattern** — the merge
  subscriber mirrors it; do NOT fork a second region-poll task style.
- **REUSE `pfs_dag`/`gl_pfs_publish`/`pfs_repl` chunk transport** (XI.3) — do NOT invent a new wire.
- **The ONLY new R3 surface:** `r3_weights_get`/`r3_weights_set` (FLAGGED — they do NOT exist today,
  XI.0 #5), the dtr-accessor mirror; a `merge_epoch` field; one merge subscriber task; one
  `EV_MERGE` galaxy type (XI.7). Do NOT fork a second `rw[]` buffer or a second R3.

**Honest bounds (what is NOT claimed):**
- **R3 toy scale** (R_DM=48, 8×64 synthetic vocab, VII.8/X stands) — `teach 2 3` is a key→value
  binding, NOT language; this is one-mind at the SUBSTRATE level, not a generative LM.
- **Averaging is lossy by the MEASURED amount** — `[onemind-divergent]` records exactly how much;
  if the cure cannot reach the bar, the slice ships the HONEST negative (plain mean insufficient at
  this divergence; weighted/Fisher merge named as the next slice) — it does NOT lower the bar to
  pass. The whole point is to refuse "the mind is one" without the number.
- **~84 KB wire cost per peer per merge round** (XI.0 #2, ~1900× Path E) — the cert PRINTS it; Path W
  is the SLOW-band fleet-sleep, not the per-teach hot path (that is Path E).
- **Region-scoped, best-effort, tamper-EVIDENT** — federation, signed-merge immune system, and
  durable death-surviving delivery beyond the DAG-chunk path inherit LM-7's VIII.7 bounds verbatim.
- **Belief-BLENDING, not revision** (XI.5) — same-key conflict averages; the answer is measured.
- **Cert verbs remain amnesia bombs** (VII.0 #5) — persisting the merged `rw[]` across runs is a
  named non-goal.

No false "by construction" theorem is asserted. **The headline is a number, measured both ways.**

### XI.7 Galaxy hook — the stars pulse in unison (collective sleep)

ONE new emission point: `galaxy_emit(EV_MERGE, me, peer, merge_epoch16, peers_folded)` at the moment
a node folds its region into `rw[]` (XI.3, after `gl_fold_cached_peers`'s R3 analogue returns). In
the galaxy view, a fleet weight-merge renders as **the region's stars pulsing in unison** — the
collective-sleep heartbeat, distinct from Path E's `EV_REMOTE_TEACH` single-fact spark. The web
bridge (`galaxy.c`) gains the `EV_MERGE` case label only (the "merge" string, mirroring the
`EV_CONSOLIDATE`/`EV_REMOTE_TEACH` labels `galaxy.c:384`). Sampled like the chatty types if needed;
ONE call site, named here so the auditor greps exactly one new `galaxy_emit(EV_MERGE`.

### XI.8 Provenance / closes-on

Design only. This slice closes when `[onemind-divergent]` (the disease matrix printed + classified),
`[onemind-cured]` (BOTH facts on BOTH nodes ≥ the auditor's measured bar, OR the honest negative
with the weighted-merge follow-up named), `[onemind-nocentral]`, and `[onemind-survive]` are green
on a clean rebuild AND CI-enforced; the LM-7 `[shared-*]` + `[g22-*]` + `[lang-*]` certs stay green
(Path W adds, regresses nothing); the ~84 KB wire cost is PRINTED. Audited by a **separate** agent on
the **commander's** binary, not the implementer's. **The audit MAKES the acceptance test (the
`[onemind-cured]` bar discovered from the disease curve, line-by-line); the commander reads the gate
formula — the disease matrix, the cure delta, the no-central `|fwd−rev|` bound at `n=R_NP`, the
survive-kill, the chunk-transport all-or-nothing guard — line-by-line** (the validator-trap memory:
certify on production code, not a sim). One epitaph line in `gap-ledger.md`: LM-7's VIII.7 deferral
("Path W is DEFERRED with its cert stated") is **discharged — the averaging-of-divergent-minds
question is MEASURED, the mind is one at the substrate to the measured accuracy, with the honest
loss/cure named.**

**COMMANDER DECISIONS NEEDED (recommended defaults):**
1. **Weight transport** (XI.3): **(T-a) N×4 KB DAG-chunk objects (RECOMMENDED** — zero new transport,
   reuses the proven `gl_pfs_publish`/`pfs_dag` path 22×, durable + death-surviving; sub-decision: one
   manifest-ref vs raise `PFS_REF_MAX`) vs **(T-b)** the `pfs_repl.c` 512 B unicast chunk stream (one
   transfer, no ref pressure, but new code on the hot weight axis). The implementer measures the
   reassembly all-or-nothing guard either way.
2. **Merge trigger** (XI.3): **BOTH (RECOMMENDED** — (M-a) fleet-DMN slow-band pulse as the
   production "collective sleep" cadence + (M-b) `mind merge` verb to drive the cert/debug) vs M-b
   only for v1 (defer the autonomous cadence — simpler, but punts the "fleet sleeps together"
   framing this Part exists for).
3. **The cure mechanism IF the disease is real** (XI.0 #4 / XI.4 #2): **post-merge interleaved
   `s_round(1)` replay of each node's OWN retained facts (RECOMMENDED** — zero new math, the LM-5
   discipline, free local data) vs **more merge rounds** (cheaper if the disease is mild — measure
   first) vs **a weighted/Fisher merge** (forks `gl_merge` into a weighted variant — ONLY if plain
   mean + replay both miss the bar; named as the escalation, NOT v1).
4. **Conflict policy on the weight path** (XI.5): **measure-and-report the blend, NO resolution
   policy in v1 (RECOMMENDED** — belief-blending is honest; revision-over-merge is its own slice) vs
   a provenance-weighted merge now (couples to the unbuilt signing immune system — rejected for v1).
5. **Path-W-vs-E shipping scope** (XI.2): **ship Path W ALONGSIDE Path E, complementary —
   E=fast-synapse, W=fleet-DMN-sleep (RECOMMENDED)** vs ship W as a replacement for E (rejected — E's
   one-packet self-consistent spread is strictly cheaper for the buzz demo; they are different jobs).

## Part XII — Path W²: the weighted / Fisher merge (does a SMARTER merge beat the plain mean?)

> Status: **design + acceptance test** (written before implementation, like Parts II–XI). Owner of
> *this* slice: the next wave (separate implementer + separate auditor). Builds ON: **LM-10 / Path W**
> (Part XI — the one-mind weight merge, `gl_merge` at `n=R_NP`, `r3_weights_get/set`, `merge_epoch`,
> the chunked 84 KB transport, the `[onemind-*]` 2×2 divergent-minds matrix harness `r3_incontext.c`),
> **G22** (`gl_accumulate`/`gl_scale`/`gl_merge` `gossip_learn.c:57-77` — the plain mean), **LM-9**
> (Part X, `R_NP=21568`, `rw[R_NP]`, `rg[R_NP]`, R3's own `r_backward` `r3_incontext.c:260` that
> accumulates per-parameter gradients into `rg[]`), **LM-5** (Part VI, `s_round(1)` union replay — the
> LM-10 cure). LM-10 is closed in `gap-ledger.md` (wave-41), so this is unblocked.

LM-10 **MEASURED the disease and named THIS slice as its own follow-up**: *"plain mean is lossy (the
disease is real), union replay makes the mind one (the cure is real) … Follow-ups: weighted/Fisher
merge for higher divergence."* XI.5 names it precisely: *"provenance-weighted/Fisher merge named as
the next slice IF plain mean is the bottleneck."* **This Part closes that homework — but it refuses
to assume the answer, exactly as LM-10 refused to assume averaging preserved both facts.** The
disease is KNOWN (wave-41, MEASURED on the `0xA5A5` substrate): two divergent minds (k1=2→3 on node0,
k2=4→1 on node1, SAME seed), each solo at **100%**; naive `gl_merge` answers **k1=100%, k2=8.8%**
(chance 1.5625%) — classification **(c) PARTIAL**, one fact survives, one collapses toward chance.
The union-replay cure recovered BOTH to 100%. **The question THIS Part measures is sharper:** can a
**weighted** merge — provenance/count-weighted (W1), Fisher-information-weighted (W2), or
task-arithmetic/sign-aligned (W3) — **preserve BOTH facts with LESS (or NO) reliance on the replay
crutch, and/or survive HIGHER divergence than plain mean?** The headline is a **comparison number**
(weighted-merge matrix vs plain-mean matrix, BOTH printed, the Δ is the result), not an assertion —
and **"the mean is hard to beat at this scale" is a legitimate, honest outcome the cert is designed
to surface truthfully.**

### XII.0 IMPORTANT — what the tree actually says (read BEFORE coding)

Eight load-bearing facts. The first three give the Fisher its machinery for FREE; #4–#5 set the
honest baseline + the "3× prior" trap; #6–#8 bound the surface and the wire.

1. **`gl_merge` is a PLAIN UNWEIGHTED mean — there is NO per-parameter weighting today.**
   `gl_merge(out, models, count, n)` (`gossip_learn.c:69`) zeroes `out`, accumulates each model with
   the SCALAR-uniform `gl_accumulate` (`acc[i] += w[i]`, `:57`), and scales by the SCALAR `1/count`
   (`gl_scale`, `:63`). There is no weight vector, no count-of-samples bias, no Fisher. **A weighted
   merge needs a per-parameter (or per-model) weight — and the anti-fork mandate is to GENERALIZE the
   existing primitives, not write a second averager (XII.6).** The minimal honest generalization:
   a `gl_accumulate_w(acc, w, wt, n)` that does `acc[i] += wt[i] * w[i]` (per-param weight) and a
   normalizing pass `acc[i] /= wsum[i]` — i.e. a per-parameter weighted mean `Σ wtₖ[i]·wₖ[i] / Σ wtₖ[i]`.
   This is a 2-line sibling of `gl_accumulate`/`gl_scale`, NOT a fork of `gl_merge`'s no-central
   structure (the `[g22-no-central]` order-independence proof transfers verbatim — a per-param
   weighted sum is STILL order-independent to float rounding, and the cert re-proves it at `n=R_NP`).

2. **`r_backward` ALREADY accumulates the per-parameter gradient into `rg[R_NP]` — so the diagonal
   Fisher is one squaring loop away, reusing EXISTING kernels (no new math class).** `r_backward(label)`
   (`r3_incontext.c:260`) runs the full backprop and writes `rg[O_BCLS+c] += …`, `rg[O_WCLS+…] += …`,
   etc. — every one of the `R_NP=21568` parameters gets its gradient. The diagonal Fisher of a node is
   the **expected squared gradient over the node's own data**: `F[i] = (1/M) Σ_examples rg[i]²`. The
   training loop ALREADY does `for i: rg[i]=0; r_forward; r_backward; rw -= lr*rg` (`:454-457`); the
   Fisher accumulation is the SAME loop with `F[i] += rg[i]*rg[i]` instead of (or alongside) the SGD
   step — over the node's RETAINED engrams (the `s_round` item list, XII.0 #4). **The Fisher is cheap
   and reuses `r_forward`/`r_backward` verbatim; the ONLY new storage is one `float F[R_NP]` per node
   (~84 KB scratch, file-static `.bss`, the stack-overflow lesson) and the diagonal-Fisher
   accumulation loop. Own the cost: ~`R_NP` extra floats + one backward pass per retained engram.**

3. **R3 has a SHARED pretrain seed (`0xA5A5`) — so the task VECTORS (W3) are well-defined.** `s_pretrain()`
   (`r3_incontext.c`) calls `r_init_weights(0xA5A5u)` then the fixed 60-epoch schedule; BOTH modelled
   nodes in the `[onemind-*]` harness start from the SAME `w_base` (`r3_onemind_test`: `r3_weights_get(w_base)`
   then each node trains from `w_base`). The **task vector** of node k is `τₖ = rwₖ − w_base` (the delta
   the node's own consolidation pushed). TIES/task-arithmetic (W3) merges `w_base + Σ (sign-resolved,
   trimmed) τₖ` instead of the raw `rwₖ`. `w_base` is ALREADY captured in the harness, so the deltas
   are FREE to compute — no new state, just a subtraction.

4. **The disease's "3× prior" is a STABLE substrate property, NOT a seed artifact — and it is exactly
   what a weighted merge must overcome.** wave-41's WHY (gap-ledger, the LM-10 epitaph): the SURVIVING
   value `OM_V1=3` clears the halved-gradient margin **because value 3 appears 3× in the SDICT prior**
   (`SDICT = {2,0,3,3,1,3,5,7}` `r3_incontext.c:1101` — class 3 at keys 2,3,5). The pretrained substrate
   already biases its readout toward class 3, so after the merge halves k1's gradient pull, the prior
   carries it; the COLLAPSING value `OM_V2=1` appears only ONCE (key 4) and has no such prior support,
   so the halved pull drops it to chance. **This means the plain mean's "win" on k1 is partly the
   PRIOR doing the work, not the average preserving the fact.** A weighted merge that up-weights node1's
   k2-bearing parameters is the principled fix — BUT the cert must measure whether it ACTUALLY recovers
   k2 above the plain mean's 8.8%, or whether the prior asymmetry is too strong to beat without replay.
   **(XII.4 mandates a `[wmerge-symmetric]` control: re-run the matrix with a SYMMETRIC binding — two
   values that BOTH have equal prior support — so the comparison is not contaminated by the 3× prior.)**

5. **The honest baseline the weighted merge must BEAT is the naive `gl_merge` (k1=100%/k2=8.8%), NOT
   the cured one.** The cure (union replay) already reaches 100%/100% (LM-10). A weighted merge that
   STILL needs the full replay to pass has not earned its complexity — the cert's reason to exist is
   **does the weighted merge reduce or remove the replay dependency?** So the comparison axis is:
   {plain mean, weighted merge} × {no replay, with replay}, and the headline cells are the **no-replay**
   ones. **If weighted-merge-no-replay ≈ plain-mean-no-replay (i.e. k2 still ~chance), the honest
   result is "the weighting does not help at this scale; the prior asymmetry / subspace misalignment
   dominates; union replay remains the cure" — and that ships as the truthful negative (XII.4, XII.6
   bounds), weighted merge NOT adopted for v1.** The number decides, not the design.

6. **The `[onemind-*]` harness is the EXACT infrastructure to reuse — save/restore `rw[]`, the 2×2
   matrix, `om_acc`, `om_print_matrix`, the divergent bindings.** `r3_onemind_test` (`r3_incontext.c`)
   already: pretrains the shared `w_base`; trains `w_a` (node0, k1→v1) and `w_b` (node1, k2→v2) by
   save/restore; calls `gl_merge(mw_out, {w_a,w_b}, 2, R_NP)`; prints the matrix via `om_print_matrix`;
   measures `om_acc(k,v)` = masked majority vote share. **The weighted-merge cert is a NEW branch in
   the SAME harness that, after computing `w_a`/`w_b`, ALSO computes each node's Fisher `F_a`/`F_b`
   (W2) or task vectors (W3), runs the weighted merge into a second output, and prints the SECOND
   matrix beside the plain one. Zero new harness scaffolding — extend `r3_onemind_test` or add a
   sibling `r3_wmerge_test()` that reuses `om_acc`/`om_print_matrix`/the divergent bindings.**

7. **Fisher needs NO extra wire if computed LOCALLY from each node's own RETAINED engrams (RECOMMENDED).**
   The diagonal Fisher of node k is computed from node k's OWN data — and node k STILL HOLDS its own
   retained engrams in `r3_fq[]` after consolidation (XI.0 #4, the same fact the LM-10 cure exploited).
   So **each node computes ITS OWN Fisher locally and publishes it ALONGSIDE its `rw[]`** — OR, the
   cheaper option, each node publishes only `rw[]` (the existing 84 KB chunks) and a RECEIVING node
   computes the Fisher of EACH peer from… no — a node cannot compute a peer's Fisher without the peer's
   data. **So the honest fork (COMMANDER DECISION 2): local-Fisher-on-wire (each node sends its `F[R_NP]`
   diagonal = a SECOND 84 KB blob/peer, doubling Path W's wire to ~168 KB/peer/round) vs.
   approximate-Fisher-from-shared-engrams (in the live fleet, Path E has ALREADY spread both facts'
   engrams to every node — so every node can compute EACH fact's Fisher from the engrams it locally
   holds, NO extra wire).** The second is the beautiful one and the recommendation: **Path E's engram
   spread makes the Fisher a LOCAL computation — no Fisher on the wire — because the receiving node
   already has the data to weight by.** The cert measures both; the wire-cost verdict is PRINTED.

8. **The new surface is SMALL and FLAGGED: a weighted accumulator + a Fisher loop + a weighted-merge
   cert branch. NO new R3, NO new transport, NO fork of `gl_merge`'s no-central core.** Flagged names
   that DO NOT EXIST today (XII.6): `gl_accumulate_w` (per-param weighted accumulate), `gl_merge_w`
   (the weighted-mean wrapper, normalizing by the per-param weight sum), `r3_fisher_diag(float *out)`
   (accumulate `rg[i]²` over the node's retained engrams into `out[R_NP]`), and the cert branch
   `[wmerge-*]`. Everything else — `r_forward`/`r_backward`/`s_round`/`rw[]`/`om_acc`/the chunk
   transport — is reused verbatim. The auditor greps that `gl_merge` (the plain mean) is UNCHANGED and
   still drives LM-10, and that the weighted path is a SIBLING, not a mutation.

### XII.1 The claim to prove

> Two divergent minds (the LM-10 `[onemind-divergent]` model — k1→v1 on node0, k2→v2 on node1, SAME
> `0xA5A5` seed, each consolidated into its OWN `rw[]`) are merged by a **weighted** rule instead of
> the plain mean: each parameter is averaged weighted by **how much it MATTERS to each node's learned
> fact** (W2, the diagonal Fisher `Fₖ[i] = E[rg[i]²]` over node k's own retained engrams) — so the
> node that actually LEARNED a fact dominates that fact's parameters. On the SAME 2×2 (node × fact)
> MASKED matrix, **WITHOUT post-merge replay**, the weighted merge answers k1 AND k2 at an accuracy
> that is **MEASURED against the plain mean's (100% / 8.8%)** — BOTH matrices printed side by side,
> the per-cell Δ being the result. The weighted merge is still **order-independent** (a per-param
> weighted sum re-proves `[g22-no-central]` at `n=R_NP`) and needs **NO extra wire** if the Fisher is
> computed locally from Path-E-spread engrams (XII.0 #7).
>
> **The expectation is a RANGE to MEASURE, stated honestly both ways.** OPTIMISTIC (the principled-ML
> hope): Fisher up-weights node1's k2-readout parameters, so the merged readout keeps k2's class-1
> mass that the plain mean halved → k2 recovers toward the solo 100% with NO replay, and the merge
> survives HIGHER divergence (more facts, larger value separation) before either fact collapses.
> PESSIMISTIC (the honest null): at R3's TOY scale (R_DM=48, R_NP=21568, a SINGLE binding per node)
> the two facts live in a SHARED, heavily-overlapping hidden subspace and the diagonal Fisher cannot
> separate them — the weighting moves k2 from 8.8% to, say, 30–50% but NOT to the 75% bar without
> replay, OR the prior asymmetry (XII.0 #4, the 3× SDICT support for v1) dominates the Fisher signal.
> **In that case the truthful result is "the diagonal Fisher does not beat the plain mean enough to
> remove the replay crutch at this scale" — and the slice ships that negative, naming full-Fisher /
> larger-scale as the future, NOT lowering a bar to pass.** The cert is built so BOTH outcomes are a
> clean, greppable, honest PASS-of-the-measurement.

This is strictly a follow-up to LM-10's XI.1, not a replacement: LM-10 proved the mind is one *with*
the replay cure; XII asks whether a smarter MERGE buys the same oneness with *less* replay, or holds
at divergence where replay alone would not.

### XII.2 The recommended primitive — W2 (diagonal Fisher), with W1/W3 named and measured

**The fork the commander owns (DECISION 1):** which weighted rule is v1. Three candidates, ALL
evaluated by the cert (XII.4 runs the matrix under each that is built); **recommend W2 for the
PRIMARY, W1 as the cheap baseline-of-the-weighted-family, W3 as the named alternative.**

**(W2) RECOMMENDED — diagonal Fisher-weighted merge (EWC/elastic-merge style).** Per parameter:
`rw_merged[i] = (F_a[i]·w_a[i] + F_b[i]·w_b[i]) / (F_a[i] + F_b[i] + ε)`, where `Fₖ[i]` is node k's
diagonal Fisher over its own retained engrams (XII.0 #2). This is the **classic principled
model-merging answer** (Fisher-weighted averaging / elastic weight consolidation): a parameter that
MATTERS to node1's fact (large `F_b[i]`) is pulled toward node1's value, regardless of node0. **WHY
it is the right primary:** (a) it directly targets the disease's mechanism — the plain mean halves
BOTH facts' gradients equally, but Fisher RESTORES the asymmetry the facts earned; (b) the cost is
freestanding and OWNED — `F[R_NP]` floats + one backward pass per retained engram, reusing
`r_backward` (XII.0 #2), `~R_NP` extra floats per node, no new math class; (c) it composes with the
existing no-central proof (a per-param weighted sum is order-independent). **The freestanding cost,
named:** one `float F[R_NP]` scratch per node (84 KB `.bss`), `M` extra `r_forward`+`r_backward`
passes (M = node's retained-engram count, ~8–16 at toy scale — cheap), and the `ε` floor (a small
constant so a parameter neither node trained falls back to the plain mean — `Fₖ[i]≈0` for both ⇒ the
formula → `(w_a+w_b)/2`, exactly the plain mean, which is the CORRECT fallback for the shared
pretrained backbone).

**(W1) — count/confidence-weighted mean (the cheap baseline of the weighted family).** Weight each
node's WHOLE model by a scalar confidence (e.g. its solo masked-accuracy on its own fact, or its
retained-engram count): `rw_merged = (c_a·w_a + c_b·w_b)/(c_a+c_b)`. This is a SCALAR weight, not
per-parameter — it cannot separate facts that share parameters, so at this single-binding scale it is
likely **indistinguishable from the plain mean when both nodes are equally confident** (both solo at
100% ⇒ c_a=c_b ⇒ exactly the mean). **It is in the cert as the cheap control** (does ANY weighting
help, or specifically the PER-PARAMETER Fisher?) — the expected result is "W1 ≈ plain mean here,"
which is itself an honest, informative measurement (it isolates that W2's gain, IF any, comes from
per-parameter resolution, not mere weighting).

**(W3) — task-arithmetic / TIES (sign-aligned delta merge).** Merge the task VECTORS `τₖ = rwₖ −
w_base` (XII.0 #3): `rw_merged = w_base + Σₖ trim(τₖ)` with TIES sign-resolution (per parameter,
keep only the deltas agreeing with the dominant sign, average them). **This is the strongest
candidate for HIGHER divergence / MORE facts** (it is designed to add many task vectors without
destructive interference) but it is the MOST new code (the trim + sign-elect pass) and its win at the
2-fact toy scale may be marginal. **Recommend it as the NAMED alternative the cert measures if W2
underperforms — the escalation for the "survive higher divergence" half, not the v1 primary.**

**Anti-fork (XII.0 #1):** all three are expressed through `gl_accumulate_w`/`gl_merge_w` (the
per-param weighted-mean generalization) — W2 passes `Fₖ` as the weight, W1 passes a broadcast scalar,
W3 operates on `τₖ` with a sign-elected per-param weight ∈ {0,1}. **One weighted-mean primitive, three
weight-vector recipes — `gl_merge` (the plain mean) stays UNCHANGED and keeps driving LM-10.**

### XII.3 Transport — the Fisher needs NO extra wire (compute it LOCALLY)

**The fork the commander owns (DECISION 2):** does the weighting add data to the wire? Path W already
pays ~84 KB/peer/round for `rw[]` (XI.0 #2). The Fisher diagonal `F[R_NP]` is ANOTHER 84 KB if sent.
Two honest options — **recommend (F-local)**:

**(F-local) RECOMMENDED — compute every contributor's Fisher LOCALLY, NO Fisher on the wire.** In
the live fleet, **Path E has already spread both facts' engrams to every node** (LM-7, the 44 B
`MT_TEACH_PKT` — that is its whole job). So a node that received peer's `rw[]` (84 KB) ALSO holds
peer's FACT-engram (44 B, via E) in its `r3_fq[]`. It can therefore compute the Fisher of EACH
contributing weight-state from the LOCAL engrams: `F_peer[i] = Σ_{peer's engrams} rg[i]²` evaluated
**on the peer's `rw[]`** (the node has both — peer's weights from W's chunks, peer's data from E's
engram). **This is the unification XII exists to surface: Path E (fast fact spread) is what makes
Path W's Fisher a free LOCAL computation — E carries the DATA, W carries the WEIGHTS, and the Fisher
that fuses them needs no third channel.** Wire cost UNCHANGED at ~84 KB/peer (the cert PRINTS that
the weighted merge adds 0 bytes to the wire vs LM-10).

**(F-wire) — each node publishes its own Fisher diagonal alongside `rw[]`.** A second 22-chunk 84 KB
blob `mf<node>` (the `mw<node>` manifest sibling), doubling Path W's wire to ~168 KB/peer/round.
**Pro:** correct even WITHOUT Path E running (a node that only got weights, never the engram, still
gets the exact local Fisher). **Con:** 2× the already-1900×-Path-E wire; couples W to nothing but
itself but pays double. **Recommend F-wire ONLY as the fallback when E is not co-running** (the cert
runs in-process where both `rw[]` and engrams are local anyway, so the cert itself never needs the
wire — the wire fork is a LIVE-path decision the `[wmerge-*]` in-process cert does NOT gate on).

**For the in-process cert:** there is NO wire at all — `w_a`,`w_b`,`F_a`,`F_b` are all local statics
(the LM-10 harness model). The transport fork is named for the LIVE `42_one_mind` extension, which is
a NAMED FUTURE for this slice (the in-process `[wmerge-*]` matrix is the headline; a live weighted
merge over the relay inherits LM-10's `[onemind-survive]` shape and is the follow-up — XII.8).

### XII.4 The acceptance test (the certificate) — THE comparison headline

All numbers PRINTED, then canonical `[tag] PASS/FAIL`, greppable. **The headline is a COMPARISON: the
SAME divergent-minds matrix under plain mean vs weighted merge, BOTH printed, the Δ the result. Bars
DISCOVERED from the disease measurement, lower-only-with-flag (IX.0 rule). If the weighted merge does
NOT beat the plain mean, that is an HONEST PASS-of-the-measurement reported truthfully — `[wmerge-vs-mean]`
PASS means the comparison RAN and printed a well-formed Δ, NOT that weighted won.**

1. **`[wmerge-vs-mean]` — THE HEADLINE (the comparison).** On the LM-10 divergent model (k1→v1 node0,
   k2→v2 node1, same seed, each consolidated), compute BOTH merges from the SAME `w_a`/`w_b`:
   `gl_merge` (plain mean, the LM-10 control) and `gl_merge_w` with the chosen weight (W2 Fisher
   primary). PRINT both 2×2 matrices and the **per-cell Δ (weighted − mean)**, vs the solo accuracies
   and chance. **PASS = the comparison ran and printed a well-formed Δ at a recorded epoch** (the
   diagnostic gate, the LM-10 `[onemind-divergent]` discipline). The auditor reads the Δ and
   classifies: **WEIGHTED-WINS** (k2 materially higher under weighted, both ≥ the discovered bar) →
   weighted merge adopted; **TIE** (|Δ| within the matrix's measurement noise on every cell) →
   "weighting does not help at this scale, plain mean + replay remains the recommendation," honest
   negative; **WEIGHTED-LOSES** (weighted is WORSE — possible if the diagonal Fisher mis-weights a
   shared parameter) → reported truthfully, weighted REJECTED. The science gate is #2.

2. **`[wmerge-noreplay]` — does the weighted merge alone clear the bar WITHOUT `s_round` replay?**
   The reason-to-exist measurement (XII.0 #5). Take the weighted-merge matrix from #1 (NO post-merge
   replay) and gate: do BOTH facts answer ≥ the bar **on the raw weighted merge**? The bar is
   DISCOVERED — candidate ≥ 75% masked (the LM-6 share gate, the LM-10 cure bar) but **the auditor
   sets it FROM the weighted-merge curve**; a weighted merge that lands k2 at 50% (better than the
   mean's 8.8% but below 75%) is a MEASURED PARTIAL that FAILS the "removes the replay crutch" claim
   honestly while the prose records the real gain. PASS = BOTH facts ≥ bar on the raw weighted merge
   (the replay crutch is REMOVED). FAIL-honest = the printed partial, with "union replay still
   required; weighted merge reduces but does not remove the dependency by Δ=…%." **Both are clean
   greppable outcomes; neither lowers a bar.**

3. **`[wmerge-divergence]` — the honest CEILING (sweep divergence, find where even weighted fails).**
   The "survive higher divergence" half. Sweep the divergence and PRINT, for plain-mean vs weighted,
   the point at which the SECOND fact collapses toward chance. Divergence knobs (pick the cleanest):
   (a) MORE facts — 2 → 3 → 4 divergent bindings merged (each on its own modelled node); (b) value
   SEPARATION — bindings whose values share less prior support; (c) the `[wmerge-symmetric]` control
   (XII.0 #4) — re-run with two values of EQUAL prior support so the 3× SDICT asymmetry does not
   contaminate the comparison. PRINT, per merge rule, the highest divergence at which BOTH facts stay
   ≥ bar (no replay). **PASS = the sweep ran and printed a well-formed ceiling curve for both rules**
   (the honest ceiling is the deliverable — whether weighted's ceiling is HIGHER than mean's is the
   measured result the prose states plainly, not a gate that can fail to "weighted didn't win").

4. **`[wmerge-nocentral]`** — the weighted merge is STILL order-independent + aggregator-free at
   `n=R_NP`. Reuse the `[onemind-nocentral]` proof shape (`r3_onemind_nocentral_test`) but through
   `gl_merge_w`: merge {node0,node1,node2} weighted, forward vs reverse, assert `|fwd−rev| max <
   1e-3` (a per-param weighted SUM is order-independent to float rounding — if it isn't, the weight
   normalization has a position bug). PLUS single-model identity (`gl_merge_w` of one model with any
   positive weight == that model). **The weighted merge must NOT smuggle a central aggregator.**

5. **`[wmerge-noregress]` — LM-10 plain Path W still works (HARD anti-regress).** The `[onemind-divergent]`,
   `[onemind-cured]`, `[onemind-nocentral]` tags stay byte-identically green (the disease matrix is
   still 100%/8.8%, the union cure still 100%/100%) — the weighted path is ADDITIVE, `gl_merge`
   UNCHANGED. The auditor greps that LM-10's three tags + the LM-7 `[shared-*]` + `[g22-*]` + `[lang-*]`
   are all still PASS. Path W² adds a path, regresses nothing.

**Live multi-node cert** is a NAMED FUTURE (XII.8): the in-process `[wmerge-vs-mean/noreplay/divergence/nocentral]`
matrix is THIS slice's headline (the comparison is fully measurable in-process — `w_a`,`w_b`,`F_a`,`F_b`
all local). A LIVE `42_one_mind`-style weighted merge over the relay (real chunked weights + Path-E
engrams → local Fisher → weighted fold) inherits the `[onemind-survive]` shape and ships as the
follow-up once the in-process comparison says weighted is worth the wire.

**CI plan (specify only — do NOT edit `ci.yml` in this Part).** The implementer wave adds an in-process
native job grepping `[wmerge-vs-mean] PASS`, `[wmerge-noreplay] PASS` (PASS or honest-negative — the
tag is greppable either way; the WIN/TIE/LOSE classification is in the printed prose the auditor
reads), `[wmerge-divergence] PASS`, `[wmerge-nocentral] PASS`, `[wmerge-noregress] PASS`. The LM-10
`[onemind-*]` + LM-7 `[shared-*]` + `[g22-*]` + `[lang-*]` greps stay green. **The implementer edits
`ci.yml` + the harness, not this document.**

### XII.5 Conflict / blending semantics under weighting (vs LM-10's plain blend)

LM-10's XI.5: the plain mean BLENDS a same-key conflict (k→v on A, k→v' on B → the merged readout is
the mean of two readouts, the answer is MEASURED, belief-BLENDING not revision). **Under weighting the
semantics SHIFT, and the cert measures the shift:** a Fisher-weighted merge of a same-key conflict
pulls the readout toward whichever node's k-parameters have HIGHER Fisher — i.e. **the node that
learned k harder (more engrams / sharper gradient) DOMINATES the blend.** This is a step TOWARD
belief-revision (the more-confident teacher wins) without an explicit policy — **but it is still
emergent from the weighting, NOT an engineered revision rule.** The cert prints the same-key blend
under W2 (the `[wmerge-vs-mean]` fifth diagnostic, mirroring LM-10's XI.5 print): teach k→v on node0,
k→v' on node1, weighted-merge, ask k, record argmax + confidence vs the plain-mean blend. **Honest
semantics, stated loudly: Fisher-weighting makes the blend CONFIDENCE-DOMINATED, which LOOKS like
provenance-weighted revision but is NOT a trust/identity policy** (the ark never verifies humans —
provenance-weighted-by-IDENTITY is a different, future thing coupled to the unbuilt signing immune
system, XII.6 bounds). v1 ships the MEASUREMENT of the confidence-dominated blend, names
identity/trust-weighted revision as its own future slice.

### XII.6 Anti-fork surface — reuse, do NOT fork

- **`gl_merge` (the plain mean, `gossip_learn.c:69`) stays UNCHANGED and keeps driving LM-10.** The
  auditor greps that LM-10's `gl_merge(mw_out, models, 2, R_NP)` call site is byte-identical and the
  `[onemind-*]` tags stay green. The weighted path is a SIBLING.
- **REUSE `gl_accumulate`/`gl_scale` (`:57/63`) by GENERALIZING, not forking:** the only new gossip
  surface is `gl_accumulate_w(acc, w, wt, n)` (per-param weighted accumulate) + `gl_merge_w(out,
  models, weights, count, n)` (normalize by `Σ wt`) — the 2-line weighted siblings, NOT a second
  no-central averager (the order-independence proof transfers; `[wmerge-nocentral]` re-certifies it).
- **REUSE `r_forward`/`r_backward`/`rg[R_NP]` (`r3_incontext.c:191/260`) for the Fisher** — the
  diagonal Fisher is `rg[i]²` accumulated over the node's retained engrams using the EXISTING backward
  pass; the only new R3 surface is `r3_fisher_diag(float *out)` (the accumulation loop, the `s_round`
  item-list discipline reused) + one `float F[R_NP]` `.bss` scratch. Do NOT write a new gradient path.
- **REUSE the `[onemind-*]` harness** (`r3_onemind_test`/`om_acc`/`om_print_matrix`/the divergent
  bindings + the save/restore-`rw[]` divergent-mind model) — the `[wmerge-*]` cert is a new BRANCH in
  the same harness, not a new test scaffold.
- **REUSE `s_round(1)` (the LM-10 union cure)** for the {weighted}×{with-replay} comparison cell — do
  NOT write a second replay loop.
- **The ONLY new surfaces (FLAGGED — they do NOT exist today):** `gl_accumulate_w`, `gl_merge_w`
  (gossip_learn), `r3_fisher_diag` + the `F[R_NP]` scratch + the `[wmerge-*]` cert branch
  (r3_incontext). For W3: a `gl_ties_elect` sign-trim pass (only if W3 is built). NO new R3, NO new
  transport (F-local, XII.3), NO fork of `gl_merge`.

**Honest bounds (what is NOT claimed):**
- **The diagonal Fisher is a DIAGONAL approximation** — it ignores parameter covariance (the
  off-diagonal Fisher / true natural gradient). At R3's toy scale with facts sharing a hidden
  subspace, the diagonal may be too coarse to separate them; the cert MEASURES whether it suffices,
  does not assume it.
- **Weighted merge may NOT beat the plain mean at this scale — and the slice ships that negative
  honestly.** R3 is TOY (R_DM=48, single binding/node, heavily-shared subspace); "the mean is hard to
  beat" is a legitimate measured outcome (XII.0 #5). The cert is built so the TIE/LOSE classification
  is a clean greppable PASS-of-the-measurement, never a bar lowered to manufacture a win.
- **The 3× SDICT prior contaminates the naive comparison (XII.0 #4)** — the `[wmerge-symmetric]`
  control quarantines it; the honest read is on the symmetric binding, not the prior-favored one.
- **F-local correctness depends on Path E co-running (XII.3)** — a node that got weights but never the
  engram cannot compute the peer's local Fisher; F-wire is the fallback at 2× wire. The in-process
  cert has all data local, so this is a LIVE-path bound, not a cert bound.
- **Toy scale, in-process headline, cert verbs are amnesia bombs** — the LIVE weighted merge over the
  relay is a NAMED FUTURE (XII.8); persisting the merged `rw[]` across runs stays a non-goal (VII.0 #5).
- **Confidence-dominated blend ≠ identity/trust revision (XII.5)** — provenance-weighted-by-IDENTITY
  couples to the unbuilt signing immune system + the ark's no-verification ethic; future slice.

No false "by construction" theorem is asserted. **The headline is a COMPARISON number, measured both
ways, with "the mean wins" a fully honest possible result.**

### XII.7 Galaxy hook — a weighted merge is the SAME pulse, annotated

Path W² does NOT add a new galaxy event type — a weighted fold IS a fold. It reuses LM-10's ONE
`galaxy_emit(EV_MERGE, me, peer, merge_epoch16, peers_folded)` (XI.7) at the live weighted-fold site,
**with a one-bit annotation** in the spare payload field distinguishing a weighted fold from a plain
one (so the galaxy view can later render "the region slept with Fisher-weighting" vs a plain pulse —
the collective-sleep heartbeat, now with a quality flag). **COMMANDER DECISION 4:** annotate the
existing `EV_MERGE` (RECOMMENDED — one bit, no new event, the auditor greps zero new `galaxy_emit`
call sites) vs a distinct `EV_WMERGE` type (rejected for v1 — a weighted fold is the same collective
sleep, just smarter; a new type forks the galaxy label surface for no behavioral difference). Since
the LIVE weighted fold is a NAMED FUTURE (XII.8), the galaxy hook ships WITH that follow-up, not the
in-process cert (which emits nothing).

### XII.8 Provenance / closes-on

Design only. This slice closes when `[wmerge-vs-mean]` (both matrices + the per-cell Δ printed and
classified WIN/TIE/LOSE), `[wmerge-noreplay]` (the raw weighted merge measured against the 75% bar —
clears it OR the honest partial), `[wmerge-divergence]` (the ceiling curve for both rules printed,
including the `[wmerge-symmetric]` prior-quarantined control), `[wmerge-nocentral]` (order-independence
at `n=R_NP` through `gl_merge_w`), and `[wmerge-noregress]` (LM-10 `[onemind-*]` + LM-7 `[shared-*]` +
`[g22-*]` + `[lang-*]` byte-identically green) are green on a clean rebuild AND CI-enforced; the
zero-extra-wire verdict (F-local) is PRINTED. Audited by a **separate** agent on the **commander's**
binary, not the implementer's. **The audit MAKES the acceptance test (the `[wmerge-noreplay]` bar and
the WIN/TIE/LOSE Δ-classification discovered from the comparison curve, line-by-line); the commander
reads the gate formula — the two matrices, the per-cell Δ, the no-central `|fwd−rev|` at `n=R_NP`, the
Fisher accumulation loop (is it `rg[i]²` over the RIGHT engrams?), the `gl_merge`-unchanged grep —
line-by-line** (the validator-trap memory: certify on production code, not a sim; the implementer ≠
auditor ≠ commander). One epitaph line in `gap-ledger.md`: LM-10's *"weighted/Fisher merge for higher
divergence"* follow-up is **discharged — the smarter-merge question is MEASURED: a diagonal-Fisher
weighted merge {beats / ties / loses to} the plain mean on the divergent-minds matrix by the printed
Δ, {removes / reduces / does-not-reduce} the replay crutch, computed with {zero / 2×} extra wire — the
number decides, and "the mean is hard to beat at this scale" is an honest result if that is what the
matrix says.**

**COMMANDER DECISIONS NEEDED (recommended defaults):**
1. **Which weighted rule is v1** (XII.2): **(W2) diagonal Fisher (RECOMMENDED** — the principled
   EWC-style answer, directly targets the disease, cheap via `r_backward`) vs **(W1)** scalar
   count/confidence (the cheap control — likely ≈ plain mean at this scale, kept to ISOLATE whether
   per-parameter resolution is what helps) vs **(W3)** TIES/task-arithmetic (the named escalation for
   the higher-divergence / more-facts half — most new code, defer to the `[wmerge-divergence]` sweep).
   The cert measures every rule that is built; **W2 primary, W1 as the in-cert control, W3 if W2
   under-delivers on the divergence sweep.**
2. **Fisher transport** (XII.3): **(F-local) compute every contributor's Fisher LOCALLY from
   Path-E-spread engrams, ZERO extra wire (RECOMMENDED** — E carries the data, W carries the weights,
   the Fisher fuses them locally) vs **(F-wire)** each node publishes its 84 KB Fisher diagonal (2×
   Path W's wire — the fallback when Path E is NOT co-running). The in-process cert has all data local,
   so this is a LIVE-path decision the cert does not gate on.
3. **Replace vs complement the LM-10 replay cure** (XII.0 #5, XII.4 #2): **COMPLEMENT — the weighted
   merge is measured as a way to REDUCE the replay dependency, and union replay (LM-10) stays the
   fallback cure (RECOMMENDED** — if weighted-no-replay clears the bar, the crutch is removed; if it
   only reduces the gap, replay remains and the slice reports the honest partial) vs claiming weighted
   merge REPLACES replay (rejected — only the MEASURED `[wmerge-noreplay]` result can earn that, and
   only if it clears the bar; the design refuses to pre-assert it).
4. **Galaxy** (XII.7): **annotate the existing `EV_MERGE` with a one-bit weighted flag (RECOMMENDED**
   — a weighted fold is the same collective-sleep pulse, no new event) vs a distinct `EV_WMERGE`
   (rejected — forks the galaxy label surface for no behavioral difference). Ships with the LIVE
   follow-up (XII.8), not the in-process cert.
5. **In-process headline vs live now** (XII.4): **ship the in-process `[wmerge-*]` comparison as THIS
   slice's headline; LIVE weighted merge over the relay (the `42_one_mind` extension) is the NAMED
   FUTURE (RECOMMENDED** — the comparison is fully measurable in-process with all data local; the live
   path inherits `[onemind-survive]` and ships once the comparison says weighted is worth the wire) vs
   forcing the live path now (rejected — the live transport adds no NEW measurement over the in-process
   matrix; it is delivery, not science, and gates on the in-process WIN/TIE result first).
