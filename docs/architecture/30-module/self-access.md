# self-access — the node touches its own body (READ-ONLY first slice)

BACKLOG 🅰 — mk_pino's *「自分の体を触る」* / the MCP-analogue embodiment. A
resident mind that lives on p-kernel should be able to *perceive its own body*
the way an animal feels its limbs: which tasks it is running, which objects its
p-fs holds, which devices/sensors are present, how long it has been alive, who
its living peers are. This is the **interoception of the machine itself**
(distinct from `interoception.md`'s stress bus, which is about felt state — this
is about *structural* self-knowledge).

This document specifies **R0**, the SAFE first slice.

## R0 scope — STRICTLY READ-ONLY

R0 gives the mind (and an operator at the shell) the ability to **READ** about
itself. It gives **no** ability to write, exec, or drive its body — those are
R1+ and are deferred behind the gates below.

What R0 exposes (all over state the node *already* knows):

| domain    | source (READ-ONLY)                                  |
|-----------|-----------------------------------------------------|
| stats     | `drpc_my_node`, `tk_get_otm` uptime, `DNODE_ALIVE` peer count in `dnode_table[]`, `dproc_running_count()` |
| tasks     | `dproc_list()` — the cluster process table          |
| files     | `pfs_dag_foreach_ref()` — named p-fs/ark objects: **names + head version only, NEVER contents** |
| devices   | `MOE_NUM_CLASSES` sensor band, `net_my_ip` netif presence |

Module: `arch/common/self_access.c` (+ `self_access.h`). Shell verb: **`body`**
in `arch/linux/{aarch64,x86_64}/usermain.c` (the `self` verb was already taken
by the living-mind Self-layer test suite).

## The read-only invariant

`self_access.c` contains **no** `fopen`-for-write, `exec`/`system`/`popen`,
driver-register, `pfs_put`/`pfs_dag_save`, task-create/kill, `ioctl`, or `mmap`
call. Every getter copies live values out of kernel/cluster tables; none mutates
them. The two helper getters added to existing modules are equally read-only:

- `pfs_dag_foreach_ref()` walks the ref table and copies each object's **name**
  (length-bounded into a local) + head `seq` + `origin` to a callback. No block
  content is read; the table is not mutated.
- `dproc_running_count()` iterates the same K-DDS slot topics `dproc_list`
  reads and counts `DPROC_RUNNING` entries. No mutation.

## Q3 = YES — every body-touch is logged to the Self-lineage

DECIDED: each **explicit** introspection invocation appends **one** event to the
hash-chained autobiographical lineage `self/lin` (via
`lm_self_append_introspect()`, which rides the existing
`lm_self_append_unit_event` path — no new chain, no new hash; anti-fork
§6 of living-mind.md III). The event kind is `LM_SELF_EV_INTROSPECT`; a
`domains` bitmask of *what* was read rides the `age_ms` encoding so the lineage
records **that** the body was examined and **which** parts — never the read
*content*. Bounded: exactly one entry per invocation (no spamming the lineage).

So the mind's self-examination becomes part of its honest history — the same
history that survives death and reconstructs ownerless. *Looking at yourself is
an autobiographical act.*

The Q3 append is the **only** write surface in the whole feature, and it writes
a self-**record**, never the body.

## Deferred to R1+ (Q1/Q2-gated)

- **R1 — write**: the mind editing its own p-fs objects / config. Q1 gate.
- **R2 — drive/exec**: the mind invoking a driver or running code against its
  body. Q2 gate.
- **autonomy**: the *mind itself* invoking introspection on its own schedule
  (R0 only provides the capability + the `body` verb; a human invokes it).

These are intentionally **not** implemented in R0. R0 is the safe sense organ;
the hands come later, behind explicit gates.

## Proof (R0)

Boot `./boot/linux/p-kernel`, run `body` twice:

- the report shows **real** node state (uptime advances 0→10ms across the two
  calls; netif up with the live IP; 3 sensor classes; the dproc table);
- after the first `body`, the second's `pfs_objects` count rises to include
  `self/lin` + `self/sig` (the introspection *sees its own previous lineage
  write* — the read reflects real state);
- the Self-lineage head `seq` increments 1 → 2 and the chain verifies each time
  (one bounded entry per invocation).
