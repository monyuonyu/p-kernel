# survival-recip — reciprocal 応援・受援 (§7 serve-side mutual aid)

> **STATUS: DECLINED 2026-07-04 — NOT IMPLEMENTED, by conscious choice.** Asked the §0
> question, mk_pino chose **pure altruism over self-defense**: the aid economy does NOT defend
> itself against free-riders — it trusts that "弱くても構わない, the herd compensates, and
> abundance makes free-riding irrelevant." No refusal is ever introduced into the serve path.
> This document is preserved as **the road not taken (歴史地層)** — the record that the
> aid economy's self-defense was designed, considered, and consciously declined is itself part
> of the honest history. Design by the fable5 design pass, 2026-07-03. Complements
> `survival-network.md` §6/§7/§8 and the routing-merit `gacc` plan in `survival-g38-impl-plan.md`.
> `gacc` = 応 (who I *ask*); `recip` = 受援 (who I *answer*) — the answer-side was left as a gift,
> unconditional. (If this is ever revisited, the design below is complete and buildable as-is.)

## 0. The question that was asked, and the answer (仕組み vs 思想) — DECLINED

**DECISION (mk_pino, 2026-07-04): pure altruism. Do NOT build recip.** The serve path stays
unconditional; a free-rider is never penalized; abundance and the herd are trusted to make
free-riding a non-problem. The reasoning below is preserved as the considered-and-declined case.

The survival network's §2/§6 core reads as **pure altruism**: "弱くても構わない — the herd
compensates." `recip` introduces a **mild refusal**: a node keeps aiding a peer only while the
exchange stays roughly balanced, so a **free-rider** (consumes remote inference, refuses to serve
it back) is measurably penalized. The mechanism stays inside the philosophy — local, emergent,
central-less, identity-free, weak-inclusive (a hard falsifiable gate, C2, protects weak-but-willing
nodes) — but **whether the aid economy should defend itself at all, or trust that abundance makes
free-riding irrelevant, is a 思想 call that is mk_pino's, not Claude's, to make.** The design is
ready either way; **do not build past this question without his word.**

## 1. Slice statement

Add a **local, per-observer reciprocity debt** on the remote-inference serve path so a node keeps
serving a peer only while their exchange stays balanced — making a free-rider measurably
underperform a mutual-aid node, while a weak-but-willing node is never penalized.

## 2. Disease (measured, real today)

`drpc_dispatch` `case DRPC_CALL_INFER` (`arch/common/drpc.c:304-321`) serves **unconditionally** —
`dtr_classify(input)` → return class to *any* `src`, no reciprocity check. In the collective-learning
economy (`gossip_learn.c` leave-one-class-out shards; a node MUST route to peers for its missing
class; solo ceiling `<80%` proven by `[g22-shard-solo]`), a free-rider that takes but gives nothing
still reaches full accuracy. **Measured (shipped = unfixed control): `F_full_acc ≈ M_full_acc`,
Δ≈0 — free-riding is costless.** That is the tragedy-of-the-commons the economy has no answer to yet.

## 3. Mechanism (real modules; hosted-gated → crown-neutral; local → NO-CENTRAL by construction)

- **Debt table** (new `arch/common/recip.c` + `recip.h`, `#ifdef _TK_HOSTED_LIBC_`, fixed dims):
  `static W recip_debt[DNODE_MAX];` — my *private* ledger of peer p; NEVER gossiped (two nodes hold
  different debt for the same peer — per-observer pheromone, like `gacc[n][c]`). Bare-metal serve path
  stays unconditional → crown `.text` byte-identical (like `moe_state_fold` at `moe.c:264-293`).
- **Serve side** (`drpc.c:316`, when I answer `src`): `recip_debt[src] += RECIP_UNIT`.
- **Requester side** (`moe.c:604-607`, `er==E_OK`, remote expert `p`): `recip_debt[p] -= RECIP_UNIT`.
- **Pure gate** (the testable seam — `drpc_dispatch` AND the cert call it VERBATIM):
  `BOOL recip_should_serve(UB peer)` = **default-open + slack band**: `return recip_debt[peer] <=
  RECIP_DEBT_MAX;`. Balanced/cold peer → served. Free-rider's debt diverges → refused until it
  reciprocates (a pure taker never does). Weak-but-willing → bounded → always served.
  Tit-for-tat / credit flow-control expressed as a local gradient (§7 のフェロモン on compute-aid).
