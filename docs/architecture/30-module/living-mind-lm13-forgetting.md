# living-mind LM-13 — graceful forgetting

> Status: **SHIPPED + CI-gated.** The in-process cert (`mind forget`) is green
> and grepped in `.github/workflows/ci.yml` (the `[forget-*]` gates, appended
> after the LM-12 `[rev-*]` block). This doc is the design-doc-first record for
> the slice; the north star is [living-mind.md](living-mind.md) Part I.

This slice extends [living-mind.md](living-mind.md) — read its **reading
convention** (dream-tier names vs artifact-tier facts) first. Everything below
is **artifact-tier**: it is measured, on R3's synthetic bounded vocab.

> **artifact today (LM-13):** the bounded R3 fact-queue's eviction — which since
> LM-5 dropped the **oldest** RETAINED fact (FIFO) when a 5th fact arrived on a
> full queue — now drops the **least earned-salient** one. `mind ask` accrues
> salience (`r3_fact_touch`, clamped to `R3_SAL_CAP`=8). With **nothing asked**
> the selector degenerates **byte-identically** to the old FIFO. Measured: a
> fact nobody asks about is evicted (its trace decays **100%→64.0%**); the SAME
> fact, **asked 12× first**, is instead *protected* — a different, unasked fact
> is evicted and the asked one SURVIVES at **100.0%**. `R_VALV`=64 ⇒ chance ≈
> 1.6%. `dmn.c` byte-identical, no wire change.

**"LM-1 cured un-chosen forgetting (catastrophic interference); LM-13 makes
chosen forgetting wise."**

---

## 1. The claim (falsifiable)

The R3 fact-queue is bounded (`R3_FQ_MAX`=4). When a new fact arrives on a full
queue, exactly one RETAINED fact must be evicted (its engram leaves the replay
union, so its weight trace will decay under continued consolidation). LM-13
claims:

1. **The evicted fact is the least-EARNED-salient one** (tie-break: oldest),
   where salience is accrued by `mind ask` — so a fact the owner keeps asking
   about resists eviction, and
2. **with zero earned salience the selector is byte-for-byte the LM-5 FIFO** —
   default salience is 1 everywhere, so min-salience(tie→min-seq) picks the
   oldest RETAINED fact, exactly as before (the LM-3 no-regress hinge), and
3. salience is **LOCAL** — it never crosses the wire (`MT_TEACH_PKT` unchanged,
   `mind_net_task` never accrues), the LM-11 **F-LOCAL** discipline, and
4. it is **one accrual site** (`r3_fact_touch`, called only from the `m_ask`
   queue-hit branch), so evals — which read the mind thousands of times — accrue
   **nothing**.

Before LM-13, eviction was pure age: the FIRST thing you taught was the first
thing forgotten, **even if you asked about it 100×** (the wave-45 unfixed
control). LM-1 cured *catastrophic interference* (un-chosen forgetting); LM-13
makes the *chosen* forgetting — which the bounded queue must do — wise.

## 2. The mechanism (zero new math, `r3_incontext.c` only)

Three small edits, all in `arch/common/r3_incontext.c`; `dmn.c` byte-identical;
no new TU; `MT_TEACH_PKT` wire unchanged.

### 2.1 `r3_fact_touch` — the ONE accrual (~10 lines)

```
INT r3_fact_touch(INT k);
   -1 = k not bound in the queue
   >0 = the NEW salience (min(salience+1, R3_SAL_CAP))
```

`m_find_key(k)`; on a hit raise `salience` (clamped to `R3_SAL_CAP`=8) and bump a
structural counter `s_touches`. It is a **pure** read of the frozen weights'
point of view — it touches neither `rw[]` nor the RNG. The **sole production
caller** is the `m_ask` queue-hit branch (after `m_find_key` succeeds); the web
`/ask` bridge routes through `mind_cmd("ask")`, so **one site covers both
mouths**. It is deliberately **NOT** called from:

- `m_masked_vote` — the pure read the evals use (accrual there would contaminate
  every certificate), nor
