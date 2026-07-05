/* ------------------------------------------------------------------------
 * cagg_origin_probe.c — [unbounded-dkva-origin] (cross-audit correction #3).
 *
 * The [unbounded-flat] cert asserts the DKVA_CAGG ORIGIN axis is sized by R:
 *     sizeof(DKVA_CAGG.slot)/sizeof(DKVA_CAGG_SLOT) == DREGION_MAX
 * but it only ever compiles at the shipping default DREGION_MAX==DNODE_MAX==64,
 * where that equality is VACUOUS: a regression to slot[DNODE_MAX] (the pre-U-0
 * dense cagg[N]) still reads 64==64 and passes. This probe closes that hole.
 *
 * The Makefile builds it against the REAL kernel headers with the two axes
 * DELIBERATELY SPLIT — -DDREGION_MAX=32 -DDNODE_MAX=256 (R < N) — so the origin
 * axis is measurable INDEPENDENTLY of the fleet axis:
 *   - a correct origin axis (slot[DREGION_MAX]) has 32 slots  -> PASS,
 *   - a regressed origin axis (slot[DNODE_MAX]) has 256 slots -> the
 *     _Static_assert below fails to compile AND the runtime check RED-flags.
 * The MEMBER axis (exp[]/got[], honestly still fleet-N sized this slice, dkva.h)
 * is printed alongside to show the split is real: origin==R while member==N.
 *
 * This is the arm the dkva-cagg crown re-bless actually needs (the ORIGIN-axis
 * O(N)->O(R) decouple), made falsifiable rather than tautological.
 * ---------------------------------------------------------------------- */
#include <stdio.h>
#include "dkva.h"      /* DKVA_CAGG / DKVA_CAGG_SLOT (REAL) + DREGION_MAX/DNODE_MAX */

/* The probe is only meaningful when R and N genuinely differ; the Makefile
 * builds it that way. A build at R==N is a mis-invocation, not a pass. */
#if DREGION_MAX >= DNODE_MAX
#error "cagg_origin_probe must be built with DREGION_MAX < DNODE_MAX (R<N) to be NON-VACUOUS (cross-audit #3)"
#endif

/* the ORIGIN axis is R (DREGION_MAX), NOT fleet N (DNODE_MAX). A regression that
 * re-indexes the origin slot array by DNODE_MAX makes this 256 != 32 -> RED. */
_Static_assert(sizeof(((DKVA_CAGG*)0)->slot) / sizeof(DKVA_CAGG_SLOT) == (unsigned)DREGION_MAX,
    "DKVA_CAGG origin axis must be DREGION_MAX (R) slots, not fleet DNODE_MAX (N) (§4)");
_Static_assert(sizeof(((DKVA_CAGG*)0)->slot) / sizeof(DKVA_CAGG_SLOT) != (unsigned)DNODE_MAX,
    "DKVA_CAGG origin axis re-coupled to fleet N (the pre-U-0 dense cagg[DNODE_MAX] regression)");

int main(void)
{
    unsigned origin_slots = (unsigned)(sizeof(((DKVA_CAGG*)0)->slot) / sizeof(DKVA_CAGG_SLOT));
    unsigned member_axis  = (unsigned)sizeof(((DKVA_CAGG_SLOT*)0)->exp);   /* [DNODE_MAX], honest */

    printf("[unbounded-dkva-origin] R(DREGION_MAX)=%d  N(DNODE_MAX)=%d  "
           "origin_slots=%u  member_axis=%u\n",
           (int)DREGION_MAX, (int)DNODE_MAX, origin_slots, member_axis);

    int fail = 0;
    if (origin_slots != (unsigned)DREGION_MAX) {
        printf("  RED: origin axis %u != R=%d (re-coupled to fleet N)\n",
               origin_slots, (int)DREGION_MAX);
        fail = 1;
    }
    if (origin_slots == (unsigned)DNODE_MAX) {
        printf("  RED: origin axis grew to fleet N=%d (the dense cagg[N] regression)\n",
               (int)DNODE_MAX);
        fail = 1;
    }
    if (member_axis != (unsigned)DNODE_MAX) {
        /* honesty guard: the member axis IS still fleet-N sized this slice; if it
         * ever silently shrank/changed, the "split is real" claim is stale. */
        printf("  NOTE: member axis %u != N=%d (the honest fleet-sized axis moved)\n",
               member_axis, (int)DNODE_MAX);
    }
    if (fail) {
        printf("[unbounded-dkva-origin]  RED — the DKVA_CAGG origin axis is not O(R)\n");
        return 1;
    }
    printf("[unbounded-dkva-origin]  PASS — origin axis is R (%d) and DISTINCT from fleet N (%d)\n",
           (int)DREGION_MAX, (int)DNODE_MAX);
    return 0;
}
