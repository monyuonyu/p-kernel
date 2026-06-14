# p-kernel Linux userspace Makefile
# Builds the same T-Kernel core that boot/aarch64 builds, but with the
# arch/linux/aarch64 backend instead of the bare-metal AArch64 one.
#
# Usage:
#   make            — build ./p-kernel
#   make run        — build and run, terminating with Ctrl-C
#   make clean

# ---- Toolchain --------------------------------------------------------
# Auto-detect host arch (mirror of boot/linux_x86_64): native build on an
# aarch64 host, cross-build via aarch64-linux-gnu-gcc elsewhere (e.g. an
# x86_64 CI runner), run via qemu-aarch64 when not native.
# Override on the command line: make CROSS=<prefix>-  or  make CC=<cc>
ifeq ($(shell uname -m),aarch64)
CROSS ?=
RUN   ?=
else
CROSS ?= aarch64-linux-gnu-
RUN   ?= qemu-aarch64
endif

# Keep the native default EXACTLY `cc`; any CROSS prefix switches to gcc.
CC      = $(if $(CROSS),$(CROSS)gcc,cc)

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
# cpu_insn.h, syslib_depend.h, tkdev_timer.h) take precedence over
# arch/aarch64/include's privileged-instruction versions. arch/linux/include
# holds the cross-arch hosted-port APIs (arch_ctx.h, arch_preempt.h).
# arch/common/include/lp64 holds LP64-uniform headers shared with the
# x86_64 sibling (offset.h, sysinfo_depend.h, profile_depend.h, etc.).
# arch/aarch64/include still provides the aarch64-specific headers that
# the linux/aarch64 port reuses verbatim (cpu_task.h, machine_depend.h,
# mmio.h, sysdef_depend.h, tkdev_conf.h, utk_config_depend.h, cpudef.h).
#
# Note: include/lib/libc deliberately stays OUT of the include path so
# Linux POSIX headers (signal.h, termios.h, etc.) resolve to the real
# system libc instead of the T-Kernel placeholder stubs.
INCDIRS = \
    -I. \
    -I$(ARCH_LINUX)/aarch64/include \
    -I$(ARCH_LINUX)/include \
    -I$(ARCH_AA64)/include \
    -I$(ROOT)/arch/common/include/lp64 \
    -I$(ROOT)/arch/common/include \
    -I$(ROOT)/relay \
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
          -I$(ROOT)/arch/common/include/lp64 \
          -I$(INC_TK) -I$(INC_TOP)

LDFLAGS = -no-pie -lrt

# ---- libtcc self-compile support (hosted-only, auto-detected) --------
# selfc lets the RUNNING kernel compile C source in-memory (libtcc,
# TCC_OUTPUT_MEMORY) and start it as a T-Kernel task. Enabled only when
# the toolchain can actually link -ltcc for THIS target ABI, so builds
# without libtcc-dev (CI, cross) still succeed — selfc.c then compiles
# as a stub. Force-disable with `make NO_LIBTCC=1`.
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
    net_relay.c \
    net_dispatch.c \
    galaxy_posix.c \
    arch_reboot.c \
    vfs_stub.c \
    preempt.c \
    fault.c \
    time.c

ARCH_S_SRCS = cpu_support.S

# Relay-side sha256 implementation, pulled into the kernel build so
# net_relay.c can compute HMAC-SHA256 for v2 wire. Single canonical
# source — kept in relay/ where it was first written.
RELAY_DIR    = $(ROOT)/relay
RELAY_C_SRCS = sha256.c
RELAY_OBJS   = $(RELAY_C_SRCS:.c=.o)

# arch/linux/ (cross-arch hosted) sources — shared with linux_x86_64
# pfs_durable.c: G24 P0 durable backend (block + ref persistence to
# $PKERNEL_PFS_DIR). Hosted-only; bare-metal targets keep the memory store.
# pfs_ark.c: wave-13 white-pearl — ARK (arch/common/arkfs.c) as p-fs's
# durable backend, selected by PKERNEL_PFS_BACKEND=ark. Hosted-only.
ARCH_SHARED_C_SRCS = selfc.c selfc_proc.c pfs_durable.c pfs_ark.c sign_entropy.c console_ring.c
ARCH_SHARED_OBJS   = $(ARCH_SHARED_C_SRCS:.c=.o)

ARCH_OBJS = $(ARCH_C_SRCS:.c=.o) $(ARCH_S_SRCS:.S=.o) $(RELAY_OBJS) \
            $(ARCH_SHARED_OBJS)

