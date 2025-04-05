# QEMU x64ブートローダー Makefile

# ツールチェイン設定 (標準gccを使用)
CC = gcc
AS = as
LD = ld

# コンパイルフラグ
CFLAGS = -Wall -Wextra -ffreestanding -fno-stack-protector -m64 -fno-builtin
ASFLAGS = -m64
LDFLAGS = -nostdlib -static -T linker.ld -m elf_x86_64

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
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
