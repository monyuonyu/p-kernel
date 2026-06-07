# Building UMP as an Android app — Phase A

This is the on-ramp for distributing p-kernel via Google Play. The kernel
itself already runs end-to-end as a Linux process on aarch64; this
document covers the wrapping that turns `./p-kernel` into `libpkernel.so`
inside an APK.

Status (updated 2026-06-07): **the full Gradle/NDK project now lives
in this repo** under `android/` — `build.gradle.kts`, `settings.gradle.kts`,
`gradlew`, and `app/src/main/{cpp,java}/…` including the JNI bridge,
`CMakeLists.txt`, `PKernel.java`, `MainActivity.kt`, and the
`PKernelService.kt` foreground service. Phases A→D have shipped: a real
installable `app-debug.apk` builds via official NDK r26d
(`./gradlew :app:assembleDebug`), boots `libpkernel.so`, joins a v2
relay mesh, and is region-aware (see the Phase D section below). The
sections below are kept as the build walkthrough; the "Known issues"
list records what was fixed along the way.

## Prerequisites

- A Linux machine (or macOS) with **Android Studio** + **NDK r25c** or
  newer. The home Ubuntu server in [[project-ump-android-node]] is the
  intended build environment.
- This repository cloned somewhere accessible to the Studio project.

## Project skeleton

The simplest path is to **open the in-repo `android/` project directly**
in Android Studio (or build headless with `./gradlew :app:assembleDebug`).
The minimum is API 26 / Android 8.0. The notes below document what that
project's `app/build.gradle.kts` contains, in case you are reconstructing
it or wiring an equivalent project by hand.

`app/build.gradle.kts` needs:

```kotlin
android {
    namespace = "io.pkernel"
    ndkVersion = "25.2.9519653"     // or whatever you have
    defaultConfig {
        externalNativeBuild {
            cmake { arguments += listOf("-DANDROID_ABI=arm64-v8a") }
        }
        ndk { abiFilters += "arm64-v8a" }    // 32-bit ABIs not supported yet
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}
```

`AndroidManifest.xml` needs INTERNET permission (for the relay /
loopback network) and a foreground service declaration (so the kernel
keeps running while the screen is off):

```xml
<uses-permission android:name="android.permission.INTERNET"/>
<uses-permission android:name="android.permission.FOREGROUND_SERVICE"/>
<uses-permission android:name="android.permission.POST_NOTIFICATIONS"/>

<service
    android:name=".KernelService"
    android:foregroundServiceType="dataSync"
    android:exported="false"/>
```

## What the JNI bridge does

`pkernel_jni.c` exposes three native methods:

- `nativeBoot(nodeId)` — Sets `PKERNEL_NODE_ID` + `PKERNEL_AUTONET=1`
  in the process environment, opens two pipes (stdin / stdout), spawns
  a pthread, and inside that thread `dup2`s the pipe ends to FD 0/1/2
  before calling `pkernel_main()` (which is just our existing `main()`
  renamed via `-Dmain=pkernel_main` to avoid colliding with JNI's
  own entry-point conventions).
- `nativeReadStdout(buf, maxlen)` — Pulls captured stdout bytes for the
  UI's TerminalView.
- `nativeWriteStdin(buf, len)` — Feeds keystrokes from the UI's input
  field into the kernel's `sio_read_line`.

The kernel has no awareness of being inside an app; from its point of
view it's just running on aarch64 Linux with stdin/stdout being a pipe.

## Build steps

1. Open the Studio project, sync gradle.
2. `Build → Make Project`. Errors here are usually missing headers —
   confirm `PK_ROOT` in `CMakeLists.txt` resolves to the repo root
   (the `../../../../../..` chain is correct when the CMakeLists is at
   `android/app/src/main/cpp/`).
3. `Run → Run 'app'` on a connected aarch64 device or emulator.
4. First boot: you should see the same banner as `./p-kernel` on Linux,
   inside the TerminalView.

## Host-side smoke test (`make libpkernel.so`)

`boot/linux/Makefile` now has a `libpkernel.so` target that builds the
same kernel sources as a `.so` using exactly the same flag set the
Android CMakeLists.txt uses (`-fPIC -fvisibility=hidden`, with
`-Dmain=pkernel_main` so the JNI bridge can call it). Plus a
`test_so_load.c` that `dlopen`s the .so and runs `pkernel_main` in a
thread — the host-side equivalent of `System.loadLibrary("pkernel")`
followed by `nativeBoot(1)`.