- `mind_net_task` — salience is LOCAL, never on the wire (F-LOCAL).

### 2.2 The eviction selector (~8 lines, the ONE eviction site)

`r3_fact_learn`'s `R3_FQ_MAX` budget branch changes its choice of victim from
`min seq` to `min salience, tie-break min seq`, over the RETAINED facts:

```
for each RETAINED fact i:
    if salience[i] < vsal  OR  (salience[i]==vsal AND seq[i] < vseq):
        pick i
```

Because default salience is **1 everywhere**, a queue with zero earned salience
picks the min-seq RETAINED fact — **byte-identical** to the LM-5 FIFO. A PENDING
fact is never evicted; an all-PENDING full queue refuses the arrival loudly (both
unchanged). The `EVICT fact seq=` print is extended with `sal=<n>
(earned|default)` — the **prefix is kept** (CI greps tags/counts, not the text;
`s_evictions` still counts one per eviction so `[stream-livehook]` is untouched).

### 2.3 Cover-all-sites

| site | behavior |
|---|---|
| `r3_fact_learn` arrival | sets `salience = 1` (unchanged). Remote (Path E) facts also enter here — no salience on the wire. |
| **the eviction site** | the ONLY eviction (revise supersedes IN PLACE, consumes no slot). Now min-salience. Emits `EV_FORGET`. |
| `r3_fact_revise` | leaves `salience` **untouched** — a corrected fact keeps the earned importance it accrued while believed. (The `seq` refresh still moves it to the FIFO tail for tie-breaks.) |
| `OM_QSNAP` (`om_q_save/load`) | copies `R3_FACT` wholesale → `salience` rides free; `sizeof(R3_FACT)==24` `_Static_assert` still holds (the `salience` byte was **reserved at 1** since LM-5 — this slice gives it its accrual source). |

New event id: `EV_FORGET = 19` (`galaxy.h`, next after `EV_REVISE=18`), emitted
once at the eviction site: `src=me, dst=NONE, a=evicted seq, b=its salience`.
`m_status` now prints `sal=<n>/R3_SAL_CAP` per fact.

## 3. The certificate (`mind forget` → `r3_forget_test`)

Held-out masked eval (`s_eval_fact` / `m_masked_vote` on `S_SEED_HELD`, chance ≈
1.6%). Setup: `m_quiesce`, `s_pretrain`, snapshot `rw[]`. `f1..f4` = the stream
cert's proven readable/off-bias **working-key** facts (keys 0..7), arrived via
`r3_fact_learn` and drained via `r3_consolidate_idle_round` → **4/4 RETAINED**
(seqs 1..4, deterministic). **f5 = ONE wide fact binding the entire upper vocab,
keys 8..15** (each a derived readable off-bias value): its single arrival forces
exactly **one** eviction, and its 8-binding consolidation is the measured disease
driver. Every run is IDENTICAL (arrivals, budgets, eval) — the **ONLY delta** is
the asks.

| tag | what it proves | gate | **measured** |
|---|---|---|---|
| `[forget-baseline]` | the 4-fact retained queue | each masked acc ≥ 75, occupancy 4/4 | f1..f4 = **100%**, 4/4 |
| `[forget-unearned]` | **DISEASE + LOAD-BEARING FALSIFIER** — zero asks; all salience 1 ⇒ min-salience(tie→oldest) = **FIFO byte-for-byte**; f5 evicts f1, whose trace then decays | evicted seq == seq(f1); acc_f1 < 70 (floor **re-baselined from the measurement**); acc_f5 ≥ 75 | evicted **seq 1 (==f1)**; acc_f1 **64.0%**; acc_f5 **100%** |
| `[forget-cured]` | the ONLY delta = **12× `r3_fact_touch(f1)`** (> cap 8) before f5 ⇒ f1's salience clamps to 8 and the selector evicts **f2** instead; f1 survives, still in the union | evicted seq == seq(f2); salience clamp == 8; acc_f1 ≥ 75; acc_f5 ≥ 75; (acc_f1 − unearned acc_f1) ≥ 20 | evicted **seq 2 (==f2)**; sal **8/8** (12 asks→cap); acc_f1 **100.0%**; acc_f5 **100%**; **+36.0 pts** |
| `[forget-noregress]` | protecting f1 does not damage the other retained facts (one eviction ⇒ f3,f4 never touched) | acc_f3, acc_f4 ≥ 75 in the cure run | f3 **100%**, f4 **100%** |
| `[forget-onesite]` | accrual happens **only** at `r3_fact_touch` | `s_touches == 12` after the cure run (which ran `s_eval_fact`×4 @200 + `m_masked_vote` @80) | `s_touches` **== 12** |