# ---- arch/common sources (portable AI + distributed layer) -----------
# Same list as boot/aarch64. These files are arch-independent C and
# expect only the T-Kernel public API + a handful of arch hooks
# (rtl8139_*, pci_*, sio_*, arch_reboot, vfs_*) that arch/linux already
# provides as either real implementations (sio) or stubs (NIC, VFS).
ARCH_COMMON = $(ROOT)/arch/common
GALAXY_HTML = $(ARCH_COMMON)/web/galaxy.html
GALAXY_GEN  = $(ROOT)/tools/galaxy/gen_page.py
GALAXY_PAGE = galaxy_page.h
MANIFESTO_TXT  = $(ARCH_COMMON)/web/manifesto.txt
MANIFESTO_PAGE = manifesto_page.h
# i18n wave: hosted embeds ALL ~32 manifesto languages (the web UI offers a
# selector). The language set + endonyms live in one shared spec.
MANIFESTO_GEN  = $(ROOT)/tools/galaxy/gen_manifestos.py
include $(ARCH_COMMON)/web/manifesto_langs.mk
MANIFESTO_SET  = $(MANIFESTO_LANGS_FULL)
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
    r3_incontext.c \
    r3_vocab.c \
    galaxy.c \
    spec.c \
    retrieval.c \
    dkva.c \
    dmn.c \
    lm_consolidate.c \
    lm_self.c \
    ark_profile.c \
    ga.c \
    fedlearn.c \
    gossip_learn.c \
    mem_store.c \
    chat.c \
    dproc.c \
    kloader_task.c \
    demo_kdds.c \
    pfs_block.c \
    arkfs.c \
    lookup.c \
    pfs_repl.c \
    pfs_dag.c \
    protect.c \
    genome.c \
    ed25519.c \
    sign.c \
    modver.c

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

# ---- libpkernel.so (Android UMP Phase A host-side smoke test) --------
#
# Builds the same kernel sources as a shared library. The Android
# CMakeLists.txt does the equivalent via NDK clang; this target lets
# you reproduce the .so build (and the test_so_load.c dlopen check)
# on any aarch64-linux host without Android Studio.
#
# Key flags vs. the regular build:
#   -fPIC                       — required for shared library
#   -fvisibility=hidden         — every symbol hidden by default;
#                                 main.c marks pkernel_main explicitly
#                                 visible; pkernel_jni.c uses JNIEXPORT
#   -Dmain=pkernel_main         — rename main so the JNI bridge can call
#                                 it without colliding with the loader's
#                                 own start symbol
#
# Use: make libpkernel.so && ./test_so_load
SO_TARGET = libpkernel.so
SO_CFLAGS = -Wall -Wextra -g -O1 -fPIC -fvisibility=hidden \
            -fno-stack-protector -fno-common -std=gnu11 \
            -D_APP_AARCH64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ \
            -Dmain=pkernel_main $(INCDIRS)
SO_KCFLAGS = $(SO_CFLAGS) -I$(INC_LIB)/libc

SO_OBJS = $(ALL_OBJS:.o=.lo)

$(SO_TARGET): $(SO_OBJS)
	$(CC) -shared -fPIC -fvisibility=hidden -o $@ $(SO_OBJS) -lrt -lpthread
	@echo "[OK] $(SO_TARGET) built ($$(stat -c %s $(SO_TARGET)) bytes)"

# .lo objects are the PIC siblings of .o. Sharing source trees but
# different flags so a single `make` doesn't have to rebuild from scratch
# when switching between executable and shared-library outputs.
main.lo: main.c
	$(CC) $(SO_CFLAGS) -c -o $@ $<

$(ARCH_C_SRCS:.c=.lo): %.lo: $(ARCH_LINUX)/aarch64/%.c
	$(CC) $(SO_CFLAGS) -c -o $@ $<

# selfc stays a stub in the .so build (SO_CFLAGS never gets
# -DHAVE_LIBTCC): the Android/.so pipeline has no libtcc yet, and
# linking a non-PIC libtcc.a into a shared object wouldn't fly anyway.
$(ARCH_SHARED_OBJS:.o=.lo): %.lo: $(ARCH_LINUX)/%.c
	$(CC) $(SO_CFLAGS) -c -o $@ $<

cpu_support.lo: $(ARCH_LINUX)/aarch64/cpu_support.S
	$(CC) $(ASFLAGS) -fPIC -c -o $@ $<

$(KERNEL_SRCS:.c=.lo): %.lo: $(KERNEL)/%.c
	$(CC) $(SO_KCFLAGS) -c -o $@ $<

