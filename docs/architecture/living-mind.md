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
