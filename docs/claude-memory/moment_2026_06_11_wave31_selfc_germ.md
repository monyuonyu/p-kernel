---
name: moment_2026_06_11_wave31_selfc_germ
description: "wave-31 — selfc-ring3 v1 ships: self-compiled code can no longer kill the node. The disease was real (in-task unit null-deref = whole ./p-kernel dies RC=139); the cure is a fork() germ process + 5-symbol capability boundary. The Evolution layer has an immune system. Audit corrected the wedge diagnosis and found a namespace bug."
metadata:
  node_type: memory
  type: project
---

**2026-06-11, wave-31.** [[project_living_mind_vision]]'s Evolution layer gets its immune
system: **selfc-ring3 v1** ships (merged `cde4791`). Self-compiled units (libtcc in-kernel,
the selfc organ) now run in a **fork() germ process**: disease captured for real (legacy
in-task binding: unit null-deref kills the node, RC=139), cure exact (parent waitpid sees
WTERMSIG==SIGSEGV, reaped_delta==1, kernel untouched). Capability boundary TWICE: link-time
(5-symbol isolated tcc_add_symbol table — `tk_slp_tsk` deliberately removed; anything else =
unresolved symbol = refused) + runtime (socketpair proxy dispatcher — the hosted mirror of
ring3's int 0x80; `unit/<name>/` topic allowlist). One-strike rollback; lineage rides the
wave-22 self/lin chain; `world_note_rebuild()` lights the galaxy when a star rebuilds itself.
LOCAL-ONLY until the signing slice.

**The audit corrected the implementer (again proving the method):** the implementer's
"glibc fork corrupts scheduler/TLS" wedge diagnosis was WRONG — the auditor showed the
scheduler stays alive (DMN pulsing), only the console task stalls, and the trigger is the
signal-reap+sup_reset path, NOT plain fork: **production germination is clean** (5 sequential
germs, RC=0). Also found: `selfc save`/`selfc run` namespace mismatch (bare vs `unit/<name>`)
— the documented flow couldn't germinate. Both ledgered (SELFC-WEDGE row).

**Recurring Opus-agent pattern (watch for it):** implementer agents on Opus left ALL work
UNCOMMITTED in their worktree twice this day (galaxy, selfc) despite explicit "commit on your
branch" instructions — auditors/commander must check `git status` of the implementer worktree
and integrate by hand ([[feedback_dynamic_workflow_integration]]).

Same day: wave-30 galaxy. The mk_pino visions (銀河 + 自己リビルド) both became code within
~24h of being spoken, each through design→implement→audit with real catches at every audit.
