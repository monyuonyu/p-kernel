# ARK filesystem — skeptical design audit (wave 15, FS-audit隊)

> **状態更新 (2026-06-07, 波15–16 で解消済み):** この監査が挙げた 🔴3 ブロッカーは
> その後すべて修正された。**(1) 256ブロック上限** → ARK-1 で媒体スケールの open-addressed
> ハッシュ index へ撤廃（+ ARK-2 で P0=64 のエンドツーエンド上限も有界キャッシュ化、カーネル
> 経路で 400 ブロック実証）。**(2) GC 無し** → ARK-2 で crash-safe な `ark_compact()`。
> **(3) ベアメタル未配線** → ARK-3 で x86(ide)、ARK-4 で aarch64(自作 virtio-blk) に実配線し、
> QEMU で電源断生還を実証。さらに 🟡 の SBレプリカ・読みfallback・header resync も ARK-2 で実装し、
> 崩壊フ ァザー(`samples/30_ark_crash`)が **0 BUG / 0 SAFE-reject**（CI `ark-crash-fuzzer` で強制）。
> 残: 実 RPi3 の SD/EMMC、erasure coding(p-fs P4)、Merkle dir tree、`ARK_MAX_FILES=32`。
> **以下の本文は修正前の評決で、歴史記録として原文のまま残す。**

> The owner's question, verbatim: *"the new filesystem we R&D'd — ~800 lines —
> is it really good? audit whether that design is actually right."*
> This document is the answer. It is deliberately the skeptic's view, not a
> celebration. Where ARK is genuinely good it says so; where it is a convincing
> demo rather than a filesystem you would trust to "survive humanity," it says
> exactly where and why, with `file:line` evidence and reproduced behaviour.

Audited tree: `master` tip `2e4381c`, branch `w15-arkfs-audit`.
Sources read in full: `arch/common/arkfs.c` (885 lines), `arch/common/include/arkfs.h`
(185), `arch/linux/pfs_ark.c` (238), `arch/common/pfs_block.c` (the ARK seams),
`samples/25_survival_fs/`, `samples/26_ark_backend/`, and the spec
`docs/architecture/survival-fs.md` against `survival-network.md §9`.

Both sample suites were built and run on this host: **25 → ALL PASS**,
**26 → RESULT: PASS**. The audit takes those passes as true and asks the harder
question: *do they test what survival actually requires?*

---

## Headline verdict

**The architecture is right. The shipped artifact is not yet a survivable
filesystem — it is a convincing hosted demo of the right ideas.**

The five core decisions — log-structured / append-only, content-addressed
(`block-id = sha256`), atomic commit, per-block self-verify (crc32 + sha256),
and epoch fencing — are individually sound and correctly implemented. Crash
rollback for *acknowledged* writes is real and well-tested. The p-fs id-space
unification (§7) is genuine, not aspirational.

But as it stands ARK is a **~1 MiB toy store** (a hard 256-block ceiling
unrelated to disk size), it **has no garbage collection** so it is guaranteed to
die by `ENOSPC` on any long-lived node, its **superblock is an un-replicated
single point of failure**, and — the sharpest gap against the vision — it is
**not actually invoked on the bare-metal RPi target it exists to save**. The
durable wiring lives entirely behind `_TK_HOSTED_LIBC_`; on real hardware ARK is
dead code and p-fs stays memory-only.

Recommendation: **keep the design, fix these N things** (ranked below) before
ARK can honestly claim the "滅びない図書館 / survive-and-reboot-with-memory"
property. Three of the N are blocking.

---

## 🔴 Critical findings

### 🔴1 — A 256-block hard ceiling, unrelated to image size. Reproduced.
`ARK_MAX_INDEX = 256` (`arkfs.h:65`) is the in-memory `g_idx[]` capacity
(`arkfs.c:159`). `ark_block_put` appends the block to disk first
(`emit_record`, `arkfs.c:505`) and only then calls `idx_add`, which returns
`ARK_E_FULL` once 256 slots are used (`arkfs.c:178-189`). I reproduced it on a
**4 MiB** image (8192 sectors), writing 300 distinct versions of one file:

