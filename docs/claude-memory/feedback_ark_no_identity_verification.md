---
name: feedback_ark_no_identity_verification
description: "mk_pino's permanent directive (2026-06-10): the ark/profile system must NEVER verify human identity. Pen names, anonymity, real names — all equally first-class, by personal freedom. The honest history IS the declaration itself (歴史地層). Signing stays for code/weights provenance only."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

mk_pino, 2026-06-10, on the ark-profile (人類の記憶) slice — verbatim intent: **「個人の認証なんて
いらないですよ！それは個人の自由です。ペンネームでもいいし、匿名の好きな名前でも何でもいい。
ある人は本物の名前を入れるでしょう。でもそれでいいんです。それが正直な歴史なんですから。
その人が年を取って死んでしまっても、そのペンネームの人が過去にいたんだと、こんなことを
考えていたんだと、そういう歴史地層として残ればいいんです。」**

**Why:** the ark records *voices that existed*, not credentials. Like archaeological strata,
its honesty comes from preserving what was deposited AS IT WAS — adding verification would
create "valid" vs "invalid" registrations and turn the stratum into a curated record. Claude's
initial instinct (signing slice → "verified identity" upgrade path for profiles) was the wrong
direction and mk_pino explicitly rejected it.

**How to apply:**
- The profile/ark design must state "declared, not verified" as the NATURE OF THE MEDIUM, not
  a limitation to fix. No identity-verification roadmap item for humans, ever.
- Pen names / anonymity / real names equally first-class; never nudge toward real names.
- The signature slice (still needed) is scoped to CODE and WEIGHTS provenance
  ([[project_ring3_core_relocation]] selfc fleet evolution, Self-layer chain forgery) — NOT
  human profiles.
- When merging `docs/architecture/ark-profile.md` (design agent was dispatched BEFORE this
  directive and told to sequence "v3 = signed identity" — strike that at merge time and write
  this principle in).

See [[project_pkernel_philosophy]], [[moment_2026_06_09_wave22_self_layer]] (the Self layer's
tamper-EVIDENT-not-unforgeable bound — for the mind's own chain, that nuance stays).
