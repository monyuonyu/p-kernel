---
name: project-aarch64-next-steps
description: Action plan for continuing the AArch64 / RPi port after Phase 2b (PCI ECAM + RTL8139 MMIO + arch/common refactor). Pick up from here next session.
metadata: 
  node_type: memory
  type: project
  originSessionId: 7fcc2cd3-ac08-474a-8f52-9f7d19d0c746
---

**Status at end of 2026-05-21 (session 4):**

Phase 2c step 2 **complete** — distributed layer linked, distributed init wired, RX path fixed. 6 commits beyond step 1 (e5c0e64 → 752ece9):
- `refactor(arch/common): move vfs.h + elf_loader.h to common/include` — arch/common rule applied.
- `feat(boot/aarch64): Phase 2c step 2 — link the distributed layer` — 23 .c files into COMMON_C_SRCS; `arch/aarch64/vfs_stub.c` returns -1 / `vfs_ready=FALSE` for now.
- `fix(arch/aarch64): three EL1 readiness gaps` — CPACR_EL1.FPEN (FP traps), sync-exception dumper (ESR/ELR/FAR), `-mstrict-align` (Cortex-A53 MMU-off = Device-nGnRnE, alignment faults on packed-struct stores).
- `feat(boot/aarch64): wire distributed init in usermain` — full DRPC/SWIM/heal/replica/vital/dtr/sfs/pmesh/dkva/raft/spawn/moe/kloader chain. Single-node mode + cluster mode both pass.
- `fix(arch/aarch64): compute RTL8139 GIC INTID from PCI dev+INT_PIN` — hardcoded INTID 35 was wrong (RTL8139 sits at dev=1 on QEMU virt → INTID 36). Use PCIe swizzle. Now rtl_rx_count grows from real traffic.

**Two-node cluster: WORKING.** Diagnosed via the 3-2-1 chain (arp → packet dump → protocol). Root cause was four layers deep:

