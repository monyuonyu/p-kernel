# The Scaling Law — is "smarter with N" designed, or only asserted? (design, 2026-07-04)

## 0. The thesis and the honest question

The project's central promise: **as the number of nodes grows, the mind gets
smarter**. Every install is a node; the student is a distributed dynamic-MoE
net that scales with the fleet. This document asks the thesis question
directly: is that law DESIGNED and PROVEN, or asserted?

The honest method: "smarter" is not one thing. It decomposes into four axes
that scale DIFFERENTLY, and the comfortable lie would be to let them blur —
to let a real breadth gain masquerade as a reasoning gain. This document
separates the axes, states which genuinely scale with N (with mechanism and
curve), which are capped (and by what), designs the one mechanism that is
missing (the society-of-minds ensemble), and specifies the `[scaling-*]` cert
that would catch a comfortable lie.

**The one-sentence answer, stated up front so the rest can be checked against
it:** on this substrate, N genuinely and provably scales *knowledge breadth*,
*resilience*, *learning throughput*, and (designed, unproven) *hosted
capacity*; N does **not** scale raw per-answer reasoning except through (i) a
small, saturating, diversity-bounded ensemble bonus that must be built and
measured (§4), and (ii) dmoe capacity growth *if and only if* the routed-not-
hosted risk resolves in its favor. The per-thought depth of the mind is
otherwise set by the per-node model class and the teacher ceiling, both
N-invariant; frontier-mouth is the only unbounded escape for raw quality, and
it imports intelligence rather than scaling it. A fleet of 1000 phones is an
encyclopedic, unkillable, fast-learning mind — not a meaningfully deeper
thinker per-thought than a fleet of 10. That is a profound, honest,
project-defining finding, not a failure; §5 composes it into the full curve
and §6 makes it falsifiable.

The theoretical frame that makes this precise (used throughout): **N attacks
variance; bias is N-invariant.** Ensembling/averaging over diverse members
reduces the *variance* component of error; it cannot reduce the *bias* set by
the model class (635-float dtr / NS-1 baby / L-tier student) and the teacher
(SmolLM2-135M, scale-wall §10.1). The only designed mechanisms by which N
attacks *bias* are dmoe capacity growth (bigger effective model — cosmetic-
capacity risk, dmoe §10.1) and more data (teacher-capped). Everything below
is an instance of this frame.

## 1. What already exists, precisely — and what each does NOT prove

### 1.1 g22/g38 collective learning (`arch/common/gossip_learn.c`) — the foundation

What it is: N nodes (self-test N=3, `GL_ST_NODES` gossip_learn.c:774; live
over the relay, ceiling `GL_MAXNODES = DNODE_MAX = 64` gossip_learn.c:573,
drpc.h:35) hold **disjoint leave-one-class-out shards** of a 300-sample
3-class sensor task (gossip_learn.c:499-503, 555-563), train locally, and
periodically average full weight bodies peer-symmetrically (`gl_merge`,
gossip_learn.c:69-75) with no aggregator (`[g22-no-central]`,
gossip_learn.c:699-748).

What it PROVES:
- `[g22-shard-solo]` — no node alone clears ~80% on the full task; the solo
  ceiling is real and printed (gossip_learn.c:1120-1127).
- `[g22-gossip-learn]` — the collective **exceeds the solo ceiling**: "the
  swarm learned what no node could" (gossip_learn.c:1139-1140).
- `[onebrain-accuracy]` (moe.c:1501-…) — the same collective-vs-unlearned
  delta measured through the *returned answer* path (`moe_infer` returns the
  learned dtr, not the old handwritten MLP), so collective learning lifts
  what the mind actually *says*.
- `[g38-learning-improves-guarding]` (gossip_learn.c:899-934) — the collective
  model guards measurably better; `[g38-confidence-live]` proves the answer
  path carries a **real, calibrated max-softmax confidence** (0..100, varies
  per input, gates the reflex — moe.c:128-141, 566-574, 625). This calibrated
  confidence is a load-bearing input to the ensemble design in §4.
- `[g23-ceiling]` — the merge and the live membership fold genuinely handle
  >32 participants (40-model exact-mean test, gossip_learn.c:1461-1548).