```sh
$ make libpkernel.so
$ make run-so-test     # dlopens, runs to the [autonet]/banner
```

Use this before pushing changes that touch the kernel — it catches
PIC-incompatibility issues without needing an Android device or NDK.

## Known issues (Phase A → Phase B)

- ~~**PIE/PIC**: hang after `[BOOT] Starting T-Kernel...`~~ **RESOLVED 2026-05-22.**
  Root cause was not the dispatcher (`adrp + add :lo12:` is in fact
  PIC-safe for same-DSO symbols) but `CFN_REALMEMEND = 0x50000000UL`
  in `utk_config_depend.h`. Under PIE/ASLR the BSS `linux_heap[]` lands
  at a much higher virtual address (e.g. `0x7f8000000000`), so the
  `if (CFN_REALMEMEND > knl_lowmem_limit) memend = knl_lowmem_limit`
  clamp in `knl_init_Imalloc` never fires, `memsz` wraps to garbage,
  and the allocator faults on first use. Fix: `arch/linux/include/
  utk_config_depend.h` shadows the bare-metal version and sets
  `SYSTEMAREA_END = ~0UL`, guaranteeing the clamp always fires on hosted.
  Also `arch/linux/aarch64/cpu_support.S` adds `.hidden knl_taskmode`
  so the linker accepts its `adrp + add` access pattern when the TU
  is included in a shared library.
- **Background lifecycle**: The Activity launches `nativeBoot` but
  Android will kill the process within minutes of the user leaving the
  app. The Foreground Service stub above is the workaround; the kernel
  thread should be created from `Service.onStartCommand`, not the
  Activity.
- **Battery**: the SIGALRM at 10 ms cadence is fine for active use,
  but for "passive node" mode the user should default to `Allow only
  while charging`. See [[project-linux-userspace-time-domain]] for
  NO_HZ-style tickless idle to address this.
- **NAT traversal**: two phones on cellular won't reach each other via
  127.0.0.1. Phase B replaces `net_unix.c`'s UDP loopback with a relay
  server (one tiny public IP). Until then the demo is "two emulators
  on one host" or "two devices on the same Wi-Fi LAN with a tiny
  STUN/relay component."

## Play Store path

This is a hosted-RTOS distribution. Google's review process should
treat the APK as an ordinary NDK app (no code executed from `/data`,
no ARM64 jit, no APIs they restrict). The honest user-facing
description is essential:

> p-kernel is research / hobby OS that uses a small slice of your
> phone's CPU and network to participate in a distributed AI fabric.
> It runs only while your phone is charging by default. You can
> disable participation at any time.

F-Droid (direct APK install) is the long-term distribution backup so
the project doesn't depend on a single store.

## Phase D entry — the APK becomes region-aware (2026-06-02)

The first installable APK (Phase C) predated the regions work, so it was
region-blind: `android/app/src/main/cpp/CMakeLists.txt` keeps its own
explicit source list (separate from the four `boot/*/Makefile`s) and
`region.c` had never been added to it. Once `capacity(N)` wired
`degrade.c` to `region_size()` / `region_id()` / `region_coordinator()`,
that omission would have failed the link with undefined `region_*`.

Fixed by adding `${ARCH_CO}/region.c` to `COMMON_SRC`. Verified end to end:

- `region.c`, `degrade.c`, `dkva.c` compile cleanly with NDK r26d's
  `aarch64-linux-android26-clang` (Bionic).
- `./gradlew :app:assembleDebug` → `app-debug.apk` (BUILD SUCCESSFUL).
- The packaged `lib/arm64-v8a/libpkernel.so` contains the `[region]` and
  `[capacity]` log strings — i.e. the regional fabric is really in the APK.

A phone running this APK is now a first-class regional node. Because the
RTT-zone simulator (`PKERNEL_RTT_ZONE_*`) is opt-in via env vars that the
app never sets, a phone forms regions from **measured** SWIM RTT — the
real-world latency that, by design, only a physical fleet can provide.
The in-app TerminalView can already drive `region` and `dist` (capacity)
since the JNI bridge is just stdin/stdout; surfacing region/capacity in
the Foreground Service notification (the "contributing X" dashboard) is
the next UX step.

### What Phase D still needs (operational, not code)

- A public relay reachable by phones behind NAT (Phase B relay, hosted).
- Real devices installing the APK + joining that relay.
- τ tuned from the fleet's measured cross-device RTT distribution.
