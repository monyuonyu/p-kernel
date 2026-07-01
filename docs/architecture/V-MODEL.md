# V-MODEL — the design↔verification spine of p-kernel

> **全体構造はこの一枚。全doc索引は [INDEX.md](INDEX.md)。読み筋（概念地図）は
> [README.md](README.md)。OPEN gap の正本は [gap-ledger.md](gap-ledger.md)。**

The V-model has two arms joined rung by rung:

- **left arm = design**, descending: なぜ (L0) → 要件 (L1) → アーキテクチャ (L2) → 詳細設計 (L3).
- **right arm = verification**, ascending: モジュール試験 → 統合 → システム → 受入.

The load-bearing idea of this file: **the horizontal rungs are not a foreign
template — they are exactly the ethos this project already runs on.** p-kernel's
rule has always been *"every design claim has a falsifiable cert"* — the
[gap-ledger](gap-ledger.md), the `[live]` / `[in-proc]` verification tiers, and CI
enforcement. So the V just makes explicit what was already true:

> **A design doc with a corresponding cert = a closed rung. A design doc with NO
> cert = a broken rung = a gap.** The correspondence table below is that check,
> made visible for every doc.

```
 LEFT ARM (design)                                          RIGHT ARM (verify)
 =================                                          ==================

 L0  00-concept/     なぜ / CONOPS  ───────────────────►  受入: the whole right
      survival-network.md  (思想の核・逐語)                  arm IS its acceptance

 L1   10-requirements/  システム要件 ──────────────────►  システム試験
       compatibility / compat-migration-chain            [migrate-forward], signed-OTA

 L2    20-architecture/  アーキテクチャ ────────────────►  統合試験  40-verify/
        regions reflex-* p-fs lookup federation          composite-scenarios.md
        survival-loop inference-engine …                 composite-loop, *-live jobs

 L3     30-module/  詳細設計 ─────────────────────────►  モジュール試験
         living-mind galaxy persistence genome           [dmn-*] [galaxy-*] [persist-*]
         ark-profile interoception relay-ha …            [ark-*] [survival-l0] …
          \                                             /
           \___________  code + CI  __________________/
                   (arch/common, .github/workflows/ci.yml)

 ── OUTSIDE THE CLASSIC V ────────────────────────────────────────────────────
 50-evolution/  ring3-core selfc-ring3 self-compile   →  ring3-survival, [selfc-*]
   the V that RUNS ON ITSELF (runtime self-modification; see §OUTSIDE THE V)
```

## Level definitions

| level (dir) | V-arm role | one line |
|---|---|---|
| `00-concept/` | **L0** left, top | なぜ作るか / mission-of-the-mission (CONOPS). The 逐語 thought core. |
| `10-requirements/` | **L1** left | system requirements = strategy & invariants written as requirements. |
| `20-architecture/` | **L2** left | high-level structure: what the organs are and how they compose. |
| `30-module/` | **L3** left, bottom | per-subsystem mechanism (詳細設計). |
| `40-verify/` | **right arm** | integration / system / acceptance *scenario* docs — the horizontal partner of L2/L3. |
| `50-evolution/` | **outside V** | runtime self-modification: the system re-runs its own V while alive. |
| _(root)_ | **meta** | README / INDEX / **this file** / gap-ledger / BACKLOG / review — the spine & ledgers, not rungs. |
| `archive/` | **spiral** | earlier turns of the V (年輪) — frozen, not re-filed. |

Cross-level notes (docs that span rungs, filed at their PRIMARY level):

- `survival-loop.md` (L2) is also the closest thing to a system-integration doc; its
  right-arm partner lives partly in `40-verify/composite-scenarios.md`.
- `survival-g38-impl-plan.md` (L3) is the impl bridge from `reflex-action.md` (L2)
  down to the `[g38-*]` certs; `reflex-action.md` itself is filed at L2 as the
  行動 axis of the reflex/deliberation architecture.
- `student-blob-transport.md` (L3) is the transport mechanism under
  `special-structure-mind.md` (L3, SS-3).
- `r3-model-widening.md` (L2) is a *rationale* 年輪 answered by `living-mind.md` (L3)
  capacity work — a design argument, not a mechanism with its own cert.

---

## The design↔verification correspondence table (the rungs)

Each design doc → its cert (CI job / self-test tag / live job). Rung status:

