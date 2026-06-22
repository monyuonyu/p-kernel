# SS-4 — function-preserving expert growth (HARDENED DESIGN)

**Status: DESIGN ONLY (no kernel code touched). 2026-06-22. Base commit `97efccfc`.**
Sequel to `special-structure-mind.md` §3.1 + §8 step 5, and `native-student.md` §A.4.
This document HARDENS the one-paragraph SS-4 sketch into an implementable spec with an
adversarial self-audit. It deliberately **corrects** the §A.4 "clone + half the router
score" recipe, which is *wrong for this codebase's router* (see LENS A).

> One-line worldview anchor: "脳がフリートで育つ" — `cap_experts_of(N)` (`degrade.c:156`)
> today sizes *nothing* (`degrade.c:148-159` says so in its own honest label). SS-4 is the
> wave that lets that number SIZE THE STUDENT'S ROUTER. This is the Evolution layer's
> first growth axis.

---

## 0. What is actually being grown (ground truth, cited)

There are **two distinct "MoE" objects** in this tree. Conflating them is the first trap.

| object | where | "expert" means | grows? |
|---|---|---|---|
| **node-router** | `moe.c` `select_expert()` (`moe.c:350`) | a whole NODE (`expert == drpc_my_node`, `moe.c:533`) chosen per weather `gate_class` by HRW/utility EWMA | already N-driven; NOT a weight tensor; **not SS-4** |
| **student MoE** | `student.c` `router_pick()` (`student.c:478`) + the SwiGLU experts (`student.c:646-695`) | a real weight-matrix expert: router row `[D]`, `W1/W3 [DFF×D]`, `W2 [D×DFF]`, indexed by `m->o_w1 + (l*E+e)*DFF*D` | **YES — this is SS-4** |

SS-4 grows the **student MoE expert count** `m->nexpert` (`student.c:248`), the one with a
real differentiable function. The node-router in `moe.c` already scales with N and needs
no function-preserving transform (selecting a new node is a discrete event, not a softmax
perturbation of a fixed function), so it is **out of scope** here except as the *trigger*
(its alive-N drives `cap_experts_of`).

### 0.1 The student forward, exactly (the math we must preserve)

Per token, per layer `l`, with `E = m->nexpert` resident experts (`student.c:609-695`):

1. gate logits: `gt[e] = Σ_i router_row[l,e][i] · fin[i]` for `e ∈ [0,E)`  (`student.c:619-624`)
2. `router_pick(gt, te, tw, E)` (`student.c:478`):
   - full descending sort `order[]` of the E experts by `gt` (`student.c:485-497`)
   - **adaptive width**: `nk = K`(=`ST_TOPK`=2); `while (nk<E && (gt[order[0]] - gt[order[nk]]) < ST_K_THETA) nk++;` (`student.c:499-501`, `ST_K_THETA=0.30f` `student.h:132`)
   - chosen ids `te[0..nk) = order[0..nk)`
   - **softmax over ONLY the nk chosen logits** (max-subtracted): `tw[j] = e^{gt[te[j]]-mx} / Σ` (`student.c:506-512`)
3. expert outputs `eo_all[j] = W2_{te[j]} · SwiGLU(W1,W3; fin)` (`student.c:668-694`)
4. **canonical weighted sum**: `moe[i] = Σ_{j<nk} tw[j]·eo_all[j][i]`, ascending j, NO reassociation (`student.c:701-706`); then `x[i] += moe[i]`.

**The function to preserve = the byte sequence of `logits` at the head** (the output of
`st_forward`), for a fixed input, at the instant `E` grows. The SS-3/SS-6 certs already
hash exactly this surface (`run_ss6.sh` M=`63e8de333e995913`, L=`67f2434f50e791b6`).

### 0.2 Why this router makes the textbook trick WRONG (the load-bearing observation)

Net2Net / `native-student.md §A.4` say: clone expert `s` into a new expert `s'`, then in a
**global softmax over all experts** give each twin half the score so the *mixture* is
identical. **This codebase does NOT have a global softmax.** It has **top-K-then-softmax
over only the chosen set**, with **margin-based widening**. Two consequences:

