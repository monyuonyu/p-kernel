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

POCS = poc_ctx_switch poc_preempt

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

# Legacy alias for Session 1
run: run-ctx

clean:
	rm -f $(CTX_OBJS) $(PRMP_OBJS) $(POCS)

.PHONY: all run run-ctx run-preempt clean
