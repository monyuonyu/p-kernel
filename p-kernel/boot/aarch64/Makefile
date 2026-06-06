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
CFLAGS  = -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic \
          -fno-common -O1 -std=gnu11 -mstrict-align \
          -D_APP_AARCH64_ \
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
    arch_reboot.c \
    vfs_stub.c

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
    vital.c \
    edf.c \
    raft.c \
    spawn.c \
    moe.c \
    dtr.c \
    dtr_train.c \
    spec.c \
    retrieval.c \
    dkva.c \
    dmn.c \
    ga.c \
    fedlearn.c \
    mem_store.c \
    chat.c \
    dproc.c \
    kloader_task.c \
    demo_kdds.c \
    pfs_block.c \
    lookup.c \
    pfs_repl.c \
    pfs_dag.c \
    genome.c

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

# ---- All objects -------------------------------------------------------
ALL_OBJS = $(BOOT_OBJS) $(ARCH_OBJS) $(COMMON_OBJS) $(KERNEL_OBJS) $(LIBSTR_OBJS) $(RELAY_OBJS)

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
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(LIBGCC)
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
	$(LD) -nostdlib -static -T linker.rpi3.ld -o $@ $(ALL_OBJS) $(LIBGCC)
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

run: $(TARGET)
	qemu-system-aarch64 $(QEMU_FLAGS)

run-debug: $(TARGET)
	qemu-system-aarch64 $(QEMU_FLAGS) -S -gdb tcp::1234 &
	@echo "GDB: aarch64-linux-gnu-gdb -ex 'target remote :1234' kernel.elf"

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
	rm -f $(ALL_OBJS) $(TARGET) kernel.rpi3.elf kernel8.img

.PHONY: all run run-debug clean
