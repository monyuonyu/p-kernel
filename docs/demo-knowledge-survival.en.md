# Demo: kill the devices — do the taught records survive? (draft)

> Draft (2026-10-25), an English rendering of [demo-knowledge-survival.md](demo-knowledge-survival.md) (2026-10-24). The numbers come from the v4 re-run in `docs/architecture/30-module/knowledge-survival.md` (2026-10-23) and the re-measurement in audit-69. Before this goes out on 11/30, a separate run audits the numbers and the wording.

## What was done

Eight p-kernel nodes run on one machine and form one swarm. Four of them each teach one fact (sea→north, stone→sweet, leaf→loud, ink→fresh). After the facts have spread, 2, 4 or 6 of the 8 nodes (f = 0.25, 0.5, 0.75) are killed with `kill -9`. Which nodes die is a seeded random choice, and the teachers can be among them. For every survivor we count, over the four facts, the **record** (whether `mind provenance` holds "key -> taught value"), and separately the **recall** (whether it answers correctly when asked).

## Baselines (computed for the same kills)

- **R1M**: no replication. The record lives only on the teacher
- **R3M**: plain 3-way replication, on the teacher and the next 2 node IDs
- **Broken build**: receivers drop the teaching they receive (`-DKS_NO_REMOTE_TEACH`). In the real swarm it should fall to R1M

Every number is the share of "survivors × 4 facts" that hold the record (held_mean).

| Killed share f | p-kernel | Broken build | R1M | R3M |
|---|---|---|---|---|
| 0.25 (2/8) | 1.0 / 1.0 | 0.125 / 0.125 | 0.125 | 0.292 / 0.458 |
| 0.5 (4/8) | 1.0 / 1.0 | 0.125 / 0.125 | 0.125 | 0.375 / 0.5 |
| 0.75 (6/8) | 1.0 / 1.0 | 0.125 / 0.125 | 0.125 | 0.375 / 0.5 |

Each cell is seed 1 / seed 2 (a different set of killed nodes), one run per seed. Audit-69 re-measured seed 2 at f=0.25 and f=0.75; the record numbers matched to the last digit.

How to read it: with 6 of 8 killed, the 2 survivors still held the records of all four facts. That is more than placing 3 copies (R3M), and the build that stops the spreading fell to "each teacher keeps only its own" (R1M) — what survived, survived because of the spreading.

## What it does not show yet

- **Recall wobbles.** A node can hold the record and still answer wrongly when asked (mostly ink→fresh answered as near or blue). The share of correct answers over survivors × facts was 0.79–1.0 over 8 runs of the normal build (6 pairs of 3 f × 2 seeds, plus 2 of those re-run). The same pair changes from run to run (f=0.25 seed 2: 0.958 and 0.792). The record is the original and the answering model is the librarian; the librarian is still the weak side
- Small: 8 nodes, 4 facts, 1 machine, 1 relay. Two seeds, one run each (2 pairs re-run). Partitions (the network splitting) are not tested
- All kills happen at once. Recovery time under continuous churn and long-term consistency are not measured yet

## Run it yourself

`tests/host/run_knowledge_survival.sh` (Linux x86_64; environment variables `KS_F` and `KS_SEED`). For the broken build, run `make -C boot/linux_x86_64 clean`, then build with `make -C boot/linux_x86_64 EXTRA_CFLAGS=-DKS_NO_REMOTE_TEACH` and run with `KS_NC=1`. To go back to the normal build, `clean` and build again: the Makefile does not notice a change of flags, so without `clean` the old binary keeps running. One run takes 3–9 minutes.
