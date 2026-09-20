#ifndef _FED_ID_H_
#define _FED_ID_H_

#include "drpc.h"

/* Federation F1 -- composite (region_id, local_id) encoding CANDIDATE.
 * docs/architecture/20-architecture/federation.md §2.2/§4-F1.
 *
 * SCOPE: this header defines ONE candidate wire encoding for the inter-leaf
 * composite ID the design doc proposes -- reinterpreting the TOP 8 bits of
 * GOBJ_MAKE's existing 24-bit `local` field (drpc.h:157-160) as a region_id,
 * leaving the low 16 bits as the true within-region local id. It is NOT
 * wired into drpc.c/dkva.c/kdds.c or any live GOBJ producer/consumer -- no
 * behavior changes anywhere else in the tree. That wiring is a LATER F1
 * slice, deliberately deferred: federation.md §5.3 lists this encoding
 * choice itself as an open question ("region_id を GOBJ の 24-bit local に
 * 畳むか、新フィールドを足すか") plus a SEPARATE open question (§5.3-2,
 * inter-coordinator rsum ordering) that a live wiring would also need to
 * answer. Shipping only the encoding, tested in isolation, is the smallest
 * honest step that does not silently pick an answer to either question for
 * the rest of the codebase.
 *
 * BACKWARD COMPAT (by construction, see fed_id_self_test): every current
 * GOBJ_MAKE call site in the tree passes a semaphore/object id that fits in
 * 16 bits (arch/common/drpc.c:303,677,687,693,785 -- all T-Kernel object
 * ids), so FED_LOCAL_MAKE(0, x) for x < 65536 reproduces today's plain
 * 24-bit local field exactly. R=1 (single region, the default today) is
 * representationally a no-op under this encoding. */

#ifndef FED_ID_BROKEN_PACK
/* production encoding: top 8 bits of the 24-bit local field = region_id */
#define FED_LOCAL_MAKE(region_id, within16) \
    ((((UW)(region_id) & 0xFFUL) << 16) | ((UW)(within16) & 0xFFFFUL))
#define FED_LOCAL_REGION(local24)   ((UB)(((UW)(local24) >> 16) & 0xFFUL))
#define FED_LOCAL_WITHIN(local24)   ((UH)((UW)(local24) & 0xFFFFUL))
#else
/* falsifier-only: swap the split point so round-trip breaks -- proves the
 * self-test actually exercises the shift math instead of trivially passing
 * because both directions share the same bug (SURVIVAL_L2_NO_ESCALATE-style
 * falsifier, see run_federation_f1.sh). Never built by default. */
#define FED_LOCAL_MAKE(region_id, within16) \
    ((((UW)(region_id) & 0xFFUL) << 8) | ((UW)(within16) & 0xFFFFFFUL))
#define FED_LOCAL_REGION(local24)   ((UB)(((UW)(local24) >> 8) & 0xFFUL))
#define FED_LOCAL_WITHIN(local24)   ((UH)((UW)(local24) & 0xFFFFFFUL))
#endif

/* Composite GOBJ helpers -- build/read a federation object id the same way
 * GOBJ_MAKE/GOBJ_NODE/GOBJ_LOCAL do (drpc.h:157-160), with the region split
 * folded into the local half. `node` keeps meaning "leaf-local node id"
 * exactly as it does today (leaf packets are unchanged per the design). */
#define FED_GOBJ_MAKE(node, region_id, within16) \
    GOBJ_MAKE((node), FED_LOCAL_MAKE((region_id), (within16)))
#define FED_GOBJ_REGION(g)  FED_LOCAL_REGION(GOBJ_LOCAL(g))
#define FED_GOBJ_WITHIN(g)  FED_LOCAL_WITHIN(GOBJ_LOCAL(g))

/* [fed-id-roundtrip] self-test: pack/unpack round-trips across the boundary
 * values, and region_id=0 reproduces the plain (pre-F1) 24-bit local field.
 * Hosted-only test helper (arch/linux/{x86_64,aarch64}/usermain.c wire this
 * as the `fed test` verb) -- this header has no .c of its own and is never
 * included from a bare-metal TU, so it carries zero crown risk by
 * construction, not by a build-time guard. */
static inline INT fed_id_self_test_run(void)
{
    INT fail = 0;

    /* R=1 backward compat: region_id=0 must reproduce the plain local field
     * for every value the current tree actually passes (<=16 bits). */
    {
        UW within = 12345u;
        UW packed = FED_LOCAL_MAKE(0, within);
        if (packed != within) fail = 1;
    }

    /* round-trip across region_id boundary values (0, 1, mid, 0xFF) and
     * within-region id boundary values (0, 1, mid, 0xFFFF). */
    {
        static const UB regions[] = { 0, 1, 128, 255 };
        static const UH withins[] = { 0, 1, 32768, 65535 };
        for (INT ri = 0; ri < 4; ri++) {
            for (INT wi = 0; wi < 4; wi++) {
                UB r = regions[ri];
                UH w = withins[wi];
                UW local24 = FED_LOCAL_MAKE(r, w);
                if (FED_LOCAL_REGION(local24) != r)   fail = 1;
                if (FED_LOCAL_WITHIN(local24) != w)   fail = 1;
                /* the packed value must still fit the existing 24-bit GOBJ
                 * local field (drpc.h:158's 0x00FFFFFF mask) -- otherwise
                 * this "candidate" encoding would silently truncate on the
                 * very macro it claims to be compatible with. */
                if ((local24 & ~0x00FFFFFFUL) != 0)   fail = 1;
            }
        }
    }

    /* full GOBJ round-trip through the existing node/local split too. */
    {
        UW g = FED_GOBJ_MAKE(7, 42, 9000);
        if (GOBJ_NODE(g) != 7)          fail = 1;
        if (FED_GOBJ_REGION(g) != 42)   fail = 1;
        if (FED_GOBJ_WITHIN(g) != 9000) fail = 1;
    }

    return fail;
}

#endif /* _FED_ID_H_ */
