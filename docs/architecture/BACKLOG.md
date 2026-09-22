## ▶▶ 2026-09-05 resync — what actually landed since the 6/27 整頓 (unattended routine, mechanical audit, not a design call)

> **UPDATE 2026-09-06 (unattended routine):** the DMOE-A / Windows-PE-port doc gap and the
> `docs/audit-trail.md` gap named below are both closed. Per mk_pino's 2026-09-05 03:05 instruction
> ("無人 run に書かせる"), a run that day wrote [[distributed_moe_design.md]] and
> [[windows-pe-port.md]] (commits `6d6602ed`/`6bcc5e81`, INDEX/V-MODEL rows added). This run
> (2026-09-06) backfilled the missing `docs/audit-trail.md` entries for both (appended at file end,
> commit `10d47403`) — both entries are explicit that they transcribe the commit + `ci.yml` comments,
> not an independent re-audit. The DLB doc-thinness item below is **also closed now**, same run:
> [[depth_iq_path_design.md]] §3.4a backfills the Wave-D1/D2 production-wiring and budget/gc
> commits (`22a43de1`/`412ab044`) that the original doc predates. Full method/caveats for the
> DMOE-A/Windows-port write-ups are in each doc's own
> "how this doc was written" note; see also `vendor-patch-inventory.md` for this same run's
> unrelated layer-B work.

**This block is additive.** It does NOT rewrite the ordered queue below (that needs a design-level
pass this routine is not authorized to make unattended); it records what a `git log --since=2026-06-27`
sweep (192 commits, 40 feat/fix) found that the sections below do not yet reflect, so the next reader
does not have to re-derive it.

**Landed, already reflected in [[V-MODEL.md]] / [[INDEX.md]] as `working` tier (2026-07-01..07-11
doc passes caught these):** LM-12 belief revision, LM-13 forgetting, LM-14 curiosity, LM-15
pull-teach, 良心 conscience floor, migration-succession (generational arch-gap crossing),
scale-wall C1 (context carry), frontier-mouth CONSULT/TEACH, society-of-minds scaling ensemble,
unbounded-N U-0. All have `docs/architecture/` design docs and INDEX rows; do not re-add them.

**Landed, NOT reflected anywhere (no INDEX row, no V-MODEL rung, no design doc found repo-wide) —
CORRECTION 2026-09-05 same run: initially mis-framed as "unaudited"; that was wrong. Both ARE
CI-gated with real anti-theater falsifiers and BOTH are green on the latest run (`33812013193`).
What's actually missing is the design doc / audit-trail narrative, not the verification:**
- **DMOE-A** (`8f754c3b`, 2026-07-05) — distributed-MoE expert bank; SS-5/SS-6's successor, makes
  fleet capacity actually GROW with N instead of just spreading FLOPs. Code: `arch/common/llm/dmoe_bank.{c,h}`,
  cert `tests/llm/run_dmoe.sh`, CI job `distributed-MoE capacity ([dmoe-*] bank; hosted-only, crown-neutral)`
  — **has an explicit anti-theater falsifier** (`ANTI-THEATER: stub the remote transport -> RED`),
  currently green. **No design doc exists under `docs/architecture/` for this at all** — checked by
  filename grep (`dmoe`) across the whole repo, zero doc hits outside code/tests/ci.yml comments.
- **native Windows (mingw-w64 PE) port P1** (`ff163ac1`, 2026-07-05) — boots to console shell,
  Windows Fibers dispatcher, cooperative (no preempt v1). CI job `Windows PE ビルドゲート (mingw-w64,
  boot/windows/x86_64)` asserts the output is a real PE32+ binary, currently green. Same doc gap:
  no file under `docs/architecture/`.
- **DLB — test-time deliberation** (`3ecb0413`, `22a43de1`, `412ab044`) — HAS a design doc
  (`depth_iq_path_design.md`, INDEX row exists, `working` tier) but the production-hardening
  follow-ups (compounding-loop close, per-trace distill budget+gc) landed after the doc/INDEX
  pass and are not mentioned in its one-liner. Low priority — the doc row is not wrong, just thin.

