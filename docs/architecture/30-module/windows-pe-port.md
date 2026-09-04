# Windows (mingw-w64 PE) port — P1, console boot

> **Status: SHIPPED 2026-07-05 (`ff163ac1`) — cross-compile CI gate green, PROMOTED TO
> BLOCKING 2026-07-12** (4 hosted runs green in a row, per the ci.yml comment).
> **Runtime execution is still uncovered**: the gate proves the cross-COMPILE only;
> nobody has run the resulting `.exe` (own words from the commit: "no wine in the
> sandbox, so the console boot itself needs a real-box run"). **This doc did not
> exist until 2026-09-05**, when the unattended routine backfilled it per mk_pino's
> instruction ("無人 run に書かせる", 2026-09-05 03:05).

> **A note on how this doc was written.** Same constraint as
> [[distributed_moe_design.md]]: material is limited to (1) the full body of commit
> `ff163ac1`, (2) the `windows-pe-build` job's comments in `.github/workflows/ci.yml`,
> and (3) the fact that CI is green. The actual target-layer source (dispatch.c,
> memory.c, the Winsock shim, etc.) was **not** re-read for extra detail — everything
> below transcribes/organizes what the commit and ci.yml already say.

Related: [[device-capacity.md]] (the closest existing doc to "adapt to what the
vessel can do" — this port is a new *kind* of vessel, not a capability tier within
one, so it is filed here rather than folded into that doc), [[BACKLOG.md]]
(2026-09-05 resync block, where this gap was first found).

Implementation (file list from `git show --stat ff163ac1`, not independently
re-read): a new μT-Kernel 3.0 target `_WINDOWS_X86_64_` (machine.h / sysdef /
profile + tk cpudef/syslib/dbgspt), `kernel/.../windows_x86_64/dispatch.c` (the
Fiber dispatcher), `time.c` (QPC/GetProcessTimes), `sio.c` (Win32 console/pipe I/O),
a Winsock2 compat shim reached via `-idirafter`, `selfc_stub.c`, an honest `fault.c`
stub, plus small `#ifdef _WIN32` shims in `arch/common/llm/pk_parallel.c`,
`arch/linux/pfs_ark.c`, `arch/linux/pfs_durable.c`, and the `KNL_UPTR` fix in
`memory.h`/`memory.c` (see §2).
Cert: CI job `windows-pe-build` ("Windows PE ビルドゲート (mingw-w64,
boot/windows/x86_64)") — cross-builds `boot/windows/x86_64/p-kernel.exe` with
`gcc-mingw-w64-x86-64` and asserts `file` reports it as a `PE32+ executable`.

---

## 1. What P1 delivers, as described in the commit

> p-kernel now cross-builds into a genuine native Windows x86-64 .exe (NOT WSL) via
> mingw-w64, the same "every install is a node" as Android/Linux. Console boot P1:
> the μT-Kernel 3.0 core, the arch/common brain and the LLM tier link and reach the
> interactive `mind` shell.

Target-dependent layer, per the commit:

- **New μT-Kernel 3.0 target** `_WINDOWS_X86_64_` (machine.h/sysdef/profile + tk
  cpudef/syslib/dbgspt), routed alongside the existing Linux targets.
- **Dispatcher = Windows Fibers** (`kernel/.../windows_x86_64/dispatch.c`):
  `ConvertThreadToFiber` at boot, one `CreateFiber` per task (the handle stored in
  `tcb->tskctxb.ssp`), `SwitchToFiber` on dispatch. Because the fiber owns the stack
  plus the full register set (including the Win64 non-volatile XMM6-15), **there is
  no hand-written context-switch asm to get wrong** — this is called out in the
  commit as the reason this approach was chosen over a manual save/restore.
- **Task restart** uses a deferred-delete "graveyard", because (per the commit) "a
  fiber can't rewrite its own resume PC".
- **Time** = `QueryPerformanceCounter`; **CPU time** = `GetProcessTimes` (`time.c`).
- **v1 is COOPERATIVE** (no SIGALRM/preempt): the tick is pumped at safe points
  (idle + dispatch entry) from QPC. The boot sequence prints an honest
  `"[win] cooperative scheduler (no preempt in v1)"` — i.e. the lack of preemption is
  disclosed at runtime, not hidden.
- **Console I/O** = Win32 console/pipe (`sio.c`). **Net** = Winsock2 via an
  `-idirafter` compat shim (BSD sockets → winsock; `close`→`closesocket`,
  `fcntl(O_NONBLOCK)`→`ioctlsocket`, `poll`→`WSAPoll`), reaching only the POSIX net
  translation units — **never** the tk-typed brain. `WSAStartup` runs at boot.
- **selfc (self-compile) is DISABLED on Windows** (`selfc_stub.c`), the same
  treatment Bionic/Android gets.
- **`fault.c` is an honest stub** — no per-task fault isolation in v1.

## 2. The real boot-blocker: LLP64

Quoting the commit, because this is the one correctness bug it says actually had to
be fixed to boot at all (not just a target shim):

> Windows `long` is 32-bit, so the allocator's pointer-flag packing (memory.h) and
> `knl_init_Imalloc` alignment (memory.c) truncated 64-bit heap pointers — the LP64
> `knl_Imalloc` trap resurrected.

Fix, per the commit: a pointer-width `KNL_UPTR` type — `unsigned long` on LP64
platforms (byte-identical there, i.e. no behavior change for existing targets) and
`unsigned long long` on LLP64 (Windows) — plus 64-bit-safe masks. `pfs_durable`,
`pfs_ark`, and `pk_parallel` get minimal `#ifdef _WIN32` shims (`mkdir`/`_commit`/
`O_BINARY`/`pread`-`pwrite`/CPU count).

## 3. Crown neutrality

Per the commit: all non-Windows edits are `#ifdef _WIN32`-guarded or `KNL_UPTR`-
aliased. The claim is that the Linux x86_64 build still links, and bare-metal x86
`.text` stays byte-identical (`sha256 260da329…`, the same crown hash cited by
[[distributed_moe_design.md]]'s pre-DMOE baseline). Cross-builds are stated to be
clean to PE32+ x86-64.

**This doc did not independently reproduce that hash** — as with the DMOE-A doc,
that reproduction is a separate audit step this backfill pass did not perform.

## 4. CI gate — advisory, then blocking

Quoting the ci.yml comment on `windows-pe-build`, since its own honesty framing is
worth preserving verbatim:

> The GitHub-hosted ubuntu-latest runner used by this job DOES ship the
> gcc-mingw-w64-x86-64 apt package (installed below), so on the hosted lane this
> gate is real. It is kept advisory because we could NOT exercise it on the
> self-hosted pkernel-thinkpad runner (this job pins ubuntu-latest, but if a future
> edit ever moves the arch matrix onto self-hosted, mingw may be absent there) — a
> runner-CAPABILITY gap must never redden master on a green tree. The build + PE32+
> assertion are DETERMINISTIC and proven locally (x86_64-w64-mingw32-gcc,
> p-kernel.exe = "PE32+ executable for MS Windows ... x86-64").
> PROMOTED TO BLOCKING 2026-07-12 (4 hosted runs green in a row) — the Windows arch
> is now trunked with a real, non-bypassable build gate.
> (Runtime execution is still uncovered — this gates the cross-COMPILE only; a wine
> smoke is the next step.) The port is crown-neutral: the crown-text-identity job
> proves the existing arches' .text is byte-identical (the shared TUs it touches are
> all #ifdef _WINDOWS_X86_64_-guarded).

In plain terms: the gate is real and blocking today, but it only proves the code
compiles into a well-formed Windows executable. **Nobody has yet booted it** — there
is no wine smoke test and no self-hosted-Windows CI job. The ci.yml comment names
"a wine smoke" as the explicit next step, in its own words, not this doc's guess.

## 5. Honest gaps

- **No runtime verification exists.** The build gate never executes the binary; the
  interactive `mind` shell reachability claimed in §1 is the commit author's report
  from a local run, not something CI (or this doc) has reproduced.
- **`fault.c` has no per-task isolation** (explicit v1 stub, per §1).
- **The v1 scheduler is cooperative only** — no preemption; a runaway task can starve
  the rest of the system, and this is disclosed at boot rather than fixed.
- **selfc is unavailable on this target** (disabled, not deferred-with-a-plan, as far
  as the available material shows).

## 6. What this doc is not

As with [[distributed_moe_design.md]], this is a backfill of an existing shipped,
CI-gated port — not a new design proposal, and not an independent audit. Anywhere
a design intent beyond what the commit/ci.yml literally state would be needed (for
example, whether/when preemption or per-task fault isolation is planned), this doc
leaves it unstated rather than guessing, per the instruction this backfill pass was
given.
