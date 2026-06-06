# 18_breathing — R3b: breathing parameters (expert specialization)

`breathe.sh` is the numeric answer to **audit §4**:

> the MoE's expert = one node, but every node holds the *same* 635 parameters
> — so a cluster is not a *mixture*, it is *N identical copies*. With no
> specialization, "the most effective expert fires" cannot hold in principle.

This demo makes specialization real and shows, in numbers, that
**participation makes the network smarter and departure degrades it
gracefully**.

## What it shows

Run it:

```
./breathe.sh        # exit 0 = PASS (join smarter, leave graceful)
```

The kernel verb `breathe` (arch/common/spec.c) trains **four genuinely
different experts** on a deterministic, deliberately hard 3-class toy task,
then evaluates *teams* of experts over a held-out split:

```
control (all-same-weights, audit §4 old state): N copies == 1 -> 64.4%
=== JOIN (add band specialists) ===
  N=1 experts  overall 64.4%   per-domain[d0=90.0% d1=53.3% d2=50.0%]
  N=2 experts  overall 74.4%   per-domain[d0=90.0% d1=83.3% d2=50.0%]
  N=3 experts  overall 87.8%   per-domain[d0=90.0% d1=83.3% d2=90.0%]
  N=4 experts  overall 87.8%   per-domain[d0=90.0% d1=83.3% d2=90.0%]
=== LEAVE (kill 1 of 4: specialist for domain 1) ===
  before (4 experts)  overall 87.8%   per-domain[d0=90.0% d1=83.3% d2=90.0%]
  after  (killed d1)  overall 77.8%   per-domain[d0=90.0% d1=53.3% d2=90.0%]
```

- **CONTROL** — identical-weight copies (the old state) score the same as one.
  Adding nodes does nothing. This is the null hypothesis the rest refutes.
- **JOIN** — `1 < 2 < 4` (strict). Each band-specialist owns one region of
  input space; the §7 gate (`moe_gate_predict`) routes that region to it, and it
  learns *only* that region's rule. The mixture beats the lone generalist by
  ~23 points.
- **LEAVE** — kill the domain-1 specialist (1 of 4). The router reroutes domain
  1 to the generalist, so **only domain 1 falls** (83→53); domains 0 and 2 are
  untouched. Overall drops gracefully (no cliff), staying well above the
  single-copy baseline. That localized fallback *is* the graceful degradation
  of survival-network.md's path B.

When the sibling ABI build + qemu are available, the script runs it too and
asserts the join/leave numbers are **byte-identical across aarch64 ⇄ x86_64**
(float32 weights are LE/IEEE754 on every target).

## How specialization is made real

- **(a) data-shard specialization** — specialist *b* trains only on the samples
  the §7 gate routes to band *b* (its own region of input space).
- **(b) different initialization** — each expert starts from a different LCG
  seed (`dtr_reinit_weights`) and learns its own shard, so the four weight
  blobs are genuinely different (verify with `breathe save`: four distinct
  content hashes).

The task is designed so specialization is *necessary*: within each temperature
band the label follows a different cyclic rule of the same feature, so one
shared 635-parameter generalist must compromise across three incompatible
rules, while each single-band specialist fits its own.

## The breathing substrate (`breathe save`)

```
breathe        # train + run the join/leave demo
breathe save   # each expert -> its own content-addressed p-fs block dtr/expert/<k>
breathe stat   # summary of the last run
```

`breathe save` writes each expert as a separate **named p-fs ref**
(`dtr/expert/0..3`). Experts grow as content-addressed blocks on p-fs — the
*vessel* for "breathing parameters": a new device's expert is a new block that
joins additively; a dead device's block simply stops being routed to.

## Scope / honesty

The breathing is computed **deterministically inside one kernel** so the
numbers are reproducible and ABI-independent. Real distributed inference over
the relay is *not* exercised here — `net_relay.c` / `relay.c` / `dkva.c` are
owned by sibling work and out of R3b's scope. The in-kernel harness is the
numeric proof; `breathe save` is the distribution substrate that a real mesh
would load. Limits: toy data, #experts == #domains, `d_model` fixed (dense
width-widening — path A — is deliberately NOT taken; it would be
function-destructive). See `docs/architecture/r3b-breathing-params.md`.