**Separate finding: `docs/audit-trail.md` itself has an unexplained gap.**
Its last dated entry before 2026-09-05 was the unbounded-N U-0 re-bless (**2026-07-05**); the next
dated entry is the IRQ-stub SS-reload work (**2026-08-12**) — five and a half weeks with zero
audit-trail commits, verified via `git log -- docs/audit-trail.md` (commit `835a09cb` 07-05 →
`1fdc1e3b` 08-12). DMOE-A, the Windows port, and the DLB hardening commits above all fall inside
that window. **Narrowed 2026-09-05, same run:** DMOE-A and the Windows port both have real,
green, anti-theater-falsified CI gates (see above) — so the *verification* almost certainly
happened; what's missing is specifically the **written record** (design doc + audit-trail prose),
not evidence that the implement→audit→commander cycle was skipped. That's a real gap (this project's
own rule is "the audit is the immune system," and an unwritten audit is hard to trust or reuse
later) but a smaller, more mechanical one than first framed: **it needs someone to write the design
doc and backfill audit-trail.md from the existing commits/CI logs, not to re-run verification from
scratch.** Left in 判断待ち because writing retroactive design docs is closer to a design/authoring
call than a doc-sync fact-check, and this routine did not want to guess at design intent unattended.

---

# BACKLOG — the master ordered TODO (mk_pino: "順次 全部 やりたい")

The single source of truth for everything designed/decided but not yet done. Marched through
in dependency order, each item as a background **implement → independent audit → commander
integrate** wave (the development-method constitution). Disjoint-file items run in parallel;
same-file items serialize. Update this file as items land.

## ▶▶▶ 2026-09-19 — mk_pino: 「調べて書く」から「作る」へ (the current order; supersedes the 06-27 block below)

**Why.** The last `feat` commit is 2026-07-12. The ~64 commits the unattended routine made
since 2026-09-03 are all docs / tests / CI — zero kernel-code change — while the gap-ledger OPEN table stayed
at 3 rows (one merged away, one newly found) and the audit docs grew by ~135k chars. That is
AUDIT-SPRAWL returning, produced by following the rules (no push, no unilateral design calls leaves
"investigate and write" as the only safe work). So the order below is BUILD work, and the routine now
works one item per `feat/<name>` branch, test-first, with implementation and audit in different runs.

**Where the concept stands (five layers).** Body — largely built. Self — built (hash-chained,
continued by another node). Collective — built at toy scale (`[live]`: a fact taught on A is answered
by B after A is killed). Evolution — first slice shipped (`migration-succession.md`, 2026-07-05).
**Brain — the gap.** The R3 mind is 21,568 params with single-token recall; the Cradle-baby student
(1.9M, M tier) got a clean NULL on C1 context carry (`scale-wall-c1.md`); the in-kernel SmolLM2-135M
teacher engine works but is not wired to teach (CT-2). Also: the student and the mind's entry points
exist only on the hosted-Linux / Android builds, not on bare metal. The mechanisms that keep a mind
alive are proven; what they keep alive is still a toy.

