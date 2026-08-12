---
name: feedback-magisk-chroot-systemd-traps
description: "Four traps when running systemd in a chroot booted from Magisk service.d — PATH, procfs propagation, backups inside service.d, and logs that lie."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 205f2522-ac99-4734-b2e7-81bab3666b86
  modified: 2026-08-08T02:17:47.438Z
---

Found while repairing the Ubuntu chroot on [[reference_pino_phone_android_env]]
(2026-08-08). All four are the kind that present as "systemd just does not start".

**1. `chroot(1)` passes the caller's PATH through.** From Magisk `service.d` that is
Android's PATH (`/system/bin`, `/apex/...`, `/vendor/bin`) — not one entry exists
inside a Linux rootfs, so the boot script's `bash` cannot find `mkdir`, `mount` or
`chroot` and dies before systemd. Any script crossing the Android→chroot boundary
must set its own PATH.

**2. procfs propagates back and steals the PID numbering.** A chroot that does
`mount --make-rshared /` and then `mount -t proc proc /proc` for its new PID
namespace propagates that mount *outward*, stacking on `$ROOTFS/proc`. Afterwards
`host /proc/1/comm` is `init` but `chroot /proc/1/comm` is `systemd`. So a helper
that scans Android's `/proc` for the daemon returns a number `chroot $R nsenter -t`
cannot resolve: `nsenter: cannot open /proc/<pid>/ns/pid`. **Find the pid in the same
/proc the entering tool will read** — scan `$ROOTFS/proc`, not `/proc`.

**3. Magisk executes everything in `service.d`.** Writing `foo.sh.bak` next to
`foo.sh` there means both run at boot, concurrently. I did this and got two racing
systemd boots. Keep backups outside the directory.

**4. Do not block boot on `/sdcard`, and do not let the log lie.** `/sdcard` is FUSE
and appears long after `sys.boot_completed`; waiting for it before starting systemd
delayed sshd by 2.5 minutes. Start systemd first, bind `/sdcard` later in the
background. And when recording the outcome, poll until the state is *settled* —
querying immediately yields "Failed to connect to bus" (reads as failure) or
"initializing" (true but says nothing).

**5. An interactive shell entered with `nsenter --pid` has no usable controlling
terminal.** The pty belongs to the *outer* namespace (Termux allocated it), the shell
to the inner one, so their process groups are numbered in different namespaces and
`tcsetpgrp()` is refused:

    bash: cannot set terminal process group (-1): Inappropriate ioctl for device
    bash: no job control in this shell

fish is stricter and says "No TTY for interactive shell (tcgetpgrp failed)" plus
"setpgid: Inappropriate ioctl for device" — which reads like a fish bug and is not.
ssh is immune because sshd runs *inside* the namespace and opens its pty there. Fix:
`nsenter ... -- script -qec "/bin/bash -l" /dev/null`, which opens a fresh pty on the
inside. `setsid --ctty` is **not** an alternative — it fails on the same ioctl
(measured). Guard it with `[ -t 0 ]` so the run-one-command path, which service.d
uses and whose output is captured, stays unwrapped.

**Why this matters:** the original log said only "systemd did not come up", because
the failing stage's output went to `/dev/null`. Worse, an earlier entry read
`result: running ssh=active` and was **not** a success — a manual session had left
systemd running and the finder simply picked it up. `boot completed after 0s` versus
`after 16s` was the tell. **A log that reports the attempt rather than the outcome
costs hours.** Related: [[feedback_validator_and_learner_traps]],
[[feedback_cert_isolation_shared_path]].

**How to apply:** when something "worked", prove it was not already working. Reboot
and re-verify rather than trusting a live state you may have created yourself, and
run the cycle more than once — the second cold boot is what exposed traps 3 and 4.
