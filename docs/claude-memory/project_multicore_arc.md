---
name: project_multicore_arc
description: "The ③ multicore strategy — option-3 (deterministic parallel matmul) chosen as the stepping-stone to option-2 (full SMP); MC-0/1 hosted, MC-2 bare-metal SMP bringup."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

2026-06-20: mk_pino asked whether p-kernel can use multiple cores. p-kernel is a
T-Kernel UNIPROCESSOR RTOS (single knl_ctxtsk/knl_schedtsk; zero pthread_create
in arch/linux; selfc uses fork). I laid out three options and he chose **③ then
②**: *"おすすめの3番をやりましょう ただし これは最終的には2番にしたい"* (do the
recommended #3, but ultimately I want #2).

- **① multi-node-per-device** — N processes, N stars. Rejected: UDP overhead, N×memory.
- **② full SMP kernel** — the north star. mk_pino believes I can do in ~1 week what
  took Linux 10 years. Huge race surface; risks breaking the byte-identity crown.
- **③ single-thread-kernel + parallel-matmul-backend** — partition the matmul by
  OUTPUT row → byte-identical to serial BY CONSTRUCTION (the "[[salty bug saga]]"
  one-mind crown survives). RECOMMENDED + the deliberate stepping-stone to ②.

The ③ arc (all cert-first, separate impl≠audit≠commander per [[feedback_development_method_is_the_life]]):
- **MC-0** — deterministic row-partitioned parallel matmul (hosted pthread pool),
  byte-identical across worker counts. `arch/common/llm/pk_parallel.{c,h}`. The
  invariant: only the OUTER output loop is split, never the contraction; each y[i]
  one worker; inner left-fold = serial order. `-O1 -ffp-contract=off` mandated.
- **MC-1** — size-gated (`pk_parallel_rows_gated`, gate=524288 MACs) on the teacher
  forward; 2.2-2.67x; full forward byte-identical with real GGUF. Small student/R3
  stay serial (below the gate).
- **MC-2** — bare-metal **constrained SMP**: wake the parked aarch64 secondary cores
  as DETERMINISTIC matmul workers behind the SAME `pk_parallel_rows` seam, WITHOUT
  making the scheduler SMP. = mk_pino's "native all-cores" AND the ② bringup
  foundation. **HONEST ELEPHANT: bare-metal has NO big matmul today** (LLM tier is
  hosted-only; only dt_linear ~48×48 runs, below the gate) → MC-2's value is the
  **②-enabling SMP bringup**, NOT an immediate speedup. Said plainly, never oversold.
  - MC-2.0 (SHIPPED, audited MERGEABLE 2026-06-20): wake ONE secondary via PSCI
    CPU_ON, run a deterministic tile, the kernel SURVIVES it (boots to shell,
    dispatcher byte-for-byte untouched). cert `[mc2-boot-survives]`, 4-way
    falsifiable. The first literal step of ②.
  - MC-2.1 (in flight): N cores + a real work-queue + `[mc2-smp-equiv]` byte-identity
    cert across {1,2,4}. Watch the slot-1 hardcode (MC-2.0's worker hardcoded
    g_cpu[1]; current_mpidr_aff0() did NOT exist — de-hardcode is task #1).
  - MC-2.2 (deferred): RPi3 hardware [live] — the ONLY place the barrier/SMPEN
    falsifier (Tooth B) actually bites. **QEMU TCG masks missing-barrier/SMPEN
    races** — a green QEMU run proves the partition/FP mechanism, NOT the barriers.

Key bare-metal facts MC-2 leans on: MMU is OFF (VA==PA, no page-table coherency
minefield); CPUECTLR.SMPEN was never set anywhere (the real coherency risk MC-2
adds); PSCI-via-HVC already works on QEMU virt (CPU_ON conduit), RPi3 = spin-table;
x86 bare metal has NO AP-startup (INIT-SIPI) → MC-2 is aarch64-ONLY, x86 deferred.
Design docs: docs/architecture/{multicore-matmul-plan, mc2-baremetal-smp-plan,
mc2-1-ncore-equiv-plan}.md. ② reuses MC-2's bringup + per-CPU data + work-queue
lock + barrier discipline; it still must add per-CPU run-queues, task migration,
cross-CPU knl_ctxtsk locking, IPIs. See [[project_ring3_core_relocation]],
[[feedback_audit_is_the_engine]].

## ② full SMP — SHIPPED through ②.2b-i (2026-06-20/21, a long autonomous marathon)
mk_pino's END GOAL ("最終目標まで進もう / ガンガン"). The production T-Kernel scheduler
is now genuinely SMP, each slice cert-first + separate-impl≠audit, the SHIPPED
uniprocessor kernel kept BYTE-IDENTICAL throughout (the crown — proven by .text/full-ELF
cmp every wave; the macro trick CUR_CTXTSK = per-CPU under SMP_SELFTEST, plain-global
otherwise). All merged + audited:
- ②.0 Big Kernel Lock (2 CPUs run the dispatcher under one lock); ②.1a first IPI (GIC
  SGI cross-CPU preempt, unforgeable sgi_taken); ②.1b N=4; **②.2a = the PRODUCTION
  scheduler SMP'd** (~211 knl_ctxtsk/schedtsk sites → CUR_* macros, 2 real tasks on 2
  cores via the real .Ldispatch_loop register-context switch; the ②.1b↔②.2a struct
  collision at offset 56 was the session's hardest merge — reconciled stack_top@56 +
  dispatch_disabled@64, SMPCPU_SIZE 72, LD_PERCPU_BASE lsl#6→(id<<6)+(id<<3)).
- SMP-N8 (8 physical cores = GICv2 ceiling; counter==8*K) + **SMP-AUTODETECT** (GICD_TYPER:
  the SAME binary adapts -smp 2/4/8 → wakes exactly that many; mk_pino's "measure the
  device + auto-fit". RPi3 is BCM2837 not a GIC → build-constant 4; DTB deferred).
- **②.2b-i true async register-context preempt FROM IRQ context** (the repo's hardest
  C-ABI piece: hook between EOIR and restore_caller_regs in _vec_el1_irq, capture
  ELR/SPSR, two-frame nesting + .Lirq_resume_tramp; a mid-loop no-poll task is suspended
  + resumes). The 5-dim audit GATED IT (deadlock-avoidance UNCERTAIN): the BKL-held guard
  was correct-by-reasoning but UNCERTIFIED — deleting it didn't fail the cert (dead code
  w.r.t. the suite); the design-mandated [smp-no-deadlock] nested-IRQ falsifier was missing.
  → NOT merged until certified (immune system working; [[feedback_audit_is_the_engine]]).
REMAINING: ②.2b-ii (secondary CNTP timer/WAIT), ②.2c ([smp-one-mind] crown: real R3 forward
byte-identical uniproc vs SMP), ②.3 (finer locks), hosted-port SMP (the teacher LLM's real
multicore — bare-metal has no big matmul; [[project_multicore_arc]] §0 elephant), RPi3 [live]
(the only place barrier/SMPEN teeth bite). Plans: full-smp-plan, smp-2-production-scheduler-plan,
smp-2b-async-preempt-plan, device-autodetect-plan.md.
See [[feedback_the_debug_env_is_real]] (the SSH ThinkPad found a real [live] bug on first use).
