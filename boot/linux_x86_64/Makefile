# p-kernel Linux userspace Makefile — x86_64 sibling.
# Mirrors boot/linux/ but builds the arch/linux/x86_64 backend.
# Cross-compiles when the build host isn't already x86_64.
#
# Usage:
#   make            — build ./p-kernel
#   make run        — build and run (qemu-x86_64 if not native)
#   make clean

# ---- Toolchain --------------------------------------------------------
# Auto-detect host arch; cross-build via x86_64-linux-gnu-gcc when not
# already on x86_64, and run via qemu-x86_64.
ifeq ($(shell uname -m),x86_64)
CROSS ?=
RUN   ?=
else
CROSS ?= x86_64-linux-gnu-
RUN   ?= qemu-x86_64
endif

CC = $(CROSS)gcc

# ---- Paths ------------------------------------------------------------
ROOT       = ../..
ARCH_LINUX  = $(ROOT)/arch/linux
ARCH_X86_64 = $(ROOT)/arch/linux/x86_64
KERNEL      = $(ROOT)/kernel/common
ARCH_COMMON = $(ROOT)/arch/common
GALAXY_HTML = $(ARCH_COMMON)/web/galaxy.html
GALAXY_GEN  = $(ROOT)/tools/galaxy/gen_page.py
GALAXY_PAGE = galaxy_page.h
MANIFESTO_TXT  = $(ARCH_COMMON)/web/manifesto.txt
MANIFESTO_PAGE = manifesto_page.h
# i18n wave: hosted embeds ALL ~32 manifesto languages.
MANIFESTO_GEN  = $(ROOT)/tools/galaxy/gen_manifestos.py
include $(ARCH_COMMON)/web/manifesto_langs.mk
MANIFESTO_SET  = $(MANIFESTO_LANGS_FULL)
INC_TK      = $(ROOT)/include/kernel/tkernel
INC_TOP     = $(ROOT)/include
INC_LIB     = $(ROOT)/include/lib

# ---- Include directories ----------------------------------------------
# arch/linux/x86_64/include comes FIRST so its shadows (cpu_status.h,
# cpu_insn.h, syslib_depend.h, tkdev_timer.h, cpu_task.h, machine_depend.h,
# sysdef_depend.h, utk_config_depend.h) take precedence. arch/linux/include
# holds the cross-arch hosted-port APIs (arch_ctx.h, arch_preempt.h,
# arch_time.h). arch/common/include/lp64 holds LP64-uniform headers
# shared with the aarch64 sibling (offset.h, sysinfo_depend.h,
# profile_depend.h, str_align_depend.h, cpu_conf.h, stdarg.h).
#
# This Makefile no longer borrows -I arch/aarch64/include; everything the
# x86_64-linux build needs is either x86_64-specific (in arch/linux/x86_64/
# include) or LP64-shared (in arch/common/include/lp64).
#
# include/lib/libc deliberately stays OUT so Linux POSIX headers
# resolve to system libc.
INCDIRS = \
    -I. \
    -I$(ARCH_X86_64)/include \
    -I$(ARCH_LINUX)/include \
    -I$(ROOT)/arch/common/include/lp64 \
    -I$(ROOT)/arch/common/include \
    -I$(ROOT)/relay \
    -I$(INC_TK) \
    -I$(INC_TOP)

# ---- Compiler flags ---------------------------------------------------
# -ffp-contract=off: never fuse a*b+c into an FMA, so the mind's float
#   math (dtr.c) rounds identically to every other target — "one mind,
#   one math". Without it clang's FMA contraction diverged from gcc and
#   the on-device mind learned 5% instead of 95% (wave-49 salty bug).
#   The `dtr fpdet` golden-logit cert guards this. Keep on every config.
CFLAGS  = -Wall -Wextra -g -O1 -no-pie -fno-pie \
          -fno-stack-protector -fno-common -std=gnu11 \
          -ffp-contract=off \
          -D_APP_X86_64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ \
          $(INCDIRS)

