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

## Spent plans & superseded reviews (archived 2026-07-01, P4)

- signing.md — provenance signatures for CODE and WEIGHTS, never humans → SHIPPED wave-38 (`arch/common/ed25519.c` + `sign.c`, 4 green CI gates). Its §0 permanent owner directive (never sign humans) was lifted into `../../product-soul.md` before archiving.
- federation-r0-plan.md — minimal live 2-cluster slice → SHIPPED (self-test + `[live]` 2-process harness); its finding DNODE_MAX=64 (drpc.h:35) is carried into the live `federation.md §1.0`.
- compatibility-r0-plan.md — interop cert + forward-compatible framing → superseded by the shipped compat layer (`compat_ota.c`, `compat` verb); live doc is `compatibility.md`.
- ss4-function-preserving-growth-plan.md — function-preserving expert growth → SHIPPED (`arch/common/llm/student.c`, cert `tests/llm/run_ss4.sh` / `student_growth_test.c`).
- device-autodetect-plan.md — runtime SMP core-count autodetect + device-capability adaptation → SHIPPED (`dev_capacity.c`, DEVFIT cert `tests/llm/run_devfit.sh`).
- arkfs-audit.md — skeptical FS design audit (wave 15) → findings folded into p-fs / survival-fs; historical.
- review-2026-06-three-brains.md — `moe_infer` three-brains critique → fixed wave-18 (moe.c routes/returns/guards from ONE learned forward); the review now misdescribes current code.
- repo-hygiene.md — 2026-06-06 HEAD cleanup → one-time spent task.

## Live-cluster consolidation (archived 2026-07-01, doc-hygiene wave 2)

Live duplicate clusters were consolidated to ONE canonical doc each; the losers
below were archived after their still-live conclusions were folded into the
canonical. Preserved verbatim (年輪).

- device-capacity-verdict.md — device-capacity/native-student audit verdict (DEFER tier/sizing; capacity meter as observability) → superseded-by `../device-capacity.md §0.5`. Its live core (R3 fixed / student-only variability / discrete tiers / honest capacity meter) is folded there; DEVFIT-1 shipped (`dev_capacity.c`, `tests/llm/run_devfit.sh`, ci.yml-wired).
- device-capacity-mind-sizing-plan.md — DEVFIT-1 "measure the device, auto-fit the mind" HARDENED design → SHIPPED (`arch/common/llm/dev_capacity.c` `tier_of()` + `student.c` `ST_TIERS`/`st_init_tier`, cert `tests/llm/run_devfit.sh` wired at `ci.yml:1258`); SHIPPED summary folded into `../device-capacity.md §0.5`.
- interocept-2-apoptosis-plan.md — apoptosis = Path W² essence-handoff (ACK-before-death handshake as load-bearing) → superseded-by `../survival-loop.md §3` (continuous-replication essence model) + §4 (democratic retirement). Its core thesis was REVERSED (sudden death can't wait for an ACK); kept as the Path W² handoff design record. `interoception.md §4` now points here + to survival-loop §3.
