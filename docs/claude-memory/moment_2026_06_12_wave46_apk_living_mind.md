---
name: moment-2026-06-12-wave46-apk-living-mind
description: wave-46 — APK 0.2.0-living-mind ships; drift was exactly 4 linchpin TUs; check_parity.sh guards recurrence; docs/demo-fleet.md = the 2-phone demo
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

# 2026-06-12 — wave-46: the living mind ships in the APK (届ける)

mk_pino picked "deliver it to people" over scale/fleet/evolution. The wave-36
CMake drift had re-accumulated by EXACTLY 4 TUs — `r3_vocab.c`, `ed25519.c`,
`sign.c`, `sign_entropy.c` — and they were linchpins: mind/PathW/galaxy organs
were already in the CMake list but INERT without them. APK **0.2.0-living-mind**
(versionCode 2, 3.9 MB) now carries real words, signing, Path W/W², 32 manifesto
endonyms (byte-verified in the packaged .so), selfc/tcc provably absent on
Bionic (zero defined AND undefined refs).

**Recurrence guard:** `tools/android/check_parity.sh` diffs android CMake vs
`boot/linux/Makefile` TU lists both directions. The auditor sabotaged it 3 ways
(removed TU, phantom TU, deleted Makefile line) — all caught. Audit PASS with
parity re-derived independently + host smoke re-run.

**Named next drift surface (auditor's true-but-unstated catch):** the
32-language `MANI_SPECS` block in CMakeLists is a hand-kept duplicate of
`manifesto_langs.mk` and is NOT guarded — a future language add would silently
not embed. Same bug class as wave-36, one layer down.

**The demo:** `docs/demo-fleet.md` — relay (`PKERNEL_RELAY_KEY` 64-hex, -p 7777)
+ 2 phones (MainActivity fields: node id / relay host:port / key) → teach on A's
galaxy → particle sinks at the real DMN tick → B answers AND names the teacher →
kill A, B still answers. Hosted node joins via PKERNEL_NODE_ID/RELAY_* envs.

APK artifact preserved at `/root/ump-0.2.0-living-mind.apk` (worktree was
cleaned). On-device verification (install/upgrade/WebView/consent on Bionic)
remains for mk_pino's phone — host smoke was the documented stand-in.
