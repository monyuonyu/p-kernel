# user_hello/Makefile
# Build hello.elf as a 32-bit static ELF for p-kernel ring-3 userspace

CROSS   ?= i686-linux-gnu-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld

CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-pic \
          -fcf-protection=none -nostdlib -O1 -std=gnu11 \
          -Wall -Wextra

TARGETS = hello.elf demo.elf test_all.elf

all: $(TARGETS)

hello.o: hello.c
	$(CC) $(CFLAGS) -c $< -o $@

hello.elf: hello.o user.ld
	$(LD) -m elf_i386 -static -nostdlib -T user.ld -o $@ $<

demo.o: demo.c plibc.h
	$(CC) $(CFLAGS) -c $< -o $@

demo.elf: demo.o user.ld
	$(LD) -m elf_i386 -static -nostdlib -T user.ld -o $@ $<

test_all.o: test_all.c plibc.h
	$(CC) $(CFLAGS) -c $< -o $@

test_all.elf: test_all.o user.ld
	$(LD) -m elf_i386 -static -nostdlib -T user.ld -o $@ $<

clean:
	rm -f hello.o hello.elf demo.o demo.elf test_all.o test_all.elf

.PHONY: all clean