**Why it is load-bearing.** The cure and the falsifier run the **same** code —
same arrivals, same budgets, same eval — differing **only** in the 12 asks. So
the cert **reddens** if the selector is rigged (protect-by-index / -key /
-recency: the zero-ask disease run would then *not* evict f1 → `[forget-unearned]`
FAIL) or if accrual leaks into a read path (`m_masked_vote`/`s_eval_fact` would
push `s_touches` past 12 → `[forget-onesite]` FAIL). The auditor's sabotage
menu: (a) rig the selector to protect index 0 → `[forget-unearned]` RED; (b) add
`r3_fact_touch` inside `m_masked_vote` → `[forget-onesite]` RED.

**Same-day unfixed control** (wave-45 discipline): on the master (FIFO) binary,
the queue evicts **f1 despite the asks** — the disease is real on the shipping
code, not a straw man. The min-salience selector is what flips the victim to f2.

## 4. Honest findings & bounds

- **The design predicted a stronger single-fact disease; the substrate corrected
  it (the LM-12 `[rev-blend]` pattern).** A single new *singleton* fact + its 10
  post-eviction rounds does **not** decay the evicted fact (measured: stays
  100%) — this bounded R3 does not catastrophically overwrite an untouched key
  in one drain. The honest, measured disease driver is a **wide** f5 (8 bindings
  on keys 8..15): its concentrated consolidation decays the evicted (no-longer-
  rehearsed) fact to **64.0%** while the protected one, still in the replay
  union, holds **100.0%**. The `[forget-unearned]` floor is therefore
  **re-baselined to < 70** (the measured 64.0% + margin, tighter than the design's
  75), not invented; the cure gate additionally requires a measured **≥ 20-pt
  gap** (measured +36).
- **One eviction, so no-regress is structural.** Because f5 is a single wide
  arrival, only one fact is evicted; in the cure run f3/f4 are never candidates,
  so `[forget-noregress]` holds with the asks on **f1 alone** (the design's "ONLY
  delta = N × `r3_fact_touch(k_f1)`").
- **Toy by parameter count** — `R_NP`≈21,568, `R_DM`=48, 16 key-words × 64
  answer-words, single-token recall, no grammar/generation (the whole
  living-mind bound). Forgetting is proven on the synthetic vocab.
- **Non-goals (deferred):** salience aging/recency; persisting salience across
  restart (pairs with Android-persistence — today it is RAM-only); salience on
  the wire / Path-W; accrual from revise/remote/interoception (a single accrual
  signal keeps the causal isolation the cert depends on); raising `R3_FQ_MAX`
  (would perturb LM-12's preconditions); curiosity (the NEXT slice — reuse
  salience for "asked-but-unknown"); the Evolution layer.

## 5. Crown

`r3_incontext.c` + `galaxy.h` are bare-metal TUs, so the `.text` **legitimately
moves** — the same intentional-drift case as every LM slice. The dev crown is
re-blessed in [../../audit-trail.md](../../audit-trail.md) (parent
`243f917b…`/`8e670a3c…` reproduced byte-identical FIRST for environment trust,
then the new values). The salience logic is **NOT** hosted-gated (that would fork
the memory behavior between hosted and bare-metal). `dmn.c` diff is empty; there
is no wire change and no new TU, so there is no parity risk.
