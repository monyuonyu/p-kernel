---
name: feedback-ndk-proot-pipeline
description: "How to actually build Android APKs from aarch64 Ubuntu PRoot (no binfmt_misc, no aarch64-host NDK). The wrap_x86_64.sh strategy + the AGP/aapt2 quirks that bite."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a4128c94-5332-49d8-ac74-41541259901a
---

Building APKs on aarch64 PRoot with x86_64-only Android NDK works,
but the pipeline has a few traps. Apply this whenever Phase C / Phase
D / future Android work needs a real APK.

**Why:** spent ~90 minutes 2026-05-26 hitting these one at a time.
Recording so the second time costs minutes, not hours.

**How to apply:**

1. **Don't try to enable binfmt_misc.** `/proc/sys/fs/binfmt_misc`
   exists in the PRoot but can't be mounted writable. Use
   /root/wrap_x86_64.sh to replace each x86_64 ELF with a shell
   wrapper that calls `qemu-x86_64 -0 "$0" -L /usr/x86_64-linux-gnu
   "$0.real" "$@"`. Re-run after every sdkmanager install.

2. **`exec -a` is bash-only.** Use qemu's `-0 argv0` to preserve
   argv[0] inside the emulator. clang vs clang++ dispatch depends
   on this.

3. **`-perm -u+x`, not `-perm -111`.** AGP's bundled aapt2 ships
   mode 744 (only user-executable). The `-111` form requires all
   three execute bits.

4. **Pin AGP's aapt2 to the SDK one.** Add to gradle.properties:
   `android.aapt2FromMavenOverride=/root/android-sdk/build-tools/34.0.0/aapt2`
   Without this, AGP extracts a fresh unwrapped aapt2 from its
   bundled jar on every build, undoing the wrapper.

5. **zlib1g for amd64 isn't on ports.ubuntu.com.** apt's amd64
   multiarch fails on the aarch64 ports mirror (404s). Manually
   `wget http://archive.ubuntu.com/ubuntu/pool/main/z/zlib/zlib1g_*.deb`
   then `ar x` (NOT `dpkg-deb -x` — PRoot trips on permissions),
   then `tar xvf data.tar.zst`, then copy libz.so.1 into
   /usr/x86_64-linux-gnu/lib/. libc6-amd64-cross + libstdc++6-amd64-
   cross + libgcc-s1-amd64-cross are in apt and cover the rest.

6. **lib/libc must be -idirafter, not -I.** In CMake:
   `set_source_files_properties(${KCOMMON_SRC} PROPERTIES
   COMPILE_OPTIONS "-idirafter;${INC_LIB}/libc")`. Leading -I
   shadows T-Kernel errno.h with the POSIX stub and "undeclares"
   E_OK / E_PAR / etc. across every kernel/common file.

7. **Disable FORTIFY_SOURCE for the NDK build.** T-Kernel's
   include/kernel/tkernel/limits.h shadow lacks SSIZE_MAX, which
   Bionic's bits/fortify/unistd.h dereferences textually. Use
   `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` in CMake's
   add_compile_options.

8. **Bionic adapters.** Bionic exposes errno via `__errno()`, not
   glibc's `__errno_location()`. One-line shim in pkernel_jni.c:
   `extern volatile int *__errno(void); int *__errno_location(void)
   { return (int *)__errno(); }` — keeps arch/linux/aarch64/*.c
   sources unchanged.

9. **sio.c needs special handling under `__ANDROID__`.** Bionic's
   <termios.h> inlines a helper using `errno = EINVAL` textually.
   Pre-declare errno + EINVAL in the TU before <termios.h>.

10. **Use explicit source lists in CMakeLists, not file(GLOB).**
    GLOB sweeps in T-Kernel-only sources (deviceio.c etc.) that the
    hosted port doesn't link. Mirror boot/linux/Makefile's lists.

11. **Phase A CMakeLists had PK_ROOT off by one** (6-level climb
    landed at the git repo, not the source root). If you see
    "Cannot find source file: /root/p-kernel/boot/...", check the
    `${CMAKE_SOURCE_DIR}/../../../...` depth. Should be 5 from cpp/.

Related: [[moment-2026-05-26-phase-c-substep2]] documents the day this
all came together; [[feedback-lp64-typedef-trap]] is a cousin
shadow-header trap for stdint.
