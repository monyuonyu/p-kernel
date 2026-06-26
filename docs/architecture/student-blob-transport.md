# Student-Blob Transport Redesign — making SS-3 `[live]` achievable

Status: DESIGN (no code). Target: replace the dead, structurally-broken
`gl_student_publish` / `gl_student_fetch` chunk-by-name transport with a
content-addressed manifest transport so a multi-MB student blob can move
peer↔peer over `./relay` and end in a real `st_merge_cohort`.

Crown baseline this design must preserve: bare-metal `.text` (the `755a20fa`
crown). Re-derivation recipe in §4.

## 0. The verified problem (ground truth)

`gl_student_publish` (`arch/common/gossip_learn.c:202`) saves ONE NAMED p-fs
ref per 4096-B chunk via `pfs_dag_save((const UB*)"st/<node>/<c>", …)`
(`gossip_learn.c:213-214`). The named-ref table is `PFS_REF_MAX=16`
(`arch/common/include/pfs_dag.h:62`; the table is `static PFSD_REF
refs[PFS_REF_MAX]` at `pfs_dag.c:170`). The smallest blob, S-tier
1,973,036 B, is `ceil(1973036/4096)=482` chunks + 1 header = **483 distinct
names** → `ref_alloc` returns NULL at the 17th name and `pfs_dag_save` returns
`PFS_E_FULL` (`pfs_dag.c:461`).

Confirmed secondary facts:
- `GL_ST_MAXCHUNK=8192` (`gossip_learn.h:133`) contradicts the 16-slot table —
  the publish loop's own cap is 512× larger than what the name table can hold.
- Zero callers of either function anywhere in the tree (grepped
  `--include=*.c --include=*.h`: only the defs + the header prototypes).
