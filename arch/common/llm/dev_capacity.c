/*
 *  dev_capacity.c — measure the device, auto-fit the mind (DEVFIT-1).
 *
 *  See dev_capacity.h + docs/architecture/device-capacity-mind-sizing-plan.md.
 *
 *  HOSTED-ONLY (built alongside student.c into boot/linux + boot/linux_x86_64;
 *  NOT on bare metal — student_stub.o resolves the ABI there). So this TU is OFF
 *  the bare-metal default link entirely and the bare-metal crown is untouched.
 *
 *  Build (one-math, wave-49): -O1 -ffp-contract=off. This TU does NO float math
 *  on the model path (it only computes integer arena sizes + picks a tier byte),
 *  so it cannot perturb st_forward. No VLA: it sizes no per-tier stack array.
 */
#include "dev_capacity.h"

#include <stdlib.h>   /* getenv, strtoul, malloc(via st_init_tier)            */
#include <string.h>   /* strcmp                                               */
#include <stdio.h>    /* FILE, fgets, sscanf — host /proc/meminfo only        */
#include <unistd.h>   /* sysconf(_SC_NPROCESSORS_ONLN) — the cores probe      */

/* ------------------------------------------------------------------ */
/* the per-tier dims, mirrored from student.h (ST_TIERS is file-static  */
/* in student.c; we read the public ST_*_<tier> macros here so the cost  */
/* tracks the SAME numbers).                                             */
/* ------------------------------------------------------------------ */
typedef struct { int d, dff, nlayer, nexpert; } dc_dims;
static const dc_dims DC_TIERS[ST_NTIER] = {
    /* S */ { ST_D_S, ST_DFF_S, ST_L_S, ST_E_S },
    /* M */ { ST_D_M, ST_DFF_M, ST_L_M, ST_E_M },
    /* L */ { ST_D_L, ST_DFF_L, ST_L_L, ST_E_L },
};

/* n_params for a tier == the EXACT offset assignment st_init_tier does
 * (student.c:251-266 / st_layout_for): embed + attn_norm + 4 attn proj +
 * ffn_norm + router + w1 + w3 + w2 + out_norm + out. Vocab V == ST_VOCAB,
 * fixed across tiers. Computed as size_t to avoid 32-bit overflow at L. */
static size_t dc_n_params(int tier)
{
    if (tier < 0 || tier >= ST_NTIER) return 0;
    const size_t V   = ST_VOCAB;
    const size_t D   = (size_t)DC_TIERS[tier].d;
    const size_t DFF = (size_t)DC_TIERS[tier].dff;
    const size_t L   = (size_t)DC_TIERS[tier].nlayer;
    const size_t E   = (size_t)DC_TIERS[tier].nexpert;
    size_t o = 0;
    o += V * D;            /* o_embed     [V][D]                  */
    o += L * D;            /* o_attn_norm [L][D]                  */
    o += 4 * L * D * D;    /* o_wq/o_wk/o_wv/o_wo each [L][D][D]   */
    o += L * D;            /* o_ffn_norm  [L][D]                  */
    o += L * E * D;        /* o_router    [L][E][D]               */
    o += 2 * L * E * DFF * D; /* o_w1/o_w3 each [L][E][DFF][D]     */
    o += L * E * D * DFF;  /* o_w2        [L][E][D][DFF]          */
    o += D;                /* o_out_norm  [D]                     */
    o += V * D;            /* o_out       [V][D]                  */
    return o;
}

size_t st_arena_bytes_for_tier(int tier)
{
    /* the arena st_init_tier mallocs: w | g | mu | vu, each n_params floats. */
    size_t n = dc_n_params(tier);
    if (!n) return 0;
    return n * 4u * sizeof(float);
}

