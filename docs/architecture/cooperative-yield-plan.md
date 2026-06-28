# Cooperative-Yield Plan — DMN consolidation must not starve the node

Status: DESIGN ONLY (cert-first). No code changed. Repo master tip 83981b7d.
Author lane: design agent. Implementation + audit are SEPARATE lanes (the method).

---

## 1. The bug (independently root-caused, evidence-backed)

On the cooperative Linux port, a node's heavy student-net consolidation is
**non-preemptible** and starves the whole node.

Mechanism, call chain:

```
dmn_task            arch/common/dmn.c:456   (loop: tk_dly_tsk(DMN_PULSE_MS); dmn_idle_work())
  dmn_idle_work     arch/common/dmn.c:198
    student_dmn_consolidate()              dmn.c:265  (gated 1-in-ST_DMN_INTERVAL)
      sleep_rounds()                       student_shell.c:583 -> :324
        for r in rounds: for w in train_windows:
          st_zero_grad / st_forward / st_backward / st_adam_step   (student.c)
```

`st_forward` / `st_backward` are pure FP matmul over the resident baby
(d_model=128, n_layers=4, n_experts=4, seqlen=32). They make **no `tk_*`
call**, so they contain **no preemption-safe point**.
`arch/linux/<arch>/preempt.c` only switches context at the next
`END_CRITICAL_SECTION` safe point — the SIGALRM handler deliberately does **not**
rewrite the mcontext (see `preempt.c:9-18`). A compute loop that never re-enters
the kernel is therefore **never preempted**. While `sleep_rounds` runs its
`rounds × train_windows` passes, no other task runs: shell, SWIM, K-DDS, net,
`mind ask`, pfs-block-serve all starve.

The node is **alive and has its data** — it is "asleep / dreaming" and cannot
serve a console or network request. Proven: post-idle `/proc/<pid>/stat` state
`R` (running), ptrace PC inside `st_forward`/`st_backward`, console never echoes
after a peer is killed, 150 s of polling served 0 requests. The 150 s magnitude
implies a **large cradle-delivered lesson corpus** inflating `train_windows`
(each `student_dmn_consolidate` then runs for seconds and repeats); the fix below
caps per-call compute **regardless of corpus size**.

### CI impact (this run, master 83981b7d)

FAILS 4 live-3node jobs that kill a node then ask a survivor to *think*:
`samples/27_protect` (serve a pfs block), `41_shared_mind` (`mind ask`),
`42_one_mind` (`mind ask`), `32_collective_learn`. **Same root cause — one fix
covers all four** (once the dreaming node yields, every other task — including
the R3 `mind ask` path and the pfs serve — gets the CPU).

MUST NOT break the kill-but-no-heavy-post-kill-compute jobs that PASS today:
`13_survival_loop`, `35_parallel_infer`, `22_composite`.

---

## 2. Hard constraints

### 2.1 Determinism is non-negotiable ("one mind, one math")

The kernel enforces byte-identical DTR/student logits (the `fpdet` BLOCKING
gate; the crown). A yield inserted into the math must not change a single output
bit — **no FP reassociation, no reordering of accumulation, no FMA-contraction
change** (cf. the salty-bug: `-ffp-contract=off` on all targets).

Therefore the chosen mechanism is **bounded WHOLE-pass scheduling**: do at most
`K` *complete* `st_forward`/`st_backward`/`st_adam_step` triples per
`student_dmn_consolidate` invocation, checkpoint the cursor, and **return** —
the next pulse resumes the remaining passes. **No intra-pass yield** is
introduced (mid-matmul yields would risk reassociation and re-entrancy and are
explicitly rejected here).

Why whole-pass slicing is byte-identical (the core proof obligation):
the consolidation is a fixed, ordered list of updates
`U = [(r,w) : r in 0..rounds-1, w in 0..train_windows-1]`, each update being
`st_zero_grad; st_forward(buf_w); st_backward(buf_w); st_adam_step(lr)`. Adam
state (`mu[]`, `vu[]`, `adam_t`) lives **inside `st_model`** and persists across
calls. Slicing changes only *where we pause between list elements*, never the
order, the operands, or the per-pass reduction order. `window()` reads from a
corpus **snapshotted at batch start** (see 3.3), so `buf_w` is identical bytes.
Hence the weights after the N-th update are **bit-identical** whether the list
was run all-at-once or in `K`-sized chunks across many pulses. The only way to
break this is for another writer to mutate the model or the corpus mid-batch —
prevented by the busy guard (2.2) and the corpus snapshot (3.3).

