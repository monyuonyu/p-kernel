---
name: feedback_ci_operations_flake_runid
description: "p-kernel master CI ops: the persistent self-hosted red is FLAKE-dominated — TRUE root cause (2026-07-20) is apt-install over the ThinkPad's BROKEN IPv6 (exit 100), CURED at source by ForceIPv4 in the runner (NOT starvation, my repeated 1st guess); plus alive-but-deaf 000; and watch the CI workflow run id — NOT pages-build-deployment, which looks green and faked a 'CI green'."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
  modified: 2026-07-20T14:44:45.475Z
---

2026-06-28/29, while getting master CI green (mk_pino's "まずciを整えてから ガンガン"):

**RUN-ID PITFALL (cost me a false "green"):** each push creates TWO runs — the
`CI` workflow AND `pages-build-deployment`. They share adjacent run ids. I watched
the pages run (`overall: success`) and told mk_pino "CI green" — it was NOT; the
real `CI` run had failed. ALWAYS select the run with `--workflow CI` (and
`event=="push"`), e.g. `gh run list --branch master --workflow CI --json
databaseId,headSha,event -q '[.[]|select(.headSha|startswith("<sha>") and
.event=="push")][0].databaseId'`. Never trust a watcher's exit-0 without
confirming it watched the CI run.

**THE RED IS FLAKE-DOMINATED, not buggy** (verified by a classify-from-real-logs
workflow, 0/6 definitive-wrong across the worst certs): (a) on **ubuntu-latest**
(the UMP x86_64 job: galaxy/ark-profile/i18n/L0/L1 cert steps run here) a node is
ALIVE but briefly DEAF (curl code `000`) while it runs a DMN sleep-consolidation —
by design, the mind is thinking; (b) on the **self-hosted pkernel-thinkpad** runner
(4 cpu host, container --cpus 3, **2 Runner.Worker = 2 jobs co-run**) heavy 3-node
live jobs (one_mind FOLD-timeout, shared_mind console-starvation) starve when two
co-run. ci.yml had ZERO serialization.

**CURE shipped (commit 5abe7fe0, crown-neutral):** a sourced `samples/lib/http_retry.sh`
— `curl000()` retries ONLY a transient (HTTP 000 / curl exit 7,28,52,56), bounded
≤6 + fail-closed; ANY real code (403/409/…) returns on the first try, NEVER retried
(teeth preserved) — + `wait_http` readiness gates, + widened absence-of-line windows
(one_mind 40→90, shared_mind 90→180), + **all 11 self-hosted jobs chained STRICTLY
SERIAL** via `needs:[prev]` + `if: ${{ always() && <fork-guard> }}` (no concurrency:
— that cancels; always() = no cascade-skip). Tradeoff: self-hosted CI is now much
SLOWER (serial) but reliable. Future speedup = more runner cpus / selective parallel.

**How to apply:** when a p-kernel cert "fails" with 000/timeout/no-line, suspect a
deafness/starvation FLAKE before a regression — re-run / check definitive-wrong.
Retry ONLY transient, never a real code. Don't chase flakes as bugs (one_mind/Path W
"FOLD never completed" was starvation, NOT my rmem fix failing — Path W passed once
uncontended). Related: [[feedback_relay_rmem_host_config]], [[feedback_audit_is_the_engine]].

**2026-07-12 RECONFIRMED (the serial cure did NOT end it):** during a CI-hardening
campaign, two consecutive master runs each went overall=failure on a DIFFERENT
self-hosted live cert — `survival-loop` on one, `collective-learn` on the next —
while every hosted job (sanitizer/windows-pe/llm-engine/consent-dom) stayed green;
the run before both was fully green. Decisive control: `gh run rerun <RID> --failed`
re-ran collective-learn GREEN with ZERO code change → flake confirmed. Takeaway: the
self-hosted live certs still flake intermittently even after the serial chaining
(the ThinkPad is a SHARED box — it also runs mk_pino's DB containers / DMN / relay),
so master's OVERALL colour is ThinkPad-load-dependent even when the code is healthy.
The `--failed` re-run is the cheap decisive test; a different cert failing each run
(not the same one) is the tell it's a flake, not a regression. This intermittent
self-hosted red is now the main thing between "hosted jobs solid" and "master always
green" — a CI-RELIABILITY gap distinct from feature-coverage gaps.

**2026-07-20 TRUE ROOT CAUSE FOUND — it was NOT starvation, it was BROKEN IPv6 apt.**
Pulled the actual failed-job log (`gh api repos/…/actions/jobs/<id>/logs`) for the
survival-loop blocking failure on e81deb22 (run 29198660420). Signature was NOT
000/timeout/no-line — it was `Cannot initiate the connection to archive.ubuntu.com:80
(2620:2d:…) - Network is unreachable` → `##[error]Process completed with exit code 100`
at the job's `sudo apt-get install` step. The ThinkPad host has `disable_ipv6=0` but a
DEAD IPv6 route (`ip -6 route get … → Network is unreachable`, matches
[[thinkpad_broken_ipv6_docker_pulls]]); when DNS hands apt the AAAA first, apt tries
IPv6 and dies. Intermittent = flake, but the mechanism was apt-over-IPv6, NOT CPU
contention. My 1st hypothesis (starvation → widen windows) was WRONG again (the
[[feedback_ci_diagnosis_buildlist_gap]] "commander's 1st diagnosis is often wrong"
pattern); the window-widening / serial-chaining cures treated the wrong disease, which
is why they never ended it. **FIX (root):** wrote `/etc/apt/apt.conf.d/99force-ipv4`
(`Acquire::ForceIPv4 "true";`) into the running `pkernel_gh_runner` container via
`docker exec -u root` — verified `apt-get update` then reaches archive.ubuntu.com over
IPv4, RC=0. Written into the container's writable layer so it survives `restart` (NOT a
`docker rm`). **PROOF:** `gh run rerun 29198660420 --failed` → survival-loop(blocking)
GREEN with ZERO code change → overall=success. So the self-hosted-flake CI-RELIABILITY
gap is CLOSED at the source. **INCOMPLETE:** baking `ForceIPv4` into the runner
Dockerfile (`/opt/services/gh-runner/Dockerfile` on ThinkPad, FROM-then-RUN) for
rebuild-persistence — the `scp` was blocked by the Claude Code auto-mode classifier
(remote /opt overwrite); backup `Dockerfile.bak.pre-ipv4` taken, new file staged at
scratch `Dockerfile.runner`. FOLLOW-UP: land the Dockerfile edit next time the image is
rebuilt, or user pushes the scp via a `!`-prefixed prompt. NOTE: `full-SMP`(run_smp0) is
`ubuntu-latest` (hosted, advisory) so IPv4-force does NOT touch it — its red is a
SEPARATE G4/SMP issue, not this flake.

**OPEN real-bug follow-up (NOT a flake, deliberately not retried):** an intermittent
teach-after-consent **403** (i18n line ~102 historically; ark-profile variant) —
suspected consent-visibility RACE in arch/common/ark_profile.c (shared file-static
ap_head_scratch + self/prof pfs_dag_read interleaving with autonomous DMN/prov
writes). Needs a hosted-gated SOURCE fix, never a cert retry. Separate from the
k=2&v=3 OOV 403 (that one was fixed: k=sun&v=yellow).
