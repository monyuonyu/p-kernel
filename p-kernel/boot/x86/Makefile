# QEMU x64ブートローダー Makefile

# ツールチェイン設定 (標準gccを使用)
CC = gcc
AS = as
LD = ld

# コンパイルフラグ
CFLAGS = -Wall -Wextra -ffreestanding -fno-stack-protector -m32 -fno-pic
ASFLAGS = -32
LDFLAGS = -nostdlib -static -T linker.ld -m elf_i386

# ソースファイル
SRCS = start.S main.c
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

# QEMUでの実行とデバッグ
run: $(TARGET)
	qemu-system-i386 -kernel $(TARGET) -serial stdio

debug: $(TARGET)
	qemu-system-i386 -kernel $(TARGET) -serial stdio -s -S

# シリアル出力をファイルに保存
run-log: $(TARGET)
	qemu-system-i386 -kernel $(TARGET) -serial file:serial.log

# ブートディスクとしての実行
run-boot: $(TARGET)
	qemu-system-i386 -drive format=raw,file=$(TARGET) -serial stdio

clean:
	rm -f $(OBJS) $(TARGET) serial.log

.PHONY: all clean run debug run-log
