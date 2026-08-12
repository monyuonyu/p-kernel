---
name: feedback_branch_landed_triage
description: "How to decide if a far-behind branch's work is safely landed in master before deleting — which tests lie and which is decisive."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

Triaging "is this stale local branch safe to delete" (2026-07-11 cleanup: 22→5 branches). Two plausible tests BOTH mislead when the branch is hundreds of commits behind master:

- **`git diff master BR -- <file>` (two-dot per-file) is NON-decisive.** master evolved the file further, so the diff is non-empty even when the branch's change fully landed. An earlier whole-tree `diff master BR` gave 150k lines for the same reason — swamped by master's newer work. Agents in the triage workflow *claimed* "per-file diff empty ⇒ landed"; that claim was false on measurement. Do not trust a subagent's stated evidence — re-run the acceptance test yourself (this is the [[feedback_audit_is_the_engine]] / "commander's 1st diagnosis is often wrong" pattern).
- **Exact added-line grep into master tree (`git grep -F` the branch's `+` lines) OVER-reports UNIQUE.** Code that landed and was then refactored/reworded no longer matches byte-for-byte → false "not in master". It false-negatived wave-interocept-s1 (1/60) and wave-idle-yield (76%) which had in fact landed.

**Decisive test:** does the branch's unique commit's set of NEW files / NEW symbols exist in master?
`git diff --diff-filter=A --name-only master...BR` then `git cat-file -e master:<f>`; for edited files grep master for the distinctive **identifiers** it defined (function/macro names), not prose lines. interocept.c/.h, tests/idle_cpu.sh, dtr_fpdet_hash, ci.yml `fpdet`/`capacity` gates all EXIST in master ⇒ landed ⇒ safe. Design-doc branches whose file is absent in master (web-os.md, self-access.md 431-line) are genuinely LIVE ⇒ keep.

**Safety net that worked:** `-d` refuses non-merged (use it where possible); `-D` only after the file/symbol test confirms landing; tag truly-unpreserved snapshots first (federation draft → `archive/federation-design-draft-2026-06-14`) since only its exact prose was lost (concepts landed at relocated path). Deleted commit objects survive 90d in reflog; remote-preserved branches (byte-identical origin ref) recover via fetch. **Never remove a worktree without `git status` first** — smp-gicv2's worktree held an uncommitted valuable GIC-v2 pin fix ([[feedback_the_debug_env_is_real]], "Opus impls leave work uncommitted").
