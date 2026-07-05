/* ------------------------------------------------------------------------
 * disease.c — the wave-45 CONTROL binary (unbounded_n_design.md §3/§9; audit
 * correction 3). NOT a printf of 2*N*N. This is the pre-localization
 * GLOBAL-view sizing with the wire naively bumped; the Makefile builds it with
 *
 *     -DDNODE_MAX=1024 -DKDDS_DKVA_PREOPEN_SCALE=DNODE_MAX
 *
 * and the REAL shipped compile-time gates REFUSE to build it. Two independent,
 * mechanical failures — either one is a genuine "does not compile":
 *
 *   (1) kdds.h's pre-open-fits-table _Static_assert (the wave-48 gate):
 *       re-coupling the dkva pre-open to fleet N makes it 3*1024 = 3072 topics
 *       against a table sized ∝ R (6*64+16 = 400). 3072+16 > 400 → RED at the
 *       point kdds.h is #included below. THIS is the exact wave-48 shape (the
 *       star went blind to nodes 46..63 on mk_pino's phone) — now a build error.
 *
 *   (2) the pmesh global-view beacon at the bumped wire is 8 + 4*DNODE_MAX =
 *       8 + 4096 = 4104 bytes, past PMESH_DATA_MAX (1380). The routing protocol
 *       physically cannot broadcast a global route table past ~344 nodes
 *       (design §1.4 — the purest proof that the global VIEW, not the constant,
 *       is the cap). Asserted below.
 *
 * A successful compile of this file would mean the cure is UNFALSIFIED theater,
 * so the Makefile treats a clean build as a HARD cert FAILURE.
 * ---------------------------------------------------------------------- */
#include "kdds.h"     /* trips gate (1) at include time (wave-48 overflow)  */
#include "pmesh.h"    /* PMESH_BEACON_PKT / PMESH_DATA_MAX                   */

/* gate (2): the global-view beacon must fit the UDP/MTU payload. */
_Static_assert(sizeof(PMESH_BEACON_PKT) <= PMESH_DATA_MAX,
    "DISEASE(wave-48/§1.4): global-view pmesh beacon (8 + 4*DNODE_MAX) exceeds "
    "the UDP/MTU payload at the bumped wire — the routing protocol cannot "
    "broadcast a global route table past ~344 nodes. Localization (region-local "
    "beacons of <= R+F entries), not a bigger constant, is the fix.");

int main(void) { return 0; }
