# BACKLOG — the master ordered TODO (mk_pino: "順次 全部 やりたい")

The single source of truth for everything designed/decided but not yet done. Marched through
in dependency order, each item as a background **implement → independent audit → commander
integrate** wave (the development-method constitution). Disjoint-file items run in parallel;
same-file items serialize. Update this file as items land.

## ▶▶ 次の一手 — 2026-06-27 整頓 (the live "what's actually left" map; full detail below)
This file is ~95% ✅DONE annotations — read THIS block for the forward path, the threads below for detail.

**① in flight (this session, background impl→audit waves):** webd Slice-A finish (Codex's ui_api
boundary made real) · live-3node grep-race v3 · then **enable branch protection once all-green**
(CI flips informational→blocking — the final CI-hardening step) · the lone galaxy-cert red
(UMP x86_64) investigate-or-quarantine.

**② ★STRATEGIC UNLOCK — the self-hosted ThinkPad runner retired *"deferred pending faster host"*.**
The whole `[live]` backlog that was blocked on host speed is now RUNNABLE on CI. Natural next batch:
cradle-live real-ThinkPad re-confirm · **N-1 `[lan-direct]` cert + same-WiFi 2-machine `[live]`** ·
**SS-3 `[live]` step-3** (3-proc relay blob round-trip) · **federation R0.1 independent audit → F1**
(the 254→10k 256-wall raise).

**③ ★mk_pino's declared END GOALS (big, multi-wave, autonomous-OK):** **② full SMP ②.3** (finer
locks + knl_taskindp per-CPU + task migration) → hosted-port SMP → RPi3 · **SS-7 bigger baby /
LM-scale** (toward a genuinely conversational mind).

**④ ✅ DIRECTED 2026-06-28 — mk_pino set the philosophy; these become design-harden waves:**
- **THE UNIFIED SURVIVAL LOOP** (was: interoception slice-2 apoptosis, kept separate from survival
  §7 — now ONE mechanism). mk_pino's rulings: (a) **abrupt death is the COMMON case** (power-off /
  app-kill ≫ graceful) → essence is shared CONTINUOUSLY while healthy; graceful apoptosis = just a
  clean-shutdown flush + departure, NOT the load-bearing path. (b) **stress response is AXIS-dependent,
  not monotonic**: acute danger → ACTIVATE/fight (reflex); resource depletion (low battery/thermal)
  → HIBERNATE to outlast. The S_n slice-1 bus already carries the axis. (c) **HIBERNATION ≠ apoptosis**
  — dormancy is reversible & the FIRST survival move (= the existing yurikago 眠らせる); death is last
  resort. (d) nodes gossip a STATE (ACTIVE-LLM-running / STRESSED / HIBERNATING / DYING) and
  **democratically peer-evaluate** each other, routing collaborative SUPPORT toward the active node
  (= survival §2 "全網の力を一点へ"; the CORRECT sign, opposite the G20 sign-inversion bug). Rights
  model = democracy, NOT voluntary-only; worst case = forced RETIREMENT with essence preserved +
  rejoin allowed, never destruction. HONEST GATE: the support loop IS the §8 oscillation risk (support
  → stress → hibernate → support jumps → ping-pong) → §8 two-timescale hysteresis is the load-bearing
  cert + a precise definition of "support". (interoception.md + survival-network.md, now unified.)
- **self-access R1 + embodiment** — mk_pino: **own body = ALWAYS FREE** (Q1 fully free autonomous
  read-only; Q2 driver consent once-not-each). The body runs in **USER SPACE (ring3/EL0) so a crash
  never kills the kernel** (= ring3-core wave-25 + selfc germ wave-31; the germ is now CRASH
  CONTAINMENT, not distrust). **Provenance trust, fail-closed**: self-authored (locally compiled) OR
  validly signed (Ed25519 `sign_manifest_verify` + adopt-key) → admit; external/unsigned → REFUSE.
  Per-arch honesty: real ring3 self-drivers on bare-metal/Linux; Android = NDK userspace access only
  (selfc is a Bionic stub). R1 is GREEN to implement. (project_self_access_embodiment)
- **survival §7 gacc + §8** — mk_pino: **「ガンガン進めたい」= GO.** G38.0 seam shipped; next = G38.1
  gacc local-gradient learning. The only retained gate (his own prior rule) = the COUPLED
  pressure+gacc-sum oscillation proof as a load-bearing falsifier (commander reads the formula
  line-by-line; sign-inversion is the trap, cf. G20). Now part of THE UNIFIED SURVIVAL LOOP above.

