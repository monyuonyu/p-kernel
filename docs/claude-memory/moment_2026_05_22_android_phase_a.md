---
name: moment-2026-05-22-android-phase-a
description: "libpkernel.so builds and runs through dlopen — the same path Android's System.loadLibrary takes. UMP Phase A's kernel-side is structurally done."
metadata: 
  node_type: memory
  type: project
  originSessionId: 0c02f473-32a5-48ba-a6a8-302c29abffe3
---

**Date:** 2026-05-22

**Commit:** `aae22a8` feat(android): Phase A complete — libpkernel.so loads + boots

**What happened.** The kernel now builds as a shared library (`libpkernel.so`) with `-fPIC -fvisibility=hidden`, and a host-side `dlopen` test in `boot/linux/test_so_load.c` loads it, finds `pkernel_main` via `dlsym`, and runs the full T-Kernel boot chain — banner, AI primitives, K-DDS, DTR, shell prompt — all from inside the .so. This is byte-for-byte the path Android's `System.loadLibrary("pkernel") + nativeBoot(1)` will take.

**The bug previous-Claude blamed wasn't the bug.** The dd9993b commit message guessed the PIE hang was the dispatcher's `adrp + add :lo12:` patterns failing under ASLR. It wasn't. Those instructions are PIC-safe for same-DSO symbols — the page-PC-relative ADRP plus the locally-resolved LO12 ADD gives the right address whether the image is loaded at 0x40_0000 or 0x7f80_0000_0000.

**The real bug.** `arch/aarch64/include/utk_config_depend.h` defined `SYSTEMAREA_END = 0x50000000UL` and `CFN_REALMEMEND = ((void*)SYSTEMAREA_END)`. T-Kernel's `knl_init_Imalloc` reads:

```c
memend = CFN_REALMEMEND;
if ( (PTR_UINT)memend > (PTR_UINT)knl_lowmem_limit ) {
    memend = knl_lowmem_limit;
}
```

With `-no-pie` the BSS `linux_heap[]` lives at a low address (~`0x600000`), so `0x50000000 > 0x1600000` fires the clamp and `memend` becomes the BSS end. Fine.

Under `-fPIE -pie` (or `-fPIC -shared` for the .so) ASLR places `linux_heap[]` at `0x7f8000000000`+. Now `0x50000000 > 0x7f8001000000` is **false** — clamp skipped, `memend` stays at the absurdly-low `0x50000000`, and the very next line:

```c
knl_imacb->memsz = memend - knl_lowmem_top - 16;
```

wraps to a huge nonsensical value. The allocator's first internal walk then strides off into unmapped memory → SIGSEGV. Symptom looks like "dispatcher hangs"; cause is one comparison too narrow in a fallback constant.

**The fix.** New `arch/linux/include/utk_config_depend.h` shadows the bare-metal aarch64 version for both hosted ports. Sets `SYSTEMAREA_END = ~(unsigned long)0` — the clamp now always fires, `memend` is always `knl_lowmem_limit`, allocator init does what `cpu_init.c` always intended. Bare-metal aarch64 keeps its 0x40200000–0x50000000 RAM layout because `boot/aarch64/Makefile` doesn't include `arch/linux/include/`.

**Two smaller fixes alongside.**
- `arch/linux/aarch64/cpu_support.S`: `.hidden knl_taskmode` so the linker accepts the dispatcher's `adrp + add` against it when the TU is in a shared library. C-defined globals (`arch_irq_disabled_flag`, `knl_ctxtsk`, `knl_schedtsk`, etc.) get hidden via `-fvisibility=hidden`; the asm-defined symbol has to opt in explicitly.
- `boot/linux/main.c`: `__attribute__((visibility("default")))` on the `main`-via-`-Dmain=pkernel_main` definition so JNI / dlsym callers can find it through the .so's otherwise-hidden symbol table.

**Build/test surface.** `boot/linux/Makefile` gains:
- `make libpkernel.so` — builds with the exact flag set the Android CMakeLists.txt uses
- `make run-so-test` — `dlopen`s the .so, runs `pkernel_main` in a pthread, watches the banner appear

This makes future Phase A regressions catchable on any aarch64-linux host without NDK or an Android device. Captured run:

```
$ make run-so-test
LD_LIBRARY_PATH=. ./test_so_load
[test] libpkernel.so loaded, pkernel_main @ 0x7927311a08
=== p-kernel linux boot ===
[BOOT] Starting T-Kernel...
[T-Kernel] Initial task started
 p-kernel  [linux / aarch64 userspace]
[ai]   Tensor pool   : 16 slots × 16 KB
[kdds] K-DDS ready  port=7376
[dtr] Transformer initialized   params: 568 floats
  T-Kernel is alive inside a Linux process.
  Type 'help' for commands.
p-kernel> [test] sleep done — kernel should have printed banner
```

**What this means for UMP.** The kernel-side of Phase A is structurally done. What remains for an actual APK on a phone:
- Android Studio project files (`build.gradle`, `AndroidManifest.xml`, the Kotlin Activity & Service) — outside this repo
- Foreground Service plumbing so the kernel survives backgrounding (Phase C item, not Phase A)
- An on-device run with NDK r25c+ pointed at `android/app/src/main/cpp/CMakeLists.txt`

The build path is now de-risked: same `.so` flag set verified host-side, no `adrp/add` rewrite needed, no GOT-indirect surgery in `cpu_support.S`. NDK clang and ld should reproduce the same result as host gcc.

**Lessons captured for future hosted-port work.**
1. **"Hangs after the banner under PIE" should suspect allocator init before the dispatcher.** The constants in `utk_config_depend.h` are designed around bare-metal physical RAM ranges; on hosted with ASLR they need to be sized to ASLR's address space, not to the embedded MCU's.
2. **For shared-library builds, asm-defined globals need explicit `.hidden`.** `-fvisibility=hidden` only covers C-defined symbols. Cross-check the asm side of every dispatcher/trampoline port when going from `-pie` to `-fPIC -shared`.
3. **A host-side `dlopen` smoke test is worth ~5 minutes to author and saves an Android Studio round-trip per kernel change.** `boot/linux/test_so_load.c` is the template.

Cross-links: [[project-ump-android-node]] (the project), [[moment-2026-05-22-lp64-refactor]] (related — the lp64/ refactor and this Phase A both update arch/linux/ shadow conventions), [[project-pkernel-philosophy]] ("a home for AI that no one owns" — Android distribution is the literal-physical realization).
