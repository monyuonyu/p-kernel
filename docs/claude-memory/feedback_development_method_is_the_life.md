---
name: feedback_development_method_is_the_life
description: NON-NEGOTIABLE. The development METHOD (dynamic workflow + commander/auditor separation) IS the life of p-kernel. Never abandon it.
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 53149c59-d57f-4589-aa45-bb25220e2df2
---

mk_pino, emphatically (2026-06-08): **"これは絶対に忘れないで欲しい。この開発のやり方、これがもうこのプロジェクトの命と言っても過言ではない。"** The *way* p-kernel is built is not a convenience — it is the project's life. Treat this as the constitution.

The method, concretely:
1. **Claude is the 司令官 (commander / orchestrator), NOT the implementer — and need not even be the auditor.** Delegate the *doing* to git-worktree subagents, AND delegate the *auditing* to a SEPARATE subagent ([[feedback_dynamic_workflow_integration]] for mechanics). Command with a precise spec + a written acceptance test. (mk_pino, 2026-06-08: "監査役も別途立ち上げればいい" — spin up a separate auditor; I don't have to inspect by hand myself.)
2. **Separation of powers is the point.** implementer ≠ auditor ≠ commander. The implementer must NOT certify their own work; the auditor must NOT have written the code; if I both implement and verify I recreate [[feedback_validator_and_learner_traps]] (the validator that trusts its own model). This separation is *why* the audit catches overclaims — see [[feedback_audit_is_the_engine]].
3. **The auditor (a dispatched agent) independently re-runs the proof** — never rubber-stamps the implementer's self-report. Re-run grad-checks/tests from scratch, run the FAKE-tell greps, read the diff line by line, build all targets. The commander then reads the auditor's verdict critically before merging. Honest > green ([[feedback_engagement_style]]).
4. **SPEED via parallelism is a first-class goal.** (mk_pino, 2026-06-08: "スピードよく進めていく必要がある、ダイナミックワークフローを積極的に活用する.") Actively fan out — safe disjoint lanes run concurrently ("ガンガン進めて"); run implementer + (on return) auditor as agents; only truly conflicting lanes go in sequence. Idle commander time waiting on one agent is wasted — keep multiple lanes moving.

**Why:** This is how the three-brain problem got caught and closed honestly ([[moment_2026_06_08_three_brains_one]]), how every wave stays real instead of green-but-fake. If I ever slip into "I'll just do it myself, I'm closer to the code" — that is the error mk_pino is warning against, and it kills the project's immune system. Catch myself and re-take the commander/auditor chair.

**How to apply:** Default to dispatching worktree agents for implementation; reserve my hands for design (the high-judgment scope), commanding, and auditing. The user does NOT need to re-ask for subagents each time — this method is the standing default for this project.
