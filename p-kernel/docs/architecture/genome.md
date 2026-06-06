# Genome — §3 self-regeneration: growing a full cell on an empty plate

> survival-network.md §3: 「装甲板＝細胞。各細胞は完全な設計図(DNA)を
> 持ち、欠けた機能は隣の細胞が引き継ぎ、新しい装甲板が来れば群れが
> それを育てる。」

Until wave 9, "the swarm raises a new plate" was carried only by
`spawn.c`, which hands a new node the raft term/leader and the alive-node
table — *cluster metadata*, not a design. Meanwhile every part of an
actual design had been built, one wave at a time:

| part | where it already lives | wave |
| --- | --- | --- |
| weights (the brain) | p-fs versioned object `dtr/weights` — content-addressed, region-replicated, validated loader (`dtr save` / `dtr load` / `dtr_recover_weights`) | 6/7 |
| code | p-fs object (`selfc save` / `selfc run`) + in-kernel libtcc compilation | 7-D |
| transport | p-fs P1 block gossip + P2 ref gossip — symmetric, no master copy | 6 |
| survival map | `world.c` decentralized situational awareness | 8 |

**The genome layer adds no new organ. It is the missing orchestration:**
a small manifest that names those parts, and the two verbs that write it
and walk it.

## 1. The manifest

`arch/common/genome.c` + `arch/common/include/genome.h`. Fixed-width,
packed, sizes pinned by `_Static_assert` (the LP64 typedef trap rules):

```c
typedef struct {                       /* 28 B per entry */
    U1   kind;                         /* WEIGHTS / CODE / ENGRAMS    */
    U1   name_len;
    UH   _pad;
    char name[24];                     /* p-fs named ref              */
} __attribute__((packed)) GENOME_ENTRY;

typedef struct {                       /* 12 B + entries (<= 124 B)   */
    UW   magic;                        /* 'GNOM' LE                   */
    UH   ver;                          /* 1                           */
    U1   role;                         /* cell/brain/sensor/relay     */
    U1   reserved;
    UW   entry_cnt;
    GENOME_ENTRY entries[4];
} __attribute__((packed)) GENOME_MANIFEST;
```

The manifest is saved (trimmed to the used entries) as the p-fs named
ref **`genome/manifest`** via `pfs_dag_save`. That single decision does
most of the work: a manifest is just a content-addressed block plus a
ref, so P1 announce/want replication and P2 ref gossip carry it to every
region peer with **zero new transport code**, and old versions survive
every republish (the version DAG never deletes).

## 2. `genome publish <role>` — a full cell sows its DNA

Run on a node that has something to pass on:

- `dtr/weights` is **mandatory** — publish refuses without it ("a genome
  without a brain describes nothing"). Train + `dtr save` first.
- `genome.c` (C source saved by `selfc save genome.c`) is included **if
  the ref exists**.
- `dtr/engrams` is included **if the ref exists** — wave 9-① is building
  that object in parallel; genome.c probes for it and never depends on it.

Roles (`cell`, `brain`, `sensor`, `relay`) are parsed, stored in the
manifest, carried to the sprouting node and displayed by `genome`.
**Honest: nothing schedules by role yet** — see §5.

## 3. `genome sprout` — an empty plate germinates

Run on a node that has nothing (or boot it with `PKERNEL_SPROUT=1`,
hosted builds only — guarded by `_TK_HOSTED_LIBC_`; the default is OFF
so no existing demo changes behaviour):

1. Wait for `genome/manifest` to gossip in. Each probe calls
   `pfs_dag_read`, which itself plants a P1 WANT when the ref is known
   but the blocks are not local yet — waiting *is* the active fetch.
2. Validate magic / version / `len == 12 + entry_cnt*28`.
3. Resolve each entry:
   - **WEIGHTS** → wait for the `dtr/weights` replica, then call
     `dtr_recover_weights()` — the same validated read+install core that
     `dtr load` and the guard fault-recovery path use. One loader, three
     callers.
   - **CODE** → read the C source from the local replica and hand it to
     `selfc_compile_and_run()` — the source becomes machine code inside
     this node's own address space. Without libtcc the node prints
     `code present, libtcc absent — skipped`; on bare metal, `no
     in-kernel compiler on this target — skipped`. Honest, not silent.
   - **ENGRAMS** → ensure the replica is local. The engram *loader*
     belongs to the dtr memory work (wave 9-①) and is deliberately not
     invoked from here.
4. Only if the brain actually arrived does the node claim:
   `[genome] sprouted: a full cell grew from the swarm's DNA`.

`spawn.c` connects the existing self-propagation worldview to this: when
the leader pushes cluster state to a freshly spawned node and this cell
has published a genome, it prints a pointer to `genome sprout` /
`PKERNEL_SPROUT=1`. The raft meta push is untouched, and the hint reads
a local flag rather than probing p-fs (the `pfs_dag_read` scratch is
shell-task-only; `spawn_rx` runs in net-task context).

## 4. The proof

`samples/14_genome/sprout.sh`: node 1 trains (95% train / 100% held-out),
saves weights, saves code, publishes. Node 2 boots **empty** with
`PKERNEL_SPROUT=1`, never trains, and ends up evaluating at *exactly*
node 1's numbers (the float32 blob is bit-identical, so the accuracy
lines match character for character) with node 1's code having executed
inside its kernel. Five asserted legs; any miss is a non-zero exit.

## 5. What this is NOT yet (honest)

- **The kernel image itself is not distributed.** "Code" means C-source
  tasks compiled by selfc inside a hosted kernel. A bare-metal node
  cannot reflash itself from a genome; that needs a bootloader story
  (kloader exists, but no genome→kloader path).
- **Roles are carried, not enforced.** No scheduler, gating network or
  degrade level consults `genome role` yet. Regions R3 / survival §7 is
  where that should land.
- **No signature, no verification.** Anyone in the region can publish
  `genome/manifest`, and sprouting compiles whatever `genome.c` arrived
  — the same stated-not-solved trust model as selfc. Content addressing
  protects against *corruption*, not *malice*.
- **One manifest name.** Last-writer-wins ref gossip means concurrent
  publishers fork the ref exactly like any p-fs name (the losing version
  stays reachable in the DAG). Per-role or per-region manifests are a
  later refinement.
- **Engrams ride along but are not installed** until wave 9-①'s loader
  exists.
