# p-kernel x86 Makefile
# boot/x86 + kernel/common + arch/x86 を統合してQEMU起動イメージを生成

# ツールチェイン
CROSS   = i686-linux-gnu-
CC      = $(CROSS)gcc
AS      = $(CROSS)as
LD      = $(CROSS)ld

# ルートディレクトリ (このMakefileからの相対パス)
ROOT    = ../..
ARCH    = $(ROOT)/arch/x86
KERNEL  = $(ROOT)/kernel/common
INC_TK  = $(ROOT)/include/kernel/tkernel
INC_LIB = $(ROOT)/include/lib
INC_TOP = $(ROOT)/include

# インクルードパス
# arch/x86/include を先に置いてFM3依存ヘッダを上書き
INCDIRS = \
    -I$(ARCH)/include \
    -I$(INC_TK) \
    -I$(INC_TOP) \
    -I$(INC_LIB)/libc \
    -I.

# コンパイルフラグ
CFLAGS  = -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic \
          -fcf-protection=none -O1 -m32 -std=gnu11 \
          -D_APP_X86_ \
          $(INCDIRS)

LIBGCC  = $(shell $(CC) -m32 -print-libgcc-file-name)
LDFLAGS = -nostdlib -static -T linker.ld -m elf_i386

# -------------------------------------------------------------------
# boot/x86 ソース
# -------------------------------------------------------------------
BOOT_SRCS_C = main.c idt.c memory.c pic.c timer.c
BOOT_SRCS_S = start.S isr.S
# 名前衝突する memory.o / timer.o は boot_ プレフィックス付きオブジェクトで区別
BOOT_OBJS   = main.o idt.o boot_memory.o pic.o boot_timer.o start.o isr.o

# -------------------------------------------------------------------
# arch/x86 ソース
# -------------------------------------------------------------------
ARCH_C_SRCS = \
    cpu_init.c \
    tkdev_init.c \
    inittask_def.c \
    inittask_main.c \
    usermain.c \
    sio.c \
    vga.c \
    keyboard.c \
    shell.c \
    pci.c \
    rtl8139.c \
    netstack.c \
    drpc.c \
    swim.c \
    kdds.c \
    heal.c \
    edf.c \
    replica.c \
    degrade.c \
    dmn.c \
    ga.c \
    vital.c \
    tensor.c \
    ai_job.c \
    pipeline.c \
    fedlearn.c \
    ai_stats.c \
    dtr.c \
    dkva.c \
    dproc.c \
    sfs.c \
    raft.c \
    spawn.c \
    moe.c \
    pmesh.c \
    persist.c \
    blk_ssy.c \
    fs_ssy.c \
    ide.c \
    fat32.c \
    vfs.c \
    net_ssy.c \
    gdt_user.c \
    syscall.c \
    userspace.c \
    elf_loader.c \
    paging.c

ARCH_S_SRCS = cpu_support.S

ARCH_OBJS = $(ARCH_C_SRCS:.c=.o) $(ARCH_S_SRCS:.S=.o)

# -------------------------------------------------------------------
# kernel/common ソース (T-Kernel コア)
# -------------------------------------------------------------------
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

# -------------------------------------------------------------------
# lib/libc/string ソース (memset, strncpy など)
# -------------------------------------------------------------------
LIBSTR_DIR  = $(ROOT)/lib/libc/string
LIBSTR_SRCS = memset.c strncpy.c memcpy.c memmove.c memcmp.c strlen.c strcmp.c strncmp.c strcpy.c
LIBSTR_OBJS = $(LIBSTR_SRCS:.c=.o)

ALL_OBJS = $(BOOT_OBJS) $(ARCH_OBJS) $(KERNEL_OBJS) $(LIBSTR_OBJS)

TARGET = bootloader.bin

