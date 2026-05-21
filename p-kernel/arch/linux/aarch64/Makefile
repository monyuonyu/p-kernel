# arch/linux/aarch64/Makefile — proof-of-concept build.
#
# Standalone for now. Once Session 2 wires the rest of the arch/linux
# port (signals, sio, the full T-Kernel integration), the kernel build
# will subsume this and the PoC source will move to a tests/ tree or
# be deleted.
#
# Usage:
#   make             — build ./poc_ctx_switch
#   make run         — build and run, expect five A/B alternations
#   make clean

CC      = cc
CFLAGS  = -O2 -Wall -Wextra -g -fno-omit-frame-pointer
LDFLAGS =

POC  = poc_ctx_switch
OBJS = poc_ctx_switch.o ctx_switch.o
HDR  = ../include/arch_ctx.h

all: $(POC)

$(POC): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

poc_ctx_switch.o: poc_ctx_switch.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

ctx_switch.o: ctx_switch.S
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(POC)
	./$(POC)

clean:
	rm -f $(OBJS) $(POC)

.PHONY: all run clean
