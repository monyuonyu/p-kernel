---
name: moment_2026_06_10_wave25_ring3_survival
description: "wave-25 — ring3 Wave B ships: the bare-metal x86 kernel SURVIVES a deliberate ring3 core crash (reap + reschedule + restart). The 'never dies' substrate becomes real code; mk_pino's ring3/EL0 directive slice #1."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**2026-06-10, wave-25.** [[project_ring3_core_relocation]] slice #1 ships: **the kernel survives
a ring3 tenant crash.** Before this, `boot/x86/idt.c` ended EVERY fault in `while(1){hlt}` — one
bad pointer in the mind killed the node. Now: exception stubs pass saved CS/EIP;
`exception_handler` branches — ring0 fault still halts honestly; a **ring3** fault prints
`[core] ring3 fault #N @eip — task reaped`, bumps exact counters, and reaps via the SYS_EXIT
unwind (`user_fault_reap` → `user_proc_unwind` → `tk_ext_tsk` — **never iret back into the dead
context**; the I.3 hard part, solved by reusing the syscall-exit state: a ring3 fault lands on
TSS.RSP0 = the task's kernel stack = exactly SYS_EXIT conditions).

**Gate `ring3 test`** (6 clauses of ring3-core.md II.3 + a 7th stronger): live ring0 `moe_infer`
oracle, reaped Δ==1 EXACT, from_ring==3 EXACT, handler-returned, post-crash sentinel counter
strictly advanced, restart==oracle, crash_exit==−86 (a crash can't impersonate a clean infer).
**The auditor ran the falsification itself** (defanged crash binary → FAIL g2-reaped!=1) — 8/8
cold-boot PASS on its own rebuild, diagnostic EIP objdump-matched to the real faulting movl.
Merged `fab550c`, epitaph `987d930` (wave-25). CI: new `ring3-survival` QEMU-boot job.

**Honest bound held in the gate language (mk_pino confirmed the scoping):** this slice claims
ONLY survival+restart — the moe math still runs ring0 behind SYS_INFER (repointed
mlp_forward→moe_infer). **Wave C = link the moe.c body into the ring3 ELF; the directive is not
"done" until then** (mk_pino: 最終的には2番にならないとやったことにはならない).

**Traps found/for later:**
- The implementer hit a REAL pre-existing bug class: the old exception stub called 32-bit C from
  64-bit mode with a broken ABI (pre-commit exception reporting was likely garbage); rewrote
  `isr_common_stub` mirroring the proven irq/syscall far-jump-to-compat pattern (reload DS/ES/SS —
  SS is null after a ring3 fault).
- A quiesce step was needed: `init.rc`'s resident `infer_d.elf` shares the ONE user address space
  (CDN-4a), so the verb pauses the heal watchdog (`heal_elf_pause`) + kills infer_d first, resumes
  after. Races fail the gate honestly (reaped=2 direction), never falsely pass.
- **Ledger follow-up (pre-existing debt, audit-found):** `dproc_kill_by_name` (`dproc.c:221`)
  tears down with tk_ter_tsk/tk_del_tsk but NO ssy cleanup / paging unregister — leaks the
  victim's page tables per kill; suspected source of a 1-of-6 post-PASS ring0 #PF flake (auditor
  could not reproduce in 8 cycles). Give it a `user_proc_unwind`-style teardown.

Method held: separate implement + separate audit agents, commander read the gate `if`s directly
([[feedback_development_method_is_the_life]]). Same day as wave-24 ([[moment_2026_06_09_wave24_lm4_handoff]]);
LM-5 (随時 stream) ran in a parallel lane throughout.
