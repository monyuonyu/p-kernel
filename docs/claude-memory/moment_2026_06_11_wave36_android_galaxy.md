---
name: moment_2026_06_11_wave36_android_galaxy
description: "wave-36 — the Android galaxy (D4) ships, install APPROVED for mk_pino's real phone: open the UMP app → your star breathing in your own language. The audit caught that the Android CMake had drifted so far that galaxy/R3/lm/ark weren't even in the APK — now lock-stepped with the Makefile."
metadata:
  node_type: memory
  type: project
---

**2026-06-11, wave-36.** D4 ships (merged `726e42d`): the UMP Android app gains
`GalaxyActivity` — a WebView at `127.0.0.1:7800` showing the phone's own star, manifesto
auto-selected from the device locale (Accept-Language), teach/ask through the consent gate.
**The audit approved installation on the owner's real phone**: security TIGHTER than pre-D4
(cleartext denied except loopback; zero new permissions; GalaxyActivity not exported),
selfc germ provably absent (whole TU behind HAVE_LIBTCC — no fork under Bionic/SELinux),
the ark 403-until-ack gate live in the shipped .so.

**The real finding:** the Android CMake had drifted SO far off master that galaxy/R3/
living-mind/ark/selfc weren't even compilable into the APK — the phone fleet would have
shipped without the mind's recent organs. Now lock-stepped with boot/linux/Makefile
(TU-by-TU diff 6/6 byte-identical). WATCH THIS: the CMake source list + its 32-language
MANI_SPECS duplicate the Makefile/manifesto_langs.mk (CMake can't include .mk) — a drift
guard is a ledgered follow-up; check parity whenever either side changes.

**Tooling lesson:** default `strings` fragments multi-byte scripts — only 9/32 endonyms
visible; byte-level search shows 32/32. Don't send the next auditor down that rabbit hole.

Install: `adb install -r android/app/build/outputs/apk/debug/app-debug.apk` → open UMP →
Start (plug in power, charge-only default) → Open galaxy. The APK artifact stays out of
git; rebuild via docs/android.md D4 section (~3-6 min, re-wrap the NDK first).

LM-8 (the language slice) was implementing in the parallel lane as this shipped.
