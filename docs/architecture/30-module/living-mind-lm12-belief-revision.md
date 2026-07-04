# living-mind LM-12 — belief revision

> Status: **SHIPPED + CI-gated.** The in-process cert (`mind revise`) and the
> live 2-node sample (`samples/46_belief_revision/run.sh`) are both green and
> grepped in `.github/workflows/ci.yml`. This doc is the design-doc-first
> record for the slice; the north star is [living-mind.md](living-mind.md) Part I.

This slice extends [living-mind.md](living-mind.md) — read its **reading
convention** (dream-tier names vs artifact-tier facts) first. Everything below
is **artifact-tier**: it is measured, on R3's synthetic bounded vocab.

> **artifact today (LM-12):** a single-token associative binding that is
> ALREADY weight-resident (`sun`→`yellow`, consolidated into `rw[]`) can be
> RE-taught a different value (`sun`→`green`) and the new belief becomes
> weight-resident (masked share ≥ 75%) while the old is DISPLACED (share ≤ 10%,
> rebound- and restart-proof), through the **same** DMN sleep path — no new
> math, `dmn.c` byte-identical, the `MT_TEACH_PKT` wire unchanged. It
> propagates over Path E across a region. NO grammar, NO generation. `R_VALV`=64
> ⇒ chance ≈ 1.6%.

---

## 1. The claim (falsifiable)

A fact `k→vo` that has been consolidated into the slow weights can be **revised**
to `k→vn` by a later teach on the same key `k`, so that:

1. the **new** belief becomes weight-resident (`share[vn] ≥ 75%` on a held-out
   masked vote), and
2. the **old** belief is **DISPLACED, not masked** (`share[vo] ≤ 10%`), and the
   displacement is **rebound-proof** (a later unrelated fact does not resurrect
   `vo`) and **restart-proof** (it survives queue annihilation + a durable
   round-trip), and
3. it happens through the **untouched** production DMN sleep path
   (`dmn_idle_work → r3_consolidate_idle_round → s_round`), never a direct weight
   write and never a queue read, and
4. it **propagates over Path E** so a region peer corrects itself (Site 2,
   last-arrival-wins) and the correction outlives its reviser's death.

