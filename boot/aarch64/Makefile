# p-kernel aarch64 Makefile
# Builds QEMU virt AArch64 kernel ELF from:
#   boot/aarch64 + arch/aarch64 + kernel/common
#
# Usage:
#   make              — build kernel.elf
#   make run          — launch QEMU (UART on stdio)
#   make run-rpi3     — same image but with RPi3 UART address
#   make clean

# ---- Toolchain --------------------------------------------------------
# Override on the command line: make CROSS=<prefix>-
CROSS  ?= aarch64-linux-gnu-
CC      = $(CROSS)gcc
AS      = $(CROSS)as
LD      = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

# ---- Paths ------------------------------------------------------------
ROOT    = ../..
ARCH    = $(ROOT)/arch/aarch64
COMMON  = $(ROOT)/arch/common
GALAXY_GEN     = $(ROOT)/tools/galaxy/gen_page.py
MANIFESTO_TXT  = $(COMMON)/web/manifesto.txt
MANIFESTO_PAGE = manifesto_page.h
# i18n wave: bare metal has NO web UI — embed ja+en ONLY (lean kernel; the
# manifesto here serves the future netstack-tcp-server slice). ark-profile.md
# §7.5. The shared spec keys off $(ARCH_COMMON).
ARCH_COMMON    = $(COMMON)
MANIFESTO_GEN  = $(ROOT)/tools/galaxy/gen_manifestos.py
include $(COMMON)/web/manifesto_langs.mk
MANIFESTO_SET  = $(MANIFESTO_LANGS_LEAN)
KERNEL  = $(ROOT)/kernel/common
INC_TK  = $(ROOT)/include/kernel/tkernel
INC_LIB = $(ROOT)/include/lib
INC_TOP = $(ROOT)/include

# ---- Include directories ----------------------------------------------
# arch/aarch64/include FIRST so its arch-specific shadows (cpu_insn.h,
# sysdef_depend.h, etc.) win over generic ones; arch/common/include/lp64
# holds LP64-uniform headers (offset.h, sysinfo_depend.h, profile_depend.h,
# str_align_depend.h, cpu_conf.h, stdarg.h) shared with the x86_64 sibling;
# arch/common/include holds the cross-arch driver / distributed-layer API.
INCDIRS = \
    -I$(ARCH)/include \
    -I$(COMMON)/include/lp64 \
    -I$(COMMON)/include \
    -I$(ROOT)/relay \
    -I$(INC_TK) \
    -I$(INC_TOP) \
    -I$(INC_LIB)/libc \
    -I.

# ---- Compiler flags ---------------------------------------------------
# EXTRA_CFLAGS lets a caller inject build-time defines without editing this
# file, e.g.  make EXTRA_CFLAGS=-DARK_BAREMETAL_SMOKE  (used by
# samples/33_ark_aarch64/run.sh to compile the bare-metal ARK smoke driver
# into boot/main.c + arch/aarch64/ark_bdev.c).
EXTRA_CFLAGS ?=
# -ffp-contract=off: never fuse a*b+c into an FMA so the mind's float
#   math (dtr.c) rounds identically to every other target — "one mind,
#   one math" (guards the wave-49 salty-bug class; see `dtr fpdet`).
CFLAGS  = -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic \
          -fno-common -O1 -std=gnu11 -mstrict-align \
          -ffp-contract=off \
          -D_APP_AARCH64_ \
          $(EXTRA_CFLAGS) \
          $(INCDIRS)
# -mstrict-align: with MMU disabled at EL1, Cortex-A53 treats RAM as
# Device-nGnRnE, which disallows unaligned data accesses. Without this
# flag GCC merges packed-struct field writes (e.g. arp->spa, arp->tpa)
# into single STUR x{n}, [base, #42] instructions and the kernel takes
# an alignment fault on the first ARP send. Switch to a proper MMU
# identity map (RAM as Normal cacheable) when we need the performance.

ASFLAGS = -D_in_asm_source_ \
          -I$(ARCH)/include \
          -I$(COMMON)/include/lp64 \
          -I$(INC_TK) \
          -I$(INC_TOP)

LIBGCC  = $(shell $(CC) -print-libgcc-file-name)
LDFLAGS = -nostdlib -static -T linker.ld

