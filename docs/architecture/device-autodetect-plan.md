# Runtime SMP core-count autodetection + device-capability adaptation: design plan (cert-first)

> **Status: DESIGN PLAN on trunk `82d0295a`** (verified `git rev-parse --short HEAD` ==
> `82d0295a` in the shared checkout; `arch/aarch64/smp.c` + `docs/architecture/device-capacity.md`
> both present there). Awaiting mk_pino's go-ahead + a separate impl→audit cycle
> (implementer ≠ auditor ≠ commander, per the constitution). Read-only on code; every claim
> below is grounded in `file:line`. **Honest > green.**

mk_pino's direction: **「デバイスのスペックを測って自動で合わしたい」** — measure the
device's specs and auto-adapt, instead of hardcoding. The SMP work so far HARDCODES the
CPU count (`SMP_MAX_CPUS = 4`, headed to 8 in a parallel N=8 slice). This wave designs how
the kernel **DETECTS** the actual core count at boot and wakes **EXACTLY** that many — so a
4-core RPi3 uses 4, an 8-core phone uses 8, **SAME binary, no recompile** — and connects
that detection to the broader (deferred) device-capacity thread that sizes the *mind* to
the device.

---

## 0. The honest split (read this first)

There are TWO things, and they are NOT the same size:

| | **Slice 1 (this wave proposes)** | **The bigger deferred thread** |
|---|---|---|
| **What** | Detect the **core count** at boot; wake exactly that many SMP CPUs | Measure a full **capability vector** (cores + RAM + compute bench + thermal) → size the **mind** (student tier S/M/L, supernode/teacher role) |
| **Where it lives** | `arch/aarch64/smp.c` + `tkdev_init.c` + `start.S` | `docs/architecture/device-capacity.md` (DRAFT) + `degrade.c` |
| **Status** | small, concrete, high-value, **buildable now** | partly-designed, **DEFERRED** by `device-capacity-verdict.md:1-3` |
| **Risk** | low (one MMIO read + a runtime loop bound; gated, N=1 byte-identical) | high (greenfield baby substrate, VLA stack-overflow risk, distillation is vapor — `device-capacity-verdict.md:27-44`) |

Slice 1 is the **core count** — one device spec, the cleanest, the one that makes the kernel
adapt to *any* board today. The full "measure specs → size the mind" is the device-capacity
revival, and `device-capacity-verdict.md` already **DEFERRED** it for stated reasons (the
baby is test-only, distillation is unverified, heterogeneous sizing fractures "one mind").
This plan does **not** un-defer that. It proposes the small concrete slice **and** sketches
the hook so the bigger thread, when it revives, plugs into the same boot-time probe.

**The `capacity(N)`-is-NOT-the-local-device distinction (must state explicitly):**
`degrade.c`'s `capacity_experts()` / `cap_experts_of()` / `capacity_score()` compute capacity
from `alive_node_cnt` — the **DISTRIBUTED FLEET size** (`degrade.c:43` `alive_node_cnt`,
counted at `degrade.c:69-74` over `dnode_table[].state == DNODE_ALIVE`). That is **how many
nodes are alive in the mesh**, NOT **how strong THIS device is**. The code says so itself:
`cap_experts_of`'s honest label (`degrade.c:149-155`) — *"this is a DEGRADE-CAPACITY estimate
… NOT the model-sizing mechanism … every model is fixed compile-time today."* The **device**
axis (cores/RAM/compute of the LOCAL machine) is the *second input* that `device-capacity.md`
proposes to multiply in (`device-capacity.md:§0` — *"`capacity(N)` の DEVICE 側"*), and it is
the deferred part. Core-count autodetect is the **first, smallest** device-axis signal — and
the only one that already has a clean hardware source on aarch64.

---

## 1. Runtime CPU-count detection — the core

### 1.1 What today does (the hardcode, and where x0 dies)

