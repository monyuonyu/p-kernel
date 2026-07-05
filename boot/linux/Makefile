# p-kernel Linux userspace Makefile — aarch64 / μT-Kernel 3.0 コア版
#
# boot/linux/Makefile（micro T-Kernel 2.0 コア）の 3.0 移行版。
# カーネルコアを kernel/common から kernel/mtkernel3 に差し替える。
# アプリ層（arch/common・arch/linux ほか）のソース構成は 2.0 版と同一。
#
# 使い方:
#   make -f Makefile.mtk3          — ./p-kernel をビルド
#   make -f Makefile.mtk3 run      — ビルドして実行
#   make -f Makefile.mtk3 clean

# ---- Toolchain --------------------------------------------------------
ifeq ($(shell uname -m),aarch64)
CROSS ?=
RUN   ?=
else
CROSS ?= aarch64-linux-gnu-
RUN   ?= qemu-aarch64
endif

CC = $(CROSS)gcc

# ---- Paths ------------------------------------------------------------
ROOT       = ../..
ARCH_LINUX  = $(ROOT)/arch/linux
ARCH_AA64   = $(ROOT)/arch/linux/aarch64
ARCH_COMMON = $(ROOT)/arch/common
GALAXY_HTML = $(ARCH_COMMON)/web/galaxy.html
GALAXY_GEN  = $(ROOT)/tools/galaxy/gen_page.py
GALAXY_PAGE = galaxy_page.h
MANIFESTO_TXT  = $(ARCH_COMMON)/web/manifesto.txt
MANIFESTO_PAGE = manifesto_page.h
MANIFESTO_GEN  = $(ROOT)/tools/galaxy/gen_manifestos.py
include $(ARCH_COMMON)/web/manifesto_langs.mk
MANIFESTO_SET  = $(MANIFESTO_LANGS_FULL)
INC_TOP     = $(ROOT)/include
INC_LIB     = $(ROOT)/include/lib

# μT-Kernel 3.0 コア
MTK3        = $(ROOT)/kernel/mtkernel3
MTK3_KNL    = $(MTK3)/kernel/tkernel
MTK3_SYSDEP = $(MTK3)/kernel/sysdepend/linux_aarch64
MTK3_LIBTM  = $(MTK3)/lib/libtm

# ---- Include directories ----------------------------------------------
# arch 系ヘッダを先に、μT-Kernel 3.0 のヘッダ群をその後に置く。
#   -I$(MTK3)/kernel/knlinc : "kernel.h"（カーネル内部共通定義）
#   -I$(MTK3)/kernel/tkernel: "task.h" 等の内部ヘッダ（guard.c 等が使用）
#   -I$(MTK3)/include       : <tk/*.h> <sys/*.h> <tm/*.h>
#   -I$(MTK3)/include/tm    : <tmonitor.h>（アプリの 2.0 互換 include）
#   -I$(MTK3)/config        : <config.h>
# INC_TOP は 2.0 と同じく最後（arch 層の共有ヘッダ用）。
INCDIRS = \
    -I. \
    -I$(ARCH_AA64)/include \
    -I$(ARCH_LINUX)/include \
    -I$(ROOT)/arch/common/include/lp64 \
    -I$(ROOT)/arch/common/include \
    -I$(ROOT)/relay \
    -I$(MTK3)/kernel/knlinc \
    -I$(MTK3)/kernel/tkernel \
    -I$(MTK3)/include \
    -I$(MTK3)/include/tm \
    -I$(MTK3)/include/compat \
    -I$(MTK3)/config

# ---- Compiler flags ---------------------------------------------------
# -ffp-contract=off: 2.0 版と同じ「one mind, one math」規律。
# -D_LINUX_AARCH64_ : μT-Kernel 3.0 のターゲット選択マクロ。
EXTRA_CFLAGS ?=
# -MMD -MP: ヘッダ依存を .d に自動生成（config/ヘッダ変更時の
# stale オブジェクト混在を防ぐ。TU 間で構造体レイアウトが食い違う
# 事故は 2.0→3.0 移行デバッグで実際に踏んだ）
CFLAGS  = -Wall -Wextra -g -O1 -no-pie -fno-pie \
          -fno-stack-protector -fno-common -std=gnu11 \
          -ffp-contract=off -MMD -MP \
          -D_APP_AARCH64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ \
          -D_LINUX_AARCH64_ \
          $(EXTRA_CFLAGS) \
          $(INCDIRS)