- The `pfs/ref` gossip plane advertises `PFSD_REF_PER_PKT=3` names per
  `PFSD_BEACON_MS=800` beacon (`pfs_dag.h:73-74`) → 483 names ≈ 128 s just to
  advertise S (and the table can't hold them anyway).
- The P1 content-announce topic is `KDDS_QOS_LATEST_ONLY`; a multi-block put
  burst clobbers all-but-one announce per poll (the G35 note, `pfs_repl.h`).

Logged OPEN in `docs/audit-trail.md` ("SS-3 `[live]` transport — BLOCKED").

## 1. Transport architecture

### 1.1 The shape (recommended, validated against the codebase)

Reuse the **content-addressed block + replication** layer that already exists
(`pfs_repl_put` → `content_id`, `pfs_repl_want`/`pfs_get`); do NOT mint a new
wire protocol and do NOT put per-chunk names on the ref plane.

```
weight bytes ──split 4 KB──► chunk[0..nchunk)   each: pfs_repl_put → cid (NO name)
                                   │
                          ordered list of cids
                                   │
                    ┌──────────────┴───────────────┐
                    │  INDEX layer (cids → blocks)  │   pfs_repl_put → leaf/root cid
                    └──────────────┬───────────────┘
                                   │  root_id
                          DESCRIPTOR block (small)        pfs_repl_put → desc_id
                                   │
                       ONE named ref "st/<node>"          pfs_dag_save (1 name)
```

The fetcher reads the **single** named ref `st/<node>` (→ 1 manifest → 1
content block = the descriptor), then pulls the index and the chunks by
content-id directly through `pfs_repl_want`/`pfs_get`, **bypassing the
per-chunk name plane entirely**. This is exactly the existing P1 path the
header already advertises: `pfs_repl_want` is documented as "Used by the P2
DAG layer to fetch a manifest learned from a ref beacon" (`pfs_repl.h`) — we
use it the same way, just for many ids.

Why this is the right reuse, not an invention: `pfs_dag_read` only ever returns
**one** content block ≤`PFS_BLOCK_MAX` (`pfs_dag.c`, `pfs_get(..., maxlen)`),
and the version DAG (`prev` chain) is HISTORY, not value-splitting. There is
**no** existing multi-block/indirect content mechanism in `pfs_*` — so the
index/indirection must be built, but built ON TOP of `pfs_repl_put`/`pfs_get`,
which already replicate any content-addressed block for free.

### 1.2 The manifest-can't-fit-one-block sub-problem (indirection)

`PFS_ID_LEN=32`, `PFS_BLOCK_MAX=4096`. With a 16-B index header,
`IDS_PER_INDEX = (4096-16)/32 = 127` ids per index block.

S-tier manifest = 482 ids × 32 B ≈ 15 KB ≫ 4096 → it MUST be chunked. Two-level
index:

- **Leaf index block** `{magic, level=0, count}` + `count`×32-B chunk-ids.
  S: `ceil(482/127) = 4` leaves. M: `ceil(5577/127) = 44` leaves.
- **Root index block** `{magic, level=1, count}` + `count`×32-B leaf-ids.
  S root holds 4 leaf-ids; M root holds 44 leaf-ids — both ≤127, so the root
  is a **single block** for both S and M.

On-wire layout (all little-endian, fixed-width `UW`/`U1` per the LP64 wire
discipline used throughout `pfs_*`):

```
INDEX block:  UW magic(=GL_ST_IDX_MAGIC) | UW level | UW count | UW _pad
              | U1 id[count][32]                            (≤ 16 + 127*32 = 4080 B)

DESCRIPTOR:   UW magic | UW version | UW tier | UW total_len
              | UW nchunk | UW depth | U1 root_id[32]       (≈ 56 B, one block)
```

`depth` is the number of index levels the reader walks (0 = root_id points
straight at a single flat index when `nchunk ≤ 127`; 1 = root→leaves→chunks).

Indirection-depth bound: depth-2 (root + leaves) covers `127 × 127 = 16129`
chunks ≈ 63 MB → **covers S (482) and M (5577) with one shared design**.
L-tier (~60330 chunks) needs depth-3 (`127³ ≈ 2.0 M` chunks) — deferred,
documented in §6, not silently broken (publish refuses when `depth > 2`).

The descriptor lives under ONE name `st/<node>` (`"st/" + ≤3 digits` ≤ 6 chars
≤ `PFS_NAME_MAX=16`). This collapses 483 names → **1 name**, the whole point of
the redesign.

## 2. p-fs replication-plane reliability

### 2.1 Does the WANT path scale to 482 outstanding wants? No — and it must not be relied on to.

`PFSR_PENDING_MAX=8` (`pfs_repl.h`). `pending_add` **silently drops** when the
8-slot table is full ("table full: drop — the next re-announce will retry us").
So firing 482 raw `pfs_repl_want` calls populates only 8; the other 474 are
lost, and there is no ambient re-announce to recover them (the publisher
already finished its put burst). A naive fetch would hang.

**Fix lives entirely in `gossip_learn.c`** via *manifest-driven, windowed,
explicit want* — never relying on the passive 8-slot table to remember
hundreds of ids:

1. Build the full ordered chunk-id list (from the index).
2. Loop in passes. Each pass: scan the not-yet-held ids; for the first `W`
   (`W ≤ PFSR_PENDING_MAX`, e.g. 6) that are missing, call `pfs_repl_want(id)`
   (which dedups and `pfs_has`-short-circuits); yield a poll interval; re-scan.
3. As chunks arrive, `pending_tick` clears their slots, freeing the window for
   the next batch; `pending_tick` also re-WANTs un-arrived ids up to
   `PFSR_WANT_TRIES=10`, so transient loss self-heals.
4. Bounded overall budget: cap total passes; if any chunk is still missing when
   the budget is exhausted, **fail closed** (return `<0`), never truncate.

This needs **no change to `pfs_repl.c`** — the 8-slot table is sufficient
because the transport paces itself to the window instead of dumping 482 wants
at once.

### 2.2 Does the LATEST_ONLY announce clobber lose chunks? Irrelevant by construction.

The G35 clobber only harms parties that rely on **passive ANNOUNCE
discovery**. This transport does **not**: every chunk/leaf/root id is known to
the fetcher from the descriptor + index, and is pulled by **explicit WANT**. A
clobbered announce changes nothing because we never wait for one. A clobbered
WANT (the `pfs/want` topic is also LATEST_ONLY) self-heals via `pending_tick`
retry. The actual bytes move on the **private `PFSR_PORT` UDP path**
(`PFSR_BLK_PKT`), which is not a LATEST_ONLY slot.

### 2.3 Verdict on `pfs_repl.c`

`pfs_repl.c` (bare-metal, crown-sensitive) needs **NO change**. The entire fix
is the new manifest transport in `gossip_learn.c` + the windowed-want driver.
This deliberately avoids bumping `PFSR_PENDING_MAX` (which would change the
bare-metal `.text` and the static `pending[]` size) and avoids touching the
LATEST_ONLY topic QoS.

### 2.4 The 64-slot P0 store — receiving nodes need the ARK durable backend (found + audit-confirmed during steps 1-2 impl)

The original draft of this doc (§0/§5/§6) addressed only the `PFS_REF_MAX=16`
name explosion and transient heap — it MISSED a second hard wall: the in-memory
P0 content store is `PFS_MAX_BLOCKS=64` (`pfs_block.h:29`). `pfs_put` returns
`PFS_E_FULL` once all 64 slots are live with no safe eviction. An S-tier blob is
**482 content blocks** — it cannot reside in a memory-only store; a naive
receiver drops blocks at #65. This was found while implementing the in-proc cert
(steps 1-2) and independently audit-confirmed (real, not a cert artifact).

Resolution (already in the shipped cert, no crown change): mount the
**eviction-capable ARK durable backend** — RAM stays a 64-slot cache, every
block is also written to the ARK log, and `pfs_get` falls through to ARK on a
cache miss with a re-hash verify (all in the UNTOUCHED `pfs_block.c`). The
fetcher therefore probes presence with `pfs_get(id, 0, 0)` (returns the stored
length, ARK-aware) and **NOT** `pfs_has` (which only sees the 64-slot RAM
cache). Content-addressing keeps recovery bit-exact across memory/ARK.

**Load-bearing consequence for step-3 `[live]`:** every node that RECEIVES a
multi-MB blob (not just the cert) must have the ARK backend mounted, or it will
silently drop chunks past #65 and `gl_student_fetch` will (correctly) fail
closed. The step-3 harness must mount ARK on all peers and assert it.

## 3. In-proc cert FIRST — `[ss3-blob-roundtrip]` (the gate)

This must PASS before any `[live]` harness is attempted.

Key enabling fact (verified): in SOLO mode (`drpc_my_node == 0xFF`)
`pfs_repl_put` stores the block **locally only** — the put-hook early-returns,
`publish_announce`/`publish_want` early-return, and `pfs_repl_want`
early-returns while `pfs_has` stays true because the same process stored the
block. So **the full publish→fetch round-trip runs in ONE process with zero
network**. And because the transport is pure byte plumbing (no float math),
recovery is **bit-exact**, so `memcmp==0` is a real, non-flaky assertion.

Cert design (hosted build, `_TK_HOSTED_LIBC_` defined):

- Link set: `student.c` + `pfs_block.c` + `pfs_dag.c` + `pfs_repl.c` +
  `gossip_learn.c` + a SOLO shim that sets `drpc_my_node = 0xFF` and provides
  benign stubs for the few `kdds_*`/`pmesh_*` symbols referenced by
  `pfs_repl_init` (topics are never opened/used in SOLO). Mirror the in-proc
  assertion style of `tests/llm/student_cohort_test.c` and `run_ss3.sh`
  (`-O1 -ffp-contract=off -Werror=vla`, greppable `[tag] PASS/FAIL`).

Assertions:
1. `[ss3-blob-roundtrip]` — `st_init_tier(&A, seed, ST_TIER_S)`, train a few
   steps, `st_save(&A, blob, cap)`; `gl_student_publish(node, blob, len)` →
   returns `nchunk≥0`; `gl_student_fetch(node, out, cap)` → returns `len`;
   `memcmp(blob, out, len) == 0` (byte-identical recovery, 482 chunks + index
   reassembled).
2. Feed `out` into a second resident S model: `st_blob_tier_ok(&B, out, len)`
   true; `st_merge_cohort(&B, {out}, {len}, 1)` accepts (1) and the in-proc
   SS-3 convergence/symmetry property from `student_cohort_test.c` still holds
   (merged loss ≤ worse parent; `merge(A,{B})` bytes == `merge(B,{A})` bytes).
3. Falsifier `[ss3-blob-roundtrip-falsify]` — corrupt one chunk-id byte in a
   leaf index (or drop one chunk block before fetch): `gl_student_fetch` MUST
   return `<0` and leave `out` unconsumed (fail closed, never a truncated
   model). Mirrors the falsifiable spirit of `[ss3-merge-falsifiable]`.

`st_merge_cohort`/`st_blob_*`/`st_save` are used **unchanged** — this cert
proves the *transport*, then hands off to the existing crown math.

## 4. Crown safety

### 4.1 What the change touches

| File/symbol | Bare metal? | Action |
|---|---|---|
| `gossip_learn.c` `gl_student_publish`/`gl_student_fetch` | YES (compiled into boot/x86 + boot/aarch64) | Rewrite, **hosted-gated** |
| `gossip_learn.h` `GL_ST_*` defines + descriptor/index structs | header | Add (no `.text` effect) |
| `pfs_repl.c` / `pfs_dag.c` / `pfs_block.c` | YES | **UNTOUCHED** |
| `student.c` `st_merge_cohort`/`st_forward`/`st_blob_*` | YES | **UNTOUCHED** |
| G22 `gl_pfs_publish`/`gl_merge`/`gl_merge_w` mesh paths | YES | **BYTE-UNCHANGED** |

Verified: no `--gc-sections` in the kernel link (`boot/x86/Makefile`,
`boot/aarch64/Makefile` — plain `-nostdlib -static -T … .ld`), so the
currently-unused `gl_student_*` bodies ARE present in the bare-metal `.text`.
A naive rewrite would therefore change `755a20fa`.

### 4.2 The hosted gate

`_TK_HOSTED_LIBC_` is defined **only** for the relay-capable linux boots
(`boot/linux/Makefile`, `boot/linux_x86_64/Makefile`) and **not** for bare
metal. The `[live]` target runs `./p-kernel` (a linux boot), so the gate is
exactly aligned with where this transport ever executes. Structure:

```c
#ifdef _TK_HOSTED_LIBC_
    /* NEW manifest transport (chunk → index → descriptor → 1 named ref) */
#else
    /* the OLD gl_student_publish/_fetch bodies, byte-for-byte UNCHANGED */
#endif
```

Keeping the `#else` branch token-identical to today guarantees the bare-metal
preprocessor output — hence `.text` — is unchanged. Any new `static` scratch
(index/reassembly buffers) goes **inside** the `#ifdef _TK_HOSTED_LIBC_` so
bare metal gains no symbols. (Use the established pattern: `galaxy.c`,
`genome.c`, `lm_self.c` all gate hosted-only code this way.)

### 4.3 Re-derivation recipe (run for BOTH bare-metal arches; must match the baseline)

```
cd boot/aarch64; git clean -fdx .; make
objcopy -O binary -j .text kernel.elf /tmp/x.bin; sha256sum /tmp/x.bin
# repeat for boot/x86 (kernel.elf), compare each to the recorded 755a20fa baseline
```

Expected: identical to the pre-change `.text` sha256 for both arches.

## 5. Sizing / memory

- **S tier is the right first `[live]` target.** S blob = 1,973,036 B
  (482 chunks); descriptor + 4 leaf index blocks + 1 root ≈ 7 extra blocks.
  Resident model ≈ 2.6 MB/proc (w+mu+vu, `st_blob_size`) × 3 procs —
  comfortable on hosted linux nodes.
- **Transient fetch memory** (hosted heap, not bare metal): one reassembly
  buffer = `total_len` (≈1.97 MB for S) + one index-block scratch (4096) + one
  chunk scratch (4096) ≈ **~2.0 MB** transient on the fetcher. Static scratch
  must be `static` (not task stack) per `feedback_hosted_relay_stack_overflow`
  / the existing `gl_st_chunk`/`gl_st_hdr` discipline (`gossip_learn.c`).
- **M tier**: depth-2 index covers it (44 leaves), but transient reassembly is
  ~22 MB and the want loop is 5577 chunks → heavier; viable on a PC/phone but
  recommended as a follow-up after S `[live]` is green (§6).

## 6. Honest bounds + open risks

**What this solves**
- The `PFS_REF_MAX=16` name explosion → exactly **1** named ref (`st/<node>`).
- The `pfs/ref` advertise-time blowup → 1 ref to gossip instead of 483.
- The LATEST_ONLY announce clobber → made irrelevant (explicit manifest-driven
  WANT, not passive discovery).
- Reliable arrival of hundreds of blocks without touching the crown
  (`pfs_repl.c`) — windowed want + content-addressed self-checking transfers.

**What it does NOT solve**
- The `pfs/want` topic is still a single LATEST_ONLY slot; under high
  concurrent fan-in many simultaneous wants can clobber. Mitigated by
  `pending_tick` retry, not eliminated. Fine for a 3-proc S cohort; a larger
  fleet may need a per-source want topic (out of scope).
- The relay is broadcast-only with a cold-ARP first-unicast drop
  (`feedback_live_forward_cold_arp`): the first WANT/CHUNK to an un-ARPed peer
  may be lost. Covered by retransmit/retry, but it is a real `[live]` latency
  tax on the first round.

**Biggest risk that could sink the implementation**
The `[live]` convergence-time / want-storm: 482 chunks pulled through an
8-deep window with 600 ms retries and cold-ARP first-drops can take many
seconds and, worse, can *stall* if the window-pacing + `PFSR_WANT_TRIES=10`
budget interact badly (a chunk that loses its first 10 WANTs to clobber/ARP is
given up on → fetch fails closed but the cohort never converges). Mitigation to
prove in the harness: ss6-style retransmit + a per-chunk retry budget that
resets while *forward progress* is observed, not a flat global cap.

**M vs S**: M is structurally feasible (same depth-2 index, same code path) but
should be a documented follow-up; ship **S-only** for the first `[live]` green.

## 7. Impl sequencing

1. **In-proc cert first** (`[ss3-blob-roundtrip]` + falsifier, §3) — SOLO,
   single process, `memcmp==0` then real `st_merge_cohort`. Gate.
2. **Transport** — rewrite `gl_student_publish`/`gl_student_fetch` inside
   `#ifdef _TK_HOSTED_LIBC_` (chunk→index→descriptor→1 ref + windowed-want
   fetch), bare-metal `#else` branch byte-identical; re-derive `755a20fa` for
   both arches (§4.3).
3. **`[live]` harness** — 3 hosted procs over `./relay`, S-tier, publish on A,
   fetch+`st_merge_cohort` on B and C, assert cohort convergence/symmetry and
   fail-closed on a dropped chunk. **Mount the ARK durable backend on every
   receiver (§2.4) or chunks drop past #65.** Watch the want-storm/convergence-
   time risk (§6): use ss6-style retransmit + a progress-reset retry budget.

## Status (2026-06-26)

- **Steps 1+2 SHIPPED + independently audited MERGEABLE** (merge `e655539d`,
  impl `43b2ad3f`, audit-trail "SS-3 student-blob transport (steps 1+2)").
  Content-addressed transport rewritten hosted-gated; in-proc
  `[ss3-blob-roundtrip]` cert 12/12 (482-chunk depth-2 S blob round-trips
  `memcmp==0` → real `st_merge_cohort`; falsifier proven load-bearing). Crown
  `755a20fa` (aarch64) / `4064d8a9` (x86) byte-identical, re-derived on trunk.
- **Step 3 (`[live]` harness) REMAINS** — host-heavy multi-process; a
  prioritization call (vs federation F1). Carries the §2.4 ARK requirement.

## Critical files
- `arch/common/gossip_learn.c` (gl_student_publish/_fetch:202/228 — the rewrite, hosted-gated)
- `arch/common/include/gossip_learn.h` (GL_ST_* defines:133 + new descriptor/index structs)
- `arch/common/pfs_repl.c` (pfs_repl_put / pfs_repl_want / pending — REUSED, unchanged)
- `arch/common/llm/student.c` (st_merge_cohort:1925 / st_blob_*:1776-1850 / st_save — crown, untouched)
- `tests/llm/student_cohort_test.c` + `tests/llm/run_ss3.sh` (in-proc cert pattern to mirror)
