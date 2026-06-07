# 30_ark_crash — ARK crash-safety fuzzer (wave 15, ARK-fuzz隊)

A **format-agnostic, seeded, reproducible** fuzzer that tests ARK's central
claim *honestly*:

> "a fresh mount yields either the last committed version complete, or the new
> version complete, and **NEVER serves corrupt data**."

The existing crash test (`samples/25_survival_fs`, env `ARK_KILL_TORN` /
`ARK_KILL_BEFORE`) only models **prefix truncation** — a SIGKILL makes device
writes stop at *some* sector, nothing after it persists. Real storage is harsher:
a cache can **reorder** or **drop** the un-`fsync`'d tail, a sector can **tear**
(half-written), and media can **bit-rot**. The audit
(`docs/architecture/arkfs-audit.md`, 🟡6) called out a corner the prefix-only
harness *cannot reach*: a **torn payload + surviving header + surviving commit**
makes the current version read `ARK_E_CORRUPT` with **no auto-fallback** to the
last good version. This fuzzer reaches it, and more.

## How it stays honest and decoupled

- It **never links `arkfs.c`** and **never `#include`s `arkfs.h`**. It drives ARK
  only through the stable CLI verbs of the *existing* `samples/25` `arkfs_test`
  binary (`format/write/read/version/readv`). `run.sh` compiles that binary the
  same way `samples/25` does, so it always tracks the current `arch/common/arkfs.c`.
- Perturbation is **generic at the 512 B sector level**. The harness learns the
  "write-set" of a single ARK write by **snapshotting the image and diffing
  sectors** before/after — it knows *nothing* about ARK's record layout. The only
  ARK-shaped thing it looks for is **its own payload marker** inside changed
  sectors, to tell payload sectors from metadata sectors.
- All randomness is **seeded** (`ARK_FUZZ_SEED`, default 1337); same seed →
  byte-identical verdict. Two deterministic probes (superblock rot, first-header
  rot) run every time regardless of seed.

## Scenario

`format` → write **v1**, write **v2** (both durable; v2 = last committed) →
snapshot the committed image → write **v3** (the "crashing" write, ~600 B so its
payload spans >1 sector). Each perturbation is applied to *the committed image +
some subset/variant of v3's write-set*, then a **fresh process re-mounts** and
reads.

## Perturbation classes

