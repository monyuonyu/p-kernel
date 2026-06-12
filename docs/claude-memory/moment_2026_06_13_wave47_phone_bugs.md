---
name: moment-2026-06-13-wave47-phone-bugs
description: wave-47 — phone wrong-answers root cause = Android -O2 vs certified -O1 (host -O2 segfaults at boot); mobile dock fix; vocab_fp guard; 32-lang entrance; APK 0.3.0; cure pending on-device
metadata:
  type: project
---

# 2026-06-13 — wave-47: the phone's first day, debugged honestly

Three falsifications then the kill: persisted state (ZERO files in the app dir —
and "pino"+consent vanish on restart: Android persists NOTHING, ark follow-up),
choreography (exact aborted-curl/dup-sky host repro answers correctly), hardware
(Termux host on the SAME phone correct). ROOT CAUSE: Android CMake built the
shared code at **-O2** while every certified build is **-O1**; flipping the host
to gcc -O2 SEGFAULTS deterministically right after "[BOOT] Starting T-Kernel..."
(auditor 4/4). Optimizer-unsafe code (suspect: volatile/clobbers around the
hosted ctx switch). Android now -O1. **Cure on-device EXPECTED NOT PROVEN** —
mk_pino's teach/ask on 0.3.0 is the missing cert.

Also shipped: mobile #dock layout (3 fixed bottom elements overlapped on narrow
WebView; overlap=0 measured headless at 412×915), Path E vocab_fp[8] guard
(MT_WIRE_VER_VOCAB — a REAL adjacent hole the lane found while chasing the wrong
suspect; the phone was peerless so it wasn't the cause), 32-language entry
screen (StarfieldView, 「光れ」hero button, taglines verbatim from manifesto
translations, ui_strings.tsv+gen_strings.py, b+zh+Hans/Hant, RTL).

Lessons: (1) certs only certify the BUILT artifact — the NDK binary had never
run one; flag parity belongs next to TU parity. (2) A lane can find a real bug
that is NOT the bug — check the found mechanism against the evidence's
preconditions (peers:[] killed the vocab story for the phone). (3) adb pair
"protocol fault" = kill the proot-side adb server first; loopback is shared
with the phone app (7800 = HIS star; test on 7853+); pkill -f self-matches.

Follow-ups: -O2-clean wave; Android persistence (getFilesDir → p-fs/ARK);
MANI_SPECS lang guard; sample 41 wire-name string.
