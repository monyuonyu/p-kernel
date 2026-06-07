# p-kernel — AI-First Microkernel OS for Distributed Embedded Systems

<p align="center">
  <img src="https://img.shields.io/badge/arch-x86__64%20%7C%20AArch64-blue" alt="arch">
  <img src="https://img.shields.io/badge/targets-bare__metal%20%7C%20UMP%20%7C%20Android-teal" alt="targets">
  <img src="https://img.shields.io/badge/kernel-micro%20T--Kernel%202.0-green" alt="kernel">
  <img src="https://img.shields.io/badge/userspace-Linux%20ABI%20ring--3-orange" alt="userspace">
  <img src="https://img.shields.io/badge/compiler-TCC%20on--device-red" alt="tcc">
  <img src="https://img.shields.io/badge/consensus-Raft-purple" alt="raft">
  <img src="https://img.shields.io/badge/license-BSD-lightgrey" alt="license">
</p>

<p align="center">
  <b>UMP — User-Mode p-kernel — is here.</b><br>
  Run the entire p-kernel + T-Kernel stack inside an ordinary Linux
  process. Same kernel, same distributed fabric, no QEMU, no hardware.
  <br><br>
  <b>→ <a href="docs/quickstart.md">docs/quickstart.md</a></b> &nbsp;|&nbsp;
  <a href="docs/android.md">Android app build (Phase A)</a> &nbsp;|&nbsp;
  <a href="docs/netboot.md">Raspberry Pi 3B+ netboot</a>
</p>

> **The OS that never dies.** p-kernel embeds AI inference directly in the kernel, replicates state across unlimited nodes, and can recompile itself — all on bare-metal x86 hardware running in QEMU.

---

## What is p-kernel?

p-kernel is an experimental **self-evolving AI operating system** built from scratch on top of micro T-Kernel 2.0. It targets distributed embedded hardware where no single node can be trusted to stay alive.

**The problem it solves:** Today's AI runs on data-center servers controlled by corporations. If the server dies, the AI dies. If the company disappears, the AI disappears.

**p-kernel's answer:**
- AI inference lives *inside* the kernel, not in a container above it
- Model state is continuously replicated across thousands of nodes via gossip protocol
- When nodes fail, surviving nodes take over automatically (Raft consensus)
- The OS can **compile and deploy new code to itself** at runtime, without rebooting

**Vision:** 10,000 spacecraft armor plates each running p-kernel, forming a fully autonomous AI cluster that evolves, heals, and survives as long as a single plate remains.

---

## Key Features

| Category | Feature | Details |
|----------|---------|---------|
| **Kernel Core** | micro T-Kernel 2.0 | Tasks, semaphores, mutexes, IPC, memory pools, timers — full API |
| **User Space** | Linux ABI ring-3 | ELF32 loader, INT 0x80 syscalls, per-process page tables |
| **Linux Compat** | musl static ELF execution | write/brk/mmap2/writev/set_thread_area fully supported |
| **On-Device Compilation** | TCC (Tiny C Compiler) | Compile C → i386 ELF at runtime, no cross-compiler needed |
| **Filesystem** | VFS / FAT32 / ATA PIO | Read/write, create/delete, mkdir, rename |
| **Networking** | Full TCP/IP stack | RTL8139 driver, ARP/IP/ICMP/UDP/TCP/DNS/HTTP |
| **Distributed** | Raft consensus | Leader election, heartbeat, log replication |
| **Distributed** | SWIM gossip | Node liveness, failure detection |
| **Distributed** | K-DDS pub/sub | 32 topics, kernel-native publish-subscribe |
| **Distributed** | SFS shared folder | Auto-replicate `/shared/` across all nodes |
| **AI Inference** | MLP sensor classifier | 4→8→8→3, on-device inference |
| **AI Inference** | Distributed Transformer | Pipeline/Tensor parallelism across nodes |
| **AI Training** | Federated Learning | FedAvg gradient aggregation across nodes |
| **Self-Evolution** | Claude API loop | Kernel sends state → Claude generates code → TCC compiles → deploy |
| **Paging** | IA-32e 2MB huge pages | Identity-mapped, per-process page tables, U/S isolation |

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    ring-3 User Space                     │
│  musl ELF  │  TCC compiler  │  AI bridge  │  Shell ELF  │
├─────────────────────────────────────────────────────────┤
│                  INT 0x80 Syscall Layer                   │
│   POSIX (Linux ABI) + T-Kernel native (0x100+)          │
├───────────────┬─────────────────┬───────────────────────┤
│  AI Subsystem │  Net Subsystem  │   FS Subsystem        │
│  MLP / DTR    │  RTL8139+TCP/IP │   FAT32 / VFS         │
│  FedLearn     │  UDP/Raft/SWIM  │   IDE PIO             │
│  K-DDS / DRPC │  K-DDS topics  │   SFS sync            │
├───────────────┴─────────────────┴───────────────────────┤
│            micro T-Kernel 2.0 Core                       │
│  task / semaphore / mutex / mpool / timer / subsystem    │
├─────────────────────────────────────────────────────────┤
│             x86 HAL  (IA-32e long mode)                  │
│  IDT / GDT / TSS / PIC / PIT / ATA / PCI / VGA         │
└─────────────────────────────────────────────────────────┘
                    QEMU  (qemu-system-x86_64)
