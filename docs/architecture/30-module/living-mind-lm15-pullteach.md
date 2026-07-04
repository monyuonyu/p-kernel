# living-mind LM-15 — region pull-teach

> Status: **implemented on `feat/lm15-pull-teach`, in-proc cert green (4/4),
> awaiting a SEPARATE audit + merge.** The in-process cert (`mind pull`) passes
> 4/4 and is grepped in `.github/workflows/ci.yml` (the `[pull-*]` gates,
> appended after the LM-14 `[curio-*]` block). A live 3-node sample
> (`samples/47_pull_teach/run.sh`) proves the mechanism over the REAL relay/
> region, and a self-hosted CI job (`pull-teach-live`) gates it. This doc is the
> design-doc-first record for the slice; the north star is
> [living-mind.md](living-mind.md) Part I.

This slice extends [living-mind.md](living-mind.md) and **cashes the LM-15 payoff
hook** LM-14 pre-built. Everything below is **artifact-tier**: measured, on R3's
synthetic bounded vocab.

> **artifact today (LM-15):** the mind now **REACHES OUT** for what it lacks.
> Where LM-7 (the shared mind, sample 41) lets a **teacher** push a fact it chose
> to share, LM-15 lets the **learner ASK**: a node that wonders about a key `K`
> (an unanswered LM-14 want) **PUBLISHES that want** on a third region-scoped
> K-DDS singleton topic `mind/want`, and a peer that **HOLDS** `K` **re-teaches
> the answer** on the EXISTING `mind/teach` topic. The answer lands through the
> asker's **UNCHANGED** `r3_fact_learn` mouth, the LM-14 `r3_want_take` conversion
> fires automatically, and the pulled answer arrives **PRECIOUS** (arrival
> salience `1 + want`). `dmn.c` byte-identical, **no new task, no new TU** (so no
> Android CMake/Makefile parity risk — the wave-36 trap does not apply).

**"LM-14 made the mind know what it doesn't know; LM-15 lets it ASK the region —
and the region answers what the asker LACKS."**

## 1. The F-LOCAL amendment (read this first)

LM-11/LM-14 law was: *"want never crosses the wire."* LM-15 **amends** it,
precisely:

> **the want KEY crosses the wire; the accrued want LEVEL never does.**

Salience and want **magnitudes** remain LOCAL forever (the LM-11 discipline is
untouched — the conversion at `r3_fact_learn` reads the **local** `r3_wq[i].want`,
never anything from a packet). Only the **key** is published, so a holder knows
*what* to re-teach. The comment blocks at `r3_incontext.c` (the `r3_wq` block and
the `r3_want_note` docblock) and
[living-mind-lm14-curiosity.md](living-mind-lm14-curiosity.md) are revised in the
same commit so the docs do not lie about the code (the 2026-07-01 doc-hygiene
rule).

## 2. The mechanism (`r3_incontext.c` + `dtr.h`/`kdds.h` comments only)

Three moving parts, all riding the EXISTING `mind_net_task` tick (one task, no
IRQ, cooperative safe point):

### 2.1 The question channel — `mind/want`
`MIND_WANT_TOPIC "mind/want"` + `MQ_WANT_PKT` (24 B, `dtr.h`): `magic`,
`want_seq`, `origin_node`, `n`, `wire_ver`, `keys[4]` (== `R3_WQ_MAX`; NO want
levels — F-LOCAL), `vocab_fp[8]`. Region-scoped `KDDS_QOS_LATEST_ONLY`, reserved
at boot in `mind_net_open()` beside `mw_ann_open()` (**before** `dkva_init`
saturates the table). Two `_Static_assert`s pin it: `sizeof(MQ_WANT_PKT) <=
KDDS_DATA_MAX` and `R3_WQ_MAX <= sizeof MQ_WANT_PKT.keys`.

### 2.2 Publish — ONE site (`mq_publish_wants`, once per tick after `m_republish_last`)
Under `m_gate`, snapshot the want-table KEYS (sorted; keys only). Publish **every
tick while non-empty** (the re-drive IS the retransmit cure — region membership
forms only after a SWIM round; the same reason `m_republish_last` exists).
`want_seq` (`mq_seq_update`) bumps **only on a key-SET change** so a responder can
answer-once; a re-drive of an unchanged set keeps it. On non-empty → empty,
publish ONE empty snapshot (overwrite the stale `LATEST_ONLY` slot) then go
**silent** — a mind that wonders nothing sends nothing.

