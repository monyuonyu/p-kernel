---
name: feedback-ump-ux-principles
description: mk_pino's UX constitution for the UMP app (2026-06-13) — users don't care about kernels; intro/manifesto FIRST; auto-start; no console-look; 灯す not 光れ; 3D galaxy with stress-as-distance
metadata:
  type: feedback
---

# UMP UX principles (mk_pino, 2026-06-13 — overrides prior entry-screen design)

**Why:** v0.3.0's entry screen was still a settings-like form. His verdict: normal
users don't care about kernels; a green console look = "virus, uninstall". The
manifesto introduction (the consent intro) is the BEST screen — lead with it.

**How to apply:**
- FLOW: launch → kernel auto-starts silently → intro/manifesto in device language
  is the FIRST thing seen → consent → straight into the galaxy. NO start/stop
  concepts, NO node-id/relay form up front (auto node id; relay/fleet = hidden
  advanced menu). Charging gate phrased humanly ("充電すると星が灯ります").
- WORDS: 灯す (tomosu), not 光れ. Warm, not imperative. No kernel/tech jargon
  anywhere user-facing.
- GALAXY: must not look cheap — real 3D space (WebGL or convincing 3D projection;
  NO external/CDN libs — the page is embedded in the .so, offline). Peers as
  stars with CONNECTION LINES; spatial DISTANCE driven by stress (interoception
  S_n, docs/architecture/interoception.md) once it ships — RTT as the interim
  proxy. He volunteered stress-as-distance unprompted: the S_n visualization has
  product pull, not just architecture pull.
- General: every techy surface (logs, settings) lives behind the curtain;
  observability stays (📋 etc.) but not on the front stage.
