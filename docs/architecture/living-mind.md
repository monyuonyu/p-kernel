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

### Provenance / closes-on

This is design only. The first slice closes when `[dmn-forgetting]`,
`[dmn-consolidated]`, `[dmn-distributed]`, `[dmn-survive]`, `[dmn-gradcheck]` are all
green on a clean rebuild AND CI-enforced (all targets), audited by a separate agent
on the commander's binary — not the implementer's. Per `gap-ledger.md` discipline:
when it ships and is CI-enforced, it earns ONE epitaph line; it does not lengthen the
ledger. No `philosophy-gap-audit-9`. The audit makes the acceptance test; the
commander reads the gate formula line-by-line.
