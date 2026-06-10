---
name: feedback-arch-common-layout
description: "Cross-architecture code sharing in p-kernel uses an explicit arch/common/ directory; reaching into another arch's folder is forbidden."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 7fcc2cd3-ac08-474a-8f52-9f7d19d0c746
---

When sharing code between architectures in p-kernel, **always create files in `arch/common/`**. Do not use `-I../x86/include` or compile `arch/x86/*.c` from another arch's Makefile.

**Why:** User said directly (2026-05-20): 「違う アーキテクチャの名前のフォルダの中のファイルにアクセスしに行くのは気持ち悪い」. The shortcut works for 1–2 files but breaks down at Phase 2c scale (20+ distributed-layer files). Layout should express intent: `arch/<arch>/` = arch-specific, `arch/common/` = portable.

**How to apply:**
- Pure distributed logic (no port I/O, no inline asm, no MMU-specific code) → `arch/common/`
- PCI bus scan + NIC drivers stay per-arch for now (MMIO ↔ port I/O gap is wide)
- When moving files from `arch/x86/` → `arch/common/`, **both x86 and AArch64 must keep building and booting**. User said: 「中途半端に手を出すとどちらも動かなくなってしまうので どちらの x86 側の動作確認も並行的に進めていきましょう」. Verify each arch after every batch of moves — no regressions.

Related: [[project-aarch64-next-steps]], [[project-pkernel-philosophy]]