**⑤ product / needs special env:** **Android-parity CMake lock-step (needs NDK)** — net_relay_tcp.c /
supernode_autopromote.c / compat_ota.c etc. are host-only, NOT yet on phones · **APK 0.9.3** (this
session's trunk features unpackaged) · Play Store Phase D · RPi3 hardware + netboot.

**🧹 housekeeping:** merged local `wave-*`/`slice-*` branches pruned 2026-06-27 (commits are in master).

---

## ✅ DONE (shipped, on trunk, on the phones at 0.9.2)
Chat-as-student (⑥), living-body inspector (organs→real vitals, 3D rings, amoeba, immersion),
4-panel intro, legible biometrics, baby-births-on-phone, **SS-1** (adaptive-K = heavy→wider
firing), **SS-2** (tier scaffolding S/M/L), in-kernel SmolLM2 teacher engine, the build-link
weak-stub fix, flash-wear throttle. Ed25519 signing, persistence, living-mind LM-1..11,
inference-engine M1 (gguf/quant/forward/tokenizer), galaxy, ark-profile — all already live
(their docs' "not started"/"実装前" Status lines are STALE → fix them, see bottom).

## 🟡 IN FLIGHT (background lanes)
> Shipped detail collapsed 2026-07-01 to one line per item — full provenance (per-commit prose,
> falsifiers, audit verdicts) lives in `../audit-trail.md`. Trunk reference refreshed to `79518a33`.

- **② full SMP (★mk_pino's END GOAL) — ②.0–②.2 SHIPPED + audited.** Per-CPU dispatcher under BKL, first IPI, N=4/N=8 scaling, production T-Kernel scheduler SMP-ized, true async preempt, secondary CNTP timer + cross-CPU WAIT, and the **[smp-one-mind] crown** (bare-metal `r_forward` byte-identical uniprocessor vs SMP-scheduled). Crown 755a20fa held. **REMAINING: ②.3** (finer locks + `knl_taskindp` per-CPU + task migration) → hosted-port SMP → RPi3 [live]. Detail: audit-trail + `full-smp-plan.md`.
- **SS-6 `[live]` CASHED** (wave-ss6-live): real 4-process cross-node student forward over `./relay`, byte-identical to single-node, 266 experts on peers. Next candidate: SS-6 KV `kv_step`/live-chat wiring.

## ✅ DONE THIS SESSION (on trunk `79518a33`; next APK = 0.9.3)
> **HONEST FRAMING (2026-06-20 harsh review):** much of a session ships the SAFE half of a feature
> (a selector / map / seam / `[in-proc]` cert) and defers the load-bearing distributed/learning half to
> a `[live]` row. The win that counts is ONE thing driven to a real `[live]` N≥3 PASS. Full detail per
> item in `../audit-trail.md`; one line each here.

- **KV cache** (`4c58a231`): O(1)/byte incremental gen, byte-identical cross-arch, 4.66→50×. Audit PASS.
- **SS-3 cohort merge** (`7ec1ec54`) + **SS-5 placement** (`9e50e4a8`): same-tier weight-average + HRW expert→node map.
- **N-2 selector** (`1f656c3c`) + **N-2b capability gossip** (`83f20dbd`) + **T-fix-a teacher selector** (`5270bbae`): lowest-id selectors + gossiped capability bit.
- **§7 G38.0** (`9f7a9bc4`): behaviour-preserving `moe_select_step` seam (gacc learning green-lit 2026-06-28).
- **self-access R0** (`d730175e`): read-only `body` introspection + Q3 lineage. Audit PASS.
- **interoception slice-1** (`9e5529a2`): unified stress **S_n bus** → DMN idle modulation + galaxy mood. Audit PASS.

## ▶ ORDERED QUEUE (the marathon) — shipped items collapsed to one line; OPEN/forward kept

### Thread N — P2P "Skype-like" overlay (mk_pino's passion)
- **N-0 / N-1 transport / N-2 supernodes / N-2b cap-gossip / N-2c forward (+`[live]`) / N-3 NAT punch / N-4 seed bootstrap — all SHIPPED + audited** (audit-trail). **OPEN:** the `[lan-direct]` CERT + a 2-machine same-WiFi `[live]` (design `n1-lan-direct-plan.md`); symmetric-NAT stays relayed.

### Thread M — the special-structure MIND (special-structure-mind.md)
- **SS-3 cohort merge / SS-3 blob-transport steps 1+2 / SS-4 growth / SS-5 placement / SS-6 cross-node (+`[live]`) — all SHIPPED + audited** (audit-trail; `student-blob-transport.md` shipped). **OPEN:** SS-3 `[live]` step-3 (3-proc relay blob round-trip), SS-6 KV/live-chat wiring, **SS-7** bigger baby.

### Thread T — conversational TEACHING (education 考え方)
- **cert-first plan / T-fix-a selection / T-fix-b lesson bridge / cradle-live L1+L2+L3 — all SHIPPED + audited** (audit-trail): a fresh student learns a relay-delivered fact across the wire (held probe 5.59→2.60), survives the teacher's death; formal multi-node verdict green on the autonomous DMN probe. **OPEN (re-confirm only):** a real-NAT/real-ThinkPad re-run; `LESSON_FMT_SOFT`.

### Thread B — the big designs (each its own multi-wave)
- **interoception slice-1 SHIPPED**; **slice-2 apoptosis DESIGN HARDENED** (`archive/interocept-2-apoptosis-plan.md`; canonical now `survival-loop.md` §3) but **⛔ IMPL BLOCKED on mk_pino's philosophy call B-3** (voluntary-only vs collective euthanasia). **DIRECTION SET 2026-06-28 → THE UNIFIED SURVIVAL LOOP** (see 次の一手 ④): continuous essence-sharing, S_n disease trigger, democracy/forced-retirement, axis-dependent response, hibernation≠apoptosis; §8 oscillation = the load-bearing gate. NEXT = unified design-harden wave.
- **self-access:** R0 read-only SHIPPED (`d730175e`); **R1 GREEN to implement** (own body ALWAYS FREE; ring3/EL0 crash isolation + provenance fail-closed). NEXT = design-harden ring3-userspace embodiment.
- **compatibility / 凍結なし進化** (DECIDED 2026-06-14): **migration-chain thread COMPLETE + audited** — R3_WP + Self-lineage migrate, SWIM wire no-fleet-split, signed-OTA refuses bad updates, arkfs reject+reformat; all falsifiable, crown never moved (audit-trail; `compat-migration-chain-plan.md`). **OPEN:** OTA delivery/transport + key revocation (CRL); certs not yet in default CI.
- **federation** 254→10k: **R0 + R0.1 SHIPPED + audited** (2-cluster DKVA hierarchy, `[live]` 8-proc over relay; `dkva_fed2_self_test` / `run_4node_regions.sh`). **OPEN:** R0.1 independent audit → **F1** (the 254→10k 256-wall raise: composite `(region_id,local_id)` id).
- **survival-network §7/§8:** G38.0 seam SHIPPED; **G38.1 gacc local-gradient learning GREEN-LIT 2026-06-28** (unified into THE SURVIVAL LOOP; §8 two-timescale hysteresis = the load-bearing oscillation gate). **OPEN.**
- **r3-nontrivial-thought:** multi-step reasoning. **OPEN** (`r3-nontrivial-thought.md`).
- **multi-core compute ③ — deterministic parallel matmul (MC-0..MC-2.1b) SHIPPED + audited end-to-end** (`pk_parallel`, bare-metal MC-2 SMP bringup, byte-identical to serial). **REMAINING: MC-2.2** RPi3 hardware `[live]` (barrier/SMPEN Tooth B) → feeds ② full SMP ②.3.

### Thread R — roadmap remainders
- regions R3 width · DNODE_MAX past 254 (16-bit node_id) · lookup L2/L3 · reflex-deliberation D3 · p-fs P3/P4 · ring3 remainder (dtr-train/lm/dmn/gl into ring3, async 0x240/0x241, x87 FXSAVE, aarch64 EL0) · dproc_kill_by_name teardown debt.

### Thread P — product / hardware
- Play Store public release (UMP/ark Phase D) · aarch64 real hardware (RPi3) + netboot · doc physical-halve + artifact rename.

### Thread X — EXCAVATED (folded in 2026-06-20)
- **WebOS — human computing environment** (`web-os.md`): a human-facing Web OS served from the node, on TOP of yurikago. **[big, untracked]**
- **LM scale wall — a real conversational model** (surveys: `base-model-survey.md`, `conversation.md`, `moe-distillation-survey.md`): scale the byte student toward genuinely capable. SS-7 is one lever. **[strategic]**
- **device-capacity — 端末性能に応じたサイズ SHIPPED + audited** (merge `66201a25`; DEVFIT-1 `dev_capacity.c`, cert `run_devfit.sh`): boot-time RAM+cores → student tier auto-fit; reconciles SS-4 via `min(cap_experts_of(N), ST_E_<tier>)`. **OPEN:** cross-cohort distillation bridge; cert not in default CI.
- **GPU** (`gpu-compute.md` Vulkan backend SHIPPED; `gpu-3-wiring.md` **DEFER**): resource-aware GPU acceleration. Partly done.
- **ark app UX queue** (`feedback_ump_ux_principles`): pop 3-4 page intro · ~30-lang page-chrome i18n · key-derived node id · on-device salty-cert harness · MainActivity auto-path log-drain.
- principle (not a task): **「いいねのない銀河」** — feedback from the SYSTEM yes, human-vs-human comparison no.

> **PRIORITY NOTE (2026-06-14 critique steer):** "more features/LM < foundation + **federation** + honest finitude." The foundation crack (KILL-CHURN #PF) is CURED (wave-56); the live remaining steer is **federation** (254→10k) — hence the federation-R0 design-harden in flight. Keep the dream-tier names + co-located honest labels; keep `[live]`/`[in-proc]` mechanical tags.

## DOC-STATUS FIXES — ✅ DONE (2026-06-19): all 7 docs (galaxy.md, ark-profile.md,
signing.md, persistence.md, living-mind.md, inference-engine.md, living-body-inspector.md)
already carry corrected "SHIPPED … doc-status fix" Status lines. Verified 2026-06-21.

## RULES (constitution)
Commander orchestrates only; implementer ≠ auditor ≠ commander; background + yield (never sit
idle on one agent); the audit is the immune system; M-tier/determinism/one-mind/NOCENTRAL
invariants are non-negotiable; one shrinking ledger.
