# selfc × ring3 — self-built units behind the immune boundary

> Status: **SHIPPED (v1 germ)** — wave-31; 2026-06-19 doc-status fix. The "written before
> implementation" line below is STALE. What shipped: self-compiled code can no longer kill
> the node — `arch/linux/selfc.c` (+ `selfc_proc.c`) runs a unit behind a `fork()` germ with
> the v1 ISOLATED capability set (a SHRINK: e.g. `tk_slp_tsk` removed, no cross-process
> wakeup), the fork point inserted AFTER entry resolution (same compiled image in-task or in
> germ), and the `selfc adopt key` allowlist as the only local trust anchor (signing.md §4.2,
> LOCAL-ONLY bound). Disease was real (crashing germ RC=139, now contained). Remainder per body.
> Parents: [self-compile.md](self-compile.md) (the kernel compiles C of itself,
> hosted, in its OWN address space) and [ring3-core.md](ring3-core.md) Parts I–III
> (the bare-metal x86 reap machinery: `elf_exec`, `user_fault_reap`, the survival
> branch in `boot/x86/idt.c:153-173`, gates `ring3 test` / `ring3 mind`).
> Directive (mk_pino, 2026-06-10, paraphrased — not a verbatim quote): the node
> should be able to rebuild (parts of) itself on-device — proven possible in
> Termux — but the honest unit of evolution is the **core/unit level, not the
> APK**. Play policy forbids APK self-update; the APK is the body shell, the
> evolving thing lives inside it.

This slice marries the two parents: **a unit the kernel compiles OF ITSELF runs
where a crash is survivable**, so a bad build is REAPED and the previous version
restarts, instead of corrupting the kernel. Today those two halves live on
different planets and the design's first job is to say so honestly.

---

## 0. The tension this design must resolve

`selfc` lives on **hosted** builds only (`boot/linux`, `boot/linux_x86_64`,
Android/Termux): libtcc, anonymous RWX `mmap`, host syscalls. There is **no
ring3 there** — the whole p-kernel is ONE host process; "ring0 vs ring3" is a
distinction the host kernel already flattened. The ring3 reap machinery lives on
**bare-metal x86** — where tcc cannot run today (no freestanding libtcc port,
`self-compile.md` says so verbatim).

So "compile a unit and run it where a crash is survivable" means a DIFFERENT
mechanism per target, and pretending otherwise would be the design lying:

