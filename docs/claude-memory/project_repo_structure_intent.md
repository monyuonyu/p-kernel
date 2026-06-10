---
name: project-repo-structure-intent
description: "The top-level README.md is an intentional separate \"front door\" holding mk_pino's original words; the p-kernel/p-kernel nesting is acknowledged debt to de-nest carefully someday."
metadata: 
  node_type: memory
  type: project
  originSessionId: 53149c59-d57f-4589-aa45-bb25220e2df2
---

Two repo-layout facts mk_pino confirmed (2026-06-07), which look similar but are opposite in nature — do not conflate them when "cleaning":

**1. The nested `p-kernel/p-kernel/` IS genuine debt (fix someday, carefully).**
The repo root is `/root/p-kernel/`; the actual source lives one level down in
`p-kernel/`, so docs sit at `p-kernel/docs/...` and there are even two `docs/`
trees (outer `docs/repo-hygiene.md` + inner `p-kernel/docs/`). mk_pino agrees
this is not good. De-nesting (lift `p-kernel/p-kernel/*` up one level) is a real
future task but a BIG mechanical surgery: every `boot/*/Makefile`, all `run.sh`,
CI's `p-kernel/...` paths, and include paths move at once — do it in a dedicated
wave with a full CI-green check, not as a side cleanup.

**2. The top-level `README.md` being separate is INTENTIONAL — do NOT relocate
or fold it.** mk_pino deliberately keeps it as a "front door / preface" in the
human's domain, set apart from the code, because it holds the **原文 (mk_pino's
own original words)** — the 目標 block with the 2025-04-06 / 2026-03-22 dated
entries in their own voice. It is the first heartbeat ("why this was started"),
kept in a place that doesn't get rewritten by code churn. When de-nesting (#1),
KEEP this README at the top untouched and lift `p-kernel/*` beneath it; never
move the 原文.

**Why this matters:** treat the top README as preserved founder-voice, not a
doc to "bring in line" mechanically — when updating it (e.g. the 4S refresh of
the 今動くもの / caveats tables) keep the 目標（原文のまま）block verbatim. It
fits the philosophy: like ARK/p-fs preserve memory, this README preserves the
origin intent in an unaltered place.

Related: [[project_pkernel_philosophy]] [[project_regions_architecture]]
[[feedback_engagement_style]].
