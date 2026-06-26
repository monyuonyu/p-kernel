---
name: moment_2026_06_26_mind_learns_across_wire
description: "cradle-live core PROVEN — a fresh student learns a relay-delivered fact across the wire; 3 [live]-exposed bugs (L1/L2/L3) fixed+audited+integrated."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

2026-06-26 — the flagship "the mind learns across the wire" is PROVEN end-to-end
and integrated to local trunk (tip fc0428af; crown 755a20fa byte-identical; push HELD).
The formal multi-node VERDICT now prints GREEN host-independently (autoprobe-cert
6d491756, independently audited MERGEABLE — see the closing paragraph).

Driving the cradle-live [live] CURE to green exposed THREE real, in-proc-masked
production bugs in a row — the 2026-06-20 harsh-review prediction validated a THIRD time
([live] keeps finding what the in-proc "safe half" hides). Each: separate impl→audit→integrate.

- **L1 lesson-format** (a8fce20f): the live lesson body was ~115 B < CRADLE_MIN_LIVE=128
  (the harness's 1280-B string was truncated by the kernel shell line-buffer when passed as a
  `cradle emit <text>` arg) → `cradle_lesson_ingest` refused it, ring stayed 0. Fix: a
  `cradle emit-canon` verb composes the lesson IN-KERNEL via `cradle_compose_canon()`
  (byte-identical to the cert's `ct_build_lesson`, 1280 B). One lesson format, one math.
- **L2 DMN-stack** (ba450a02): `st_forward`/`st_backward` overflow an 8 KB task stack, so the
  AUTONOMOUS DMN sleep (`dmn_task` prio 13) + galaxy_task + the init/shell task crashed
  (EXIT 139) the moment they trained — LATENT because every prior living-mind cert drove the
  math from a host 8 MB stack, never the in-kernel 8 KB task. Fix: 256 KB for the 3 hosted
  tasks that reach st_forward (SS-6 precedent). The living-mind's sleep can now actually run
  on a real in-kernel node.
- **L3 beacon-vs-ref race** (d8eb6710): the LATEST_ONLY beacon outruns the lossy/lagging
  region "pfs/ref" gossip, so `pfs_dag_read(newest ct/<t>/<seq>)` returns NOTFOUND at
  `ref_find` though S has earlier resolvable refs + the identical content. Crown-safe fix in
  cradle_net.c (the ref-gossip lives in bare-metal pfs_dag.c/pfs_repl.c — NOT touched): scan
  seq downward to hw+1, pull the newest RESOLVABLE seq (+ an honest OFF-gate restoration —
  Arm A had passed only vacuously while the pull was broken).

PROOF: `fallback: pulled resolvable seq=6 (beacon seq=11)` → `ingest ring_len=1279` → the
autonomous DMN consolidates → held probe **5.5915 → 2.6025** (~2.99 nats below chance,
generalization: trainer [0,train_end) disjoint from probe [train_end,...)). Falsifiers honest
(OFF ring0/chance, SCRAMBLE ring-fills-stays-chance, DEATH still-answers). All three audited
MERGEABLE; crown re-derived byte-identical each time.

FORMAL VERDICT NOW GREEN (host-independent): the `baby 16` (16 sync rounds over a ~247MB
student) timed out the 180s cap on the cooperative-single-core PRoot host (post-probe starved),
so the autoprobe-cert (merge 6d491756, shell-only, independently audited MERGEABLE) re-points
the cure/scramble/death arms at the student's OWN autonomous DMN idle probe (the production
sleep): VERDICT `PASS held probe 5.5915->2.6025 over the wire; teaching-OFF / scrambled stayed
at chance; the mind survived the teacher's death`, DONE_RC=0. The auditor refuted the fallback-
masking concern at SOURCE (single emit site student_shell.c:674; monotone training ⇒ tail -1
reads worst-case for the scramble assert ⇒ conservative) and witnessed its own clean GREEN
first-try. REMAINING (re-confirm only, NOT a gate): a real-NAT / real-ThinkPad re-run; the
earlier `baby 16` harness loaded the ThinkPad → SSH-unreachable, mk_pino to clean stray procs.

METHOD LESSONS: (1) LOCAL multi-process reproduction beat TWO confident-but-wrong static-
analysis agents on L1 (they blamed region-scope/RTT; the real bug was a too-short body) — when
static analyses disagree, reproduce empirically. (2) The same [live] symptom (ring stays 0)
had THREE distinct causes across the layers; peel one, re-run, find the next. (3) crown-safety
is the gate that kept all 3 fixes in the hosted layer (cradle_net.c / usermain.c / inittask
override) — the bare-metal ref-gossip + the student math are crown, so the robustness lives in
the hosted cradle layer. See [[feedback_live_forward_cold_arp]] (same passive-drop pattern),
[[feedback_development_method_is_the_life]], [[project_living_mind_vision]].