### 2.2 Re-entrancy

`st_forward` is documented single-threaded / non-reentrant — it shares file-
static `tw_buf` / `st_frozen_pos` (student.c:765, :867). Two facts make the
sliced design safe:

1. **No true concurrency.** Cooperative scheduling means only one task runs at a
   time; yields happen **only** at our explicit checkpoint return (a pass
   boundary), never mid-`st_forward`. At that boundary `tw_buf`/`st_frozen_pos`
   are quiescent (they are reset at the top of every `st_forward`, student.c:645,
   and used only within one call that always completes before we return).
2. **Readers are safe; only writers need a guard.** An interleaved inference
   (`mind ask` / student generate) that arrives between two slices is a **pure
   read** of the model: it runs its own complete `st_forward` (re-initialising
   the static buffers), returns the current (partially-consolidated) weights'
   answer, and does not touch the update order. It cannot corrupt the batch.
   What *would* corrupt determinism is a **writer** that replaces or mutates
   `g_student` mid-batch (human `student` retrain, Path-W `merge`,
   `st_load` restore, a fresh birth).

State machine (file-static cursor in student_shell.c, hosted-only):

```
g_consol_active : 0 = no batch in flight ; 1 = a sliced batch is draining
g_consol_idx    : next flat index into U (0..rounds*train_windows)
g_consol_total  : rounds*train_windows for this batch (snapshot)
g_consol_seqlen, g_consol_trainw, g_consol_lr : the batch plan (snapshot)

student_dmn_consolidate():
  if !active:
      <no-op gates: persistence + resident baby, unchanged>
      cradle_poll_and_pull(); snapshot corpus -> trainw/seqlen/lr
      g_consol_total = ROUNDS*trainw; g_consol_idx = 0; g_consol_active = 1
  run up to K passes starting at g_consol_idx (resumable sleep_rounds), advancing
      g_consol_idx; each pass is the SAME zero_grad/forward/backward/adam triple
  if g_consol_idx == g_consol_total:        # batch COMPLETE
      <worth_save persist — unchanged math, run ONCE at batch end>
      g_consol_active = 0
      return 1            # "a consolidation completed" (drives dmn.c print/galaxy)
  else:
      return 0            # work done, batch still draining (silent, see 3.4)

student_consol_busy()  -> g_consol_active        # observability + cert
student_consol_abort() -> g_consol_active = 0     # writers call before replacing g_student
```

A writer that replaces weights calls `student_consol_abort()` first. Aborting is
**safe**: every applied `st_adam_step` left the model in a valid (merely under-
trained) state, and the writer was about to overwrite anyway. The next DMN tick
starts a fresh batch. No pass is ever half-applied (we only ever pause at a
triple boundary), so the cursor never loses or repeats a partial update.

### 2.3 Crown safety (.text invariance on bare metal)

`student.c` / `student_shell.c` are **HOSTED-ONLY**; bare metal links
`student_stub.c` weak no-ops, so all cursor state and the resumable loop are
`.text`-neutral on bare metal. **`dmn.c` is `arch/common` and BARE-METAL-LINKED
— it must stay byte-identical** (crown 755a20fa / 4064d8a9).

Design choice that guarantees this: **all slicing lives inside the hosted
`student_dmn_consolidate()`**; the dmn.c call site (`dmn.c:265`,
`... && student_dmn_consolidate()`) is **unchanged**. dmn.c keeps calling it
once per `ST_DMN_INTERVAL` pulse; the function internally advances `K` passes and
returns. **Zero diff to dmn.c → crown trivially preserved.** No `_TK_HOSTED_LIBC_`
guard is even needed in arch/common, because nothing in arch/common changes.

Lines that are hosted-only and may change: `student_shell.c:273-275`
(add `ST_DMN_PASS_BUDGET`), new file-static cursor near :270, the body of
`sleep_rounds` (student_shell.c:324-340) refactored to a resumable form **for the
DMN caller only**, and `student_dmn_consolidate` (student_shell.c:555-618). The
two **non-DMN** `sleep_rounds` callers (`merge` :969, human `student` retrain
:1045) keep the synchronous loop — they are explicit human commands, not the
autonomous starve path, and run to completion intentionally.

---

## 3. Chosen mechanism (detail)

### 3.1 Resumable sleep loop

Refactor `sleep_rounds` (student_shell.c:324) so the DMN path can run a bounded
prefix and resume. Keep the existing synchronous signature for the two human
callers; add a resumable variant the DMN tick uses, e.g.:

