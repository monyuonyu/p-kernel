# p-kernel Windows ネイティブ移植 Makefile — x86_64 / μT-Kernel 3.0 コア版
#
# boot/linux_x86_64/Makefile の Windows（mingw-w64 PE）移植版。
# カーネルコア・arch/common ブレイン・LLM 層は Linux 版と同一ソースを
# ビルドし、ターゲット依存部だけを差し替える:
#   - ディスパッチャ    : windows_x86_64/dispatch.c（Windows Fiber）
#   - タイマ tick        : 協調スケジューラの安全点ポンプ（プリエンプト無し）
#   - シリアル/コンソール: arch/windows/x86_64/sio.c（Win32 コンソール）
#   - ネットワーク       : Winsock2（-lws2_32）+ compat 経由の BSD ソケット shim
#
# ONE MATH 規律: -O1 -ffp-contract=off（fleet の分散数値を Linux/Android と
# ビット一致させる。wave-47 salty-bug の教訓）。
#
# 使い方:
#   make            — p-kernel.exe をクロスビルド
#   make clean

# ---- Toolchain --------------------------------------------------------
CC = x86_64-w64-mingw32-gcc

# ---- Paths ------------------------------------------------------------
# この Makefile は boot/windows/x86_64/ にあるため、リポジトリ root は 3 つ上。
ROOT        = ../../..
ARCH_LINUX  = $(ROOT)/arch/linux
ARCH_X86_64 = $(ROOT)/arch/linux/x86_64
ARCH_WIN    = $(ROOT)/arch/windows/x86_64
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
MTK3_SYSDEP = $(MTK3)/kernel/sysdepend/windows_x86_64
MTK3_LIBTM  = $(MTK3)/lib/libtm

# ---- Include directories ----------------------------------------------
# arch 系ヘッダを先に、μT-Kernel 3.0 群をその後に。socket shim（compat）は
# -idirafter で最後に置き、mingw が持たないヘッダ（<sys/socket.h> 等）だけ
# を補う。
INCDIRS = \
    -I. \
    -I$(ARCH_WIN) \
    -I$(ARCH_X86_64)/include \
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
# -D__USE_MINGW_ANSI_STDIO=1: %lld / %zu 等の C99 書式を正しく扱う。
# -include win_predef.h: dprintf 等 mingw に無い libc 宣言を全 TU へ供給
#   （winsock は含まない — arch/common の tk 型宇宙と衝突させない）。
EXTRA_CFLAGS ?=
CFLAGS  = -Wall -Wextra -g -O1 \
          -fno-stack-protector -fno-common -std=gnu11 \
          -ffp-contract=off -MMD -MP \
          -D__USE_MINGW_ANSI_STDIO=1 \
          -D_APP_X86_64_ -D_APP_WINDOWS_ -D_TK_HOSTED_LIBC_ \
          -D_WINDOWS_X86_64_ \
          -include $(ARCH_WIN)/win_predef.h \
          $(EXTRA_CFLAGS) \
          $(INCDIRS) \
          -idirafter $(ARCH_WIN)/compat

# -static: fold the mingw runtime (libgcc, libwinpthread) INTO the exe so the
# commander's box needs no extra DLLs — only the always-present Windows system
# DLLs (KERNEL32/WS2_32/msvcrt) stay dynamic (they come from import libs).
LDFLAGS = -static
# -lpthread: mingw-w64 libwinpthread — provides pthread_* (LLM worker pool),
# clock_gettime and nanosleep (net/sign/llm timing) in one library.
LDLIBS  = -lws2_32 -lpthread

# selfc（自己コンパイル）は Windows では無効化（stub）。libtcc 探索もしない。

# ---- boot sources -----------------------------------------------------
BOOT_C_SRCS = main_win.c
BOOT_OBJS   = main_win.o

# ---- arch 層（共有 = arch/linux/x86_64、POSIX ソケットは compat で shim） ----
# Linux 版から除外: sio/time/preempt/fault/arch_reboot（Windows 版で置換）、
# selfc/selfc_proc（selfc_stub で置換）。
ARCH_SHARED_X86_SRCS = \
    usermain.c \
    pci.c \
    rtl8139.c \
    net_unix.c \
    net_relay.c \
    net_relay_tcp.c \
    net_lan.c \
    net_dispatch.c \
    supernode_autopromote.c \
    galaxy_posix.c \
    vfs_stub.c
ARCH_SHARED_X86_OBJS = $(ARCH_SHARED_X86_SRCS:.c=.o)

# ---- arch/windows/x86_64 固有ソース ----------------------------------
ARCH_WIN_SRCS = \
    sio.c \
    time.c \
    fault.c \
    arch_reboot.c \
    win_prim.c \
    win_mmap.c \
    net_win.c \
    selfc_stub.c
ARCH_WIN_OBJS = $(ARCH_WIN_SRCS:.c=.o)

RELAY_DIR    = $(ROOT)/relay
RELAY_C_SRCS = sha256.c
RELAY_OBJS   = $(RELAY_C_SRCS:.c=.o)