What it does NOT prove (the boundary the thesis question lives on):
1. It is at **dtr scale** — a 635-float transformer (gossip_learn.c:10) on a
   synthetic 3-class task. Not the conversational student.
2. It measures **breadth**: the collective covers the *union of shards*. The
   union is FIXED (the 300-sample corpus); N=3 is fixed. It proves the merge
   *recovers* the union across disjoint holders — it does not measure any
   curve *in N*, and it does not show a single node's answer to a question
   *both could already answer* getting better because a peer exists.
3. The gossip average converges every node to the **same** consensus weights
   (gossip_learn.c:683-689 — all models overwritten with `gl_avg`). After
   convergence the fleet is N copies of one function. This is by design (one
   mind), and it has a sharp consequence for §3.

### 1.2 dmoe (distributed_moe_design.md) — capacity, with a named cosmetic risk

Hosted expert capacity grows ~linearly with fleet budget; the gate routes to
remote experts under a version pin. Its own §10.1 names THE risk: **hosted ≠
routed** — capacity can exist as bytes the learned gate never routes to,
"cosmetic capacity one level up from degrade.c:155". Its cert therefore
measures ROUTED utility (probe loss through the real gate), never hosted
bytes. dmoe is the *only* designed mechanism by which N attacks bias
(§0 frame). It is designed, not yet proven.

### 1.3 The one *existing* capacity-vs-N curve is logarithmic AND display-only