- **✓ CLOSED** — a dedicated cert exercises the design claim (tier per
  [gap-ledger.md](gap-ledger.md): `[live]` ≥2 processes+kill, `[in-proc]` real
  code in one process).
- **◐ PARTIAL** — an early slice is certed; later slices are design-only (the gap
  to the strong form is the open rung).
- **○ OPEN** — a design doc with **no corresponding cert yet** (a broken rung).
- **N/A** — a CONOPS / survey / rationale doc where a single cert is not the right
  instrument (the whole right arm, or a future mechanism, verifies it).

> Certs harvested from `.github/workflows/ci.yml` tags/jobs, the gap-ledger Closed
> epitaphs, and the `docs/architecture/*` back-references in the code. This is the
> map, not a re-run; the canonical PASS/FAIL is CI + gap-ledger.

### L1 — 10-requirements/

| doc | design claim | cert | rung |
|---|---|---|---|
| compatibility.md | old & new nodes don't split (generational succession) | `[migrate-forward]` (compat migration-chain) | ✓ CLOSED |
| compat-migration-chain-plan.md | per-version migration chain + signed-OTA gate | `[migrate-forward]` + `[sign-*]` | ✓ CLOSED |

### L2 — 20-architecture/

| doc | design claim | cert | rung |
|---|---|---|---|
| regions.md | latency-clustered regions, locality-MoE, capacity(N) | `[capacity-score]`, `[g13-arrival]`, `[g23-ceiling]`; live `parallel-infer-live` | ✓ CLOSED |
| reflex-deliberation.md | two-layer time-constant split (no oscillation) | `[moe-twolayer]` `[moe-osc]`; live `twolayer-couple-live` | ✓ CLOSED |
| reflex-action.md | inference class → real local defense (G38 coupling) | `[g38-confidence-live]` `[g38-learning-improves-guarding]` `[g38-guard-feeds-learning]` `[g33-controlled]` | ✓ CLOSED |
| p-fs.md | content-addressed, gossip-replicated, DAG FS | `[pfs]` `[pfs-durswallow]` `[pfs-dagrefs]` `[hrw]` | ✓ CLOSED |
| decentralized-lookup.md | no-central location lookup (HRW + WANT gossip) | `[hrw]` `[hrw-l1]` (L0/L1) | ◐ PARTIAL — L2/L3 (full gossip/world-table) design-only |
| inference-engine.md | own GGUF loader + quantized matmul | `shipped-llm-certs` `[result]` (gguf/qmatmul/tokenizer) | ✓ CLOSED |
| memory-thought.md | forward pass reads p-fs (memory→thought wiring) | — (covered indirectly by `[persist-mind]`; no dedicated cert) | ○ OPEN |
| survival-loop.md | interocept+gate+worldmap+hibernate+death as one loop | `[survival-l0]` `[survival-l1]`; live `survival-loop` | ✓ CLOSED (L0/L1; L2–L4 design) |
| federation.md | hierarchical bridge past the 32-node wall | `[g23-ceiling]` (ceiling→64); discovery-mesh live | ◐ PARTIAL — F1–F3 design-only |
| connect-anywhere.md | heartbeat + UDP↔TCP fallback connectivity | `connect-anywhere-certs`: heartbeat, autofallback | ✓ CLOSED |
| p2p-overlay.md | central-less mesh + supernode forwarding | `[mesh-discovery]` (live), autopromote | ◐ PARTIAL — NAT traversal design-only |
| closed-loop.md | join green parts into one negative-feedback loop | `composite-loop` (`22_composite`), `20_closed_loop` | ✓ CLOSED |
| r3-nontrivial-thought.md | in-context real learning (not a toy) | `[r3-incontext-gradcheck]` `[r3-incontext-learned]` `[r3-incontext-handif]` `[r3-incontext-frozen]` | ✓ CLOSED |
| r3-model-widening.md | widen the net so distribution becomes necessary | — (年輪 rationale; realized via `[lang-capacity-v2]` in living-mind) | N/A rationale |
| base-model-survey.md | which OSS base to borrow (branch A2 study) | — (survey; no mechanism to cert) | N/A survey |
| moe-distillation-survey.md | borrow vs distill own MoE (study) | — (survey) | N/A survey |
| native-student.md | a brain sized to the vessel, grown from a baby | `shipped-llm-certs` (student); NS-1 `student_test.c` | ✓ CLOSED |
| full-smp-plan.md | SMP the kernel without splitting the mind (②.0–②.2) | `smp-autodetect` (`[smp-autodetect]`, run_smp0.sh) | ✓ CLOSED (②.0–②.2; ②.3 roadmap) |