- `SMP_MAX_CPUS` is a compile-time `#define 4` in **two** places that must agree:
  `arch/aarch64/smp.c:163` and `arch/aarch64/include/smp_percpu.h:70`. (A sibling
  `PK_SMP_MAX_CPUS 4` lives in `arch/aarch64/mc2_smp.c:50` for the MC-2 tile path.)
- The bringup loop is **hardcoded to the ceiling**: `smp_bringup_secondary()` wakes
  `for (c = 1; c < SMP_MAX_CPUS; c++)` (`smp.c:568`) — i.e. it *always* tries to PSCI
  CPU_ON cores 1,2,3 regardless of how many the board actually has.
- The certs read the **compile-time** count: `main.c:324` `const unsigned long N = 4UL;`
  and `main.c:337` `expect = N * K;` — the expected mutex total is wired to 4.
- **No CPU-count register is read anywhere.** A tree-wide search for `GICD_TYPER` / `TYPER`
  / `CPUNumber` / `availableProcessors` returns **zero** hits in `arch/`, `kernel/`. The
  only `0x004` near the GIC is `GICC_PMR` (`tkdev_conf.h:37`, `smp.c:179`) — the CPU-interface
  priority-mask register, a different block. So **GICD_TYPER is confirmed NOT read today.**
- **The DTB pointer is discarded at `_start`.** `start.S:41-43` is the *first* thing the
  primary does: `mrs x0, mpidr_el1` — it **clobbers x0** to extract the core id, before any
  `_el1_entry` setup. On QEMU virt / RPi3 the bootloader hands a DTB (FDT) physical address
  in **x0** at entry (the Linux aarch64 boot convention; the RPi3 image header at
  `start.S:25-34` is exactly that convention). We **overwrite x0 immediately** and never
  save it. So today there is **no FDT pointer to parse** even if we wanted one — recovering
  it is part of the DTB-option cost (see §1.3).

### 1.2 The recommended mechanism: GICD_TYPER bits[7:5] (PRIMARY)

The GICv2 distributor reports the number of implemented CPU interfaces **directly**, in a
single MMIO read, with **zero firmware dependency**. It works on QEMU virt and on RPi3's
GICv2 layout — exactly the two boards `tkdev_conf.h:23-30` already targets.

**The register.** `GICD_TYPER` is the GIC Distributor Type Register at distributor
offset **`0x004`**. `tkdev_conf.h:33` already defines `GICD_CTLR 0x000` (the register right
before it) and `GICD_BASE` (`tkdev_conf.h:28` QEMU virt `0x08000000`, `:25` RPi3
`0x40041000`). We add **one** offset constant:

```c
/* tkdev_conf.h — add next to GICD_CTLR (0x000) */
#define GICD_TYPER  0x004   /* GIC Distributor Type Register (read-only) */
```

**The decode.** `GICD_TYPER` bits **[7:5]** = `CPUNumber` = *(number of CPU interfaces
implemented) − 1* for GICv2. (Bits [4:0] = `ITLinesNumber` size the SPI space — unrelated.)
So:

```c
/* tkdev_init.c — inside the !BOARD_RPI3 GICv2 gic_init() (around :93),
 * OR a small detect helper smp.c calls before bringup. */
static unsigned int gic_cpu_count(void)
{
    unsigned int typer = mmio_read32(GICD_BASE + GICD_TYPER);
    return ((typer >> 5) & 0x7u) + 1u;     /* CPUNumber+1, range 1..8 */
}
```

- `mmio_read32` already exists (`tkdev_init.c:24-27`) — no new MMIO primitive needed.
- The field is **3 bits → max 8** CPU interfaces. That is the GICv2 ceiling, and it
  matches the SGI 8-bit target list (§2). **>8 cores ⇒ GICv3** (a separate lift, §5).
