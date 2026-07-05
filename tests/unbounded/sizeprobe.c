/* ------------------------------------------------------------------------
 * sizeprobe.c — the [unbounded-coupling] measuring stick (audit correction 1).
 *
 * Compiled by the Makefile THREE times against the REAL kernel headers with
 * DNODE_MAX ∈ {64,256,1024} (drpc.h is now `#ifndef`-guarded so the wire
 * ceiling is overridable). It prints, as key=value pairs:
 *
 *   R-column  (sized by DREGION_MAX, an INDEPENDENT literal — MUST stay
 *              byte-for-byte CONSTANT as the wire widens):
 *                DREGION_MAX, KDDS_TOPIC_MAX, sizeof(kdds_topics),
 *                KDDS_DKVA_PREOPEN, sizeof(NODEMAP)
 *   fleet-col (sized by DNODE_MAX — MUST grow, proving the wire truly moved):
 *                sizeof(dnode_table)
 *
 * coupling_check.sh diffs the three outputs. This is the mechanical proof that
 * bumping DNODE_MAX to admit more nodes does NOT regrow the per-node R tables —
 * the whole point of splitting DREGION_MAX out of DNODE_MAX.
 * ---------------------------------------------------------------------- */
#include <stdio.h>
#include "kdds.h"      /* KDDS_TOPIC_MAX, KDDS_DKVA_PREOPEN, kdds_topics[]   */
#include "drpc.h"      /* DNODE_MAX, DREGION_MAX, dnode_table[]             */
#include "nodemap.h"   /* NODEMAP                                          */

int main(void)
{
    printf("DNODE_MAX=%d ",           (int)DNODE_MAX);
    printf("DREGION_MAX=%d ",         (int)DREGION_MAX);
    printf("KDDS_TOPIC_MAX=%d ",      (int)KDDS_TOPIC_MAX);
    printf("kdds_topics_bytes=%lu ",  (unsigned long)sizeof(kdds_topics));
    printf("dkva_preopen=%d ",        (int)KDDS_DKVA_PREOPEN);
    printf("nodemap_bytes=%lu ",      (unsigned long)sizeof(NODEMAP));
    printf("dnode_table_bytes=%lu\n", (unsigned long)sizeof(dnode_table));
    return 0;
}
