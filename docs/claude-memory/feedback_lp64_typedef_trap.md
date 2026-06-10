---
name: feedback-lp64-typedef-trap
description: "T-Kernel's typedef.h types W/UW/U4/S4 are spelled with `long`, which is 8 bytes on LP64. On any 64-bit Linux target this silently breaks every struct and global. Spot it via sizeof/offsetof, not by code reading."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 5e4de8fe-6dd6-4370-af76-b4662b3ce1d6
---

T-Kernel `typedef.h` declares the "32-bit integer" family with `signed long` / `unsigned long`:

```c
typedef signed long	W;	/* Signed 32 bit integer */
typedef unsigned long	UW;	/* Unsigned 32 bit integer */
typedef signed long	S4;
typedef unsigned long	U4;
```

On the original i386 target this was correct (ILP32: `long` = 32 bits). On any LP64 ABI (AArch64 Linux, x86-64 Linux), `long` is **8 bytes**, and every UW/W/U4/S4 is silently 64 bits. Symptoms:

- **Globals**: a `UW foo` ends up as an 8-byte slot in `.bss`/`.data`. Anything that reads `foo` as 4 bytes sees only half; anything that reads neighbouring fields sees `foo`'s upper half.
- **Packed network structs**: every UW/W field doubles in size. IP_HDR grows 28 → 36; ARP_PKT grows 28 → 36. Wire-format reads of `ip->dst` end up looking 8 bytes into where the header thinks the field lives — you get garbage and every IP packet gets dropped.
- **TCB layout**: T-Kernel's TCB has W/UW fields, so its size shifts. AArch64's `arch/aarch64/include/offset.h` had `TCB_tskctxb = 200` extracted under the broken assumption — silently load a bad SP, sync abort on the first task dispatch.
- **Hand-rolled atomic ops on globals**: a 32-bit `str w1, [x0]` to a now-64-bit storage location only touches half. We hit this with `knl_taskindp` long before we connected it to the typedef.

**How to apply:** the moment any T-Kernel-derived code is ported to a new ABI, the very first thing to check is the sizeof/offsetof of W, UW, U4, S4, and the TCB. The damage is silent — code reads fine, builds fine, runs fine until you exercise the path where the size matters (wire packet, multi-byte global, asm-vs-C struct offset). A single `_Static_assert(sizeof(UW) == 4)` in a shared header catches it forever.

The fix is to spell these as `int` rather than `long`. `int` is 32 bits on every Linux ABI we currently target (ILP32 and LP64), and it's a portable name for "exactly 32 bits" in this kind of vintage C code. No `#ifdef` needed.

Related: [[moment-2026-05-21-two-node-cluster]] documents the diagnostic session that surfaced this.