/* ------------------------------------------------------------------ */
/* the mapping tier_of(ram, cores) — bottleneck-min, conservative      */
/* ------------------------------------------------------------------ */
int tier_of(const struct dev_capacity *c)
{
    if (!c) return ST_TIER_DEFAULT;

    /* RAM tier: the HARD ceiling (the model must FIT). The thresholds are the
     * COMPUTED per-tier arena cost * head-room (NOT magic numbers), so they
     * track the real cost — this is what makes [device-fit] non-vacuous. */
    const size_t M_FIT = st_arena_bytes_for_tier(ST_TIER_M) * DEVCAP_HEADROOM;
    const size_t L_FIT = st_arena_bytes_for_tier(ST_TIER_L) * DEVCAP_HEADROOM;

    int ram_tier;
    if      ((size_t)c->ram_bytes >= L_FIT) ram_tier = ST_TIER_L;
    else if ((size_t)c->ram_bytes >= M_FIT) ram_tier = ST_TIER_M;
    else                                    ram_tier = ST_TIER_S;

    /* cores tier: a SOFT preference (parallelism head-room). */
    int core_tier;
    if      (c->cores >= 6) core_tier = ST_TIER_L;
    else if (c->cores >= 3) core_tier = ST_TIER_M;
    else                    core_tier = ST_TIER_S;

    /* BOTTLENECK: the smaller wins — never pick a tier RAM cannot hold. */
    int t = ram_tier < core_tier ? ram_tier : core_tier;

    /* TRUST degrade: if RAM is a LOW-trust build constant (bare metal), IGNORE
     * it and fall back to the trustworthy cores-only tier. */
    if (c->ram_trust == DEVCAP_TRUST_LOW) t = core_tier;

    return t;   /* ST_TIER_S / _M / _L */
}

/* ------------------------------------------------------------------ */
/* the probe (host) + fixture injection                                */
/* ------------------------------------------------------------------ */

/* Bare-metal / unreadable fallback: a conservative LOW-trust RAM constant. We
 * pick a value that lands the cores bottleneck at the default M (so an
 * unmeasurable device behaves like today's M baby). 4 GiB is well above M_FIT
 * and below L_FIT, so combined with the LOW-trust cores-only override it is the
 * "ships at M" default. */
#define DEVCAP_RAM_FALLBACK  (4ull * 1024ull * 1024ull * 1024ull)

/* read MemTotal (kB) from /proc/meminfo; 0 if unreadable. */
static unsigned long dc_proc_memtotal_bytes(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    unsigned long kb = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) break;
    }
    fclose(f);
    return kb ? kb * 1024ul : 0ul;
}

static int dc_env_tier(const char *v)
{
    if (!v) return -1;
    if (!strcmp(v, "S") || !strcmp(v, "s")) return ST_TIER_S;
    if (!strcmp(v, "M") || !strcmp(v, "m")) return ST_TIER_M;
    if (!strcmp(v, "L") || !strcmp(v, "l")) return ST_TIER_L;
    return -1;
}

