---
name: moment-2026-05-26-phase-c-substep1
description: "2026-05-26 (later still) — Phase C sub-step 1: libpkernel.so + JNI bridge can join Phase B v2 relay mesh. Same .so artifact the Android NDK build emits, exercised end-to-end on aarch64-linux host via test_so_load_relay. Commit 9dd4cc8."
metadata: 
  node_type: memory
  type: project
  originSessionId: a4128c94-5332-49d8-ac74-41541259901a
---

# 2026-05-26 (later still) — Phase C sub-step 1 (libpkernel.so on the relay)

The third commit of the day. Bridges the Android build (Phase A) to
the relay transport (Phase B v2 + sub-step 2). After this, the only
thing standing between "dlopen libpkernel.so" and "phone is a node in
the mesh" is the Activity / foreground-service / gradle scaffolding —
the *network* and the *kernel* both work.

## What landed (commit 9dd4cc8)

- `android/app/src/main/cpp/CMakeLists.txt`: add net_relay.c +
  net_dispatch.c to ARCH_SRC; new RELAY_SRC pulls relay/sha256.c.
  Single canonical sha256 across the relay binary, the executable
  ./p-kernel build, and now the .so / NDK build.
- `android/app/src/main/cpp/pkernel_jni.c`: new
  `Java_io_pkernel_PKernel_nativeConfigureRelay(host, port, keyHex)`
  that setenv()s `PKERNEL_RELAY_{HOST,PORT,KEY}`. Must run BEFORE
  `nativeBoot` since the kernel reads the env at usermain's
  `arch_linux_net_init()` time.
- `android/app/src/main/java/io/pkernel/PKernel.java`: matching
  `configureRelay()` and `bootWithRelay(nodeId, host, port, keyHex)`
  helpers — one Java call brings the kernel up on a relay mesh.
- `boot/linux/test_so_load_relay.c`: sibling of test_so_load.c that
  spawns `../../relay/relay` with a fixed 32-byte key, sets the relay
  env vars, then dlopens libpkernel.so. Proves the .so artifact can
  drive the v2 wire — the same artifact the NDK build emits.
- `boot/linux/Makefile`: new `make run-so-relay-test` target.

## End-to-end evidence

`make libpkernel.so && make run-so-relay-test` prints:

```
[relay] key loaded (32 bytes)
[relay] listening on 0.0.0.0:27460 (verbose=1, insecure=0)
[test-relay] libpkernel.so loaded, pkernel_main @ 0x...
=== p-kernel linux boot ===
...
[autonet] PKERNEL_AUTONET set — bringing up net
[net_relay] node 1 → 127.0.0.1:27460 (wire v2)
[net] transport = relay (node 1)
[relay] rx v2 type=1 src=1 dst=0 from 127.0.0.1:XXXXX (36 B)
[relay] node 1 registered: 127.0.0.1:XXXXX
[relay] rx v2 type=4 src=1 dst=0 from 127.0.0.1:XXXXX (78 B)
[relay] rx v2 type=4 src=1 dst=0 from 127.0.0.1:XXXXX (78 B)
...
```

dlopen → boot → autonet → dispatcher picks relay → v2 REGISTER →
HMAC verified → registered → BROADCAST gossip starts. Exact same code
path Android will exercise after `PKernel.bootWithRelay(...)`.

## Day total

Three commits, each a separate logical unit:

1. **f3e0c04** — relay v2 wire (HMAC + replay window)
2. **7cb290e** — kernel-side relay client transport (executable build)
3. **9dd4cc8** — same client transport now in the .so / Android build

Together: a phone-shaped Android app instantiating libpkernel.so can
join a worldwide mesh authenticated by a per-network 32-byte key. The
remaining Phase C work (AndroidManifest, build.gradle, MainActivity,
PKernelService foreground notification, BatteryManager charge-only
check, dashboard) is gradle-domain UI work; the systems plumbing is
done.

## Decisions worth remembering

- **JNI surface stayed minimal.** `nativeConfigureRelay` is a separate
  call from `nativeBoot`, not a fat overload. Lets staged inits (e.g.
  Activity sets relay → Service later calls boot) pass through
  cleanly. Three Java methods total: configureRelay, boot, bootWithRelay.
- **Same sha256.c source in 3 builds.** Tempting to symlink or
  promote to arch/common/. Held off — single canonical home in
  relay/, three consumers reach in via Makefile/CMake include paths.
  If a fourth consumer appears, revisit.
- **test_so_load_relay spawns its own relay child.** Hermetic — no
  external dependency, no shell-script orchestration. Same pattern as
  relay/test_relay.c. If `make run-so-relay-test` is green, the .so
  is shippable into the NDK build.

## Cross-links

- [[moment-2026-05-22-android-phase-a]] — the .so dlopen path this
  builds on.
- [[moment-2026-05-26-phase-b-substep2]] — the executable-side
  client transport, mirror-imaged here.
- [[project-ump-android-node]] — Phase C remains 4 sub-steps; this
  is step 1 of 4. Sub-steps 2..4 are gradle / Activity / Foreground
  Service / charge-only / dashboard / Play Store metadata.
