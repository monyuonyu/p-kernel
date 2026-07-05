/* ------------------------------------------------------------------------
 * unbounded_test.c — [unbounded-flat]: the CURE, measured on the REAL kernel.
 *
 * unbounded_n_design.md §8. The thesis (§2): "N is unbounded IFF per-node cost
 * is independent of N." This cert links NO kernel .c — it #includes the REAL
 * shipped headers (drpc.h / kdds.h / dkva.h / nodemap.h) and measures the REAL
 * kernel structures, so a regression that re-imports a fleet-N dimension is
 * caught HERE, not by inspection (audit correction 2: NOT a mock struct).
 *
 * Two teeth:
 *   (A) COMPILE-TIME budget gate — sizeof the REAL per-node coordinator
 *       aggregation state (DKVA_CAGG, from dkva.h) is bounded by a byte budget
 *       derived from its honest fields. A [4096]/[1024] literal ADDED to the
 *       measured struct (DKVA_CAGG_SLOT) overruns the budget → RED. (Proven by
 *       adding the field, seeing RED, reverting — see the wave notes.)
 *   (B) RUNTIME R-bound — drive N ∈ {64,256,1024,4096} LOGICAL fleet ids
 *       through the REAL nodemap primitive (nodemap.h) and assert the region
 *       occupancy never exceeds R and the fleet still WORKS (region forms, HRW
 *       resolves an in-region owner). Genericity: ids + RTTs are seed-shuffled.
 *
 * The other two arms live in the Makefile (design §9, "build the disease binary,
 * never hand-run"): [unbounded-coupling] (recompile the headers at DNODE_MAX
 * 64/256/1024, prove the R-cost is constant) and [unbounded-disease] (the
 * wave-45 control fails to compile). Exit 0 = every gate here green.
 * ---------------------------------------------------------------------- */
#include <stdio.h>
#include <string.h>

#include "kdds.h"      /* DNODE_MAX, DREGION_MAX, KDDS_TOPIC_MAX, kdds_topics */
#include "dkva.h"      /* DKVA_CAGG / DKVA_CAGG_SLOT / DKVA_RESP_PKT (REAL)   */
#include "nodemap.h"   /* NODEMAP — the region-local primitive (REAL)        */
#include "drpc.h"      /* DNODE / dnode_table (the fleet-sized contrast)      */

/* ---- the simulated fleet sizes (the g22 sweep) --------------------------- */
#define N_SMALL   64u
#define N_MED     256u
#define N_BIG     1024u
#define N_HUGE    4096u
#define NSWEEP    4
#define TAU_MS    50u            /* region latency threshold (region.h)       */
#define F_CAP     32u            /* remote-region delegate directory (§2)     */

/* ==========================================================================
 * TOOTH A — the REAL per-node coordinator-aggregation footprint, budgeted.
 * A [4096] added to DKVA_CAGG_SLOT (dkva.h) blows both asserts → RED here AND
 * bloats the shipping kernel .bss by DREGION_MAX×. The budget is derived from
 * the struct's HONEST fields (a RESP pkt + small scalars + the two node-id
 * member axes), so only an ADDED/MOVED cap trips it.
 * ======================================================================== */
#define CAGG_SLOT_BUDGET  ((unsigned long)(sizeof(DKVA_RESP_PKT) + 2u*(unsigned)DNODE_MAX + 64u))
#define CAGG_BUDGET       ((unsigned long)((unsigned long)DREGION_MAX * CAGG_SLOT_BUDGET \
                                           + sizeof(NODEMAP) + 64u))
_Static_assert(sizeof(DKVA_CAGG_SLOT) <= CAGG_SLOT_BUDGET,
    "DKVA_CAGG_SLOT exceeds its honest byte budget — a cap was added/moved (§8)");
_Static_assert(sizeof(DKVA_CAGG) <= CAGG_BUDGET,
    "DKVA_CAGG exceeds R*slot budget — the per-node aggregation state is not O(R) (§8)");
/* the ORIGIN axis is bounded by R: the per-origin slot array has EXACTLY
 * DREGION_MAX entries (the nodemap that indexes it has capacity R). */
_Static_assert(sizeof(((DKVA_CAGG*)0)->slot) / sizeof(DKVA_CAGG_SLOT) == DREGION_MAX,
    "coordinator ORIGIN axis must be R slots, not fleet N (§4)");

