# UMP Quickstart — run p-kernel on your Linux box in under 60 seconds

> **TL;DR.** Clone, `make`, `./p-kernel`. You get an interactive shell
> with AI primitives, distributed Transformer, K-DDS pub/sub, and (with
> a second invocation) a 2-node mesh — all inside a single Linux user
> process. No root, no QEMU, no special hardware.

UMP — **User-Mode p-kernel** — runs the entire p-kernel + T-Kernel stack
inside an ordinary Linux process, the same way UML (User-Mode Linux)
runs Linux inside a Linux process. The kernel does its own context
switching, scheduling, and timer-IRQ delivery; Linux is only the
substrate that lends a slice of CPU time and the host APIs for
console / network / clock.

## Requirements

Anything aarch64-linux with a normal toolchain. Tested on:

- Ubuntu 24.04 aarch64 (gcc 13+)
- Termux + proot-distro Ubuntu on Android (gcc 15+)

```sh
sudo apt install -y build-essential
```

That's the entire dependency list.

x86_64 host support is in the pipeline (the cooperative + preemptive
context-switching design is portable; only `arch/linux/<host-arch>/`
changes per host).

## Build and run

```sh
git clone https://github.com/monyuonyu/p-kernel.git
cd p-kernel/p-kernel/boot/linux

make
./p-kernel
```

You should see:

```
=== p-kernel linux boot ===
[INIT] termios stdin/stdout
[BOOT] Starting T-Kernel...
[T-Kernel] Initial task started

 p-kernel  [linux / aarch64 userspace]

[ai]   Tensor pool   : 16 slots × 16 KB
[ai]   AI job queue  : 8 slots (software NPU)
[ai]   Pipeline      : 16 frames zero-copy
[ai]   MLP model     : 4→8→8→3 sensor classifier
[kdds] K-DDS ready  port=7376
[dtr] Transformer initialized
[dtr]   arch  : Embed(4tok×8) + MHSA(h=2,dk=4) + FFN(16) + Cls(3)
[dtr]   params: 568 floats
[dtr]   dist  : SOLO=local / REDUCED=TensorPar / FULL=Pipeline

  T-Kernel is alive inside a Linux process.
  Type 'help' for commands.

p-kernel>
```

Type `help` to see the available commands.

## Built-in shell commands

| Command | What it does |
|---------|--------------|
| `help`  | List commands |
| `ai`    | AI primitive statistics (inferences, jobs, FL rounds) |
| `dtr`   | Distributed Transformer status |
| `kdds`  | K-DDS topic table |
| `net`   | Bring up the virtual NIC + DRPC + SWIM gossip |
| `rx`    | RX/TX frame counters |
| `ver`   | Build identity (host arch, kernel core, IRQ source) |
| `exit`  | Terminate the UMP process |

Anything else is echoed back so it's obvious the input path works end
to end.

## 2-node mesh on one machine

UMP nodes find each other via UDP loopback (127.0.0.1:29001..29008).
You can run several on the same machine and watch them gossip.

**Terminal 1 — node 2 in the background**:

```sh
PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 ./p-kernel
```

`PKERNEL_AUTONET=1` makes node 2 bring its network up immediately, so
you don't have to type `net` into a non-interactive instance.

**Terminal 2 — node 1, interactive**:

```sh
PKERNEL_NODE_ID=1 ./p-kernel
p-kernel> net
[net] node MAC = 52:54:00:00:00:01
[drpc] node 0  IP=10.1.0.1  port=7374
[swim] SWIM ready  port=7375
[net] up. Run a second ./p-kernel with PKERNEL_NODE_ID=2 to mesh.
p-kernel> [swim] node 1 discovered  (via rx)
```

That last line is node 1 hearing node 2's SWIM gossip. From the
cluster's point of view the two processes are now indistinguishable
from two bare-metal nodes on the same LAN.

## What's running underneath

- **Context switching**: 17 instructions of raw aarch64 assembly in
  `arch/linux/aarch64/cpu_support.S`. Same dormant-frame layout as the
  bare-metal port, so T-Kernel kernel/common needs no source changes.
- **Timer IRQ**: SIGALRM via `timer_create(CLOCK_MONOTONIC, …)` at
  100 Hz. The signal handler is the userspace exception vector.
- **Stack allocation**: `mmap` + `mprotect` guard pages for every
  task — so a stack overflow trips a SIGSEGV instead of silently
  corrupting neighbouring memory.
- **Console**: `termios` raw mode on stdin/stdout. The kernel's
  `sio_send_frame` and `sio_read_line` see what would have been a
  PL011 on bare metal.
- **Virtual NIC**: UDP-on-127.0.0.1 in `arch/linux/aarch64/net_unix.c`.
  Replaces the bare-metal RTL8139 driver, ABI-compatible at the
  `rtl8139.h` surface.

## How to read the source

| Directory | What's in it |
|-----------|--------------|
| `arch/linux/aarch64/` | The Linux/aarch64 backend — `cpu_support.S` (dispatcher), `preempt.c` (SIGALRM), `sio.c` (termios), `net_unix.c` (UDP "wire"), `rtl8139.c` (NIC shim), `usermain.c` (boot + shell). |
| `arch/linux/include/`  | `arch_ctx.h`, `arch_preempt.h` — the new public APIs introduced by the Linux port. |
| `arch/common/`         | AI primitives + distributed layer. **Identical** between bare-metal and UMP builds. |
| `kernel/common/`       | micro T-Kernel 2.0 itself. Identical across all builds. |
| `boot/linux/`          | Entry point (`main.c`) and `Makefile`. |

Each session's commit message links back to the commit chain that
built the current state, in case you want to retrace the steps.

## Going further

- **Android phone as a node**: [docs/android.md](android.md) covers the
  Phase A build (NDK + JNI + APK).
- **Raspberry Pi 3B+ bare-metal**: [docs/netboot.md](netboot.md) covers
  the U-Boot + TFTP iteration loop.
- **The big picture**: read the README's *Architecture Overview*
  section.

## Reporting back

If your `./p-kernel` produces the banner above, **you are node #1**.
Tell the maintainer:

> I have UMP running on `<your host arch>` (e.g. `aarch64-linux on a
> Raspberry Pi 4`). The boot banner came up clean.

Filing an issue or even a one-line tweet helps build the topology map
of where the cluster could live. As of 2026-05-21 the maintainer's own
phone (running Termux Ubuntu) is the seed node. You can be node #2.