void dev_capacity_probe(struct dev_capacity *out)
{
    if (!out) return;
    out->cores       = 1;
    out->ram_bytes   = DEVCAP_RAM_FALLBACK;
    out->cores_trust = DEVCAP_TRUST_HIGH;
    out->ram_trust   = DEVCAP_TRUST_LOW;   /* conservative default            */

    /* --- raw host measurement (host: /proc/meminfo + sysconf) --- */
    unsigned long ram = dc_proc_memtotal_bytes();
    if (ram) { out->ram_bytes = ram; out->ram_trust = DEVCAP_TRUST_HIGH; }

#if defined(_SC_NPROCESSORS_ONLN)
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc >= 1) { out->cores = (unsigned int)nproc; out->cores_trust = DEVCAP_TRUST_HIGH; }
#endif

    /* --- fixture overrides (CI / profile injection; plan §6, risk #6) --- */
    const char *e_ram   = getenv("PKERNEL_DEVICE_RAM_BYTES");
    const char *e_cores = getenv("PKERNEL_DEVICE_CORES");
    const char *e_rtr   = getenv("PKERNEL_DEVICE_RAM_TRUST");
    const char *e_tier  = getenv("PKERNEL_DEVICE_TIER");

    if (e_ram) {
        out->ram_bytes = strtoul(e_ram, NULL, 10);
        out->ram_trust = DEVCAP_TRUST_HIGH;
    }
    if (e_cores) {
        unsigned long c = strtoul(e_cores, NULL, 10);
        out->cores = c ? (unsigned int)c : 1u;
        out->cores_trust = DEVCAP_TRUST_HIGH;
    }
    if (e_rtr) {
        unsigned long tr = strtoul(e_rtr, NULL, 10);
        out->ram_trust = (tr <= DEVCAP_TRUST_HIGH) ? (unsigned int)tr : DEVCAP_TRUST_HIGH;
    }

    /* PKERNEL_DEVICE_TIER forces the chosen tier DIRECTLY (the verdict's hook):
     * synthesize a profile whose tier_of() == the forced tier, HIGH trust, so
     * the whole pipeline (tier_of + step-down) still runs honestly. */
    int forced = dc_env_tier(e_tier);
    if (forced == ST_TIER_S) {
        out->ram_bytes = (unsigned long)st_arena_bytes_for_tier(ST_TIER_S) * DEVCAP_HEADROOM;
        out->cores = 1; out->ram_trust = DEVCAP_TRUST_HIGH;
    } else if (forced == ST_TIER_M) {
        out->ram_bytes = (unsigned long)st_arena_bytes_for_tier(ST_TIER_M) * DEVCAP_HEADROOM;
        out->cores = 4; out->ram_trust = DEVCAP_TRUST_HIGH;
    } else if (forced == ST_TIER_L) {
        out->ram_bytes = (unsigned long)st_arena_bytes_for_tier(ST_TIER_L) * DEVCAP_HEADROOM;
        out->cores = 8; out->ram_trust = DEVCAP_TRUST_HIGH;
    }
}

/* ------------------------------------------------------------------ */
/* the production sizing wrapper + alloc-fail step-down                 */
/* ------------------------------------------------------------------ */
int st_init_device(st_model *m, uint32_t seed)
{
    if (!m) return ST_E_ARG;

#ifdef DEVFIT_IGNORE_MEASURE
    /* FALSIFIER: hardcode the LARGEST tier + DISABLE the step-down. A small
     * device then tries to malloc the L arena and OOMs/fails — proving the
     * measurement is load-bearing (the [device-fit] cert goes RED). */
    return st_init_tier(m, seed, ST_TIER_L);
#else
    struct dev_capacity c;
    dev_capacity_probe(&c);
    int tier = tier_of(&c);

    /* alloc-fail STEP-DOWN (L->M->S): the measurement proposes, the allocator
     * disposes. A lying/optimistic RAM number degrades to a FITTING mind
     * instead of a dead node. st_init_tier returns ST_E_OOM (student.c:270)
     * when the arena malloc fails. */
    for (;;) {
        int rc = st_init_tier(m, seed, tier);
        if (rc == ST_OK) return ST_OK;
        if (rc != ST_E_OOM) return rc;   /* a non-OOM error is not retryable  */
        if (tier == ST_TIER_S) return ST_E_OOM;  /* even S won't fit: give up */
        tier = (tier == ST_TIER_L) ? ST_TIER_M : ST_TIER_S;  /* step down     */
    }
#endif
}

/* ------------------------------------------------------------------ */
/* SS-4 reconciliation: RULE [device-ceiling-fleet-grows-within]        */
/* ------------------------------------------------------------------ */
int st_grow_ceiling_for_tier(int tier)
{
    if (tier < 0 || tier >= ST_NTIER) return 0;
    return DC_TIERS[tier].nexpert;   /* ST_E_S=2 / ST_E_M=4 / ST_E_L=8 */
}

int st_fleet_expert_target(int fleet_cap_experts, int tier)
{
    int ceiling = st_grow_ceiling_for_tier(tier);
    if (ceiling <= 0) ceiling = ST_E_M;   /* fail-safe to the default tier    */

    int e_target = fleet_cap_experts;
    if (e_target < ST_TOPK)  e_target = ST_TOPK;   /* never below K_min       */
    if (e_target > ceiling)  e_target = ceiling;   /* DEVICE CEILING WINS      */
    return e_target;
}
