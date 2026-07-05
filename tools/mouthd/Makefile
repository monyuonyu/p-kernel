# tools/mouthd/Makefile — the Frontier Mouth host companion daemon.
#
#   make          — build ./mouthd
#   make clean
#
# A HOST tool (like tools/relay), NOT kernel code and NOT in CI. It includes
# only arch/common/llm/frontier.h for the FM1 op bytes + fr_echo_of + the
# teacher_kind enum; it links no kernel object. TLS to the provider is done by
# invoking the system curl (a host tool is allowed a TLS stack — the kernel is
# not). Binds 127.0.0.1 only.
CC      = cc
FRONTIER_H_DIR = ../../arch/common/llm
CFLAGS  = -Wall -Wextra -O2 -std=gnu11 -fno-stack-protector -I$(FRONTIER_H_DIR)
LDFLAGS =

TARGET = mouthd

all: $(TARGET)

$(TARGET): mouthd.c $(FRONTIER_H_DIR)/frontier.h
	$(CC) $(CFLAGS) -o $@ mouthd.c $(LDFLAGS)
	@echo "[OK] mouthd built ($$(stat -c %s $@) bytes) — [live] only, never CI"

clean:
	rm -f $(TARGET)

.PHONY: all clean
