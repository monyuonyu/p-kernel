# 37_ark_merkle — ARK's Merkle directory tree (format v3)

`arch/common/arkfs.c`'s original directory was a **flat snapshot** capped at
`ARK_MAX_FILES = 32`, serialized in full into **every** commit — so it could not
scale and every mutation re-wrote the whole table. This sample exercises the
**Merkle directory tree** added in wave 17 (format v3).

## The design (what landed)

A directory is now a **content-addressed block** (a *node*): an `ark_mnode_hdr`
followed by name-sorted `ark_ment[]` entries. Each entry is
`{type, size, child, name}` where `child` is the **block-id of the subtree it
names** — a file's content block, or another directory node. Because a node's
own id is `sha256(its bytes)`, that id cryptographically commits to its entire
subtree: this is what makes it a Merkle tree.

- **On-disk shape.** Nodes ride ARK's existing append-only, self-verifying block
  store (`ark_block_put`/`ark_block_get`). The **root id** is recorded in the
  `COMMIT` record (`ark_commit_hdr.mroot`), alongside the legacy flat table.
- **Root hash.** `ark_mtree_root()` returns the 32-byte root id. Entries are
  kept name-sorted so identical directory contents always serialize to identical
  bytes → identical id (deterministic, mergeable).
- **Update.** `ark_mtree_put(path, …)` rewrites only the changed path's nodes
  bottom-up and commits the new root. The commit payload is **O(1)** in the
  namespace (just the root id), not O(entries).
- **Crash-safety.** The commit is the same single atomic visibility point as the
  whole-file API: a torn commit fails crc on replay → the prior root stands; the
  new (uncommitted) nodes are tail garbage reclaimed on remount.
- **Self-verify.** A node read goes through the block store's crc+sha check, so a
  tampered node returns `ARK_E_CORRUPT` and is never served.

### Format version bump

The commit shape changed (it now carries `mroot` + a reserved flags word), so
the superblock magic/version moved **v2 → v3** (`"ARKLOG02"` → `"ARKLOG03"`,
`ARK_FMT_VERSION = 3`). A v2 image cleanly refuses to mount under v3 (never a
mis-mount). Every harness reformats its image, so all stay valid; a
fresh/legacy-only image simply reads back an all-zero (empty) Merkle root.

## What this sample proves (cross-process, file-backed device)

- **(a) SCALE > 32** — 40 files stored under one directory node and all read
  back, content-verified (the flat 32-file snapshot could not hold these).
- **(b) TAMPER-EVIDENT** — the root hash changes iff an entry changes; restoring
  an entry's exact bytes restores the exact root.
- **(c) SELF-VERIFY** — a directory node corrupted on disk is rejected on a fresh
  mount (`ARK_E_CORRUPT`).
- **(d) CRASH-SAFE** — a writer `SIGKILL`'d mid dir-update (real power loss in
  the block device) rolls back to the prior committed root, at every device-write
  point.

The in-process `ark_self_test` (run by `samples/25`) checks the same four
properties on a RAM device.

## Run

```sh
./run.sh        # exit 0 = all PASS
```

## Scope / honest limits

- Files in the Merkle layer are **single-block** (≤ `ARK_BLOCK_MAX` = 4 KiB);
  multi-block files via a manifest node are a future increment.
- Path depth is bounded by `ARK_MTREE_MAXDEPTH`; a component by `ARK_MENT_NAME`.
- The Merkle namespace is **additive** to (independent of) the legacy whole-file
  namespace; both persist in the same commit and the same content-addressed log.
  `ark_compact()` preserves a non-empty Merkle tree (its nodes + content).