| class | what it models |
|---|---|
| **(a) prefix-truncate** | writes stop at every sector boundary (the existing harness's only mode) |
| **(b) reorder / drop** | a cache reorders/loses an arbitrary subset of the un-fsync'd tail (incl. *drop a payload sector but keep the commit*) |
| **(c) torn-sector** | a sector half-written then power-lost; two variants: *stop after the tear*, and *tear an interior payload while the commit survives* |
| **(d) bit-flip** | media rot: random byte flips in the log, plus two deterministic probes (superblock sector 0, first log-record header sector 1) |

## Classification & gate

- **OK-new / OK-old** — mount serves a complete, self-verified version (new, or a
  prior committed one). The claim holds.
- **SAFE-reject** — current view withheld (`CORRUPT`/not-found) but **no corrupt
  bytes served** *and* a prior good version is still recoverable. Integrity kept;
  "always last-good" **not** kept. This is the 🟡6 corner.
- **BUG** — corrupt bytes served, **or** the store is wedged / silently empty
  (no good version obtainable at all).

`run.sh` exits **non-zero iff any BUG run** occurs. SAFE-reject does **not** fail
the gate — it is a tolerated, documented degradation. This is the regression gate
**ARK-2 must turn fully green**.

## Run

```sh
./samples/30_ark_crash/run.sh                 # seed 1337, 256-sector image
ARK_FUZZ_SEED=99 ARK_FUZZ_RUNS=200 ./samples/30_ark_crash/run.sh
```

## Results today (tip of this branch, seed 1337)

```
 PERTURBATION                 runs  OK-old  OK-new  SAFE-rej   BUG
 (a) prefix-truncate            6       5       1         0     0
 (b) reorder/drop              65      46      15         4     0
 (c) torn-sector                2       1       0         1     0
 (d) bit-flip                  66      11      39        12     4
 TOTAL                        139      63      55        17     4
```

### What ARK survives (cleanly)

- **(a) prefix-truncate — 0 BUG, 0 SAFE-reject.** Every prefix cut lands on
  v2-whole or v3-whole. This is exactly what `samples/25` already proves; we
  reproduce it as a baseline.
- **Self-verify is airtight: across *all* classes and seeds, ARK NEVER served a
  corrupt byte.** Every failure mode is *withholding* data, never mis-serving it.
  Content addressing (sha256 == block id) genuinely does its job.

### What this fuzzer EXPOSES (the prefix-only harness cannot)

1. **🟡6 — torn/dropped payload + surviving commit → no auto-fallback
   (SAFE-reject).** Reorder and torn-with-surviving-commit reproduce it
   deterministically.
   Reproducer (seed-independent, class **(b)**):
   ```
   op: reorder DROP payload sector 14, keep [13, 15, 16, 17]
   -> read current = CORRUPT;  version = 2;  readv 1/2 = intact.
   ```
   And class **(c)**:
   ```
   op: tear payload sector 14 but commit SURVIVES
   -> read current = CORRUPT;  prior v2 still recoverable.
   ```
   ARK keeps integrity (never serves the rot) but the *current view* is
   unreadable with no automatic rollback — **not** "always last-good." Fixing
   this (read-path auto-fallback to the newest intact version) is ARK-2's job.

2. **🟡5 — superblock rot wedges the entire library (BUG).** Sector 0 has a crc
   but **no replica**.
   Reproducer (deterministic):
   ```
   op: bitflip SUPERBLOCK sector 0 byte 10
   -> ark_mount returns CORRUPT -> MOUNT-FAIL -> total loss, unmountable.
   ```

3. **NEW finding — one rotted *header* byte silently truncates the whole log
   (BUG).** The mount scan **stops at the first invalid record header**, so a
   single bit-flip in an *early* record header discards every committed record
   after it. The store still "mounts" but presents an **empty** filesystem — a
   silent total data loss that looks like a fresh disk (arguably worse than the
   loud SB wedge).
   Reproducer (deterministic):
   ```
   op: bitflip FIRST log record header (sector 1 byte 0)
   -> read = NOTFOUND; version = -4; ls / = 0 entries.  All versions gone.
   ```
   Random bit-flips hitting any header sector at/before the latest commit
   reproduce the same truncation (e.g. seed 1337: `bitflip log [(1,41),...]`).

## Honest verdict on ARK's crash-safety today vs its claim

- **"NEVER serves corrupt data" — TRUE, and strongly so.** No perturbation in any
  class or seed ever produced a corrupt-served byte. The content-addressed
  self-verify on the read and replay paths is the real, load-bearing strength.
- **"last committed complete OR new complete" — only TRUE for prefix crashes.**
  Under realistic **reorder / drop / torn-with-surviving-commit**, ARK degrades to
  **SAFE-reject**: the current view becomes unreadable with **no auto-fallback**
  to the last good version (🟡6). It is *safe* (no corruption) but *not*
  "always last-good."
- **"survives the flood (bit-rot)" — FALSE for two single-byte cases.** With **no
  superblock replica** and a **scan-stops-at-first-bad-header** replay, a single
  rotted byte in sector 0 (wedge) or in any early record header (silent empty
  library) destroys the whole store. These are the gating BUGs.

**Bottom line:** ARK is *integrity-safe* (it never lies about data) but not yet
*availability/durability-safe* under reordering and rot. The gate is **RED by
design today** — it documents real defects (🟡5, 🟡6, and the header-truncation
finding). ARK-2 turns it green by adding (a) read-path auto-fallback to the
newest intact version, (b) a superblock replica, and (c) a replay that can skip a
rotted header instead of truncating the log.
