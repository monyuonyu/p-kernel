---
name: project-next-phase-vision
description: mk_pino's next-phase vision (2026-06-13, after the wave-50..55 sweep) — 4 buckets: polish (intro/i18n), the LM scale wall (real conversational model), the body (GPU+resource-aware via interoception), the net (multi-platform comms + NAT/external relay)
metadata:
  type: project
---

# Next-phase vision (mk_pino, 2026-06-13)

Stated after the external-audit sweep closed (ledger open rows → 1). The ark app
is functional (0.4.7 "remembers"); now: quality + the real frontier. Four buckets
by nature:

## A. Polish — known work, ready to wave
1. Picture-rich 3-4 page intro for first-time users (already queued in
   feedback_ump_ux_principles).
2. Finish i18n: galaxy page human-language chrome is ja/en only → remaining 30
   langs (the MANI_SPECS/page-chrome i18n debt).

## B. The LM scale wall — escape the vocabulary cage (THE MOUNTAIN)
3. From ~dozens of words (R_NP=21568, single-token recall) to a REAL
   conversational language model. Claude's honest read: this is a DIFFERENT
   ORDER of magnitude (tens of millions+ params), NOT a few-day wave — it's the
   project's NEXT CHAPTER, design-first. BUT it's where the project's reason to
   exist finally becomes necessary: a model too big for one device MAKES
   distribution mandatory. "まず原理、規模は後" — the "後" has arrived.

## C. The body — run to device capability (GPU / battery / memory)
4. Network size auto-scales to device capability (capacity(N) exists).
5. GPU utilization: use the phone's GPU (Adreno/Mali via Vulkan compute / NNAPI)
   if present; fall back to CPU grades; degrade. NEW big capability — bare metal
   has no GPU drivers; the APK kernel-as-Linux-process could reach the GPU via
   compute APIs. Its own chapter.
6. Resource-aware operation: watch battery level + memory headroom, degrade
   appropriately. HALF-EXISTS: capacity(N), degrade levels (FULL/REDUCED/SOLO),
   and especially [[project-interoception]] (S_n negative-energy bus already
   designed to fold battery/memory/threat into one stress scalar that modulates
   behavior). Resource-aware degradation = the interoception SLICE 1 implementation.

## D. The net — multi-platform comms + NAT/external (MOSTLY EXISTS)
7. Android app <-> this emulated env (x86/ARM nodes here) can communicate;
   multi-platform comms verified.
8. NAT traversal + a real EXTERNAL environment (public-IP relay).
   GOOD NEWS: relay v2 IS the NAT-traversal substrate; demo-fleet.md is the
   2-phone+relay scenario. This is mostly OPS (stand up a public relay + actually
   connect) not new code — and is TESTABLE NOW (Android + a host x86/aarch64 node
   on the same relay).

## Sequencing (Claude's proposal, not yet chosen by mk_pino)
- A is quick wins. D is "wire + ops, testable now". C-resource-aware = implement
  interoception SLICE 1. C-GPU and B-real-LM are the two real chapters (design-first).
- The honest dependency: real conversation (B) is what makes everyone WANT to use
  ark (per product-soul) AND what makes distribution necessary (C/D matter at scale).
  But B is the hardest. Possible path: prove the distributed-big-model PLUMBING
  (D + tensor/pipeline parallel already exist) on a model just big enough to not
  fit one phone, before chasing true conversational quality.