### 2.3 Answer — ONE site (`mq_poll_wants`, once per tick)
Gates in the `mind/teach` order (magic → `wire_ver` → `vocab_fp` → own-origin
drop). For each wanted key, under `m_gate`, `mq_answer_build` decides
**engram-only** (`m_find_key`; zero forward passes, no RNG, no `m_boot`): a hit
builds an `MT_TEACH_PKT` in a **separate** file-static `mq_ans_pkt` (never
`mt_pub_last` — clobbering it would break the LM-12 Site-3 stale-re-drive clear),
**forwarding the ORIGINAL** attribution:
- learned remotely → `mt_rprov_find` gives the original teacher's node + prov
  head (the asker names the TRUE teacher, not the middleman);
- taught locally → self + the per-fact prov head captured at teach time in the
  new 4-slot `mq_lprov` (`§2.5`);
- no prov → self + zeroed head (anonymous, honestly printed at the asker).

The answer is **published on `mind/teach`** through the factored single wire-
writer `mt_wire_send` (which `m_publish_teach` and `m_republish_last` now share
too — one wire-write site, one receive path, zero forked queues). `fact_seq` = my
local seq, so a re-drive is idempotent at the asker (`mt_last_seq` high-water).

**Anti-storm:** answer-once per `(origin, want_seq, key)` (compared with `!=`, so
an asker reboot re-serves, like `mt_last_seq`) + a `(node & 3)` stagger before
the first answer + a re-drive cap `MQ_ANS_REDRIVE_MAX=20` (a dead asker's want
lingers in the `LATEST_ONLY` slot). One answer publish per tick max.

**Self-forgotten corner:** if the forwarded origin == the asker (the asker forgot
its OWN fact after LM-13 eviction + share decay), the asker's own-origin loop
guard would eat the answer — so substitute self as origin + anonymous prov and
print `re-teaching node N its own forgotten fact` (risk 5, flagged).

### 2.4 How the answer rejoins LM-7/LM-14 (zero new code on the asker)
The answer is a well-formed `MT_TEACH_PKT` on `mind/teach`; the asker's
`mind_net_task` ingests it through the UNCHANGED path → `r3_fact_learn` →
`r3_want_take` fires → `[r3-fq] a long-WANTED key arrived (want w) -> arrival
salience s` with `s = min(1+w, R3_SAL_CAP)` → want cleared → `mt_rprov_put` →
`EV_REMOTE_TEACH` → `mind ask` names the teacher. **Preciousness comes from the
asker's OWN local want state, not from the wire** — which is why the answer needs
no flag: it is wire-indistinguishable from a normal teach.