/* deterministic per-seed RNG (LCG) — genericity: no hardcoded node layout */
static unsigned long g_rng;
static void     rng_seed(unsigned long s){ g_rng = s ? s : 0x9E3779B9UL; }
static unsigned  rng_next(void){ g_rng = g_rng*6364136223846793005UL + 1442695040888963407UL; return (unsigned)(g_rng>>33); }

/* HRW / rendezvous hash over an explicit member set (mirrors placement.c
 * st_expert_owners_in). Region-local: evaluated over ≤ R members. */
static unsigned hrw_mix(UW id, UW key){
    unsigned h = 2166136261u;
    h = (h ^ (id       & 0xff)) * 16777619u;
    h = (h ^ ((id>>8)  & 0xff)) * 16777619u;
    h = (h ^ ((id>>16) & 0xff)) * 16777619u;
    h = (h ^ (key      & 0xff)) * 16777619u;
    return h;
}
static UW hrw_owner_in(const NODEMAP *m, UW key){
    UW best_id = 0xFFFFFFFFUL; unsigned best_w = 0; int found = 0;
    for (UH s = 0; s < (UH)DREGION_MAX; s++){
        if (!m->used[s]) continue;
        unsigned w = hrw_mix(m->id[s], key);
        if (!found || w > best_w){ best_w = w; best_id = m->id[s]; found = 1; }
    }
    return found ? best_id : 0xFFFFFFFFUL;
}

/* Drive ONE node through a fleet of `simN` logical ids using the REAL nodemap.
 * Returns peak region occupancy; sets *works. Genericity: ids + RTTs shuffled;
 * a dense RTT cluster LARGER than R is deliberately generated so admission is
 * FORCED to bound at R (the anti-theater runtime tooth). */
static UH cure_drive(unsigned simN, int *works_out)
{
    NODEMAP region, directory;
    nodemap_init(&region,    (UH)DREGION_MAX);   /* cap R */
    nodemap_init(&directory, (UH)F_CAP);         /* cap F */

    UW self_id = 0x10000000UL ^ (rng_next() & 0xffffff);
    UH peak = 0;
    for (unsigned i = 0; i < simN; i++){
        UW peer_id = (UW)(0x20000000UL + (unsigned long)i*2654435761UL);
        peer_id ^= (rng_next() & 0xffff);
        if (peer_id == self_id) continue;
        unsigned rtt = rng_next() % 120u;                 /* 0..119 ms */
        if (rtt <= TAU_MS){
            (void)nodemap_admit(&region, peer_id);        /* bounded at R  */
        } else {
            (void)nodemap_admit(&directory, peer_id % 97u); /* bounded at F */
        }
        UH occ = nodemap_count(&region);
        if (occ > peak) peak = occ;
    }

    /* --- it must actually FUNCTION -------------------------------------- */
    int works = 1;
    UH occ = nodemap_count(&region);
    if (occ == 0) works = 0;                              /* a region formed */
    if (occ > (UH)DREGION_MAX) works = 0;                 /* never exceeds R  */
    if (nodemap_count(&directory) > (UH)F_CAP) works = 0; /* never exceeds F  */
    UW owner = hrw_owner_in(&region, 0xABCDu);            /* HRW resolves ... */
    if (owner == 0xFFFFFFFFUL) works = 0;
    if (nodemap_find(&region, owner) == NODEMAP_NOSLOT) works = 0; /* ...in-region */

    if (works_out) *works_out = works;
    return peak;
}

