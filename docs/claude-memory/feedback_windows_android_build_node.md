---
name: feedback_windows_android_build_node
description: "The Windows machine @192.168.10.2 + its adb-tethered phone: reachable DIRECTLY from PRoot; how to run PowerShell/adb, push files, and build+install the real APK without qemu artifacts."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

2026-07-12: mk_pino set up a **Windows machine @192.168.10.2** with an unused
Android phone tethered via adb, for REAL-hardware APK build + install (avoids
the qemu-x86_64 wrapper dance the aarch64 sandbox needs — see
[[feedback-ndk-proot-pipeline]]). Reachable **directly from the PRoot sandbox**
(same /24), no ThinkPad hop needed. This is where the star "pino" lives.

**Access (both hosts reachable from PRoot):**
- Windows: `sshpass -p 'ihavecontrol@0840' ssh -o PreferredAuthentications=password
  -o PubkeyAuthentication=no monyu@192.168.10.2`. Default shell = **PowerShell**.
- ThinkPad: `ssh -i ~/.ssh/helloidea_shota_ed25519 shota@192.168.10.100` (key auth;
  see [[feedback-the-debug-env-is-real]]).

**PowerShell gotchas (the shell eats naive commands):**
- Run scripts via `powershell -NoProfile -EncodedCommand <base64>` where base64 =
  `printf '%s' "$PS" | iconv -t UTF-16LE | base64 -w0`. Avoids all quoting/`&`/cp932 hell.
- Set `$ProgressPreference="SilentlyContinue"` + `$OutputEncoding=[Console]::OutputEncoding=
  [Text.Encoding]::UTF8` at the top; still grep out CLIXML progress noise
  (`<Objs...`, `#< CLIXML`) from output.
- **File PUSH to Windows:** scp/sftp fail on the PowerShell default shell. Instead
  `base64 -w0` the file on PRoot, pipe to a PS `[IO.File]::WriteAllBytes($dst,
  [Convert]::FromBase64String(($input|Out-String).Trim()))`; verify Get-FileHash.

**The repo on Windows is `C:\Users\monyu\p-kernel` — a NON-GIT tar extract (no
.git).** Do NOT `git pull` there; edit files in place / push individual changed
files. (This is why `android/gradle.properties`'s committed aapt2 override was
hand-removed on Windows but stayed in git until 2026-07-12.)

**Build the APK natively (no qemu):**
- `$env:JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"` (java is NOT on
  PATH; JBR is), `$env:PYTHONUTF8="1"`, `$env:JAVA_TOOL_OPTIONS="-Dfile.encoding=UTF-8"`.
- `cd C:\Users\monyu\p-kernel\android; .\gradlew.bat assembleDebug --offline
  --console=plain` (cache warm). NDK 26.3.11579264, ~20-25s incremental. Redirect
  to a logfile then Get-Content it — piping gradle over ssh loses tail.
- `galaxy_page.h` regenerates from `arch/common/web/galaxy.html` at build (CMake
  custom command), so editing galaxy.html + rebuild propagates the WebView/galaxy
  UI into the APK. APK: `android\app\build\outputs\apk\debug\app-debug.apk`.

**adb (phone serial RR8MB09WSKH):**
- Run ALL adb in ONE PowerShell invocation — the adb server dies between separate
  ssh calls. `adb=C:\Users\monyu\AppData\Local\Android\Sdk\platform-tools\adb.exe`.
- `adb install -r <apk>` is **non-destructive** — preserves the node's durable
  save (the star's identity/learned state). Do NOT `pm clear` / uninstall to force
  the first-run consent screen without the owner's OK — it wipes the star.

Cross-links: [[feedback-ndk-proot-pipeline]], [[moment-2026-06-12-first-phone-star]],
[[project-ump-android-node]], [[feedback-proot-sandbox-net-limits]].