ASFLAGS = -D_in_asm_source_ -D_APP_X86_64_ -D_APP_LINUX_ \
          -I$(ARCH_X86_64)/include \
          -I$(ROOT)/arch/common/include/lp64 \
          -I$(INC_TK) -I$(INC_TOP)

LDFLAGS = -no-pie -static -lrt

# ---- libtcc self-compile support (hosted-only, auto-detected) --------
# Same probe as boot/linux. On an aarch64 host the only libtcc.a is the
# aarch64 one, so this -static x86_64 link probe fails and selfc stays
# a stub here — that is the detection working as intended, not a bug.
# Force-disable with `make NO_LIBTCC=1`.
# (-include libtcc.h instead of a "#include" line: '#' inside $(shell)
#  is a make-comment minefield. This still proves both the header and
#  the library exist and link for the target ABI.)
ifeq ($(NO_LIBTCC),)
HAVE_LIBTCC := $(shell echo 'int main(void){tcc_new();return 0;}' | \
                 $(CC) $(LDFLAGS) -include libtcc.h -x c - -ltcc -o /dev/null 2>/dev/null && echo yes)
endif
ifeq ($(HAVE_LIBTCC),yes)
CFLAGS  += -DHAVE_LIBTCC
LDFLAGS += -ltcc
endif

# ---- boot/linux_x86_64 sources ---------------------------------------
BOOT_C_SRCS = main.c
BOOT_OBJS   = main.o

# ---- arch/linux/x86_64 sources ---------------------------------------
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
    net_relay.c \
    net_lan.c \
    net_dispatch.c \
    galaxy_posix.c \
    arch_reboot.c \
    vfs_stub.c \
    preempt.c \
    fault.c \
    time.c

ARCH_S_SRCS = cpu_support.S

# Relay-side sha256 implementation, pulled in so net_relay.c can compute
# HMAC-SHA256 for Phase B v2. Single canonical source in relay/.
RELAY_DIR    = $(ROOT)/relay
RELAY_C_SRCS = sha256.c
RELAY_OBJS   = $(RELAY_C_SRCS:.c=.o)

# arch/linux/ (cross-arch hosted) sources — shared with boot/linux
# pfs_durable.c: G24 P0 durable backend (block + ref persistence to
# $PKERNEL_PFS_DIR). Hosted-only; bare-metal targets keep the memory store.
# pfs_ark.c: wave-13 white-pearl — ARK (arch/common/arkfs.c) as p-fs's
# durable backend, selected by PKERNEL_PFS_BACKEND=ark. Hosted-only.
ARCH_SHARED_C_SRCS = selfc.c selfc_proc.c pfs_durable.c pfs_ark.c sign_entropy.c console_ring.c node_id.c
ARCH_SHARED_OBJS   = $(ARCH_SHARED_C_SRCS:.c=.o)

ARCH_OBJS = $(ARCH_C_SRCS:.c=.o) $(ARCH_S_SRCS:.S=.o) $(RELAY_OBJS) \
            $(ARCH_SHARED_OBJS)

# ---- arch/common sources (portable AI + distributed layer) -----------
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
    galaxy.c \
    gx_sha1.c \
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
    tcb_churn.c \
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
    sign.c \
    modver.c

COMMON_OBJS = $(COMMON_C_SRCS:.c=.o)

# ---- arch/common/llm sources (self-built SmolLM2 inference engine) ----
# Mirror of boot/linux/Makefile (kept verbatim so the two host minds are
# identical). HOST/Android tier: these TUs use the REAL system libc
# (malloc/calloc/mmap/<stdint.h>/<stdio.h>), so they compile with $(CFLAGS)
# (system headers), NOT $(KERNEL_CFLAGS) (T-Kernel placeholder libc). They are
# a SEPARATE group from COMMON_C_SRCS (different libc tier; no place on bare
# metal). check_parity.sh compares the NAMED lists only — this new variable is
# not one of them, so it creates no Android-CMake drift; the host-vs-host check
# likewise only diffs the named lists, but we keep this list identical to
# boot/linux anyway.
#
# Step ③ (wave-student-in-kernel): student.c (NS-1 Cradle baby) + its bridge
# student_shell.c build IN the hosted kernel — the RESIDENT, PERSISTED student.
# Same host-libc tier + LLM_CFLAGS as the engine; NOT on bare metal (stays out
# of COMMON_C_SRCS). Mirror of boot/linux/Makefile (kept verbatim).
LLM_DIR     = $(ARCH_COMMON)/llm
LLM_C_SRCS  = gguf.c quant.c tokenizer.c forward.c sample.c llm_shell.c \
              student.c student_shell.c
