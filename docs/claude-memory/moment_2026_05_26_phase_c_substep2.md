---
name: moment-2026-05-26-phase-c-substep2
description: "2026-05-26 (capstone) — Phase C sub-step 2: first end-to-end UMP APK built. 3.5 MB, arm64-v8a, libpkernel.so + Phase B v2 relay client inside an Android Foreground Service with charge-only default. Real NDK r26d build via qemu-x86_64 wrappers under aarch64 PRoot. Commit 60e1dc3."
metadata: 
  node_type: memory
  type: project
  originSessionId: a4128c94-5332-49d8-ac74-41541259901a
---

# 2026-05-26 (capstone) — Phase C sub-step 2 (first UMP APK)

The day that started with "Phase B v2 進めよう" ended with a
sideloadable Android APK that turns a phone into a UMP node. Four
commits, one closed loop.

## What landed (commit 60e1dc3)

- **AndroidManifest** — INTERNET / FOREGROUND_SERVICE (+ Android 14
  typed-FGS DATA_SYNC) / POST_NOTIFICATIONS / WAKE_LOCK perms;
  MainActivity launcher, PKernelService (foregroundServiceType=
  dataSync).
- **MainActivity.kt** — four EditTexts (node_id + relay host/port/key),
  Start/Stop, scrollable green-on-black log view that drains kernel
  stdout every 250 ms via PKernelService.drainLog().
- **PKernelService.kt** — charge-only by default
  (BatteryManager.EXTRA_PLUGGED gate; auto-starts on
  ACTION_BATTERY_CHANGED when external power lands), posts a sticky
  notification, calls PKernel.bootWithRelay() on a worker thread,
  ring-buffers the last 64 KB of stdout.
- **Gradle** — AGP 8.5.2 / Kotlin 1.9.24 / gradle 8.7 / JDK 17,
  compileSdk 34, minSdk 26 (Oreo, required for FGS), arm64-v8a only,
  ndkVersion 26.3.11579264.
- **CMake fixes** — off-by-one PK_ROOT (Phase A bug; never hit before
  because Phase A used host .so, not NDK), explicit source lists
  (file(GLOB) had pulled in T-Kernel-only deviceio.c etc.),
  -idirafter for lib/libc (was leading -I, shadowed T-Kernel errno.h
  with the POSIX stub → broke E_OK in every kernel/common file),
  FORTIFY_SOURCE=0 (limits.h shadow lacks SSIZE_MAX).
- **Bionic adapters** — `__errno_location()` shim in pkernel_jni.c
  forwards to Bionic's `__errno()`; in sio.c, `#ifdef __ANDROID__`
  declares errno + EINVAL manually before <termios.h> (Bionic's
  termios_inlines.h uses them textually at parse time).

## The build pipeline (how this even happened in PRoot)

aarch64 Ubuntu 25.10 PRoot has no native Android NDK and binfmt_misc
is unwritable. Got to APK anyway:

1. apt install openjdk-21-jdk-headless (~94 MB)
2. Download Android cmdline-tools (~150 MB), sdkmanager auto-accepts
   licenses, installs platform-34 + build-tools 34.0.0 + ndk
   26.3.11579264 + cmake 3.22.1 (~3 GB total — all x86_64 binaries).
3. Manually extract zlib1g_*.deb (apt's amd64 multiarch fails on ports
   mirror; pull from archive.ubuntu.com) for libz.so.1 in
   /usr/x86_64-linux-gnu/lib/.
4. wrap_x86_64.sh — walks NDK, build-tools, platform-tools, cmake bins;
   replaces each x86_64 ELF with a /bin/sh wrapper that calls
   `qemu-x86_64 -0 "$0" -L /usr/x86_64-linux-gnu "$0.real" "$@"`.
   The `-0` preserves argv[0] for multi-call binaries like clang vs
   clang++. POSIX-sh-safe (no `exec -a` — that's bash-only).
5. android.aapt2FromMavenOverride pinned to the SDK's aapt2 because
   AGP re-extracts its bundled aapt2 from a JAR on every build, undoing
   the wrap.
6. AGP also bundles a separate aapt2 in ~/.gradle/caches/transforms-4/
   — wrap that too. (Also: wrap_x86_64.sh needed `-perm -u+x` not
   `-perm -111`; AGP's aapt2 ships mode 744.)

End result: `./gradlew assembleDebug` works under PRoot. NDK clang
compiles native via qemu (slow but works).

## End-to-end evidence

```
$ unzip -l app-debug.apk | grep pkernel
  279648  ...  lib/arm64-v8a/libpkernel.so

$ file libpkernel.so
ELF 64-bit LSB shared object, ARM aarch64, version 1 (SYSV),
dynamically linked, for Android 26, built by NDK r26d (11579264)

$ aapt2 dump badging app-debug.apk | head -3
package: name='io.pkernel.ump' versionCode='1' versionName='0.1.0-phase-c'
sdkVersion:'26'
targetSdkVersion:'34'

$ llvm-nm -D libpkernel.so | grep -E 'native|pkernel_main'
T Java_io_pkernel_PKernel_nativeBoot
T Java_io_pkernel_PKernel_nativeConfigureRelay
T pkernel_main
```

A real APK that an Android 8+ phone can install and tap. Hold the
relay key on a server, hand the same key to the phone via QR/paste,
press Start, watch SWIM gossip + K-DDS pub/sub flow over the relay.

## Decisions worth remembering

- **Wrap each x86_64 binary individually, don't try to fix
  binfmt_misc.** PRoot can't mount binfmt_misc writable; wrappers
  work and are debuggable. wrap_x86_64.sh lives at /root/ — re-run
  it after every sdkmanager install (it's idempotent).
- **qemu-x86_64 `-0` over POSIX sh `exec -a`.** dash (default /bin/sh
  on Ubuntu) has no `exec -a`. qemu's `-0` does the same job inside
  the emulator.
- **android.aapt2FromMavenOverride is unstable but necessary.** AGP
  loudly warns it's experimental. Without it, every gradle build
  re-extracts a fresh unwrapped aapt2 from the bundled jar.
- **lib/libc must be a -idirafter fallback, not a leading -I.** Same
  as host build's KERNEL_CFLAGS ordering. As a leading -I, the
  POSIX errno.h stub shadowed T-Kernel's errno.h → E_OK etc. became
  undeclared identifiers across every kernel/common .c. Cost an
  hour of debugging.
- **__errno_location → __errno is a 2-line shim.** Don't rewrite
  arch/linux/* sources to be Bionic-aware — just provide the symbol
  Bionic doesn't have.

## Day total

Four commits, each a coherent logical unit:

1. **f3e0c04** — relay v2 wire (HMAC + replay window)
2. **7cb290e** — kernel-side relay client transport (executable build)
3. **9dd4cc8** — same transport in .so / Android NDK CMake build
4. **60e1dc3** — first end-to-end UMP APK (Activity + FGS + charge-only)

The arc from "no auth on the wire" to "tap-installable Android node"
in one day. Phase C still has sub-step 3 (Dashboard/stats) and sub-step
4 (Play Store metadata), but those are polish on a working product.

## Cross-links

- [[moment-2026-05-26-phase-b-v2]] — auth wire this app uses.
- [[moment-2026-05-26-phase-b-substep2]] — client transport ditto.
- [[moment-2026-05-26-phase-c-substep1]] — JNI bridge wiring this APK
  picks up.
- [[moment-2026-05-22-android-phase-a]] — original dlopen smoke that
  proved the .so loads on Bionic; still load-bearing.
- [[project-ump-android-node]] — strategic roadmap. Phase C is now
  *partially* shipped (real APK), not just designed.