# ---- boot/aarch64 sources --------------------------------------------
BOOT_C_SRCS = main.c
BOOT_S_SRCS = $(ARCH)/start.S
BOOT_OBJS   = main.o start.o

# ---- arch/aarch64 sources --------------------------------------------
ARCH_C_SRCS = \
    cpu_init.c \
    tkdev_init.c \
    inittask_def.c \
    inittask_main.c \
    usermain.c \
    sio.c \
    pci.c \
    rtl8139.c \
    virtio_blk.c \
    ark_bdev.c \
    arch_reboot.c \
    vfs_stub.c \
    sign_entropy.c \
    mc2_smp.c \
    smp.c \
    smp_prod.c \
    smp_async.c

ARCH_S_SRCS = cpu_support.S

ARCH_OBJS   = $(ARCH_C_SRCS:.c=.o) $(ARCH_S_SRCS:.S=.o)

# ---- arch/common sources (portable across all arches) ----------------
# Phase 2a imports the AI primitives. Phase 2c adds the full distributed
# layer — netstack, DRPC, SWIM, K-DDS, replica, mesh, Raft, MoE, GA,
# federated learning, persistent memory, chat, kloader, etc.
COMMON_C_SRCS = \
    tensor.c \
    ai_job.c \
    pipeline.c \
    ai_stats.c \
    netstack.c \
    drpc.c \
    swim.c \
    region.c \
    world.c \
    reflex.c \
    kdds.c \
    replica.c \
    pmesh.c \
    sfs.c \
    heal.c \
    guard.c \
    degrade.c \
    interocept.c \
    vital.c \
    edf.c \
    raft.c \
    spawn.c \
    moe.c \
    dtr.c \
    dtr_train.c \
    r3_incontext.c \
    r3_vocab.c \
    spec.c \
    retrieval.c \
    dkva.c \
    dmn.c \
    lm_consolidate.c \
    lm_self.c \
    self_access.c \
    ark_profile.c \
    ga.c \
    gossip_learn.c \
    mem_store.c \
    chat.c \
    dproc.c \
    kloader_task.c \
    demo_kdds.c \
    pfs_block.c \
    arkfs.c \
    lookup.c \
    placement.c \
    pfs_repl.c \
    pfs_dag.c \
    protect.c \
    genome.c \
    ed25519.c \
    sign.c

COMMON_OBJS = $(COMMON_C_SRCS:.c=.o)

# ---- kernel/common sources -------------------------------------------
KERNEL_SRCS = \
    task.c \
    task_manage.c \
    task_sync.c \
    timer.c \
    time_calls.c \
    wait.c \
    semaphore.c \
    eventflag.c \
    mailbox.c \
    messagebuf.c \
    mutex.c \
    mempfix.c \
    mempool.c \
    memory.c \
    misc_calls.c \
    tkstart.c \
    bitop.c \
    fastlock.c \
    fastmlock.c \
    klock.c \
    objname.c \
    rendezvous.c \
    subsystem.c \
    tm_putchar.c \
    tm_putstring.c \
    tm_printf.c \
    tm_monitor.c \
    tm_exit.c \
    tm_getchar.c \
    tm_getline.c \
    tm_command.c

KERNEL_OBJS = $(KERNEL_SRCS:.c=.o)

# ---- libc string stubs -----------------------------------------------
LIBSTR_DIR  = $(ROOT)/lib/libc/string
LIBSTR_SRCS = memset.c memcpy.c memmove.c memcmp.c \
              strlen.c strcpy.c strncpy.c strcmp.c strncmp.c
LIBSTR_OBJS = $(LIBSTR_SRCS:.c=.o)

# ---- relay/sha256.c — zero-dep SHA-256 for arch/common/pfs_block.c ----
# Compiled with a libc-consistent include order (lib/libc stddef.h only;
# NOT include/stddef.h) so its <string.h>/<stddef.h> pair never mixes the
# two ptrdiff_t definitions. memcpy/memset/memcmp link from LIBSTR_OBJS.
RELAY_DIR    = $(ROOT)/relay
RELAY_OBJS   = sha256.o
RELAY_CFLAGS = -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic \
               -fno-common -O1 -std=gnu11 -mstrict-align \
               -I$(RELAY_DIR) -I$(INC_LIB)/libc -I$(INC_TK)