ASFLAGS = -D_in_asm_source_ -D_APP_AARCH64_ -D_APP_LINUX_ -D_LINUX_AARCH64_ \
          -I$(MTK3_SYSDEP) \
          -I$(MTK3)/include -I$(MTK3)/config

LDFLAGS = -no-pie -static -lrt -lpthread

# ---- libtcc self-compile support (hosted-only, auto-detected) --------
ifeq ($(NO_LIBTCC),)
HAVE_LIBTCC := $(shell echo 'int main(void){tcc_new();return 0;}' | \
                 $(CC) $(LDFLAGS) -include libtcc.h -x c - -ltcc -o /dev/null 2>/dev/null && echo yes)
endif
ifeq ($(HAVE_LIBTCC),yes)
CFLAGS  += -DHAVE_LIBTCC
LDFLAGS += -ltcc
endif

# ---- boot/linux sources ---------------------------------------
BOOT_C_SRCS = main_mtk3.c
BOOT_OBJS   = main_mtk3.o

# ---- arch/linux/x86_64 sources ---------------------------------------
# 2.0 版から除外: cpu_init.c tkdev_init.c inittask_def.c inittask_main.c
# cpu_support.S（μT-Kernel 3.0 の sysinit/inittask/sysdepend が代替）
ARCH_C_SRCS = \
    usermain.c \
    sio.c \
    pci.c \
    rtl8139.c \
    net_unix.c \
    net_relay.c \
    net_relay_tcp.c \
    net_lan.c \
    net_dispatch.c \
    supernode_autopromote.c \
    galaxy_posix.c \
    arch_reboot.c \
    vfs_stub.c \
    preempt.c \
    fault.c \
    time.c

RELAY_DIR    = $(ROOT)/relay
RELAY_C_SRCS = sha256.c
RELAY_OBJS   = $(RELAY_C_SRCS:.c=.o)

ARCH_SHARED_C_SRCS = selfc.c selfc_proc.c pfs_durable.c pfs_ark.c sign_entropy.c console_ring.c node_id.c
ARCH_SHARED_OBJS   = $(ARCH_SHARED_C_SRCS:.c=.o)

ARCH_OBJS = $(ARCH_C_SRCS:.c=.o) $(RELAY_OBJS) $(ARCH_SHARED_OBJS)

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
    conscience.c \
    vital.c \
    edf.c \
    raft.c \
    spawn.c \
    moe.c \
    dtr.c \
    dtr_train.c \
    r3_incontext.c \
    r3_vocab.c \
    ui_api.c \
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
    ss6_live.c \
    supernode.c \
    pfs_repl.c \
    pfs_dag.c \
    protect.c \
    genome.c \
    ed25519.c \
    sign.c \
    compat_ota.c \
    compat_arkfs_gap.c \
    cradle_net.c \
    modver.c

COMMON_OBJS = $(COMMON_C_SRCS:.c=.o)

# ---- arch/common/llm sources（自己ビルド SmolLM2 推論エンジン） -------
LLM_DIR     = $(ARCH_COMMON)/llm
LLM_C_SRCS  = gguf.c quant.c pk_parallel.c tokenizer.c forward.c sample.c \
              llm_shell.c student.c dev_capacity.c student_shell.c cradle.c
LLM_OBJS    = $(LLM_C_SRCS:.c=.o)
LLM_CFLAGS  = -Wall -Wextra -g -O1 -no-pie -fno-pie \
              -fno-stack-protector -fno-common -std=gnu11 \
              -ffp-contract=off -MMD -MP -I$(LLM_DIR)

# ---- μT-Kernel 3.0 core sources ---------------------------------------
# コア本体（kernel/tkernel）
MTK3_KNL_SRCS = \
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
    objname.c \
    rendezvous.c \
    klock.c \
    cpuctl.c \
    int.c \
    power.c \
    device.c \
    deviceio.c \
    tkinit.c

MTK3_KNL_OBJS = $(MTK3_KNL_SRCS:.c=.o)

# 起動・初期タスク・標準ライブラリ・T-Monitor 互換ライブラリ
MTK3_MISC_OBJS = sysinit.o inittask.o tstdlib_string.o tstdlib_bitop.o \
                 libtm.o libtm_printf.o tm_com.o

# ターゲット依存部（Linux x86-64 ポート）
MTK3_SYSDEP_SRCS = cpu_cntl.c interrupt.c hw_setting.c devinit.c power_save.c
MTK3_SYSDEP_OBJS = $(MTK3_SYSDEP_SRCS:.c=.o) dispatch.o