# -------------------------------------------------------------------
# kloader.bin — Stage-1 Kernel Loader (OS 依存なし, 0x50000 にロード)
# -------------------------------------------------------------------
KLOADER_SRCS_C = kl_main.c kl_fat32.c kl_net.c
KLOADER_SRCS_S = kloader_start.S
KLOADER_OBJS   = kl_main.o kl_fat32.o kl_net.o kloader_start.o

# kloader は OS ヘッダ不要 — インクルードパスなし
KLOADER_CFLAGS = -Wall -ffreestanding -fno-stack-protector -fno-pic \
                 -fcf-protection=none -O1 -m32 -std=gnu11 -I.

KLOADER_LDFLAGS = -nostdlib -static -T kloader.ld -m elf_i386

kloader.bin: $(KLOADER_OBJS)
	$(LD) $(KLOADER_LDFLAGS) -o $@ $^ $(LIBGCC)

kl_main.o:       kl_main.c;       $(CC) $(KLOADER_CFLAGS) -c $< -o $@
kl_fat32.o:      kl_fat32.c;      $(CC) $(KLOADER_CFLAGS) -c $< -o $@
kl_net.o:        kl_net.c;        $(CC) $(KLOADER_CFLAGS) -c $< -o $@
kloader_start.o: kloader_start.S; $(CC) $(KLOADER_CFLAGS) -c $< -o $@

# -------------------------------------------------------------------
# ビルドルール
# -------------------------------------------------------------------
all: $(TARGET) kloader.bin

$(TARGET): $(ALL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBGCC)

# boot/x86 C ソース (カレントディレクトリ) - 名前衝突回避のため個別ルール
main.o:        main.c;        $(CC) $(CFLAGS) -c $< -o $@
idt.o:         idt.c;         $(CC) $(CFLAGS) -c $< -o $@
boot_memory.o: memory.c;      $(CC) $(CFLAGS) -c $< -o $@
pic.o:         pic.c;         $(CC) $(CFLAGS) -c $< -o $@
boot_timer.o:  timer.c;       $(CC) $(CFLAGS) -c $< -o $@

# boot/x86 アセンブリ (カレントディレクトリ)
start.o: start.S; $(CC) $(CFLAGS) -c $< -o $@
isr.o:   isr.S;   $(CC) $(CFLAGS) -c $< -o $@

# arch/x86 C ソース
$(ARCH_C_SRCS:.c=.o): %.o: $(ARCH)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# arch/x86 アセンブリ
$(ARCH_S_SRCS:.S=.o): %.o: $(ARCH)/%.S
	$(CC) $(CFLAGS) -c $< -o $@

