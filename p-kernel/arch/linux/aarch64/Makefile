# arch/linux/aarch64/Makefile — proof-of-concept build.
#
# Standalone for now. Once Session 3 wires the rest of the arch/linux
# port (sio, full T-Kernel integration), the kernel build will subsume
# this Makefile and these PoC sources will move to a tests/ tree or
# be deleted.
#
# Usage:
#   make                — build all PoCs
#   make run-ctx        — Session 1: cooperative context switch
#   make run-preempt    — Session 2: timer-driven preemption
#   make clean

CC      = cc
CFLAGS  = -O2 -Wall -Wextra -g -fno-omit-frame-pointer
LDFLAGS = -lrt

# Session 3a's dispatcher PoC needs adrp/add against globals defined
# in the same final executable. PIE adds a layer of indirection (GOT
# / PLT) that the dispatcher's inline asm does not perform. Disable
# PIE for now; full T-Kernel integration in Session 3b will revisit.
DISPATCH_LDFLAGS = -no-pie

# Build path so cpu_support.S finds offset.h from the aarch64 tree:
DISPATCH_CFLAGS = -no-pie \
                  -I$(CURDIR)/include \
                  -I$(CURDIR)/../include \
                  -I$(CURDIR)/../../aarch64/include \
                  -I$(CURDIR)/../../common/include \
                  -I$(CURDIR)/../../../include/kernel/tkernel \
                  -I$(CURDIR)/../../../include \
                  -I$(CURDIR)/../../../include/lib/libc \
                  -D_APP_AARCH64_ -D_APP_LINUX_

POCS = poc_ctx_switch poc_preempt poc_dispatch

# Session 1 — cooperative arch_ctx_switch
CTX_OBJS  = poc_ctx_switch.o ctx_switch.o
CTX_HDR   = ../include/arch_ctx.h

# Session 2 — preemption via SIGALRM + mcontext rewrite
PRMP_OBJS = poc_preempt.o trampoline.o
PRMP_HDR  = ../include/arch_preempt.h

all: $(POCS)

poc_ctx_switch: $(CTX_OBJS)
	$(CC) -o $@ $^

poc_preempt: $(PRMP_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

# Session 3a — T-Kernel dispatcher unit test
DISPATCH_OBJS = poc_dispatch.o cpu_support.o
poc_dispatch: $(DISPATCH_OBJS)
	$(CC) $(DISPATCH_LDFLAGS) -o $@ $^

# poc_dispatch.c uses system libc; cpu_support.S needs T-Kernel headers
# (offset.h). Different include paths.
poc_dispatch.o: poc_dispatch.c
	$(CC) $(CFLAGS) -no-pie -c -o $@ $<

cpu_support.o: cpu_support.S
	$(CC) $(CFLAGS) $(DISPATCH_CFLAGS) -c -o $@ $<

poc_ctx_switch.o: poc_ctx_switch.c $(CTX_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

ctx_switch.o: ctx_switch.S
	$(CC) $(CFLAGS) -c -o $@ $<

poc_preempt.o: poc_preempt.c $(PRMP_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

trampoline.o: trampoline.S
	$(CC) $(CFLAGS) -c -o $@ $<

run-ctx: poc_ctx_switch
	./poc_ctx_switch

run-preempt: poc_preempt
	./poc_preempt

run-dispatch: poc_dispatch
	./poc_dispatch

# Legacy alias for Session 1
run: run-ctx

clean:
	rm -f $(CTX_OBJS) $(PRMP_OBJS) $(DISPATCH_OBJS) $(POCS)

.PHONY: all run run-ctx run-preempt run-dispatch clean