MTK3_OBJS = $(MTK3_KNL_OBJS) $(MTK3_MISC_OBJS) $(MTK3_SYSDEP_OBJS)

# ---- libc string sources ---------------------------------------------
# 2.0 版はベアメタル互換のため lib/libc/string を自前リンクしていたが、
# hosted の mtk3 ビルドでは glibc（-static）が memset 等を提供するため
# 不要。プレースホルダヘッダ（include/lib/libc）が 2.0 の stdtype.h に
# 依存している問題も同時に回避できる。

ALL_OBJS = $(BOOT_OBJS) $(ARCH_OBJS) $(COMMON_OBJS) $(LLM_OBJS) $(MTK3_OBJS)

TARGET = p-kernel

# アプリ層カーネル隣接 TU（2.0 版の KERNEL_CFLAGS 相当）
# 2.0 版はベアメタル互換のため include/lib/libc（プレースホルダ libc
# ヘッダ）を与えていたが、これは 2.0 の stdtype.h に依存する。hosted の
# mtk3 ビルドではシステム libc ヘッダをそのまま使う（arch/linux 層と同じ）。
KERNEL_CFLAGS = $(CFLAGS)

# μT-Kernel 3.0 コア用: sysinit の main() を knl_main() に改名して
# boot/linux/main_mtk3.c の main() と共存させる
MTK3_CFLAGS = $(CFLAGS) -DADD_PREFIX_KNL_TO_GLOBAL_NAME

# ---- Build rules -----------------------------------------------------
all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(CC) -o $@ $(ALL_OBJS) $(LDFLAGS)
	@echo "[OK] $(TARGET) built ($$(stat -c %s $(TARGET)) bytes)"

