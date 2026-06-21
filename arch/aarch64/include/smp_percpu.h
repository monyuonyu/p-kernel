/*
 *  smp_percpu.h (aarch64) — ②.2a per-CPU scheduler-state accessors.
 *
 *  THE CONVERSION (docs/architecture/smp-2-production-scheduler-plan.md §2.2):
 *  the production T-Kernel scheduler reads "the task running on THIS CPU"
 *  (knl_ctxtsk) and "the next task to run" (knl_schedtsk) through TWO global
 *  variables.  ②.2 makes a real SMP kernel where N cores each run their OWN
 *  current task — so those globals become PER-CPU.
 *
 *  This header provides the STORAGE the per-CPU accessor macros index: the
 *  struct smp_cpu layout + the g_smpcpu[] / smp_this_cpu() externs.  The
 *  macros themselves (CUR_CTXTSK / CUR_SCHEDTSK / CUR_DISPATCH_DISABLED) live
 *  in arch/aarch64/include/cpu_status.h (the arch header kernel.h pulls
 *  everywhere), so they are available wherever a kernel TU expands a critical
 *  section — even TUs that include <cpu_status.h> but not <task.h>.
 *
 *  ────────────────────────────────────────────────────────────────────────
 *  THE CROWN CONSTRAINT (§0): with SMP OFF (the DEFAULT build, no
 *  -DSMP_SELFTEST), every macro expands to the PLAIN GLOBAL — textually a
 *  parenthesised identifier, which gcc compiles BYTE-IDENTICALLY to the bare
 *  identifier (proven: objcopy -j .text cmp).  So the shipped uniprocessor
 *  kernel, and x86/linux/rl78 (which share kernel/common/ but never define
 *  SMP_SELFTEST), are UNCHANGED.  N=1 == today, byte-for-byte.
 *
 *  With SMP ON, the macros resolve to g_smpcpu[smp_this_cpu()].{...} — the
 *  per-CPU SMP block (arch/aarch64/smp.c, ONE source of truth, the SAME block
 *  the asm dispatcher loads via its SMPCPU_* offsets).  smp_this_cpu() is
 *  MPIDR Aff0 (firmware-independent "which core am I").  The BKL serialises
 *  kernel entry, so a kernel critical section reading CUR_CTXTSK always sees
 *  ITS OWN CPU's current task and never races another CPU's slot.
 *  ──────────────────────────────────────────────────────────────────────── */

#ifndef _SMP_PERCPU_H_
#define _SMP_PERCPU_H_

#ifdef SMP_SELFTEST

/* TCB may not be typedef'd yet when this header is first reached. */
#ifndef __tcb__
#define __tcb__
typedef struct task_control_block TCB;
#endif

/* The per-CPU SMP block — the ONE source of truth, defined in
 * arch/aarch64/smp.c.  struct smp_cpu is declared HERE so the macros below
 * can index it as a typed lvalue (assignments + ->tskque all work).  The
 * asm dispatcher (cpu_support.S) mirrors the off-0 / off-8 fields via the
 * SMPCPU_CTXTSK / SMPCPU_SCHEDTSK macros; smp.c _Static_asserts the layout.
 *
 * KEEP THE FIRST THREE FIELDS (ctxtsk off 0, schedtsk off 8, dispatch_disabled
 * — note: the asm only relies on off 0/8) STABLE; later observability fields
 * are appended after them (see smp.c). */
struct smp_cpu {
    TCB           *ctxtsk;        /* off 0:  this CPU's running task          */
    TCB           *schedtsk;      /* off 8:  this CPU's next task             */
    unsigned long  exec_count;    /* off 16: per-CPU dispatch/exec counter    */
    unsigned long  cpu_id;        /* off 24: MPIDR Aff0                        */
    volatile unsigned long live;  /* off 32: set 1 when CPU enters dispatcher */
    volatile unsigned long preempted_at; /* off 40                            */
    volatile unsigned long highprio_ran; /* off 48                            */
    /* ②.1b/②.2a MERGE: ②.1b appended stack_top@56, ②.2a appended
     * dispatch_disabled.  Reconciled: stack_top@56, dispatch_disabled@64,
     * sizeof 72.  THIS LAYOUT MUST EQUAL struct smp_cpu in arch/aarch64/smp.c
     * (the asm dispatcher's SMPCPU_SIZE stride) — a desync reads the wrong
     * CPU's slot. */
    unsigned long  stack_top;            /* off 56: this CPU's dispatcher SP   */
    INT            dispatch_disabled;    /* off 64: per-CPU dispatch-disable   */
};

#define SMP_MAX_CPUS 4

extern struct smp_cpu g_smpcpu[SMP_MAX_CPUS];
extern unsigned long  smp_this_cpu(void);

#endif /* SMP_SELFTEST */

#endif /* _SMP_PERCPU_H_ */
