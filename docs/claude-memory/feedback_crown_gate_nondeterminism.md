---
name: feedback_crown_gate_nondeterminism
description: "The crown \"byte-identical linked .text\" gate is non-deterministic because modver.o embeds __DATE__/__TIME__; gate on object-level .text, not the linked ELF."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

Found 2026-07-11 while verifying feat/arch-windows was crown-neutral (adversarial audit, complete-windows-p1b workflow). The crown freeze gate compares the linked ELF's `.text` byte-for-byte against PARENT ([[feedback_swim_selfsuspect_storm]] "Crown gate = .text-vs-PARENT freeze"). That gate is **flaky**: rebuilding the SAME source gives a linked `.text` that flips hash intermittently (observed ~1/6 builds for boot/linux_x86_64).

**Root cause:** `arch/common/modver.c:130` embeds `__DATE__ " " __TIME__`. The compile-time timestamp string shifts a `.rodata` SHF_MERGE section address → changes one relocation-resolved operand inside the linked `.text`. `modver.o` is the ONLY object that differs between the flipped variants; every other object's `.text` is stable.

**Consequence:** the linked-.text crown gate can **spuriously fail** a clean change OR **spuriously pass** a bad one on a lucky single sample. It is not a robust freeze invariant. This bit the verify agent (its "linked .text identical, hash-for-hash" was one lucky sample); the audit caught it by building 6× and object-diffing.

**The robust invariant** to gate on is **object-level `.text` identity** (per-TU `.text` sha256), excluding modver.o — or kill the nondeterminism at the source by making modver.c's `__DATE__`/`__TIME__` overridable by a fixed build id (e.g. `MODVER_BUILD` / `SOURCE_DATE_EPOCH`) so `-ffp-contract=off`-style "one mind, one math" reproducibility also holds for the build stamp. NOT fixed yet — reported to mk_pino as a separate cleanup. Aligns with [[project_ci_hardening]] (strict = green-when-healthy, not more-red) and [[feedback_branch_landed_triage]] (don't trust a single agent's stated evidence — reproduce).
