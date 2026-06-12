---
name: moment_2026_06_12_alldirections
description: "2026-06-12 全方向 batch: signing (wave-38, first crypto primitive) + LM-9 (wave-39, R_DM capacity surgery 4->16 bindings) shipped in parallel; the IST lane FALSIFIED its own task premise (the crash is a kill-path use-after-free, NOT a stack overflow) and honestly kept the ledger row OPEN. mk_pino: 'these mechanisms are 変態的' — comparative: the parts have precedent (Petals/BOINC/IPFS/FL), the synthesis is alone."
metadata:
  node_type: memory
  type: project
---

**2026-06-12.** mk_pino said "全方向いきましょう、順番は司令官に任せる" — fanned out 3 disjoint
lanes (Opus agents). Two shipped, one delivered an exemplary honest negative.

**wave-38 SIGN-1 — the Ed25519 provenance primitive** (`signing.md`, `0c248fa`). First crypto
in the project. TweetNaCl v20140427 transcribed verbatim + SHA-512, RFC 8032 KAT-gated every
build. The audit verified THREE ways — self-test, **OpenSSL 3.5.3 byte-identical on RANDOM
data**, Python cryptography — so it's real Ed25519, not a self-consistent lookalike. HARD
BOUNDARY held: signs CODE/WEIGHTS only, NEVER humans (keys belong to nodes; lm_self/ark_profile
untouched) — [[feedback_ark_no_identity_verification]]. Scoped to primitive + 4 gates; the 3
live integrations (Self verify, selfc adopt, genome_sprout) are unblocked follow-ons. Honest:
S-malleability (stock TweetNaCl), bare-metal weak-keygen is VISIBLE not silent.

**wave-39 LM-9 — the R_DM capacity surgery** (`living-mind.md` X, `9fc03dd`). The mind's thinking
WIDTH widened: R_DM 32->48, R_KEYV 8->16, R_VALV 32->64, R_NP=21568. **comfortable-N 4->16**
(audited, curve-discovered). The shared-LayerNorm bump (DTR_LN_MAXW 32->64) is PROVABLY FREE —
dtr.c zero-diff, a bit-exact dtr_ln_bwd(n=8) oracle, 38-tag sensor sweep byte-clean
([lang-sensor-intact]). Honest deviation: pretrain ~23s (not the design's ~4s) — train_n
192->512 is real capacity need, CI timeout 420->720. Teaches the user's question: capacity is
set by ATTENTION WIDTH (R_DM), not vocab size.

**The IST lane (no wave — row stays OPEN): the model example of the immune system.** Tasked to
close KILL-CHURN-CRASH with "an IST for IRQs", it FALSIFIED that premise experimentally (the
crash reproduces identically at 2/8/16 KB stacks -> NOT a stack overflow) and found the real
root cause: a **kill-path TCB use-after-free** (infer_d blocked in a timed sem wait; tk_ter_tsk
+ user_proc_teardown + tk_del_tsk leaves a timer/sem-queue node referencing the freed TCB).
It did NOT hit and did NOT weaken the >=20-boot bar; the row stays honestly OPEN with a precise
handoff (dproc.c:221, task_manage.c:260). Side deliverables (audit pending): a #DF->IST2 honest-
halt net + a repair of a real pre-existing master build break (boot/x86 missing ark_profile.c
since wave-33). ALSO found: this x86 port runs in IA-32e LONG MODE (CS=0x18, 64-bit IDT gates),
not 32-bit as the docs implied — verify before the next kill-race fix.

**Comparative (mk_pino: 変態的?):** the parts have precedent — Petals/hivemind (distributed LLM
inference across volunteer nodes, the closest to DKVA), BOINC/SETI@home (every-device-a-node +
the galaxy-screensaver feel), federated learning (gl_merge), IPFS (the ark), SWIM/CRDT, MoE.
The SYNTHESIS is alone: kernel-level + ownerless + never-dies + autobiography + the human-memory
ark + consent ethics as load-bearing code + the honesty-audit method. Behind on ML scale
(9K-21K params vs Petals' 100B+), single on the integrated-organism vision.

Follow-up to merge when the IST audit returns: the #DF net + the boot/x86 build fix, and ledger
the KILL-CHURN-CRASH CORRECTED mechanism (use-after-free, not overflow), row OPEN.