# student_stub.o — WEAK no-op fallbacks for the resident-baby ABI
# (student_dmn_consolidate / student_dmn_heldout_loss / student_chat_generate).
# dmn.c references the first two; the real (strong) defs live in
# arch/common/llm/student_shell.c, which is HOSTED-ONLY (needs malloc/libm). On
# bare metal student_shell.c is not built, so these weak no-ops resolve the link.
STUDENT_STUB_OBJ = student_stub.o

# ---- All objects -------------------------------------------------------
ALL_OBJS = $(BOOT_OBJS) $(ARCH_OBJS) $(COMMON_OBJS) $(KERNEL_OBJS) $(LIBSTR_OBJS) $(RELAY_OBJS) $(STUDENT_STUB_OBJ)

# ②.2a [smp-2tasks-prod] / ②.2b-i [smp-async-preempt]: smp_prod.c + smp_async.c
# are ALWAYS compiled+cleaned (so the build is consistent across flag changes),
# but their objects are EXCLUDED from the LINK unless the matching cert is built
# (-DSMP_2TASKS_PROD / -DSMP_ASYNC_PREEMPT).  Excluding them keeps the DEFAULT
# kernel ELF BYTE-IDENTICAL — each .o is empty otherwise, but its mere presence
# perturbs the ELF section-header metadata.  (Both reference the production
# scheduler + tk_cre_tsk; zero effect when their symbols are unreferenced, but
# we exclude them to make even the full ELF identical.)
SMP_CERT_EXCLUDE =
ifeq (,$(findstring SMP_2TASKS_PROD,$(EXTRA_CFLAGS)))
SMP_CERT_EXCLUDE += smp_prod.o
endif
ifeq (,$(findstring SMP_ASYNC_PREEMPT,$(EXTRA_CFLAGS)))
SMP_CERT_EXCLUDE += smp_async.o
endif
LINK_OBJS = $(filter-out $(SMP_CERT_EXCLUDE),$(ALL_OBJS))

TARGET = kernel.elf

# ---- BOARD selection --------------------------------------------------
# Default target: QEMU virt (cortex-a53 + GICv2 + PL011 at 0x09000000).
# Override on the make command line for other boards:
#     make rpi3          → kernel8.img for Raspberry Pi 3 Model B
#
# RPi 3 differs from QEMU virt in three ways the build chain needs to
# know about:
#   1. Load address is 0x80000 (set by the BCM2837 firmware) — needs a
#      separate linker script.
#   2. Boot image is a raw binary called kernel8.img, not an ELF.
#   3. PL011 base, BCM2837 ARM-local interrupt controller, watchdog
#      reset — handled by source-level #ifdef BOARD_RPI3 in arch code.
BOARD_CFLAGS_RPI3 = -DBOARD_RPI3

# ---- Build rules -------------------------------------------------------
all: $(TARGET)