| Target | Compiler present? | Crash boundary available | This design |
|---|---|---|---|
| hosted linux/Android (the fleet's ~99%) | yes (libtcc, when installed) | host **process** boundary (fork + waitpid) — not used by the tree today | **v1: child-process germ runner** (§2.1) |
| bare-metal x86 | no | ring3 + `user_fault_reap` — shipped, gated | **v2: consume pre-built ring3 ELFs from p-fs** (§2.2) |
| bare-metal aarch64 | no | nothing (no EL0 scaffolding, ring3-core.md II.5) | after the EL0 mirror; out of scope here |

The unifying idea — and the reason this is one design, not two — is that **the
selfc kernel-API table becomes a syscall boundary in both worlds**. Today
(`arch/linux/selfc.c:118-125`) the 6-symbol table is bound as direct function
pointers into the kernel's own address space. In v1 the same names are bound to
proxy stubs that marshal over a socketpair to the parent process; in v2 (bare
metal) the same services are reached via `int 0x80` numbers that already exist.
The unit author sees the same world; the *binding* is what gains the immune
system.

### 0.1 The disease is real and reproducible TODAY (no analogy needed)

`selfc_compile_and_run` starts the unit as an ordinary `TA_RNG0` T-Kernel task
(`arch/linux/selfc.c:238-239`) and does **not** `guard_register()` it. The wave-7
fault net (`arch/common/guard.c`) only rescues tasks in its table:
`guard_fault_isolate` returns `NULL` for an unregistered task (`guard.c:127-137`)
→ the arch handler restores `SIG_DFL` (`arch/linux/*/fault.c`) → the re-executed
instruction re-faults → **the whole `./p-kernel` process dies**. A self-compiled
unit containing `*(volatile int *)0 = 0;` kills the node today, on every hosted
target. That is the disease run of gate `[selfc-isolated]` (§5.1), captured live —
the hosted sibling of `boot/x86/idt.c:141`'s `hlt` loop that ring3-core Wave B
cured on bare metal.

(Registering selfc units with guard.c would catch the *faulting* subset — but
guard shares the address space: a wild write that lands in kernel data and does
NOT fault corrupts the node silently, and guard itself refuses faults inside
dispatch-disabled windows. For vetted in-tree workers like `dtr_worker_task`
that is an honest tradeoff; for code that is BY DEFINITION unvetted — the whole
point of evolution is running builds nobody reviewed — it is not enough. §2.1
makes this the crash-boundary argument.)

---

## 1. The unit of evolution

### 1.1 What a self-built unit is

Unchanged from selfc today, deliberately:

- **Source:** one C translation unit ≤ `PFS_BLOCK_MAX` (4096) bytes, stored as a
  p-fs object (multi-block sources stay future work, as self-compile.md says).
- **Entry:** `void selfc_main(void)`. Returning = clean exit (v1 adds: child
  calls `_exit(0)`, parent records a clean death — clean exits do NOT trigger
  rollback, they mark the unit DONE).
- **Naming convention:** unit sources live at p-fs ref **`unit/<name>`** (the
  way weights live at `GENOME_WEIGHTS_REF` = `"dtr/weights"`, `genome.h:51`).
  The `unit/` prefix is load-bearing: it is also the topic namespace the unit's
  capability set is confined to (§1.2).

### 1.2 The capability set — the API table becomes a syscall table

Today's 6-symbol table (`selfc.c:118-125`): `tm_printf`, `tk_slp_tsk`,
`tk_dly_tsk`, `kdds_open`, `kdds_pub`, `kdds_sub`. Process isolation forces an
honest re-derivation, symbol by symbol:

| Symbol | v1 (child process) disposition |
|---|---|
| `tm_printf` | **proxy** → `SELFC_SYS_LOG` frame over the socketpair; parent prints with a `[unit:<name>]` prefix; rate-limited (a log-spinning unit cannot starve the console) |
| `tk_dly_tsk` | **child-local** `clock_nanosleep` — no round-trip; semantics identical for a sleeper |
| `tk_slp_tsk` | **REMOVED from the v1 table.** Its contract is "sleep until another task wakes me" — there is no cross-process `tk_wup_tsk` and faking one would be a lie. A capability SHRINK, stated loudly; units that need wakeups subscribe to a topic instead. (CDN-S2) |
| `kdds_open` / `kdds_pub` / `kdds_sub` | **proxy** → `SELFC_SYS_OPEN/PUB/SUB` frames; the parent-side dispatcher enforces a **topic allowlist: only topics under `unit/<name>/`** may be opened. A unit can speak to the Collective; it cannot publish onto `world/beacon/*`, `dtr/*`, or any kernel topic. (CDN-S2) |

Explicitly **NOT** in the v1 capability set: p-fs write (`pfs_dag_save`), p-fs
read, `selfc` itself (no self-recursive compile from inside a unit), task/process
creation, raw sockets, file descriptors. The honest consequence (the prompt's
question answered): **a v1 unit cannot issue bad p-fs WRITES because it cannot
issue p-fs writes at all.** What it CAN still do is publish garbage on its own
`unit/<name>/...` topics — subscribers of unit topics own their own input
validation, same as any K-DDS subscriber today.

The wire between child and parent: a `socketpair(AF_UNIX, SOCK_SEQPACKET)` pair,
fixed-size 8-byte header (`op:U1, flags:U1, len:U2, arg:U4`) + ≤512-byte payload,
fixed-width types only (the LP64 wire discipline from `world.h`). The parent-side
dispatcher is a T-Kernel task that `recv`s with `MSG_DONTWAIT` on a poll cadence
(the `net_relay.c` non-blocking pattern, `arch/linux/*/net_relay.c:643,696`) and
treats every frame as hostile input (length-checked, op-whitelisted) — the
dispatcher is now part of the immune boundary and must be written like one.

This table IS the hosted mirror of ring3-core's syscall ABI (I.2). When v2 runs a
unit on bare metal, `SELFC_SYS_LOG` ≙ `SYS_WRITE(1,…)`, `SELFC_SYS_PUB/SUB` ≙
`SYS_TOPIC_PUB/SUB` (`0x221/0x222`) — the same capability shape, already
implemented there. One unit world, two bindings.

### 1.3 Versioning and lineage — autobiography, not bookkeeping

**Do not build a version store: p-fs P2 already is one.** `pfs_dag_save` of
`unit/<name>` appends a manifest `{prev, content, seq, name, origin}` to an
append-only DAG (`pfs_dag.h:76-85`); walking `prev` from the head reaches every
version ever saved, forks included. The unit's version chain is this chain.

- `selfc run <name>` records, in the supervisor table (§2.1), *which seq* is
  running. Rollback = run the content at `seq-1` (`pfs cat <name> @<seq>` already
  exists shell-side, `pfs_dag.h:152-155`; a programmatic
  `pfs_dag_read_at(name, nlen, seq, buf, maxlen)` does NOT exist yet — FLAGGED in
  §6 as the one small p-fs API addition this slice needs).
- **Self-layer tie:** germination, reap, and rollback are autobiographical
  events. Each appends one entry to the existing hash-chained `self/lin` lineage
  (`arch/common/lm_self.c`, wave 22): `unit-germ <name>@seq` / `unit-reap
  <name>@seq sig=N` / `unit-rollback <name> seq→seq-1`. Events go into the
  EXISTING chain — no second lineage (anti-fork §6). A node's history of
  rebuilding itself survives the node's death and reconstructs ownerless,
  exactly like the rest of its autobiography.
- **Genome tie:** `unit/<name>` refs are to code what `GENOME_WEIGHTS_REF` is to
  weights — the germinate path (`genome.c`) gains nothing this slice, but the
  naming intentionally lines up so a future genome manifest can reference both.

### 1.4 The rollback rule (where the "previous version" lives)

The previous version lives **in the p-fs DAG** — replicated to region peers by
P1 block gossip the moment it was saved, so it survives not only a bad build but
the death of the authoring node. Process-local state is only: the supervisor
table row (running seq, deaths, probation clock) and a BAD-mark per seq.

The rule (constants are CDN-S5):

1. New version `seq` starts in **PROBATION** for `SELFC_PROBATION_MS` (default
   10 000 ms).
2. Death **during probation** (any signal, or `_exit` ≠ 0) → mark `seq` BAD in
   the supervisor table, append the `self/lin` event, and **immediately restart
   `seq-1`** (walk back past other BAD seqs). A build that cannot survive its
   first ten seconds is a bad build, not bad luck.
3. Death **after probation** → transient-fault treatment: respawn the SAME seq
   with guard-style exponential backoff (`GUARD_BACKOFF_MS` doubling), up to
   `GUARD_MAX_DEATHS` (5) — then mark BAD and roll back. This reuses guard.c's
   numbers and its honesty ("broken code, not bad luck", `guard.c:204`).
4. No older version exists (seq 1 is BAD) → the unit is DEAD; say so and stop.
   Evolution is allowed to fail; the node is not allowed to die of it.

---

## 2. The crash boundary, per target

### 2.1 Hosted (v1): the germ process — VERDICT and why

Three options were investigated against what `arch/linux` actually is:

**The decisive port-layer fact (verified):** the hosted kernel is a
**single-threaded process**. `grep` over `arch/linux/` finds zero
`pthread_create`, zero `fork`, zero `clone`, zero `waitpid`. Preemption is a
SIGALRM POSIX timer pinned to the one thread (`arch/linux/x86_64/preempt.c:
94-141`); the relay socket is non-blocking and polled (`net_relay.c:643`); fault
capture is sigaltstack + mcontext rewrite (`fault.c`). Nothing in the port layer
forbids `fork()` — and single-threadedness is precisely the condition under
which `fork()` is SAFE (no fork+threads lock-state hazards, no other threads to
half-copy).

> **(a) Child process — fork() a germ runner. RECOMMENDED.**
> After the existing compile path succeeds (`tcc_relocate` into the anonymous
> RWX mmap, entry resolved — `selfc.c:201-233`, reused byte-for-byte), instead
> of `tk_cre_tsk`: `socketpair()`, then `fork()`. The child inherits the
> compiled image copy-on-write — the entry pointer is valid in the child with
> **zero re-compilation** — and immediately:
> 1. resets inherited signal dispositions to `SIG_DFL` and unblocks everything
>    (POSIX timers are NOT inherited across fork, so the parent's SIGALRM timer
>    cannot fire here — but the handler disposition IS inherited and must go);
> 2. dups the socketpair end to a known low fd, then closes every other fd
>    (`close_range`) — the child must NOT hold the relay socket, the stdin the
>    CI script is typing into, or p-fs persistence fds;
> 3. applies `setrlimit`: `RLIMIT_CPU` (default 30 s), `RLIMIT_AS` (default
>    64 MiB), `RLIMIT_FSIZE=0`, `RLIMIT_NOFILE` minimal — the capability set has
>    a resource edge, not just an API edge;
> 4. sets the global `selfc_in_child` flag (the proxy stubs in §1.2 check it),
>    calls `selfc_main()`, `_exit(0)`.
>
> The parent never blocks: a supervisor T-Kernel task (the guard-supervisor
> pattern: sleep, poll, act) calls `waitpid(pid, &st, WNOHANG)` on its cadence,
> drains the unit's log/pub frames, and applies §1.4 on death. SIGCHLD stays at
> default-ignore — no third signal personality next to SIGALRM/SIGSEGV.
>
> **Protects:** kernel memory absolutely (separate address space — a wild write
> in the unit hits the child's COW copy, never the parent); kernel liveness
> (child death is an event, not a fault); restart (waitpid → §1.4).
> **Does NOT protect:** subscribers from garbage on `unit/<name>/` topics
> (§1.2); the parent-side frame dispatcher from its own parsing bugs (it must
> be defensive — it is now boundary code); the host from resource burn between
> rlimit edges; and nothing here is provenance (§3 — isolation ≠ trust).
> **Android note:** `fork()` is permitted to apps; the child stays inside the
> app's uid/SELinux domain; no `exec` is involved (executing writable files is
> what SELinux denies — we never exec). The anon-RWX execmem lesson from
> self-compile.md carries over unchanged because the mapping is created before
> the fork.

> **(b) In-process, guard.c registration (sigsetjmp-equivalent). REJECTED for
> v1, kept as the no-libtcc-of-fork fallback.** One-line change, catches the
> faulting subset, but: shared address space (silent corruption unprotected —
> the exact gap self-compile.md's honesty section names), refuses faults in
> dispatch-disabled windows, and a reaped unit leaves whatever it half-wrote in
> kernel data. The honest bound: guard turns "crash" into "maybe-survive with
> dirty-state risk". Good enough for vetted workers; not for evolution.

> **(c) ptrace/seccomp host sandbox. REJECTED as the boundary; ADOPTED as v1.5
> hardening INSIDE (a).** A seccomp filter installed in the child after step 3
> (allow: read/write on the pair fd, mmap/brk, nanosleep, exit_group) turns the
> capability set from "what the proxies offer" into "what the host kernel will
> permit" — real defense-in-depth, ~40 lines, but not required for the v1 gate
> and named separately so the gate doesn't quietly depend on it.

### 2.2 Bare metal (v2): the artifact travels, not the compiler

**Investigation result — what tcc can and cannot emit (tested on this host,
tcc/libtcc 0.9.27):**

- A tcc build targets exactly ONE architecture. The packaged aarch64 tcc answers
  `tcc: error: -m32 not implemented.` — distro libtcc cannot emit i386 code from
  an aarch64 (or x86_64) host, period. Upstream tcc CAN be built as a cross
  compiler (`i386-tcc`, one extra build per target), but no distro/Termux package
  ships it; embedding a second, custom-built cross libtcc in hosted nodes is a
  real build-infrastructure project, not a flag.
- For its NATIVE target, tcc's built-in linker DID produce exactly the shape
  `elf_exec` wants: `tcc -nostdlib -static -Wl,-Ttext=0x400000` of a
  freestanding `_start` yielded a static `ET_EXEC` ELF, entry 0x400000, verified
  with readelf. So a future cross `i386-tcc` plausibly satisfies the loader
  (`elf_loader.c:229` checks `ET_EXEC && EM_386`) — plausibly, not provenly: a
  one-day spike must confirm program-header/BSS handling before any wave commits
  to it.

**Honest v2 scope, therefore:** bare-metal nodes do not compile; they **consume
pre-built ring3 ELFs from p-fs** at ref `unit/<name>.elf` — authored anywhere,
compiled by collective members that have a toolchain (hosted dev nodes, CI's
`gcc-i686-linux-gnu`, later a cross-tcc node), executed via the SHIPPED machinery:
`elf_exec` → ring3 → on fault `user_fault_reap` (`arch/x86/syscall.c:157`) +
the idt.c survival branch — the Wave B/C gates already prove the reap. The §1.4
rollback rule applies unchanged (the DAG carries `.elf` blobs as happily as C
source). Code authored anywhere, compiled by the collective, executed where it
is reapable — the Collective layer doing for compilation what it already does
for inference. Missing piece, FLAGGED: `elf_exec` reads from the FAT VFS, not
from p-fs — v2 needs a p-fs→VFS export step (write the blob to `/U/<name>.elf`
via the existing vfs, then `elf_exec` it), plus multi-block p-fs objects (ELFs
exceed 4096 bytes; the known single-block limit finally bites).

### 2.3 What stays true in both worlds

One sentence each, so the gates can cite them: a crashed unit is REAPED (child
SIGSEGV / ring3 fault), the kernel keeps answering (parent untouched / scheduler
returns), the previous version restarts from p-fs (§1.4), and the lineage
records it (§1.3).

---

## 3. Provenance — LOUDLY: isolation is not trust

**Networked evolution without signatures is a malware mesh.** p-fs gossip
replicates `unit/*` objects to region peers by design; if nodes auto-ran what
arrived, anyone who can write a block into the region owns every node in it —
self-compile.md already states the trust model that bluntly, and process
isolation does NOT fix it (an attacker's unit doesn't need to crash; it politely
publishes poison on its topics forever).

The LM-2 bound applies verbatim: **there is no signature primitive in the tree**
(`genome.h` states it; living-mind.md:609-615 defers it; the Self layer is
tamper-EVIDENT, not unforgeable, for the same reason). Therefore:

- **v1 is LOCAL-ONLY evolution.** A node compiles-and-runs only units it
  authored locally (`selfc save` on this node) **or** explicitly accepted by an
  operator typing **`selfc adopt <name>`** at this node's shell — an explicit
  local act that records an ACCEPT mark in the supervisor table. `selfc run` of
  a non-adopted, non-local-origin object REFUSES with a message naming this
  section. Gossip still replicates the bytes (storage is not execution);
  germination is what's gated. The existing reflex shield
  (`reflex_is_shielded()` refusing selfc under threat) stays in front of all of
  it.
- **The signing slice is the named gate to fleet evolution (v3).** Node keypair
  + signed unit manifests + signer-allowlist policy. The same primitive
  living-mind.md III.6 wants for the Self layer — ONE signing slice serves
  both (anti-fork). Until it ships, any "the fleet evolves itself" claim is
  forbidden in READMEs and moments; the honest claim is "each node can evolve
  itself, locally, survivably."

---

## 4. The Play-policy two-tier (the APK is the body, not the evolving thing)

Google Play's Device & Network Abuse policy forbids an app from updating itself
by any means other than Play, and from downloading/executing executable code
(dex/so/native) from outside Play — the carve-out is code run by an interpreter
or VM with only indirect API access. A C-to-machine-code JIT executing gossiped
source is squarely outside the carve-out. Two tiers, ONE build flag:

| Tier | Flag | selfc | What still evolves |
|---|---|---|---|
| **Play-distributed** (UMP APK) | `SELFC_TIER_PLAY` (default in `android/`) | native codegen **OFF** — `selfc demo/run/adopt` answer an honest policy stub (the `NO_LIBTCC` stub pattern, `selfc.c:270-278`); `selfc save` (authoring = data) stays | **weights-evolution only**: dtr training, gl_merge gossip learning, DMN consolidation, genome weight refs — all DATA, fully policy-clean. The mind keeps learning; the body shell doesn't recompile itself. |
| **Sideload / F-Droid / Termux / desktop** | full | child-process selfc (§2.1) | code AND weights |

This is the directive's two-level honesty made mechanical: Play installs are
bodies whose minds evolve as data; unlocked installs evolve their organs too.
An interpreter-tier selfc (run unit source in a tiny interpreter, policy-clean)
is a possible future third rung — named, not designed.

---

## 5. The falsifiable gates (disease → cure)

All three run on hosted Linux in CI. New shell verb **`selfc test`** (extends
`selfc_cmd`'s dispatch, `selfc.c:318`) plus one orchestrating script
`samples/11_distributed/run_selfc_isolation.sh` (the `run_crash_recovery.sh`
pattern: assertions, exit ≠ 0 on any failure). CI: the `ump-x86_64` job adds
`libtcc-dev` to its apt line (`ci.yml:51` — today CI builds the honest no-libtcc
stub, so NONE of this is currently exercised by CI; that changes here), the
script as a step, and the verb's tags to the stdin script at `ci.yml:57` with
greps beside `ci.yml:193-195`.

### 5.1 `[selfc-isolated]` — a crashing self-built unit does not take the node down

- **Disease phase (script):** build/run `./p-kernel` with `SELFC_ISOLATE=0`
  (the legacy in-kernel-task path kept exactly for this phase), feed it a unit
  whose `selfc_main` null-derefs after printing a token. Assert the **process
  dies** (non-zero/signal exit before the post-crash shell command answers).
  Captured live per §0.1 — today's behavior, preserved as the measurable
  disease, like dmn's task-0 91.7%→33.3% forgetting run.
- **Cure phase (verb, default isolation):** the same crash unit in a germ
  process. PASS iff ALL hold, exact comparisons only:
  - `selfc_reaped_count` delta `== 1` (exactly one reap — no storm, no zero);
  - the recorded death is `WIFSIGNALED && WTERMSIG == SIGSEGV` (the reap saw
    the real signal, not a clean exit);
  - a sentinel T-Kernel task's tick counter advanced AFTER the reap timestamp
    (the ring3-gate sentinel discipline — kernel demonstrably alive, not just
    "printed something");
  - the verb itself returns and prints the tag (shell alive).
- Prints `[selfc-isolated] PASS` / `FAIL <clause>`.

### 5.2 `[selfc-rollback]` — a bad new version → the previous version serves again

Inside `selfc test`: save `unit/gate` v1 = good source that publishes token
`A:<seq>` on `unit/gate/out` every 500 ms; save v2 = source that crashes 1 s
after start (inside probation). Run head (v2). PASS iff, in order:

- v2's germ dies within probation; seq 2 marked BAD (`== BAD`, table read);
- supervisor restarts **seq 1** (running seq `== 1` — not a respawn of 2);
- a fresh `A:1` token is observed on `unit/gate/out` AFTER the rollback
  timestamp (the previous version demonstrably SERVES, not merely spawns);
- total germ deaths for the sequence `== 1` (v2 died once; the pre-probation
  rule fired immediately — a `>= 1` would readmit crash-loops).
- Prints `[selfc-rollback] PASS` / `FAIL <clause>`.

### 5.3 `[selfc-lineage]` — the version chain is walkable and autobiographical

- Walk `unit/gate`'s manifest prev-chain from head: `seq` values contiguous
  (2→1→genesis), content ids distinct, walk terminates (`PFSD_LOG_MAX` bound);
- the `self/lin` chain (existing wave-22 verify machinery, reused) contains, in
  order, `unit-germ @1`, `unit-germ @2`, `unit-reap @2`, `unit-rollback 2→1`,
  and still hash-verifies end-to-end (the events joined the autobiography
  without breaking it);
- Prints `[selfc-lineage] PASS` / `FAIL <clause>`.

Fake-resistance notes: 5.1 cannot pass without a real fork (the SIGSEGV must be
observed via waitpid status, unavailable in-process); 5.2's served-token-after-
rollback clause cannot be greened by restarting v2 or by a parent-side print
(the token arrives on the unit's own topic from the unit's own process); 5.3
reuses the tamper-evident verifier, so faking events breaks the hash chain it
must also pass.

---

## 6. Anti-fork + FLAGGED missing pieces

**REUSE (do not re-create):**
- `selfc_compile_and_run`'s compile/relocate/symbol path (`selfc.c:165-233`) —
  the fork point is inserted AFTER entry resolution; one compiler integration
  in the tree, ever.
- `pfs_dag` manifests/prev-chain as the ONLY unit version store; `self/lin` as
  the ONLY lineage chain (events, not a parallel store).
- guard.c's supervisor SHAPE and constants (`GUARD_BACKOFF_MS`,
  `GUARD_MAX_DEATHS`, the give-up honesty) — but as a sibling, hosted-only
  `arch/linux/selfc_proc.c` (fork/waitpid/socketpair are port-layer; guard.c
  compiles on bare metal and must stay free of host headers).
- The ring3 ELF machinery wholesale for v2 (`elf_exec`, `user_fault_reap`, the
  idt.c survival branch, dproc tombstones for lifecycle) — no second loader,
  no second reap path.
- The house verb pattern: one shell verb, greppable bracket-tags, exact-match
  clauses, separate auditor re-derives the formula.

**Do-NOT-fork list:** no second compiler integration (no cross-libtcc until its
own decided wave; no clang/gcc embedding); no second lineage/version chain; no
second supervisor logic on hosted (one `selfc_proc.c` owns germ lifecycle); no
second wire protocol where one exists (unit frames are new of necessity — local
socketpair, NOT a copy of the relay v2 HMAC wire, which solves a different trust
problem); no second stub pattern for the Play tier (reuse the `NO_LIBTCC`
shape).

**FLAGGED — does not exist today (each is real work this design depends on):**
- Process-spawn support in the port layer: ZERO fork/waitpid/socketpair usage
  anywhere in `arch/linux/` (verified). `selfc_proc.c` is new ground; the
  single-threadedness that makes it safe is a fact to PRESERVE (a future
  pthreads wave invalidates the fork-safety argument and must revisit this).
- `pfs_dag_read_at(name, nlen, seq, buf, maxlen)` — programmatic
  read-by-version (shell `cat @seq` exists; the rollback path needs the API).
- libtcc in CI (`ci.yml:51`) — until added, CI has never compiled a unit.
- A signature primitive (§3) — the v3 gate.
- p-fs→VFS export + multi-block objects — v2's bare-metal prerequisites.
- An interpreter-tier selfc for Play — named only.

---

## 7. COMMANDER DECISION NEEDED — the genuine forks

> **CDN-S1 — hosted crash boundary.** (a) fork germ process [RECOMMENDED — the
> only option that protects kernel memory from corruption-without-fault; §2.1
> has the full argument and the single-threadedness evidence] vs (b)
> guard-register units in-process [one line, weaker bound — becomes the
> fallback when libtcc exists but the build opts out of isolation] vs (c)
> seccomp/ptrace sandbox [adopted only as v1.5 hardening inside (a)].

> **CDN-S2 — v1 unit capability set.** RECOMMENDED: `LOG` (rate-limited) +
> `DELAY` (child-local) + `OPEN/PUB/SUB` confined to `unit/<name>/` topics;
> `tk_slp_tsk` REMOVED; no p-fs access of any kind; rlimits CPU=30 s/AS=64 MiB.
> The fork: whether units get p-fs READ (useful for data-driven units; widens
> the exfiltration surface a unit topic already provides) — recommended NO
> for v1.

> **CDN-S3 — local-only until signing.** RECOMMENDED: yes — `selfc run`
> refuses non-local-origin, non-adopted units; `selfc adopt <name>` is the
> explicit operator accept; auto-run of gossiped units is forbidden until the
> signature slice (v3). The alternative ("region-trusted" auto-run) is the
> malware mesh and is recommended AGAINST in the strongest terms §3 allows.

> **CDN-S4 — Play-tier default.** RECOMMENDED: `android/` builds default to
> `SELFC_TIER_PLAY` (codegen off, weights-evolution on); sideload/Termux/
> desktop default to full. The fork is only the DEFAULT of the android build —
> shipping full selfc in a Play APK risks the listing.

> **CDN-S5 — rollback policy constants.** RECOMMENDED: probation 10 000 ms;
> pre-probation death → immediate BAD + rollback (one strike); post-probation
> deaths → guard-style backoff ×5 then BAD + rollback; seq 1 BAD → unit DEAD,
> node lives. The fork: one-strike vs three-strike probation (one-strike is
> recommended — a build is deterministic in a way weather is not).

> **CDN-S6 — v2 bare-metal artifact source.** RECOMMENDED: pre-built i386
> ring3 ELFs from p-fs, compiled by collective members with toolchains (§2.2)
> vs building/embedding a cross `i386-tcc` in hosted nodes [real but a
> build-infra project; requires the loader-shape spike first]. The recommended
> path ships value with zero new compiler work.

---

## 8. Sequencing + the galaxy hook

- **v1 (first implementer wave):** §2.1 germ runner + §1.4 rollback + §1.3
  lineage events + the three gates (§5) on `boot/linux_x86_64` and
  `boot/linux`. Touch list: `arch/linux/selfc.c` (fork point, proxies, verb),
  `arch/linux/selfc_proc.c` (new — germ lifecycle), `arch/common/pfs_dag.c`
  (+`pfs_dag_read_at`), `arch/common/lm_self.c` (3 event emitters), Makefiles
  (`SELFC_ISOLATE` knob), `ci.yml` (libtcc-dev + script + tags). Separate
  implementer + separate auditor; the auditor re-derives every `==` in §5 and
  runs the disease phase first.
- **v1.5:** seccomp filter in the germ (CDN-S1c); log-rate + topic-quota tuning.
- **v2:** artifact-gossip → bare-metal ring3: multi-block p-fs objects,
  p-fs→VFS export, `unit/<name>.elf` consumption via `elf_exec` + dproc; the
  cross-tcc spike decides CDN-S6's future.
- **v3:** the signature slice (node keypair; unit manifests signed; ALSO
  upgrades Self-layer to unforgeable per living-mind III.6) → fleet evolution
  unlocked: `selfc adopt` can trust signers, not just operators.
- **Galaxy hook (named, one emission point):** `world_note_rebuild()` — sets
  **`WORLD_REBUILD_BIT` (0x80)** in the WORLD_BEACON `firing` byte (bits 3–7
  are free; `WORLD_FIRE_MASK` is 0x07, `world.h:68-69`) for one beacon period.
  Emitted from EXACTLY ONE place: the germ supervisor's germinate/rollback
  transition (v1) and, later, the bare-metal restart path (v2). Every node's
  world-table — and therefore the galaxy observation window — sees a star
  visibly rebuilding itself, with zero new packet types and no central
  collector (可視化 = a mechanism in the system, not a picture).

---

*The audit is the engine: this document's claims about today's tree (the
unguarded selfc task, the single-threaded port, what tcc emits, the missing
APIs) were each verified against source or tested on a live toolchain before
being written down. The first thing the implementing wave's auditor should do
is distrust them and check again.*