$(COMMON_C_SRCS:.c=.lo): %.lo: $(ARCH_COMMON)/%.c
	$(CC) $(SO_KCFLAGS) -c -o $@ $<

$(LIBSTR_SRCS:.c=.lo): %.lo: $(LIBSTR_DIR)/%.c
	$(CC) $(SO_KCFLAGS) -c -o $@ $<

$(RELAY_OBJS:.o=.lo): %.lo: $(RELAY_DIR)/%.c
	$(CC) $(SO_CFLAGS) -c -o $@ $<

test_so_load: test_so_load.c $(SO_TARGET)
	$(CC) -o $@ $< -ldl -lpthread

# Run the host-side dlopen smoke test: confirms libpkernel.so loads and
# pkernel_main runs to the T-Kernel boot banner.
run-so-test: test_so_load
	LD_LIBRARY_PATH=. ./test_so_load

# Variant: dlopen libpkernel.so AND join a Phase B v2 relay mesh.
# Spawns ../../relay/relay on port 27460 with a fixed key, then sets
# PKERNEL_RELAY_{HOST,PORT,KEY}+AUTONET before dlopen so the kernel's
# net_dispatch.c picks the relay transport. Confirms end-to-end that
# the .so artifact (= what the Android NDK build ships) can drive the
# v2 wire successfully.
test_so_load_relay: test_so_load_relay.c $(SO_TARGET) $(ROOT)/relay/relay
	$(CC) -o $@ $< -ldl -lpthread

$(ROOT)/relay/relay:
	$(MAKE) -C $(ROOT)/relay relay

run-so-relay-test: test_so_load_relay
	LD_LIBRARY_PATH=. ./test_so_load_relay

# so_node — a pure single-node launcher (no relay fork). Run N copies as
# separate processes to build a real multi-node mesh from the SAME .so the
# APK ships. Used by samples/11_distributed/run_region_relay_so.sh to prove
# region formation over the relay from *measured* RTT (no sim zone).
so_node: so_node.c $(SO_TARGET)
	$(CC) -o $@ $< -ldl -lpthread


# boot/linux/main.c — uses system libc directly
main.o: main.c
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/linux/aarch64 C sources
$(ARCH_C_SRCS:.c=.o): %.o: $(ARCH_LINUX)/aarch64/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/linux/ cross-arch hosted C sources (selfc)
$(ARCH_SHARED_OBJS): %.o: $(ARCH_LINUX)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/linux/aarch64 ASM
cpu_support.o: $(ARCH_LINUX)/aarch64/cpu_support.S
	$(CC) $(ASFLAGS) -c -o $@ $<

# relay/sha256.c — built with the same CFLAGS as arch/linux C sources
$(RELAY_OBJS): %.o: $(RELAY_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# kernel/common — uses T-Kernel placeholder libc
$(KERNEL_OBJS): %.o: $(KERNEL)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

# arch/common — portable AI + distributed layer; same libc as kernel/common
$(COMMON_OBJS): %.o: $(ARCH_COMMON)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

# galaxy v1 (galaxy.md D3): build-time HTML -> C array (both .o and .lo
# paths, since galaxy.c builds into the .so too). galaxy.o/.lo are the
# only TUs that #include galaxy_page.h.
$(GALAXY_PAGE): $(GALAXY_HTML) $(GALAXY_GEN)
	python3 $(GALAXY_GEN) $(GALAXY_HTML) $@

galaxy.o galaxy.lo: $(GALAXY_PAGE)

# ark-profile v1 + i18n (ark-profile.md §7.1/§7.5): the manifesto TABLE —
# one generated header with N per-language byte arrays + manifesto_table[].
# Hosted embeds the FULL set. ark_profile.o/.lo and galaxy.o/.lo are the
# only TUs that #include manifesto_page.h.
$(MANIFESTO_PAGE): $(MANIFESTO_SRCS) $(MANIFESTO_GEN)
	python3 $(MANIFESTO_GEN) $@ $(MANIFESTO_SET)

ark_profile.o ark_profile.lo galaxy.o galaxy.lo: $(MANIFESTO_PAGE)

# libc string sources
$(LIBSTR_OBJS): %.o: $(LIBSTR_DIR)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

run: $(TARGET)
	$(RUN) ./$(TARGET)

clean:
	rm -f $(ALL_OBJS) $(SO_OBJS) $(TARGET) $(SO_TARGET) $(GALAXY_PAGE) $(MANIFESTO_PAGE) \
	      test_so_load test_so_load_relay so_node

.PHONY: all run run-so-test run-so-relay-test clean
