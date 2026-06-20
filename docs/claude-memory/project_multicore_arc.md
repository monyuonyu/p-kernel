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
