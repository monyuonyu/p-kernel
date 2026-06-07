# Code-side notes (formerly the inner code README)

> **This file used to be the in-tree code README** (`p-kernel/p-kernel/README.md`
> before the de-nest). It predated waves 7–16 and made several claims that are
> now wrong — "the AI is never trained", "the OS compiles itself", "x86_64-linux
> is in progress", `DNODE_MAX 8`, nested `p-kernel/p-kernel/` paths. Rather than
> keep a second, stale marketing README around, it has been **trimmed to a
> pointer**. The single source of truth for "what works / what is designed /
> what is vision" is the root README, kept deliberately honest.

## Where to look

| You want… | Go to |
|-----------|-------|
| The honest status table (works / in-flight / vision) | [`/README.md`](../README.md) — repo root |
| The architecture map + per-doc honest-status sections | [`docs/architecture/README.md`](architecture/README.md) |
| Run p-kernel as a Linux process in 60 s (UMP) | [`docs/quickstart.md`](quickstart.md) |
| Shell command cheatsheet (bare-metal x86) | [`docs/cheatsheet.md`](cheatsheet.md) |
| Android APK build | [`docs/android.md`](android.md) |
| Raspberry Pi 3B+ bare-metal netboot | [`docs/netboot.md`](netboot.md) |
| The NAT-traversal relay (v2 wire) | [`docs/phase_b_relay.md`](phase_b_relay.md) |
| Benchmarks (latency / locality / survival) | [`docs/benchmarks/`](benchmarks/) |

## Repository layout (de-nested — source lives at the repo root)

```
arch/        per-arch HAL. arch/x86 (bare metal), arch/aarch64 (bare metal,
             incl. a virtio-blk driver), arch/linux/{aarch64,x86_64}
             (userspace UMP), arch/common (AI + distributed layer, shared)
boot/        build + run per target: x86, aarch64, linux, linux_x86_64
             (plus partial h8300, rl78)
kernel/      micro T-Kernel 2.0 core (kernel/common)
lib/         support libraries
relay/       the NAT-traversal relay server (v2 wire) + tests
samples/     numbered demos / kill-tests (01_… through 33_ark_aarch64)
tools/       sim + tooling (e.g. tools/sim/latency_twolayer_sim.py)
android/     in-repo Gradle + NDK project for the UMP APK
docs/        all documentation (this file lives here)
```

## Component READMEs (in the source tree)

| Topic | File |
|-------|------|
| x86 / QEMU build + run | [`boot/x86/README.md`](../boot/x86/README.md) |
| x86 architecture + TCP/IP stack | [`arch/x86/README.md`](../arch/x86/README.md) |
| T-Kernel core API reference | [`kernel/common/API_REFERENCE.md`](../kernel/common/API_REFERENCE.md) |
| Kernel core modules | [`kernel/common/README.md`](../kernel/common/README.md) |
| Sample index | [`samples/README.md`](../samples/README.md) |

---

> If anything here ever contradicts the root [`README.md`](../README.md) or the
> [architecture map](architecture/README.md), **those win** — fix this file (or
> delete the contradiction) rather than letting two stories diverge again.
