---
name: feedback-ci-diagnosis-buildlist-gap
description: "How to diagnose p-kernel CI red from the sandbox (job-level logs via gh api, ThinkPad tar-over-ssh for hosted runtime), and the cert-first blind spot — adding a fn/file to arch/common silently breaks every existing build list that links it."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

Two hard-won CI lessons from the 2026-07-07 green campaign ([[project_8design_implementation]]).

**1. Reading CI red from the PRoot sandbox.** `gh run view --log` / `--log-failed` return EMPTY here
(the zip-extract step is blocked in the sandbox). BUT the per-job log is plain text via the API and
needs no unzip: `gh api repos/{owner}/{repo}/actions/jobs/<jobId>/logs`. Get the failed jobs +
their true failed STEP with `gh run view <runId> --json jobs` then
`gh api .../actions/jobs/<id> -q '.steps[]|select(.conclusion=="failure").name'`. This is how the
real failure line (e.g. `make: aarch64-linux-gnu-gcc: No such file`, `undefined reference to
conscience_check`) becomes visible when the summary only says "exit code 1". mk_pino's prompt "thinkpad
にアクセスできるのでは？" was the unlock — don't give up at an empty `--log`.

**2. Hosted RUNTIME certs must run on the ThinkPad, not the sandbox.** Native x86_64 `./p-kernel`
SIGILLs under PRoot; qemu-aarch64 boots but student inference never completes (>1500s), so
`[law-teacharound]`-style legs behind `m_ask`/`student_chat_generate` are unreachable locally. The
ThinkPad is a REAL x86_64 → build + run there. `~/p-kernel` is NOT a git repo (the self-hosted runner
lives in the docker container `pkernel_gh_runner`), and the HOST has no `qemu-system-aarch64` — so
DON'T try to reproduce bare-metal SMP on the host. Instead **tar-over-ssh** a committed tree:
`tar czf - --exclude=.git --exclude='*.o' . | ssh shota@192.168.10.100 'rm -rf /tmp/X;mkdir /tmp/X;
tar xzf - -C /tmp/X'`, then `make -C /tmp/X/boot/linux_x86_64` and drive the CI stdin sequence. Toolchain
gap A itself is impl-innocent and reproduces GREEN locally once the toolchain is present.

**3. The cert-first blind spot (why 8 designs each passing left CI red).** When you add a function or
.c to `arch/common`, EVERY existing build list that links that TU must be updated in the SAME change —
they are not covered by the new design's own fixture: `tests/**/run_*.sh` SRC lists (yield-test needed
conscience.c/frontier.c), `tools/android/check_parity.sh` (host-only files → documented allowlist, not
CMake add), pinned forward hashes (`student_devfit_test.c` after a RoPE/seq change → re-pin to the
sibling ss6's GREEN values, with the reason in a comment — NOT theater), and any CI `grep` string tied
to a Makefile message. A whole-program cross-audit still misses this layer; only running the FULL CI
(or `git grep` for callers of the new symbol) catches it.
**3b. `check_parity.sh` itself has a blind spot: it does NOT compare the LLM source lists** (host
`boot/linux*/Makefile` `LLM_C_SRCS` vs Android `CMakeLists.txt` `LLM_STUDENT_SRC`/`LLM_STUB_SRC`) —
it only diffs COMMON/ARCH_SHARED/ARCH/KERNEL/RELAY. So an llm/*.c that is in the host build but missing
from CMake passes check_parity GREEN yet breaks the Android NDK link (`--no-undefined` is the NDK
default for SHARED libs → real APK build fails identically). 2026-07-11: Wave-C added a
`dlb_compound_*` call into `student_shell.c` (a CMake-compiled TU) but `dlb.c` was absent from
CMake LLM_STUDENT_SRC; check_parity stayed green, only the NDK link (r26.3, `llvm-nm` shows
`U dlb_compound_*`) exposed it. Fix landed (49ffdf29, dlb.c → LLM_STUDENT_SRC). FOLLOW-UP: extend
check_parity.sh to diff the LLM lists so this drift class is guarded, not just link-time. When you touch
any hosted `arch/common/llm/*.c` symbol from a CMake-linked TU, `git grep` the symbol AND check the
Android CMake list — check_parity will not save you here.

**4. The commander's first diagnosis is often wrong — that's why impl≠audit≠commander.** This run the
commander mis-diagnosed B3 (blamed disease.c gate-order; real cause = stale grep string) and B4 (blamed
`ink` unregistered; real = ink is vocab-15, resident from cumulative state), and proposed a B4 delta
patch that the AUDITOR flagged as masking a possible rebind-leak → hardened to seq-invariant. Trust the
separate auditor to refute you; verify mechanism on the real box before crediting a fix.
[[feedback_audit_is_the_engine]] [[feedback_the_debug_env_is_real]] [[feedback_ci_operations_flake_runid]]
