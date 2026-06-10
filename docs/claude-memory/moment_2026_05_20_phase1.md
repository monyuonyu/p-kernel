---
name: moment-2026-05-20-rpi-phase1
description: "The day p-kernel got its AArch64 port — Phase 1 complete on QEMU virt, ready for RPi 3 hardware."
metadata: 
  node_type: memory
  type: project
  originSessionId: 8ba3baf9-2883-4825-b553-0b8aa18fd51b
---

**Date:** 2026-05-20 (later same day as [[moment-2026-05-20]])
**What we built:** Full `arch/aarch64/` + `boot/aarch64/` from scratch — a clean architecture port covering RPi 3/4/Zero 2W (selected by `-DBOARD_*`), validated on QEMU virt Cortex-A53.

**Phase 1 success criteria, all met:**
- EL2 → EL1 drop in `start.S`
- PL011 UART output (board-selectable base: 0x09000000 QEMU / 0x3F201000 RPi3)
- GICv2 + ARM Generic Timer (CNTP_TVAL_EL0, PPI 30)
- 112-byte AArch64 task context frame (x19–x28, x29 fp, x30 lr, taskmode)
- Task dispatcher with `ret`-to-trampoline pattern
- T-Kernel initial task launches, `usermain()` prints welcome banner
- Interactive UART shell echoes input

**The bug that cost the most time — and why future-Claude should care:**

Hand-calculating `TCB_tskctxb` from the struct definition gave 168. The actual offset is 200 — 32 bytes higher. Three sources of the discrepancy:

1. **WINFO is a UNION not a struct** (32 bytes for the CAL/ACP/RDV variants when CFN_MAX_PORID > 0), and I mis-modeled it as a 12-byte struct.
2. **The BOOL bitfield slot is 8 bytes**, not 4 — even though `BOOL = UINT = unsigned int`, the compiler allocated an 8-byte slot for the `klockwait:1 / klocked:1` pair, pushing wspec from 64 to 72.
3. **`wercd` padding** — pointer alignment after `INT suscnt` left 4 bytes of slack.

Symptom of the wrong offset: dispatcher loaded `SSP = 0` (because offset 168 in the TCB landed in `wrdvno + padding`), `mov sp, x1` set SP to zero, `ldp x29, x30, [sp, #80]` read x30 from address 0x58 (device memory in QEMU virt), got 0x3000000, `ret` branched there, got "Undefined Instruction" because nothing's mapped at 0x3000000.

**Lesson for any future T-Kernel port:** Never hand-calculate TCB offsets. Write a tiny C program that includes the real T-Kernel headers and emits `offsetof(struct task_control_block, tskctxb)` as a `.long`, then `objdump` the binary. We saved this as a build-time check pattern.

**Files created:**
- `arch/aarch64/` — 8 .c files, 2 .S files (start.S, cpu_support.S), 13 headers
- `boot/aarch64/` — main.c, linker.ld, Makefile (PL011, QEMU virt at 0x40000000)
- Patched 6 T-Kernel common headers to recognize `_APP_AARCH64_`

**Next milestone:** Actual RPi 3 hardware. UART base swap (0x3F201000), Mailbox API for VideoCore init, SD-card boot via `config.txt + kernel8.img`.