- On **QEMU virt** the GIC instantiates exactly as many CPU interfaces as `-smp N`
  requests (for N ≤ 8), so `gic_cpu_count()` returns N — *the* mechanism that makes the
  same binary adapt to `-smp 2/4/8`. On **RPi3** GICv2 reports 4. (RPi3 caveat below.)

**Where it runs.** Two honest options:
- (a) **In `gic_init()`** (`tkdev_init.c:93`), which already touches `GICD_BASE + GICD_CTLR`
  — so the distributor base is known-good at that point. Cache the result in a global
  `g_smp_ncpu`. But the SMP self-test (`smp.c`) currently runs **before**
  `knl_t_kernel_main()` (and thus before `gic_init`, see `smp.c:190-194`, `:272-292`), so
  for the self-test path the detect must run from the SMP side.
- (b) **In `smp.c`** as `gic_cpu_count()` reading `SMP_GICD_BASE` (`smp.c:174`, already the
  same `0x08000000`), called at the top of `smp_selftest_run()` / `smp_bringup_secondary()`.
  This keeps the self-test self-contained (it already stands up the distributor itself,
  `smp.c:273-292`). **Recommend (b) for slice 1** (the cert path lives in `smp.c`), with the
  production `gic_init()` also caching it for the eventual production bringup.

