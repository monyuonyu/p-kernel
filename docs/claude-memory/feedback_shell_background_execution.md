---
name: feedback-shell-background-execution
description: "mk_pino wants Bash/shell commands run in the background by default, not foreground."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 205f2522-ac99-4734-b2e7-81bab3666b86
  modified: 2026-08-08T01:28:53.208Z
---

Run shell commands with `run_in_background: true` by default. mk_pino asked for this
explicitly (2026-08-08) after several foreground commands hit the tool's 2-minute
default timeout and he had to background them by hand mid-turn.

**Why:** the work in this environment is long-running by nature — 300MB downloads over
a phone's wifi, `apt` inside a chroot, waiting on systemd to come up, adb reboots. A
foreground call that exceeds the timeout is not just slow, it *kills the thing it
started*: one `timeout 90` wrapper killed the very systemd daemon it had launched, and
the failure looked like "systemd would not start" rather than "I killed it".

**How to apply:** default to `run_in_background: true`, then Read the task output file.
Never wrap a daemon-launching command in `timeout` — daemons are launched detached
(`setsid ... &`) and the timeout reaps the daemon, not just the wait.

Related: [[feedback_engagement_style]], [[feedback_development_method_is_the_life]]
