# BACKLOG — the master ordered TODO (mk_pino: "順次 全部 やりたい")

The single source of truth for everything designed/decided but not yet done. Marched through
in dependency order, each item as a background **implement → independent audit → commander
integrate** wave (the development-method constitution). Disjoint-file items run in parallel;
same-file items serialize. Update this file as items land.

## ✅ DONE (shipped, on trunk, on the phones at 0.9.2)
Chat-as-student (⑥), living-body inspector (organs→real vitals, 3D rings, amoeba, immersion),
4-panel intro, legible biometrics, baby-births-on-phone, **SS-1** (adaptive-K = heavy→wider
firing), **SS-2** (tier scaffolding S/M/L), in-kernel SmolLM2 teacher engine, the build-link
weak-stub fix, flash-wear throttle. Ed25519 signing, persistence, living-mind LM-1..11,
inference-engine M1 (gguf/quant/forward/tokenizer), galaxy, ark-profile — all already live
(their docs' "not started"/"実装前" Status lines are STALE → fix them, see bottom).

## 🟡 IN FLIGHT (background lanes)
- **KV cache** (faster chat) — in separate audit; on PASS → 0.9.3 build (with Note10 fix + ⑤).
- **survival-§7/G38 design-harden** — automated cert-first design workflow → an implementation-ready plan doc (read-only on code).

## ✅ DONE THIS SESSION (on trunk, NOT yet in an APK — next APK = 0.9.3)
- **self-access R0** (`d730175e`): READ-ONLY body introspection verb `body` + Q3 self/lin lineage append (1/invocation, tamper-evident). Separate audit PASS / ledger CLOSE.
- **N-2 slice 1** (`1f656c3c`): the emergent-supernode deterministic selector + host cert (see Thread N below). Separate audit PASS-WITH-NITS / CLOSE (auditor falsified the cert by sabotage → it correctly went RED).
- **N-2b SWIM capability-bit gossip** (wave-n2b-capability-gossip): makes slice-1's `super_capable[]` table FLEET-REAL — each node self-declares capability (`PKERNEL_SUPERNODE`) and it propagates over SWIM, converging every node on the same supernode with NO vote. Reuses the reserved `_pad` byte of `SWIM_GOSSIP_EVT` (wire byte-identical, **`SWIM_VERSION` NOT bumped** → old nodes emit 0 = non-capable = relay-fallback, never crash). Self-authoritative + relayed verbatim; applied under the SAME `(incarnation,state)` LWW gate (no regress on a stale rumor). Host cert `swim_cap_gossip_self_test()` (verb `nodes cap`): converge + staleness + falsifiable (sabotage-rebuild → RED), PASS on aarch64+x86_64 linux; `nodes test`/`region test` regress clean. Honest bound: capability env-fixed at init (no runtime flip yet); true multi-process live mesh left as a deferred `[live]` row.
- **interoception slice-1** (`9e5529a2`): the unified stress **S_n bus** (`arch/common/interocept.c`, integer EWMA over REAL sources: reflex-threat/SWIM-RTT/in-context-surprise/ring3-fault/degrade) → **DMN idle-threshold modulation** (stress shrinks the sleep gap, deadband-16 + hysteresis, only pulls down) + **galaxy me-star real mood/hue** (`me.s_n`/`me.s_axis`). Separate audit PASS. Apoptosis (slice-2) still queued.

## ▶ ORDERED QUEUE (the marathon)

### Thread N — Network = the "Skype-like" P2P (mk_pino's passion; verdict SOUND)
- **N-0 decentralized node-id** (HARD PREREQUISITE — two phones default to id=1 → SWIM ignores each as "self"). Key-derived/random unique id. [touches the boot/id path]
- **N-1 LAN-direct** `net_lan.c` (3rd transport backend behind PKERNEL_LAN=1; net_unix.c template, 127.0.0.1→255.255.255.255:7351 + learned-peer table; SWIM's broadcast beacon becomes real LAN UDP). → **two same-WiFi phones auto-mesh, NO relay.** Host-first (2 namespaces/veth; SO_REUSEPORT caveat), then Android (CMake TU + parity). [net layer]
- **N-2 emergent supernodes** (relay forwarding logic lifted into capable nodes, NOCENTRAL deterministic-from-SWIM). **N-3 NAT hole-punching** (supernode-assisted; cone-NAT only, symmetric stays relayed). **N-4 bootstrap/seed** (PKERNEL_SEED list; relay = one optional seed). Doc: p2p-overlay.md.
  - **N-2 slice 1 DONE** (wave-n2-supernode-select): the deterministic **selection function** `region_supernode()` in `arch/common/region.c` (lowest-id node that is BOTH a region member AND supernode-capable; `0xFF` = degrade to central relay) + a local capability table/setter + `PKERNEL_SUPERNODE=1` self opt-in + host cert `region_supernode_test()` (verb `region test`; 8/8 PASS: lowest-capable-wins, convergence/determinism, survives-death by recompute (no vote), relay-fallback, non-member-capable ignored). Integer-only → cross-arch identical.
  - **N-2 slice 2b DONE** (wave-n2b-capability-gossip): SWIM **capability-bit gossip** — the self-declared capability rides each node's own ALIVE gossip in the `_pad`→`capability` byte of `SWIM_GOSSIP_EVT` (wire byte-identical, `SWIM_VERSION` unchanged; old nodes emit 0 → relay-fallback). Self-authoritative origination, verbatim relay, applied under the existing `(incarnation,state)` last-writer-wins gate so it converges in lock-step with membership and never regresses on a stale rumor. `region_supernode()` math untouched; the table is now fleet-real. Host cert `swim_cap_gossip_self_test()` (verb `nodes cap`; converge/staleness/falsifiable, cross-arch PASS, no swim/region regression). **DEFERRED to later N-2 slices:** supernode packet forwarding (N-2c, relay REL_DATA/REL_BROADCAST relocation), NAT hole-punch (N-3), seed bootstrap (N-4); runtime capability flip (needs incarnation bump) + true multi-process live-mesh `[live]` cert.

### Thread M — the special-structure MIND = 完全体 (special-structure-mind.md)
- **SS-3** same-tier merge cohorts · **SS-4** function-preserving expert growth (cap_experts_of(N) sizes the router) · **SS-5** deterministic expert placement · **KV cache** (in flight; SS-6 prereq) → **SS-6** cross-node firing (remote experts on wide tokens, canonical fold order) · **SS-7** bigger baby. [all touch student.c — SERIAL]

### Thread T — conversational TEACHING (education 考え方; conversational-teaching.md; needs-work)
- **T-fix** NOCENTRAL teacher selection (gossip the has-GGUF capability) + BPE↔byte tokenizer reconcile. **T-1** 2-node teach demo (A teaches B a fact via a lesson pack over the mesh, B distills it). Depends on Thread N (mesh) + the teacher engine.

### Thread B — the 🅰 big designs (each its own multi-wave)
- **interoception / 負のエネルギー**: ✅ slice-1 DONE (`9e5529a2`: S_n bus → DMN modulation + galaxy hue). **REMAINING: slice-2 apoptosis** via Path-W² essence handoff (delicate — controlled node death + essence transfer; touches dproc lifecycle + gl_merge). Follow-up: discover per-axis normalization bands from measured curves (§2.4; current maps are honest first-cut linear defaults). (interoception.md)
- **self-access / 自分の体を触る**: ✅ R0 read-only introspection DONE (`d730175e`). **REMAINING: R1** the mind invokes `body` AUTONOMOUSLY (DMN idle hook) → **BLOCKED on user decision Q1** (how free is autonomous read-only) **/ Q2** (driver consent once vs each); then drive own shell/storage/devices → self-write+compile drivers. Q3=YES already (log body-touch to lineage). (project_self_access_embodiment)
- **imaginary-UI**: conversation conjures ephemeral GUIs on demand ("電卓が欲しい"→builds one). (web-os.md)
- **compatibility / 凍結なし進化** (DECIDED 2026-06-14): generational succession + per-version migration chain + signed-OTA. (compatibility.md, project_compat_evolution)
- **federation** 254→10k staircase, R0 2-cluster live cert. (federation.md)
- **survival-network「考える器官」**: §7 distributed gating (= regions R3 / mutual-aid) · §8 two-layer oscillation fix · decentralized whole-network situational-awareness map. (survival-network.md) — ✅ §7/G38 **cert-first impl PLAN DONE** (`docs/architecture/survival-g38-impl-plan.md`): winner = minimal-diff `gacc` skeleton; auditor owns the PASS/FAIL formula; highest-risk gate = the COUPLED pressure+gacc-sum oscillation proof (a slow bias does NOT low-pass a fast oscillation in a shared `expert_utility` sum). **NEXT: a separate impl→audit wave** — touches moe.c (serializes after KV/Thread-M frees the student path; commander reads the gate formula line-by-line before crediting). §8 map + slice-2 still queued.
- **r3-nontrivial-thought**: multi-step non-trivial reasoning. (r3-nontrivial-thought.md)

### Thread R — roadmap remainders
- regions R3 width · DNODE_MAX raise · lookup L2/L3 · reflex-deliberation D3 (熟慮の中身) · p-fs P3/P4 · ring3 remainder (dtr-train/lm/dmn/gl into ring3, async 0x240/0x241, x87 FXSAVE, aarch64 EL0) · dproc_kill_by_name teardown debt.

### Thread P — product / hardware
- Play Store public release (UMP/ark Phase D) · aarch64 real hardware (RPi3) + netboot · doc physical-halve + artifact rename (the critique's shrink move; "年輪" growth accepted, so this is light).

## DOC-STATUS FIXES (stale "not started/実装前" lines to correct)
galaxy.md, ark-profile.md, signing.md, persistence.md, living-mind.md, inference-engine.md,
living-body-inspector.md — mark what actually shipped. (housekeeping wave)

## RULES (constitution)
Commander orchestrates only; implementer ≠ auditor ≠ commander; background + yield (never sit
idle on one agent); the audit is the immune system; M-tier/determinism/one-mind/NOCENTRAL
invariants are non-negotiable; one shrinking ledger.