- **Refusal wiring** (no new failure path): `DRPC_CALL_INFER` returns a negative ER (e.g. `E_BUSY`)
  when `!recip_should_serve(src)` → propagates `send_reply → drpc_call → dtk_infer` (`r<0 return`,
  `drpc.c:743`) → `moe_infer`'s existing `er!=E_OK` fallback (`moe.c:608-611`) → the free-rider's
  offloaded requests silently collapse onto its own weak local model. Integer-only, `-ffp-contract=off`
  safe (no float / no salty-bug FMA surface).
- **§8 two-time-constant** (avoid serve/refuse flap): debt updates ride the fast per-request path, but
  the gate is evaluated against a **slow-decayed** debt (decay `×(N/D)` on the deliberation tick, reuse
  `MOE_DELIB_TICK_MS`=2000) — only a *sustained* taker trips the gate, not a brief imbalance.

## 4. Falsifiable cert (`recip_self_test()` in `recip.c`, CI printf-harness `[recip-*]`)

In-proc mixed swarm on the real `gossip_learn.c` leave-one-class-out dataset, remote help routed
through the **production** `recip_should_serve`. Actors: **M** (mutual), **F** (free-rider: always
refuses to serve), **W** (weak-but-willing: tiny/degraded local model, answers every request it can).
Two arms, reciprocity the ONLY toggled variable:

| arm | serve gate | expected |
|---|---|---|
| NAIVE (= shipped) | unconditional | F ≈ M |
| RECIP | `recip_should_serve` | F ≪ M |

Load-bearing gates (auditor owns the final formula):
- **D1 (disease real):** NAIVE `|F_acc − M_acc| ≤ 5` (free-riding costless today).
- **C1 (cure):** RECIP `M_acc − F_acc ≥ 20pt` (F collapses to its solo ceiling `<80`; M stays ~100).
- **C2 (no friendly fire — the 弱くても構わない guard, falsifiable):** RECIP `W_acc − F_acc ≥ 15pt`
  AND `|W_acc(RECIP) − W_acc(NAIVE)| ≤ 5` (the weak-but-willing node is NOT gated).
- **C3 (NO-CENTRAL held):** F's debt at node-A ≠ F's debt at node-B under different traffic.
- **C4 (honest cold-start):** a never-interacted peer (debt==0) is served byte-identically to shipped.

**Falsifiers with teeth:** removing the gate (or `RECIP_DEBT_MAX`→∞) collapses the cure arm into the
naive slot → **C1 goes RED** (mechanism provably load-bearing). A lazy impl that gates on raw
throughput instead of reciprocity starves W → **C2 goes RED** (protects weak-inclusion).
`[live]` tier OPEN: ≥2 processes over `relay`, N≥5, a real free-rider config (`PKERNEL_FREERIDE=1`
stubbing the serve handler) + the salty-bug on-device mandate; ledger `[in-proc] PASS / [live] OPEN`.

## 5. Crown-safety
Every line under `#ifdef _TK_HOSTED_LIBC_`; bare-metal `DRPC_CALL_INFER` stays unconditional → the
`.text`-vs-PARENT crown gate stays byte-identical. No wire change (`WORLD_BEACON`/DRPC packet
untouched; debt never serialized) → no migration-chain cert owed. Refusal reuses the existing
`dtk_infer r<0 → moe_infer` fallback → non-regression req: re-run `survival-loop [live]` green.

## 6. Scope / non-goals
~1 wave, ~120 lines hosted-only (`recip.c/.h` + 3 call-sites + `recip_self_test` + one `[recip-*]` CI
block + `RECIP_DEBT_MAX`/`RECIP_UNIT`/decay `#define`s beside the §8 constants in `moe.h`). Separate
implementer → separate auditor (auditor owns C1–C4). **NOT:** gossiped/identity/crypto (judges
reciprocation, never *who you are* — consistent with "the ark never verifies humans"); does not touch
`gacc` routing-merit (complementary); does not gate the p-fs weight-body serve (`gl_student_*` —
harder content-addressed case, deferred); does not punish weakness (C2 is a hard gate). The robust
part is the *sign + slow-decay*, not the magnitudes — re-fit on live flood, never by weakening the cert.