int main(void)
{
    int fails = 0;
    const unsigned Ns[NSWEEP] = { N_SMALL, N_MED, N_BIG, N_HUGE };

    printf("======================================================================\n");
    printf(" [unbounded-flat] cert — REAL per-node kernel footprint vs fleet N\n");
    printf("   R (DREGION_MAX)=%d   wire DNODE_MAX=%d   KDDS_TOPIC_MAX=%d\n",
           (int)DREGION_MAX, (int)DNODE_MAX, (int)KDDS_TOPIC_MAX);
    printf("======================================================================\n\n");

    /* ---- TOOTH A: measure the REAL kernel structures ------------------- */
    printf("[unbounded-flat] A  measured KERNEL structs (dkva.h / kdds.h / nodemap.h)\n");
    printf("   sizeof(DKVA_CAGG)       = %-8lu B   (budget %lu; per-node aggregation state)\n",
           (unsigned long)sizeof(DKVA_CAGG), CAGG_BUDGET);
    printf("   sizeof(DKVA_CAGG_SLOT)  = %-8lu B   (budget %lu; one origin slot)\n",
           (unsigned long)sizeof(DKVA_CAGG_SLOT), CAGG_SLOT_BUDGET);
    printf("   origin slots            = %-8d     (== R; the O(N)->O(R) axis)\n",
           (int)(sizeof(((DKVA_CAGG*)0)->slot)/sizeof(DKVA_CAGG_SLOT)));
    printf("   sizeof(NODEMAP)         = %-8lu B   (the region-local primitive)\n",
           (unsigned long)sizeof(NODEMAP));
    printf("   sizeof(kdds_topics)     = %-8lu B   (topic table; sized by R)\n",
           (unsigned long)sizeof(kdds_topics));
    printf("   sizeof(dnode_table)     = %-8lu B   (FLEET-sized, DNODE_MAX; deferred)\n",
           (unsigned long)sizeof(dnode_table));
    /* budget gate (the [4096]->RED tooth). The _Static_asserts above already
     * enforce this at compile time; assert again at runtime for the log. */
    if (sizeof(DKVA_CAGG) <= CAGG_BUDGET && sizeof(DKVA_CAGG_SLOT) <= CAGG_SLOT_BUDGET)
        printf("   => budget gate: sizeof(DKVA_CAGG) within R*slot budget      PASS\n");
    else { printf("   => budget gate: RED — a cap was added to the measured struct\n"); fails++; }
    printf("\n");

    /* ---- TOOTH B: runtime R-bound on the REAL nodemap ------------------ */
    printf("[unbounded-flat] B  runtime: N logical ids through the REAL nodemap (cap R)\n");
    printf("  %-8s %-14s %-12s %s\n", "simN", "peak occ", "<= R?", "works");
    UH occ_at[NSWEEP];
    for (int k = 0; k < NSWEEP; k++){
        unsigned simN = Ns[k];
        UH peak_occ = 0; int all_work = 1;
        for (unsigned seed = 1; seed <= 8; seed++){        /* multi-seed genericity */
            rng_seed(0xC0FFEEUL * seed + simN);
            int works; UH occ = cure_drive(simN, &works);
            if (occ > peak_occ) peak_occ = occ;
            if (!works) all_work = 0;
        }
        occ_at[k] = peak_occ;
        printf("  %-8u %-14u %-12s %s\n", simN, (unsigned)peak_occ,
               (peak_occ <= (UH)DREGION_MAX) ? "yes" : "NO(RED)",
               all_work ? "YES" : "NO(RED)");
        if (peak_occ > (UH)DREGION_MAX){ printf("    RED: region occupancy %u > R=%d\n",
               (unsigned)peak_occ, (int)DREGION_MAX); fails++; }
        if (!all_work){ printf("    RED: fleet did not form/route at simN=%u\n", simN); fails++; }
    }
    /* the occupancy SATURATES at R and does NOT grow as the fleet grows:
     * 256 == 1024 == 4096 (a 16x fleet range, ZERO change) = "independent of N". */
    int occ_flat_at_scale = (occ_at[1] == occ_at[2]) && (occ_at[2] == occ_at[3]);
    if (!occ_flat_at_scale){
        printf("  RED: region occupancy grew with the fleet at scale (%u/%u/%u)\n",
               (unsigned)occ_at[1], (unsigned)occ_at[2], (unsigned)occ_at[3]); fails++;
    }
    printf("  => occupancy BOUNDED <= R=%d; FLAT at scale: N=256/1024/4096 -> %u == %u == %u\n",
           (int)DREGION_MAX, (unsigned)occ_at[1], (unsigned)occ_at[2], (unsigned)occ_at[3]);
    printf("     (fleet grew 16x from 256 to 4096; per-node region cost did NOT move)\n\n");

    printf("======================================================================\n");
    if (fails == 0){
        printf(" [unbounded-flat]  PASS — per-node coordinator+region cost is O(R), bounded\n");
        printf("   (coupling probe + disease control are the Makefile's coupling/disease gates)\n");
        printf("======================================================================\n");
        return 0;
    }
    printf(" [unbounded-flat]  %d RED — per-node cost is NOT independent of fleet N.\n", fails);
    printf("======================================================================\n");
    return 1;
}
