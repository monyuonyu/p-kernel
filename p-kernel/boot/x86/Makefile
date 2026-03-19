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
    drpc.c

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
# ビルドルール
# -------------------------------------------------------------------
all: $(TARGET)

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

# Distributed two-node setup (QEMU socket networking = virtual Ethernet cable)
# Run in two separate terminals: make run-node0, then make run-node1
NODEDEV0 = -netdev socket,id=n0,listen=:1234 \
           -device rtl8139,netdev=n0,mac=52:54:00:00:00:01
NODEDEV1 = -netdev socket,id=n0,connect=localhost:1234 \
           -device rtl8139,netdev=n0,mac=52:54:00:00:00:02

run: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 $(NETDEV)

run-headless: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 -display none $(NETDEV)

run-nonet: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 -display none

run-log: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial file:serial.log -cpu qemu64 $(NETDEV)

debug: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -s -S -cpu qemu64 $(NETDEV)

run-node0: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 $(NODEDEV0)

run-node1: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 $(NODEDEV1)

# 旧来の単体テスト用ターゲット (boot/x86のみ)
run-32: $(TARGET)
	qemu-system-i386 -kernel $(TARGET) -serial stdio -display none

run-boot: $(TARGET)
	qemu-system-x86_64 -drive format=raw,file=$(TARGET) -serial stdio -cpu qemu64 -display none

clean:
	rm -f $(ALL_OBJS) $(TARGET) serial.log

.PHONY: all clean run run-log debug run-32 run-boot run-node0 run-node1
