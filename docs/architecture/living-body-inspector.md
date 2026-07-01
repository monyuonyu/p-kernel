# Living-body inspector — the star's organs ARE wired to REAL vitals

**Status: SHIPPED (organs wired to REAL vitals; on the phones). HONEST-GLOW is the law
and is enforced in code.**
The inspector emits the organism's REAL, O(1)-read vitals: `arch/common/galaxy.c`
(lines ~262-291) writes the organism's REAL,
O(1)-read vitals in the snapshot JSON — `training`, `facts_learned`, `epoch`, `idle_runs`,
`engram_fill/cap`, `infer_count`, `last_class`, `last_conf`, `lineage`. The browser side
(`arch/common/web/galaxy.html`: `ME_ORGANS[]` + `organGlow()`) maps each ring/organ to its
matching live vital, so an organ only brightens/grows/pulses from a value that really changed
between two honest reads (3D rings, amoeba, full interior immersion). (Original design below.)

mk_pino's want: the star interior must be REALLY connected to the mind's live state
("ちゃんと中身と繋がってるか"), active organs GLOW, you fully immerse INSIDE the body,
and it reads like a PC task-manager for the organism's vitals — in 3D.

## CORE LAW (the honesty contract)
An organ may only brighten / grow / pulse from a value that **really changed** between two
honest reads. The existing time-only breathe (`galaxy.html:676`) stays ONLY as a faint ≤4%
idle shimmer; ALL meaningful motion is state-driven and goes to ZERO extra when the vital is
at rest (exactly like `ms.stress==0` preserves the calm baseline). **Falsifiability check
during dev:** freeze the snapshot (pause poll) → the interior must go static except the
bounded shimmer. Any motion driven by `tms` alone beyond the shimmer is a bug.

Today's organs (`ME_ORGANS`, galaxy.html:627-632) are decorative (fixed frac/label, read
zero live state). This wires them to truth.

## Vital → organ map  ((SNAP)=already in /galaxy.json, (NEW)=needs an emit)
| organ | vital | source |
|---|---|---|
| **DMN** (indigo) | bloom from `m.dmn` (SNAP, galaxy.c:243); heartbeat from `idle_runs` rate (NEW, dmn.h:62) | dreaming/consolidating |
| **R3** (teal) | size from `facts_learned` = RETAINED count (NEW, r3_incontext.c:1216, NOT static /vocab); flicker from `training`=`r3_round_busy` (NEW, :569); slow tint by `epoch`=`r3_merge_epoch()` (SNAP, :603) | the slow weight memory |
| **engram** (gold) | fill = `engram_fill/engram_cap` (NEW, lm_ring_n[] sum / LM_T·B_RING, lm_consolidate.c:259) | the fast replay ring (hippocampus) |
| **self** (pink) | ticks on `lineage` = self DAG seq (NEW; mirror gx_build_self_json's read galaxy.c:341 into the poll) | hash-chained lineage |
| **Collective** (blue, NEW shell ~0.95) | density = `nPeers` (SNAP, derived); per-point color by peer threat/rtt | the swarm |
| **inference nucleus** (not a shell) | resting heartbeat ∝ `infer_count` rate (SNAP-cheap, kernel_infer_count extern dtr.h:206, browser differences polls); tint by `last_class`/`last_conf` (NEW getter moe_infer_last moe.h:171) | the reflex brain |
| **stress** (global, NOT an organ) | reuse `ms.stress` from `m.pressure`+`m.threat` (SNAP) — red-shifts the whole interior + drives amoeba wobble together | the body's distress |

New snapshot fields are all O(1) scalar `gx_qs`/`gx_qdec` writes in the `me{}` block of
`gx_build_galaxy_json` (~120 bytes, well under GX_OUTBUF=4096); NO new buffers, NO rw[] L2
copy on the task stack, GALAXY_EV (12B, _Static_assert) untouched.

## Immersion camera — pierce the membrane, fly among the shells
Depth IS the mode (no button; chat overlay stays reachable). One continuous eased dolly,
three regimes:
- **R1 EXTERIOR** (`pscale<=INTERIOR_AT=1.6`): unchanged calm galaxy, organs hidden (byte-for-byte today).
- **R2 SURFACE** (`1.6<pscale<INTERIOR_FULL=3.1`): organs fade in as a translucent screen-space interior (today's behavior).
- **R3 INTERIOR** (`camD` below a new `CAM_SURFACE` into `CAM_INNER=-286`): organs switch to
  WORLD coords via `project()`; the camera sits inside.

Math/guards: each organ gets a world radius `Rw[i]=ME_WORLD_R·frac` with `Rw_max≤200 <
FOV+CAM_INNER=234` (outermost shell stays in front of the camera at full penetration).
Mandatory **per-point cull** `zc=FOV+Zrot+camD; if(zc<=8) continue;` (never divide
through/behind camera — replaces the old global CAM_MIN clamp for negative camD). A single
`rev_inner` scalar crossfades: membrane alpha 1→0 (fade drawMeBlob :600-606 so you PIERCE not
clip), body-circle clip (:668) disabled, organ pos screen-space↔world-space blended (no
geometric pop). Depth-sort points by `zc` (real parallax/occlusion). Same shared yaw/CP/SP, so
auto-yaw + drag orbit you INSIDE. Exit by dollying out / double-tap empty → `camDTarget=CAMD`
(860) smooth pull-back. Calm R1 path stays byte-identical.

## Biometric task-manager (two surfaces, both from the 1Hz snapshot)
- **Billboarded organ labels** (3D interior, screen-space text, never rotated): e.g. DMN
  `"dream: consolidating  {idle_runs}/min"`, R3 `"learned {facts} ep{epoch} ●training"`,
  engram `"{fill}/{cap} (pct%)"`, self `"lineage {n}"`, Collective `"linked {nPeers}"`,
  nucleus `"{infer}/s cls{c}@{conf}%"`.
- **HUD vitals list** (extend #hudtech / a #panel slide-in): a monospace table — DMN state,
  consolidation rate (runs/min), R3 sleep rounds, facts learned, facts pending, merge epoch,
  training ●/○, engram fill %, inference/s, last decision class@conf, pressure, threat, stress
  S_n, peers alive, dropped events, lineage seq. Rates computed BROWSER-SIDE by differencing
  successive snaps with wallclock dt (no kernel rate counter). Tiny fixed-size ring +
  sparkline for infer/s and stress = the "task-manager graph" feel at near-zero cost. Reachable
  in BOTH calm and immersion (fixed overlay), coexists with chat. All labels i18n via S().

## Implementation notes
- **Rebase onto the chat rework FIRST** (shares galaxy.c/galaxy.html); emit vitals over
  whichever channel is live (poll /galaxy.json and/or WS snapshot frame).
- Server: tiny getters (r3_round_busy_get, r3_retained_count, lm_ring_fill/cap) + scalar emits;
  r3_merge_epoch/kernel_infer_count/moe_infer_last already public. Hand-rolled gx_qs only
  (no snprintf, no 2nd JSON lib). Compile-check x86_64 + aarch64 (LP64: UW/UB, no `long`).
- Browser: pass `snap.me` into meStarInterior (call site :885); replace fixed `o.frac` with the
  glow_rule sizing; allocation-free per-frame; gate world-space path + sparklines behind a perf
  check on low-DPR/small canvas; keep 60fps (reduce point counts before reducing honesty).
- E2E cert: teach → engram fills + facts_learned rises + training strobes; sleep → DMN blooms +
  idle_runs ticks; raise stress → interior reddens + trembles with the membrane; dolly to
  CAM_INNER → fly inside; chat usable throughout.
