# Archive — historical design docs (年輪, preserved verbatim)

These docs shaped p-kernel and are kept for provenance. They are OUT of the hot
path because the work they planned has SHIPPED (and is CI-gated) or was
superseded. Nothing here is authoritative for present state — see
`../gap-ledger.md` (canonical live state) and `../README.md` (the map).
Banners and 年輪 inside each file are intentionally left as-written.

## SMP / multicore / threading plans (archived 2026-07-01, P1)

All shipped: `arch/aarch64/smp.c` (~90KB), `smp_async.c`, `smp_secwait.c`,
`smp_onemind.c`, `mc2_smp.c`; certs `tests/aarch64/run_smp0..5.sh`,
`tests/llm/run_yield.sh`. The live ②.3 roadmap remains `../full-smp-plan.md`.

- smp-1-ipi-preempt-plan.md — ②.1 cross-CPU preemption via GIC SGI IPIs → shipped (smp.c IPI preempt path).
- smp-2-production-scheduler-plan.md — ②.2 production T-Kernel scheduler → SMP → shipped (smp.c / smp_prod.c).
- smp-2b-async-preempt-plan.md — ②.2b async register-context preemption → shipped (smp_async.c).
- smp-2b-ii-secondary-timer-plan.md — ②.2b-ii secondary-CPU CNTP timer + WAIT → shipped (smp_secwait.c).
- smp-2c-one-mind-plan.md — ②.2c [smp-one-mind] byte-identity crown cert → shipped (smp_onemind.c, run_smp5.sh).
- mc2-baremetal-smp-plan.md — MC-2 bare-metal constrained-SMP matmul workers → shipped (mc2_smp.c).
- mc2-1-ncore-equiv-plan.md — MC-2.1 N-core work-queue + [mc2-smp-equiv] byte-identity cert → shipped (mc2_smp.c).
- multicore-matmul-plan.md — ③ deterministic parallel matmul stepping-stone → shipped (mc2_smp.c).
- cooperative-yield-plan.md — DMN consolidation must not starve the node → shipped (run_yield.sh); content still valid, referenced by CI-fix work.
- thread-t-impl-plan.md — Thread T teacher→student teaching over the mesh → shipped (gossip_learn.c / cradle teach path).

## philosophy-gap-audit 1..8 (archived 2026-07-01, P2)

The 8-file, ~2,503-line point-in-time self-audit pile. Every gap G1–G37 it
raised is Closed (and CI-enforced) per `../gap-ledger.md`, whose Closed table is
now the canonical record. Archiving this pile structurally closes the
AUDIT-SPRAWL gap-ledger row. Preserved verbatim; already carried a
「歴史記録・凍結 / superseded by gap-ledger.md」 banner. `audit-9` was never made.

- philosophy-gap-audit.md — audit-1, the original gap sweep → all rows closed, see gap-ledger.md Closed.
- philosophy-gap-audit-2.md — audit-2 continuation → closed.
- philosophy-gap-audit-3.md — audit-3 continuation → closed.
- philosophy-gap-audit-4.md — audit-4 continuation → closed.
- philosophy-gap-audit-5.md — audit-5 continuation → closed.
- philosophy-gap-audit-6.md — audit-6 continuation → closed.
- philosophy-gap-audit-7.md — audit-7 continuation → closed.
- philosophy-gap-audit-8.md — audit-8, the final sweep → closed. Canonical live gaps: ../gap-ledger.md.
