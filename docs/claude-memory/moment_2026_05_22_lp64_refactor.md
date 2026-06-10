---
name: moment-2026-05-22-lp64-refactor
description: "Introduced arch/common/include/lp64/ — six headers move out of arch/aarch64/include, and x86_64-linux drops the \"borrow from aarch64\" awkwardness."
metadata: 
  node_type: memory
  type: project
  originSessionId: 0c02f473-32a5-48ba-a6a8-302c29abffe3
---

**Date:** 2026-05-22 (single session, ~1 hour)

**Commit:** `a2c5530` refactor(arch/common): introduce lp64/ for LP64-uniform headers

**What changed structurally.** Six headers that are identical for any LP64 ABI moved from `arch/aarch64/include/` to `arch/common/include/lp64/`:
- `cpu_conf.h`, `offset.h`, `profile_depend.h`, `stdarg.h`, `str_align_depend.h`, `sysinfo_depend.h`

The x86_64-linux build previously borrowed these (plus a few others) by adding `-I$(ARCH_AA64)/include` to its include path — a hack noted as "feels off" in [[moment-2026-05-21-first-pkernel-on-x86_64]]. That borrow is now gone.

**The real bug it surfaced.** `arch/aarch64/include/machine_depend.h` defines `_APP_AARCH64_=1` unconditionally. With `-I aa64/include` in scope, the x86_64-linux build was silently picking that up and ending the compile with **both** `_APP_AARCH64_` *and* `_APP_X86_64_` set. T-Kernel common headers (`sysinfo_common.h`, `syslib_common.h`, etc.) `#ifdef` on both, so each `<sysinfo_depend.h>` etc. include was being processed twice from the same file. By luck none of the conditional branches were mutually exclusive in a way that broke the build.

**The fix.** Three new x86_64-specific shadows in `arch/linux/x86_64/include/`:
- `machine_depend.h` — clean: just `ALLOW_MISALIGN/BIGENDIAN/INT_BITWIDTH`, no `_APP_AARCH64_=1`.
- `sysdef_depend.h` — minimal: just `INTNO_TIMER` (a `knl_intvec` slot id, not a hardware PPI).
- `utk_config_depend.h` — explicit copy of the object-count limits that the TCB layout depends on (must match aarch64's byte-for-byte).

`arch/linux/x86_64/include/offset.h` was deleted — `arch/common/include/lp64/offset.h` is byte-identical and now the single source of truth.

**The "borrow from aarch64" stays for arch/linux/aarch64/.** That port is LP64-on-aarch64, so reusing arch/aarch64/include/ headers (cpu_task.h, mmio.h, sysdef_depend.h, tkdev_conf.h, utk_config_depend.h, cpudef.h, machine_depend.h) is correct, not a hack — same instruction set, same SoC concepts.

**Build verification.** All four builds rebuilt clean:
- `boot/linux_x86_64/p-kernel` → 1,735,648 bytes (identical to pre-refactor)
- `boot/linux/p-kernel` → 832,880 bytes (was 832,840 — 40-byte diff, harmless)
- `boot/aarch64/kernel.elf` → 148454 / 1200 / 8553812 (identical)
- `boot/x86/{bootloader.bin, kernel.elf, kloader.bin}` → unchanged

Plus the three `arch/linux/aarch64/` PoCs (`poc_ctx_switch`, `poc_preempt`, `poc_dispatch`) still pass. Boot banner reached on x86_64 + aarch64 hosted + aarch64 bare-metal under QEMU virt.

**Why this matters for the project.** Adding a future LP64 arch (riscv64-linux, loongarch-linux, …) now means: write the arch-specific headers, drop them in `arch/linux/<arch>/include/`, write a Makefile that adds `-I arch/common/include/lp64`, done. No more "borrow from aarch64 and hope the `_APP_AARCH64_` leak doesn't bite."

Cross-links: [[feedback-arch-common-layout]] (the arch/common/ rule that motivated this), [[project-linux-userspace-port]] (the parent port project), [[moment-2026-05-21-first-pkernel-on-x86_64]] (where the cleanup-needed note was first written).