| order | item | design / starting point |
|---|---|---|
| D1 | RNG0 regression test — **DONE, merged to local master 2026-09-20** (`feat/rng0-regression-test`, `f8b0abd2`; RED on master, GREEN on a matched control arm; behind a flag, crown-neutral). **2026-09-23: now wired into CI** as a non-blocking monitor (`feat/rng0-ci-wire`, independently re-verified both arms in an isolated container before merge — positive RED/control GREEN, matching the script's own expectations). The underlying hang itself is still unfixed — that needs a human to pick between gap-ledger's (a)/(b)/(c)/(d) options; this only watches that the known-bug baseline hasn't silently changed | `gap-ledger.md` RNG0 row |
| **A1** | **CT-2 — the SmolLM2 teacher generates the child's lessons** (compare against the fixture-only child under the same budget) | `30-module/conversational-teaching.md` §CT-2; `forward.c` `lm_generate` |
| **A2** | **scale-wall C2 — data reservoir 4 KB → ≥10 MB** (does C1's A(d) move off zero?) | `scale-wall-c1.md` (note: the `scale_wall_design.md` it cites was never committed — write a short design note first) |
| B3 | self-access R1 (first T1 affordance `self_access_publish` shipped+audited 2026-09-21, `feat/self-access-r1-publish`; crown grows +1560B x86/+1440B aarch64 bare-metal — re-bless is judgment-pending. **Audit independently re-verified and merged to local master 2026-09-21** — see `docs/audit-trail.md`. **2026-09-23: `[self-access-r1-test]` wired into CI** — never had a CI assertion until now, run as its own isolated `net`+`body test` invocation) | `30-module/self-access-design.md` R1 |
| B1 | the unified survival loop — **correction 2026-09-21: L0 (STATE bus) and L1 (STATE-aware support routing + §8 hysteresis) are ALREADY SHIPPED + CI-gated** (`world.c`'s `world_self_state_step`/`moe.c`'s `moe_state_fold`, `survival l0\|l1` shell verbs, `tests/host/run_survival_l{0,1}.sh`, `ci.yml`'s `[survival-l0/l1] ALL PASS` jobs; both crown-preserving per §9/§10, commander-decided 2026-06-28). **L2 (HIBERNATING) STATE-FSM half implemented 2026-09-21, independently audited (clean-worktree crown re-derivation, full L0/L1/L2 cert re-run incl. falsifier) and merged to local master 2026-09-21** — crown byte-identical to merge-base, hosted-only. **2026-09-23: all four of L2's deferred sub-items now shipped+independently-audited+merged to local master** — mind_net_task/mind_merge_task pause (`feat/survival-l2-mind-pause`, incl. a self-found+fixed +8B crown drift), beacon-cadence reduction (`feat/survival-l2-beacon-cadence`, `world_beacon_interval_ms()`), routed-work shedding (`feat/survival-l2-routed-shed`, `moe.c`'s `eff_state_penalty`/`moe_hibernate_route_test`, `[hibernate-shed]` cert — its first falsifier attempt was toothless, self-caught+fixed, independently re-verified RED), and heavy DMN consolidation pause (`feat/survival-l2-dmn-pause`, `dmn.c`'s `dmn_paused_for_hibernation()` gates `lm_consolidate_idle_round`/`r3_consolidate_idle_round`/`student_dmn_consolidate`, leaves `ga_step()` unpaused as genuinely tiny). All four needed only `_TK_HOSTED_LIBC_` guards, no re-baseline, crown byte-identical each time (independently rebuilt+sha256-verified). **L2 is now feature-complete AND CI-gated** (`run_survival_l2.sh`/`run_mind_pause.sh`/`run_dmn_pause.sh` were merged but never wired into `ci.yml` until 2026-09-23 — closing that gap the same run, same pattern as L0/L1's existing jobs). Stop before L4 (democratic retirement) | `20-architecture/survival-loop.md` §6-L2, §9, §10 |
| C1 | federation F1 (254 → 10k) — **correction 2026-09-21: R0.1 was ALREADY independently audited and CLOSED** (`docs/audit-trail.md` line ~803, auditor "did NOT write the code", verdict MERGEABLE, 2026-06-21 — this file's own line ~198 already said so; only the ordered-queue row below was stale). Composite `(region_id,local_id)` id + a coordinator-only upper SWIM/kdds mesh (`federation.md` §2.2/§4-F1) is a multi-file, invasive change; §5.3 itself flags 2 open design forks inside it (wire encoding, rsum ordering). **First slice cut 2026-09-21** (`feat/federation-f1-composite-id`, `federation.md` §7): the encoding ONLY, as a standalone header-only primitive (`fed_id.h`, `[fed-id-roundtrip]` cert incl. falsifier), answering fork (1) (the wire encoding) but not wired into any live GOBJ path — deliberately, since wiring it in would touch fork (2) (coordinator-cross rsum ordering, eventual vs raft-per-region), which is still unresolved. **Independently audited PASS and merged to local master 2026-09-22** (`2cea9e4f` — this row was stale, still said "audit pending"). Next slice: resolving fork (2) is a human design call (judgment-pending). **Checked 2026-09-23 whether fork (1)'s encoding could be wired into a live GOBJ path without prejudging fork (2)**: the 5 `GOBJ_MAKE` call sites (`drpc.c:303,677,687,693,785`) all pass plain T-Kernel object ids with no coordinator-mesh consumer yet, so wiring `FED_GOBJ_MAKE(node, 0, id)` in today would be a behaviorally-identical no-op (region_id hardcoded to 0) — pure ceremony, not a real step, until the coordinator layer that would actually read a nonzero region_id exists. Not worth doing yet; genuinely blocked on fork (2) plus the coordinator mesh itself | `20-architecture/federation.md` §4-F1, §5.3, §7 |
| D2 | Android emulator smoke: no HTTP answer on 7800-7862 within 60 s (red for months) | CI job log |
| D4 | reconcile this file with reality — **the `r3-nontrivial-thought` example this row used to cite was itself stale: line ~200 already says "reconciled 2026-07-01", so that example is fixed.** Found a real one 2026-09-21: B1's row said "L0 → L3" while L0/L1 are shipped+CI-gated (fixed, see B1 above). **Second pass, same run: 4 more stale spots found+fixed** — the "② STRATEGIC UNLOCK" and federation Thread-B bullets both still said "R0.1 independent audit → F1" after R0.1 was already closed 2026-06-21; the survival-loop Thread-B bullet still said "NEXT = design-harden wave" after L0/L1/L2 shipped; the self-access Thread-B bullet still said "not merged to master" after this run's audit merged it. All 4 were downstream of the SAME source-of-truth rows (B1/B3/C1 above) drifting out of sync with the prose below them — worth checking whether that's a recurring pattern (rows updated, prose below not) next time this file is touched | — |

Needs the maintainer (not the routine): the same-Wi-Fi two-machine `[live]`, RPi3 hardware, APK
releases / Play Store, crown re-bless for any bare-metal change, and the L4 retirement philosophy.

## ▶▶ 次の一手 — 2026-06-27 整頓 (the live "what's actually left" map; full detail below)
This file is ~95% ✅DONE annotations — read THIS block for the forward path, the threads below for detail.

**① in flight (this session, background impl→audit waves):** webd Slice-A finish (Codex's ui_api
boundary made real) · live-3node grep-race v3 · then **enable branch protection once all-green**
(CI flips informational→blocking — the final CI-hardening step) · the lone galaxy-cert red
(UMP x86_64) investigate-or-quarantine.

**② ★STRATEGIC UNLOCK — the self-hosted ThinkPad runner retired *"deferred pending faster host"*.**
The whole `[live]` backlog that was blocked on host speed is now RUNNABLE on CI. Natural next batch:
cradle-live real-ThinkPad re-confirm · **N-1 `[lan-direct]` cert + same-WiFi 2-machine `[live]`** ·
**SS-3 `[live]` step-3** (3-proc relay blob round-trip) · **federation F1**
(the 254→10k 256-wall raise; R0.1's audit already closed 2026-06-21, see C1 row above — stale
"R0.1 independent audit → F1" phrasing here fixed 2026-09-21).

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
- **interoception slice-1 SHIPPED**; **slice-2 apoptosis DESIGN HARDENED** (`archive/interocept-2-apoptosis-plan.md`; canonical now `survival-loop.md` §3) but **⛔ IMPL BLOCKED on mk_pino's philosophy call B-3** (voluntary-only vs collective euthanasia). **DIRECTION SET 2026-06-28 → THE UNIFIED SURVIVAL LOOP** (see 次の一手 ④): continuous essence-sharing, S_n disease trigger, democracy/forced-retirement, axis-dependent response, hibernation≠apoptosis; §8 oscillation = the load-bearing gate. **STATUS 2026-09-21 (was stale "NEXT = design-harden wave"): L0/L1 SHIPPED+CI-gated, L2 STATE-FSM half merged to local master — see B1 row above** for current detail; NEXT = L2's deferred pause-half or L3.
- **self-access:** R0 read-only SHIPPED (`d730175e`); **R1 first T1 affordance (publish on own topic) shipped+audited 2026-09-21** on `feat/self-access-r1-publish` (343e0d58) — `[self-act-guarded]` matched-arm gate ALL PASS after `net` init (drpc identity); crown `.text` grows +1560B x86 bare-metal / +1440B aarch64 bare-metal (expected, self_access.c/reflex.c are crown-linked). **Independently re-audited and merged to local master 2026-09-21** — crown growth stands, **re-bless (docs/audit-trail.md + ci.yml crown constants) is still a human decision, not done**, push not done. Remaining R1 scope (p-fs write / `mind teach`) deferred: no existing germ-capability table for parent-side code (real design fork, see commit body + judgment-pending). NEXT after re-bless = R2 device-detect loop.
- **compatibility / 凍結なし進化** (DECIDED 2026-06-14): **migration-chain thread COMPLETE + audited** — R3_WP + Self-lineage migrate, SWIM wire no-fleet-split, signed-OTA refuses bad updates, arkfs reject+reformat; all falsifiable, crown never moved (audit-trail; `compat-migration-chain-plan.md`). **OPEN:** OTA delivery/transport + key revocation (CRL); certs not yet in default CI.
- **federation** 254→10k: **R0 + R0.1 SHIPPED + audited** (2-cluster DKVA hierarchy, `[live]` 8-proc over relay; `dkva_fed2_self_test` / `run_4node_regions.sh`; R0.1's independent audit closed 2026-06-21, `audit-trail.md` line ~803 — stale "OPEN: R0.1 audit" phrasing here fixed 2026-09-21). **OPEN → F1** (the 254→10k 256-wall raise: composite `(region_id,local_id)` id + coordinator-only upper mesh, `federation.md` §4-F1). **First slice cut 2026-09-21**: the encoding alone (`feat/federation-f1-composite-id`, `federation.md` §7), self-tested, crown-neutral, independent audit pending.
- **survival-network §7/§8:** G38.0 seam SHIPPED; **G38.1 gacc local-gradient learning GREEN-LIT 2026-06-28** (unified into THE SURVIVAL LOOP; §8 two-timescale hysteresis = the load-bearing oscillation gate). **OPEN.**
- **r3-nontrivial-thought:** multi-step reasoning. **SHIPPED + CI-gated (Closed wave-19,
  reconciled 2026-07-01)** — a *capacity* certificate for the substrate, not a claim that
  every reasoning task is solved (`20-architecture/r3-nontrivial-thought.md`).
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
