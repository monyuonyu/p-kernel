---
name: feedback-cert-isolation-shared-path
description: "A live/integration cert cannot ATTRIBUTE an effect to the mechanism under test if the fixture's own setup also drives a redundant baseline channel that produces the same effect — stub the mechanism in the SAME harness and see if the cert still passes."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**The trap (LM-15 pull-teach, 2026-07-04).** The live cert (samples/47) meant to prove "the mind ASKS the
region and a holder PULLS the answer back" was CONFOUNDED: its cure leg teaches fact K to peer B *locally*,
but that same local teach ALSO fires B's ordinary LM-7 Path E gossip, which carries the **same (origin,seq)
packet** as the pull answer. The asker's want→salience conversion lands precious on WHICHEVER arrives. So the
cert's assertions (arrival over the wire + precious salience) are satisfied by Path E **whether or not the pull
mechanism does anything**. The gates greened; the mechanism was unproven.

**Why the standard checks missed it.** The in-binary unit gates were toothy (each sabotaged → RED). The live
cert had a disease leg and a cure leg. It LOOKED rigorous. The hole was ATTRIBUTION, not teeth: the cure leg's
fixture (teach B) is not a clean intervention on the mechanism — it co-activates a redundant delivery path.

**The cure (generalizes wave-45 [[moment_2026_06_12_wave45_kill_churn]]).** The auditor ran the *mechanism*
negative control in the SAME harness: stub the mechanism to a no-op (here: both `mt_wire_send(&mq_ans_pkt)`
answer-publish calls removed) and re-run. It **still passed** → the cert cannot tell a working mechanism from a
dead one → confound proven, not argued.

**How to apply.**
- For any live/integration cert claiming "mechanism M caused effect E", ask: does the fixture SETUP also drive
  some OTHER path that produces E? If yes, the cert proves correlation/WIRING, not M's causation.
- Always run the **mechanism-stubbed negative control in the same harness** before crediting a live cert (not
  just the input-absent disease leg — the disease leg only shows E needs *something*, not that it needs M).
- A truly isolating cert must differ from the passing run ONLY in M — often that means removing the redundant
  path (e.g. make the baseline channel unable to deliver: actor out-of-scope during the setup window, then
  brought in). "Necessary but not sufficient" partial isolations (e.g. displace a cache slot) are a common trap
  — reason through whether the baseline STILL leaks before trusting them.
- Honest disposition when isolation is hard: SHIP the mechanism on its toothy unit gates + name the live cert
  for what it actually earns (WIRING + downstream effect), and record the true-isolation cert as a named
  follow-up. Do NOT let the live cert's prose claim causation. This is [[feedback_audit_is_the_engine]] +
  [[feedback_validator_and_learner_traps]] in the same spirit.

Related: [[feedback_cert_must_cover_all_paths]] (coverage) vs this (attribution) — different failure modes of
the same "a green cert can lie" family.
