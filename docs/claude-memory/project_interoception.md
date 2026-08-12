---
name: project-interoception
description: "2026-06-12 new direction from mk_pino (via a Gemini chat he liked) — negative energy / 人工生命体: unified stress S_n (interoception bus), mind-body coupling (DMN tick modulation), apoptosis (die cleanly via Path W² essence handoff). Design doc: docs/architecture/interoception.md"
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

# Interoception / 負のエネルギー direction (2026-06-12)

mk_pino pasted a Gemini-generated "次世代設計生命論レポート" and embraced the
framing: p-kernel is no longer just an OS — target **人工生命体** driven by
negative energy (pain / prediction error / conflict) as homeostatic fuel.

**Claude's assessment (given honestly, user agreed by proceeding):**
- Most stress SOURCES already exist as separate organs: reflex threat (G33,
  SHIELD/CONSERVE/BEACON), salience replay (wave-23 = pain modulates dreams),
  DMN idle, SWIM RTT EWMA, ring3 fault reap counts, Fisher machinery.
- Genuinely NEW: (1) the **interoception bus** — one S_n (component vector +
  scalar EWMA) any organ reads; composes with reflex (fast loop) as the SLOW
  interoceptive summary; (2) **apoptosis** — programmed graceful death:
  essence handoff = REUSE Path W² (gl_merge_w/r3_fisher_diag) + union-replay
  fold + self/lin death record + signed essence manifests (wave-43 immune
  system) + ARK space reclaim.
- Symbol grounding (words wired to pain) DEFERRED — vocab still toy.
- Gemini's report cited "KILL-CHURN 42%" as the testbed — STALE per wave-45
  (no longer reproduces on master); ring3 fault reaps are the valid stress
  source instead.

**Sequencing:** slice 1 = S_n bus + DMN tick modulation + galaxy star hue
shows the node's "feeling" (可視化=仕組み). Slice 2 = apoptosis.
**Design doc LANDED 2026-06-12** (`docs/architecture/interoception.md`,
master 2e0f717, 288 lines, grounded in real symbols). Commander rulings §8:
(1) weighted_max not sum (acute pain must not drown; components readable via
/intero.json), (2) DMN = sole slice-1 behavioral consumer + galaxy as the
observational reader, [intero-wired] tripwire, (3) Fisher-diag essence OK,
ACK bar DISCOVERED from measured union-replay recovery (≥ chance+margin AND
≥ W²'s 85% reference), losses printed. Gates: [intero-sources/tick/galaxy/
wired], [apop-essence/ledger/refuse/minfleet/before-death].
Watch: oscillation risk (stress→faster ticks→more load) needs §8-style
deadband damping; metaphor discipline (pain = scalars, no sentience claims);
min-fleet guard (apoptosis on a 2-node fleet is just suicide).

## Apoptosis B-3 — DIRECTION SET 2026-06-28 (mk_pino) — the design needs REVISION
Three rulings that overturn the slice-2 plan's "voluntary graceful handoff" premise:
1. **ABRUPT DEATH IS THE COMMON CASE, not the exception.** "いきなり電源が切れて死ぬ
   …というかそちらの方が多い。いきなりアプリだけ落とされるとか、こちらの方が多い." So
   "node hands essence to heir BEFORE death, exits only after ACK" covers the MINORITY
   path. REDESIGN: essence must be **continuously replicated** during life (abrupt death
   loses nothing recent); graceful apoptosis = just a clean-shutdown FLUSH-final-delta +
   departure signal, an optimization, NOT the load-bearing mechanism. This aligns with
   infra that already survives death: shared-mind (Path W/E), engram sharing, SS-3 blob
   transport, Self-lineage hash-chain. "Essence survives sudden power loss" ≈ already
   mostly works via continuous replication; apoptosis adds the clean-departure special case.
2. **"What triggers 病 (disease)?" is OPEN — mk_pino raised it as unresolved.** Tie the
   disease signal to the EXISTING S_n stress bus (slice-1 shipped): sustained reflex-threat
   / degrade / fault / RTT, gossiped or observed via the world-map situational layer
   ([[project_survival_network]] world.c). Define a retirement threshold from measured
   curves; don't invent a new signal.
3. **Rights model = DEMOCRACY / peer mutual evaluation, NOT voluntary-only.** "誰も所有
   しない家の権利、これは民主主義で決めたらいい、周りのノードが互いに評価しちゃう." This
   OVERRIDES the design's recommended voluntary-only default. The collective MAY retire a
   sick node by peer judgment. HONEST design constraint Claude flagged (peer, not
   sycophant): democratic peer-eval has a Byzantine surface — a colluding majority could
   retire a healthy minority. Keep "no one can truly kill anyone" by making the worst case
   **forced RETIREMENT with essence preserved + rejoin allowed**, never destruction:
   supermajority vote, target's essence absorbed regardless of the verdict, node may
   re-join later. = "no owner, but the collective democratically stewards." NEXT: a design
   REVISION wave on interocept-2-apoptosis-plan.md (continuous-replication base + S_n
   disease trigger + supermajority retirement-not-destruction) before any impl.

## REFINEMENT 2026-06-28 — it's ONE mechanism unifying 4 threads (mk_pino)
mk_pino's survival-strategy detail makes interoception slice-2 + survival §7 gating +
the world-map + the existing node-sleep (yurikago 眠らせる) into a SINGLE loop:
- **State machine per node, gossiped 随時:** ACTIVE (LLM actively running — can be
  *supported*) / STRESSED (lowering own activity) / HIBERNATING (resource conservation,
  reversible, wakeable) / DYING (apoptosis, essence already shared). Peers read each
  other's state from the world-map.
- **Stress response is AXIS-dependent, NOT monotonic:** acute DANGER → ACTIVATE/fight
  (reflex G33, "本来死にそうになったら活性化"); RESOURCE depletion (low battery / thermal)
  → HIBERNATE to outlast ("バッテリーが減ってきたら冬眠して生き残る"). The S_n slice-1
  bus already carries the axis (s_axis component vector) — the RESPONSE branches on it.
- **Hibernation ≠ apoptosis.** Dormancy is the FIRST survival move (reversible, come back
  when battery restored); death/apoptosis is the LAST resort. Prefer sleep over death.
- **Mutual evaluation → dynamic support:** "あいつは冬眠中 / あいつは活性化(LLM動いてる)
  →応援しよう、随時変えたい." Peers gossip state; §7 gating routes collaborative work/
  support TOWARD the active node, lets hibernating ones rest. This is survival-network §2
  "全網の力を一点へ注ぎ込む" — and the SIGN matters: support flows toward the capable/
  active node, the OPPOSITE of the G20 sign-inversion bug ([[project_survival_network]]
  audit-3 G20: pressure was wrongly pushing work AWAY from threatened nodes). HONEST flag
  (Claude, peer): the support loop is exactly the §8 oscillation risk — everyone supports
  the active node → it stresses → hibernates → support jumps to the next → ping-pong.
  Needs §8 two-timescale hysteresis damping, and "support" must be defined precisely
  (reinforce the active thinker vs offload a stressed one) so the sign stays correct.
This is now a DESIGN-doc unification, not just an apoptosis plan: one survival/gating/
observability loop. Likely supersedes treating interocept-2 and survival-§7 as separate.