# arch/linux 共有（POSIX 汎用: console_ring/node_id/sign_entropy/pfs_*）。
# selfc.c / selfc_proc.c は除外（selfc_stub で代替）。
ARCH_SHARED_C_SRCS = pfs_durable.c pfs_ark.c sign_entropy.c console_ring.c node_id.c
ARCH_SHARED_OBJS   = $(ARCH_SHARED_C_SRCS:.c=.o)

ARCH_OBJS = $(ARCH_SHARED_X86_OBJS) $(ARCH_WIN_OBJS) $(RELAY_OBJS) $(ARCH_SHARED_OBJS)

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
    gen_succession.c \
    cradle_net.c \
    modver.c \
    scaling_cert.c

COMMON_OBJS = $(COMMON_C_SRCS:.c=.o)

# ---- arch/common/llm sources（自己ビルド SmolLM2 推論エンジン） -------
LLM_DIR     = $(ARCH_COMMON)/llm
LLM_C_SRCS  = gguf.c quant.c pk_parallel.c tokenizer.c forward.c sample.c \
              llm_shell.c student.c dev_capacity.c student_shell.c cradle.c dlb.c \
              frontier.c
LLM_OBJS    = $(LLM_C_SRCS:.c=.o)
DMOE_OBJS = dmoe_bank.o
LLM_CFLAGS  = -Wall -Wextra -g -O1 \
              -fno-stack-protector -fno-common -std=gnu11 \
              -ffp-contract=off -MMD -MP \
              -D__USE_MINGW_ANSI_STDIO=1 \
              -include $(ARCH_WIN)/win_predef.h \
              -I$(LLM_DIR) -DCRADLE_HAS_CONSCIENCE $(EXTRA_CFLAGS) \
              -idirafter $(ARCH_WIN)/compat

# ---- μT-Kernel 3.0 core sources ---------------------------------------
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

MTK3_MISC_OBJS = sysinit.o inittask.o tstdlib_string.o tstdlib_bitop.o \
                 libtm.o libtm_printf.o tm_com.o

# ターゲット依存部（Windows x86-64 Fiber ポート）
# dispatch は Linux 版の .S ではなく .c（Windows Fiber）。
MTK3_SYSDEP_SRCS = cpu_cntl.c interrupt.c hw_setting.c devinit.c power_save.c dispatch.c
MTK3_SYSDEP_OBJS = $(MTK3_SYSDEP_SRCS:.c=.o)

MTK3_OBJS = $(MTK3_KNL_OBJS) $(MTK3_MISC_OBJS) $(MTK3_SYSDEP_OBJS)

ALL_OBJS = $(BOOT_OBJS) $(ARCH_OBJS) $(COMMON_OBJS) $(LLM_OBJS) $(DMOE_OBJS) $(MTK3_OBJS)

TARGET = p-kernel.exe

KERNEL_CFLAGS = $(CFLAGS)

# tm_com.c（Linux 版）は write(2)/STDOUT で T-Monitor 出力を行う。mingw は
# _write/STDOUT_FILENO を <io.h>/<unistd.h> で提供するため同一ソースを流用。
MTK3_CFLAGS = $(CFLAGS) -DADD_PREFIX_KNL_TO_GLOBAL_NAME

# ---- Build rules -----------------------------------------------------
all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(CC) -o $@ $(ALL_OBJS) $(LDFLAGS) $(LDLIBS)
	@echo "[OK] $(TARGET) built ($$(stat -c %s $(TARGET)) bytes)"

main_win.o: main_win.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(ARCH_SHARED_X86_OBJS): %.o: $(ARCH_X86_64)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(ARCH_WIN_OBJS): %.o: $(ARCH_WIN)/%.c
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

# T-Monitor 通信の下位（Windows: Win32 コンソール = arch/windows/sio.c）
tm_com.o: $(MTK3_LIBTM)/sysdepend/windows_x86_64/tm_com.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

$(MTK3_SYSDEP_OBJS): %.o: $(MTK3_SYSDEP)/%.c
	$(CC) $(MTK3_CFLAGS) -c -o $@ $<

# ---- アプリ層（arch/common） ----
$(COMMON_OBJS): %.o: $(ARCH_COMMON)/%.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

ss6_live.o: KERNEL_CFLAGS += -I$(LLM_DIR)

dmoe_bank.o: $(LLM_DIR)/dmoe_bank.c
	$(CC) $(CFLAGS) -I$(LLM_DIR) -c -o $@ $<

$(LLM_OBJS): %.o: $(LLM_DIR)/%.c
	$(CC) $(LLM_CFLAGS) -c -o $@ $<

$(GALAXY_PAGE): $(GALAXY_HTML) $(GALAXY_GEN)
	python3 $(GALAXY_GEN) $(GALAXY_HTML) $@

galaxy.o: $(GALAXY_PAGE)

$(MANIFESTO_PAGE): $(MANIFESTO_SRCS) $(MANIFESTO_GEN)
	python3 $(MANIFESTO_GEN) $@ $(MANIFESTO_SET)

ark_profile.o galaxy.o: $(MANIFESTO_PAGE)

-include $(wildcard *.d)

clean:
	rm -f *.o *.d $(TARGET) $(GALAXY_PAGE) $(MANIFESTO_PAGE)

.PHONY: all clean
