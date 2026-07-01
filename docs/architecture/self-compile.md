# Self-compilation (selfc) — first milestone

> **現在地（2026-07-01・doc-hygiene 追記／本文は 年輪 として保存）:** この first milestone は **SHIPPED**：selfc は fork() germ ＋ capability 境界で自己コンパイルコードを隔離し（wave-31）、SIGN-2（wave-43）で fleet-signed germ まで到達した。本文はマイルストーンの 年輪。正本は [[gap-ledger.md]]。

> "The OS can compile and deploy new code to itself."

Until this milestone, that README sentence was carried entirely by the
`evolve` pipeline — which outsources the actual compile to a mothership
(a Python harness plus a human-driven compiler). PR #4's structural-gap
analysis named this the single largest gap: *not one byte of compiler
lived in the repository*.

`selfc` closes the first half of that gap: **the running kernel now
contains a C compiler and can turn C source into a living, running task
inside its own address space**, with no external process, no temp files,
and no mothership.

## What works today

Hosted builds only (`boot/linux`, `boot/linux_x86_64` — p-kernel running
as a Linux process), when libtcc is available at build time:

| Command | Effect |
| --- | --- |
| `selfc demo` | Compile a C source string baked into the kernel binary, in-memory, with libtcc (`TCC_OUTPUT_MEMORY`); start the result as a new T-Kernel task. |
| `selfc save <name>` | Save that same C source as p-fs object `<name>`. From that moment it is ordinary content-addressed data: P1 block gossip replicates it to region peers, P2 gives it version history. |
| `selfc run <name>` | Read C source from p-fs object `<name>` (retrying briefly while blocks are still replicating), compile it in-process, start it as a task. |
| `selfc ls` | List the units compiled since boot. |

The combination is the point: `samples/11_distributed/run_selfc_propagate.sh`
has node 1 author code (`selfc save genome.c`), lets p-fs gossip carry it
over the relay, and node 2 runs `selfc run genome.c` — **code written on
one node becomes machine code generated and executed inside another
node's kernel.** The kernel that runs the code never saw a file, never
forked a compiler, and never contacted the author again after the blocks
arrived.

## How it works

`arch/linux/selfc.c` (`arch/linux/include/selfc.h`):

1. `tcc_new()` + `tcc_set_output_type(TCC_OUTPUT_MEMORY)`, with
   `-nostdinc -nostdlib` — compiled code sees **no host headers and no
   libc**.
2. A small, explicit kernel-API table is exported symbol by symbol via
   `tcc_add_symbol()`:
   `tm_printf`, `tk_slp_tsk`, `tk_dly_tsk`, `kdds_open`, `kdds_pub`,
   `kdds_sub`. We deliberately do **not** link the kernel with
   `-rdynamic`: anything outside the table is an unresolved symbol at
   relocate time, so compiled code cannot *link against* arbitrary kernel
   internals. (It can still corrupt memory — see the honesty section.)
3. Two-step `tcc_relocate()`: first call with `NULL` to size the image,
   then into an anonymous `PROT_READ|WRITE|EXEC` `mmap`. This is not
   cosmetic — `TCC_RELOCATE_AUTO` places code on the malloc heap and then
   `mprotect(PROT_EXEC)`s it, which Android/Termux SELinux denies
   (`execheap`); anonymous executable mappings (`execmem`) are allowed.
4. `tcc_get_symbol(entry)` (default entry: `void selfc_main(void)`), then
   `tk_cre_tsk`/`tk_sta_tsk` wraps it in a fresh T-Kernel task (8 KiB
   stack, priority 6) that calls `tk_ext_tsk()` when the entry returns.
5. The `TCCState` and the code mapping are kept alive for the life of the
   kernel process — the spawned task may run forever. Up to 8 units per
   boot; no unloading yet.

### Build plumbing

`boot/linux` and `boot/linux_x86_64` Makefiles probe the toolchain:

```make
HAVE_LIBTCC := $(shell echo 'int main(void){tcc_new();return 0;}' | \
                 $(CC) $(LDFLAGS) -include libtcc.h -x c - -ltcc -o /dev/null 2>/dev/null && echo yes)
```

Only when the probe **links for the target ABI** does the build add
`-DHAVE_LIBTCC -ltcc`. Consequences, all intended:

- A machine without `libtcc-dev` (CI, fresh checkout) builds fine;
  `selfc` then answers with an honest stub message.
- On an aarch64 host, the x86_64 cross build (`-static` against an
  aarch64-only `libtcc.a`) fails the probe and auto-disables — proof the
  detection keys on the target, not the host.
- `make NO_LIBTCC=1` force-disables for testing the stub path.
- The `libpkernel.so` (Android) build stays a stub on purpose: no NDK
  libtcc yet, and a non-PIC `libtcc.a` can't go into a shared object.

Even without libtcc, `selfc save` still works — a node with no compiler
can still *author* code for nodes that have one.

## What is honestly NOT done

- **No verification, no sandbox, no signatures.** Compiled code runs at
  full kernel privilege in the kernel's own address space. The symbol
  table limits what it can *link*, not what it can *do* — a stray pointer
  write trashes the kernel like any other kernel bug. This is the minimal
  organ of self-evolution by design, but anyone deploying it should know
  the trust model is "whoever can write to your p-fs region owns your
  node". **Update (2026-06-07): the task-fault-isolation safety net has
  since shipped** — a guarded task that faults is killed and respawned
  while the kernel survives (`guard.c` / `fault.c`, see
  `fault-recovery.md`), so a compiled unit that faults *in ordinary task
  context* takes down only its task. It is still not a sandbox (a fault
  inside an IRQ-disabled window, or memory corruption that does not fault,
  is unprotected), and under threat the reflex layer can refuse new `selfc`
  germination entirely (`reflex_is_shielded()`, see `reflex-action.md`).
- **No bare-metal selfc.** `boot/x86` / `boot/aarch64` have no libtcc
  port. Embedding TCC's codegen in a freestanding kernel is a real
  (future) project: it needs an allocator shim, no-libc TCC build, and
  per-arch executable-page management.
- **No resource reclamation.** Units are never unloaded; 8 per boot.
- **No multi-block sources.** A p-fs object is one block today, so source
  files are capped at `PFS_BLOCK_MAX` (4096 bytes).
- **TCC is not GCC.** C99-ish, `-O0`-quality code, no warnings to speak
  of. Fine for the genome-sized programs this stage targets.

## Relationship to evolve

`evolve` (docs/evolve.md) is the *loop*: observe → mutate → build →
deploy. Its weakest link was that "build" meant shipping source to a
mothership and waiting for a human-adjacent compile. `selfc` internalizes
exactly that link: the build step can now happen inside the running
kernel of whichever node needs the code. The two compose — evolve decides
*what* to compile; selfc makes compiling *self-contained*. Wiring evolve's
mutation output into `pfs save` + `selfc run` is the obvious next stitch.