# kernel/common C ソース
$(KERNEL_OBJS): %.o: $(KERNEL)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# lib/libc/string C ソース
$(LIBSTR_OBJS): %.o: $(LIBSTR_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# -------------------------------------------------------------------
# 実行ターゲット
# -------------------------------------------------------------------
NETDEV  = -netdev user,id=n0 -device rtl8139,netdev=n0

# Distributed two-node setup (QEMU UDP multicast = virtual Ethernet hub)
# Run in two separate terminals: make run-node0, then make run-node1
# Both use the same multicast group — no listen/connect asymmetry needed.
MCAST     = 230.0.0.1:1234
NODEDEV0  = -netdev socket,id=n0,mcast=$(MCAST) \
            -device rtl8139,netdev=n0,mac=52:54:00:00:00:01
NODEDEV1  = -netdev socket,id=n0,mcast=$(MCAST) \
            -device rtl8139,netdev=n0,mac=52:54:00:00:00:02
NODEDEV2  = -netdev socket,id=n0,mcast=$(MCAST) \
            -device rtl8139,netdev=n0,mac=52:54:00:00:00:03

# -------------------------------------------------------------------
# ディスクイメージ (FAT32, ATA PIO経由でVFSからマウント)
# -------------------------------------------------------------------
DISK_IMG = disk.img
DISK_SIZE_MB = 32
USERLAND_X86 = ../../userland/x86

# FAT32ディスクイメージ作成
# mkdosfs / mformat が必要 (dosfstools パッケージ)
$(DISK_IMG): $(TARGET)
	dd if=/dev/zero of=$@ bs=1M count=$(DISK_SIZE_MB)
	mkdosfs -F 32 -n "PKERNEL" $@
	mcopy -i $@ $(TARGET) ::PKNL.BIN
	$(MAKE) -C $(USERLAND_X86) CROSS=$(CROSS)
	mcopy -i $@ $(USERLAND_X86)/01_posix/01_hello/hello.elf         ::hello.elf
	mcopy -i $@ $(USERLAND_X86)/01_posix/02_posix_io/posix_io.elf   ::posix_io.elf
	mcopy -i $@ $(USERLAND_X86)/01_posix/03_stdin_echo/stdin_echo.elf   ::stdin_echo.elf
	mcopy -i $@ $(USERLAND_X86)/01_posix/04_readdir/readdir.elf         ::readdir.elf
	mcopy -i $@ $(USERLAND_X86)/01_posix/05_posix2/posix2.elf           ::posix2.elf
	mcopy -i $@ $(USERLAND_X86)/01_posix/06_kdds/kdds_demo.elf          ::kdds_demo.elf
	mcopy -i $@ $(USERLAND_X86)/02_rtos/01_rtos_task/rtos_task.elf ::rtos_task.elf
	mcopy -i $@ $(USERLAND_X86)/02_rtos/02_rtos_sync/rtos_sync.elf ::rtos_sync.elf
	mcopy -i $@ $(USERLAND_X86)/02_rtos/03_mutex/mutex.elf             ::mutex.elf
	mcopy -i $@ $(USERLAND_X86)/02_rtos/04_task_ext/task_ext.elf      ::task_ext.elf
	mcopy -i $@ $(USERLAND_X86)/03_ipc/01_mailbox/mailbox.elf         ::mailbox.elf
	mcopy -i $@ $(USERLAND_X86)/03_ipc/02_msgbuf/msgbuf.elf           ::msgbuf.elf
	mcopy -i $@ $(USERLAND_X86)/03_ipc/03_rendezvous/rendezvous.elf  ::rendezvous.elf
	mcopy -i $@ $(USERLAND_X86)/04_memory/01_mempool/mempool.elf         ::mempool.elf
	mcopy -i $@ $(USERLAND_X86)/05_time/01_time/time.elf               ::time.elf
	mcopy -i $@ $(USERLAND_X86)/05_time/02_cyc_alm/cyc_alm.elf        ::cyc_alm.elf
	mcopy -i $@ $(USERLAND_X86)/06_system/01_ref/ref.elf                 ::ref.elf
	mcopy -i $@ $(USERLAND_X86)/06_system/02_misc/misc.elf               ::misc.elf
	mcopy -i $@ $(USERLAND_X86)/07_network/01_net_infer/net_infer.elf      ::net_infer.elf
	mcopy -i $@ $(USERLAND_X86)/07_network/02_http_get/http_get.elf       ::http_get.elf
	mcopy -i $@ $(USERLAND_X86)/07_network/03_multicast/multicast.elf     ::multicast.elf
	mcopy -i $@ $(USERLAND_X86)/07_network/04_edf_infer/edf_infer.elf     ::edf_infer.elf
	mcopy -i $@ $(USERLAND_X86)/08_system/01_kloader_d/kloader_d.elf      ::kloader_d.elf
	mcopy -i $@ $(USERLAND_X86)/09_ai/01_infer_d/infer_d.elf             ::infer_d.elf
	mmd   -i $@ ::etc 2>/dev/null || true
	mcopy -i $@ ../../samples/08_system/init.rc                            ::etc/init.rc

disk: $(DISK_IMG)

run: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 $(NETDEV)

run-headless: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 -display none $(NETDEV)

run-disk: $(TARGET) $(DISK_IMG)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 -display none \
	    -drive file=$(DISK_IMG),format=raw,if=ide,index=0

# ディスク + ネットワーク (RTL8139 QEMU user-net 10.0.2.0/24)
# TCP/UDP サンプル (18_http_get, 17_net_infer) を動かす場合に使用
run-disk-net: $(TARGET) $(DISK_IMG)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 -display none \
	    -drive file=$(DISK_IMG),format=raw,if=ide,index=0 $(NETDEV)

run-nonet: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 -display none

run-log: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial file:serial.log -cpu qemu64 $(NETDEV)

debug: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -s -S -cpu qemu64 $(NETDEV)

# デバッグシンボル付きビルド (gdb用)
TARGET_ELF = bootloader.elf
debug-build: CFLAGS += -g3 -O0
debug-build: LDFLAGS_ELF = -nostdlib -static -T linker.ld -m elf_i386
debug-build: $(ALL_OBJS)
	$(LD) $(LDFLAGS_ELF) -o $(TARGET_ELF) $^ $(LIBGCC)
	$(LD) $(LDFLAGS) -o $(TARGET) $^ $(LIBGCC)
	@echo "[OK] debug build: $(TARGET_ELF) + $(TARGET)"

# GDBアタッチ (別ターミナルで make debug 後に実行)
gdb: $(TARGET_ELF)
	gdb-multiarch -x .gdbinit $(TARGET_ELF)

# シンボルなしでも接続だけする場合
gdb-bare:
	gdb-multiarch -ex "set architecture i386:x86-64" \
	              -ex "target remote localhost:1234"

run-node0: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 $(NODEDEV0)

run-node1: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 $(NODEDEV1)

run-node2: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 $(NODEDEV2)

# -------------------------------------------------------------------
# kloader ベースの実行ターゲット
# kloader.bin (0x50000) → FAT32ディスク上の PKNL.BIN (0x100000) を起動
# -------------------------------------------------------------------
run-kloader: kloader.bin $(DISK_IMG)
	qemu-system-x86_64 -kernel kloader.bin -serial stdio -cpu qemu64 -display none \
	    -drive file=$(DISK_IMG),format=raw,if=ide,index=0

# 3ノードクラスタ kloader 版
# 事前に: cp disk.img node0.img && cp disk.img node1.img && cp disk.img node2.img
run-kloader-node0: kloader.bin node0.img
	qemu-system-x86_64 -kernel kloader.bin -serial stdio -cpu qemu64 \
	    -drive file=node0.img,format=raw,if=ide,index=0 $(NODEDEV0)

run-kloader-node1: kloader.bin node1.img
	qemu-system-x86_64 -kernel kloader.bin -serial stdio -cpu qemu64 \
	    -drive file=node1.img,format=raw,if=ide,index=0 $(NODEDEV1)

run-kloader-node2: kloader.bin node2.img
	qemu-system-x86_64 -kernel kloader.bin -serial stdio -cpu qemu64 \
	    -drive file=node2.img,format=raw,if=ide,index=0 $(NODEDEV2)

# 旧来の単体テスト用ターゲット (boot/x86のみ)
run-32: $(TARGET)
	qemu-system-i386 -kernel $(TARGET) -serial stdio -display none

run-boot: $(TARGET)
	qemu-system-x86_64 -drive format=raw,file=$(TARGET) -serial stdio -cpu qemu64 -display none

clean:
	rm -f $(ALL_OBJS) $(TARGET) $(KLOADER_OBJS) kloader.bin serial.log
	$(MAKE) -C $(USERLAND_X86) clean 2>/dev/null || true

clean-disk:
	rm -f $(DISK_IMG)

.PHONY: all clean clean-disk disk run run-disk run-disk-net run-log debug run-32 run-boot \
        run-node0 run-node1 run-node2 \
        run-kloader run-kloader-node0 run-kloader-node1 run-kloader-node2 \
        debug-build gdb gdb-bare