main_mtk3.o: main_mtk3.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(ARCH_C_SRCS:.c=.o): %.o: $(ARCH_AA64)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(ARCH_SHARED_OBJS): %.o: $(ARCH_LINUX)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(RELAY_OBJS): %.o: $(RELAY_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- μT-Kernel 3.0 core build rules ----
$(MTK3_KNL_OBJS): %.o: $(MTK3_KNL)/%.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

sysinit.o: $(MTK3)/kernel/sysinit/sysinit.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

inittask.o: $(MTK3)/kernel/inittask/inittask.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

tstdlib_string.o: $(MTK3)/kernel/tstdlib/string.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

tstdlib_bitop.o: $(MTK3)/kernel/tstdlib/bitop.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

libtm.o: $(MTK3_LIBTM)/libtm.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

libtm_printf.o: $(MTK3_LIBTM)/libtm_printf.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

tm_com.o: $(MTK3_LIBTM)/sysdepend/linux_aarch64/tm_com.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

$(MTK3_SYSDEP_SRCS:.c=.o): %.o: $(MTK3_SYSDEP)/%.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

dispatch.o: $(MTK3_SYSDEP)/dispatch.S
	$(CC) $(ASFLAGS) -c -o $@ $<

# ---- アプリ層（2.0 版と同一構成） ----
$(COMMON_OBJS): %.o: $(ARCH_COMMON)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

ss6_live.o: KERNEL_CFLAGS += -I$(LLM_DIR)

$(LLM_OBJS): %.o: $(LLM_DIR)/%.c
	$(CC) $(LLM_CFLAGS) -c -o $@ $<

$(GALAXY_PAGE): $(GALAXY_HTML) $(GALAXY_GEN)
	python3 $(GALAXY_GEN) $(GALAXY_HTML) $@

galaxy.o: $(GALAXY_PAGE)

$(MANIFESTO_PAGE): $(MANIFESTO_SRCS) $(MANIFESTO_GEN)
	python3 $(MANIFESTO_GEN) $@ $(MANIFESTO_SET)

ark_profile.o galaxy.o: $(MANIFESTO_PAGE)



# ---- libpkernel.so (Android UMP ホスト側スモークテスト) ---------------
#
# 同じカーネルソースを共有ライブラリとしてビルドする。Android の
# CMakeLists.txt は NDK clang で同等のことを行う。このターゲットは
# Android Studio なしで .so ビルド（と test_so_load.c の dlopen 検証）を
# aarch64-linux ホスト上で再現するためのもの。
SO_TARGET = libpkernel.so
SO_CFLAGS = -Wall -Wextra -g -O1 -fPIC -fvisibility=hidden \
            -fno-stack-protector -fno-common -std=gnu11 \
            -ffp-contract=off -MMD -MP \
            -D_APP_AARCH64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ \
            -D_LINUX_AARCH64_ \
            -Dmain=pkernel_main $(INCDIRS)
SO_MTK3_CFLAGS = $(SO_CFLAGS) -DADD_PREFIX_KNL_TO_GLOBAL_NAME

SO_OBJS = $(ALL_OBJS:.o=.lo)

$(SO_TARGET): $(SO_OBJS)
	$(CC) -shared -fPIC -fvisibility=hidden -o $@ $(SO_OBJS) -lrt -lpthread
	@echo "[OK] $(SO_TARGET) built ($$(stat -c %s $(SO_TARGET)) bytes)"

main_mtk3.lo: main_mtk3.c
	$(CC) $(SO_CFLAGS) -c -o $@ $<

$(ARCH_C_SRCS:.c=.lo): %.lo: $(ARCH_AA64)/%.c
	$(CC) $(SO_CFLAGS) -c -o $@ $<

$(ARCH_SHARED_OBJS:.o=.lo): %.lo: $(ARCH_LINUX)/%.c
	$(CC) $(SO_CFLAGS) -c -o $@ $<

$(RELAY_OBJS:.o=.lo): %.lo: $(RELAY_DIR)/%.c
	$(CC) $(SO_CFLAGS) -c -o $@ $<

# μT-Kernel 3.0 コアの PIC 版
$(MTK3_KNL_OBJS:.o=.lo): %.lo: $(MTK3_KNL)/%.c
	$(CC) $(SO_MTK3_CFLAGS) -c -o $@ $<

sysinit.lo: $(MTK3)/kernel/sysinit/sysinit.c
	$(CC) $(SO_MTK3_CFLAGS) -c -o $@ $<

inittask.lo: $(MTK3)/kernel/inittask/inittask.c
	$(CC) $(SO_MTK3_CFLAGS) -c -o $@ $<

tstdlib_string.lo: $(MTK3)/kernel/tstdlib/string.c
	$(CC) $(SO_MTK3_CFLAGS) -c -o $@ $<

tstdlib_bitop.lo: $(MTK3)/kernel/tstdlib/bitop.c
	$(CC) $(SO_MTK3_CFLAGS) -c -o $@ $<

libtm.lo: $(MTK3_LIBTM)/libtm.c
	$(CC) $(SO_MTK3_CFLAGS) -c -o $@ $<

libtm_printf.lo: $(MTK3_LIBTM)/libtm_printf.c
	$(CC) $(SO_MTK3_CFLAGS) -c -o $@ $<

tm_com.lo: $(MTK3_LIBTM)/sysdepend/linux_aarch64/tm_com.c
	$(CC) $(SO_MTK3_CFLAGS) -c -o $@ $<

$(MTK3_SYSDEP_SRCS:.c=.lo): %.lo: $(MTK3_SYSDEP)/%.c
	$(CC) $(SO_MTK3_CFLAGS) -c -o $@ $<

dispatch.lo: $(MTK3_SYSDEP)/dispatch.S
	$(CC) $(ASFLAGS) -fPIC -c -o $@ $<

$(COMMON_OBJS:.o=.lo): %.lo: $(ARCH_COMMON)/%.c
	$(CC) $(SO_CFLAGS) -c -o $@ $<

ss6_live.lo: SO_CFLAGS += -I$(LLM_DIR)

$(LLM_OBJS:.o=.lo): %.lo: $(LLM_DIR)/%.c
	$(CC) $(LLM_CFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<

test_so_load: test_so_load.c $(SO_TARGET)
	$(CC) -o $@ $< -ldl -lpthread

run-so-test: test_so_load
	LD_LIBRARY_PATH=. ./test_so_load

test_so_load_relay: test_so_load_relay.c $(SO_TARGET) $(ROOT)/relay/relay
	$(CC) -o $@ $< -ldl -lpthread

$(ROOT)/relay/relay:
	$(MAKE) -C $(ROOT)/relay relay

run-so-relay-test: test_so_load_relay
	LD_LIBRARY_PATH=. ./test_so_load_relay

so_node: so_node.c $(SO_TARGET)
	$(CC) -o $@ $< -ldl -lpthread

# 自動生成されたヘッダ依存（-MMD）の取り込み
-include $(wildcard *.d)

run: $(TARGET)
	$(RUN) ./$(TARGET)

clean:
	rm -f *.o *.d $(TARGET) $(GALAXY_PAGE) $(MANIFESTO_PAGE)

.PHONY: all run clean