```
write #257 FAILED rc=1 out=WRITE: -6 ver=256     (-6 == ARK_E_FULL)
BLOCKS: 256        VERSION: 256
```

The store is full at **256 distinct blocks ≈ 1 MiB of content, forever**, on a
device with 4 MiB free. This directly contradicts "Keep-everything / the library
that does not perish": you cannot keep everything in a 1 MiB ceiling. Worse, the
caps are **internally inconsistent**: `ARK_MAX_FILES=32 × ARK_MAX_BLK=16 = 512`
possible block references, but only 256 index slots — you cannot even fill 32
max-size (64 KiB) files. The replay path silently ignores `idx_add` failure
(`arkfs.c:470-471`), so a too-large image just loads the first 256 blocks and
drops the rest with no diagnostic.

### 🔴2 — No GC / compaction → guaranteed eventual `ENOSPC` death.
The log only ever grows. **Every** mutation appends a full `REC_COMMIT`
snapshot (`commit_live`, `arkfs.c:354-362`), and on the p-fs path **every raw
block put also appends a commit** (`pfs_ark_put` → `ark_block_put` +
`ark_checkpoint`, `pfs_ark.c:215-226`). An actively-rewritten file consumes the
disk linearly — old versions and superseded blocks are never reclaimed — until
`emit_record` hits `g_head + need > total_sectors` and returns `ARK_E_FULL`
(`arkfs.c:222`). At that point the filesystem is write-dead. For a node meant to
run for years and "survive humanity," **keep-everything with no capacity story is
not a feature, it is a fuse**. `survival-fs.md §8` admits this ("append-only は
永遠に増える … 当面『捨てない』") but the consequence — terminal, not deferrable —
is understated.

### 🔴3 — Not wired on bare metal; survival exists only as a Linux process.
`arkfs.c` compiles into all four targets (`boot/{x86,aarch64,linux,linux_x86_64}/
Makefile`), but the **durable wiring is entirely hosted-only**:
- `pfs_ark.c` is built only by `boot/linux/Makefile` and
  `boot/linux_x86_64/Makefile`.
- Every ARK call in `pfs_block.c` is inside `#ifdef _TK_HOSTED_LIBC_`
  (`pfs_block.c:200-209` put, `:242-264` get, `:374` restore), and
  `_TK_HOSTED_LIBC_` is defined only in those two Makefiles.

So on the **actual RPi / x86 bare-metal target** — the hardware the whole
"survive a reboot with memory intact" story is about — ARK is linked dead code,
and p-fs P0 stays in-memory (lost on every reboot). The survival property is
demonstrated only when p-kernel runs as a Linux userspace process. This is
disclosed in `survival-fs.md §8` ("ベアメタルはビルド通過のみ"), but it is the
single biggest gap between the claims and the lived behaviour and deserves
top-line honesty: **today ARK saves a Linux process, not a Pi.**

---

## 🟡 Significant findings

### 🟡4 — Full-log replay every mount; no checkpoint pointer.
The superblock is **write-once** (only `ark_format` writes sector 0,
`arkfs.c:418`; no commit or checkpoint ever rewrites it — note this contradicts
the audit brief's "rewrite-superblock-every-commit" premise: ARK does *not* do
that, which is good for SB safety). The flip side: the superblock records **no
pointer to the latest commit or log head**, so `ark_mount` replays the whole log
from sector 1 on every boot (`arkfs.c:455-477`). Boot cost is
O(total-lifetime-bytes-written), not O(live-data). Combined with 🔴2 (no GC) this
grows without bound. `ark_history` / `ark_read_version` are likewise full-log
linear scans that re-read and re-hash every commit (`arkfs.c:687-750`).

### 🟡5 — Superblock is an un-replicated single point of failure.
Sector 0 has a crc but **no backup copy**. One rotted byte there →
`ark_mount` returns `ARK_E_CORRUPT` (`arkfs.c:434-436`) → the entire library is
unmountable. The §5 crash proof reasons carefully about the *log tail* but never
addresses sector-0 media failure — which is precisely the "flood" (bit-rot, bad
block) the FS claims to survive. Write-once status reduces *tearing* risk but
does nothing for *rot* on that one critical sector.

### 🟡6 — The atomicity claim is slightly too strong; the test only models prefix crashes.
`survival-fs.md §5.2` claims the observed state is always "last-good OR
new-complete, never half." There is one reachable exception. If a data block's
**header** sector persists but its **payload** is torn, *and* the trailing
`REC_COMMIT` also persists, replay will: index the rotted block (mount indexes
by header regardless of payload validity, `arkfs.c:462-467`, by design so reads
can flag rot) and adopt the commit. `ark_read_file` then returns `ARK_E_CORRUPT`
with **no automatic fallback to the prior intact version** (`arkfs.c:590-593`).
Integrity is preserved (corruption is detected, never silently served), but the
*current view* of that file becomes unreadable until a manual `ark_read_version`
rollback — that is not "always last-good." This ordering requires device
write-reordering inside the un-`fsync`'d window, i.e. it is only reachable on a
crash **before the put's fsync returns** (so the write was never acknowledged),
which keeps it from threatening durable data — but it is real. And the crash
test cannot catch it: `fb_write` (`arkfs_test.c:61-92`) models only
prefix-truncation ("kill at write #k, nothing after #k persists"). It never
models reordering, so the strongest part of the suite proves a weaker property
(prefix-durability) than the prose claims (full atomicity under arbitrary
power-loss).

### 🟡7 — Per-put fsync + per-put full commit = heavy write amplification on SD/flash.
`pfs_ark_put` does, per single block: append block record, append commit record,
`fsync` (`pfs_ark.c:219-224`). On a Pi SD card that is two records and a sync per
block — small-block workloads thrash, and each commit re-serialises the whole
live directory table (`serialize_live`, `arkfs.c:323-334`). Append-only is the
right *family* for flash (sequential, no in-place rewrite), but this realisation
amplifies it badly.

### 🟡8 — Block written to disk before the index-full check.
`ark_block_put` calls `emit_record` (advancing `g_head`, touching the device)
*before* `idx_add` can report `ARK_E_FULL` (`arkfs.c:505-507`). The orphan is
harmless (no commit follows, so remount reclaims it via `head_after_commit`,
`arkfs.c:479`), but on a full index every `ark_write_file` attempt still burns
device writes before failing.

---

## 🟢 What is genuinely right (do not change)

- **Content-addressing shared byte-for-byte with p-fs.** `ARK_ID_LEN ==
  SHA256_DIGEST_SIZE` and the same `relay/sha256.c`; `_Static_assert`s pin it
  (`arkfs.c:95`). Sample 26 proves the §7 seam for real: put → `kill -9` →
  remount → `pfs get` served *from the ARK log*, sha-verified. The "filesystem
  and content store are one thing" claim holds.
- **Self-verify is on the read path, the replay path, and (p-fs) re-checked
  again.** `read_record` re-hashes payload and compares to the header id every
  time (`arkfs.c:309-314`); `ark_block_get` returns `ARK_E_CORRUPT` on mismatch
  (`arkfs.c:520`); `pfs_get` re-hashes once more before serving
  (`pfs_block.c:248-256`). Rot is caught, never silently served — verified by
  both samples.
- **Header-validity vs payload-validity separation** (`arkfs.c:271-317`) is a
  genuinely good idea: a single rotted block advances the scan by its declared
  length instead of truncating the whole log, while still surfacing as CORRUPT on
  read. This is more careful than most toy log FSes.
- **Epoch fencing** (`arkfs.c:398-405`, checked at `:288`) correctly stops a
  prior format's stale-but-valid tail from being replayed as live — a real trap
  they hit and closed.
- **ABI-stable on-disk layout.** Fixed-width `U1/U2/U4` only, every struct size
  pinned by `_Static_assert` (`arkfs.c:93-102`); honours the LP64 typedef trap.
  Images are portable across aarch64 / x86_64 / i686.
- **The FAT32 critique is fair.** `arch/x86/fat32.c` (retained, 1030 lines) does
  overwrite in place, carries zero checksums, and keeps no versions. ARK's three
  properties genuinely fix all three. The polemic in `survival-fs.md §0` is
  accurate, not strawmanned.
- **Crash rollback for acknowledged writes is sound and well-tested.** Commit is
  the single visibility gate, `fsync` follows it (`commit_live`, `arkfs.c:360`),
  replay accepts up to the last fully-valid commit and reclaims the tail. Sample
  25 exercises *every* device-write point of an update; all land on v1-whole or
  v2-whole.

---

## Honest residuals (what is design-only, stated plainly)

| Claimed / implied | Reality | Evidence |
|---|---|---|
| ARK as durable backend | Hosted-only (`boot/linux*`), bare-metal = dead code | 🔴3 |
| Real VFS dispatch | None. `VFS_BACKEND` enum has only `{FAT32, PFS}`; no `ARK`; `vfs.c` has no dispatch table ("the two coexist as separate entry points") | `vfs.h:85-88` |
| Bare-metal mount | Build-passes only; `ARK_BDEV`↔`blk_ssy` bridge not written | `survival-fs.md §8` |
| Merkle directory tree | Flat path-string table, cap 32 files | `arkfs.c:155`, §2.4 |
| Log GC | None; "捨てない" | 🔴2 |
| fd-stream API (open/seek) | Whole-file only; adapter design-only | §6 |
| "Keep everything" | Capped at 256 blocks ≈ 1 MiB | 🔴1 |

To the authors' credit, `survival-fs.md §8` already lists most of these
honestly. The audit's job is to rank their severity: items 🔴1–🔴3 are not
"future increments," they are blockers to the survival claim.

---

## Prioritized next steps for ARK

1. **(🔴 blocking) Kill the fixed caps.** Make the block index, live table, and
   per-file block list scale with image capacity (or back the index on disk),
   and resolve the `512 refs > 256 slots` inconsistency. Don't touch the device
   in `ark_block_put` until index space is confirmed (fixes 🟡8 too). Until this
   lands, ARK is a 1 MiB store regardless of media.
2. **(🔴 blocking) Add a cleaner / capacity policy.** A compaction pass that
   rewrites live blocks + the latest commit into a fresh epoch (or a retention
   horizon driven by `degrade.c` capacity signals, as the doc gestures at).
   "Keep-everything" needs a survival story for the day the disk fills, or it is
   a fuse.
3. **(🔴 blocking) Wire ARK on bare metal.** Bridge `ARK_BDEV` to
   `blk_ssy`/`ide0` and do a real mount so the RPi target gains durable memory
   across reboot. The doc says this is "1 function"; until it exists the survival
   property is a Linux-process demo (🔴3).
4. **(🟡) Superblock redundancy + a head/checkpoint pointer.** A second SB copy
   (e.g. last sector) and a recorded latest-commit location make sector-0 rot
   recoverable (🟡5) and make mount O(live) instead of O(lifetime) (🟡4).
5. **(🟡) Read-path auto-fallback + a reordering crash test.** On a CORRUPT
   current version, fall back to the newest intact prior version (true
   "always last-good," 🟡6); extend `fb_write` to model write reordering, not
   just prefix truncation.
6. **(🟡) Batch commits.** Coalesce many block puts under one fsync'd checkpoint
   to cut SD/flash write amplification (🟡7).
7. **(🟢 later) Merkle directory tree + real VFS dispatch table.** Unlocks the
   flat 32-file namespace and aligns ARK with the p-fs object layer.

---

## One-paragraph answer to the owner

Yes, the design is *right* — the bones (append-only log, content addresses,
atomic commit, self-verify, epoch fencing) are the correct choices and are
implemented carefully, and the p-fs unification is real. But the ~800 lines you
shipped are closer to a **convincing hosted demo than a filesystem that survives
humanity**, for three concrete reasons: it caps out at ~1 MiB (256 blocks) no
matter how big the disk is; it has no garbage collection, so any long-running
node eventually wedges at `ENOSPC`; and it isn't actually switched on for the
bare-metal hardware it's meant to protect — the durability is Linux-process-only
today. Fix those three (plus a superblock backup and an auto-rollback on read),
and ARK becomes the library that does not perish. Until then it's the right ark
with the hull still on the slipway.