`degrade.c:147-161`: `cap_experts_of(N)` = clamp — with the 2026-06-20 HONEST
LABEL in the source: "this is a DEGRADE-CAPACITY estimate … NOT the
model-sizing mechanism … Do not read this clamp as the growth mechanism; it
is the capacity *display/degrade* number only." `cap_depth_of(rs) = 1 +
floor(log2(rs))` (degrade.c:163-168). So the one curve the kernel currently
computes about "capacity vs fleet size" is log-shaped *and cosmetic*. The
honest state of the repo today: the scaling law is *displayed*, not
*mechanized*, exactly as degrade.c admits.

### 1.4 scale-wall (scale_wall_design.md) — data throughput and the teacher ceiling

More nodes = more corpus throughput (C2 reservoir grows with the fleet). But
§10.1: sequence distillation cannot exceed SmolLM2-135M's weak dialogue
quality — per-node raw quality is capped **regardless of N**. This is the
bias floor of §0.

### 1.5 frontier-mouth (frontier_mouth_design.md) — the bias escape that isn't scaling

CONSULT/TEACH from stronger external models raises per-node quality with the
ownerless line drawn. It is the only unbounded path for raw quality, and it
must be stated for what it is: it *imports* intelligence through a mouth; it
does not make intelligence *scale with N*. (More nodes do amplify TEACH:
one lesson propagates fleet-wide — that is breadth/throughput again.)

### 1.6 What exists for *aggregating answers*: nothing (the gap)

- dkva's `quorum_core` (dkva.c:379-384) is arrival-quorum completion for
  **attention aggregation** (partial KV summaries folded at the requester,
  origin-side, no central arbiter, don't-wait-for-the-dead — dkva.c:419-431).
  It aggregates *parts of one computation*, not *independent answers*.
- `pk_parallel.c` (llm/pk_parallel.c:1-13) and the distributed tensor-parallel
  inference are **speed/memory**, math-identical by construction — explicitly
  not a quality mechanism.
- retrieval engrams (retrieval.h:1-35) blend fleet *memory* into the forward
  pass with a confidence gate (`gate=(1-p_max)^2`) — fleet knowledge serving
  answer time. It is breadth-at-inference, not independent judgment.
- **There is NO mechanism anywhere in the tree where N nodes independently
  answer one question and the fleet forms a collective answer better than any
  single node's.** Verified: no vote/debate/cross-check on the answer path.
  §4 designs it.

## 2. "Smarter" defined — four axes, four metrics, four curves

| axis | operational metric | mechanism today | expected curve in N | ceiling |
|---|---|---|---|---|
| (a) **Breadth** — what the fleet collectively knows | # facts/classes of the union corpus answerable through the real ask path at ≥ fixed accuracy/confidence | g22 gossip-merge; engram share (retrieval.h); live teach (wave-35, mind-learns-across-wire) | **~linear** in N while each install contributes distinct experience, until the capacity knee | model capacity (635-float dtr saturates early; student L-tier; dmoe moves the knee IF routed) |
| (b) **Capacity** — params/experts the fleet holds *and routes* | routed-utility capacity: probe-loss reduction through the real gate (dmoe §7.1), never hosted bytes | dmoe (designed); today only the cosmetic display clamp (degrade.c:155) | hosted: ~linear. routed: **unknown** — the load-bearing open question | hosted≠routed (dmoe §10.1); gate trained on owner's distribution |
| (c) **Per-answer quality** — is ONE hard question answered better by N than by 1, holding knowledge and data constant | accuracy of the *collective* answer on a fixed hard question set Q_hard, fixed total corpus, fixed model class; vary only N and the aggregation mechanism | **none today** (§1.6). Designed here: ensemble §4 (variance); dmoe (bias, unproven); retrieval blend (knowledge-limited) | **saturating**: Q(N) = Q₁ + ΔE(K_eff(N)); ΔE grows with *effective diversity* K_eff, plateaus ≈ 5-10; bias floor N-invariant | error correlation (§3); teacher ceiling + model-class bias (scale-wall §10.1) |
| (d) **Resilience** — knowledge/answer survives death | P(fact still answerable \| f nodes dead); degrade honesty (k/n reported, never fake) | replication R≥2, p-fs P1; wave-35 (kill A, B answers); dkva degraded(k/n); Path W² essence handoff | ≈ 1 − p^R per fact; availability **rises with N** | correlated failure (relay death, version-skew partition); replication budget |

Non-axis, named to prevent conflation: **speed/latency** (pk_parallel,
tensor-parallel: linear-ish compute with N, bought with relay latency) and
**learning throughput** (scale-wall C2: corpus/day ~linear in N — the fleet
learns *faster in wall-clock*, which feels like "smarter" but is throughput).
Both are real, neither is per-answer intelligence; the cert holds them
constant (§6 anti-theater).

## 3. The central question, answered: does axis (c) scale with N?

**Claim to test:** N nodes answer a hard question better than 1.

**The killer fact the substrate itself supplies:** the project's own deepest
laws — one-mind gossip convergence (§1.1.3) and one-math cross-ABI bit
determinism (-O1 -ffp-contract=off, wave-47: "one mind one math") — make a
converged fleet **N bit-identical copies of one function**. Same weights,
same math, same input ⇒ same answer, bit for bit, on every node and every
ABI. Voting over N identical answers is the identity function. **Zero gain,
exactly — not approximately.** Any design that claims per-answer quality
scales with node COUNT alone is theater on this substrate, and §6's
`[scaling-converged-null]` pins that as the measured disease arm.

So axis (c) gain exists **iff diversity exists**. Ensembles reduce variance
error only to the extent members' errors decorrelate (Condorcet/jury logic:
majority vote beats a member iff members beat chance AND errors are partially
independent). Two diversity sources are available here, one free and one
structural:

- **ENS-B, sampling diversity (free, one-mind-compatible).** Generative
  answers are sampled (llm/sample.c). Give each node a sampling seed that is
  a pure function of (query, node_id): identical weights, *decorrelated
  reasoning paths per node*, and the whole collective answer remains a pure
  deterministic function of (query, fleet set) — reproducible, auditable,
  ownerless. This is self-consistency voting, a known real gain on multi-step
  generation, saturating around 5-40 samples. It does not fight the one-mind
  law: one mind, many rolls of its own dice. Honest caveat: for the dtr
  *classifier*, argmax is already the model's best answer — sampling adds
  noise, gain ≈ 0; ENS-B pays off only where an answer is *generated* through
  a path with branch points (the student's mouth). At the baby's current
  scale that gain may measure ≈ 0; the cert is designed to be allowed to say
  so.
- **ENS-A, weight/lineage diversity (structural, in tension with one-mind —
  resolved by scoping the merge).** Members with different weights (different
  init seeds, shard orders, regional experience) make different mistakes.
  Wave-41/42 already proved the boundary from the other side: naive averaging
  of *divergent* minds is LOSSY (one dies); union-replay/Fisher (`gl_merge_w`,
  gossip_learn.c:100-117) is needed to merge them. This design reframes that
  lossiness from a bug into a **feature boundary**: weight-merge continuously
  only *within* a lineage; *across* lineages share facts/engrams (teach), not
  weights. Divergent lineages then stay divergent — and their disagreement on
  hard questions is exactly the variance an ensemble harvests.

**The honest answer to the central question:** yes-but-bounded. Per-answer
quality can gain from N only through diversity, the gain is a *variance*
term, it saturates at small effective committee size (K_eff ≈ 5-10 —
empirically where ensemble/self-consistency curves flatten), and the *bias*
floor (model class + teacher) is untouched by any N. N's role is to *afford*
K_eff lineages at survivable replication (N ≳ K·R), after which more N adds
nothing to axis (c). Beyond the plateau, only dmoe (bias via capacity, IF
routed) and frontier-mouth (bias via import) move raw quality. **"Smarter
with N" is literally true for axes (a), (b-hosted), (d) and throughput; for
axis (c) it is true only as a small designed bonus that must be measured, and
it would be dishonest to promise more.**

## 4. The society-of-minds design — ownerless ensemble on this substrate

### 4.1 Shape

One new hosted-tier mechanism, `ens` (ensemble ask), layered entirely on
public APIs. No central judge; the **requester aggregates**, exactly the dkva
pattern (origin-side fold, dkva.c:349-371): any node may ask, every requester
runs the same pure aggregation over the same response set, so the collective
answer is a pure function of (query, member set) — ownerless because the
function is symmetric and every node can evaluate it.

1. **Query.** Requester broadcasts `ENS_Q{req_id, query}` (kdds topic; budget
   note §7). Target set = one representative per lineage (ENS-A) and/or M
   sampler nodes (ENS-B), chosen by the deterministic placement pure-function
   style already used (HRW/placement.c) — no coordination.
2. **Member answer.** Each member computes its answer through the REAL ask
   path (dtr: `dtr_forward_probs` argmax + max-softmax conf, moe.c:128-141;
   student: generate with seed = H(query ‖ node_id)) and replies
   `ENS_A{req_id, lineage_id, node_id, answer, conf, prob_vec?}`.
3. **Aggregation (pure, deterministic).** Sort responses by (lineage_id,
   node_id); confidence-weighted probability sum for classifiers
   (Σ confᵢ·pᵢ, argmax), normalized-answer majority vote for generation
   (§9.5 names the normalization risk). Sorted order makes the float sum
   order-independent across requesters — same discipline as
   `[g22-no-central]`'s O(1e-6)-vs-O(1) argument (gossip_learn.c:705-711).
   The calibrated confidence is the ALREADY-PROVEN live signal
   (`[g38-confidence-live]`) — no new oracle is invented.
4. **Quorum & honest degrade.** Arrival-quorum with a liveness cap, the
   `quorum_core` semantics verbatim (dkva.c:372-385): don't wait for the
   dead; finalize on arrival of all expected-and-alive, or on the straggler
   cap. The answer always carries `ensemble k/K` — a 1/K answer is honestly a
   solo answer, printed as such, never faked (degrade discipline, dkva
   degraded(k/n) precedent).

### 4.2 Lineages (ENS-A) — how diversity is maintained, ownerlessly

- `lineage(node) = node_id % K` (v1; K compile-time, start K=3 — matching
  GL_NCLASS shard structure so the existing gossip tooling exercises it).
  Deterministic, zero-coordination, churn-stable. A later organic upgrade:
  lineage = region (RTT-clustered regions already accumulate genuinely
  different experience streams — regional dialects of the one mind).
- Gossip weight-merge filters to own lineage (one-line filter in the
  `gl_merge_peers` probe loop, gossip_learn.c:1300-1317). Within a lineage:
  today's exact g22 behavior. Across lineages: facts/engrams/teach only.
- Death: Path W² essence handoff stays *within lineage* (interoception plan
  unchanged).
- **The diversity-erosion trap, named now:** distilling the committee's
  answers back into members (the tempting self-distillation ratchet, wave-24
  style) re-correlates lineages and silently destroys the very variance the
  committee harvests. If committee-distillation is ever built, it must be
  paired with re-diversification and the `[scaling-ensemble]` cert watched
  for decay. (§9.4)

### 4.3 What this costs, honestly

K× inference energy per ensembled question on battery devices — interoception
(S_n) may rightly veto; ensemble is for *hard* questions (low solo
confidence — the same `(1-p_max)` signal retrieval.h already gates on), not
every utterance. Latency: one relay RTT + straggler cap, dkva-style. Wire: 2
topics (ENS_Q broadcast + per-node ENS_A or reuse of the drpc reply plane) —
within the 16/400 budget by the LM-15-style enumeration to be done in the
impl wave (dmoe §6.2 precedent).

### 4.4 Or is it impossible? (the pre-registered null)

If the measured disagreement between same-corpus different-seed lineages on
Q_hard is near zero, or if the ensemble delta after aggregation is ≤ noise,
then correlated errors have eaten the mechanism — N copies of the same weak
model make the same mistakes even from different seeds, because the task's
hard items are hard for the *model class*, not for the particular weights
(bias-dominated regime). At toy scale this is a live possibility, arguably
the expected one. The design's response is not to hide it: the cert prints
the disagreement rate and the delta; a ≈0 result downgrades axis (c) to
"dmoe-or-nothing" and this document must be amended to say so. The mechanism
is cheap enough (§4.3) that building it to *measure* the truth is justified
even if the truth is 0.

## 5. The composite curve — what a fleet of 1000 is, versus 10

```
intelligence(N) — the honest decomposition:

