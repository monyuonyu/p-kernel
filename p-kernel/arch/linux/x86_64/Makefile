# arch/linux/x86_64/Makefile — proof-of-concept build for the x86_64
# Linux user-mode HAL backend. Mirrors arch/linux/aarch64/Makefile.
#
# Host architecture: this needs an x86_64 toolchain. On an aarch64
# build machine, set CROSS=x86_64-linux-gnu- and run binaries via
# qemu-x86_64 (qemu-user).
#
# Usage:
#   make             — build all PoCs
#   make run-ctx     — Session 1: cooperative context switch
#                      (auto-prefixes RUN with qemu-x86_64 when cross-
#                       compiling on a non-x86_64 host)
#   make clean

# Pick toolchain and runner. If running on a non-x86_64 host, fall back
# to a cross compiler and run binaries through qemu-x86_64.
ifeq ($(shell uname -m),x86_64)
CROSS  ?=
RUN    ?=
else
CROSS  ?= x86_64-linux-gnu-
RUN    ?= qemu-x86_64
endif

CC      = $(CROSS)gcc
CFLAGS  = -O2 -Wall -Wextra -g -fno-omit-frame-pointer
LDFLAGS = -static

POCS = poc_ctx_switch

CTX_OBJS = poc_ctx_switch.o ctx_switch.o
CTX_HDR  = ../include/arch_ctx.h

all: $(POCS)

poc_ctx_switch: $(CTX_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

poc_ctx_switch.o: poc_ctx_switch.c $(CTX_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

ctx_switch.o: ctx_switch.S
	$(CC) $(CFLAGS) -c -o $@ $<

run-ctx: poc_ctx_switch
	$(RUN) ./poc_ctx_switch

# Legacy alias
run: run-ctx

clean:
	rm -f $(CTX_OBJS) $(POCS)

.PHONY: all run run-ctx clean
