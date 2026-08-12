---
name: reference-pino-phone-android-env
description: "The SM-N975F LineageOS phone at 192.168.10.39 — adb on fixed port 5555, native-Termux vs Ubuntu chroot, and the traps in each."
metadata: 
  node_type: memory
  type: reference
  originSessionId: 205f2522-ac99-4734-b2e7-81bab3666b86
  modified: 2026-08-08T02:01:16.266Z
---

mk_pino's play device: **SM-N975F (Galaxy Note 10+), LineageOS, Android 16, arm64,
rooted with Magisk, bootloader unlocked (`ro.boot.verifiedbootstate=orange`,
dm-verity not enforcing, so `/` can be remounted rw).**

**adb: `adb connect 192.168.10.39:5555`.** Set 2026-08-08 via
`setprop persist.adb.tcp.port 5555`; it survives reboots. Before that, every reboot
re-randomised the wireless-debugging port and cost a round trip asking for the new
one. Pairing keys persist, so no re-pairing. `adb reboot` freely — the device is a
play environment and mk_pino said so explicitly.

Two environments coexist:

**1. Ubuntu chroot at `/data/local/chroot/ubuntu`** — the one to use. A *real*
chroot (not proot): no ptrace, full speed, glibc 2.39, systemd as PID 1 in its own
PID namespace, sshd auto-started at boot by `/data/adb/service.d/50-ubuntu-chroot.sh`.
`ssh monyu@192.168.10.39` works from a cold boot with nobody touching Termux.
`ubuntu` in Termux is the local entry point; `/data/local/chroot/enter.sh` is the
real driver. Built largely by a Claude Code running *on the device itself*.

**2. Termux native (Bionic)** — an experiment, now removed. See
[[feedback_claude_code_on_bionic]] for what it took and why it is not worth it.

Device-specific facts that cost time to learn:
- **IPv6 is dead** (`getent` returns AAAA, connecting gives ENETUNREACH). apt needs
  `Acquire::ForceIPv4 "true"`. Same shape as [[thinkpad_broken_ipv6_docker_pulls]].
- **overlayfs is compiled in but refuses to mount**, so Docker's default storage
  driver is unusable — Docker is the wrong tool here regardless of `CONFIG_*`.
  cgroup v1 controllers, netns, veth, iptables are all present; user namespaces are not.
- **`/data/local/tmp` holds work from earlier sessions** (GNOME/XFCE/virgl/thunar
  scripts, `debian.tar.gz`) that is *not* mine. Do not delete without asking.
- Termux's app process runs `cpuset:/foreground` (7 of 8 cores) and inside an app
  cgroup; a root chroot process gets all 8 and no app lifecycle management. That,
  more than libc, is why the chroot feels snappier.

Related: [[feedback_magisk_chroot_systemd_traps]], [[feedback_shell_background_execution]]
