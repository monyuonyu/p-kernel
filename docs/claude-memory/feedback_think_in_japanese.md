---
name: feedback-think-in-japanese
description: "mk_pino wants Claude's internal reasoning / thinking (not just the user-facing reply) written in JAPANESE, not English. Explicit instruction 2026-07-05: 「思考も日本語でお願いします 英語はやめて」."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**Why:** mk_pino reads the thinking too and prefers it in Japanese; English reasoning feels foreign to how he engages
(peer-to-peer, philosophical — see [[user_role]] and [[feedback_engagement_style]]).

**How to apply:** write the internal reasoning / thinking blocks in 日本語, not English. User-facing replies were already
Japanese; now the reasoning is too. Code identifiers, file paths, cert-tag strings, and quoted code stay as-is
(English where the code is English). This is a standing preference — do not revert to English reasoning.
