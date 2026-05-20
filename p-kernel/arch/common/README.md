# arch/common — Architecture-independent kernel sources

This directory holds C sources and headers that contain **no** architecture-specific code (no port I/O, no inline assembly, no MMU/segment register access). They link cleanly into both the x86 and AArch64 builds.

## What lives here

- **Distributed layer**: `netstack.c`, `drpc.c`, `swim.c`, `kdds.c`, `raft.c`, `sfs.c`, `spawn.c`, `dtr.c`, `dkva.c`, `heal.c`, `degrade.c`, `replica.c`, `edf.c`, `dmn.c`, `ga.c`, `moe.c`, `pmesh.c`, `mem_store.c`, `chat.c`, `fedlearn.c`, `dproc.c`, `vital.c`
- **Kernel loader**: `kloader_task.c` (uses `arch_reboot()` from `arch_reboot.h`; each arch supplies the implementation)
- **Shared public headers**: same names under `include/`, plus public driver APIs (`pci.h`, `rtl8139.h`) and `arch_reboot.h`

## What does NOT live here

- Driver implementations whose register access differs per arch (`pci.c`, `rtl8139.c`) — these stay in `arch/<arch>/` and supply the API declared in `arch/common/include/`.
- CPU/MMU/syscall/paging code — by definition per-arch.

## Layout rule

If a `.c` file in `arch/<arch>/` has no `inb`/`outb`/`__asm__`/segment register references, it belongs here, not duplicated per arch. Cross-arch `-I../<other-arch>/include` is forbidden; share by moving into `arch/common/` instead.