- If `s'` is a *clone* of `s`, it has the SAME gate logit as `s`. After the descending sort
  `s` and `s'` are adjacent at the top; the margin test `(top1 - gt[order[nk]]) < THETA` is
  satisfied with gap 0 < 0.30, so **`nk` widens by (at least) 1** and `s'` IS admitted into
  the chosen set. The "half the score" idea is inert — softmax is over chosen logits, and
  two equal logits each get equal share; halving both leaves them equal.
- Once `s'` is in the chosen set with a nonzero `tw`, it steals softmax mass from the
  incumbents AND adds its own `eo`. Even the "duplicate output, halve weight" identity
  (`½·eo_s + ½·eo_{s'} = eo_s` when `eo_{s'}=eo_s`) holds **only if `nk` and the OTHER
  experts' weights are unchanged** — but widening changed `nk`, and renormalizing over a
  bigger chosen set changed every other `tw[j]`. So a naive clone is **NOT** function
  preserving here. This is the LENS A finding, surfaced up front.

The fix is to make the new expert **provably never enter the chosen set** at growth time
(§1), which makes preservation EXACT, not ε.

---

## 1. The growth transform (EXACT, not ε)

**Mechanism: ADD a `DEAD` expert (zeroed function + a sort-stable, never-admitted gate),
then later RESURRECT-by-cloning when the DMN decides to specialize it.** Two phases keep
the two hard requirements (never-selected AND zero-output) separate and each individually
provable.

### 1.1 Reshard the parameter buffer (size, layout)

Growth `E_old → E_new` (e.g. 4→8) changes `SZ_ROUTER`, `SZ_W1/W3/W2` (`student.c:136-139`,
all `∝ L·E`). The buffer is **heap** (`malloc` in `st_init_tier`, `student.c:289`), so
growth is a `realloc`-class reshard, NOT a stack/VLA change (the §3.2 trap is avoided
because every *scratch* array is bounded by the FIXED L-tier `*_MAX`, `student.c:103-118` —
those do NOT change). New API (proposed): `st_grow_experts(st_model *m, int E_new)`:

1. allocate a new arena sized for `E_new` (recompute offsets exactly as `st_init_tier`,
   `student.c:252-272`);
2. **copy each retained block verbatim** for the `E_old` incumbents at their new strides
   (embeddings, attention, norms, output head are E-independent → bitwise copy; the
   per-layer `[E,...]` blocks copy the first `E_old` slices);
3. **initialize the `E_new − E_old` new slots as DEAD** (§1.2);
4. free the old arena; update `m->nexpert=E_new`, `m->n_params`, re-zero `g/mu/vu` for the
   new slots only (Adam moments for incumbents are RETAINED — they are valid; the new
   slots start fresh, FedAvg-style).

Because the cache is reallocated lazily by sequence length (`cache_get`, `student.c` note
at `:319`), the next `st_forward` rebuilds it at `E_new` automatically.

### 1.2 DEAD-expert initialization — the EXACT identity

For each new expert `e' ∈ [E_old, E_new)` in every layer:

- **Router row = 0** (`router_row[l,e'][i] = 0 ∀i`). Then `gt[e'] = Σ 0·fin[i] = 0` for
  ALL inputs, exactly (no float error: a sum of exact-zero products is exact 0).
- **Down-projection `W2_{e'} = 0`** (zero the `[D×DFF]` block). Then `eo_all[j]=0` for that
  expert regardless of `W1/W3` (`eo[i] = Σ w2r[h]·eh[h] = Σ 0·eh[h] = 0`, exact).
- `W1_{e'}, W3_{e'}` = arbitrary (copy of source expert `s` chosen by load — they are
  *latent*, contribute nothing while `W2=0`; pre-loading them as a clone of the busiest
  incumbent gives a warm start for §2). RMSNorm gains/embeddings unaffected (E-independent).

**Why this is EXACT (both legs), not ε:**

- *Never-selected leg:* a gate logit of **exactly 0** does NOT by itself guarantee
  non-selection (if some incumbent also has logit near 0, the margin test could admit it).
  So zeroing the router row is **not sufficient on its own** — see LENS A. We need the
  stronger guarantee in §1.3.
