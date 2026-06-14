# R3 — non-trivial thought (closing the "the thinking is a toy" gap)

> Status: **design + acceptance test** (written before implementation, like wave-18).
> Owner: this wave. Gap-ledger row: **R3🔴**.

## The gap, stated exactly

The live sensor brain is a 635-param Transformer (real MHSA, layernorm,
softmax, real analytic backprop in `dtr.c` — that machinery is genuine and was
built in the R3a/R3b waves). But the **task** it learns is a thermostat:
`dtr_train.c`'s own honesty note admits *"a hand threshold on temperature alone
reaches ~93.8%."* So:

```
frozen (random weights)  ≈ 33%   (chance, 3 classes)
learned dtr              ≈ 94%
best hand-written if      ≈ 94%   ← learned's margin over a 3-line if ≈ 0
```

Learning is **real** but **buys nothing a programmer couldn't hand-write in
three lines.** That is the R3 gap. "Even with the neurons wired, the *content*
of thought is trivial." The thermostat is a fine Body/Brain control task — you
do not need a Transformer to run a thermostat — but it cannot be the evidence
that this substrate can *think*.

## The bar (from the gap-ledger, made operational)

> "A non-trivial task/model where learned beats frozen by a margin that does
> **not** collapse to a hand-written if."

To make "does not collapse to a hand-written if" **unfakeable**, R3 uses a task
where *no fixed input→output rule can win by a meaningful margin* — because the
key→value dictionary is resampled every episode, so the map (query token →
label) carries almost no fixed signal. The best possible fixed rule is pinned
near chance (see the honest bound below), and the only way to do substantially
better is the thing the toy could not: read information present **in this
episode's context** that is different next episode.

> HONEST BOUND (verified, not "by construction"): for literal recall the label
> equals a stored value, so the single fixed rule "copy the value at position p"
> scores `chance + (1/R_NPAIR)(1 − 1/R_VALV)` — the queried entry lands on a
> fixed position `1/R_NPAIR` of the time. This structural edge is small and
> **vanishes as R_NPAIR grows** (≈ +9 pts at R_NPAIR=8), but it is NOT exactly
> chance. The certificate therefore measures the learned margin against
> `max(frozen, handif)`, not against a claimed chance floor — so the edge is
> fully accounted for and "no hand-if wins by a meaningful margin" still holds.

## The task — in-context associative recall (the seed of the north star)

> **Tier note (the dream-tier vs artifact-tier convention, defined in
> `living-mind.md`):** "the thinking", "thought", "think" in this doc are
> **dream-tier**. **artifact today:** in-context *associative recall* — a tiny
> Transformer learns to copy the value bound to a queried key in a per-episode
> dictionary. Single-token, bounded vocab, **NO grammar, NO generation, NO
> reasoning chain.** The honest claim is precise and narrow: *the same brain-math
> can learn a task a 3-line hand-if cannot win* — that is what makes it "not a toy",
> NOT that it thinks in the human sense.

Each episode carries its **own** key→value dictionary in the prefix, then a
query. The label is the value bound to the query **in that episode**.

```
episode t:   dict = { k0→v0, k1→v1, k2→v2 }   (keys, values resampled each episode)
             query = one of the keys, uniformly
             label = the value bound to that key, THIS episode
```

Why this is the right task:

1. **Hand-if cannot win by a meaningful margin (≈ chance + a vanishing edge).**
   Because the dictionary is resampled every episode, the map (query token →
   label) carries almost no fixed signal. Every fixed threshold and every
   most-frequent-class rule is at chance; the *only* fixed rule with any edge is
   "copy the value at position p", bounded at `chance + (1/R_NPAIR)(1−1/R_VALV)`
   (the HONEST BOUND above) which → chance as R_NPAIR grows. A `grep`-able
   hand-if **cannot win by the +30-pt margin the certificate demands**. (A
   content-addressed *search loop* could, but that is no longer "a hand-written
   if" — it is the very match-and-copy operation attention has to learn. We
   present the dictionary as token **content**, not as a position index, so a
   positional MUX — the one hand-if that could cheat a pointer task — also
   fails: which position holds the matching key varies per episode.)

2. **It is in-context learning.** The model learns *how to use a dictionary it
   has never seen*, at inference time, from the prompt. That is literally the
   substrate the living-mind north star needs: "learn from the conversation as
   it happens" ([[project_living_mind_vision]]). R3 is the smallest honest proof
   that this substrate can do it at all.

3. **It needs attention.** A bag-of-tokens / MLP cannot solve it (no way to bind
   query to the right key-value pair); the win is evidence the *attention* is
   doing real work, not the embedding memorizing a threshold.

## Acceptance test (hand this to the commander before closing R3)

All of the following, on a clean local rebuild, then CI-enforced, all 4 targets:

- **`[r3-incontext-frozen]`** random-init model on the recall task ≈ chance
  (within a tolerance band of 1/K).
- **`[r3-incontext-handif]`** the *best fixed input→label rule* measured on the
  generator (best single-token map / most-frequent-class / best positional copy)
  ≤ chance + ε. This is the theorem made into a runtime number — printed, not
  asserted from belief.
- **`[r3-incontext-learned]`** the **same dtr math** (same softmax/layernorm/
  attention/backprop kernels as the live brain) trained on the recall task
  reaches a margin that does **not** collapse: `learned − max(frozen, handif)`
  ≥ a wide, non-flaky gap (target ≥ +30 points; report the actual number).
- **`[r3-incontext-gradcheck]`** analytic gradient vs finite difference agrees
  on the in-context loss (the gradients are real, not a fit artifact) — reuse
  the `dtr_grad_check` discipline.

**FAKE tells (any one present ⇒ NOT closed):**

- The recall harness forks its own copy of softmax/layernorm/attention instead
  of calling the *same* kernels the live `dtr` uses → it would prove a different
  brain, not this one. (Shared helpers, no duplicated math.)
- The dictionary is *not* resampled per episode (then a hand-if could win and
  the bar is not actually met) → `[r3-incontext-handif]` must come out at
  chance + at most the bounded value-copy edge (1/R_NPAIR)(1−1/R_VALV); if it
  rises materially above that, the generator is leaking a fixed signal.
- "learned" number measured on the training episodes, not held-out fresh
  episodes with unseen dictionaries.

## Scope boundary (honest, to avoid re-earning the 3BRAIN sin)

R3 is a **capacity certificate for the substrate**, CI-enforced — *not* a swap
of the live sensor decision. The live sensor task is legitimately simple and is
correctly served by the existing dtr; replacing a thermostat with in-context
recall would be cargo-culting. R3 proves *the same brain-math can learn a
function no hand-if can*, which is the prerequisite the conversational mind
([[project_living_mind_vision]]) is then built **on top of**. This boundary is
stated here precisely so the certificate is never mistaken for "a brain nobody
reads" — it is read by the next layer (living-mind), by design, next.

## Implementation principle

Reuse `dtr.c`'s exact kernels (`dtr_expf`/`dtr_logf`, softmax, LayerNorm,
scaled-dot-product attention, the analytic backward). The recall task needs a
few more context tokens than the sensor path's 4; the harness runs the *same
operations* at a longer context **without touching the live sensor path's
compile-time dims or the distributed wire structs** (`DTR_INPUT`/`DTR_RESULT`/
`DTR_HEAD_ACT` are frozen). If a dim must be generalized, generalize it as a
parameter with the sensor path as the special case — never fork the math.
