---
name: project-compat-evolution
description: "The compatibility/upgrade strategy for the no-central never-dying fleet (yurikago). DECIDED 2026-06-14: no frozen core — evolve by generational succession. How to ship updates after release without splitting the swarm."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

mk_pino raised this **right before the first public release** (2026-06-14): "Once we
release, we distribute updates while keeping compatibility — can the current mechanism
be changed later?" For a no-central fleet you can NEVER recall or force-update shipped
nodes; some run old versions forever. So protocol/format/model evolution must not split
the swarm into non-communicating islands. Design doc = `docs/architecture/compatibility.md`
(on branch wave-i18n-galaxy; commit d2bba92).

**Ground truth (verified in code):** every seam has magic+version, but every rx handler
does `version != CURRENT → drop` (swim.c:285, drpc.c:352, kdds.c:364, raft.c, pfs_repl.c,
replica.c, the mind/teach gate r3_incontext.c:2684). Packets are fixed packed structs,
exact-length receive → NO TLV/skip-unknown. Precedent that evolution works: MT_WIRE_VER
evolved 0→1→2. So shipping v2 today would HARD-FORK the fleet (v1 and v2 drop each other).

**THE DECISION (mk_pino corrected my first framing TWICE — this is the canon):**
My initial proposal was "freeze 3 things perfectly in v1 (loader + profile + wire), make
everything else fluid." **mk_pino rejected it:**

1. **"A perfect frozen v1 is impossible — everything WILL change."** So the premise is
   *nothing is permanently frozen* (not even the loader/wire/self-format). Survive deep
   change via **generational succession** (already designed for the model, now applied to
   the whole stack): a new generation (new wire/loader/arch) is born ALONGSIDE the old;
   **bridge nodes** that speak both versions connect old↔new during transition; the old
   generation eventually **dies** — and the system is **death-native**, so that's fine.
   Continuity is carried NOT by a frozen format but by **lineage (Self layer hash-chain)**
   + **collective knowledge (Path E)**. The only realistic invariant is a **CHAIN**: each
   version can read the immediately-previous version (pairwise migration) → you can always
   walk forward. **No bedrock — just an unbroken chain.**

2. **"Weight/knowledge propagation already exists (Path E, teach A→B)."** Correct — it's
   NOT a new mechanism. Separate two things: **knowledge** (taught content) already flows
   via Path E, AND — elegantly — **arch change needs NO weight-translation: a new-arch node
   re-learns from the collective as a baby** (from-baby IS the migration mechanism; knowledge
   transfers by RE-TEACHING, not weight-copying). The genuinely-new/hard part is **CODE/arch
   distribution**, two tiers: (a) **loader-compatible = signed payloads (weights, small code
   modules) over the MESH, hot-loaded — this is mk_pino's "auto-update," and it MUST be
   signed** (Ed25519/SIGN_MANIFEST; sign code+weight PROVENANCE only, never human identity;
   sandbox new code via the selfc germ, wave-31) or the fleet becomes a botnet; (b) **deep
   (loader/wire itself changes) = platform APK update (A/B + rollback) + generational succession.**

**One line:** Nothing is frozen. Unbroken chain (each version reads the previous) +
generational succession + death-native + knowledge transfers by re-teaching + only NEW CODE
is signed-OTA (mesh = small updates / platform = deep updates). This makes "everything can
change" + "can't be hijacked" + "auto-update" all hold at once. **mk_pino's view of death
(death-native, knowledge is collective, lineage persists) IS the compatibility strategy.**

**Release-minimum (the only thing v1 must get right):** NOT "freeze perfectly" but just
(i) don't hard-drop unknown/higher versions or trailing bytes (the first link of the chain),
(ii) a signature-verified update-accept path (mesh small updates), (iii) lineage(Self) +
collective(Path E) continuity. Perfection unneeded — if the next version can read this one,
anything can change later.

Still a DRAFT/design — interop cert tags proposed ([compat-mesh], [compat-degrade],
[compat-persist-read-old], [compat-skip-unknown]); not yet implemented. Open: schema
agreement bet on gossip/eventual-consistency (no gatekeeper) vs raft-for-schema-only
(unproven convergence); version/capability-poison defended only via signed content-ids.

Related: [[project_living_mind_vision]] (Evolution layer = arch mutable while alive),
[[project_pkernel_philosophy]] (death-native, 歴史地層), [[feedback_ark_no_identity_verification]]
(sign code/weights, never humans), [[project_survival_network]].
