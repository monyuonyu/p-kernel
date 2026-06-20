/*
 *  placement_stub.c — link-only kernel-symbol stubs for the SS-5 host cert.
 *
 *  lookup.c (the shared HRW primitive placement.c reuses) references a few
 *  kernel globals/functions for its LIVE/egocentric wrappers + L1 cache
 *  (region_recompute/region_is_member, dnode_table[], drpc_my_node,
 *  tk_get_otm). The SS-5 cert (placement_test.c) drives ONLY the PURE
 *  cores st_expert_owner_in()/st_expert_owners_in() -> lookup_responsible(),
 *  which read NO globals — so these stubs are NEVER executed; they exist
 *  solely to satisfy the host linker. Signatures match the kernel headers
 *  (included below) so the types are byte-correct.
 */
#include "region.h"   /* region_recompute / region_is_member               */
#include "drpc.h"     /* DNODE, dnode_table[], drpc_my_node, DNODE_MAX      */

DNODE dnode_table[DNODE_MAX];
UB    drpc_my_node = 0xFF;

void region_recompute(void) { }
BOOL region_is_member(UB node) { (void)node; return FALSE; }

/* tk_get_otm — t-kernel system-time getter the L1 cache stamps with.
 * Never reached by the SS-5 cert; return a zero clock. */
ER tk_get_otm(SYSTIM *tim)
{
    if (tim) { tim->hi = 0; tim->lo = 0; }
    return 0;
}
