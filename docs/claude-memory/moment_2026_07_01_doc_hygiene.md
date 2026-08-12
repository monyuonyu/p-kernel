---
name: moment_2026_07_01_doc_hygiene
description: "2026-07-01 doc-hygiene wave: a 13-reader harsh audit found the docs 'lie about TIME not code' (0 fabrications, ~57/85 status-rot); remediation archived 26 docs + trimmed living-mind 4037→254, closed the AUDIT-SPRAWL gap-ledger row structurally (OPEN 3→0), all crown-neutral (docs only). Report at docs/review-2026-07-01-doc-audit.md."
metadata:
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

2026-07-01, mk_pino: "全体を俯瞰して手厳しく監査 + ドキュメントを大規模メンテ."

**The audit's one truth (worth re-reading):** the docs don't lie about the CODE
— they lie about TIME. 97 docs across 13 parallel readers (docs↔code), **0
fabrications**, but ~57/85 carried status-rot: shipped+CI-enforced features still
wearing `DESIGN ONLY` / `COMMANDER DECISION NEEDED` headers, caveats bolted on
instead of bodies reconciled. The design-doc-first discipline had become
design-doc SPRAWL — the project had NAMED this itself (`AUDIT-SPRAWL`, the single
open gap-ledger row) and left it open. The meta layer preached "rows decreasing,
not documents increasing" while being the biggest doc pile in the tree.

**Remediation (branch docs/hygiene-2026-07-01 → master 03ac286f, pushed):** ~4.4K
lines net out of the hot path + **26 docs archived** (git mv, 100% renames, zero
deletions) into docs/architecture/archive/; the 8 philosophy-gap-audit files +
~10 SMP plans + spent-plan carcasses moved out; **living-mind.md 4037→254**
(scaffolding → a per-slice SHIPPED table; Part I + honesty conventions kept);
gap-ledger OPEN **3→0** (AUDIT-SPRAWL / NET-DISCOVERY-STAR / KDDS-HANDLE-LEAK →
one-line Closed epitaphs); 3 shipped-but-unguarded certs wired into CI.

**Method notes (the immune system worked):**
- Separate implementer + separate auditor + commander held. The IMPLEMENTER
  caught 3 FALSE "shipped" claims from the audit and handled them honestly
  (e.g. the `[sign-genome]` CI gate does NOT exist → reconciled the doc to
  "shipped in code" + an explicit "not yet a CI gate" note, no overclaim).
  Trust the code, not the audit's word — VERIFY-BEFORE-ARCHIVE.
- The AUDITOR found one real inconsistency (compat note said CI "runs none" of the
  migration certs after P12 had wired one) + recommended deferring an unproven
  heavy live CI job (federation-r0-live) onto a still-red pipeline. Commander
  applied both fixups by hand (03ac286f) — small audit-specified integration edits.
- Invariants HELD: top `/README.md` 原文 untouched, docs/claude-memory/ untouched,
  survival-network Part I + compatibility DECISION verbatim intact, archive-not-delete.

**Also observed:** worktree sprawl (a dozen stale git worktrees) mirrors the doc
sprawl — a hygiene follow-up. Related: [[feedback_audit_is_the_engine]],
[[feedback_development_method_is_the_life]], [[project_repo_structure_intent]].