- *Zero-output leg:* even IF `e'` were admitted, `W2_{e'}=0 ⇒ eo=0 ⇒ tw·eo = 0` for any
  `tw`. This leg is unconditionally exact. **But** admission still perturbs the *other*
  experts' `tw` via renormalization (softmax denominator grows by `e^{gt[e']-mx}`). So the
  zero-output leg alone is also insufficient. **Both legs together are still not enough**
  unless `e'` is kept OUT of the chosen set. Hence §1.3.

### 1.3 The exact never-admit guarantee (the crux)

The chosen set and `nk` must be **bit-identical** before and after growth. We guarantee it
by a **2-line, opt-in router guard** that masks DEAD experts out of selection:

- Maintain a per-expert `m->alive[e]` flag (new `int8` array, E-sized, in the model
  struct; incumbents=1, new DEAD experts=0). `st_grow_experts` sets it.
- In `router_pick`, when building `order[]` and counting toward `nk`, **skip experts with
  `alive[e]==0`** (treat them as `gt = -∞`, i.e. never `best`, never admitted by the
  widening loop). This is the EXACT analogue of the only-textbook-correct route: a DEAD
  expert has *effective logit −∞*, so it is provably absent from `order[0..nk)` for EVERY
  input, hence the softmax is over the IDENTICAL chosen set as before growth.

Result: with all three set (`router_row=0`, `W2=0`, `alive=0`), the chosen set, `nk`, every
`tw[j]`, every `eo`, and `moe[]` are **byte-identical** to the pre-growth forward, for ALL
inputs — not just the cert's one input. **Function preservation is EXACT.** (The
`router_row=0` + `W2=0` are then redundant-but-cheap defense-in-depth; the `alive` mask is
what makes it exact. We keep all three: if a future refactor drops the mask, `W2=0` still
bounds the damage.)

**Byte-identity preservation of the off-path:** the `alive` skip must compile to the
pre-SS-4 behavior when `E_old==E_new` and all alive — i.e. an all-ones `alive[]` reproduces
the current `router_pick` byte-for-byte. The cert `[grow-noop-identity]` asserts this
(growth by ZERO experts ⇒ output hash unchanged), the structural analogue of SS-6's
"hook NULL ⇒ byte-identical."

---

## 2. Specialization (DEAD → differentiated, no discontinuity)

A DEAD expert with `router_row=0, W2=0, alive=0` is a *fixed point of the forward* but NOT
of training — and we must turn it on without a step-change in output.

### 2.1 Resurrection event (atomic, still ε=0 at the instant)

When the DMN decides expert `e'` should join (e.g. consolidation budget pressure, or a
scheduled "grow then learn" tick), flip it ALIVE by **cloning the busiest incumbent `s`**:

- `router_row[l,e'] := router_row[l,s]` (so its gate logit becomes *equal* to `s`'s);
- `W1/W3/W2_{e'} := W1/W3/W2_{s}` (full clone);
- `alive[e']=1`;
- **AND apply the genuine Net2Net repair, correctly this time:** to keep the *output*
  identical at the resurrection instant despite `s` and `e'` now both being selected with
  equal logits, scale BOTH twins' **down-projection** by ½: `W2_s *= 0.5; W2_{e'} *= 0.5`.
  Because the chosen set now contains both `s` and `e'` with equal logits, softmax gives
  them equal weight `w`; their combined contribution is `w·(½eo_s) + w·(½eo_s) = w·eo_s`,
  exactly the single-expert contribution `s` had before — **but only if `nk`/other weights
  are unchanged**, which they are NOT (admitting `e'` widened `nk`). Therefore resurrection
  is **ε-preserving at best, NOT exact**, and we DO NOT claim it is. (This is the honest
  inverse of §1's verdict: *adding a DEAD expert is exact; turning it ON is the perturbation
  we deliberately accept, because that IS the learning event.*)

**Design decision:** keep the EXACT-preserving event (§1, add DEAD) and the
deliberately-perturbing event (§2.1, resurrect) **separate**. The `[expert-growth-preserves]`
cert (§4) gates ONLY the EXACT event. Resurrection is gated by a *different*, looser cert
`[grow-then-learn]` (loss does not spike at resurrection beyond a stated bound, and then
*decreases*). This avoids over-claiming exactness for a step that cannot be exact under a
margin-widening router.

