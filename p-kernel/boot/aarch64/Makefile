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
CROSS   = aarch64-linux-gnu-
CC      = $(CROSS)gcc
AS      = $(CROSS)as
LD      = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

# ---- Paths ------------------------------------------------------------
ROOT    = ../..
ARCH    = $(ROOT)/arch/aarch64
KERNEL  = $(ROOT)/kernel/common
INC_TK  = $(ROOT)/include/kernel/tkernel
INC_LIB = $(ROOT)/include/lib
INC_TOP = $(ROOT)/include

# ---- Include directories ----------------------------------------------
# arch/aarch64 FIRST so its cpu_insn.h, sysdef_depend.h, etc. shadow x86's;
# arch/x86 included for AI/distributed headers that are arch-independent
# (ai_kernel.h, dtr.h, dkva.h, drpc.h, kdds.h, etc.).
INCDIRS = \
    -I$(ARCH)/include \
    -I$(INC_TK) \
    -I$(INC_TOP) \
    -I$(INC_LIB)/libc \
    -I$(ROOT)/arch/x86/include \
    -I.

# ---- Compiler flags ---------------------------------------------------
CFLAGS  = -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic \
          -fno-common -O1 -std=gnu11 \
          -D_APP_AARCH64_ \
          $(INCDIRS)

ASFLAGS = -D_in_asm_source_ \
          -I$(ARCH)/include \
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
    sio.c

ARCH_S_SRCS = cpu_support.S

ARCH_OBJS   = $(ARCH_C_SRCS:.c=.o) $(ARCH_S_SRCS:.S=.o)

# ---- Portable AI / distributed sources (built from arch/x86 location)
# These files have no x86-specific assembly or I/O — they're the "brain"
# layer of p-kernel. Phase 2 imports the AI primitives only; distributed
# features come after we add a PCI/NIC driver for AArch64.
X86_PORTABLE_AI = \
    tensor.c \
    ai_job.c \
    pipeline.c \
    ai_stats.c
# fedlearn.c needs drpc — add it back once networking is up

AI_OBJS = $(X86_PORTABLE_AI:.c=.o)

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

# ---- All objects -------------------------------------------------------
ALL_OBJS = $(BOOT_OBJS) $(ARCH_OBJS) $(AI_OBJS) $(KERNEL_OBJS) $(LIBSTR_OBJS)

TARGET = kernel.elf

# ---- Build rules -------------------------------------------------------
all: $(TARGET)

$(TARGET): $(ALL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(LIBGCC)
	@echo "[OK] $(TARGET) built"
	@$(CROSS)size $@

# boot/aarch64 C sources
main.o: main.c
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/aarch64 ASM entry
start.o: $(ARCH)/start.S
	$(CC) $(CFLAGS) $(ASFLAGS) -c -o $@ $<

# arch/aarch64 dispatcher assembly
cpu_support.o: $(ARCH)/cpu_support.S
	$(CC) $(CFLAGS) $(ASFLAGS) -c -o $@ $<

# arch/aarch64 C sources
cpu_init.o: $(ARCH)/cpu_init.c
	$(CC) $(CFLAGS) -c -o $@ $<

tkdev_init.o: $(ARCH)/tkdev_init.c
	$(CC) $(CFLAGS) -c -o $@ $<

inittask_def.o: $(ARCH)/inittask_def.c
	$(CC) $(CFLAGS) -c -o $@ $<

inittask_main.o: $(ARCH)/inittask_main.c
	$(CC) $(CFLAGS) -c -o $@ $<

usermain.o: $(ARCH)/usermain.c
	$(CC) $(CFLAGS) -c -o $@ $<

sio.o: $(ARCH)/sio.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Portable AI sources (built from arch/x86 location)
$(AI_OBJS): %.o: $(ROOT)/arch/x86/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# kernel/common C sources
%.o: $(KERNEL)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# libc string sources
$(LIBSTR_SRCS:.c=.o): %.o: $(LIBSTR_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

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

clean:
	rm -f $(ALL_OBJS) $(TARGET)

.PHONY: all run run-debug clean
