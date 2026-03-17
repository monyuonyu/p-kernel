# QEMU x64ブートローダー Makefile

# ツールチェイン設定 (i686クロスコンパイラを使用)
CROSS = i686-linux-gnu-
CC = $(CROSS)gcc
AS = $(CROSS)as
LD = $(CROSS)ld

# コンパイルフラグ（32ビットブート、64ビット実行）
CFLAGS = -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic
ASFLAGS =
LDFLAGS = -nostdlib -static -T linker.ld -m elf_i386

# ソースファイル
SRCS = start.S main.c idt.c isr.S memory.c
OBJS = $(SRCS:.c=.o)
OBJS := $(OBJS:.S=.o)

# 出力ファイル
TARGET = bootloader.bin

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(AS) $(ASFLAGS) $< -o $@

# QEMUでの実行とデバッグ（64ビット対応CPUで32ビットベース実行）
run: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -cpu qemu64 -display none

debug: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial stdio -s -S -cpu qemu64 -display none

# シリアル出力をファイルに保存
run-log: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -serial file:serial.log -cpu qemu64 -display none

# 32ビットモードでの実行
run-32: $(TARGET)
	qemu-system-i386 -kernel $(TARGET) -serial stdio -display none

# ブートディスクとしての実行
run-boot: $(TARGET)
	qemu-system-x86_64 -drive format=raw,file=$(TARGET) -serial stdio -cpu qemu64 -display none

clean:
	rm -f $(OBJS) $(TARGET) serial.log

.PHONY: all clean run debug run-log