**Honest caveat on RPi3.** RPi3's interrupt block is the **BCM2837 ARM Local Interrupt
Controller, NOT a GIC** — `tkdev_init.c:29-70` documents this in full (*"It is NOT a GIC.
There is no distributor or CPU interface…"*). Under `BOARD_RPI3`, `GICD_BASE` in
`tkdev_conf.h:25` (`0x40041000`) is a *nominal* address and the `gic_init()` path there
(`tkdev_init.c:61-70`) does **not** use it. So **GICD_TYPER detection is QEMU-virt-clean but
on real RPi3 hardware the core count must come from a different source** (the BCM2837 has 4
cores fixed; a constant `4` under `BOARD_RPI3`, or the DTB `/cpus` node, is the honest path
there). Slice 1's cert is **QEMU-virt** (where GICD_TYPER is exact); the RPi3 number is a
build-constant for now and a DTB follow-up later. State this in the cert (§3).

### 1.3 The alternatives, compared honestly

**DTB `/cpus` node (the canonical source; richer; more code) — DEFER.**
The device tree's `/cpus` node enumerates every CPU with its `reg` (MPIDR) and, crucially,
its `compatible` / `capacity-dmips-mhz` — i.e. it carries the **per-CPU info** GICD_TYPER
cannot: which cores are big vs LITTLE. It is *the* canonical answer on any board (RPi3
included, where GICD_TYPER is unavailable). **Cost:** (1) **save x0 at `_start`** — today
it's clobbered at `start.S:41` before anything; we'd stash the FDT phys addr into a global
*before* `mrs x0, mpidr_el1`, on the primary only (secondaries get a context_id in x0, not a
DTB); (2) a **minimal FDT parser** (big-endian header, token walk to `/cpus`, count `cpu@*`
children) — ~150-250 lines of new, carefully-bounded code in a libc-free kernel. That is
real surface, and it buys nothing slice 1 needs (the count). **Recommend: GICD_TYPER now;
DTB as the richer future option** — specifically the source that unlocks RPi3-real-hardware
count and big.LITTLE tiering (§4, §5).

**PSCI-probe (try CPU_ON each MPIDR) — REJECT.**
We could loop MPIDR 0..7 calling `smp_psci_cpu_on()` (`smp.c:90-105` already wraps the SMC64)
and count the `PSCI_SUCCESS` / `PSCI_ALREADY_ON` returns. **Why reject:** it is **slow**
(each probe is an HVC round-trip + bringup) and **destructive** (it actually *starts* cores
as a side effect of counting them, so you can't count-then-decide; you'd have to count by
launching). GICD_TYPER answers the same question with one non-destructive load. Keep PSCI
strictly for *bringing up* the cores we decided to wake, not for *counting* them.

**Verdict:** **GICD_TYPER bits[7:5] is the primary mechanism** (one MMIO read, zero firmware
deps, QEMU-virt + RPi3-GICv2 clean, exact on QEMU). **DTB `/cpus` is the deferred richer
option** (canonical, carries big.LITTLE info, but needs x0-save + an FDT parser).
**PSCI-probe is rejected** (slow/destructive).

---

## 2. Auto-size the SMP fleet

The clean separation: **`SMP_MAX_CPUS` becomes the compile-time ARRAY CEILING; a new
runtime `g_smp_ncpu` is the active count.**

### 2.1 Ceiling vs runtime count

```c
/* smp.c (and mirrored in smp_percpu.h) */
#define SMP_MAX_CPUS  8     /* GICv2 CEILING: the SGI target list is 8 bits,
                             * GICD_TYPER CPUNumber maxes at 7 → 8 interfaces.
                             * (was 4 at :163; the N=8 slice raises it.) */

/* NEW: the RUNTIME active count, decided at boot. */
static unsigned int g_smp_ncpu = 1;   /* default 1 until detect runs */
```

- **`SMP_MAX_CPUS = 8`** is the right ceiling because (a) GICv2's `GICD_TYPER.CPUNumber` is
  3 bits → max 8, and (b) the GICv2 **SGI target list is 8 bits** — `smp_send_reschedule()`
  already builds `(1u << cpu) << 16` into `GICD_SGIR` (`smp.c:229`), and that target byte
  holds 8 CPUs. Beyond 8 you are in GICv3 IPI territory (§5). The parallel N=8 slice raising
  `SMP_MAX_CPUS` to 8 is the prerequisite this slice builds on.
- **`g_smp_ncpu = min(detected, SMP_MAX_CPUS)`**, computed once at boot from
  `gic_cpu_count()`:

```c
void smp_detect_cpus(void)            /* call once, before bringup */
{
    unsigned int n = gic_cpu_count(); /* §1.2 — GICD_TYPER decode */
    if (n < 1) n = 1;
    if (n > SMP_MAX_CPUS) n = SMP_MAX_CPUS;   /* clamp to the array ceiling */
    g_smp_ncpu = n;
}
```

### 2.2 The bringup loop reads the runtime count

Every `SMP_MAX_CPUS` that is a **loop bound over live cores** becomes `g_smp_ncpu`; every
`SMP_MAX_CPUS` that **sizes a static array** stays the ceiling. The split, by line:

| `smp.c` site | today | change |
|---|---|---|
| `:353` `g_smpcpu[SMP_MAX_CPUS]` | array size | **stays** `SMP_MAX_CPUS` (ceiling) |
| `:214/:216` `g_resched_pending[]`, `g_sgi_taken[]` | array size | **stays** ceiling |
| `:568` `for (c=1; c<SMP_MAX_CPUS; c++)` bringup | loop bound | → `c < g_smp_ncpu` |
| `:587` `smp_wait_secondary_live` loop | loop bound | → `c < g_smp_ncpu` |
| `:658` `g_barrier < SMP_MAX_CPUS` | concurrency target | → `< g_smp_ncpu` |
| `:641` `SMP_NTASKS == SMP_MAX_CPUS` | task count | → `g_smp_ncpu` tasks seeded |
| `:261,:599,:606,:792,:959…` bounds checks | `me < SMP_MAX_CPUS` OOB guard | **stays** ceiling (guards the array; a core id ≥ ncpu but < ceiling is still a valid slot to *guard*) |
| `main.c:324` `N = 4UL` | cert expected | → `N = g_smp_ncpu` (read via a new getter) |

**Per-CPU stacks (`smp.c:523-525`, `:534-542`).** `_stack_top_cpu1/2/3` are linker-provided
regions selected by `smp_sec_stack_for(cpu)` (`smp.c:534`). For an 8-core ceiling the linker
must provide `_stack_top_cpu1..7` and the `switch` extends to `case 7`. These are **statically
sized to the ceiling** (8 stacks reserved in the linker script). When `g_smp_ncpu < 8`, the
bringup loop simply **never references** stacks `[g_smp_ncpu .. 7]` — they sit unused in BSS.
This is the standard "statically size to the ceiling, use the first N" pattern, and it is
already how `g_smpcpu[]` (`:353`) and the per-CPU flag arrays (`:214-216`) work. **Cost of the
ceiling:** 8 reserved dispatcher stacks + an 8-element `g_smpcpu[]` (`sizeof(struct smp_cpu)`
== 72, `smp.c:342`) — trivial BSS. No runtime cost when fewer cores boot.

### 2.3 The certs read the runtime count

`main.c:312-378` currently hard-codes `N = 4`. Add a getter and read it:

```c
/* smp.c */
unsigned int smp_ncpu(void) { return g_smp_ncpu; }   /* the runtime count */

/* main.c (replaces :324 N=4UL) */
const unsigned long N = smp_ncpu();
const unsigned long expect = N * K;          /* :337 — now runtime-sized */
```

The per-CPU loops in `main.c:332-353` (`for c in 0..3`) become `for c in 0..N-1` (or stay at
the ceiling with an `e[c]>0` check **only** for `c < N`). The mutex total
`smp_get_counter()` (`smp.c:611`) must equal `g_smp_ncpu * K` — the cert's whole point.

---

## 3. THE CERT `[smp-autodetect]` (cert-first, falsifiable)

**Claim.** The **SAME kernel binary** (one `kernel.elf`, built once with `-DSMP_SELFTEST`),
booted under QEMU virt with `-smp 2`, `-smp 4`, `-smp 8`, **auto-detects** the core count via
GICD_TYPER and wakes **EXACTLY** that many — proving **no recompile** is needed for a
different device.

**Observable PASS conditions (per run):**

| run | detected `g_smp_ncpu` | woken-core set | mutex total | per-CPU `exec_count>0` |
|---|---|---|---|---|
| `-smp 2` | 2 | cpu0,cpu1 only | `2*K` | cpu0,1 yes; cpu2..7 absent |
| `-smp 4` | 4 | cpu0..3 | `4*K` | cpu0..3 yes |
| `-smp 8` | 8 | cpu0..7 | `8*K` | cpu0..7 yes |

i.e. `SMP-MUTEX: PASS` with `total == g_smp_ncpu * K`, and the woken-core set (cores that
emitted `[SMP] cpuN entered dispatcher`, `smp.c:837`, and whose `exec_count>0`,
`smp.c:856`) **equals** the `-smp` count. The harness greps the UART for the detected count
(add a `smp.c:smp_dbg` line: `[SMP] detected N cpus via GICD_TYPER`) and for
`total == N*K`.

**One binary, three runs.** The existing `run_smp0.sh` (`tests/aarch64/run_smp0.sh`)
already builds **once** and boots under QEMU; it snapshots the image per run
(`run_smp0.sh:38-45` note) so a concurrent rebuild can't corrupt it. `[smp-autodetect]`
**extends it**: build the `-DSMP_SELFTEST` kernel **once**, then loop
`for SMP in 2 4 8; do run_qemu … -smp $SMP …; assert total==$((SMP*K)); done` — **no rebuild
between runs**, which is exactly what proves "same binary adapts."

**THE FALSIFIER (load-bearing).** Build a variant that **ignores GICD_TYPER** and hardcodes
the count (e.g. `-DSMP_FORCE_NCPU=4`, forcing `g_smp_ncpu=4`). Then:
- under **`-smp 2`**: the bringup loop tries PSCI CPU_ON for cores 2,3 that **don't exist**
  → `smp_psci_cpu_on` returns non-success, **or** `smp_wait_secondary_live()` (`smp.c:580`)
  times out waiting for cores that never go live → `SMP-BOOT: FAIL` / hang up to the watchdog
  (`smp.c:582` `MAX_TRIES`), and the mutex total ≠ `2*K`. **The cert FAILs** → proves the
  detect is load-bearing.
- under **`-smp 8`**: the hardcoded-4 build wakes only 4 of 8 → `total == 4*K` not `8*K` →
  the device is **under-used**, mismatch detected. **The cert FAILs.**

This is the same falsifier discipline as the existing `-DSMP_MUTEX_NOLOCK`
(`smp.c:500-510`, `run_smp0.sh`) and `-DSMP_NO_IPI` (`smp.c:223-227`): a build flag that
**breaks the one mechanism under test** and proves the green isn't vacuous.

**QEMU-testable vs the honest big.LITTLE caveat.** Each `-smp N` is a **real QEMU run** —
fully automatable in CI, no hardware needed. The honest bound: GICD_TYPER counts CPU
**interfaces equally** — on an 8-core big.LITTLE phone it returns 8, but those 8 are
**heterogeneous in performance** (4 big + 4 LITTLE). GICD_TYPER **does not tier them**;
slice 1 wakes all 8 and treats them as equals. **Perf-tiering is device-capacity's job**
(§4), and the per-CPU `capacity-dmips-mhz` that distinguishes them lives in the **DTB**, not
in GICD_TYPER (§1.3). State this in the cert doc so nobody reads `[smp-autodetect]` PASS as
"big.LITTLE solved." It is not; it counts cores, it does not rank them.

---

## 4. The device-capability hook (connect to the deferred thread)

Core count is **one** device spec. The full vision (`device-capacity.md`) is sizing the
**mind** — the student tier (S/M/L), whether this node should be a supernode/teacher — to the
device's RAM / compute / thermal headroom. The bridge is a **capability vector** measured at
boot, of which core-count is the first, already-buildable component.

### 4.1 From "count cores" to "measure a capability vector"

```c
/* The boot-time probe slice 1 plants the seed for. */
struct dev_capability {
    unsigned int cores;        /* SLICE 1: GICD_TYPER (§1.2) — buildable NOW */
    unsigned long ram_bytes;   /* SLICE 2: from the memory map / DTB /memory  */
    unsigned int  bench_score; /* SLICE 2: a quick st_forward micro-benchmark */
    unsigned int  thermal_hr;  /* SLICE 2: thermal headroom (S_n / sensor)    */
};
```

This vector then feeds **TWO consumers**:
1. **The SMP fleet size** — already wired by slice 1 (`g_smp_ncpu = min(cores, ceiling)`,
   §2). The `cores` field *is* `g_smp_ncpu`.
2. **The device-capacity tier** — the deferred consumer. `device-capacity.md:§0` proposes
   multiplying a **device coefficient** into `capacity_score()`. Today
   `capacity_score()` = `experts × depth × kv` driven by `alive_node_cnt`
   (`degrade.c:203-208`, `:179-182`). The hook is: a **second input** — the local device's
   capability scalar — that scales the per-node share. **Slice 1 does NOT touch `degrade.c`**
   (that's the deferred mind-sizing); it only ensures the **probe** exists and is the
   place the future `device` coefficient reads from.

### 4.2 What's a small concrete next slice vs the bigger deferred revival

- **Concrete now (slice 1):** core-count autodetect + `[smp-autodetect]`. One MMIO read, a
  runtime loop bound, a cert. Changes the *kernel's* core-count adaptation. **Does NOT touch
  the mind** (R3 `rw[]` stays fixed fleet-wide — the crown invariant,
  `device-capacity-verdict.md:11-16`).
- **Slice 2 (this plan stages, smaller than the full revival):** add `ram_bytes` +
  `bench_score` + `thermal_hr` to the probe and surface them as **honest observability**
  (the capacity METER) — exactly what `device-capacity-verdict.md:63-72` (SLICE 0) already
  blessed as low-risk: *"Detect real device signals … surface them … labeled '担当キャパシティ
  (capacity), not 賢さ (smartness)' … changes NO model behavior."* The probe must be
  **fixture-injectable** (`device-capacity-verdict.md:71`, env `PKERNEL_DEVICE_TIER`) so CI
  without real hardware can cert it (the salty-bug lesson).
- **The bigger DEFERRED revival (NOT this plan):** sizing the *mind* to the tier (student
  S/M/L, distillation across tiers). `device-capacity-verdict.md:45-61` lists the **gate** it
  must pass first (real baby substrate, no-VLAs, working distillation, `[baby-merge-isolation]`
  tripwire, bigger baby first). Slice 1/2 deliberately stay **below** that gate.

### 4.3 The `capacity(N)` ≠ local-device clarification (restate)

`degrade.c`'s `capacity_*()` is **fleet-size-driven** (`alive_node_cnt`, `degrade.c:43,69-74`)
and the code labels its expert clamp as a **degrade/display number, not a sizing mechanism**
(`degrade.c:149-155`). The **device coefficient** this plan's probe seeds is the *orthogonal*
input `device-capacity.md` calls the "DEVICE 側." Do not conflate "N alive nodes in the mesh"
with "this device has M cores." Slice 1 measures the latter, for the SMP fleet only.

---

## 5. Sequencing + N=1 / byte-identity boundary

### 5.1 Slices

- **Prerequisite:** the **N=8 ceiling slice** (`SMP_MAX_CPUS 4 → 8`, linker stacks
  `_stack_top_cpu4..7`, the certs generalized to N=8) — in flight in parallel. Slice 1
  **builds on it** (the ceiling must be 8 before "detect up to 8" means anything).
- **Slice 1 (this wave):** GICD_TYPER core-count autodetect (`gic_cpu_count()`,
  `g_smp_ncpu = min(detected, SMP_MAX_CPUS)`, bringup loop reads the runtime count) +
  `[smp-autodetect]` cert on QEMU `-smp 2/4/8` (one binary) + its hardcode falsifier.
- **Slice 2:** the capability vector (`ram_bytes` + `bench_score` + `thermal_hr`) surfaced as
  the **honest capacity METER** (device-capacity SLICE 0, already blessed) + the
  fixture-injectable probe. Feeds the future `device` coefficient hook (no `degrade.c`
  behavior change yet).

### 5.2 DEFER (explicit)

- **DTB / FDT parsing** (needs x0-saved at `_start`, `start.S:41`, + a minimal parser) —
  the richer canonical source; unlocks RPi3-real count + big.LITTLE info. §1.3.
- **big.LITTLE perf-aware scheduling** — GICD_TYPER counts cores, does not tier them; the
  perf metadata is in the DTB `capacity-dmips-mhz`. §3, §4.
- **GICv3 (>8 cores)** — GICv2's `GICD_TYPER.CPUNumber` (3 bits) and the 8-bit SGI target
  list cap at 8; >8 needs GICv3 (GICR/GICD redistributors + `ICC_SGI1R_EL1` IPIs), a
  separate lift. §1.2, §2.1.
- **The full device-capacity mind-sizing** (student tiers, cross-tier distillation) — gated
  by `device-capacity-verdict.md:45-61`; stays deferred. §0, §4.2.
- **RPi3-real core count** — GICD_TYPER is unavailable on the BCM2837 local controller
  (`tkdev_init.c:29-70`); under `BOARD_RPI3` use the constant 4 (or, later, the DTB). §1.2.

### 5.3 N=1 / byte-identity boundary (non-negotiable)

The shipped uniprocessor kernel and the x86/linux/rl78 ports must stay **byte-identical**.
This is already the law for all SMP work: `smp.c` is **entirely `#ifdef SMP_SELFTEST`-gated**
(`smp.c:73`, `:1063`) — *"with no flag, this TU is an empty object — the shipped kernel
carries NONE of it"* (`smp.c:53-56`); `smp_percpu.h` macros collapse to the plain global at
N=1, proven byte-identical by `objcopy -j .text cmp` (`smp_percpu.h:20-23`).

**Slice 1 keeps this invariant exactly:**
- The new `gic_cpu_count()` / `g_smp_ncpu` / `smp_detect_cpus()` live **inside the
  `SMP_SELFTEST` gate** in `smp.c` — the default build never compiles them.
- The one production-side addition (caching the count in `gic_init()`, `tkdev_init.c:93`)
  is a **no-op at N=1**: with no secondaries to wake, reading GICD_TYPER and storing a count
  changes no control flow (the production dispatcher still runs one CPU). If even the read is
  unwanted in the default build, gate it behind the same SMP flag and have `gic_init()` read
  it only when SMP is on. **Recommend: keep the production read out of the default path for
  slice 1** (do it in the SMP-gated `smp.c` path), so the default ELF is provably unchanged.
- **At N=1 the detect is a no-op:** `g_smp_ncpu` defaults to 1 (§2.1); a 1-core QEMU run
  detects 1, wakes zero secondaries, and behaves as today.
- **The mind is untouched in slice 1** — no `degrade.c`, no student, no R3. Core-count
  autodetect is purely a kernel-bringup concern.

---

## 6. Summary for mk_pino

**Slice 1 is the small, concrete, high-value piece:** one MMIO read of `GICD_TYPER`
bits[7:5] (`((TYPER>>5)&0x7)+1`), a runtime `g_smp_ncpu = min(detected, 8)`, and a bringup
loop + certs that read the runtime count instead of the hardcoded 4. The **same binary**
then wakes 4 cores on a 4-core board and 8 on an 8-core phone — proven by `[smp-autodetect]`
booting one kernel under QEMU `-smp 2/4/8` and getting `total == N*K` each time, with a
hardcode-the-count **falsifier** that hangs/under-uses to show the detect is load-bearing.

**The big "measure specs → size the mind" vision is the deferred device-capacity thread** —
already designed (`device-capacity.md`), already DEFERRED with a stated gate
(`device-capacity-verdict.md`), and **not un-deferred here**. Slice 1 only plants the
**probe** (the boot-time capability vector, starting with cores) that the future mind-sizing
will read. Honest line: the kernel adapting its core count is buildable today; the mind
adapting its size to the device is a bigger, gated, later thread.

**Honest bounds restated:** GICD_TYPER is GICv2-and-QEMU-virt-clean (exact) and RPi3-GICv2
nominal — but **RPi3 real hardware uses the BCM2837 local controller, not a GIC**, so its
count is a build-constant 4 until the DTB option lands; **>8 cores needs GICv3** (separate
lift); **big.LITTLE heterogeneity is a perf concern this slice does NOT solve** (it counts
cores, it does not tier them — that's device-capacity + the DTB).

---

*Grounding index (file:line):* hardcode sites `smp.c:163`, `smp_percpu.h:70`, `mc2_smp.c:50`;
bringup loop `smp.c:568`; cert N `main.c:324,337`; GICD offsets `tkdev_conf.h:28,33`;
GICD_TYPER NOT read today (tree-wide grep, zero hits); x0 clobbered at `start.S:41`; RPi3 not
a GIC `tkdev_init.c:29-70`; SGI target list `smp.c:229`; per-CPU stacks `smp.c:523-542`;
gate `smp.c:73,1063`; N=1 byte-identity `smp_percpu.h:20-23`; capacity=fleet-size
`degrade.c:43,69-74,179-208`; capacity honest label `degrade.c:149-155`; device axis
`device-capacity.md:§0`; defer verdict + gate `device-capacity-verdict.md:1-3,27-61,63-72`;
existing harness `tests/aarch64/run_smp0.sh`; falsifier precedents `smp.c:500-510,223-227`.
