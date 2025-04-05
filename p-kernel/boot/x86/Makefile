# QEMU x64ブートローダー Makefile

# ツールチェイン設定
CC = x86_64-elf-gcc
AS = x86_64-elf-as
LD = x86_64-elf-ld

# コンパイルフラグ
CFLAGS = -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
ASFLAGS = 
LDFLAGS = -nostdlib -static -T linker.ld

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
