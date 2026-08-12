---
name: project_self_access_embodiment
description: MCP-analogue self-access / embodiment — the resident mind freely drives its OWN node (shell/storage/devices) + self-writes & selfc-compiles drivers for new hardware. Body×Evolution layer. Design-doc-first.
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

2026-06-14, mk_pino's idea: an **MCP-analogue "self-access" / embodiment protocol**. MCP gives an LLM structured tool-access to the OUTSIDE; this is the mirror — give the mind that lives on p-kernel structured, relatively-free access to its OWN node: shell, storage, devices. Crux of the second half: when a **new device** appears, the mind **writes its own driver, selfc-compiles it, loads it, uses it** = the Body × Evolution layer handshake (the mind feels its body and grows new organs).

**Why this is ~70% already built (don't start from zero):** selfc (libtcc germ + 5-symbol capability boundary + ring3 germ, [[moment_2026_06_11_wave31_selfc_germ]] / [[moment_2026_06_10_wave27_ring3_mind]]) = the compile-and-load heart; the T-Kernel shell verbs = the human-facing surface; p-fs = storage; ring3 core = safe-ish execution; consent gate + dproc/guard reaping = immune system. The TWO real gaps: (a) a MIND-facing structured affordance surface (today's shell is human-facing, not a callable protocol for the resident mind), (b) the device-detect→author-driver→compile→load→use loop.

**Design tensions named to the user (honest, peer-level):** (1) FREEDOM vs the IMMUNE SYSTEM — "free self-access" must NOT bypass the capability boundary/consent/germ-isolation that selfc built *because self-compiled code can kill a node*; free ≠ unguarded, must stay fail-closed. (2) "driver" means different things per arch — bare-metal/Linux can write real/userspace drivers; **Android has NO ring0 driver loading (Bionic .so; selfc is a STUB on Bionic)** → there "driver" = userspace device access via NDK/Android APIs; don't blur this. (3) scope = the node's OWN body only; cross-node stays governed by the Collective layer.

**Status:** design-doc-first per project norm (like living-mind/regions/survival-network). Dispatched a design agent to write `docs/architecture/self-access.md` (substrate inventory → 2 gaps → protocol → immune reconciliation → per-arch reality table → R-plan smallest-slice-first → open questions). NOT implementing yet. Connects to [[project_ring3_core_relocation]] (Evolution foundation) and [[project_living_mind_vision]] (the mind that evolves its own architecture while alive).

**Open-question answers (mk_pino, 2026-06-14):** Q3 = YES — self-access LOGS the mind touching its own body to the Self-layer lineage/歴史地層 (the body-touch log is kept; it's permanent autobiography, not transient). Pairs with the OUTWARD mirror [[project_webos_human_environment]] (imaginary-UI), which mk_pino decided to "prepare," and with the galaxy-screen → chat-format decision.

**Q1 + Q2 RESOLVED 2026-06-28 — "自分の体であれば常に自由にしておきたい":** for its OWN body the mind is ALWAYS FREE — no per-access gating on autonomous read-only introspection (Q1 = fully free), and driver/device drive consent is once-not-each-time (Q2 = free for own body). So R1 (the DMN idle hook autonomously calling `body`) is GREEN to implement, and the later self-write/self-drive of the node's own shell/storage/devices inherits the same "own body = free" rule. The immune-system caveat from design-tension (1) STILL HOLDS and is NOT in conflict: "free" governs *whether the mind may touch its own body* (yes, always), NOT *whether self-compiled code is allowed to be unsafe* — selfc germ-isolation / capability boundary / fail-closed reaping still apply to CODE SAFETY. Freedom of access ≠ removing the germ sandbox. Cross-node still governed by the Collective layer (NOT "own body").

**TRUST + ISOLATION MODEL SET 2026-06-28 (mk_pino) — resolves the freedom-vs-immune tension:**
"ユーザー空間で動かす人体。クラッシュしてもカーネルは死なない。自分で自分用に作ったコードは
基本信じる。署名付きも信じる。外部からのものは基本入れない." Two orthogonal rules:
1. **CRASH ISOLATION via user space (ring3/EL0):** the "body" / embodiment code runs in
   USER SPACE, so a crash CANNOT kill the kernel. This is [[project_ring3_core_relocation]]
   (wave-25 already proved the kernel survives a ring3 core crash) + the selfc fork() germ
   (wave-31). So the germ/sandbox's role is REFRAMED: it's CRASH CONTAINMENT (a trusted-
   but-buggy self-driver must not take the node down), NOT distrust of self-code.
2. **PROVENANCE-BASED TRUST (fail-closed against external):** the admit policy is —
   (a) **locally self-authored code → trusted by default** ("自分で自分用に作ったコードは
   基本信じる"); (b) **validly signed code → trusted** ("署名付きも信じる") = the Ed25519
   `sign_manifest_verify` + `selfc adopt key` anchor (wave-38/42); (c) **external / unsigned
   → basically REFUSED** ("外部からのものは基本入れない"), fail-closed. So trust = (locally
   authored) OR (valid signature from an adopted key); everything else is denied entry.
This is COHERENT with existing infra and mostly already built: ring3 survival + selfc germ
(isolation) + Ed25519 signing (the trust anchor). PER-ARCH honesty still holds: real ring3
user-space self-compiled drivers on bare-metal/Linux; Android/Bionic has NO ring0 driver
load + selfc is a STUB there → "body" = userspace device access via NDK, no self-compiled
native drivers. So R1 (autonomous body read) + the embodiment write/drive path = GREEN to
design as: ring3 user-space execution + provenance trust gate + always-free for own body.