```c
/* returns the new cursor; runs passes [start, min(start+budget, total)) */
static int sleep_rounds_resume(st_model *m, int seqlen, int train_windows,
                               int rounds, float lr, int start, int budget);
```

The flat index `i` maps to `r = i / train_windows`, `w = i % train_windows` —
**exactly** the nesting order of today's `for(r) for(w)` (2.1), so the update
order is preserved bit-for-bit. The `logits` scratch is `malloc`/`free`d per
call (already the case at :328-339); no cross-call buffer state beyond the model.

### 3.2 K (per-call pass budget)

`K = ST_DMN_PASS_BUDGET`, a compile-time constant (a fixed `K` — not a wall-clock
cut — keeps the cert reproducible; note that even a wall-clock cut would be
final-weight-safe per 2.1, but fixed K is preferred for the gate).

Sizing: target ≤ ~50 ms of compute per call so the node is comfortably
responsive against the 1000 ms `DMN_PULSE_MS` cadence. **Measured pass cost: not
yet measured in-tree** (design-only; building the kernel was out of scope this
lane). Analytic estimate for the M-tier baby (d=128, L=4, E=4, seq=32): one
forward+backward+adam triple is a few MFLOP → ~1–5 ms on a host core, so
`K ≈ 8–16` lands near the 50 ms budget. **Action for the implement lane:** add a
one-shot timer around a single triple in the in-proc cert and set `K` from the
measurement; until then default `K = 8` (conservative).

### 3.3 Corpus snapshot

`cradle_poll_and_pull()` and the `trainw`/`seqlen`/`train_end` computation
(student_shell.c:573-581) run **once, at batch start**, and the resulting plan is
frozen in the cursor. A lesson that arrives mid-batch is **not** pulled until the
current batch completes (picked up by the next batch). This is what makes the
sliced run byte-identical to all-at-once (2.1); the cost is a bounded latency
(one batch) before a brand-new lesson begins consolidating — acceptable.

### 3.4 Persist + print cadence (preserve existing invariants)

- **Persist once per batch**, at completion (3.2 state machine), not per slice.
  The `worth_save` / throttle math (student_shell.c:585-616) already assumes a
  *settled* post-sleep state; persisting mid-batch would write under-trained
  weights and wear flash. End-of-batch persist matches today's
  one-write-per-consolidation semantics.
- **Return 1 only when the batch completes**, 0 while draining. dmn.c prints
  `"[dmn] sleep: distilled teacher -> resident student (baby)"` and emits
  `EV_CONSOLIDATE` exactly when the call returns 1 (dmn.c:265-268). Returning 1
  once per completed batch keeps "one sleep line / one galaxy particle per
  consolidation" — protecting any count-based cert (cf. wave-29's exact-count
  expectations) and keeping dmn.c semantics unchanged.

---

## 4. The cert (falsifiable, load-bearing)

Add a hosted in-proc cert (the fast gate) plus reuse the 4 live jobs (end-to-end).

### 4.1 Cert A — sliced == all-at-once (byte-identical; the determinism proof)

