# relay/Makefile
#
# Build:
#   make             — build the relay (./relay) and the test harness
#   make test        — build everything and run the round-trip smoke test
#   make clean
#
# The relay is one static C source. The test harness is independent —
# it spawns the relay as a child, drives two simulated clients, then
# verifies cross-forwarding.

CC      = cc
CFLAGS  = -Wall -Wextra -O2 -std=gnu11 -fno-stack-protector
LDFLAGS =

TARGETS = relay test_relay

all: $(TARGETS)

relay: relay.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "[OK] relay built ($$(stat -c %s $@) bytes)"

test_relay: test_relay.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "[OK] test_relay built ($$(stat -c %s $@) bytes)"

test: $(TARGETS)
	./test_relay

clean:
	rm -f $(TARGETS)

.PHONY: all test clean
