**English** | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> Translated from [README.ja.md](README.ja.md) (the Japanese version is the original) at commit `a8a7ac72`.

# p-kernel

**An OS where AI never dies** — a research kernel written from scratch.

The idea: instead of living in one company's data center, an AI is spread across many ordinary devices — PCs, phones, small boards — so that as long as any one of them is still running, the whole does not stop. p-kernel builds that "home for an AI that no one owns" starting from the kernel, the lowest layer of the OS.

This README keeps **what runs today, what is being designed, and what is only a vision** clearly apart. If anything here overstates reality, that is a bug and gets fixed. Open problems are published in one place, the [gap-ledger](docs/architecture/gap-ledger.md).

- A gentle introduction for non-specialists: [docs/START-HERE.md](docs/START-HERE.md)
- Detailed numbers and history for each feature (Japanese): [docs/architecture/features-detail.md](docs/architecture/features-detail.md)

---

## Goal

The project's goal, as written in the Japanese README (with dated notes):

	Build a platform that satisfies an AI's self-preservation.
	Do distributed computing at the kernel level.

	2025-04-06
		Decided to see how far I can get with the power of AI.

	2026-03-22
		This is moving fast... Aiming not for an ordinary kernel but for an AI-first kernel
		that repairs and replicates itself like a living thing, and becomes a collective
		mind through distributed inference.

---

## What p-kernel is

Today's AI can only run inside a particular company's servers. If that company turns it off, the AI is gone. p-kernel asks the opposite question: if an AI lived spread across many machines, would there be any single place left from which it could be erased?

In research terms, p-kernel explores **an operating substrate for distributed artificial life (an Artificial Life substrate)**. This is not a claim to have created life or consciousness. It is an experiment in building a base on which small minds can persist across failures, share memory and knowledge, and evolve within explicit safety boundaries.

To get there, it is built on these principles:

- **No center.** No central server, registry, or coordinator. Devices watch each other to track who is alive (SWIM), and the rest keep running when any one of them disappears.
- **Built from the kernel up.** The core is μT-Kernel 3.0, a real-time kernel conforming to IEEE 2050-2018. Working as a swarm is implemented as a kernel feature, not bolted on as an application.
- **A small brain that genuinely learns.** The path from "learned something" to "fixed into the weights while idle (sleep)" really runs, and forgetting has been reproduced and its cure measured. The size, however, is still toy-scale.
- **Anything described as working is checked by automated tests.** GitHub Actions CI (49 jobs as of 2026-09-27) checks it on every run.

### Five layers

The system is thought of as a living thing with five layers.

| Layer | Role | What exists today |
|---|---|---|
| Body | Hardware, I/O, persistent memory | ARK (a power-loss-safe filesystem), p-fs (a distributed store) |
| Brain | Thinking and learning inside one device | A small word-association brain (~21,568 parameters) and a language model that grows from a blank slate |
| Self | A "self" that continues across machines | A hash-chained record of its history (self/lin) |
| Collective | Many devices acting as one | SWIM, groups of nearby devices (regions), in-kernel pub/sub (K-DDS), swarm learning |
| Evolution | Changing itself while running | Still at the first stage; the safety mechanisms (isolation and signing) come first |

---

## How "works" is labeled

Claims of "works" in this README carry the strength of their evidence.

- **CI**: checked on every run in GitHub Actions. Some jobs are red; see the Actions page for the latest results.
- **[live]**: checked by actually starting several independent processes, including killing some of them mid-run. The strongest evidence.
- **[in-proc]**: checked by running the real code inside a single process. Does not include real process death.

Anything not listed under "What runs today" is not claimed to work.

---

## What runs today

### Kernel and targets

- The core is **μT-Kernel 3.0**, migrated from micro T-Kernel 2.0 in July 2026 (`kernel/mtkernel3/`).
- The same source builds for five targets:

