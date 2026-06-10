---
name: project_ring3_core_relocation
description: "mk_pino's directive (2026-06-09) to move the self-modifying AI core DOWN from ring0/EL1 to ring3/EL0 (user space), so the constantly-evolving core can't crash the kernel. The Evolution-layer foundation. Design-doc-first wave, not yet started."
metadata: 
  node_type: memory
  type: project
  originSessionId: 53149c59-d57f-4589-aa45-bb25220e2df2
---

**Directive (mk_pino, 2026-06-09, verbatim intent):** "ちょっと大変ですけど ring3に降ろしましょう。
arm は el0 ですかね。やっぱりこの部分は常に変化していく領域なので不安定になると思うので
ユーザー空間においた方がいいですよね。" → **Move the AI core from ring0/EL1 to ring3/EL0
(user space).** Rationale = the self-evolving AI region is inherently unstable; isolating it in
user space means a buggy/corrupted core CANNOT take down the kernel. This is the **Evolution
layer** foundation (the least-built worldview layer) and directly serves "never dies": the
kernel = a minimal immutable substrate that survives even when the mind crashes. See
[[project_living_mind_vision]] (Evolution layer), [[project_pkernel_philosophy]].

**Privilege fact confirmed this session (the ring-0 investigation):** the AI core runs TODAY as
**kernel tasks at ring0 (x86) / EL1 (aarch64)** — invoked from `arch/x86/shell.c` and the three
`usermain.c` in kernel context (moe_infer, lm_test, lm_self_test, r3, dtr, reflex, dmn, gl).
ARM answer for mk_pino: **yes, AArch64 user space = EL0** (EL1=kernel/OS, EL2=hypervisor,
EL3=secure monitor); so x86 ring0→ring3 maps to EL1→EL0.

**Crucial scoping nuance (tell mk_pino):** "ring3/EL0" only has LITERAL meaning on the two
**bare-metal** targets — `boot/x86` (i686) and `boot/aarch64` (QEMU virt). On `boot/linux` and
`boot/linux_x86_64` the WHOLE p-kernel is already a host user-space process (no ring0 there), so
the relocation is a bare-metal concern; Linux/UMP targets are already "in user space" in the
host sense.

**Scaffolding that already exists (x86, most mature):** `boot/x86/start.S` GDT (0x20 ring3 code,
0x28 ring3 data, 0x30 TSS), `boot/x86/gdt_user.c`, `boot/x86/syscall.c` (`SYS_INFER`=0x210,
`SYS_INFER_SLA`=0x230), `boot/x86/isr.S` (syscall_isr, IDT gate DPL=3, ring3 CS=0x23,
ring3→ring0 transition frame), `arch/x86/include/p_syscall.h` (:316 0x210, :323 0x230),
`arch/common/include/edf.h` (edf_infer). **`boot/x86/README.md:411` has an UNCHECKED TODO:**
"p_syscall 拡張 (0x230 SYS_INFER_SUBMIT, 0x231 SYS_INFER_WAIT)". aarch64 uses SVC for syscalls;
less scaffolding → EL0 is a later/mirror slice.

**Wave A DONE (2026-06-10):** separate design agent wrote `docs/architecture/ring3-core.md`
(449 lines), merged to master `17a147f`. Commander verified the two load-bearing code claims in
the live tree: (1) `boot/x86/idt.c` halts on EVERY fault except #3 (`while(1){hlt}`) — the
"core crash kills the node" bug is real; (2) the README:411 `0x230 SYS_INFER_SUBMIT` TODO
COLLIDES with live `SYS_INFER_SLA=0x230` (`p_syscall.h:323`) — async must go on the reserved
`0x240/0x241 SYS_DTR_SUBMIT/WAIT` (WAIT is a -1 stub today). NOTE: scaffolding lives in
**`arch/x86/`** (gdt_user.c, syscall.c, userspace.c, elf_loader.c), not boot/x86 as first
remembered. aarch64 has ZERO EL0/SVC scaffolding — EL0 is a sketch (doc II.5).

**Commander decisions (mk_pino approved 2026-06-10):** CDN-1..5 all at the design's recommended
defaults (DMN "when" stays ring0 / weights owned by ring0 p-fs + mapped copy / transport stays
ring0 via existing SYS_TOPIC_*/SYS_UDP_* / ONE user address space first / async on 0x240-0x241,
never 0x230). **Sequencing: Wave B = slice #1 (survival mechanism only: idt.c/isr.S saved-CS
branch, ring3 fault → reap + reschedule + restart; ring3 task may still syscall into ring0 math
via SYS_INFER 0x210); Wave C = slice #2 (moe.c body actually linked into the ring3 ELF — only
THEN is the directive's intent "done"; mk_pino: 最終的には2番にならないとやったことにはならない).
Gate language stays honest about which slice claims what.** Gate formula = the 6-clause check in
doc II.3 (`r3_class==r0_class && reaped==1 && from_ring==3 && handler-returned &&
post-crash-sentinel-counter-advanced && restart_class==r0_class`), greppable tag
`[ring3-survival]`.

**State (2026-06-10, end of day): Waves A+B+C ALL SHIPPED.** Wave B (survival) = wave-25
`fab550c`; Wave C (the math in ring3) = wave-27 `b429308` — **the directive is TRUE for the
inference path** ([[moment_2026_06_10_wave25_ring3_survival]],
[[moment_2026_06_10_wave27_ring3_mind]]). Design Part III (`ring3-core.md`) holds the Wave C
record; CDN-6..10 all at defaults (weights 0x213, MIND_NOTE 0x214, verb `ring3 mind`, infer_d
unchanged, whole-file gc-sections — link surface held at exactly the 14-symbol shim).

**Remaining (the next ring3 waves, in rough order):** (1) more modules into ring3 per CDN-4b
(dtr training, lm/dmn/gl organs — each needs its kernel-service syscalls); (2) **x87 FXSAVE/
CR0.TS before ANY concurrent ring3 minds** (whole x86 port has zero FPU context save — silent
float corruption otherwise; ledgered); (3) async infer on 0x240/0x241 (CDN-5); (4) per-module
address spaces (CDN-4b); (5) aarch64 EL0 mirror (II.5 sketch — zero scaffolding exists);
(6) dproc_kill_by_name teardown debt (audit-found, ledgered).
