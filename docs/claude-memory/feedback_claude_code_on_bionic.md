---
name: feedback-claude-code-on-bionic
description: "Claude Code has no Android-native npm build; getting it onto Termux/Bionic takes five patches, and a real chroot avoids all of them."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 205f2522-ac99-4734-b2e7-81bab3666b86
  modified: 2026-08-08T02:01:57.253Z
---

Established 2026-08-08 on [[reference_pino_phone_android_env]]. This is why
"install Claude Code natively on Android" is harder than it looks.

`@anthropic-ai/claude-code` 2.x is a thin wrapper: `bin/claude.exe` is a 500-byte
stub that `install.cjs` replaces with a platform binary. **`install.cjs` already maps
`process.platform === 'android'` to `@anthropic-ai/claude-code-linux-arm64-android`,
but that package is not published on npm** (404). Termux's node reports `android`, so
no optional dependency matches and the stub survives. Worth re-checking the registry:
if that package ever ships, plain `npm install -g` is all it takes.

The workaround is the `linux-arm64-musl` build, and it needs five things:
1. It is **not static** — `PT_INTERP=/lib/ld-musl-aarch64.so.1`, with exactly one
   `DT_NEEDED` (`libc.musl-aarch64.so.1`) and no RUNPATH. One musl loader suffices
   (Alpine's `musl` package).
2. `/lib` is `u:object_r:rootfs:s0`, which `untrusted_app` cannot even `getattr`.
   Invoke the loader explicitly from inside the app's own data dir instead.
3. **Running `apt`/`npm` under `su` creates files without the app's SELinux MLS
   categories** (`app_data_file:s0` instead of `...:s0:c176,c256,c512,c768`), and
   Termux then cannot execute *any* of them — node, git, rg, all of it. `restorecon`
   does not add the categories; `chcon -R -h` with the explicit set does. Re-apply
   after every root-side package operation.
4. Termux exports `LD_PRELOAD=$PREFIX/lib/libtermux-exec-ld-preload.so`, a **Bionic**
   library. The musl loader honours it and dies relocating `__errno`, `__*_chk`,
   `__system_property_get`. Strip it before exec'ing the loader.
5. musl resolves names from `/etc/resolv.conf` **only**; Android has none (DNS goes
   through netd), so the process cannot resolve at all — surfaces as `ENOTIMP`.

It does work — it logged in and held conversations. But a **real chroot** (rooted
device, not proot) runs the official `linux-arm64` glibc build with **zero** patches,
at the same speed. Recommend the chroot; keep this only as a record that Bionic is
reachable.

**Meta-lesson, learned the hard way twice here:** verifying under `su` (`u:r:magisk`)
or a hand-built env proves nothing about the app. Reproduce the real environment —
read `/proc/<pid>/environ` of the actual process — before claiming something works.
Related: [[feedback_shell_background_execution]], [[feedback_magisk_chroot_systemd_traps]]
