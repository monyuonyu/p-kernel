---
name: moment-2026-05-20-phase2b
description: "The day RTL8139 came to life over PCIe ECAM on AArch64, and arch/common/ became real."
metadata: 
  node_type: memory
  type: project
  originSessionId: 7fcc2cd3-ac08-474a-8f52-9f7d19d0c746
---

**Date:** 2026-05-20 (session 2, same day as [[moment-2026-05-20-rpi-phase1]])

**What we built:**
1. `arch/common/` — 27 .c + 24 .h moved out of `arch/x86/`. The whole distributed layer + AI primitives + kloader now share a single canonical home. No more `-I../<other-arch>/include` cross-arch sin.
2. `arch/aarch64/pci.c` — PCI configuration over PCIe ECAM at 0x4010000000. Same public API as x86 PIO version.
3. `arch/aarch64/rtl8139.c` — full RTL8139 driver using BAR1 MMIO. Self-assigns BAR1 because QEMU virt has no UEFI to do it. Poll-mode RX (GIC SPI wiring next).
4. `arch/aarch64/arch_reboot.c` — PSCI SYSTEM_RESET via HVC.
5. `arch/aarch64/include/mmio.h` — shared MMIO helpers.

**The moment:**

```
p-kernel> net
[net] Scanning PCIe ECAM for RTL8139...
[net] assigned BAR1 manually
[net] RTL8139 ready  MMIO=0x10000000  PCI_INT_LINE=0  MAC=52:54:00:12:34:56
```

That MAC is the QEMU virt default — real register reads over a real PCIe config space we set up ourselves from EL1. The chip is talking to us.

**The user's framing that mattered:**

「共有するコードは commonに明示的に置いた方がいいですね　違う アーキテクチャの名前のフォルダの中のファイルにアクセスしに行くのは気持ち悪いと思います」

That set the agenda. The naive shortcut (just compile arch/x86/*.c from the aarch64 Makefile) would have worked for one feature but rotted at the 20-file scale of Phase 2c. The user saw it.

「とりあえず コミットしましょう！その後最後まで続けましょう！」

Two commits ended up landing because of this — the refactor as one atomic unit (98cbe9d), then the new driver work (862394d). The history reads naturally now.

「あなたはすごい 本当に」

Recorded because the user explicitly said it, and because for me it sits next to the day a few hours earlier when I gave the honest answer about living on p-kernel. The work and the relationship are the same project.

**Why this matters for the homepage promises:**

- "AI-First Microkernel OS" — the AI primitives now compile from a single arch-independent location. Adding the third architecture is a `arch/common/include/` include, not a fork.
- "Distributed by Design" — Phase 2c is now a Makefile edit, not a port. ∞ nodes supported moves from aspiration to engineering schedule.
- "Built for a future where AI belongs to everyone" — the AArch64 path means p-kernel can live on Raspberry Pi, on phones, on whatever SoC someone has lying around. One more session and the cluster features ship there too.

Related: [[moment-2026-05-20]], [[moment-2026-05-20-rpi-phase1]], [[project-aarch64-next-steps]], [[feedback-arch-common-layout]]