### L3 — 30-module/

| doc | design claim | cert | rung |
|---|---|---|---|
| living-mind.md | continuously-taught ownerless mind (LM-1..11) | `[dmn-*]` `[handoff-*]` `[stream-*]` `[lang-*]` `[salience-*]` `[self-*]` `[onemind-*]` `[wmerge-*]` | ✓ CLOSED |
| ark-profile.md | human-chapter autobiography + i18n | `[ark-consent]` `[ark-profile]` `[ark-provenance]` `[i18n-manifesto]` | ✓ CLOSED |
| galaxy.md | per-node observation window | `[galaxy-serve]` `[galaxy-events]` `[galaxy-teach]` | ✓ CLOSED |
| persistence.md | the ark that does not forget (durable layer) | `[persist-identity]` `[persist-mind]` `[persist-mind-stale]` | ✓ CLOSED |
| genome.md | empty node germinates a full cell from gossip | `14_genome` (CI kill test) | ✓ CLOSED |
| survival-fs.md | ARK FS: content-addressed durable backend | `ark-crash-fuzzer`; `23_durable`; `arkfs-audit.md` | ✓ CLOSED |
| relay-ha.md | multiple relays, central-less failover | `relay-tests` (6 scenarios), `[drpc-oob]` | ✓ CLOSED |
| supernode-autopromote.md | measured-fitness supernode promotion (N-2d) | `connect-anywhere-certs`: autopromote | ✓ CLOSED |
| conversational-teaching.md | a live teacher node educates the child over the wire | `[teach-arrival]` `[teach-live]` `[teach-consolidated]` | ✓ CLOSED (live) |
| death-piercing.md | swarm answers even if inference dies mid-flight | live `survival-loop` (`13_survival_loop`) | ✓ CLOSED (live) |
| fault-recovery.md | task isolation + respawn from p-fs weights | live `survival-loop`; KILL-CHURN cured | ✓ CLOSED (live) |
| r3b-breathing-params.md | breathing params = expert specialization | `18_breathing` | ✓ CLOSED |
| special-structure-mind.md | fleet-scale sparse cross-node unified mind | `[expert-growth-preserves]` (SS-4); SS-1..6 live | ◐ PARTIAL — SS-7 design-only |
| student-blob-transport.md | variable-length student blob transport (SS-3) | `run_ss3_blob.sh` (`student_blob_test.c`) | ✓ CLOSED (in-proc) |
| survival-g38-impl-plan.md | §7/G38 distributed-gating wiring plan | `[g38-*]` (realized) | ✓ CLOSED |
| interoception.md | node "pain" onto one `S_n` bus | `[survival-l0]` STATE bus (Slice-1) | ◐ PARTIAL — Slice-2 apoptosis design-only |
| device-capacity.md | modulate load by device capability | `[device-fit]` (DEVFIT-1 mind-sizing) + falsifier | ◐ PARTIAL — tier/continuous modulation design-only |
| dynamic-id.md | churn-tolerant node-id | `[swim-incarn]` (relay lease) | ◐ PARTIAL — full P2P id design-only |
| self-access.md | a node touches its own body (read-only slice) | — (first slice; no dedicated CI cert) | ○ OPEN |
| gpu-compute.md | run the mind's math on device GPU (Vulkan) | — (matmul backend shipped; mind-integration uncerted) | ○ OPEN |
| gpu-3-wiring.md | wire Vulkan matmul into the mind | — (audit verdict: DEFER implementation) | ○ OPEN (deferred) |
| webd-user-space.md | move the web server out of the substrate | — (Slice A partial; no cert) | ○ OPEN |
| living-body-inspector.md | wire star organs to REAL vitals (HONEST-GLOW) | — (via galaxy certs; no dedicated inspector cert) | ○ OPEN |
| n1-lan-direct-plan.md | relay-less auto-mesh on same WiFi | — (design plan; LAN-direct transport uncerted) | ○ OPEN |
| conversation.md | escape the vocabulary cage (strategy branch) | — (strategy exploration) | N/A exploration |

