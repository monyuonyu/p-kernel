# living-mind LM-14 — curiosity

> Status: **implemented on `feat/lm14-curiosity`, in-proc cert green, awaiting a
> SEPARATE audit + merge.** The in-process cert (`mind curious`) passes 7/7 and
> is grepped in `.github/workflows/ci.yml` (the `[curio-*]` gates, appended after
> the LM-13 `[forget-*]` block) plus a live-wiring leg (`mind ask` → `mind
> wonder`). This doc is the design-doc-first record for the slice; the north star
> is [living-mind.md](living-mind.md) Part I.

This slice extends [living-mind.md](living-mind.md) — read its **reading
convention** (dream-tier names vs artifact-tier facts) first. Everything below
is **artifact-tier**: it is measured, on R3's synthetic bounded vocab.

> **artifact today (LM-14):** the mind now **remembers keys it was ASKED about
> but did NOT know** (a bounded **want-table**, `R3_WQ_MAX`=4 — the DUAL of
> LM-13's earned salience), **speaks** them (`mind wonder`, `EV_WONDER`=20), and
> when a wanted key is finally **taught**, the accrued want **CONVERTS into
> arrival salience** — the answer to a question the mind kept asking arrives
> **precious and resists eviction**. The load-bearing claim is pure **SELECTION**
> (which fact the bounded queue evicts — the evicted-`seq` sequence), proven
> **without any accuracy measurement**, exactly like LM-13: two runs are
> byte-for-byte identical (same arrivals, same drains) and the **ONLY delta is 12
> pre-teach wonders** of `k_u`. With **nothing wondered** the arrival is default
> salience 1 and evicts on schedule (evicted-seq `[1,2,3,4,5]`, f5 dies); wondered
> 12× first, f5 arrives at salience **8/8** and a **different** fact leaves
> (evicted-seq `[1,2,3,4,6]`, **f5 SURVIVES**). `dmn.c` byte-identical, no wire
> change, no new K-DDS topic.

**"LM-13 made chosen forgetting wise; LM-14 makes the mind know what it doesn't
know — and treasure the answer when it finally comes."**

---

## 1. The claim (falsifiable)

The mind is asked things it cannot answer. Before LM-14 the `m_ask` miss branch
(`r3_incontext.c`) merely printed *"key not in the live queue — answer is the
substrate prior only"* and returned: **no state was written**, so when that same
key was later taught it entered at default salience 1 and was FIFO-evicted **as
if nobody had ever asked**. LM-14 claims:

1. **The mind accrues a WANT for a key it was ASKED about but does NOT know** —
   `r3_want_note`, one accrual site (the `m_ask` MISS branch). SHARE-QUALIFIED:
   the mind is curious ⇔ the key is **unbound** (`m_find_key < 0`) **AND** its
   masked modal share `< M_KNOWN_SHARE` (75.0, the `[teach-consolidated]` bar).
   This qualifier is **load-bearing**: LM-13 §4 showed an EVICTED fact keeps
   ~100% masked accuracy, so *"unbound ⇒ curious"* would mislabel a well-known
   evicted fact as a thing to want. And
2. **when a wanted key is finally TAUGHT, the accrued want CONVERTS into arrival
   salience** — `r3_want_take` inside `r3_fact_learn`: `salience = min(1+want,
   R3_SAL_CAP)`. This is the DUAL of LM-13: the answer to a long-asked question
   resists the min-salience eviction. It covers **every** arrival site (local
   teach, remote `mind_net_task` = the pre-built LM-15 payoff hook, cert
   arrivals). And
3. **the load-bearing property is the SELECTION** (which fact the eviction
   picks — the evicted-`seq` sequence), proven **independently of any accuracy
   number**: two runs share the same arrivals/drains and differ **only** in 12
   wonders; the falsifiers bite on the evicted-`seq` assertion alone. And
4. want is **LOCAL** (never on the wire — the LM-11 F-LOCAL discipline;
   `mind_net_task` must NOT call `r3_want_note`), and accrues at **one site**
   (`r3_want_note`), so the evals — which read the mind thousands of times —
   accrue **nothing** (`s_wonders` unchanged).

**INVARIANT:** a key is **never** in both `r3_fq` (facts) and `r3_wq` (wants) —
`r3_want_note` refuses a bound key (returns −1), and `r3_want_take` clears an
entry the moment its key arrives.

## 2. The mechanism (zero new math, `r3_incontext.c` only)

All in `arch/common/r3_incontext.c`; `dmn.c` byte-identical; no new TU; no wire
change; no new K-DDS topic. `EV_WONDER=20` is one `#define` in
`arch/common/include/galaxy.h` + one hosted-only `gx_type_name` case in
`galaxy.c`.

### 2.1 The want-table (~20 lines, next to `r3_fq`)

```
#define R3_WQ_MAX     4              /* dual of R3_FQ_MAX             */
#define R3_WANT_CAP   R3_SAL_CAP     /* want clamp == the salience cap (8) */
#define M_KNOWN_SHARE 75.0f          /* the [teach-consolidated] bar */
typedef struct { UB key; UB want; UB used; UB _pad; UW seq; } R3_WANT;  /* 8 bytes */
static R3_WANT r3_wq[R3_WQ_MAX];
static UW r3_wq_seq;   static UW s_wonders;   /* structural counter, [curio-*] */
```

`s_fq_reset` clears `r3_wq[]`, `r3_wq_seq`, and `s_wonders` (the VII.0 #5 amnesia
bomb — a cert never inherits a stale want).

### 2.2 `r3_want_note` — the ONE want accrual (the DUAL of `r3_fact_touch`)

```
INT r3_want_note(INT k, float share_modal);
   -1 = k is BOUND (already held — nothing to want)
    0 = unbound but the WEIGHTS already know it (share >= M_KNOWN_SHARE)
   >0 = it WONDERED: the NEW want (min(want+1, R3_WANT_CAP))
```

On a wonder: raise `want` (clamped), bump `s_wonders`, emit `EV_WONDER`. Table
full ⇒ evict **min-want, tie-break OLDEST** (the dual of LM-13's
min-salience-tie-oldest). It is a **pure** bookkeeping read — it touches neither
`rw[]` nor the RNG (the caller supplies the already-computed masked share). The
**sole production caller** is the `m_ask` MISS branch (passing `share[pred]`); the
cert calls it directly. It is deliberately **NOT** called from `m_masked_vote` /
`s_eval_fact` (eval purity) nor `mind_net_task` (F-LOCAL).

### 2.3 The conversion (inside `r3_fact_learn`, right after `salience = 1`)

```
UB want = r3_want_take(keys, n);   /* max want over the arriving keys; clears them */
if (want > 0) f->salience = min(1 + want, R3_SAL_CAP);   /* + a print */
```

`r3_want_take` is the sole clearer of a want (restoring the INVARIANT). Because it
lives at the ONE arrival site, **every** arrival converts uniformly. Revise
(`r3_fact_revise`) supersedes IN PLACE and never reaches this branch, and a bound
key is never in the want-table — so revise is untouched (the INVARIANT holds).

### 2.4 The verbs

- `mind wonder` — lists the want-table (key word, `want`/`R3_WANT_CAP`, `seq`);
  *"curious about nothing"* when empty.
- `mind curious` → `r3_curiosity_test` (the cert below).
- `EV_WONDER=20` (`galaxy.h`, after `EV_FORGET=19`): a want-ray leaves my star,
  `a = key, b = accrued want`. No-op inline on bare-metal, exactly like
  `EV_FORGET`; hosted `gx_type_name` returns `"wonder"`.

## 3. The certificate (`mind curious` → `r3_curiosity_test`)

Held-out masked eval (`s_eval_fact` / `m_masked_vote` on `S_SEED_HELD`, chance ≈
1.6%). Baseline like `fgt_build`: `f1..f4` (keys 0..7) drained to RETAINED (seqs
1..4). `f5..f9` = **singletons** on distinct upper-vocab keys with readable
off-bias values (`forget_pick_readable` — **the live mouth's shape, NO
adversarial single-class construction**); each singleton arrival forces exactly
**one** eviction. Two runs are IDENTICAL (arrivals, drains) — the **ONLY delta**
is 12 pre-teach wonders of `k_u` (= `f5`'s key).

**`k_u` is SELECTED, not assumed.** The substrate has strong per-key biases
(LM-13 note (a)): measured here, key 8's masked prior is **97.5%** — the frozen
mind is *confident* about it, so it is correctly **not** curious. The cert
therefore **probes** and picks `k_u` = the first upper-vocab key genuinely unknown
(masked prior `< 75`) on the post-`fgt_build` state (measured: key 10 "stone",
65.0%), and `[curio-prior]` re-checks the precondition **with teeth** (it FAILs if
no unknown key exists). The `[curio-bounded]` leg likewise picks 5 distinct
unknown keys dynamically.

| tag | what it proves | gate | **measured** |
|---|---|---|---|
| `[curio-prior]` | `k_u` is genuinely UNKNOWN (precondition, checked not assumed) | masked prior `< 75`; want-table 0/4 | key 10 "stone" prior **65.0%**; 0/4 |
| `[curio-accrue]` | 12 wonders accrue (clamped) at the one site | `want(k_u) == 8`; `s_wonders == 12`; table 1/4 | **8/8**, **12**, **1/4** |
| `[curio-unwondered]` | **DISEASE control, LOAD-BEARING SELECTION**: 0 wonders ⇒ default salience 1 ⇒ min-seq FIFO ⇒ f5 dies. RED if the conversion fires spuriously. | evicted-seq `== [1,2,3,4,5]` **[load-bearing]** | **[1,2,3,4,5]** (f5 dies) |
| `[curio-wondered]` | **CURE, LOAD-BEARING SELECTION**: the ONLY delta = 12 wonders ⇒ f5 arrives at cap, a DIFFERENT fact leaves, **f5 SURVIVES**. RED if the conversion is broken. | f5 arrival salience `== 8`; want entry cleared; evicted-seq `== [1,2,3,4,6]` **[load-bearing]** | salience **8/8**; cleared; **[1,2,3,4,6]** (f6 dies, **f5 SURVIVES**) |
| `[curio-weightknown]` | the **share qualifier** guard: LM-13's class-spread eviction leaves the evicted key's weights ~100%, so wonder-probing it is NOT curious. RED if the qualifier is dropped. | evicted key unbound; masked share `>= 75`; `r3_want_note == 0` | unbound; **100.0%**; **0** |
| `[curio-clean]` | eval/hit **purity**: reads accrue nothing | `s_eval_fact`×4 + `m_masked_vote`×80 + 5×`r3_fact_touch` ⇒ `s_wonders` delta `== 0` | delta **0** |
| `[curio-bounded]` | table overflow evicts **min-want tie-oldest** | 5th want ⇒ min-want (oldest) evicted, want=3 entry survives | B(want1,oldest) evicted; **A(want3) survives**; 4/4 |

**Why it is load-bearing.** The load-bearing property is the **SELECTION** — the
evicted-`seq` sequences — and the cure/disease runs share the **same** code (same
arrivals, budgets, drains), differing **only** in the 12 wonders. So the cert
**reddens** if the conversion is rigged, dropped, or the accrual leaks. The
falsifiers were **run** (implementer, 2026-07-04) and each BIT:

- **kill the conversion** (`r3_want_take → 0`) ⇒ `[curio-wondered]` **RED**
  (evicted-seq stays `[1,2,3,4,5]`, f5 no longer protected); `[curio-unwondered]`
  stays green — the differential is EXACTLY the conversion;
- **drop the share qualifier** ⇒ `[curio-weightknown]` **RED** (`r3_want_note`
  now wonders about a well-known evicted key, returns `>0`);
- **leak accrual into a read path** (`s_wonders++` in `m_masked_vote`) ⇒ both
  `[curio-accrue]` **RED** (`s_wonders` exceeds 12) and `[curio-clean]` **RED**
  (delta `> 0`).

**Live-wiring leg** (covers the production `m_ask` MISS call-site the in-proc cert
bypasses): `mind ask leaf` on the RAW substrate (leaf's prior 45% `< 75`) WONDERS
and `mind wonder` LISTS it (grepped in CI). It is placed **before** `mind teach
sun yellow`, because once sun→yellow is consolidated the substrate generalizes and
leaf reads "yellow" at 100% — at which point NOT wondering about leaf is the
qualifier **working correctly** (see §4).

## 4. Honest findings & bounds

- **`k_u` must be SELECTED, and the qualifier can (correctly) suppress a wonder.**
  The design's first choice (key 8 = "fire" as `k_u`) FAILED empirically: the
  frozen substrate's masked prior on key 8 is **97.5%**, so the share qualifier
  (rightly) judged the mind *not curious* about it. Only 3 upper-vocab keys
  (10, 12, 15) are genuinely unknown post-`fgt_build`; the cert now **probes** and
  selects one, with `[curio-prior]` asserting the precondition with teeth. This is
  the LM-13 note-(a) discipline (facts/keys must not already be known) applied to
  curiosity. The same effect shows in the live leg: after sun→yellow is
  consolidated, "leaf" reads "yellow" at 100% and the mind stops wondering about
  it — **that is the qualifier working**, not a bug.
- **The SELECTION is generic; there is NO accuracy pathology.** Unlike LM-13
  (whose accuracy leg needed a worst-case single-adversarial-class f5), LM-14's
  singletons carry readable off-bias values (the live mouth's shape) and the cert
  makes **no accuracy claim at all** — it asserts only which fact the eviction
  picks. This is the LM-13 lesson applied by construction: the disease is generic
  (a 4-slot queue overflowing under ordinary singleton teaches), not staged.
- **Detects UNCERTAINTY, not ERROR (honest bound).** A confidently-WRONG answer
  reads as "known" (high masked share) and is therefore **not** wondered about —
  that is LM-12 (belief revision) territory, not LM-14. LM-14 only catches
  *"asked AND the weights are unsure."*
- **Toy by parameter count** — `R_NP`≈21,568, `R_DM`=48, 16 key-words × 64
  answer-words, single-token recall, no grammar/generation (the whole
  living-mind bound). Curiosity is proven on the synthetic vocab.
- **Non-goals (deferred):** LM-15 region pull-teach (the answer channel + payoff
  ALREADY ship in LM-14 — `r3_want_take` converts a remote `mind_net_task`
  arrival too; LM-15 only needs the `mind/want` question topic, budget-checked
  against `kdds.h` 16-slot, the wave-48 lesson); want **persistence** across
  restart (pairs with Android-persistence — today it is RAM-only); want as an
  interoception axis; want **aging**; natural-language questions. A single accrual
  signal (`r3_want_note`) keeps the causal isolation the cert depends on.

## 5. Crown

`r3_incontext.c` + `galaxy.h` are bare-metal TUs, so the `.text` **legitimately
moves** — the same intentional-drift case as every LM slice. `galaxy.c` (the
`gx_type_name` case) is hosted-only. `dmn.c` diff is empty; there is no wire
change and no new TU, so there is no parity risk. The want logic is **NOT**
hosted-gated (that would fork the memory behavior between hosted and bare-metal).

Dev crown `.text` sha256 (sandbox gcc 15.2.0; parent `f51eb00e…`/`a0bed501…`
reproduced byte-identical FIRST for environment trust, then the new LM-14 values
reproduced by the implementer's clean build):

    aarch64  be41bbf6b9f018b761ca1e1278878936d2693cf58ca845e520d46c3b6912ed78   (was f51eb00e…8127f7cba5)
    x86      248633de008d0ff036597ab9f8a96f141334732f94f085e29dddddf20aaab019   (was a0bed501…8d741237)

NOT merged by the implementer — a SEPARATE audit + the commander's own clean-build
reproduction of these crowns precede any merge.
