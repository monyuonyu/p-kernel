# SCALE-WALL rung C1 — context carry (IMPLEMENTED)

> Status: IMPLEMENTED 2026-07-05 (NS student v2). Design: `scale_wall_design.md`
> §3/§4/§8/§9 (the doc-first wave). Cert: `tests/llm/ctxcarry_test.c` +
> `tests/llm/run_ctxcarry.sh`, wired into `.github/workflows/ci.yml`.
> Grounding rule: every claim is cited to file:line or to the cert's printed
> output; the measured A(d) magnitude is reported honestly, never forced green.

## What shipped

C1 is the FIRST buildable rung of "real conversation": the atomic ability to be
*told* something and use it. Two changes to the Cradle baby
(`arch/common/llm/student.c`, all HOSTED-tier — bare metal links
`student_stub.c`, so **zero crown impact**; both bare-metal `.text` hashes are
byte-identical after the edit, verified):

1. **RoPE positional encoding.** `q,k` (not `v`) are rotated by absolute position
   before the causal score (`st_rope_apply`, the same adjacent-pair recipe as the
   teacher's `forward.c` `rope_head`/`lm_sincosf` — one math, `-O1
   -ffp-contract=off`). Base 10000. Parameter-free. Applied consistently in
   `st_forward`, the KV path `kv_step`, and — as the exact rotation TRANSPOSE
   `R(-θ)` — in the backward before the `Wq/Wk` gradient (the grad-check confirms
   the analytic adjoint: max_rel_err 0.0103 at n=12, 0.011 at n=40, bar 5e-2).

2. **Context window `ST_MAXSEQ` 64 → 256.** A cache/scratch dim only (KV plane,
   attention scratch, forward cache) — it does NOT change `n_params` or the `o_*`
   weight offsets, so the blob byte-layout is unchanged. A fact told dozens of
   bytes back is now INSIDE the window.

Because RoPE is parameter-free and the window is a cache dim, **v1 and v2 blobs
have identical dims/`n_params`** — a v1 blob would silently LOAD into a v2
(new-math) model and a v1 peer could merge into a v2 cohort (cross-generation
Path-W corruption). So `NS_STUDENT_VER` 1→2 is **load-bearing**: `st_load` /
`st_blob_tier_ok` / `st_merge_cohort` refuse across versions (the existing
`ns_ver` header guard). Old babies are **reborn + re-distilled**, never
translated (compat/evolution law). This is a generational succession, not an
upgrade.

## The `[ctx-carry]` cert (§8)

Probe: `fact: <K> is <V>. <filler d> question: what is <K>? answer: <V>`, K/V
random 6-8 char. Metric `A(d) = CE(answer | ablated) − CE(answer | intact)` in
nats, where the ablated control replaces the fact SENTENCE with equal-length
random bytes (A is a DIFFERENCE, so "loss dropped everywhere" cannot fake it).
Recall-lookup exclusion: eval K/V carry a reserved bigram `"qz"` absent from the
`"qz"`-free training corpus, and `r3_vocab_key_id(K) == -1` — no memorization, no
R3 binding, no `mind ask` path can supply the answer; only in-context reading.

Gated arms (all PASS; independent of the training budget):

- **`[ctx-carry-window]` — the load-bearing anti-theater falsifier.** Perturb ONE
  distant fact byte: with the FULL window the answer-position logit FNV SHIFTS
  (the window READS the distant byte); under a forced 64-clamp the FNV is
  UNCHANGED (the byte is provably invisible). The falsifier goes RED (signal
  vanishes) exactly when the widened window is STUBBED — not fakeable by the
  sampler or the loop. This is the mechanism proof the cert bets on.
- **`[ctx-carry-clamp]`** — re-eval the TRAINED model with the window clamped to
  64: `A_clamp(d≥96)` collapses to ≈0 (the fact is dropped by construction).
- **`[gen-cohort-island]`** — a synthesized v1 blob (ns_ver patched to 1) is
  refused by `st_blob_tier_ok` / `st_load` / `st_merge_cohort`, model
  byte-unchanged; a matching v2 blob is accepted (control).
- **`[ctx-carry-determinism]`** — the fixed probe byte-stream FNV
  (`0x13fc6601ce8904d0`) is byte-identical on native aarch64 and qemu-x86_64
  (integer-only generation).

Printed, NOT gated: the A(d) curve magnitude, and a RoPE-vs-NoPE side-by-side
(§8: decoder-only nets can learn implicit position — the cert bets on the WINDOW,
not on RoPE; the measured RoPE/NoPE sign is noisy at this budget, as predicted by
§10.5).

## Honest measured result (the knee)

At toy scale (1.9M-param M tier, a small seeded cert corpus, CI budget) the A(d)
magnitude is **within noise** — `|A(d)| < 0.01` nats at both d=32 and d=96, and
the SIGN flips run-to-run with the training seed/budget (observed A(32) ∈
{+0.008, −0.004}, A(96) ∈ {+0.001, −0.006} across CI-budget runs). In other
words:

| d | A_full(d) | reading |
|---|---|---|
| 32 | ≈ 0 (±0.01, sign unstable) | no reliable within-window carry at CI budget |
| 96 | ≈ 0 (±0.01, sign unstable) | no long-range carry — the fact >64B back is unused |
| any ≥96, clamped to 64 | +0.0000 exactly | fact dropped by construction (falsifier) |

**This is a clean NULL — the pre-registered outcome `scale_wall_design.md` §10
named in advance.** The learned long-range copy does NOT emerge at this
data/compute scale. What C1 *did* deliver, provably, is the MECHANISM: the
widened window genuinely reads distant bytes (the anti-theater proof), and the
clamp-collapse shows that any future gain will be window-dependent, not just
"got better at English". Moving the A(d) curve off zero is the job of C2 (data
reservoir 4 KB → ≥10 MB) and C4 (capacity), not more C1 tuning — a 1.9M-param
byte model on a few-hundred-example corpus is orders of magnitude short of what
learning to copy a value 96 bytes back requires. The curve, not a forced pass,
is the deliverable.

Scope line the cert prints itself: 256-byte context carry is NOT conversation —
it is the atomic prerequisite. The toy ceiling stands until C2–C4 move the curve.

## CI

`run_ctxcarry.sh` runs a REDUCED sweep (`d ∈ {32,96}`, ~2 min) in strict CI; the
FULL nightly curve (`d ∈ {16,32,48,96,128,192}`, 64 probes) is `CTXCARRY_FULL=1`.
The gated arms above are asserted in the CI block; the A(d) magnitude is printed
for the record, never gated.

## C2 plan (`scale_wall_design.md` was never committed — this is the short note
BACKLOG's A2 row asks for, not a claim that the missing file has been recovered)

**Question:** does A(d) move off the C1 clean-NULL (`|A(d)| < 0.01` nats) if the
training reservoir grows ~4 KB (CI budget: `rounds=3, n_ex=20`, ≈60 examples,
avg ≈114 B/example ⇒ ≈6.8 KB) toward ≥10 MB? `scaling-law.md` §1.4/§297 frames
C2 as *fleet* corpus throughput (`corpus/day ~ linear in N`); this is narrower
and single-node — it scales the SAME synthetic generator `ctxcarry_test.c`
already uses (`build_probe`'s `"fact: K is V. <filler d> question: ... answer:
V"` template, `gen_train_tok`/`gen_eval_tok`), not a newly-sourced corpus. No
new corpus-provenance/license question — every training byte is procedurally
generated from an LCG seed, same as C1. The GATED arms
(`[ctx-carry-window]`/`[ctx-carry-clamp]`/`[gen-cohort-island]`/determinism)
are training-budget-independent per the cert's own design; only the PRINTED
A(d) magnitude is expected to change, if anything does.

**Method:** add a `c2` mode to `ctxcarry_test.c` that raises `rounds`/`n_ex`
until total training bytes reach a target (env-overridable), reusing
`train_ctx`/`measure_A` unchanged. Calibrate wall-clock per example first
(this host throttles under load — `benchmark-on-throttling-laptop` memory) and
size the run to what actually finishes, reporting the reservoir size reached
honestly if it falls short of 10 MB rather than forcing the target.

**Honest limitation up front:** a 1.9M-param model trained on billions of
repeats of one template family may overfit the template rather than learn a
general long-range copy skill; a moved A(d) here would need the SAME
anti-theater discipline as C1 (a mechanism check, not just a bigger number)
before it is called a real result. This run does not invent that falsifier —
if A(d) does move, the mechanism question is left for the next rung/run.

**2026-09-21 (measured, `feat/scale-wall-c2`, `ctxcarry_test.c`'s new `c2` mode):
≥10 MB is infeasible in a single unattended run on this host, and this itself
is the first honest result.** Calibrated steady-state throughput on
`pkernel_audit_ss`: **≈286 B/s** (20,052 B / 130 examples / 70.1 s, converged
by ~50 examples — see the `c2` mode's per-5%-progress log). At that rate 10 MB
needs **≈10.2 hours** of single-threaded wall time; C1's own "full nightly"
budget (rounds=24, n_ex=128, ≈445 KB estimated) already takes minutes-to-tens-
of-minutes at this throughput, consistent with the doc's own "this is the slow
part" comment. **A 2 MB run (≈4.5x past the full-nightly estimate, ≈2 hours at
this rate) was launched detached in `pkernel_audit_ss`
(`C2_TARGET_BYTES=2097152 ./ctxcarry_c2 c2 > /build/c2-2mb.log`, PID recorded
in the baton) rather than the full 10 MB** — committing a single unattended
run to a target that needs multiple future runs' worth of compute, before
confirming the smaller step even moves the needle, did not seem like a call
this routine should make unilaterally. Whether it is worth continuing toward
the full 10 MB after seeing the 2 MB result is in `pkernel-baton.md`'s
judgment-pending list.

**2026-09-21 (2 MB result recovered): the run finished — 2,097,212 bytes /
14,141 examples in 6556.6 s (320 B/s steady-state, matching the 286-312 B/s
calibration band).** Gated arms (`[ctx-carry-window]`, `[gen-cohort-island]`)
still PASS — unsurprising, they test the window mechanism and the version
guard, not the training budget. The printed, non-gated A(d) curve:

| d | A_full(d) | A_clamp64(d) |
|---|---|---|
| 16 | -0.28334 | +0.20526 |
| 32 | -0.46261 | +0.00000 |
| 48 | +0.65619 | +0.00000 |
| 96 | -0.03943 | +0.00000 |
| 128 | +0.31673 | +0.00000 |
| 192 | +0.64798 | +0.00000 |

**Honest reading: this is NOT a clean win.** The magnitude did move well off
the C1 clean-null band (`|A(d)| < 0.01`) — most points are now `0.03`-`0.66`
nats — but the sign and size do not vary monotonically or even consistently
with `d`: `d=48` and `d=192` are large and positive, `d=16`/`d=32` are
negative, `d=96` drops back near zero in between them. A genuine learned
long-range-copy skill would be expected to show *some* legible relationship
with distance, not this pattern. The more likely explanation, flagged as a
risk in this doc's own "honest limitation up front" above, is template
overfitting on ~14K repeats of one synthetic probe family rather than a
general in-context-carry skill — the model may be keying on
position/template artifacts that happen to correlate with the fact token at
each specific offset, not reading the fact robustly. **No new anti-theater
falsifier was built to distinguish these two explanations; per the plan
above, that is left as an open mechanism question, not silently called a
win.** Given this, and the ~10.2 h single-run cost for 10 MB, continuing
toward 10 MB is not obviously worth it until a falsifier exists that could
tell "learned to copy" apart from "learned this template's offset
fingerprints" — left for `pkernel-baton.md`'s judgment-pending list to
decide, not decided here.
