# Gap Ledger — the single living list of OPEN gaps (canonical)

> One table. The gaps that are **open RIGHT NOW on master**. Proof of progress is
> **rows DECREASING**, not documents increasing.
>
> **Rule of this file (the antidote to audit-sprawl):**
> 1. This table only **shrinks**. New findings **replace** closed rows; they do not lengthen it.
> 2. When a gap ships AND is CI-enforced, **delete its row** (move its epitaph to "Closed" below, one line).
> 3. **Do NOT spawn `philosophy-gap-audit-9`.** v1..v8 are the frozen historical trail
>    (see `README.md` and the banner atop each). The canonical open list is *here*.
> 4. Keep it short. If this file ever rivals the learning code again, it has become the disease it tracks.
>
> Reconciled across `philosophy-gap-audit-{,-2..-8}.md` + `../review-2026-06-three-brains.md`.
> Verified on master `803a465`. Severity: 🔴 contradicts the core · 🟡 weakens it.

---

## OPEN (this is the whole list)

| id | one-line | sev | evidence it is still open (file:line on master) | "closed" means |
|---|---|---|---|---|
| **G33** | Reflex threat *level* is released by a 5 s wall-clock timer + a clamped scalar nudge, not by a controlled quantity. | 🟡 | `reflex.c:304-311`, `reflex.h:60` (`HOLD=5000`), `reflex.c:321-352` (scalar nudge) | Threat level rises/falls with the controlled variable, not a timer. |
| **G13** | Cross-region inference is re-serialized by the coordinator's fixed 200 ms blocking window. (Partly probed by `parallel-infer-live`; residual remains.) | 🟡 | `dkva.c:252-269/640`, `dkva.h:66` (`WIN=200`) | Cross-region inference not gated by a fixed blocking window. |
| **AUDIT-SPRAWL** (review #5) | The self-audit became a second product (v1..v8 ≈ 2431 lines, rivalling the learning code); gaps were *versioned*, not *closed*. | 🟡 | `philosophy-gap-audit{,-2..-8}.md` (8 files) | This ledger is the sole open list; rows only shrink; no v9 is ever spawned. Closes by sustained discipline. |

**Open rows: 3.** Every other G-number is closed (below).

---

## Closed (epitaphs only — one line each; do not re-litigate)

CI-enforced and shipped, therefore removed from the open table:

- **G20** rally/two-axis gate (flee→approach) — CI `[moe-protect] PASS` `ci.yml:68`, `27_protect`.
- **G28** grounded protect (guarded object is first-class; actuator drives threat→0; survives owner kill) — CI `[protect-ground]/[protect-loop]` `ci.yml:75-76`, live `protect-loop-live` `ci.yml:202`.
- **G35** plural protect (many points defended in parallel; survives owner kill) — CI `[plural-protect]` `ci.yml:79`, live `plural-protect-live` `ci.yml:232`, `28_plural_protect`.
- **G22** collective > individual (disjoint shards, no-central gossip avg, survives kill+rejoin) — CI `[g22-gossip-learn]` `ci.yml:86`, live `collective-learn-live` `ci.yml:263`, `32_collective_learn`.
- **G23** node ceiling > 32 — `DNODE_MAX` 32→64; `GL_MAXNODES`, `KDDS_TOPIC_MAX` (5×), `KDDS_HANDLE_MAX` (10×) now derive from `DNODE_MAX` (one source of truth). Per-node static tables + pmesh BEACON scale automatically; single-char node-topic encoders stay unique to id≈79. `[g23-ceiling]` exercises a 40-model `gl_merge` AND a 40-entry live `dnode_table` membership fold (no 32-cap). CI `[g23-ceiling] PASS` `ci.yml`. **Closed wave-19.** FOLLOW-UP: past id 254 needs a 16-bit `node_id` (wire-protocol change; do NOT bump `DNODE_MAX` alone). Test form: in-process (a >32 live mesh of 33–40 `./p-kernel`+`relay` is the stronger form but too heavy for CI here; the in-process test drives the REAL merge+membership code, not a stub).
- **G24** durable memory / ARK FS (content-addressed, versioned, crash-safe, power-cut survival on real HW) — `23_durable`, `ark-crash-fuzzer` `ci.yml:358`, `arkfs-audit.md`.
- **G27 / G32** live-path CI gates (self-test green is not promoted; live N≥3 + kill is the gate) — the live jobs above are the institution.
- **G29** + the earlier gaps **G1–G19, G21, G25, G26, G30, G31, G36, G37** — closed across waves 1–16; full provenance in `philosophy-gap-audit-{,-2..-8}.md`.
- **R3** the thinking is no longer a toy — the SAME dtr kernels (anti-fork: shared `dt_linear`/`dt_softmax`/width-parameterized LayerNorm) learn an in-context associative-recall task whose label is resampled every episode. Independently audited on a clean rebuild (gradcheck re-run at stride 1/eps 5e-4: 21516/21516 params agree, zero kink-exclusions): learned **100%** held-out vs handif 35.2% (= chance + bounded value-copy edge) vs frozen 24.7% → margin **+64.8 pts**. CI `[r3-incontext-gradcheck/frozen/handif/learned]`, 28/28 self-tests, all 4 builds. **Closed wave-19** (capacity certificate for the substrate; NOT a live-sensor swap — the conversational mind builds on top). Doc `r3-nontrivial-thought.md`.
- **3BRAIN / G38** (review #1·#3) the three brains are one — `moe_infer` runs **one** dtr forward whose argmax is the routed, returned, and guarded class; gate if-ladder + `(void)hum/press/light` and `mlp_forward` are gone from the live path (`drpc` infer uses `dtr_classify`). CI `[onebrain-unified]/[onebrain-channels]/[onebrain-nomlp]/[onebrain-accuracy]` `ci.yml:75-78`, RETURNED accuracy 33%→83% via G22. **Closed wave-18-A.** Residual (honest, out of live path): `spec.c` band partitioner predicts band≠class by design; `mlp_forward` remains only in demo paths (edf/pipeline/fedlearn/ai_job task).

---

## Closure record — wave-18-A (three-brain unification) ✅

The acceptance test below was the gate for deleting the 3BRAIN row. All REAL tokens went green
and every FAKE tell was confirmed absent on a clean local rebuild (commander-verified, not the
agent's binary). Kept as the epitaph's proof; **do not re-litigate**.

REAL — all green:

- **One brain, one answer.** `[onebrain-unified] PASS` — for 4 inputs, `returned==routed==guarded==dtr argmax`,
  all from the single `dtr_decide()` forward at `moe.c:439`.
- **All four channels matter.** `[onebrain-channels] PASS` — per-channel |Δprob|×1e-3 = temp 1, hum 9,
  press 21, light 33 (all nonzero). The `(void)hum/press/light` discard is gone.
- **Live path follows the learned brain.** `[onebrain-nomlp] PASS` — on an input where the handwritten
  MLP says 0 and the learned dtr says 1, `moe_infer` returns 1.
- **Learning lifts the RETURNED accuracy.** `[onebrain-accuracy] PASS` — RETURNED-class accuracy
  UNLEARNED 33% → G22-LEARNED 83% (3 nodes, leave-one-class-out, no-central gossip).

FAKE tells — all absent in the live path:

- `grep -nE 'if \(temp < 20\)' arch/common/moe.c` → no hit.
- `mlp_forward` does **not** feed `result_class` in `moe.c` (only call is inside the `[onebrain-nomlp]`
  test); `drpc` infer dispatch uses `dtr_classify`. The `temp HIGH` constants survive only in demo paths.
- `result_class`, gate, and reflex class are all assigned from the one `learned_class`.
- `(void)hum` appears only in comments describing the *old* code.