### 2.2 Divergence via the existing DMN/train path

After resurrection the twins `s` and `e'` are identical, so the router load-balances them
identically (degenerate). Differentiation comes for FREE from the **existing** training:

- The router rows for `s` and `e'`, though equal at t=0, receive **different gradients** as
  soon as any tie-break or noise routes a token to one and not the other (the SwiGLU
  outputs are equal so the CE gradient is symmetric only at t=0; the `½`-scaled `W2` rows
  get gradients that break symmetry on the first non-degenerate batch).
- This is the same `st_backward` + DMN consolidation path SS-3 uses; **no new training
  code**. The §A.4 claim "DMN distill differentiates the twins" is reused verbatim.
- **Honest risk (LENS A.2):** perfect symmetry is a saddle; with deterministic data +
  `-ffp-contract=off` the twins could stay tied forever (zero gradient to break them). The
  mitigation is to add a **tiny deterministic asymmetry** to `e'`'s router row at
  resurrection (e.g. add `+δ` to one coordinate, `δ ~ 1e-4`), which makes resurrection
  ε-preserving with a *named, bounded* ε rather than relying on float noise. This δ is the
  ONLY intentional perturbation and is logged.

---

## 3. Fleet coherence (mixed-size minds)

### 3.1 The merge-across-sizes problem, stated precisely

`st_merge_cohort` (SS-3) and `gl_merge`/`gl_merge_w` (`gossip_learn.c:69,100`) all take a
flat `n` parameter count and average element-wise. **A 4-expert node and an 8-expert node
have different `n_params`, different `SZ_ROUTER/W1/W3/W2`, and different per-layer strides.
Element-wise averaging them is ill-typed — it would average expert-3's `W2` against
expert-3's-and-half-of-4's bytes. It does NOT "typecheck" (semantically).**

### 3.2 The rule (inherit SS-3's tier-cohort discipline, extended to dynamic E)

SS-3 already partitions the fleet into **same-tier merge-cohorts**: the fail-closed
`st_blob_tier_ok` guard in `st_load` REFUSES a cross-tier blob (`special-structure-mind.md`
SS-3 row; `[ss3-cohort-island]`). SS-4 **extends the cohort key from `tier` to
`(tier, nexpert)`**:

> **RULE [grow-cohort]:** two student blobs may be averaged (`st_merge_cohort` /
> `gl_student_*`) **iff they have IDENTICAL `(tier, nexpert)`**. The merge guard ANDs the
> existing tier check with an `nexpert` equality check. A node that has grown to E=8 is in a
> DIFFERENT cohort from an E=4 node; they form merge-islands by construction, exactly as
> S/M/L tiers do today. This NEVER calls `gl_merge`/`gl_merge_w` on mismatched `n` (the
> ill-typed case is structurally impossible), and NEVER touches R3 `rw[]`
> (`[baby-merge-isolation]` preserved).