### 40-verify/ (right arm)

| doc | role | cert | rung |
|---|---|---|---|
| composite-scenarios.md | integration scenarios closing the survival loop | `composite-loop` (`22_composite`), audit G19 | ✓ CLOSED |

### 50-evolution/ (outside the classic V)

| doc | design claim | cert | rung |
|---|---|---|---|
| ring3-core.md | move the self-modifying core to ring3/EL0 | `ring3-survival`: `[iso-userptr]` `[ring3-survival]` `[ring3-mind]` `[dproc-teardown]` `[fpu-ctx]` | ◐ PARTIAL — inference path certed; training modules design |
| selfc-ring3.md | self-built unit inside the immune boundary | `[selfc-isolated]` `[selfc-rollback]` `[selfc-lineage]` | ✓ CLOSED |
| self-compile.md | self-compile (selfc) first milestone | `[selfc-isolated]` `[selfc-rollback]` `[selfc-lineage]` | ✓ CLOSED |

### L0 — 00-concept/

`survival-network.md` is the CONOPS (なぜ). It has no single cert; its acceptance
test is **the entire right arm** — every `-live` kill test and every closed rung
above is a piece of its verification. This is the top-of-V ⇄ acceptance rung.

---

## Rung tally (the finding)

Of the **50** hot docs (excluding the L0 CONOPS which the whole right arm verifies):

| rung | count | which |
|---|---|---|
| **✓ CLOSED** (design↔cert complete) | **30** | most of L1/L2/L3 + verify + 2 evolution |
| **◐ PARTIAL** (early slice certed, later slices design-only) | **8** | decentralized-lookup, federation, p2p-overlay, special-structure-mind, interoception, device-capacity, dynamic-id, ring3-core |
| **○ OPEN** (design doc, no cert = broken rung) | **7** | memory-thought, self-access, gpu-compute, gpu-3-wiring, webd-user-space, living-body-inspector, n1-lan-direct-plan |
| **N/A** (survey / rationale / CONOPS) | **5** | survival-network (concept), r3-model-widening (rationale), base-model-survey, moe-distillation-survey, conversation |

**Reading of the finding:** the *shipped organs* are almost all closed rungs; the
open/partial rungs cluster in exactly two frontiers — **embodiment/observation**
(self-access, living-body-inspector, webd, gpu-compute/gpu-3-wiring) and **new
transport/scale** (n1-lan-direct, federation F1–F3, lookup L2/L3, p2p NAT). These
are the honest broken V-rungs. gap-ledger's OPEN table is 0 because those rows are
all CI-enforced *shipped* work; the OPEN rungs here are **design-ahead-of-cert**,
a different and complementary kind of gap — surfaced by the V for the first time.

---

## OUTSIDE THE V — the three-fold reality

The classic V is one pass (design down, verify up). p-kernel actually runs the V
in **three nested ways**:

1. **Per-wave V (the sprint).** Each development wave IS one V traversal: a design
   slice on the left, a cert on the right, joined by a rung, merged only when the
   rung is green. The "implementer ≠ auditor ≠ commander" discipline is the V's
   left-arm/right-arm separation made organizational.

2. **Spiral across waves (the 年輪 / archive).** Stacking many per-wave Vs over time
   is a spiral. `docs/architecture/archive/` is the frozen record of the earlier
   turns — the `philosophy-gap-audit-{,-2..-8}` pile, superseded plans, not-taken
   surveys. Re-filing them would erase the growth rings; the archive is the
   project's memory of its own earlier Vs. It is never re-levelled.

3. **Evolution — the runtime self-V (`50-evolution/`).** Uniquely, this system can
   run the V **on itself while alive**: it self-compiles code (`self-compile.md`),
   admits self-built units through an immune boundary (`selfc-ring3.md`), and runs
   its own mind's math in a crash-isolated ring3 core (`ring3-core.md`). Here the
   "design" and the "verification" are both performed by the running node — the
   left arm and right arm fold inward. This is why `50-evolution/` sits **outside**
   the classic single-pass V: it is a V whose subject is the V-runner itself.

> 迷ったら [survival-network.md](00-concept/survival-network.md)（なぜ）へ戻り、
> どの rung が緑かは [gap-ledger.md](gap-ledger.md) と CI を正本とせよ。
