# p-kernel Linux userspace Makefile
# Builds the same T-Kernel core that boot/aarch64 builds, but with the
# arch/linux/aarch64 backend instead of the bare-metal AArch64 one.
#
# Usage:
#   make            — build ./p-kernel
#   make run        — build and run, terminating with Ctrl-C
#   make clean

# ---- Toolchain --------------------------------------------------------
CC      = cc

# ---- Paths ------------------------------------------------------------
ROOT       = ../..
ARCH_LINUX = $(ROOT)/arch/linux
ARCH_AA64  = $(ROOT)/arch/aarch64
KERNEL     = $(ROOT)/kernel/common
INC_TK     = $(ROOT)/include/kernel/tkernel
INC_TOP    = $(ROOT)/include
INC_LIB    = $(ROOT)/include/lib

# ---- Include directories ----------------------------------------------
# arch/linux/aarch64/include comes FIRST so its shadows (cpu_status.h,
# cpu_insn.h) take precedence over arch/aarch64/include's privileged-
# instruction versions. arch/linux/include holds the new APIs
# (arch_ctx.h, arch_preempt.h). The aarch64 tree provides everything
# else (cpu_task.h, offset.h, type defs, sysdef/syslib).
#
# Note: include/lib/libc deliberately stays OUT of the include path so
# Linux POSIX headers (signal.h, termios.h, etc.) resolve to the real
# system libc instead of the T-Kernel placeholder stubs.
INCDIRS = \
    -I$(ARCH_LINUX)/aarch64/include \
    -I$(ARCH_LINUX)/include \
    -I$(ARCH_AA64)/include \
    -I$(ROOT)/arch/common/include \
    -I$(INC_TK) \
    -I$(INC_TOP)

# ---- Compiler flags ---------------------------------------------------
# -no-pie keeps the dispatcher's adrp/add against globals direct
#   (avoids GOT/PLT indirection mid-context-switch).
# -D_APP_AARCH64_ tells T-Kernel internals this is a 64-bit ARM target.
# -D_APP_LINUX_ is for arch/linux files to know they're being built
#   for the userspace port (orthogonal to host arch).
CFLAGS  = -Wall -Wextra -g -O1 -no-pie -fno-pie \
          -fno-stack-protector -fno-common -std=gnu11 \
          -D_APP_AARCH64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ \
          $(INCDIRS)

ASFLAGS = -D_in_asm_source_ -D_APP_AARCH64_ -D_APP_LINUX_ \
          -I$(ARCH_LINUX)/aarch64/include \
          -I$(ARCH_AA64)/include \
          -I$(INC_TK) -I$(INC_TOP)

LDFLAGS = -no-pie -lrt

# ---- boot/linux sources ----------------------------------------------
BOOT_C_SRCS = main.c
BOOT_OBJS   = main.o

# ---- arch/linux/aarch64 sources --------------------------------------
ARCH_C_SRCS = \
    cpu_init.c \
    tkdev_init.c \
    inittask_def.c \
    inittask_main.c \
    usermain.c \
    sio.c \
    pci.c \
    rtl8139.c \
    net_unix.c \
    arch_reboot.c \
    vfs_stub.c \
    preempt.c

ARCH_S_SRCS = cpu_support.S

ARCH_OBJS = $(ARCH_C_SRCS:.c=.o) $(ARCH_S_SRCS:.S=.o)

# ---- arch/common sources (portable AI + distributed layer) -----------
# Same list as boot/aarch64. These files are arch-independent C and
# expect only the T-Kernel public API + a handful of arch hooks
# (rtl8139_*, pci_*, sio_*, arch_reboot, vfs_*) that arch/linux already
# provides as either real implementations (sio) or stubs (NIC, VFS).
ARCH_COMMON = $(ROOT)/arch/common
COMMON_C_SRCS = \
    tensor.c \
    ai_job.c \
    pipeline.c \
    ai_stats.c \
    netstack.c \
    drpc.c \
    swim.c \
    kdds.c \
    replica.c \
    pmesh.c \
    sfs.c \
    heal.c \
    degrade.c \
    vital.c \
    edf.c \
    raft.c \
    spawn.c \
    moe.c \
    dtr.c \
    dkva.c \
    dmn.c \
    ga.c \
    fedlearn.c \
    mem_store.c \
    chat.c \
    dproc.c \
    kloader_task.c

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

# ---- libc string sources ---------------------------------------------
LIBSTR_DIR  = $(ROOT)/lib/libc/string
LIBSTR_SRCS = memset.c memcpy.c memmove.c memcmp.c \
              strlen.c strcpy.c strncpy.c strcmp.c strncmp.c
LIBSTR_OBJS = $(LIBSTR_SRCS:.c=.o)

ALL_OBJS = $(BOOT_OBJS) $(ARCH_OBJS) $(COMMON_OBJS) $(KERNEL_OBJS) $(LIBSTR_OBJS)

TARGET = p-kernel

# ---- T-Kernel kernel/common needs its own private libc placeholders
#      (they declare e.g. size_t but not full stdio). Use a separate
#      flag set for those files so they pick up the placeholder libc.
KERNEL_CFLAGS = $(CFLAGS) -I$(INC_LIB)/libc

# ---- Build rules -----------------------------------------------------
all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(CC) -o $@ $(ALL_OBJS) $(LDFLAGS)
	@echo "[OK] $(TARGET) built ($$(stat -c %s $(TARGET)) bytes)"

# boot/linux/main.c — uses system libc directly
main.o: main.c
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/linux/aarch64 C sources
$(ARCH_C_SRCS:.c=.o): %.o: $(ARCH_LINUX)/aarch64/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/linux/aarch64 ASM
cpu_support.o: $(ARCH_LINUX)/aarch64/cpu_support.S
	$(CC) $(ASFLAGS) -c -o $@ $<

# kernel/common — uses T-Kernel placeholder libc
$(KERNEL_OBJS): %.o: $(KERNEL)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

# arch/common — portable AI + distributed layer; same libc as kernel/common
$(COMMON_OBJS): %.o: $(ARCH_COMMON)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

# libc string sources
$(LIBSTR_OBJS): %.o: $(LIBSTR_DIR)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(ALL_OBJS) $(TARGET)

.PHONY: all run clean
