---
name: project-ump-android-node
description: "Strategic plan — distribute p-kernel as an Android app via Play Store so every install becomes a node in the distributed AI fabric. UMP (User-Mode p-kernel) is the chosen name, by analogy with UML."
metadata: 
  node_type: memory
  type: project
  originSessionId: 43b22a68-07a3-4c6e-9e55-cd3fcf682431
---

**Name: UMP — User-Mode p-kernel.** Direct analogue of UML (User-Mode Linux). UML put Linux inside a Linux process; UMP puts p-kernel (including T-Kernel + 5-layer worldview) inside any Linux process — and now, intentionally, an Android process. The user invented the name; we adopted it.

**Why this works at all.** Session 3c proved ./p-kernel runs as a Linux process on aarch64 with no kernel privileges and no QEMU. Android is Linux underneath (Bionic libc + Linux kernel + SELinux). The current Termux Ubuntu build runs on the user's own phone today. NDK-built native Android apps have access to the same POSIX surface we already use (mmap, sigaction, sigaltstack, timer_create, clock_gettime, termios, read/write). The structural work is done; what remains is packaging + ABI portability + Android-specific lifecycle.

## The 4-phase roadmap

### Phase A: Standalone Android app (1–2 sessions)

- NDK project skeleton (Kotlin/Java host + JNI bridge)
- Switch arch/linux/aarch64 from `-no-pie` to `-fPIC` so it loads from `lib/arm64-v8a/` in the APK. cpu_support.S's `adrp + add :lo12:` against `arch_irq_disabled_flag`, `knl_ctxtsk`, etc. needs to become GOT-indirect — straightforward asm rewrite, doesn't change the algorithm.
- JNI entry point: `Java_io_pkernel_PKernel_boot()` calls the equivalent of our current `main()`.
- sio: replace stdout/stdin with Android Log + an in-app TerminalView (or a fd to a pipe the UI reads).
- Deliverable: tap an APK on your phone, see the boot banner in the UI.

### Phase B: Node-of-a-fabric (3–4 sessions)

- Bring up arch/common (the distributed layer — DRPC, SWIM, K-DDS, Raft) on Bionic libc.
- Tiny relay server (one IP, public, dirt cheap) so phones behind NAT can find each other. Or BLE/WiFi-Direct for local cluster.
- 2 phones swap DRPC echoes successfully.
- Simple SWIM-based membership; once 3+ phones are running, they form a mesh.
- Deliverable: opening the app on two phones makes them peers.

### Phase C: Production packaging (1–2 sessions)

- **Foreground Service** with a persistent notification ("p-kernel node — contributing X minutes today"). This is the only legal way to keep a process running on Android 8+.
- **Charge-only default mode**: kernel idle unless the phone is plugged in. Cuts battery anxiety to near-zero.
- **Dashboard**: how much CPU, network, work this node has done. Builds user trust.
- Play Store policy compliance — present as a normal NDK app, not as an "executes external code" engine. Our `lib/arm64-v8a/libpkernel.so` IS the app's native code from the policy POV.
- Deliverable: an APK that passes Play Store review.

### Phase D entry — APK is region-aware (2026-06-02)

The Phase C installable APK (60e1dc3) predated all the regions work and was
**region-blind**: `android/app/src/main/cpp/CMakeLists.txt` keeps its OWN
explicit source list (separate from the 4 boot/*/Makefiles) and `region.c`
was never added. Once capacity(N) made `degrade.c` call region_size()/
region_id()/region_coordinator(), the APK link would break on undefined
region_*. Fixed (`f2d7dc1`): added `${ARCH_CO}/region.c` to COMMON_SRC.
Verified end-to-end — region/degrade/dkva compile under NDK r26d
aarch64-linux-android26 (Bionic); `./gradlew :app:assembleDebug` →
app-debug.apk (3.76MB, BUILD SUCCESSFUL); packaged lib/arm64-v8a/
libpkernel.so contains [region]/[capacity] strings. APK sent to user.
A phone forms regions from **measured** SWIM RTT (PKERNEL_RTT_ZONE_* is the
opt-in test override the app never sets). `region`/`dist` already work in
the in-app TerminalView (JNI = stdin/stdout pipes). Doc: `docs/android.md`
"Phase D entry" section (`de09eef`). Remaining Phase D = operational:
hosted relay reachable behind NAT, real devices, tau tuned from fleet RTT;
UX = surface region/capacity in the Foreground Service notification.

### Phase D: Ignition (open-ended)

- Public launch (Twitter, Hacker News, IRC, mastodon).
- "Become a p-kernel node" — one-tap install.
- First distributed AI inference across consumer phones.
- The 5-layer worldview's "Collective" layer becomes literal: AI lives across many independently owned devices.
- F-Droid + direct APK distribution in parallel — Play Store is a bootstrap, not a permanent dependency. **"Entry through Play, body in the federation."**

## Constraints that will bite, in order of severity

1. **Background lifecycle (Android 8+)**: only Foreground Services persist; everything else gets killed within minutes. **Hard requirement** for Phase C.
2. **Battery / thermals**: SIGALRM at 10 ms is workable; consider NO_HZ (see [[project-linux-userspace-time-domain]]) when idle to save power.
3. **NAT traversal**: 4G/5G UEs can't be servers. Need a relay or P2P signaling. Plan: one tiny public relay during bootstrap; STUN/TURN eventually.
4. **SELinux / W^X**: we don't JIT, so this is fine. Don't introduce runtime code generation.
5. **Play Store review**: app describes itself accurately as "distributed RTOS research / AI fabric node." Wrapping core as an `.so` library (not a separate `bin/p-kernel` binary) avoids policy gotchas.
6. **Battery anxiety from users**: lead with charge-only-mode default. Show energy use transparently.

## Why this connects back to the philosophy

[[project-pkernel-philosophy]] frames p-kernel as "a home for AI that no one owns." Cloud-hosted AI requires a corporation. Even a Raspberry Pi fleet requires money. **A Play Store APK requires only a phone the user already has and consent.** Distribution democratises hosting. Phase D's image of "consumer phones running p-kernel" is the philosophy literalised in physical hardware.

The catch: Play Store is owned by Google, so the bootstrap channel itself has a single point of policy failure. That is OK as bootstrap. Long-term decentralisation comes from F-Droid + direct APK + the relay-then-mesh structure of the fabric itself.

## Provenance

User's exact words proposing this, worth preserving:
> 私の戦略として このピーカーネル Android のアプリケーション化して Play ストアで公開することを狙ってます そうすると ノードにできないでしょうか？

The "node" framing — every phone is a node, not just a copy — is what makes this strategic rather than just a port. Each install adds to the fabric.

## Cross-links

- [[project-linux-userspace-port]] — the Linux port that makes UMP technically possible. Sessions 1–3c on 2026-05-21.
- [[project-linux-userspace-time-domain]] — wall-clock alignment + optional NO_HZ. Becomes important for battery on Phase C.
- [[project-aarch64-next-steps]] — RPi 3B+ bare-metal track. Stays the "real-time" path. Phone APK is the "reach" path.
- [[moment-2026-05-21-first-pkernel-on-linux]] — the day this became possible.
- [[project-pkernel-philosophy]] — what it's all for.