**The single shared mind across all sizes stays the R3 `rw[]` crown** (`r_forward`,
`r3_incontext.c:192`), which has **no expert axis at all** and is byte-identical
fleet-wide regardless of student size (`special-structure-mind.md` §3.2: "the shared
fleet-wide 'one mind' stays the R3 `rw[]` core (unchanged), so heterogeneous students never
threaten the crown result"). So "are grown + ungrown nodes ONE mind?" → **yes, at the R3
crown layer; the student is a per-cohort accelerator, not the identity carrier.** The
cross-cohort bridge for student learning is **distillation (teacher path)**, the same
honest gap SS-3 already declares — NOT merge.

### 3.3 Does growth make the Path-W lossiness worse? (honest)

Path W¹ (`gl_merge`) is LOSSY for two minds that learned DIFFERENT facts
(`moment_2026_06_12_wave41_one_mind`). SS-4 does **not** average across sizes (§3.2), so it
does NOT add a new lossy cross-size merge. *Within* a cohort, growth is orthogonal to the
W¹/W² question — all members have the same E, so the existing SS-3 convergence (and
Path-W² Fisher recovery, when it lands) applies unchanged. **SS-4 neither improves nor
worsens Path-W; it sidesteps the cross-size case by cohorting.** (If a future wave wants
TRUE cross-size merge, it needs an expert-alignment / optimal-transport step — explicitly
DEFERRED, §6.)

### 3.4 Thrash / churn (N oscillates → grow/shrink churn?)

`cap_experts_of(N)` is monotone in N and clamped to `[1, CAP_E_MAX=16]`
(`degrade.c:160-164`; `capacity_self_test` proves monotonicity + clamp,
`degrade.c:301-338`). But a flapping N (a node joining/leaving) could thrash E. Rules:

- **Growth is hysteretic, not instantaneous.** Drive E from a **smoothed** alive-N (e.g. an
  EWMA or a "sustained for T seconds" debounce), NOT the raw `alive_node_cnt`. Reuse the
  degrade-level transition discipline (`degrade.c` already debounces level changes;
  `transition_cnt`).
- **Growth is the cheap direction (add DEAD experts, EXACT, §1).** Adding capacity that
  contributes nothing is harmless, so the grow threshold can be eager.
- **Shrink is the expensive direction** (data loss) and is gated harder (§3.5).

### 3.5 Shrink path (is growth monotonic? what about a node that loses peers?)

Growth is NOT strictly monotonic over the fleet's lifetime, but **per-node E is
monotone-up by default** (we do not shrink on transient peer loss). A real shrink (E=8→4,
e.g. permanent fleet contraction or thermal demotion `device-capacity.md`) is the
**L→S thermal-demotion data-loss hole** called out in the regions verdict. SS-4's shrink
rule:

> **RULE [grow-shrink-fold]:** before dropping expert `e`, **fold its learned mass into its
> placement-replica** (the SS-5 `st_expert_owners` replica list) via a *weighted* combine,
> THEN drop it. A bare drop loses learning; the fold is lossy-but-honest (a half-width mind
> cannot hold a full-width mind's specialization). Shrink is **NOT exact** and is NOT gated
> by `[expert-growth-preserves]`; it gets a separate `[shrink-folds-mass]` cert (the dropped
> expert's most-confident inputs still route sanely post-fold).

**Scope call:** shrink is DESIGNED here but **DEFERRED to a later slice** — the watch-class
M baby (E=4) runs everywhere and rarely needs to shrink; growth is the headline. A node
that merely loses peers does **nothing** to its own E (it keeps its experts; only the
node-router pool shrinks, handled by `moe.c`/SWIM already).

---

## 4. The cert `[expert-growth-preserves]` (falsifiable, non-vacuous)

Mirrors the `[smp-one-mind]` crown's FNV-1a discipline (`r3_onemind_forward_hash`,
`r3_incontext.c:640-682`) and SS-6's `st_forward`-hash discipline.

**Procedure (in-process, no network — the SS-3/SS-5/SS-6 cert style):**

```
1. st_init_tier(m, SEED, M)          # E_old = 4
2. (optionally train a few steps so experts are NON-trivial — a TRAINED model,
    not fresh init, so the cert is non-vacuous: random experts could coincidentally
    pass; trained ones make any selection-set change visible in the output)
3. fix an input X (fixed bytes, like K[]/V[] in r3_onemind_forward_hash:646-648)
4. st_forward(m, X, n, logits_before); H_before = FNV1a(logits_before)   # full [n*V]
5. st_grow_experts(m, 8)             # E_new = 8, all new experts DEAD (§1.2 + alive=0)
6. st_forward(m, X, n, logits_after);  H_after  = FNV1a(logits_after)
7. ASSERT H_after == H_before        # BYTE-IDENTICAL  => EXACT preservation
   ALSO ASSERT m->nexpert == 8 AND firing-width nk unchanged for every token
   (st_last_fire_width / topk_n) => the new experts truly never fired (non-vacuity)
```

**FALSIFIER (must be a real, runnable control, per `feedback_cert_must_cover_all_paths`):**
replace step 5's DEAD init with **naive random-init new experts** (`alive=1`,
`router_row=runi(...)`, `W2=runi(...)` — the textbook-wrong path). Then the new experts
enter the chosen set on some tokens, `nk` changes, the softmax renormalizes, and
**`H_after != H_before` ⇒ the cert FAILS.** The falsifier build is shipped alongside
(`-DSS4_GROW_NAIVE`, the SS-6 `SMP_ONEMIND_RACE` pattern) and must go RED deterministically
on every run (no flakiness — the §②.2c nit lesson).

**Hash surface:** FNV-1a over the FULL `logits[n*V]` (every token, every vocab logit), not
just the last token — maximizes the surface a perturbation must survive (same reasoning as
`r3_incontext.c:670-674` hashing both `rc.probs` AND `loss`).

**`[grow-then-learn]` (separate, looser):** after `[expert-growth-preserves]`, RESURRECT
one expert (§2.1) and run K DMN/train steps; ASSERT (a) the resurrection-instant loss spike
≤ a stated bound (the §2.1 ε from `nk` widening), and (b) held-out loss after training is
≤ the pre-resurrection loss (the new expert *earned its keep*). This is the
"distill lowers loss" half of the §8-step-5 sketch.

**`[grow-noop-identity]`:** `st_grow_experts(m, m->nexpert)` (grow by zero) ⇒ output hash
unchanged AND `alive[]` all-ones path reproduces pre-SS-4 `router_pick` byte-for-byte (the
off-path-untouched gate).

**`[grow-cohort]` (fleet):** an E=4 blob offered to an E=8 merge is REFUSED (the
`(tier,nexpert)` guard); the E=8 weights are byte-unchanged (mirror `[ss3-cohort-island]`).

---

## 5. Byte-identity / crown gating (LENS B)

**The shipped kernel + the `[smp-one-mind]` crown `0x2856a99b23880b4c` (commit `755a20fa`)
are UNAFFECTED by SS-4.** Proof obligations:

- **The crown hashes `r_forward` (R3, `r3_incontext.c`), which has NO expert axis** (its
  flat `rw[R_NP=21568]` layout `r3_incontext.c:107-131` is dense Embed+MHSA+FFN+Cls; there
  is no `nexpert`, no router-over-experts). SS-4 touches **only `student.c` / a new
  `st_grow_experts` + the `alive`-mask 2 lines in `router_pick`**. It does NOT touch
  `r_forward`, `r3_onemind_forward_hash`, or any file the crown hashes. ⇒ the crown's static
  `rw[]` is not even in the same translation unit; **growth is a runtime-only event on the
  student's heap arena and never changes the static R3 `rw[]` the crown hashes.** The crown
  does NOT need re-pinning.
- **`router_pick` byte-identity (the student's own off-path):** the `alive[]` skip MUST
  compile to the pre-SS-4 byte sequence when `alive[]` is all-ones. Guarded by
  `[grow-noop-identity]` (§4) and the SS-6 self-test hashes (M=`63e8de333e995913`,
  L=`67f2434f50e791b6`) must be RE-VERIFIED unchanged for a non-grown model after the patch
  (the "re-prove 755a20fa after the edit or STOP" discipline from ②.2b-ii). If the all-ones
  path is NOT byte-identical, STOP.
- **No default-linked R3/crown function is edited.** Unlike ②.2b-ii (which had to edit the
  default-linked `knl_make_ready`), SS-4 edits only the student MoE. The single risk is the
  shared `router_pick`; it is gated as above.

**Verdict:** the crown stays valid by *construction* (different network, different TU,
runtime-only). The only re-pin needed is the **student's** SS-6 forward hashes, re-asserted
unchanged for the non-grown model.

---

## 6. LENS C residue + honest scope / deferrals

- **Cross-size TRUE merge** (averaging an E=4 and E=8 mind by expert-alignment / OT):
  **DEFERRED.** SS-4 sidesteps via `(tier,nexpert)` cohorts (§3.2). The bridge across
  cohorts is distillation, the same honest gap SS-3 declares.
- **Shrink (`[grow-shrink-fold]`):** DESIGNED (§3.5) but **DEFERRED** to a later slice;
  growth is the headline, M baby rarely shrinks.
- **Resurrection exactness:** explicitly **ε, not exact** (§2.1) — and we DON'T gate it with
  the EXACT cert. Honest.
- **`d_model` / `n_layers` growth:** OUT OF SCOPE (§3.1 / `native-student.md §A.4`: second/
  third axes touch attention/embedding/layout and break weight translation).
- **Benefit is small until the baby is larger** (verdict point 5, `special-structure-mind.md`
  SS-4 row): adding DEAD experts to a 4-expert watch-class M baby buys little until SS-7
  grows the baby. SS-4's VALUE today is the *mechanism + the EXACT cert*, proving "脳が
  フリートで育つ" is real and safe — not an accuracy jump.
- **SERIAL constraint:** SS-4 and SS-7 BOTH edit `student.c` (`special-structure-mind.md`
  §8). They must be sequenced, not parallelized, or hand-merged. SS-4 also adds a 2-line
  edit to the shared `router_pick`; SS-7 (bigger baby) likely re-touches the tier table —
  whoever lands second rebases on the first. **Do not dispatch SS-4 and SS-7 to parallel
  worktree agents.**

---

## 7. Open risks for the implementer

1. **(LENS A, biggest)** The `alive[]` mask is what makes preservation EXACT — NOT the
   zeroed router row alone (a 0 logit can still be admitted by margin-widening if an
   incumbent is also near 0). If you implement only "router=0, W2=0" and skip the mask, the
   cert can still PASS on the chosen input by luck while the off-input behavior drifts.
   **Implement the mask; the cert's `nk`-unchanged assertion (§4 step 7) is the tripwire.**
2. **`router_pick` byte-identity off-path.** The all-ones-`alive` path must reproduce the
   pre-SS-4 bytes. Re-verify the SS-6 hashes (`63e8de333e995913` / `67f2434f50e791b6`) for a
   non-grown model after the edit, or STOP. `-ffp-contract=off`, `-O1`, both arches
   (the salty-bug discipline).
3. **The resurrection saddle (LENS A.2).** Cloned twins with equal weights have symmetric
   gradients and may never diverge. Add the bounded δ asymmetry (§2.1) and PROVE divergence
   in `[grow-then-learn]` (the twins' router rows differ by > threshold after K steps), else
   "differentiation is free" is vacuous.
4. **Adam-moment handling across reshard.** Retain incumbents' `mu/vu` at their new strides;
   zero the new slots. Getting the stride remap wrong silently corrupts training (no crash).
5. **Cohort-key guard coverage.** Per `feedback_cert_must_cover_all_paths`: the
   `(tier,nexpert)` check must be added at EVERY merge entry point (`st_merge_cohort`,
   `gl_student_fetch`/`st_load`), not just one. Enumerate them (the SS-3 `st_blob_tier_ok`
   sites) and cover each.
6. **VLA tripwire.** `st_grow_experts` must not introduce a stack array sized by the runtime
   E; everything stays bounded by the FIXED `*_MAX` (the `-Werror=vla` gate, §3.2).
7. **`cap_experts_of` clamp vs `ST_E_MAX`.** `CAP_E_MAX=16` (`degrade.h:40`) but the L-tier
   `ST_E_MAX=ST_E_L=8` (`student.h:92,100`). The router-sizing curve must clamp E to
   `ST_E_MAX`, not `CAP_E_MAX`, or growth overruns the scratch ceilings. Reconcile the two
   ceilings explicitly (likely: SS-4 grows E within `[ST_TOPK, ST_E_MAX]`; `CAP_E_MAX` stays
   the *display/degrade* number until SS-7 raises `ST_E_MAX`).

---

## 8. Summary verdict

- **Adding capacity (DEAD experts) is EXACTLY function-preserving** (byte-identical output
  for ALL inputs), via the `alive`-mask + zeroed `W2`/router-row (§1.3). ε = 0.
- **Turning capacity ON (resurrection) is deliberately ε-perturbing** (the margin-widening
  router cannot keep a newly-firing expert exactly free); we don't claim exactness there and
  gate it with a separate, looser cert (§2.1, §4).
- **The crown stays valid by construction** — different network, runtime-only, never touches
  the static R3 `rw[]` (§5).
- **Mixed-size minds stay ONE mind at the R3 crown layer**; the student is a per-`(tier,
  nexpert)`-cohort accelerator bridged across cohorts by distillation, not by merge (§3.2).
- **The textbook §A.4 "clone + half the router score" recipe is WRONG for this router** and
  is corrected here (§0.2, §1.3, §2.1).
