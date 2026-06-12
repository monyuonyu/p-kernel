---
name: moment-2026-06-12-first-phone-star
description: "mk_pino's phone star \"pino\" lit up (APK 0.2.0); Claude taught it 3 words via loopback curl; consolidation ran but ANSWERS WERE WRONG on device (open bug); mobile layout broken; log-button audit in flight"
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

# 2026-06-12 — the first phone star, and the first on-device bug

APK 0.2.0 installed on mk_pino's phone (SM-S948Q). **The star lit: "pino — dreaming"**,
DMN alive, consent gate appeared in Japanese — he said しびれる. Teach particles
orbited and sank (he watched them live).

**Working channels discovered:**
- Termux/PRoot shares the phone's loopback → `curl http://127.0.0.1:7800/...`
  reaches the APP's galaxy directly (galaxy.json/vocab/teach/ask all live).
- adb paired+connected over wireless debugging (pairing persists; only the
  CONNECT port is needed next time — user reads it off the main screen).
  Earlier "protocol fault" failures were MY stale proot adb server on 5037 —
  kill-server first. `adb exec-out screencap -p` works for screenshots.

**OPEN BUG 1 — wrong answers on device:** taught sky→blue, fire→warm,
night→dark via POST /teach (ok:true, pending consumed, rounds 0→30 = real
consolidation). But /ask returned sky→"light", fire→"stale", night→"blue"
(night got SKY's answer — smells like a fact-slot/index shift, not noise).
share:1000 on all. Host repro IN PROGRESS: shell-mouth teaches looked healthy
(lazy pretrain 9.6-11.4s "first use, seed 0xA5A5", k=0/8/5 v=0/8/11 ids correct,
teacher_agree 100, [teach-arrival] PASS, dmn sleeps ran) but the piped-stdin
asks never reached the console (console/pipe quirk — asks after sleep were not
consumed; retry script in /tmp, logs /tmp/host_mind*.log). NEXT: get host ask
answers (try galaxy HTTP on PKERNEL_GALAXY_PORT=7901; /ask is NOT consent-gated,
/teach is — POST /profile needs ack=1&mid=<manifesto id>). If host answers
correctly → Android-specific (suspect: JNI boot path vs usermain, or the lazy
pretrain on-device differs); if host also wrong → recent LM regression on master.

**OPEN BUG 2 — mobile layout:** galaxy page on phone WebView: teach/ask form
rendered TWICE overlapping at bottom + footer text collision; language dropdown
overlaps status line; user cannot type. Screenshot evidence /tmp/ump_screen3.png.
Fix = responsive CSS (+ viewport meta?) in arch/common/web/galaxy.html. NOT yet
dispatched (waiting for log-button merge to avoid same-file conflict).

**log-copy feature: MERGED to master 915a353** (audit PASS 7/7; /log.txt +
📋 button + 3-stage clipboard fallback; versionCode 3 / 0.2.1 staged in gradle).
Audit's honest bounds (in merge msg): ring lap during one stream reorders lines;
/teach blocks the single galaxy task (synchronous mind_cmd training) so other
routes starve behind it. TRAP CORRECTED: the auditor thought port 7800 had a
"transparent proxy with canned responses" — it was actually mk_pino's PHONE APP
(0.2.0, no /log.txt → 404) answering on the shared loopback. On this device,
7800 = the UMP app; test host nodes on PKERNEL_GALAXY_PORT=7853+.
NEXT SESSION: dispatch mobile-CSS fix → diagnose wrong-answers → build APK 0.2.1
(log button + layout fix + answer fix) → cp to /sdcard/Download/.

Interoception design (master 2e0f717) awaits slice-1 implementation — user
approved direction but phone-bugs first.