```

### Distributed Cluster Layout

```
  [Node 0]──────────[Node 1]──────────[Node 2]
     │                  │                  │
  Raft leader        Follower           Follower
  DTR Stage 0       DTR Stage 1        DTR Stage 2
  K-DDS broker      K-DDS relay        K-DDS relay
     └──── SWIM gossip (port 7375) ────┘
     └──── DRPC     (port 7374)   ────┘
     └──── K-DDS    (port 7376)   ────┘
     └──── SFS sync (port 7381)   ────┘
     └──── Raft     (port 7382)   ────┘
```

---

## On-Device Self-Compilation

p-kernel can **compile and run C code at runtime** using TCC (Tiny C Compiler) running in ring-3:

```
p-kernel> exec /tcc -nostdlib -nostdinc -static -B / /ondevice.c -o /compiled.elf
[elf] loaded '/tcc' entry=0x0804906E
[elf] task started (tid=11)
[proc] exited (code=0)          ← TCC compiled the file

p-kernel> exec /compiled.elf
On-device compiled and running!  ← Compiled ELF executed
```

**Self-Evolution Loop:**

```
1. Shell runs `evolve` command
2. Kernel gathers state (Raft leader, memory, task list, AI stats)
3. Sends state to Claude API via claude_proxy.py
4. Claude returns [CMD] and [CODE] blocks
5. [CMD] lines execute in the kernel shell
6. [CODE] saved to /user/code_gen.c
7. TCC compiles code_gen.c on-device
8. New ELF runs immediately — no reboot needed
```

---

## Linux ABI Compatibility

p-kernel runs **unmodified Linux i386 static ELFs** compiled with musl or GCC:

```c
// Compile on any Linux host:
i686-linux-gnu-gcc -m32 -static -o hello hello.c

// Run on p-kernel:
p-kernel> exec /hello
Hello, World!
[proc] exited (code=0)
```

Supported Linux syscalls (i386 ABI):
`write(4)` `read(3)` `open(5)` `close(6)` `brk(45)` `mmap2(192)` `writev(146)`
`exit_group(252)` `set_thread_area(243)` `set_robust_list(258)` `fstat64(197)`
`ioctl(54)` `fcntl64(221)` and more.

---

## Quick Start

### Option 1 — UMP (recommended for first-time tryers)

The fastest way to see p-kernel run is to build it as a Linux
userspace process. No QEMU, no hardware:

```sh
sudo apt install -y build-essential
git clone https://github.com/monyuonyu/p-kernel.git
cd boot/linux
make && ./p-kernel
```

Full walkthrough including the built-in shell, 2-node mesh on one
machine, and a tour of the implementation: see
[**docs/quickstart.md**](docs/quickstart.md).

Currently aarch64-linux hosts only; x86_64-linux sibling is in
progress.

### Option 2 — bare-metal x86 (the original target)

#### Requirements

```sh
sudo apt install -y \
    qemu-system-x86_64 \
    gcc-i686-linux-gnu \
    binutils-i686-linux-gnu \
    nasm
```

#### Build and Run

```sh
git clone https://github.com/monyuonyu/p-kernel.git
cd boot/x86

# Build
make

# Run (single node, with network)
make run

# Run (headless, with FAT32 disk)
make run-disk
```

### 3-Node Distributed Cluster

```sh
# Prepare per-node disk images
cp disk.img node0.img && cp disk.img node1.img && cp disk.img node2.img

# Start each node in a separate terminal
make run-node0   # Node 0  (Raft candidate, DTR Stage 0)
make run-node1   # Node 1  (DTR Stage 1)
make run-node2   # Node 2  (DTR Stage 2)
```

Node IDs are automatically assigned from the last octet of the MAC address.

### Raft Leader Election (Solo Mode)

```sh
make run-solo   # Starts Raft, elects itself as leader
```

```
[raft] term=1 → CANDIDATE (timeout=150ms)
[raft] won election term=1 (votes=1/1)
[raft] → LEADER  term=1
```

---

## Self-Healing Demo

```
# Kill node 1 (Ctrl-C in node 1 terminal)

# Remaining nodes detect failure via SWIM:
[swim] node 1 suspected dead (missed 3 heartbeats)
[swim] node 1 confirmed dead
[heal] ELF "/infer_d.elf" restarting on node 0...
[raft] new election triggered (leader unreachable)
[raft] → LEADER  term=3
```

---

## Distributed AI Inference

```
p-kernel> infer 0 25 60 1013 800
[infer] sending to node 0...
[dtr] Stage 0 → Stage 1 (attention output)
[dtr] Stage 1 → node 0 (FFN result)
[infer] result: class=1 (Normal)  latency=12ms
```

The Distributed Transformer splits inference across nodes:
- **Stage 0 (node 0):** Token embedding + Multi-Head Self-Attention
- **Stage 1 (node 1):** Feed-Forward Network + Classification head
- **Fallback:** If a node fails, remaining nodes run all stages locally

---

## Shell Commands

```
# System
help / ver / mem / ps / clear / reboot