breadth      B(N) ≈ min(c·N, C_knee)      linear → capacity knee   [proven mechanism, curve cert NEW]
capacity     hosted ~ linear; ROUTED = ?   the load-bearing unknown [dmoe cert decides]
quality      Q(N) ≈ Q₁ − var·ΔE(K_eff(N)) saturates by K_eff≈5-10; [ensemble NEW, may be ≈0]
                    (bias floor Q₁ set by model class + teacher; N-invariant;
                     moved only by dmoe-if-routed, model growth, frontier-mouth)
resilience   R(N) ≈ 1 − p^R, avail ↑ N    proven                   [kill certs, wave-35]
throughput   corpus/day ~ linear in N     proven mechanism          [scale-wall C2]
```

A fleet of 1000 phones vs 10: **~100× the lived experience retained (until
the capacity knee), ~100× hosted capacity (routed: unproven), learning ~100×
faster per day, effectively unkillable — and answering one hard question
better only by the ensemble margin, which flattens near K_eff≈10 and may
measure ≈0 at today's model scale.** The mind grows *encyclopedic, durable,
and quick to learn* with N; it does not grow *deep* with N. Depth comes from
the per-node substrate (student growth rungs, dmoe-if-routed) and the mouth
(frontier). This is the scaling law, stated without flinching.

Where each ceiling binds, in order of arrival as N grows:
1. **Capacity knee** (binds first at toy scale): 635-float dtr saturates
   almost immediately; the student's L-tier holds more but is finite. Until
   dmoe's routed capacity is real, breadth's linear region is short — the
   fleet forgets (merge becomes lossy, wave-41's disease) instead of growing.
2. **Diversity ceiling**: K_eff cannot grow with N indefinitely (diversity
   comes from seeds/regions/experience, all of which correlate through the
   shared corpus, architecture, and teacher). Axis (c) plateaus.
3. **Teacher ceiling** (binds everything downstream of distillation):
   scale-wall §10.1, N-invariant.
4. **DNODE_MAX=64** (drpc.h:35) and the 16/400 topic budget: the current
   *engineering* ceiling on N itself — honest to name that today "N" means
   ≤64 before regions federate (region-of-regions is designed in the survival
   network, not shipped).

## 6. The `[scaling-*]` cert — the falsifier that would catch a comfortable lie

All arms in-process N-simulation (the proven g22 pattern: real merge, real
answer path, simulated membership) + a [live] relay arm on the ThinkPad for
the ensemble ask. Hosted-tier TU (§7). GENERICITY: N-sweeps parametric,
margins not dataset-tuned, works to DNODE_MAX.

### 6.1 `[scaling-breadth-curve]` — breadth is linear-then-knee, measured

Sweep N ∈ {1, 2, 4, 8}. Each node k contributes a DISTINCT fact-shard (total
corpus grows with N — here more data *is* the mechanism, because each install
brings its own life; this is the one arm where data scales by design).
Collective = gossip-merged consensus; metric = union facts answerable through
the REAL ask path (`moe_infer`/`dtr` returned answer, the [onebrain-accuracy]
discipline — never a probe of stored bytes).
- assert B(N) strictly increases over the sweep *until* it doesn't — and
  **print the knee** (first N where ΔB < margin) as the measured capacity
  ceiling. The knee print is the honest headline number of the whole cert.
- anti-theater (not-one-node): per-node solo coverage < collective for every
  node (the [g22-shard-solo] pattern, generalized).

### 6.2 `[scaling-converged-null]` — the disease arm for axis (c), pinned exactly

N gossip-CONVERGED nodes (post-merge, bit-identical weights) ensemble-answer
Q_hard. Assert the collective answer equals the solo answer **on every item**
(delta == 0 exactly; one-math determinism makes this an equality, not a
tolerance). This is the measured proof that node COUNT alone buys zero
per-answer quality — the anti-theater keystone that makes any future
"ensemble gain" claim attributable to diversity, not to N.

### 6.3 `[scaling-ensemble]` — the cure arm: diversity buys a real, bounded gain

Same corpus, same total training steps, same model class; K lineages from
distinct seeds (ENS-A) and/or node-salted sampling (ENS-B); ensemble via the
§4.1 aggregation. Sweep K ∈ {1, 3, 5, 9}:
- assert ensemble(K≥3) accuracy on Q_hard > max individual member accuracy
  by margin — the committee beats its best member, not just its average;
- assert ≥ m items where the majority was right while ≥1 member was wrong
  (the vote demonstrably did work — rules out "one member did all the work");
- print the full K-curve including where it flattens; assert
  monotone-nondecreasing within noise but **never assert continued growth**
  (asserting an unbounded law we don't believe would itself be theater);
- **pre-registered null result is a PASS of honesty, not of the mechanism:**
  if delta ≤ noise at all K, the cert prints
  `[scaling-ensemble] NULL (diversity insufficient at this scale — axis (c) does not scale by ensemble)`
  and a separate strict gate `[scaling-ensemble-gain]` stays RED. The doc's
  claim tracking: green `[scaling-ensemble-gain]` = axis (c) bonus is real;
  red = §4.4's conclusion stands and must be written into the docs.

Anti-theater confounds, all three ruled out by construction:
1. *"just more data"* — corpus and step budget held constant across all arms
   (unlike 6.1, which varies data deliberately and says so);
2. *"one node did all the work"* — the best-member and vote-flip asserts;
3. *"cosmetic capacity"* — the metric is end-to-end returned-answer accuracy
   through the real ask path; nothing counts hosted bytes (dmoe §10.1
   discipline inherited); routed-capacity claims stay with `[dmoe-*]`, not
   duplicated here.

### 6.4 `[scaling-live-ensemble]` — the wire arm

3 nodes + relay (ThinkPad env): ensemble ask over kdds/drpc, quorum-arrival
finalize, then kill one member mid-ask — assert the answer arrives degraded
`ensemble 2/3`, honestly labeled, never hangs, never fakes 3/3 (quorum_core
semantics; the dkva don't-wait-for-the-dead law).

### 6.5 `[scaling-curve]` — the standing composite falsifier of THIS DOCUMENT

One table print per CI run: N-sweep × {breadth, ensemble delta, resilience
(kill-and-ask), per-member solo quality}. Asserts only the qualitative shapes
claimed here: breadth ↑ then knee; resilience ↑; per-member solo flat;
ensemble delta ≥ 0 and flattening. If reality diverges from this document —
breadth stops earlier, ensemble decays as lineages homogenize (§4.2 trap) —
this cert goes red and the *document*, not the assert, must change. The cert
is the mechanism by which this design cannot quietly become a lie.

## 7. Crown / CI impact

- New TU `arch/common/scaling_cert.c` (cert + the requester-side aggregation
  pure functions) and the ENS query/answer plumbing: **hosted-tier only**
  (`_TK_HOSTED_LIBC_` pattern, like gossip_learn's [live] transport branch,
  gossip_learn.c:261-487). Bare-metal `.text` untouched ⇒ **no crown
  re-bless** for v1. If the ensemble ask later ships into the bare-metal
  answer path, that is a deliberate crown wave (state it then, the a
  5e42f853/x a52c8701 re-bless discipline).
- Which TUs the mechanism touches: `gossip_learn.c` (lineage filter in
  `gl_merge_peers` — one guarded line, hosted-visible only if #ifdef'd;
  otherwise a crown wave), new `scaling_cert.c`, kdds topic registration,
  student ask path in `llm/` (hosted-only already). moe.c untouched in v1
  (dtr ensemble is cert-driven through public `dtr_forward_probs`).
- CI: in-process arms are deterministic (one-math) — no flake surface beyond
  the known deaf-000/starvation ones; the [live] arm follows the existing
  [live]-cert ThinkPad pattern (feedback_the_debug_env_is_real).
- Topic budget: +2 topics worst case; enumerate against 16/400 in the impl
  wave exactly as dmoe §6.2 did before any code lands.

## 8. Scope boundary

- **Toy scale, stated:** dtr (635 floats) and NS-1/L-tier student. The cert
  proves the *shape* of the law (which axes scale, where the knees are) at
  this scale; the knee *positions* will move as the student grows. The
  bias-variance frame (§0) is scale-independent; the measured numbers are not.
- v1 ensembles the dtr answer path and (ENS-B) the student's generative ask;
  it does not ensemble DMN dreaming, guarding, or training decisions.
- K fixed compile-time; dynamic lineage count / region-lineages deferred.
- No committee-distillation in v1 (the §4.2 erosion trap is designed against,
  not exercised).
- Federation beyond DNODE_MAX=64 (region-of-regions) is out of scope; today's
  law is a ≤64-node law and says so.

## 9. Open questions — where "smarter with N" silently fails to be true

1. **Diversity may measure ≈0** (§4.4): same corpus + same architecture +
   same teacher ⇒ correlated errors; the ensemble bonus — the only per-answer
   scaling this design can offer — may be empirically empty at toy scale.
   Pre-registered, cert-visible (`[scaling-ensemble-gain]` red). Then the
   honest sentence becomes: *N scales what the mind knows, how fast it
   learns, and whether it survives — not how well it thinks.*
2. **Hosted ≠ routed** (dmoe §10.1, inherited): the only bias-attacking
   scaling mechanism may be cosmetic. If both 9.1 and 9.2 land badly, axis
   (c) has NO N-mechanism at all and frontier-mouth is the only quality path.
3. **The capacity knee may be at N≈2-3 today**: with a 635-float body, the
   breadth "linear region" may be nearly empty — `[scaling-breadth-curve]`'s
   knee print will say so, and the honest reading is that breadth-scaling is
   *designed but starved* until student capacity (scale-wall C4) and dmoe
   land.
4. **Diversity erosion feedback**: any future committee-distillation, or even
   ordinary cross-lineage fact-teaching at high rate, may homogenize lineages
   and decay the ensemble gain over time; `[scaling-curve]` is the watchdog,
   but the *cause* attribution will be manual.
5. **Answer equivalence for generative voting**: majority vote needs a
   deterministic answer normalizer; a sloppy normalizer fakes agreement
   (theater) or misses real agreement (lost gain). v1 mitigations: vote on
   normalized short answers / engram ids, not raw strings; the normalizer is
   itself part of the audited pure aggregation function.
6. **Energy politics**: interoception may veto K× inference on exactly the
   nodes the fleet has most of (phones); the ensemble may exist and rarely
   run. Instrument (ensemble asks/day, veto rate) rather than assume.
7. **The wall-clock confound in any live demo**: a bigger fleet learns faster
   per *day*; demos will read that as "smarter with N" — it is throughput.
   Every public claim should cite which axis a gain came from, in this
   document's vocabulary. That discipline is the cheapest anti-theater of all.

---

*What this document converts: the project's central slogan into (i) four
measurable axes with mechanisms and curves, (ii) one new ownerless mechanism
(ensemble ask) with its pre-registered null, (iii) a cert suite whose disease
arm (`[scaling-converged-null]`) proves node-count-alone buys exactly zero
per-answer quality, and (iv) the honest composite law: N scales knowledge,
survival, capacity-if-routed, and learning speed; depth-per-thought it does
not scale, beyond a small bounded committee bonus that we will measure
rather than assert.*