LLM_OBJS    = $(LLM_C_SRCS:.c=.o)
# Own flag set (mirror of boot/linux): the engine includes ONLY system libc +
# its sibling headers, and must NOT see the kernel include/ dirs (include/
# stdint.h typedefs int64_t as `long long`, conflicting with glibc's `long`
# once <stdlib.h> pulls bits/stdint-intn.h — the LP64 typedef trap). Drop all
# kernel -I, keep -O1 -ffp-contract=off (one math). Matches run_forward.sh.
LLM_CFLAGS  = -Wall -Wextra -g -O1 -no-pie -fno-pie \
              -fno-stack-protector -fno-common -std=gnu11 \
              -ffp-contract=off -I$(LLM_DIR)

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

ALL_OBJS = $(BOOT_OBJS) $(ARCH_OBJS) $(COMMON_OBJS) $(LLM_OBJS) $(KERNEL_OBJS) $(LIBSTR_OBJS)

TARGET = p-kernel

KERNEL_CFLAGS = $(CFLAGS) -I$(INC_LIB)/libc

# ---- Build rules -----------------------------------------------------
all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(CC) -o $@ $(ALL_OBJS) $(LDFLAGS)
	@echo "[OK] $(TARGET) built ($$(stat -c %s $(TARGET)) bytes)"

main.o: main.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(ARCH_C_SRCS:.c=.o): %.o: $(ARCH_X86_64)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/linux/ cross-arch hosted C sources (selfc)
$(ARCH_SHARED_OBJS): %.o: $(ARCH_LINUX)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

cpu_support.o: $(ARCH_X86_64)/cpu_support.S
	$(CC) $(ASFLAGS) -c -o $@ $<

# relay/sha256.c — same CFLAGS as arch/linux C sources
$(RELAY_OBJS): %.o: $(RELAY_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(KERNEL_OBJS): %.o: $(KERNEL)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

$(COMMON_OBJS): %.o: $(ARCH_COMMON)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

# arch/common/llm — self-built SmolLM2 engine; HOST libc tier with its OWN
# flag set ($(LLM_CFLAGS)): no kernel -I dirs (system <stdint.h> wins, see the
# LP64 typedef trap above), same -O1 -ffp-contract=off discipline.
$(LLM_OBJS): %.o: $(LLM_DIR)/%.c
	$(CC) $(LLM_CFLAGS) -c -o $@ $<

# galaxy v1 (galaxy.md D3): build-time HTML -> C array so the embedded
# page can never drift from source. galaxy.o (the only TU that #includes
# galaxy_page.h) depends on it.
$(GALAXY_PAGE): $(GALAXY_HTML) $(GALAXY_GEN)
	python3 $(GALAXY_GEN) $(GALAXY_HTML) $@

galaxy.o: $(GALAXY_PAGE)

# ark-profile v1 + i18n (ark-profile.md §7.1/§7.5): the manifesto TABLE
# (N per-language byte arrays + manifesto_table[]). Hosted embeds the FULL
# set. ark_profile.o and galaxy.o are the only TUs that #include it.
$(MANIFESTO_PAGE): $(MANIFESTO_SRCS) $(MANIFESTO_GEN)
	python3 $(MANIFESTO_GEN) $@ $(MANIFESTO_SET)

ark_profile.o galaxy.o: $(MANIFESTO_PAGE)

$(LIBSTR_OBJS): %.o: $(LIBSTR_DIR)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

run: $(TARGET)
	$(RUN) ./$(TARGET)

clean:
	rm -f $(ALL_OBJS) $(TARGET) $(GALAXY_PAGE) $(MANIFESTO_PAGE)

.PHONY: all run clean