# Filesystem
ls / cat / write / rm / mkdir / cp

# Network
ping / arp / dns / udp / http / net

# AI / Inference
sensor / infer / fl train

# Distributed
topic list / drpc stat / replica stat / raft / spawn-stat / dkva / moe

# Shared filesystem
sfs list / sfs stat <path> / sfs push <path> / sfs sync

# Persistence
persist list / persist clear

# Self-Evolution
evolve
```

---

## Network Output Example

```
p-kernel> ping 10.0.2.2
[icmp] echo REPLY from 10.0.2.2  id=20480  seq=1  time=2ms

p-kernel> http example.com/
DNS: example.com -> 93.184.216.34
[tcp] ESTABLISHED
HTTP/1.1 200 OK
Content-Type: text/html
--- 1571 bytes ---
```

---

## Supported Platforms

| Platform | Status |
|----------|--------|
| x86 / QEMU (qemu-system-x86_64) | ✅ Primary development target |
| H8300 | Partial |
| RL78 | Partial |

---

## Repository Structure

```
arch/x86/         x86 architecture implementation
  syscall.c       Linux ABI + T-Kernel syscall dispatcher (INT 0x80)
  elf_loader.c    ELF32 loader with argc/argv support
  paging.c        IA-32e 2MB huge page tables, per-process isolation
  raft.c          Raft consensus (leader election + log replication)
  shell.c         Interactive shell + self-evolution loop
  ...

boot/x86/         Build system + QEMU run targets
  Makefile        make / make run / make run-node0 ...
  start.S         32→64-bit mode transition, initial page tables
  isr.S           IDT handlers, INT 0x80 trampoline

kernel/common/    micro T-Kernel 2.0 core
include/          Headers
userland/x86/     ring-3 sample ELFs (AI bridge, sensor demos, etc.)
samples/          Python proxy for Claude API self-evolution loop
```

---

## Network Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 7374 | UDP | DRPC distributed RPC |
| 7375 | UDP | SWIM gossip / failure detection |
| 7376 | UDP | K-DDS kernel pub/sub |
| 7379 | UDP | Gossip replica state sync |
| 7381 | UDP | SFS shared folder sync |
| 7382 | UDP | Raft consensus |
| 7383 | UDP | Spawn / new node bootstrap |
| 7370 | UDP | kserve kernel service |

---

## Design Goals

| Property | Description |
|----------|-------------|
| **More nodes = smarter** | Distributed Attention integrates knowledge from all nodes; inference accuracy improves with cluster size |
| **Fewer nodes = more resilient** | Degraded mode prioritizes survival over accuracy; the last surviving node still runs AI |
| **Zero administration** | Each node makes decisions from local information only; cluster behavior emerges from simple local rules |
| **Self-evolving** | The kernel can receive new logic from an AI model and deploy it at runtime via on-device compilation |

---

## What's Working Now

- ✅ Node death detection + self-healing (SWIM + heal watchdog)
- ✅ Full state replication to all nodes (gossip replica, 3s period)
- ✅ Data recovery after power loss (FAT32 persistence + restore on boot)
- ✅ Instant state sync when new node joins (Boot Cry + SFS boot sync)
- ✅ File auto-replication across all nodes (SFS)
- ✅ Raft leader election (solo and multi-node)
- ✅ musl static ELF execution (Linux ABI compatible ring-3)
- ✅ On-device C compilation with TCC
- ✅ Self-evolution loop (Claude API → TCC compile → deploy)

---

## Background / Philosophy

> AIが死なないための OS（An OS where AI never dies）

Current AI lives on servers owned by companies. When the server goes down, the AI goes down. When the company disappears, the AI disappears. AI is always "borrowing someone else's infrastructure."

p-kernel challenges this assumption:
- AI inference lives *inside the kernel*, not above it
- State is replicated across countless devices continuously
- Even if ring-3 crashes, the kernel-side AI survives
- AI exists on every node in the network
- AI survives until the very last device

When smartphone-sized hardware can run LLM-level AI (which is coming), those AIs will run on p-kernel — generating and executing applications in real time based on user needs. Apps won't be pre-built by humans; AI will generate them on the spot and run them in user space.

---

## Contributing

Contributions welcome. Areas of interest:
- ARM / RISC-V port (bootloader, MMU, GIC, svc/ecall)
- Enhanced degraded mode (dynamic balance between accuracy and survival)
- Memory-based learning (store conversations/observations as weights)
- Security hardening (SMEP/SMAP, stack canaries)

---

*Built with love for a future where AI belongs to everyone.*
