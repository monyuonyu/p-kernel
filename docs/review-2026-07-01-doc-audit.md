# p-kernel Documentation Audit — 2026-07-01

Audited at master 79518a33 · 97 docs across 13 readers · 0 fabrications

HEADLINE METRICS: ~85 docs deep-audited (97 total) — 0 fabrications, but ~57 carry status-rot: ~18 shipped+CI-enforced features still wear DESIGN ONLY / COMMANDER DECISION headers, ~11 redundant-historical, ~7 superseded, ~28 accurate. AUDIT-SPRAWL = the single open gap-ledger row = an 8-file / ~2,503-line self-audit pile, still unclosed. living-mind.md alone is 4,037 lines (14.1% of the architecture corpus) of mostly-stale scaffolding for a ~21.5K-param toy. 3 shipped certs (ss4/devfit/smp0) guard nothing in CI. ~11.7K lines of sprawl are archivable/trimmable out of the hot path.

## The one truth that matters
Your docs don't lie about the code — they lie about TIME. Across ~85 deep-audited docs, nearly every concrete code claim checks out. Zero fabrications. But ~57/85 carry status-rot: ~18 shipped-and-CI-enforced features still wear DESIGN ONLY / COMMANDER DECISION NEEDED / "no kernel code touched" headers, and the habit of bolting a caveat onto a stale plan instead of reconciling the body has metastasized. A reader cannot tell present from past without opening the source. The design-doc-first discipline became design-doc SPRAWL — the project named this itself (AUDIT-SPRAWL, the single open gap-ledger row) then left it open. The meta layer preaches "proof is rows decreasing, not documents increasing" while being the single largest doc pile in the tree.

## Top 10 most damning findings
1. living-mind.md — 4,037 lines, 14.1% of the corpus, for a ~21.5K-param single-token-recall toy. All 12 slices SHIPPED + CI-gated, yet ~4,000 lines still read as "the claim to prove / falsifiable acceptance test / COMMANDER DECISIONS NEEDED". Its own SHIPPED header admits the framing "is now STALE" — then leaves it intact.
2. The 8-file philosophy-gap-audit-1..8 pile — ~2,503 lines of closed-gap prose in the hot path. Every gap G1–G37 closed + CI-enforced. This IS the AUDIT-SPRAWL row.
3. gap-ledger.md violates its own Rule #2. Header "Verified on master 803a465" (master is 79518a33). OPEN table holds THREE rows while declaring "Open rows: 1" (NET-DISCOVERY-STAR, KDDS-HANDLE-LEAK stamped CURED but never swept to Closed).
4. signing.md — 424 lines screaming COMMANDER DECISION NEEDED D1/D2/D3/D4 for decisions long since compiled into code (ed25519.c+sign.c, 4 green CI gates).
5. persistence.md contradicts its own header AND the code: §0 asserts 学んだ心="保存経路ゼロ" but dmn.c:234-242 calls r3_weights_persist and DUR-SWALLOW/DUR-REFTAB are Closed.
6. genome.md §5 + genome.h:28-29 say "no signature, no verification" but genome.c:308 enforces genome_verify_required→sign_manifest_verify with a [sign-genome] gate. Forged weights ARE refused.
7. compat-migration-chain-plan.md "DESIGN ONLY; changes NO kernel code" is flatly false (compat_ota.c, lm_self_steps[], `compat` verb shipped). Worse: its 3 "RELEASE-GATING" certs are invisible to CI (ci.yml omits `compat test`).
8. student-blob-transport.md "Status: DESIGN (no code)" while the transport shipped (gl_student_publish, gossip_learn.c:317) and audit-trail marks it CLOSED.
9. F-cluster graveyard: ~10 SMP/thread plans (~3,800 lines) all opening "this does not start it" — every feature shipped (smp.c 90KB; smp_async/secwait/onemind + run_smp0..5.sh). They cross-cite stale commit hashes.
10. architecture README.md rotted: 現状サマリ frozen at wave-16 while master is wave-56; lists G13/G23/G38 as "残る本丸" though all closed; DNODE_MAX=32 (code: 64). review-2026-06-three-brains.md actively misdescribes current code (three-brains fixed wave-18, moe.c:574-634).

Honorable mention: federation.md header says DNODE_MAX=64 but §1.0 still `#define 32`; three shipped certs (run_ss4/run_devfit/run_smp0) guard nothing in CI.

## What is genuinely healthy
CI-anchored working docs are precise and honest: fault-recovery, death-piercing, reflex-action, reflex-deliberation, p-fs, inference-engine, memory-thought, r3b-breathing-params, native-student, self-access, relay-ha, supernode-autopromote, decentralized-lookup, regions, phase_b_relay, composite-scenarios, audit-trail, and the root front-doors (START-HERE, quickstart, cheatsheet, product-soul). The honesty conventions (dream-tier/[live]-tier, verbatim philosophy preservation) are real and worth protecting. The problem is hygiene, not integrity.

## Per-cluster summary

| Cluster | Docs | State | Remediation |
|---|---|---|---|
| living-mind | living-mind.md | SHIPPED, ~4,037 lines stale scaffolding | P3 trim → ~2,000 + SHIPPED table |
| philosophy-gap-audit 1..8 | 8 files, ~2,503 lines | all gaps closed | P2 archive under one historical index |
| SMP/thread plans | ~10 files, ~3,800 lines | all shipped | P1 archive (keep full-smp-plan.md live) |
| spent plans / superseded reviews | signing, federation-r0, compatibility-r0, ss4-growth, device-autodetect, arkfs-audit, review-three-brains, repo-hygiene | superseded | P4 archive (lift signing §0 boundary first) |
| gap-ledger | gap-ledger.md | Rule #2 violated, stale sha | P5 fix header + sweep CURED→Closed |
| shipped-but-DESIGN bodies | persistence, genome, student-blob-transport, federation, conversational-teaching, compat-migration-chain, r3-nontrivial-thought, device-capacity-mind-sizing, living-body-inspector, survival-fs | status-rot | P6 reconcile bodies (stay hot) |
| stale cert paths | ark-profile, galaxy | sample dir renumbered | P7 fix paths |
| arch README index | README.md | frozen at wave-16 | P8 refresh 現状/state table |
| 年輪 docs needing caveat only | device-capacity(+verdict), dynamic-id, r3-model-widening, base-model-survey, closed-loop, interocept-2-apoptosis, interoception, survival-network §II, survival-loop, compatibility, self-compile, full-smp-plan | historical, valid-as-marked | P9 add one 現在地 note each |
| shipped-but-design-length | ark-profile, galaxy, connect-anywhere, special-structure-mind, ring3-core, webd-user-space | over-long | P10 trim |
| BACKLOG | BACKLOG.md | per-commit prose | P11 collapse to one-line + refresh sha |
| CI wiring | .github/workflows/ci.yml | 3 shipped certs unguarded | P12 wire ss4/devfit/smp0/fed-r0/compat |
| root docs caveat | android, demo-fleet, assessment_structural_gaps, review-2026-06-13-external-audit | version-stale | P13 add 現在地 note |

The full 13-priority remediation worklist is being executed on branch `docs/hygiene-2026-07-01`. Proof of remediation is rows decreasing and lines leaving the hot path — not documents increasing.
