# relay/Makefile
#
# Build:
#   make             — build the relay (./relay) and the test harness
#   make test        — build everything and run the round-trip smoke test
#   make clean
#
# The relay is two C sources (relay.c + sha256.c — no openssl). The
# test harness is independent: spawns the relay as a child, drives
# simulated clients, then verifies forwarding + auth + replay rules.

CC      = cc
CFLAGS  = -Wall -Wextra -O2 -std=gnu11 -fno-stack-protector
LDFLAGS =

TARGETS = relay test_relay

all: $(TARGETS)

relay: relay.c sha256.c sha256.h
	$(CC) $(CFLAGS) -o $@ relay.c sha256.c $(LDFLAGS)
	@echo "[OK] relay built ($$(stat -c %s $@) bytes)"

test_relay: test_relay.c sha256.c sha256.h
	$(CC) $(CFLAGS) -o $@ test_relay.c sha256.c $(LDFLAGS)
	@echo "[OK] test_relay built ($$(stat -c %s $@) bytes)"

test: $(TARGETS)
	./test_relay

clean:
	rm -f $(TARGETS)

.PHONY: all test clean