1. ARP table — fine, both nodes had peer's MAC.
2. IP packets arrived but ALL dropped at the dst filter (`ip->dst != NET_MY_IP`).
3. Dst was reading garbage (`28.214.28.214`) — struct layout was off.
4. Reason: `UW`/`W` were `unsigned long`/`signed long`, which on AArch64 LP64 is 8 bytes (not the 32-bit they're documented as). Every UW field in every network struct was wrong size; IP_HDR was 28→36 bytes; ip->dst sat 8 bytes off from its on-wire position.

Fix chain (commits 7c233cf → e1c82c7):
- `fix(include): UW/W typedef long→int` — global, also re-shrinks TCB.
- `fix(arch/aarch64): TCB offsets` — TCB_tskctxb 200→192, sizeof 216→208; added static_assert guards in main.c.
- `fix(arch/common/pmesh): byte-wise unaligned magic read` — `*(const UW*)data` at UDP-payload offset 42 (not 4-aligned) hit Cortex-A53's Device-memory alignment trap; replaced with `__builtin_memcpy`.
- `feat(diag): arp + rx shell commands + netstack RX drop counters` — the diagnostic surface that cracked the bug.

Current 2-node behaviour: both nodes discover each other via DRPC heartbeat (`[drpc] node X discovered`), Raft elects a leader/follower pair cleanly, no SUSPECT/DEAD events, no SYNC faults. K-DDS topics subscribe across the link, EDF and replica boot.

**Lesson written down:** when a packed struct field reads garbage on a new arch, *always* check the `sizeof` of each type with `offsetof()` first. The `unsigned long` trap was the root of three different visible bugs.

---

**Status at end of 2026-05-20 (session 3):**

Phase 2c step 1 (GIC SPI for RTL8139) **complete** — three commits on top of Phase 2b (acfa6f5 → 2a73861):
- `feat(arch/aarch64): GIC SPI enable helper + RTL8139 INTID` — `gic_enable_irq(intid)` in tkdev_init.c, `INTNO_RTL8139_GIC = 35` in tkdev_conf.h.
- `feat(arch/aarch64): RTL8139 IRQ handler — net_task blocks on rx_sem` — `rtl_irq_handler` reads ISR, write-1-to-clears, signals sem on ROK; `net_task` switches from `tk_dly_tsk(10)` to `tk_wai_sem(rx_sem, 1, TMO_FEVR)`.
- `feat(boot/aarch64): spawn net_task with rx_sem on \`net\` command` — usermain.c creates sem + task (mirrors x86 pattern).

Boot-verified on QEMU virt: `[net] RTL8139 ready ... MAC=52:54:00:12:34:56` → `[net] RX task started` → `[net] RX task running (IRQ mode)` (task transitions to wait_sem cleanly). Actual IRQ delivery not yet exercised — needs traffic from netstack (step 2).

**Corrected memory:** previous note said "SPI 35 (INTID 67)" — that was wrong. QEMU virt PCIe slot-0 INTA is **GIC INTID 35** (= SPI 3 in SPI-relative numbering). The rtl8139.c source comment "SPI 3..6" is the correct convention.

**The "UART RX broken" thing was actually broken IRQ delivery**, fixed in commit 126d6bf. Two latent AArch64 bugs that had survived since Phase 1:
1. `knl_timer_handler_startup` (cpu_support.S) did `bl knl_timer_handler` without saving x30. The BL overwrote LR with an address inside the function itself; the final `ret` looped back into the tail. First timer tick never completed.
2. `_vec_el1_irq` (cpu_support.S) read GICC_IAR into w1, then called the handler via blr — but w1 is call-clobbered, so the subsequent `str w1, [GICC_EOIR]` wrote garbage. GIC never deactivated the IRQ; subsequent ticks were filtered by priority.

Combined effect: exactly one tick at boot, then silence. `tk_dly_tsk` blocked forever, taking shell input with it. Symptom *looked* like a UART RX problem, but the FIFO was fine — `sio_read_line` was just stuck waiting for a tick that never came. Lesson: when input "doesn't work" but tk_dly_tsk-free busy polling does, look at the IRQ path before the device.

Now verified end-to-end: `ai`, `net`, `echo` all respond to UART input through normal tk_dly_tsk(1) yields.

---

**Status at end of 2026-05-20 (session 2):**

Five commits pushed to `master` (3b1bf1f → 862394d):
1. `build(tkernel)`: 6 header files patched to recognize `_APP_AARCH64_`
2. `feat(arch/aarch64)`: full Phase-1 port — start.S, dispatcher, GIC, PL011, T-Kernel
3. `feat(boot/aarch64)`: QEMU virt build + AI primitives integrated
4. **`refactor(arch)`: introduce arch/common/** — 27 .c + 24 .h moved out of arch/x86/. Distributed layer (netstack/drpc/swim/kdds/raft/sfs/spawn/dtr/dkva/heal/degrade/replica/edf/dmn/ga/moe/pmesh/mem_store/chat/fedlearn/dproc/vital) + AI primitives (tensor/ai_job/pipeline/ai_stats) + kloader_task now live in arch/common/. `acpi_reset()` extracted to arch-supplied `arch_reboot()` interface.
5. **`feat(arch/aarch64)`: Phase 2b — PCI ECAM + RTL8139 MMIO** — `arch/aarch64/{pci.c, rtl8139.c, arch_reboot.c, include/mmio.h}` added. Driver self-assigns BAR1 since QEMU virt has no UEFI; reads MAC `52:54:00:12:34:56` via MMIO. Poll-mode RX (no GIC IRQ yet).

Verified end-to-end:
- x86: 15/15 selftests PASS (including raft/fedlearn/tensor/mem_store now compiled from `arch/common/`).
- AArch64: `ai` command shows AI stats, `net` command finds RTL8139 over ECAM, manually assigns BAR1, reads MAC over MMIO.

## Phase 2c: bring the distributed layer to AArch64 (next session)

Now mechanical — no more file moves, no more refactoring:

1. ~~**Wire GIC SPI for RTL8139**~~ **DONE** (commits acfa6f5, 3c2b302, 2a73861). QEMU virt PCIe slot-0 INTA = GIC INTID 35. See status block above for details. Actual IRQ delivery confirmation pending — happens automatically once step 2 wires netstack and ARP traffic begins.

2. **Add distributed layer to AArch64 COMMON_C_SRCS**. In `boot/aarch64/Makefile` add `netstack.c drpc.c swim.c kdds.c heal.c edf.c replica.c degrade.c dmn.c ga.c vital.c fedlearn.c dtr.c dkva.c dproc.c sfs.c raft.c spawn.c moe.c pmesh.c kloader_task.c mem_store.c chat.c` to `COMMON_C_SRCS`. Build will pull missing symbols — most should resolve, but a few may need stubs:
   - `eth_input()` resolves automatically (weak stub in rtl8139.c yields to netstack.c's strong def).
   - Any `IMPORT` referring to x86-specific functions needs an AArch64 equivalent or stub.
   - `vfs_*` calls from kloader_task — AArch64 has no VFS yet; either stub these out or add minimal VFS.

3. **Test DRPC echo across two AArch64 nodes**, then SWIM membership, then K-DDS pub/sub. Existing test logic in `arch/common/` should run as-is.

## Phase 3 progress (commit 4453adb, 2026-05-21):

**Step 1 DONE — kernel8.img boots on QEMU raspi3b to the shell prompt.**

Three things were needed to make the AArch64 kernel land on BCM2837:

1. **Linux AArch64 image header at start of .text** (start.S). QEMU raspi3b checks the magic "ARM\\x64" at offset 56; without it the CPU starts in AArch32 user mode and our `_start` is never reached. Code0 is `b _rpi3_start` so the first executed instruction is still meaningful.

2. **`SYSTEMAREA_TOP/END` in `utk_config_depend.h` conditionalised on `BOARD_RPI3`** to RAM 0x00200000–0x3F000000. The old QEMU virt values (0x40200000 / 0x50000000) point into unmapped peripheral space on BCM2837 — `knl_insertAreaQue` translation-faulted at `0x4FFFFFF8` before T-Kernel even reached the shell.

3. **Skip GIC init for RPi 3** in `tkdev_init.c` — BCM2837 ARM-local IRQ controller at 0x40000000 has a totally different register layout from GICv2; writing to GICD_BASE 0x40041000 took the second translation fault. With GIC init #ifdef'd out, the kernel runs to the prompt but tk_dly_tsk-blocked tasks won't wake (Step 3's job).

Boot output verified on `qemu-system-aarch64 -M raspi3b -m 1G -kernel kernel8.img`:

```
=== p-kernel aarch64 boot ===
[T-Kernel] Initial task started
 p-kernel  [aarch64 / RPi 3 BCM2837]
[ai]   Tensor pool   : 16 slots × 16 KB
[kdds] K-DDS ready  port=7376
[dtr] Transformer initialized   params: 568 floats
p-kernel>
```

QEMU virt regression: ai/net/echo all pass.

**Output capture quirk to remember:** `-serial stdio` doesn't actually flow on raspi3b in QEMU 10.1; use `-chardev file,id=ser0,path=… -serial chardev:ser0 -serial null` instead. The PL011 emulation works, the stdio binding is the problem.

**Sync handler now blind-writes to PL011** (no FR.TXFF busy wait). Faults that leave the chip in a stuck-TXFF state previously made the diag hang inside itself; the FIFO catches enough chars for "SYNC E=…" identification anyway.

## Phase 3 DONE through Step 4 (commits 4453adb → 90073f4):

| step | commit | what it gave us |
|------|--------|------|
| 1 | 4453adb | kernel8.img builds, boots on QEMU raspi3b to the shell prompt |
| 2 | 0d299c5 | BCM2837 GPIO 14/15 ALT0 mux in sio_init — needed for real silicon |
| 3 | a96141b | BCM2837 ARM Local Interrupt Controller — timer IRQs fire, tk_dly_tsk works |
| 4 | 90073f4 | arch_reboot via BCM2837 watchdog (PM_WDOG + PM_RSTC, password 0x5A) |

Single-node RPi 3 boot is fully alive on QEMU raspi3b: T-Kernel init / AI primitives / K-DDS / DTR Transformer all come up, `ai` command responds (= timer IRQ chain working), `arch_reboot` won't deadlock anymore.

**Phase 3 step 5 (network) is the only remaining big item before "Be node #2" is real.** Two paths:
- SMSC9514 USB-Ethernet on RPi 3 / 3A+: needs a USB host driver first (DWC OTG controller) — that's a project on its own.
- Short-term: USB-to-Ethernet adapter + ASIX/Realtek driver, OR Ethernet HAT.
- cyw43439 wireless (only RPi 3B+ / Zero 2 W) — heavier, full WiFi stack.

For demo/testing without a NIC, a Pi 3 can already serve as a single-node p-kernel that exercises the Body / Brain / Self layers; the Collective layer (DRPC/SWIM/raft) needs a NIC.

**QEMU raspi3b output capture quirk (still relevant):** `-serial stdio` doesn't connect on QEMU 10.1; use `-chardev stdio,id=ser0 -serial chardev:ser0 -serial null` for interactive runs, or `-chardev file,…` for trace captures.

## Phase 3: RPi 3 hardware (after QEMU full stack works)

1. **Boot**: SD card with `config.txt` (`arm_64bit=1`, `kernel=kernel8.img`) + objcopy ELF→raw binary.
2. **UART**: PL011 driver, base = 0x3F201000. BCM2837 quirks: GPIO mux on pins 14/15, mini-UART vs PL011 selection, VPU clock for PL011.
3. **Mailbox**: BCM2837 0xB880 — needed for VideoCore comms. Write `arch/aarch64/bcm_mailbox.c`.
4. **Timer/GIC**: RPi 3 uses BCM2837's local controller, not standard GICv2. Conditional code in tkdev_init.c.
5. **arch_reboot for RPi**: PSCI not available; use watchdog register sequence (`PM_WDOG @ 0x3F100024`).
6. **Network**: on-board USB-Ethernet chip depends on board revision — **RPi 3B = SMSC9514** (USB 2.0, 100Mbit-class), **RPi 3B+ = LAN7515 (Microchip)** (USB 2.0 → gigabit-PHY, throughput ~300Mbit). Target chip is the 3B+ variant (LAN7515) since that's what we're acquiring. Both sit behind the same DWC OTG USB host. Skip initially; use USB-to-Ethernet or design "node #2" with Ethernet HAT. **U-Boot has drivers for both chips already**, so netboot iteration is unaffected by which one we write last.

## Phase 3 iteration tooling: netboot route chosen (2026-05-21, session 5)

**Decision:** real-hardware iteration will use **U-Boot + TFTP**, not SD-flash-and-reboot. SD card gets written *once* with U-Boot + Pi firmware; every subsequent kernel.img iteration goes over Ethernet via TFTP.

**Hardware to acquire:**
- **RPi 3B+** (matches current port target; native netboot model)
- **USB-UART (CP2102 or FT232)** — connect to GPIO 14/15 + GND for PL011 console
- Existing home **Ubuntu server** will host TFTP/DHCP (replaces Termux proot, which can't bind UDP 69)

**Why netboot unblocks more than just SD wear:** U-Boot has BCM2837 SMSC9514 USB-Ethernet support built in. U-Boot pulls kernel.img over Ethernet, then `go 0x80000` to p-kernel — **p-kernel itself does not need a NIC driver to iterate on real silicon.** This means Phase 3 step 5 (NIC) is not a blocker for validating Phase 3 steps 1-4 on actual BCM2837 hardware. The netboot loop is also the iteration mechanism *for* writing the eventual USB-Ethernet driver.

**Pre-hardware prep (can do now on the Ubuntu server):**
1. Install `tftpd-hpa`, set up `/srv/tftp/`.
2. Cross-build U-Boot for `rpi_3_defconfig` with `aarch64-linux-gnu-` toolchain.
3. Fetch RPi firmware blobs (`bootcode.bin`, `fixup.dat`, `start.elf`) from `raspberrypi/firmware`.
4. Commit a `tools/netboot/` directory or `docs/netboot.md` with: SD-card-one-time recipe, `make tftp` Makefile target that copies `kernel8.img` to `/srv/tftp/`, U-Boot `bootcmd` doing `dhcp; tftpboot 0x80000 kernel8.img; go 0x80000`.
5. Sanity-check the existing AArch64 link layout uses `0x80000` as load addr (matches U-Boot's `go` convention on AArch64).

**Order of operations after hardware arrives:**
1. Wire UART (GPIO 14 TX → adapter RX, GPIO 15 RX → adapter TX, GND → GND).
2. Burn SD card *once* with firmware + U-Boot.
3. Confirm U-Boot prompt over UART.
4. Confirm `dhcp` gets an IP, `tftpboot` pulls kernel8.img.
5. First `go 0x80000` — expect either the existing QEMU raspi3b boot banner, or a faceplant that pinpoints real-vs-emulated divergence. Either way the iteration loop is established.

## Architectural rules that hold now

- **`arch/common/` is the shared layer.** Cross-arch `-I../<other-arch>/include` is gone and stays gone. See [[feedback-arch-common-layout]].
- **Driver header API in `arch/common/include/`, implementation per-arch.** Pattern: `pci.h` + `rtl8139.h` in common, `arch/<arch>/pci.c` + `arch/<arch>/rtl8139.c` for the per-arch implementation.
- **`arch_reboot()` is the system-reset abstraction.** Add a new per-arch impl when porting to a new board (RPi: watchdog reg; QEMU virt/A53: PSCI HVC; x86: ACPI port 0xCF9).

## Pitfalls re-confirmed this session

- **QEMU virt PCIe needs manual BAR assignment** without UEFI firmware. BARs read back 0; write your chosen address, then re-read. PCI MMIO32 window starts at 0x10000000.
- **PCI_INT_LINE is undefined without firmware** — don't trust it for IRQ routing on QEMU virt. Use the well-known SPI mapping (PCIe INTA = SPI 35).
- **Weak stubs let drivers compile before their consumers exist.** `__attribute__((weak)) void eth_input(...)` in rtl8139.c — when netstack.c gets linked in Phase 2c, its strong def takes precedence.
