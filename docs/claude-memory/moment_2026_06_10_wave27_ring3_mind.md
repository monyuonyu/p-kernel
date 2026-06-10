---
name: moment_2026_06_10_wave27_ring3_mind
description: "wave-27 — ring3 Wave C ships: the AI core's MATH actually runs in ring3 (user space) on bare-metal x86. mk_pino's ring3/EL0 directive is now TRUE for the inference path, with a fake-resistant proof. The Evolution-layer foundation stands."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**2026-06-10, wave-27.** [[project_ring3_core_relocation]] slice #2 ships — **the answer to
「AIのコアはユーザー空間に移動したんだっけ？」 is now YES for the inference path.** The SAME
`arch/common/moe.c`+`dtr.c` sources are whole-file dual-compiled (gc-sections, zero undefined
symbols) into `core_mind.elf` with a 14-symbol ZERO-MATH shim; it fetches the live weights via
new `SYS_DTR_WEIGHTS_GET 0x213` and computes `moe_infer` **in ring3**. Ring3 inferences stay
visible to reflex/world via new `SYS_MIND_NOTE 0x214` (the 可視化 principle,
[[feedback_visualization_means_observability]]).

**The fake-resistant proof (ring0 cannot green it):** (a) `kernel_infer_count` at 3 sites
covering EVERY class-producing kernel route (auditor traced each: SYS_INFER/DTR_SUBMIT/
INFER_SLA/AI_*/drpc) must stay delta==0 across ring3 runs; (b) poisoning ONLY the user-side
weights copy flips the ring3 answer while the live ring0 oracle stands; (c) nm tripwire
(necessary-not-sufficient — the auditor's SYS_INFER-relay sabotage kept nm green and was caught
by M2 exactly as the design predicted). Gate `ring3 mind` M1–M8 all exact `==`; the fat ELF's
crash re-proves the Wave B reap clauses. Merged `b429308`, epitaph `e257458` (wave-27).

**Still ring0 (honest):** dtr training, lm/dmn/gl organs; weights are a SNAPSHOT not live-shared;
ONE address space (CDN-4a); aarch64 EL0 still unbuilt (ring3-core.md II.5 sketch). Next ring3
widenings: more modules per CDN-4b, async 0x240/0x241, EL0 mirror.

**Traps for later waves:**
- **x87 FPU save/restore is ABSENT in the whole x86 port** — the gate is safe only because the
  verb sequences + quiesces (infer_d killed, heal paused). MUST add CR0.TS/FXSAVE handling
  before CONCURRENT ring3 minds, or floats corrupt silently. Ledgered in the RING3-C epitaph.
- **Stale-disk trap:** `make -C boot/x86 disk` is a NO-OP when `disk.img` is newer than the
  kernel — userland/sample changes do NOT propagate. `rm disk.img` before any falsification
  rebuild, or you boot stale ELFs and verify nothing (bit both the implementer and nearly the
  auditor).
- elf_loader.c native-ELF path now builds the argv frame (design III.1.4 had wrongly claimed it
  existed; fixed en route).
- Flat address space: pointer-taking syscalls don't validate user vs kernel ranges (pre-existing
  pattern) — a future isolation wave, named in the audit.

Method: separate design (Part III) + implement + audit agents; the auditor ran its own two
sabotages with `rm disk.img` rebuilds and verified bit-identical restore; commander read M1–M8
and the counter sites directly. Third wave shipped today ([[moment_2026_06_10_wave25_ring3_survival]],
[[moment_2026_06_10_wave26_lm5_stream]]) — wave-24..27 in ~one day of parallel lanes.