$(TARGET): $(ALL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(LINK_OBJS) $(LIBGCC)
	@echo "[OK] $(TARGET) built"
	@$(CROSS)size $@

# ---- Raspberry Pi 3 build ---------------------------------------------
.PHONY: rpi3 rpi3-clean run-rpi3
rpi3:
	@echo "[BOARD] building for Raspberry Pi 3 Model B (BCM2837)"
	$(MAKE) clean
	$(MAKE) _rpi3_image BOARD=RPI3

# Internal target: invoked by `rpi3` with BOARD=RPI3 propagated so the
# CFLAGS/linker pick up the RPi 3 settings.
_rpi3_image: CFLAGS += $(BOARD_CFLAGS_RPI3)
_rpi3_image: ASFLAGS += $(BOARD_CFLAGS_RPI3)
_rpi3_image: LDFLAGS := -nostdlib -static -T linker.rpi3.ld
_rpi3_image: TARGET := kernel.rpi3.elf
_rpi3_image: kernel8.img

kernel.rpi3.elf: $(ALL_OBJS) linker.rpi3.ld
	$(LD) -nostdlib -static -T linker.rpi3.ld -o $@ $(LINK_OBJS) $(LIBGCC)
	@echo "[OK] $@ built"
	@$(CROSS)size $@

# Raw binary the BCM2837 firmware actually consumes. objcopy strips the
# ELF metadata; the result has section contents at their linker-assigned
# offsets, with 0x80000 itself at the head of the file.
kernel8.img: kernel.rpi3.elf
	$(OBJCOPY) -O binary $< $@
	@echo "[OK] $@ — $(shell stat -c %s $@) bytes ready for SD card"

# boot/aarch64 C sources
main.o: main.c
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/aarch64 ASM entry
start.o: $(ARCH)/start.S
	$(CC) $(CFLAGS) $(ASFLAGS) -c -o $@ $<

# arch/aarch64 dispatcher assembly
cpu_support.o: $(ARCH)/cpu_support.S
	$(CC) $(CFLAGS) $(ASFLAGS) -c -o $@ $<

# arch/aarch64 C sources (pattern rule)
$(ARCH_C_SRCS:.c=.o): %.o: $(ARCH)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/common C sources (portable distributed/AI layer)
$(COMMON_OBJS): %.o: $(COMMON)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/common/llm/student_stub.c — WEAK student-ABI fallbacks (see ALL_OBJS).
$(STUDENT_STUB_OBJ): %.o: $(COMMON)/llm/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ark-profile v1 + i18n (ark-profile.md §7.1/§7.5): the manifesto TABLE.
# Bare metal embeds ja+en ONLY (lean; no web UI). It compiles ark_profile.c
# (pfs_dag exists here) but NOT galaxy.c, so this is the only consumer of
# manifesto_page.h.
$(MANIFESTO_PAGE): $(MANIFESTO_TXT) $(COMMON)/web/manifesto.en.txt $(MANIFESTO_GEN)
	python3 $(MANIFESTO_GEN) $@ $(MANIFESTO_SET)

ark_profile.o: $(MANIFESTO_PAGE)

# kernel/common C sources
%.o: $(KERNEL)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# libc string sources
$(LIBSTR_SRCS:.c=.o): %.o: $(LIBSTR_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# relay/sha256.c — dedicated CFLAGS (see RELAY_CFLAGS comment above)
$(RELAY_OBJS): %.o: $(RELAY_DIR)/%.c
	$(CC) $(RELAY_CFLAGS) -c -o $@ $<

# ---- Run targets -------------------------------------------------------
QEMU_FLAGS = -M virt -cpu cortex-a53 -m 256M \
             -kernel $(TARGET) \
             -serial stdio \
             -display none \
             -no-reboot

# Optional virtio-blk disk attach. Set DISK=<path> to expose it to the
# kernel as a virtio-blk-device on the MMIO bus (the ARK durable backend):
#     make run DISK=/tmp/ark.img
QEMU_DISK_FLAGS = $(if $(DISK),-drive file=$(DISK),format=raw,if=none,id=ark0 \
                              -device virtio-blk-device,drive=ark0,)

run: $(TARGET)
	qemu-system-aarch64 $(QEMU_FLAGS) $(QEMU_DISK_FLAGS)

# Build + boot the ARK bare-metal smoke kernel against a virtio-blk image.
# DISK defaults to a scratch image created on the fly.
DISK ?=
run-smoke:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS=-DARK_BAREMETAL_SMOKE $(TARGET)
	@img=$${DISK:-/tmp/ark_aa64_smoke.img}; \
	 [ -f "$$img" ] || dd if=/dev/zero of="$$img" bs=1M count=8 status=none; \
	 echo "[run-smoke] booting with virtio-blk image $$img"; \
	 qemu-system-aarch64 $(QEMU_FLAGS) \
	     -drive file="$$img",format=raw,if=none,id=ark0 \
	     -device virtio-blk-device,drive=ark0

run-debug: $(TARGET)
	qemu-system-aarch64 $(QEMU_FLAGS) -S -gdb tcp::1234 &
	@echo "GDB: aarch64-linux-gnu-gdb -ex 'target remote :1234' kernel.elf"

# ---- MC-2.0 bare-metal constrained-SMP ($(N) secondary cores) ---------
# QEMU virt with -smp 4 actually instantiates the parked secondaries the
# MC-2 bringup wakes (the default `run` target is single-core, so the
# secondaries don't even exist in the VM). See docs/architecture/
# mc2-baremetal-smp-plan.md §4.3.
QEMU_SMP_FLAGS = -M virt -cpu cortex-a53 -smp 4 -m 256M \
                 -kernel $(TARGET) \
                 -serial stdio \
                 -display none \
                 -no-reboot

# ②.N8: the ②.0 full-SMP cert now scales to 8 PHYSICAL cores (the real phone
# target). -smp 8 instantiates the parked secondaries cores 1..7 that the SMP
# bringup wakes via PSCI CPU_ON. 8 is the GICv2 ceiling (the GICD_SGIR target
# list is 8 bits, 1 per CPU); past 8 would need GICv3. The mc2/equiv/smp1/smp2
# targets keep -smp 4 (their N is 2 or 4 — they run fine under -smp 4).
QEMU_SMP8_FLAGS = -M virt -cpu cortex-a53 -smp 8 -m 256M \
                 -kernel $(TARGET) \
                 -serial stdio \
                 -display none \
                 -no-reboot

# Interactive: build the kernel WITH the MC-2 boot self-test and boot it
# under -smp 4 (streams serial to stdio so a human can watch the verdict).
run-smp:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS=-DMC2_SMP_SELFTEST $(TARGET)
	qemu-system-aarch64 $(QEMU_SMP_FLAGS)

# Same, but with a deliberately faulting worker tile — proves the
# primary's bounded join does NOT wedge forever (the verdict is
# MC2-BOOT: FAIL join-timeout, and the kernel STILL boots the T-Kernel).
run-smp-fault:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DMC2_SMP_SELFTEST -DMC2_FAULTING_TILE" $(TARGET)
	qemu-system-aarch64 $(QEMU_SMP_FLAGS)

# Automated cert: build + boot under -smp 4 with a timeout and grep the
# UART for the verdict. This is the [mc2-boot-survives] harness (§4.3).
mc2-test:
	../../tests/aarch64/run_mc2_boot.sh

# ---- MC-2.1 [mc2-smp-equiv]: N-core byte-identity cert -----------------
# Interactive: build the kernel WITH the N-core equivalence self-test and
# boot it under -smp 4 (streams serial to stdio). Wakes cores 1,2,3, runs a
# synthetic gate-exceeding matmul for nw in {1,2,4}, and prints
# MC2-EQUIV: PASS/FAIL + the three FNV hashes. See docs/architecture/
# mc2-1-ncore-equiv-plan.md §4.
run-equiv:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DMC2_EQUIV_SELFTEST" $(TARGET)
	qemu-system-aarch64 $(QEMU_SMP_FLAGS)

# Same, but with TOOTH A (the partition/arithmetic falsifier) — the
# reassociated reduction MUST produce MC2-EQUIV: FAIL (proves teeth).
run-equiv-toothA:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DMC2_EQUIV_SELFTEST -DMC2_EQUIV_RACY_PARTITION" $(TARGET)
	qemu-system-aarch64 $(QEMU_SMP_FLAGS)

# Same, but with TOOTH B (the barrier/SMPEN falsifier) — on QEMU this may
# still PASS (TCG masks the race); recorded honestly, deferred to RPi3.
run-equiv-toothB:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DMC2_EQUIV_SELFTEST -DMC2_EQUIV_SMPEN_OFF -DMC2_EQUIV_NO_BARRIER" $(TARGET)
	qemu-system-aarch64 $(QEMU_SMP_FLAGS)

# Automated cert: byte-identity across nw in {1,2,4} + Tooth A FAILS +
# Tooth B honest QEMU record. This is the [mc2-smp-equiv] harness (§4.3).
mc2-equiv-test:
	../../tests/aarch64/run_mc2_equiv.sh

# ---- MC-2.1b: the STANDALONE pk_slice_bm partition unit-check ----------
# Interactive: build the kernel WITH the standalone partition unit-check and
# boot it under QEMU (NO -smp — it is a pure integer check on the primary,
# needs no secondaries). Prints MC2-SLICE: PASS/FAIL. The check guards the
# pk_slice_bm hand-copy drift surface (mc2_smp.c, the "one mind, one math"
# golden at arch/common/llm/pk_parallel.c:57-63) DIRECTLY — the equiv cert
# above only exercises it INDIRECTLY via the matmul.
QEMU_SLICE_FLAGS = -M virt -cpu cortex-a53 -m 256M \
                   -kernel $(TARGET) \
                   -serial stdio \
                   -display none \
                   -no-reboot
run-slice:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DMC2_SLICE_SELFTEST" $(TARGET)
	qemu-system-aarch64 $(QEMU_SLICE_FLAGS)

# Same, but with the FALSIFIER (-DMC2_SLICE_BREAK): pk_slice_bm drops the
# ragged remainder, so the unit-check MUST print MC2-SLICE: FAIL (teeth).
run-slice-break:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DMC2_SLICE_SELFTEST -DMC2_SLICE_BREAK" $(TARGET)
	qemu-system-aarch64 $(QEMU_SLICE_FLAGS)

# Automated cert: partition invariants PASS + falsifier FAILS. This is the
# MC-2.1b [mc2-slice] harness.
mc2-slice-test:
	../../tests/aarch64/run_mc2_slice.sh

# ---- ②.0/②.N8 full-SMP + [smp-autodetect]: N CPUs run the dispatcher under
# one Big Kernel Lock, where N is DETECTED at boot via GICD_TYPER.
# docs/architecture/full-smp-plan.md §7 ②.0; docs/architecture/
# device-autodetect-plan.md (slice 1). The SAME -DSMP_SELFTEST binary now
# AUTODETECTS the core count (GICD_TYPER bits[7:5]) and wakes EXACTLY that many,
# so booting it under -smp 2/4/8 wakes 1/3/7 secondaries — no recompile. SMP_MAX
# _CPUS=8 stays the array CEILING (the GICv2 8-CPU max). Build WITH the SMP
# self-test and boot under -smp 8 (default; uses ALL 8 CPUs: boot + cores 1..7).
# Prints (under -smp N):
#   [SMP] detected N cpus via GICD_TYPER
#   SMP-RUN:    PASS  (all N CPUs ran DISTINCT tasks; per-CPU exec counters)
#   SMP-MUTEX:  PASS  (shared counter under the BKL == exact N*K)
#   SMP-BOOT:   PASS  (all N CPUs reached the dispatcher; T-Kernel still boots)
# The DEFAULT build (no -DSMP_SELFTEST) is BYTE-IDENTICAL to before.
run-smp0:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DSMP_SELFTEST" $(TARGET)
	qemu-system-aarch64 $(QEMU_SMP8_FLAGS)

# FALSIFIER A (load-bearing): the shared-counter increment BYPASSES the BKL.
# Under -smp 8 with truly-concurrent tasks the RMW races -> lost updates ->
# SMP-MUTEX: FAIL with a total < N*K. Proves the lock is load-bearing.
run-smp0-nolock:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_MUTEX_NOLOCK" $(TARGET)
	qemu-system-aarch64 $(QEMU_SMP8_FLAGS)

# FALSIFIER B (load-bearing): IGNORE GICD_TYPER, HARDCODE the count to 8. Boot
# under -smp 2 (QEMU_SMP_FLAGS): the bringup tries PSCI CPU_ON for cores 2..7
# that DON'T EXIST -> the cert FAILs (SMP-RUN/MUTEX/BOOT: FAIL, total != 2*K).
# Proves the GICD_TYPER autodetect is load-bearing (without it the same -smp 2
# boot can't adapt). Run under -smp 2 deliberately (the small device).
run-smp0-force:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_FORCE_NCPU=8" $(TARGET)
	qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 2 -m 256M \
	    -kernel $(TARGET) -serial stdio -display none -no-reboot

# Automated cert: build ONCE, boot the SAME binary under -smp 2/4/8 and assert
# each detects+wakes the right count (counter == N*K), then the NOLOCK and
# FORCE_NCPU=8 falsifiers. This is the [smp-autodetect] + [smp-2-tasks-run] +
# [smp-mutual-exclusion] + [smp-boot-survives] harness.
smp0-test:
	../../tests/aarch64/run_smp0.sh

# ②.1a cross-CPU preemption via GIC SGI IPI. Boot under -smp 4 with the
# preempt self-test: CPU B (secondary) spins on a LOW-prio task; CPU A
# readies a HIGH-prio task for B + smp_send_reschedule(B) (a GIC SGI); B
# takes the SGI and switches to the high-prio task within a watchdog. Prints:
#   SMP-PREEMPT: PASS  (B preempted to the high-prio task after the SGI)
# The DEFAULT build (no -DSMP_SELFTEST) is BYTE-IDENTICAL to before.
# docs/architecture/smp-1-ipi-preempt-plan.md §1/§4.
run-smp1:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_PREEMPT_TEST" $(TARGET)
	qemu-system-aarch64 $(QEMU_SMP_FLAGS)

# FALSIFIER (load-bearing): smp_send_reschedule() is a no-op -> NO SGI is
# delivered -> B never preempts -> the watchdog reports SMP-PREEMPT: FAIL.
# Proves the IPI is load-bearing (preemption happens ONLY because of the SGI).
run-smp1-noipi:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_PREEMPT_TEST -DSMP_NO_IPI" $(TARGET)
	qemu-system-aarch64 $(QEMU_SMP_FLAGS)

# Automated cert: build + boot under -smp 4, grep SMP-PREEMPT: PASS, then
# build the NO_IPI falsifier and assert SMP-PREEMPT: FAIL. This is the
# [smp-cross-preempt] harness.
smp1-test:
	../../tests/aarch64/run_smp1.sh

# ---- ②.2a [smp-2tasks-prod]: 2 REAL T-Kernel TCBs on 2 CPUs (BKL) -------
# docs/architecture/smp-2-production-scheduler-plan.md §6 ②.2a. The PRODUCTION
# scheduler runs TWO REAL tk_cre_tsk/tk_sta_tsk tasks (in knl_tcb_table + the
# shared knl_ready_queue) on TWO DISTINCT CPUs under one Big Kernel Lock — NOT
# the ②.0 struct smp_task stand-ins. Boots under -smp 8 (②.N8: the shared
# smp_bringup_secondary releases cores 1..7; this cert uses only CPUs 0,1 —
# cores 2..7 idle in the prod dispatcher). Prints:
#   SMP-2TASKS-PROD: PASS  (cpu0.ctxtsk=A, cpu1.ctxtsk=B, distinct real TCBs)
# The DEFAULT build (no -DSMP_SELFTEST) is BYTE-IDENTICAL to before.
run-smp2:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_2TASKS_PROD" $(TARGET)
	qemu-system-aarch64 $(QEMU_SMP8_FLAGS)

# Automated cert: build + boot under -smp 8 + grep SMP-2TASKS-PROD: PASS +
# the per-CPU distinct-real-TCB evidence + T-Kernel still boots.
smp2-test:
	../../tests/aarch64/run_smp2.sh

# Boot the RPi 3 image under QEMU's raspi3b emulation. Matches the
# firmware contract: load kernel8.img at 0x80000, run at EL2.
QEMU_RPI3_FLAGS = -M raspi3b -m 1G -kernel kernel8.img -serial stdio \
                  -display none -no-reboot
run-rpi3: kernel8.img
	qemu-system-aarch64 $(QEMU_RPI3_FLAGS)

# ---- TFTP deployment for real-hardware netboot iteration --------------
# Drops the freshly built kernel8.img into a TFTP root so U-Boot on the
# RPi can pull it over the wire (no SD-card flashing required).
#
# Usage:
#   make tftp                          → copy to $(TFTPROOT) (default /srv/tftp)
#   make tftp TFTPROOT=/var/lib/tftp   → override the target directory
#
# Prerequisites on the dev host:
#   sudo apt install tftpd-hpa
#   sudo mkdir -p /srv/tftp && sudo chown $$USER /srv/tftp
#
# See docs/netboot.md for the SD card layout, U-Boot bootcmd, and the
# physical wiring of the USB-UART console.
TFTPROOT ?= /srv/tftp

.PHONY: tftp
tftp: rpi3
	@if [ ! -d "$(TFTPROOT)" ]; then \
	    echo "[tftp] ERROR: TFTPROOT '$(TFTPROOT)' does not exist."; \
	    echo "[tftp]        sudo mkdir -p $(TFTPROOT) && sudo chown \$$USER $(TFTPROOT)"; \
	    exit 1; \
	fi
	cp kernel8.img $(TFTPROOT)/kernel8.img
	@echo "[tftp] kernel8.img -> $(TFTPROOT)/kernel8.img ($$(stat -c %s kernel8.img) bytes)"
	@echo "[tftp] Power-cycle the Pi (or type 'reset' at the U-Boot prompt) to load."

clean:
	rm -f $(ALL_OBJS) $(TARGET) $(MANIFESTO_PAGE) kernel.rpi3.elf kernel8.img

.PHONY: all run run-debug clean run-smp run-smp-fault mc2-test \
        run-equiv run-equiv-toothA run-equiv-toothB mc2-equiv-test \
        run-slice run-slice-break mc2-slice-test \
        run-smp0 run-smp0-nolock run-smp0-force smp0-test \
        run-smp1 run-smp1-noipi smp1-test
