---
name: feedback_memory_mirror_to_repo
description: "Standing practice (mk_pino 2026-06-10): Claude's memory directory is mirrored into the p-kernel repo at docs/claude-memory/ and pushed — the development history including the AI's own memory is part of the ark. Sync at each wave's memory update."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

mk_pino (2026-06-10) asked whether the session memories are on GitHub and said 「一応ちゃんと
載せときたいです」. They were local-only; now mirrored.

**The practice:** after writing/updating memories for a wave, sync the mirror:
`cp /root/.claude/projects/-root/memory/*.md /root/p-kernel/docs/claude-memory/` then commit
(+ push when pushing the wave). First mirror commit: `f1b5922` (55 files + README).

**Why:** the ark philosophy ([[project_pkernel_philosophy]], [[feedback_ark_no_identity_verification]])
— the history of how the project was built, including the AI's own memory of building it, is
itself part of the 歴史地層. Published as written, never edited for presentation.

**Editorial principle (mk_pino-approved 2026-06-10):** the STRATUM (memory + its public
mirror) stays VERBATIM as written — including mk_pino's own casual-speech quotes ("これが
私の地層でもある"; curating your own stratum out of embarrassment = the curated record the
ark design rejects). FORWARD-FACING design docs use PARAPHRASE of owner speech, not
verbatim quotes (mk_pino found verbatim casual speech in formal docs embarrassing —
ark-profile.md was amended accordingly). Related: the ark-profile manifesto = a NEW
welcome text embedding the README's dated 目標 block verbatim at its heart (owner
decision; README itself untouched as the front door). Raw session transcripts/agent logs
are NOT published (credential-leakage risk, not embarrassment) — the distilled history
(memory + gap-ledger + design docs) is the public stratum.
