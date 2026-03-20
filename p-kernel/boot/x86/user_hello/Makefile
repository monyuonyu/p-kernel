# user_hello/Makefile
# Build hello.elf as a 32-bit static ELF for p-kernel ring-3 userspace

CROSS   ?= i686-linux-gnu-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld

CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-pic \
          -fcf-protection=none -nostdlib -O1 -std=gnu11 \
          -Wall -Wextra

TARGET  = hello.elf

all: $(TARGET)

hello.o: hello.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): hello.o user.ld
	$(LD) -m elf_i386 -static -nostdlib -T user.ld -o $@ $<

clean:
	rm -f hello.o $(TARGET)

.PHONY: all clean
