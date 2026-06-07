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

POCS = poc_ctx_switch poc_preempt poc_dispatch

CTX_OBJS  = poc_ctx_switch.o ctx_switch.o
CTX_HDR   = ../include/arch_ctx.h
PRMP_OBJS = poc_preempt.o trampoline.o
PRMP_HDR  = ../include/arch_preempt.h

# poc_preempt needs librt for timer_create/timer_settime (only matters
# on glibc < 2.34; harmless on newer toolchains).
PRMP_LDFLAGS = $(LDFLAGS) -lrt

# Session 3a needs T-Kernel headers (offset.h, utk_config_depend.h,
# config.h) so cpu_support.S can resolve TCB_SSP. The Linux x86_64
# offset.h shadow lives in include/.
DISPATCH_CFLAGS = -I$(CURDIR)/include \
                  -I$(CURDIR)/../include \
                  -I$(CURDIR)/../../../include/kernel/tkernel \
                  -I$(CURDIR)/../../../include \
                  -D_APP_X86_64_ -D_APP_LINUX_
DISPATCH_OBJS = poc_dispatch.o cpu_support.o

all: $(POCS)

poc_ctx_switch: $(CTX_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

poc_preempt: $(PRMP_OBJS)
	$(CC) -o $@ $^ $(PRMP_LDFLAGS)

poc_ctx_switch.o: poc_ctx_switch.c $(CTX_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

ctx_switch.o: ctx_switch.S
	$(CC) $(CFLAGS) -c -o $@ $<

poc_preempt.o: poc_preempt.c $(PRMP_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

trampoline.o: trampoline.S
	$(CC) $(CFLAGS) -c -o $@ $<

# Session 3a — T-Kernel dispatcher unit test
poc_dispatch: $(DISPATCH_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

poc_dispatch.o: poc_dispatch.c
	$(CC) $(CFLAGS) -c -o $@ $<

cpu_support.o: cpu_support.S
	$(CC) $(CFLAGS) $(DISPATCH_CFLAGS) -c -o $@ $<

run-ctx: poc_ctx_switch
	$(RUN) ./poc_ctx_switch

run-preempt: poc_preempt
	$(RUN) ./poc_preempt

run-dispatch: poc_dispatch
	$(RUN) ./poc_dispatch

# Legacy alias
run: run-ctx

clean:
	rm -f $(CTX_OBJS) $(PRMP_OBJS) $(DISPATCH_OBJS) $(POCS)

.PHONY: all run run-ctx run-preempt run-dispatch clean
