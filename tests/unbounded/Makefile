# ===========================================================================
# tests/unbounded/Makefile — the [unbounded-*] cert (unbounded_n_design.md §8/§9)
#
# Three MECHANICAL gates, all built here (design §9: "build the disease binary,
# never hand-run"). This replaces the prior printf-math theater the audit
# rejected — every number below comes from the REAL shipped kernel headers.
#
#   make cure      [unbounded-flat]     measure the REAL per-node kernel structs
#                                        (sizeof DKVA_CAGG / NODEMAP / kdds_topics,
#                                        real KDDS_TOPIC_MAX) + a runtime R-bound
#                                        on the REAL nodemap primitive. A byte
#                                        budget gate: a [4096] added to the
#                                        MEASURED kernel struct → RED.
#   make coupling  [unbounded-coupling] recompile the REAL headers at DNODE_MAX
#                                        ∈ {64,256,1024}; PROVE the R-sized cost
#                                        is byte-for-byte CONSTANT while the
#                                        fleet-sized table (dnode_table) GROWS.
#   make disease   [unbounded-disease]  build the wave-45 CONTROL: the pre-fix
#                                        global-view sizing with the wire bumped.
#                                        It MUST FAIL to compile (topic-table
#                                        overflow + pmesh beacon MTU). A clean
#                                        build here is a HARD cert failure.
#
#   make check     run all three; exit 0 iff cure GREEN, coupling FLAT, disease
#                  REFUSED to build.
# ===========================================================================

ROOT   := ../..
CC     ?= cc
CFLAGS := -std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla \
          -D_APP_AARCH64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ -D_LINUX_AARCH64_
INC := -I$(ROOT)/arch/linux/aarch64/include \
       -I$(ROOT)/arch/linux/include \
       -I$(ROOT)/arch/aarch64/include \
       -I$(ROOT)/arch/common/include/lp64 \
       -I$(ROOT)/arch/common/include \
       -I$(ROOT)/relay \
       -I$(ROOT)/kernel/mtkernel3/kernel/knlinc \
       -I$(ROOT)/kernel/mtkernel3/kernel/tkernel \
       -I$(ROOT)/kernel/mtkernel3/include \
       -I$(ROOT)/kernel/mtkernel3/include/tm \
       -I$(ROOT)/kernel/mtkernel3/include/compat \
       -I$(ROOT)/kernel/mtkernel3/config

B := ./_build

.PHONY: all check cure coupling disease clean

all: check

$(B):
	@mkdir -p $(B)

# --- [unbounded-flat] : the CURE, measuring REAL kernel structs -------------
cure: $(B)
	@echo "== [unbounded-flat] build (real DKVA_CAGG / NODEMAP / kdds_topics) =="
	@$(CC) $(CFLAGS) $(INC) unbounded_test.c -lm -o $(B)/unb
	@$(B)/unb

# --- [unbounded-coupling] : widen the wire, prove R-cost constant -----------
coupling: $(B)
	@echo "== [unbounded-coupling] widen DNODE_MAX 64->256->1024, R-cost must be FLAT =="
	@$(CC) $(CFLAGS) $(INC) -DDNODE_MAX=64   sizeprobe.c -o $(B)/p64
	@$(CC) $(CFLAGS) $(INC) -DDNODE_MAX=256  sizeprobe.c -o $(B)/p256
	@$(CC) $(CFLAGS) $(INC) -DDNODE_MAX=1024 sizeprobe.c -o $(B)/p1024
	@$(B)/p64   > $(B)/o64
	@$(B)/p256  > $(B)/o256
	@$(B)/p1024 > $(B)/o1024
	@echo "  wire=64  : `cat $(B)/o64`"
	@echo "  wire=256 : `cat $(B)/o256`"
	@echo "  wire=1024: `cat $(B)/o1024`"
	@sh ./coupling_check.sh $(B)/o64 $(B)/o256 $(B)/o1024

# --- [unbounded-disease] : the wave-45 CONTROL must FAIL to build ------------
disease: $(B)
	@echo "== [unbounded-disease] build the pre-fix global view at a BUMPED wire =="
	@echo "   (expected: the shipped compile-time gates REFUSE it — wave-48 + MTU)"
	@if $(CC) $(CFLAGS) $(INC) -DDNODE_MAX=1024 -DKDDS_DKVA_PREOPEN_SCALE=DNODE_MAX \
	        disease.c -o $(B)/disease 2>$(B)/disease.err ; then \
	    echo "  [unbounded-disease]  FAIL — the disease BUILT (control unfalsified!)"; \
	    rm -f $(B)/disease; exit 1; \
	else \
	    echo "  disease REFUSED to build (correct). Compiler diagnostics:"; \
	    grep -aE "static_assert|Static_assert|DISEASE|wave-48|overflow|MTU|beacon" \
	        $(B)/disease.err | sed 's/^/    /' | head -8; \
	    echo "  [unbounded-disease]  PASS (localization, not the constant, is the fix)"; \
	fi

check: cure coupling disease
	@echo
	@echo "[unbounded-*] ALL GATES GREEN (cure flat + coupling constant + disease refused)"

clean:
	@rm -rf $(B)