1. Init two clones `M_ref`, `M_sliced` from the **same seed/weights**.
2. `M_ref`: run the whole batch synchronously (today's `sleep_rounds`).
3. `M_sliced`: drive `sleep_rounds_resume` in `K`-sized chunks until the cursor
   reaches `total`, **interleaving a pure-read inference** (`st_eval_loss` /
   generate) between chunks (proves reader-interleave does not corrupt — 2.2).
4. Assert `st_save(M_ref)` bytes `==` `st_save(M_sliced)` bytes (weights, Adam
   moments, `adam_t` all identical). GREEN only if slicing is bit-exact.
   RED if a broken slicer re-pulls the corpus mid-batch, reorders, or yields
   mid-pass.

### 4.2 Cert B — responsive while thinking, AND thinking not skipped

Simulate the DMN pulse loop over a batch and assert all three:

- (responsive) per call, the `st_forward` invocation count delta ≤ K
  (use a forward-call counter; bounded per-call compute = the node returns to
  `dmn_task`'s `tk_dly_tsk` yield every pulse);
- (not skipped) cumulative forward count across the whole batch
  `== rounds * train_windows`;
- (correct) final held-out loss `==` the all-at-once final loss (Cert A already
  proves byte-identity; B asserts the loss equality as the human-readable form).

This **distinguishes "responsive while thinking" from "thinking got skipped"**:
a cheat that restores responsiveness by dropping rounds fails the cumulative-
count and final-loss asserts.

### 4.3 Falsifier (must go RED without the fix)

Build with `-DYIELD_DISABLE`: `student_dmn_consolidate` runs the **entire** batch
in the first call (today's behaviour). Then:

- Cert B's per-call forward-count delta `== rounds*train_windows` (not ≤ K) → the
  "bounded per call" assert FAILS RED (models the stall).
- End-to-end: the live job's survivor stays in `R`/compute and the console/net
  request **times out** → 27/41/42/32 RED. With the fix, the request is served
  within the bounded window and the batch still completes → GREEN.

### 4.4 Tie-in to the live jobs

27/41/42/32 are the end-to-end proof (currently RED → GREEN). 13/35/22 are the
guard rail (must stay GREEN — and *are* unaffected: a node with no resident baby
/ no persistence still hits the unchanged no-op gates at the top of
`student_dmn_consolidate`, so its `.text`/behaviour for those jobs is untouched).

---

## 5. Relationship to ② full SMP (honest scope)

The real long-term answer is ② full SMP: a dedicated **thinking core** running
consolidation in parallel with a **serving core** handling console/SWIM/net, so
heavy math never competes with responsiveness at all. This cooperative-yield fix
is **not a replacement** for that — it is the correct fix for the *single-
threaded cooperative port* (where there is exactly one core and yields can only
happen at safe points) and a clean **stepping stone**: it removes the starvation
without needing real parallelism, and its batch/checkpoint state machine is the
same bookkeeping a future thinking-core would own. Do not scope-creep this lane
into SMP.

---

## 6. Exact loci

| Locus | Change |
|---|---|
| `arch/common/dmn.c:265` (and dmn.c overall) | **NO CHANGE** (crown). Call site stays `... && student_dmn_consolidate()`. |
| `arch/common/llm/student_shell.c:270-275` | add `ST_DMN_PASS_BUDGET` (K) + file-static cursor (`g_consol_active/idx/total/seqlen/trainw/lr`). |
| `arch/common/llm/student_shell.c:324-340` (`sleep_rounds`) | add resumable variant `sleep_rounds_resume`; keep synchronous `sleep_rounds` for the two human callers. |
| `arch/common/llm/student_shell.c:555-618` (`student_dmn_consolidate`) | wrap in the cursor state machine: snapshot plan on batch start, run ≤K passes, persist + return 1 only at batch end, return 0 while draining. |
| `arch/common/llm/student_shell.c` (new exports) | `student_consol_busy()`, `student_consol_abort()`. |
| weight-replacing callers (`merge` :957, `st_load` restore, birth) | call `student_consol_abort()` before replacing `g_student` (verify exact sites). |
| new cert (hosted, e.g. a `student`/`dmn` subcommand or CI in-proc test) | Cert A + Cert B + `-DYIELD_DISABLE` falsifier. |

---

## 7. Honest risk list

1. **(Biggest) Proving sliced == all-at-once byte-identical.** Mitigation: Cert A
   memcmp of the full `st_save` blob (weights + Adam moments + `adam_t`); corpus
   snapshot at batch start (3.3); persist only at batch end; never yield mid-pass.
   The whole design hangs on the update list being identical in order/operands —
   if any of those three guards slips, `fpdet` will (correctly) block.
2. **Not breaking 13/35/22.** They reach the unchanged no-op gates (no baby / no
   persistence) → behaviour and `.text`-path unchanged. Must be re-run GREEN.
3. **Log / galaxy / count-cert cadence.** "Return 1 only on batch completion"
   keeps one sleep-line + one `EV_CONSOLIDATE` per consolidation (3.4); verify no
   cert asserts an exact count that the slicing perturbs.
4. **Writer interleave** (Path-W `merge`, `st_load`, birth mid-batch). Mitigation:
   `student_consol_abort()`; **verify every site that replaces/mutates `g_student`
   is wired** (a cert-must-cover-all-paths concern — enumerate them).
5. **Brand-new lesson latency.** A lesson arriving mid-batch waits one batch
   (3.3). Acceptable; note it so no one reports it as a regression.
6. **K tuning.** Too large → still stalls; too small → batch drains slowly.
   Measure one triple in Cert B and set K to the ~50 ms budget; default K=8.
7. **Out of scope but adjacent:** R3 (`r3_consolidate_idle_round`) and lm
   (`lm_consolidate_idle_round`) also run per-pulse compute via `dtr_*` (a
   *different* net from `st_*`). The proven PC is in `st_*`, so this fix covers
   the four failing jobs; a sibling lane should confirm the R3/lm paths can't
   independently starve under a different scenario.
