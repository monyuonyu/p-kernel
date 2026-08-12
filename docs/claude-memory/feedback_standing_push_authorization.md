---
name: feedback_standing_push_authorization
description: "mk_pino granted STANDING push authorization (2026-06-28) — push verified work without stopping to ask each time; the human-gate friction is lifted, but the verification discipline is not."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

2026-06-28, mk_pino: "pushはいつでも許可します" (push is always authorized). This is a STANDING
authorization that lifts the per-push human-gate ASK — I no longer stop to request permission
before each push to master. It MODIFIES (does not erase) the rule in
[[feedback_development_method_is_the_life]] that "PUSH is the human gate": the gate is now
*trust granted in advance*, not *removed*.

**Why:** the CI campaign produced many small verified integrations and stopping to ask each
time was friction the user didn't want.

**How to apply:** push verified, integrated, crown-clean work freely. Still NEVER push
unverified/in-flight work — the discipline ("確実に本物にする", crown byte-identity, independent
audit cleared, certs green) is unchanged; only the *asking* is waived. Report each push after
the fact (sha range). Note: the auto-mode permission classifier may STILL prompt on
"Git Push to Default Branch" since a chat statement isn't a persisted permission rule — if it
blocks, cite this standing authorization or ask the user to add a Bash permission rule / run
via `!`.