### 2.5 Provenance for locally-taught facts (`mq_lprov`)
`ark_prov_head_id` returns the CURRENT chain head — correct AT teach time, stale
later. So at the two local prov-write sites (`m_teach`'s teach + revise tails) we
record `(fact_seq → prov_head-at-write)` in a 4-slot `mq_lprov`, **deliberately
separate** from `mt_rprov` so `m_ask`'s "taught by node …" print keeps its exact
current semantics (it must not name ME as a remote teacher of a LOCAL fact).

## 3. Topic budget (constraint 2)

`KDDS_SINGLETON_TOPICS = 16` is **NOT** bumped (minimal-change; the ledger proves
16 still holds): eager-at-boot singletons go **7 → 8** of 16
(pfs ann/want/sync/ref, mind/teach, mind/w, cradle/teach, **+mind/want**); the
named worst-case co-active set is **15 of 16** (one spare). Pool-level eager boot
327 → 328 of 400. **Named obligation:** the NEXT cluster-singleton (LM-16+) must
bump `KDDS_SINGLETON_TOPICS` 16 → 20. The `kdds.h` singleton-enumeration comment
is updated to name `mind/want`.

## 4. The cert — disease, then cure (the audit is the engine)

### 4.1 In-binary — `mind pull` → `r3_pull_test()` (single node, CI selftest chain)
Dynamic key selection (`curio_pick_unknown`); **no** hard-coded key/value/
salience (the LM-13 GENERICITY trap). Four gates:
- **`[pull-snapshot-honest]`** — the publish snapshot is exactly the want KEYS, no
  bound key, and **byte-IDENTICAL when only the want LEVELS change** (F-LOCAL
  anti-leak: RED if a level ever encodes into the packet).
- **`[pull-seq-gen]`** — `want_seq` bumps ONLY on a key-SET change; clear-to-empty
  is ONE empty publish then suppressed.
- **`[pull-answer-src]`** — engram-bound → answers forwarding `(origin, seq,
  prov)` (local self+`mq_lprov`, remote `mt_rprov`, self-forgotten substitution);
  unbound → refuses; **weight-known-but-EVICTED → refuses** (rebuilds the LM-13 §4
  class-spread eviction, then asserts no answer — **pins the engram-only scope cut
  RED-ably** so LM-16 must consciously change a gate, not drift into it).
- **`[pull-answered-once]`** — one answer per `(origin, want_seq, key)`; a
  `want_seq` change re-answers; a **LOWER** `want_seq` (reboot) re-answers (`!=`
  not `>`).

### 4.2 Live — `samples/47_pull_teach/run.sh` (3 nodes over the REAL relay/region)
Topology = sample 41's: A(1),B(2) in region 0; C(3) in zone 1, OUTSIDE. The
script NEVER types `mind teach <K>` on A or C. K is DISCOVERED (the first
candidate word A prints `wondering about` for). Disease and cure in ONE run:
`[pull-want-live]` (B hears A's want on the REAL topic), `[pull-unknown-silent]`
(no holder → zero arrival on A, want persists — the mechanism does not
hallucinate), `[pull-region-quiet]` (C's want never reaches B);
`[pull-answered]` (teach B only → **B prints `answering want key <K> from node
1`, emitted ONLY by `mq_poll_wants`, NOT by normal teach-gossip → PROVES the pull
path** → A learns from node 2 → arrival salience `== 1 + want`),
`[pull-want-cleared]`, `[pull-grounded]` (A names node 2), `[pull-consolidated]`
(A's OWN DMN, share ≥ 75), `[pull-region]` (C: still wondering, never received),
`[pull-storm-bounded]` (A learns K exactly once; B's answers capped).

## 5. Crown / CI impact

`r3_incontext.c` + `dtr.h` link into `boot/x86` + `boot/aarch64` ⇒ bare-metal
`.text` moves ⇒ the crown-text-identity relative gate goes RED on this commit vs
its parent — **expected, intentional**. Re-blessed (sandbox gcc 15.2.0; parent
`be41bbf6…`/`248633de…` reproduced byte-identical first):

    aarch64  3e20edbd6a6696de6b3cf3b9e7767d849f9520ad2fcf0863fffd75dc3db34b83  (was be41bbf6…)
    x86      b6a748daacdc0c2b16a8c80e5885fdf3f343964c0e0846ea5beb9082e9d11e10  (was 248633de…)

See the CROWN RE-BLESS entry in [../../audit-trail.md](../../audit-trail.md). The
`ci.yml` selftest chain appends `mind pull` + the four `[pull-*]` gates; a new
`pull-teach-live` self-hosted job runs the sample.

## 6. Scope boundary — what LM-15 does NOT do

- **No weights-only answering.** A peer whose engram was EVICTED but whose weights
  still know K stays SILENT (`[pull-answer-src]` pins this). A weights-only answer
  has no `fact_seq`, no per-fact provenance, and needs `M_ASK_N` forward passes
  per probed key — a full slice (LM-16 candidate).
- **No cross-region pull** (region-scoped topic).
- **No curiosity aggregation / region want-map**, no adaptive backoff beyond the
  `(node & 3)` stagger, no want TTL on the wire.
- **No new galaxy event**; no `mind` verb changes except the `pull` cert verb.

## 7. Open risks (not papered over)

1. **`LATEST_ONLY` slot contention** — many askers share one `mind/want` slot per
   receiver; the live cert uses ONE asker. N-asker fairness is future work
   (fix direction: per-origin want topics → re-opens the budget bump, §3).
2. **Stagger `(node & 3)`** is a guess; two same-mod knowers double-answer
   (harmless — idempotent asker; wasteful at fleet scale). Measure on the ThinkPad.
3. **First-teach pretrain on the responder** delays the first answer (~23–46 s
   hosted); the sample windows carry it (sample-41 precedent) but a loaded runner
   may still flake — same flake class as sample 41 today.
4. **Self-forgotten corner** re-teaches the original teacher **anonymously** (the
   loop guard forces the substitution; the prov chain COULD prove ownership via
   the forwarded head — a stronger story is deferred, flagged).
5. **Eviction race** — a want can be evicted from the 4-slot table while its
   answer is in flight; the answer then lands as a plain remote teach (salience 1,
   not precious). Correct per the invariant; the live cert keeps ≤ 4 wants live to
   avoid a flaky `[pull-answered]`.
