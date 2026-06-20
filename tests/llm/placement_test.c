/*
 *  placement_test.c — host cert for SS-5 deterministic expert placement map
 *  (arch/common/placement.c, special-structure-mind.md §6 / §8 item 6).
 *
 *  SS-5 makes "which node holds which expert?" a NOCENTRAL LOCAL function:
 *  place expert e on the node that WINS a rendezvous hash (HRW) of
 *  (expert-id, alive-member-set). Every node computes the IDENTICAL map
 *  from the SAME membership view — no broadcast, no vote, no leader. It is
 *  a thin shim over the EXISTING HRW primitive lookup_responsible()
 *  (arch/common/lookup.c); sha256-based => byte-identical across ABIs.
 *
 *  Certs (all self-contained, IN-PROCESS — no network; the cores are PURE
 *  functions of (expert_id, members)):
 *    [place-deterministic] given ONE synthetic alive-member set, the owner
 *                          of each expert is IDENTICAL no matter which node
 *                          computes it (the function reads NO self-id, so
 *                          "as several nodes" == recompute == same map).
 *                          Cross-arch identity is by construction (HRW is
 *                          sha256 + byte-picked; lookup.h contract).
 *    [place-rehome]        kill the OWNER of one expert from the member set
 *                          -> that expert re-homes to the next HRW winner
 *                          AND every OTHER expert's owner is UNCHANGED
 *                          (HRW minimal disruption). Falsifiable: a non-HRW
 *                          modulo-N placement reshuffles MANY experts when
 *                          the set shrinks — the cert distinguishes.
 *    [place-balance]       experts spread across a uniform member set
 *                          (reported, NOT over-claiming perfect balance).
 *
 *  HONEST (scope): this is the placement MAP + cert ONLY. Remote-expert
 *  EXECUTION (firing an expert on its owner over the mesh) is SS-6 and is
 *  DEFERRED. On a single node the alive set is {self}, so every expert
 *  maps to the one node — the map is SS-6's foundation. A true multi-node
 *  live placement-convergence run is a deferred [live] row; this cert
 *  drives the REAL function with synthetic member sets.
 *
 *  Build: cc -std=c11 -O1 -Wall -Wextra -Werror=vla (run_ss5.sh) over
 *  placement.c + lookup.c + relay/sha256.c + a tiny kernel-symbol stub
 *  (the PURE cores never touch dnode_table[]/region — see stub below).
 */
#include "placement.h"
#include "drpc.h"

#include <stdio.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

/* puts-style sink for st_placement_self_test (the in-kernel `place` verb
 * uses this same callback). */
static void emit(const char *s) { fputs(s, stdout); }

/* A non-HRW control (modulo-N) — used to PROVE [place-rehome] falsifiable. */
static int modN_owner(unsigned e, const unsigned char *m, int n)
{
    return (int)m[(int)e % n];
}

