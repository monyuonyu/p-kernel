# Generational migration succession — a mind crosses an architecture gap without dying

> Evolution layer capstone. Companion to `10-requirements/compat-migration-chain-plan.md`
> (the FORMAT-migration chain) and `compatibility.md` §5.3/§5.4. The full design
> rationale is the `evolution-migration-design.md` note (referenced verbatim by the
> code comments); this file is the in-repo record of the SHIPPED v1 slice.
>
> The constitution it implements (compatibility.md §DECISION, mk_pino 2026-06-14):
> 「凍る核は無い。世代交代で進化する … bedrock は無い、途切れない鎖があるだけ。」

## The problem

Generation N+1 has a different mind architecture (new `R_DM`, expert count,
attention shape, vocab), so gen-N's raw weights `rw[R_NP]` (`R_NP=21568` at
`R_DM=48`) do **not** map onto gen-N+1 (`R_NP=70720` at `R_DM=96`). The shipped
wave-47 dims/vocab guard (`r3_incontext.c:1183-1190`) already REFUSES such a
cross-load — the tree treats a cross-arch weight blob as poison. Yet the mind's
IDENTITY (Self-lineage) and its LEARNED KNOWLEDGE must provably cross the gap,
on an ownerless fleet where no core may freeze.

**The decided law:** the chain migrates FORMATS; the GENERATION gap is crossed by
carrying **identity** (lineage) and **knowledge** (re-education/replay), never raw
weights. This slice makes that carriage a protocol with a falsifiable cert.

## What shipped (v1, hosted-tier, single node)

| Piece | Where | Note |
|---|---|---|
| `GEN_ARCHSPEC` object | `arch/common/gen_succession.c/.h` | `{R_DM,R_NH,R_KEYV,R_VALV,R_NPAIR,R_NP,key/val vocab id}`; content-id = the generation's identity. The exact fields the R3_WP header pins. |
| `GEN_SUCC_MANIFEST` object | same | predecessor/successor archspec ids, `successor_pk` (0=no rotation), `engram_flush_id`, `probe_digest_id`, `token_map_id` (0=superset vocab), `invariant_ids[5]`. Travels INSIDE the signed OTA body → bound by sign gates 1-2. |
| Gate 5 (named predecessor) | `compat_ota.c` `compat_ota_accept_gen` | ADDITIVE to the shipped 4-gate AND (`compat_ota_accept` untouched → `[signed-ota-gate]` stays green). A node whose archspec ≠ the named predecessor REFUSES. |
| `LM_UNIT_EV_SUCCESSION` + `lm_self_append_succession` | `lm_self.h/.c` | A new event kind via the existing `age_ms` scheme (ADDITIVE — the 148 B entry format does NOT bump; wave-22 `[self-*]` certs stay green). The succession entry NAMES the successor archspec (`model_ver`) + the bundle manifest (`eng_digest`); the successor's first entry links `prev_entry` to it — identity persists THROUGH the gap. |
| Carriage = **leg-1 engram replay** | driven in the cert on the real R3 mind | For R3, teach IS key→value pairs, so replay is EXACT transfer. Leg-2 soft-target distillation is deferred (`§10`: adds nothing until knowledge is weight-resident in a way replay can't reach). |
| Conscience floor = HARD invariant #5 | manifest `invariant_ids[GEN_INV_FLOOR]` + `law_verify()` no-regress gate | A successor lacking/weakening the floor is an ILLEGAL successor (mk_pino: the floor is immutable). |

## The cert — `[generation-survives]` (`compat test gen`, `-DGEN_SURVIVE_CERT`)

Two pinned R3 configs with a GENUINELY different arch (R_DM 48 vs 96 → structurally
incompatible `R_NP`). Per-run NONCE facts (seeded from real entropy — no teacher,
corpus, or shared store can know them). Asserts: lineage continuous
genesis→successor + succession NAMES the successor archspec + signatures verify;
gen-N+1 recovers ≥ 75% of the SURVIVABLE set after replay; gen-N answered
throughout. Prints the honest SURVIVES table (identity GUARANTEED, engrams
verbatim, raw-rw[] dies-by-decision, un-probed tail DIES).

**Falsifiers (each goes RED):**
- **F1** arch-delta guard — archspec ids DIFFER + the dims guard REFUSES a raw
  cross-load (anti-theater: survival can't "pass" because nothing needed to survive).
- **F2** `-DGEN_SKIP_EDUCATE` — skip replay → recovery drops to chance → RED.
  *Verified: recovered 0/4.*
- **F3** impostor — a from-genesis forgery under an unpinned key is REJECTED by the
  pinned-key verify (the shipped `[sign-selflayer-live]` machinery).
- **F4** illegal successor — wrong named predecessor → gate 5 refuse; lower
  `artifact_ver` → gate 4 refuse.
- **F5** `-DOTA_SKIP_VERIFY` — the accept step is vacated → tampered/downgrade
  artifact accepted → RED. *Verified: F5-tamper=ACCEPT, F5-down=ACCEPT → FAIL.*
- **Isolation sabotage** — delete the bundle mid-run → recovery collapses to chance,
  proving the bundle CONTENT (not any stale in-proc queue) is load-bearing.
  *Verified: 0/4.* (Consolidation needs a freshly-taught PENDING anchor from the
  bundle; RETAINED residue alone never replays.)

## Crown / CI

All succession machinery is hosted-only. `gen_succession.c` is added to the two
HOSTED Makefiles only; `lm_self_append_succession` is behind `_TK_HOSTED_LIBC_`;
the `LM_UNIT_EV_SUCCESSION` macro + guarded prototype emit no code. So the default
bare-metal `.text` is byte-IDENTICAL to the parent crown, verified this wave:

    aarch64 .text  7f3fbda47451133c8b3a28a49ec8edd0af208e814b7658d79ec01114e5e177f1
    x86     .text  260da329dd641ccf0937761ef20f5f11f7bcaa422f6fcfe0db319c667a353f64

CI job `generation-survives` (native gcc) runs the cure + F2 + F5. The day a
generation change reaches BARE METAL (an R_DM bump compiled into boot/aarch64),
the crown legitimately changes — that is an intentional re-bless (the tkernel3
precedent), NOT this wave.

## Honest limits (do not hide)

- **In-proc harness.** The ARCH gap is modelled as DATA (arch-specs + the dims-guard
  refusing a raw cross-load, F1); the KNOWLEDGE recovery is driven on the ONE real
  R3 mind (leg-1 exact replay), weights reset to the fact-free base to spawn the
  fresh successor. Not a true two-binary [live] harness (that is ThinkPad-runner-only,
  §9) — the same "simulate the essential gap, drive the production helper" honesty as
  `compat_arkfs_gap.c`.
- **The un-elicited weight-resident long tail DIES** (design §11). A fact that lives
  ONLY in gen-N's weights and is never re-asked in the education window transfers
  nowhere — printed as coverage, never claimed lossless. The successor is the same
  mind by lineage and by MEASURED recovery; it is not the same mind numerically.
- **Deferred (named, not hand-waved):** runtime arch-spec objects + the student MoE
  generation; key rotation across the gap (design is present, cert is a follow-up);
  fleet-assisted education; the eventual-convergence bet under competing successors;
  simultaneous arkfs-format + generation gap (v1 sequences them).