Before LM-12, a re-teach of a bound key was **refused** ("belief revision is a
future slice"). LM-11 measured that naive weight-averaging *blends* beliefs; this
slice closes the remaining case: **revising one belief in place.**

## 2. The mechanism — `r3_fact_revise` (zero new math)

`arch/common/r3_incontext.c`, sibling to `r3_fact_learn`:

```
INT r3_fact_revise(UB k, UB v_new);
   -1 = key not bound      (caller falls back to plain teach)
   -2 = same value bound   (no-op; caller drops)
    0 = superseded in place (a DMN sleep now owes the consolidation)
```

The key move is **supersede in place**, not enqueue:

1. `m_find_key(k)` → if unbound, return `-1` (this is *not* a revision).
2. Re-read `(k, v_new)` through the **FROZEN fast layer** — the identical
   majority-of-`R3_TEACH_READS` SUPPORT read `r3_fact_learn` performs at arrival
   (on a dedicated probe RNG so the deterministic arrival stream is untouched).
   The oracle only grades; a misread value is memorized as read.
3. **Supersede the existing binding IN PLACE:** overwrite `yhat[bind] = reading`,
   set `state = R3F_PENDING`, `rounds_done = 0`, `seq = ++r3_fq_seq`. **No queue
   slot is consumed.** The old engram *leaves the replay union* — so the D1
   contradiction (both `vo` and `vn` rehearsed at once) becomes **structurally
   impossible**, not merely discouraged.
4. `dmn_trigger()` — a revision IS a stimulus.

Consolidation then happens **only** via the untouched production path. `dmn.c`
has **no diff**; there is **no wire change**. The caller reads back
`r3_fq[fi].yhat[bind]` for `teacher_agree`, exactly as `m_teach` does after
`r3_fact_learn`.

**Why in-place is the whole idea.** The stability/forgetting engine is
replay-union SGD (`s_round`): every retained fact's engram is rehearsed each
round. If a revision *added* a second fact for `k`, both `k→vo` and `k→vn`
engrams would sit in the union and pull the same masked prompt in opposite
directions. Superseding removes `k→vo` from the union, so the union is
self-consistent and the sleep re-grounds cleanly on `vn`.

## 3. The four call sites (cover-all-sites)

| # | site | file:function | behavior |
|---|---|---|---|
| **1** | shell/web mouth | `m_teach` (r3_incontext.c) | the old *refusal* branch becomes the **revision** branch: print the belief-inversion pair (`pre_share[vo]` high / `pre_share[vn]` low, from the M_PRE_N novelty read), call `r3_fact_revise`, then the **same tail as teach** — `ark_prov_record` with the **NEW** seq (the old prov stays in the hash chain — 歴史地層), `m_publish_teach`, `galaxy_emit(EV_REVISE,…)`, `[revise-arrival]` tag. Same value ⇒ "no revision needed". |
| **2** | remote mouth | `mind_net_task` (r3_incontext.c) | the "already bound — refused" **different-value** branch becomes a **remote revision**: `r3_fact_revise` + teacher **re-attribution** (`mt_rprov_put` re-points B's provenance at the new teacher) + `[shared-revise]` PASS + `EV_REVISE`. **Same-value stays a silent duplicate drop.** Conflict = **last-arrival-wins**, printed loudly. |
| **3** | stale re-drive | inside `r3_fact_revise` | the trap: a node re-drives its last local teach every poll (`m_republish_last`). If `mt_pub_last.key==k && mt_pub_last.val!=v_new`, **clear `mt_pub_have`** so a late region joiner is not re-infected with the superseded value. The local mouth (Site 1) re-publishes the NEW value right after; the remote mouth (Site 2) relies on this clear. |
| **4** | direct `r3_fact_learn` | (unchanged) | left **guard-free** — it is the cert's permanent **D1 naive control** (a naive dual-enqueue that must NOT install the new belief). |

New event id: `EV_REVISE = 18` (galaxy.h, next after `EV_STATE=17`);
`a = key`, `b = (v_old<<8)|v_new`. Local revise: `src=me,dst=NONE`. Remote
revise: `src=origin,dst=me`.

## 4. The certificate (`mind revise` → `r3_revise_test`)

Held-out masked vote on `S_SEED_HELD` (chance ≈ 1.6%). The queue holds `REV_K` +
2 other facts (3 of `R3_FQ_MAX=4`); the naive-add D1 control reaches 4/4 but
triggers **no** FIFO eviction (the add at `n=3<4` never evicts). The NEW value is
selected programmatically as a **readable, off-bias** value for `REV_K` (the
LM-8 hand-derivation of `{5,7}`, done in code): `sun`'s pair is `OLD=3 "yellow"`,
`NEW=1 "green"`.

| tag | what it proves | gate | **measured** |
|---|---|---|---|
| `[rev-baseline]` | the first belief consolidated | modal=`vo`, `share[vo] ≥ 75` | modal=3, **share[vo]=100%**, share[vn]=0% |
| `[rev-blend]` | **DISEASE D1** — naive dual-enqueue (raw `r3_fact_learn`, same start/budget/eval) cannot install the new belief | `share[vn] < 75` | modal=3, share[vo]=100%, **share[vn]=0%** (BLOCKED) |
| `[rev-not-masked]` | **DISEASE D2 / LOAD-BEARING FALSIFIER** — supersede the engram but run **ZERO** rounds: the weights are untouched so a masked ask STILL reads `vo` | modal=`vo`, `share[vo] ≥ 75` | modal=3, **share[vo]=100%**, share[vn]=0% |
| `[rev-cured]` | revise + **production** idle rounds | modal=`vn`, `share[vn] ≥ 75`, `share[vo] ≤ 10`, `teacher_agree=100`, `rounds==budget` | modal=1, **share[vn]=100%**, **share[vo]=0%**, agree=100, rounds_done=10/10 |
| `[rev-noregress]` | no catastrophic interference | every other retained fact's masked acc `≥ 75` | key4→1: 100%→100%; key6→5: 100%→100% |
| `[rev-rebound]` | an overwritten belief does not resurge | after curing + a NEW unrelated fact, re-ask `k` → still `vn` | modal=1, **share[vn]=100%**, share[vo]=0% |
| `[rev-persist]` | the belief is **weight-resident** | annihilate the queue → masked ask answers `vn`; hosted+`PKERNEL_PFS_DIR` also disk round-trips | modal=1, **share[vn]=100%** (IN-MEMORY in CI; DISK when a durable dir is set) |

### Live (`samples/46_belief_revision/run.sh`, the 41_shared_mind shape)

2-node region over the relay: teach `sun→yellow` on A → B answers yellow;
`mind teach sun green` on A (Site 1 revision) → B applies the remote revision
(Site 2) via its own DMN → B answers green ≥ 75; `kill -9 A` → B still green.
`[rev-live]` (the end-to-end) + `[rev-stale-mouth]` (after ≥ 3 poll cycles
post-revision, neither node reverts to yellow — the Site 3 guard held). CI job
`belief-revision-live`, serialized after `shared-mind-live`.

## 5. Honest findings & bounds

- **The D1 disease is a BLOCK, not a 50/50 blend.** The original design predicted
  naive dual-enqueue would *blend* (`max(share)<75`). The measurement corrected
  it: because `s_round` drains the oldest PENDING fact to RETAINED first, the OLD
  belief consolidates and then **dominates the replay union** — the new belief is
  **blocked** (measured `vo=100% / vn=0%`), it never installs. This is a
  *stronger* argument for `r3_fact_revise`: you cannot revise a consolidated
  belief by naively re-teaching it; the old engram must LEAVE the union. The
  `[rev-blend]` gate is therefore the load-bearing half only — `share[vn] < 75`
  (naive cannot install `vn`) — and the tag name is kept for continuity.
- **`[rev-not-masked]` is the load-bearing falsifier.** It RED-flips if `ask`
  ever reads the queue instead of the weights, or if `r3_fact_revise` ever wrote
  weights directly instead of via DMN sleep. D2 (supersede + 0 rounds → still
  `vo`) vs the cure (same supersede + 10 rounds → `vn`) isolates the sleep as the
  sole cause of the displacement.
- **Toy by parameter count** — `R_NP`≈21,568, `R_DM`=48, single-token recall, no
  grammar/generation (the whole living-mind bound). Revision is proven on the
  synthetic vocab, not on real conversational streams.
- **Last-arrival-wins** is the region conflict rule; a network partition that
  stages two *simultaneous* different-value teaches converges to whichever
  arrives last. The old belief remains in the immutable prov hash chain
  (歴史地層) — revision changes the weights, not the history.
