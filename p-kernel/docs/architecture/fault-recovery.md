# Fault recovery: task isolation + respawn from p-fs weights (wave 7)

PR #4, hole (4): *"heal.c can only restart ring-3 ELF daemons. The
flagship Transformer runs ring-0, so one bug = the whole node dies."*

This wave closes that hole for the hosted ports: a fault in a guarded
ring-0 task kills **only that task**. The kernel process survives, the
supervisor respawns the task, and its `recover_fn` reloads the trained
weights from the p-fs object `dtr/weights` (persisted by wave 6's
`dtr save`). The brain survives its body.

## What is protected — and what is not

Protected: tasks registered via `guard_register()` (today: the dtr
inference worker), faulting in ordinary task code with interrupts and
dispatch enabled.

NOT protected (the node aborts, honestly): faults in unguarded tasks
(shell, idle, kernel housekeeping), faults inside an IRQ-disabled or
dispatch-disabled window (kernel state is mid-mutation and cannot be
trusted), faults in handler context, and a second fault while a kill
is already in flight. Recovery is for the AI workload, not a blanket
"never crash" lie.

## The path of one fault (hosted Linux)

```
guarded task writes through NULL
  └─ SIGSEGV  →  fault.c handler (on the sigaltstack, so a smashed
     │          task stack cannot take the handler down)
     ├─ arch_irq_disabled_flag set?            → SIG_DFL, abort
     ├─ guard_fault_isolate(): faulting task in the guard table,
     │  dispatch enabled, no kill in flight?   → else SIG_DFL, abort
     └─ rewrite ONLY the named registers of the interrupted context:
        PC → guard_task_killer, SP → static emergency stack
        (aarch64: pc/sp/x29/x30. x86_64: RIP/RSP/RBP — NEVER memcpy
        the mcontext; its fpregs pointer goes stale at sigreturn.)
  └─ sigreturn resumes the task in guard_task_killer(): ordinary task
     context again, so kernel calls are legal. Log sig/pc/addr, mark
     the entry DEAD, wake the supervisor, tk_exd_tsk(). Every other
     task keeps running.
  └─ guard supervisor task: after exponential backoff (200 ms doubling
     per death, give-up after 5 deaths) run recover_fn — for dtr,
     reload `dtr/weights` from p-fs — then tk_cre_tsk/tk_sta_tsk a
     fresh incarnation.
```

The emergency stack matters: the dying task's own stack may be the
very thing that broke. `tk_exd_tsk` runs on a static 8 KB stack owned
by guard.c; only one kill is in flight at a time (a second concurrent
fault aborts the node).

## Division of labour with heal.c

| | heal.c (waves 3+) | guard.c (wave 7) |
|---|---|---|
| protects | ring-3 ELF daemons; guarded tasks of **dead peer nodes** | ring-0 tasks on **this** node |
| detects | SWIM DEAD verdict / DORMANT tid poll | synchronous fault (SIGSEGV/SIGBUS/SIGFPE) |
| recovers | restart on heir node / elf_exec | respawn task + recover_fn (p-fs weight reload) |

They compose: guard keeps a node alive through its own bugs; if the
node dies anyway (unguarded fault, power), heal + p-fs replication
recover the work on a peer. `dtr save` is the bridge — the same blob
serves both recovery paths.

## Files

- `arch/common/guard.c` + `include/guard.h` — table, killer,
  supervisor, `guard` shell verb. Pure T-Kernel API: compiles on all
  four targets (i686, bare aarch64, linux/aarch64, linux/x86_64).
- `arch/linux/{aarch64,x86_64}/fault.c` — signal capture + context
  rewrite. Installed by tkdev_init.c after `arch_signals_init()`.
- `arch/common/dtr_train.c` — `dtr_worker_task` (guarded worker),
  `dtr crash` (fault injection: zero the in-memory weights, then NULL
  write — so the post-recovery `dtr eval` can only pass if the p-fs
  reload really happened), `dtr_recover_weights` (recover_fn).
- `samples/11_distributed/run_crash_recovery.sh` — end-to-end demo +
  assertions: train → save → crash → kernel-alive → respawn →
  trained accuracy back (95%/100%). Exit ≠ 0 on any failure.

## Bare-metal hook (future)

guard.c already compiles and links on the bare-metal targets; what is
missing is the capture side. The contract is two functions:
`guard_fault_isolate(sig, pc, addr)` from the data-abort / #PF vector
(returns the emergency SP or NULL → panic as today), then arrange the
exception return to land in `guard_task_killer()` on that stack —
i.e. patch ELR_EL1/SP_EL1 (aarch64) or the iret frame (x86) instead
of a Linux mcontext. The decision logic, respawn, backoff and shell
verb need no changes.