int main(void)
{
    printf("== SS-5 deterministic expert placement map (placement.c) ==\n\n");

    /* ---------------------------------------------------------------- *
     * (0) Run the SHIPPED in-kernel self-test verbatim (the same code  *
     *     the `place` shell verb runs). Its internal asserts cover all  *
     *     three certs; we then RE-derive the key sub-claims explicitly  *
     *     below so the cert is legible + extra-falsifiable.             *
     * ---------------------------------------------------------------- */
    printf("[self-test] st_placement_self_test (the in-kernel `place` verb):\n");
    int st_rc = st_placement_self_test(emit);
    CHECK(st_rc == 0, "st_placement_self_test() returns 0 (all internal certs PASS)");
    printf("\n");

    /* a synthetic alive-member set: 5 distinct node ids, NOT 0..4 (so we
     * also prove the map does not assume contiguous-from-zero ids). */
    unsigned char members[5] = { 3, 9, 17, 40, 62 };
    int NM = 5;
    int NE = 8;   /* sweep 8 experts (>= L-tier ST_E_MAX) */

    /* ---- [place-deterministic] -------------------------------------- */
    /* compute the owner map; recompute 5x ("as 5 different nodes") and
     * demand byte-for-byte identity. */
    int base[8];
    int det = 1;
    for (int e = 0; e < NE; e++) {
        base[e] = st_expert_owner_in((unsigned char)e, members, NM);
        if (base[e] < 0) det = 0;
    }
    for (int pass = 0; pass < 5; pass++) {
        for (int e = 0; e < NE; e++) {
            if (st_expert_owner_in((unsigned char)e, members, NM) != base[e]) det = 0;
        }
    }
    /* every owner must be a MEMBER of the set */
    for (int e = 0; e < NE; e++) {
        int inset = 0;
        for (int i = 0; i < NM; i++) if (members[i] == (unsigned char)base[e]) inset = 1;
        if (!inset) det = 0;
    }
    printf("[place-deterministic] owner map over members {3,9,17,40,62}:\n  ");
    for (int e = 0; e < NE; e++) printf("e%d->n%d  ", e, base[e]);
    printf("\n");
    CHECK(det, "[place-deterministic] map identical across recomputes; every owner in the set");

    /* owners list (replica order) primary == single-owner */
    {
        unsigned char top[ST_PLACE_RMAX];
        int nr = st_expert_owners_in(0, members, NM, top, ST_PLACE_RMAX);
        CHECK(nr == ST_PLACE_RMAX && (int)top[0] == base[0],
              "[place-deterministic] owners_in()[0] == owner_in() (primary is the winner)");
    }

    /* ---- [place-rehome] --------------------------------------------- */
    /* kill the owner of expert 0; build the reduced set; assert ONLY the
     * experts owned by the victim move, all others UNCHANGED. */
    unsigned char victim = (unsigned char)base[0];
    unsigned char reduced[5];
    int NR = 0;
    for (int i = 0; i < NM; i++) if (members[i] != victim) reduced[NR++] = members[i];

    int rehome_ok = 1, hrw_moved = 0;
    for (int e = 0; e < NE; e++) {
        int after = st_expert_owner_in((unsigned char)e, reduced, NR);
        if (after < 0) { rehome_ok = 0; continue; }
        if (base[e] == (int)victim) {
            if (after == (int)victim) rehome_ok = 0;   /* must leave the dead node */
            hrw_moved++;
        } else {
            if (after != base[e]) { rehome_ok = 0; hrw_moved++; }  /* must NOT move */
        }
    }
    printf("[place-rehome] killed n%d: %d expert(s) re-homed, others unchanged\n",
           victim, hrw_moved);
    CHECK(rehome_ok, "[place-rehome] only the dead node's experts re-home; all others UNCHANGED");
    CHECK(hrw_moved >= 1, "[place-rehome] at least one expert (the dead node's) actually moved");

    /* falsifiability: the modulo-N control reshuffles MANY more experts. */
    int modn_moved = 0;
    for (int e = 0; e < NE; e++) {
        if (modN_owner((unsigned)e, members, NM) != modN_owner((unsigned)e, reduced, NR))
            modn_moved++;
    }
    printf("[place-rehome] falsifiable control: modulo-N moved %d experts vs HRW %d\n",
           modn_moved, hrw_moved);
    CHECK(modn_moved > hrw_moved,
          "[place-rehome] FALSIFIABLE: non-HRW (modulo-N) reshuffles strictly more (HRW is minimal)");

    /* ---- [place-balance] (honest, reported) ------------------------- */
    {
        int SWEEP = 32;
        unsigned char seen[5] = {0,0,0,0,0};
        int distinct = 0;
        for (int e = 0; e < SWEEP; e++) {
            int o = st_expert_owner_in((unsigned char)e, members, NM);
            for (int i = 0; i < NM; i++)
                if (members[i] == (unsigned char)o && !seen[i]) { seen[i] = 1; distinct++; break; }
        }
        printf("[place-balance] %d experts spread across %d/%d nodes (reported)\n",
               SWEEP, distinct, NM);
        CHECK(distinct >= 2, "[place-balance] experts NOT all piled on one node (spread reported, not perfect)");
    }

    /* ---- single-node honesty (SS-6 deferred) ------------------------ */
    {
        unsigned char solo[1] = { 42 };
        int solo_ok = 1;
        for (int e = 0; e < NE; e++)
            if (st_expert_owner_in((unsigned char)e, solo, 1) != 42) solo_ok = 0;
        CHECK(solo_ok, "single-node: every expert maps to the one node (SS-6 remote firing DEFERRED)");
    }

    printf("\n== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