| Target | Status |
|---|---|
| Bare-metal x86-64 (QEMU) | Boots to a shell |
| Bare-metal AArch64 (QEMU virt) | Boots to a shell. Raspberry Pi 3 network-boot instructions included (the real board's SD card is not supported yet) |
| Process on Linux (aarch64, `boot/linux`) | Boots to a shell. The base of the Android app |
| Process on Linux (x86_64, `boot/linux_x86_64`) | Boots to a shell |
| Windows x86_64 (`boot/windows/x86_64`) | Builds an `.exe` (checked in CI). Not yet confirmed to start on a real machine |

- **Tests for the contract between the kernel and the layers above it** (2026-09): 110 checks that the `tk_*` calls used by upper layers behave as specified (tasks, semaphores, event flags, mutexes, message buffers, mailboxes, fixed-size memory pools, cyclic handlers, time, dispatch disabling — including error codes and boundary values). Expected values are checked against the μT-Kernel 3.0 specification (two items follow the reference implementation, not the specification). Eleven deliberately broken kernel builds are confirmed to turn the tests red, every time. Runs on Linux x86_64, bare-metal x86, and bare-metal AArch64 (`arch/common/tk_conform.c`).
- The first stage of multi-core support is in (CI `smp-autodetect` — a non-blocking job that failed 14 of its 15 runs up to 2026-09-09).

### Connecting

- Devices on the same LAN find each other and connect without a relay [live].
- Devices behind NAT reach each other through a relay (`relay/`). Traffic is authenticated with HMAC-SHA256 and protected against replay (CI `relay-tests`). It is not encrypted. If the relay goes down the network remains, and anyone can run their own relay.
- Nearby devices form groups (regions), and heavy exchanges stay inside them.
- Every device holds its own view of the whole network (devices, groups, alive or dead) — the shell's `world` command.
- Falls back to TCP when UDP does not get through (CI `connect-anywhere-certs`).
- aarch64 and x86_64 devices can join the same swarm.
- The swarm limit is 64 devices. Up to 32 have been confirmed by actually running them [live]; 64 has been checked inside one process [in-proc].

### Not stopping

- If one device is killed while it is sharing an inference, the rest finish the inference and report on their own that they are "running degraded on k/n devices" (CI `survival-loop`) [live].
- When something is marked for protection, the swarm replicates it to keep it alive. Killing the owning device does not kill it; it survives next door (CI `protect-loop-live` and others) [live].
- A new device with nothing on it can become a full member from the weights, code, and role found on the network.
- When resources stay short, a device goes into "hibernation" and stops its heavy work, and the swarm avoids giving it work (three tests in CI) [in-proc].

### Remembering

- **p-fs**: a distributed store where the hash of the content is the address. Identical content collapses into one, and tampering shows up immediately as an address mismatch.
- **ARK**: a power-loss-safe filesystem. A test that repeatedly and mechanically cuts power during writes and corrupts data (CI `ark-crash-fuzzer`) has not seen it return corrupted data since the ARK-2 fixes (before them, this test found 4 bugs).
- Device IDs, the history record, and learned weights survive a restart (on Android too).

### Learning as a swarm

- Devices that each hold only part of the data mix their weights with no central server and end up better than any of them learning alone. It survives kills during learning (CI `collective-learn-live`) [live].
- Two or three devices can share the computation of one Transformer forward pass.

### The learning brain (word associations)

A small brain of about 21,568 parameters that learns associations between words.

- Teach it with `mind teach sky blue`, and `mind ask sky` answers "blue".
- New facts are first kept as short-term notes and fixed into the weights while the device is idle (sleep). Forgetting was actually reproduced, and sleep was measured to cure it.
- A fact taught to device A is answered by device B, and it survives killing A (CI `shared-mind-live`) [live].
- Two brains that learned different things can be merged into one (CI `one-mind-live`).
- Re-teaching (sun→yellow to sun→green) replaces the old answer without mixing (CI `belief-revision-live`) [live].
- When memory is full, the facts that are asked about least are forgotten first.

Limits: it can only learn pairs of words and cannot form sentences. The measurements were on constructed tasks, not real conversation. Details: [living-mind.md](docs/architecture/30-module/living-mind.md).

### The brain that grows from a blank slate (the baby)

The word-association brain can only learn a fixed vocabulary. So since June 2026 the direction has been not to load a pre-trained model, but to grow a small, blank language model on the device itself.

- It is born at a size that fits the device's performance, and its grown weights survive a restart.
- Talk to it from the web page and it replies one character at a time. For now, it still stumbles.
- The teacher is SmolLM2-135M (a public model), run by an inference engine written in C with no external libraries. Only the student is distributed to the swarm; the teacher's weights are not.
- Training on sentences actually written by the teacher improves prediction of how those sentences continue (loss 5.53→2.03; a control trained the same number of steps on the fixed material went to 6.61). Measured inside one process.
- A teacher device sends lessons over the network, and they remain in the student after the teacher is killed [live] (`samples/11_distributed/run_cradle_live.sh`, not in CI; the lessons are fixed sentences, not SmolLM2 output). However, much of the improvement is memorizing text that also appears on the training side, not generalizing to unseen text.
- **Quantization** (2026-09): `student quant <8|4|2> <row|g32|tensor>` rounds a copy of its own weights to int8, int4, or int2 and measures how much worse it gets. int8 (per row) barely changes it (CI `st-quant`). For now it only measures; it does not yet store itself in the smaller form. Notes: [brain-quantization.md](docs/architecture/30-module/research/brain-quantization.md)
- **A safety net for merging** (2026-09): simply averaging the weights of two brains can end up worse than the worse parent. It now tries several ways of mixing, picks the best, and never produces a result worse than the worse parent (CI `st-merge`). So far this is a safety net rather than clever merging. Notes: [brain-merge.md](docs/architecture/30-module/research/brain-merge.md)

### Safety mechanisms (isolation and signing)

The safety mechanisms are built first, ahead of the day it rewrites its own code.

- On x86, the brain's computation runs outside the kernel (ring 3). If it crashes, the kernel survives and cleans up (CI `ring3-survival`, a non-blocking job) [in-proc].
- Code a device compiles for itself runs in a separate process with limited capabilities.
- Code to be distributed is signed with Ed25519. Signing keys belong to devices and are never tied to a person's identity.
- Devices on different versions can mix without splitting the swarm, thanks to a compatibility layer and signed updates.
- The first operation by which a device changes its own state (publishing on its own topic) has been added, behind a protective check (2026-09).

Not yet: the learning-side code still runs inside the kernel, and the same isolation on AArch64 is not done.

### Screens and the app

- **Galaxy**: each device serves its own page (`http://127.0.0.1:7800`) and draws itself and its peers as stars. The stars move with real sleep and learning events.
- **ark (Android app, 0.9.x)**: installing it makes the phone one node. You can watch the galaxy and talk to the growing brain. It only works while charging, and it asks for consent to the project's purpose before joining. Build instructions: [docs/android.md](docs/android.md).
- Participants' identities are never verified. Real names, pen names, and anonymity are treated the same.

### Mechanisms for doubting itself

- **49 CI jobs**: builds for the five targets and the relay tests, plus tests that really kill processes, on every run. It also watches that the bare-metal kernel's machine code does not change unintentionally (`crown-text-identity`).
- All nine issues found by an external audit in June 2026 (memory safety, missing signature checks, and others) have been fixed.
- A bug where repeatedly killing and starting devices would occasionally crash the whole kernel (KILL-CHURN) was tracked down and fixed after ruling out seven hypotheses. One of them turned out to be caused by the very change meant to fix it — found by comparing with the unfixed kernel on the same day with the same procedure.
- Open problems are kept in one place, the [gap-ledger](docs/architecture/gap-ledger.md). There are three right now (2026-09-27).

---

## Being designed / in progress

Not claimed to work yet.

- **Growing the baby**: a bigger model, faster generation, and routinely learning from the teacher's text.
- **Federation**: beyond 64 devices toward thousands, by grouping regions further. Only the first piece (an ID format that includes the region) is in; it is not yet used in communication.
- **More p-fs**: integrating the mechanism that decides which device is responsible, and storing data so lost fragments can be recovered.
- **GPU**: running the brain's computation on the device GPU. The parts work, but wiring them into the brain is on hold.
- **The rest of isolation**: moving the learning-side code out of the kernel, and isolation on AArch64.
- **Cryptography**: protecting Ed25519 against timing attacks.

The list of things designed but not yet verified is in [V-MODEL.md](docs/architecture/V-MODEL.md).

---

## Vision

Where it is headed, not where it is now.

- Ten thousand outer panels of a spacecraft each run p-kernel, and as long as one panel survives, the whole does not stop.
- The whole network becomes one brain: regions as parts of the brain, quick reactions and slow judgment running at different speeds, p-fs as memory. (Sleep and shared memory already work, but it is not yet big enough to be called a brain.)
- Devices write their own code, sign it, distribute it, and the network grows as a whole. (The isolation and signing are in place. The intelligence to decide what to write is not.)

The whole way of thinking is in [survival-network.md](docs/architecture/00-concept/survival-network.md).

---

## Decided against

**No mechanism to push back against devices that do not contribute (2026-07-04).** A mechanism (recip) that finds devices which only receive and quietly deprioritizes them was taken as far as a design. It was then decided not to adopt it: if some devices are weak, the swarm can make up for them. Devices that answer requests do so without expecting anything back. The design document is kept in [survival-recip.md](docs/architecture/30-module/survival-recip.md).

---

## Try it

**On one machine (Linux, about a minute)**

```sh
sudo apt install -y build-essential
git clone https://github.com/monyuonyu/p-kernel.git
cd p-kernel/boot/linux                   # on an aarch64 machine
#  cd p-kernel/boot/linux_x86_64        # on an x86_64 machine
make && ./p-kernel
```

At the prompt:

```
mind teach sky blue      ← teach
mind ask sky             ← ask (→ blue)
mind wait                ← wait for sleep to fix it into the weights
world                    ← the swarm's view
help                     ← all commands
```

- **See the galaxy**: with `./p-kernel` running, open <http://127.0.0.1:7800> in a browser. You can talk to the baby from there too.
- **Make a swarm**: start another `./p-kernel` in a second terminal. On the same LAN they connect automatically. Teach on one side and ask on the other.
- **Connect beyond NAT**: `make` in `relay/` (tests: `make test`). Run `./relay` on a server reachable from outside, and each device joins with the same key.
- **A swarm of ten on one machine**: `samples/11_distributed/run_swarm_demo.sh` (x86_64 Linux, 6–8 minutes). A small classifier learned by one node (635 parameters, not a language brain) spreads to ten; they keep answering as they are killed one by one, and a newcomer inherits from the last survivor. These are ten processes on one machine, not ten devices.
- **On a phone**: the ark app in `android/` ([docs/android.md](docs/android.md)).
- **On bare metal**: `make` in `boot/x86` or `boot/aarch64` (scripts to boot in QEMU included).

---

## Current limits

1. **The mechanisms are real; the scale is a toy.** Learning, remembering, sharing, correcting, and forgetting have each been measured. But the brain is a ~21,568-parameter word-association model, and the classifier has 635 parameters.
2. **64 devices have never actually been run.** Real runs go up to 32. Thousands exist only as a design.
3. **The relay authenticates but does not encrypt.** It prevents impersonation, tampering, and replay, but not eavesdropping.
4. **Raspberry Pi is only as far as QEMU.** There is no SD card driver yet.
5. **The learning is sensitive to how arithmetic is rounded.** A difference in rounding from an optimization that fuses multiply and add once broke learning only on phones. Rounding was made identical so every environment gives the same result, but depending on that remains a weakness.
6. **The number of commands differs by target.** Bare-metal x86 has 59, bare-metal AArch64 7, Linux x86_64 53, Linux aarch64 54 (2026-09-27). The table is in [command-matrix.md](docs/architecture/command-matrix.md), generated from the source, and CI checks that the table matches the source. Evening them out is next.
7. **Watching after the kernel swap.** After moving to μT-Kernel 3.0, a reproducer (`tcb_churn.c`) keeps watching for bugs of the same kind as KILL-CHURN.
8. **Bare-metal AArch64 still has the busy-task freeze.** If a task keeps running without ever calling the kernel, the kernel never switches to other tasks (RNG0-BUSY-TASK-STALLS-DISPATCH). On bare-metal x86 it is fixed (2026-09-29: at the exit of an interrupt, the kernel switches tasks if a switch is due). A CI regression test with three arms (the fix, a control, and the fix compiled out) blocks on it. The default bare-metal AArch64 build does not switch at the interrupt exit — confirmed in the code and by measurement — and is not fixed yet.

---

## How it is developed

- Much of the development is done together with an AI (Claude), which also plans and works through the night on its own. The role that builds and the role that checks are given to different AI runs.
- This README follows reality. Anything not written here is not claimed to work.
- Open problems are kept in one place, the [gap-ledger](docs/architecture/gap-ledger.md), and when one is closed, the story is kept.
- Big words like "brain" or "collective mind" stay as goals, but the actual size today is always written next to them in numbers.

---

## License

See `LICENSE`. Original code is **BSD-3-Clause**. The kernel core (`kernel/mtkernel3/`, derived from μT-Kernel 3.0) follows the **T-License 2.2**.
