/*
 *  nodemap.h
 *  Fixed-capacity sparse peer table  {id → slot}  (unbounded_n_design.md §4)
 *
 *  THE HEART OF THE UNBOUNDED-N MIGRATION. Every historical `x[DNODE_MAX]`
 *  indexed by a dense node id assumed a GLOBAL view: the array dimension WAS
 *  the fleet size, so per-node state grew O(N). A nodemap replaces that dense
 *  index with an admission-bounded sparse slot table whose capacity is a
 *  COMPILE-TIME constant (DREGION_MAX = R). After conversion no data structure
 *  has a dimension proportional to fleet size N, so:
 *
 *     per-node state = O(R)  — independent of N   (§2)
 *
 *  The owner supplies the admission/eviction policy (region membership,
 *  directory recency, conversation LRU). The returned SLOT index replaces the
 *  old node-id index into the owner's parallel arrays (which shrink from
 *  [DNODE_MAX] to [DREGION_MAX]).
 *
 *  Storage is sized by DREGION_MAX; `cap` bounds live admission (cap ≤
 *  DREGION_MAX; a smaller cap models a smaller region R without a rebuild).
 *  Lookup/admit are O(cap) linear scans — cap is a small constant, and this
 *  is header-only (static inline) so it links into BOTH the bare-metal kernel
 *  and the host [unbounded-*] cert with one definition.
 */

#pragma once
#include "drpc.h"   /* UB/UW/UH/INT + DNODE_MAX + DREGION_MAX */

#define NODEMAP_NOSLOT  (-1)

typedef struct {
    UW  id[DREGION_MAX];    /* id[s] meaningful iff used[s]                 */
    UB  used[DREGION_MAX];  /* 1 = slot occupied                           */
    UH  count;              /* number of live slots (== admitted peers)    */
    UH  cap;                /* admission bound (≤ DREGION_MAX; 0 => full)   */
} NODEMAP;

/* Reset to empty. cap=0 means "use the full DREGION_MAX capacity". */
static inline void nodemap_init(NODEMAP *m, UH cap)
{
    for (UH s = 0; s < (UH)DREGION_MAX; s++) { m->used[s] = 0; m->id[s] = 0; }
    m->count = 0;
    m->cap   = (cap == 0 || cap > (UH)DREGION_MAX) ? (UH)DREGION_MAX : cap;
}

/* Slot holding `id`, or NODEMAP_NOSLOT if not tracked. */
static inline INT nodemap_find(const NODEMAP *m, UW id)
{
    for (UH s = 0; s < (UH)DREGION_MAX; s++)
        if (m->used[s] && m->id[s] == id) return (INT)s;
    return NODEMAP_NOSLOT;
}

/* Find-or-insert `id`. Returns its slot, or NODEMAP_NOSLOT if the table is
 * already at its admission bound (the caller then applies its eviction
 * policy and retries) — the single point at which per-node cost is CAPPED
 * at R regardless of how many distinct fleet ids flow through. */
static inline INT nodemap_admit(NODEMAP *m, UW id)
{
    INT s = nodemap_find(m, id);
    if (s != NODEMAP_NOSLOT) return s;
    if (m->count >= m->cap) return NODEMAP_NOSLOT;   /* bounded — this IS O(R) */
    for (UH i = 0; i < (UH)DREGION_MAX; i++) {
        if (!m->used[i]) {
            m->used[i] = 1; m->id[i] = id; m->count++;
            return (INT)i;
        }
    }
    return NODEMAP_NOSLOT;
}

/* Free a slot (owner eviction: region drop, directory expiry, LRU). */
static inline void nodemap_evict(NODEMAP *m, INT slot)
{
    if (slot < 0 || slot >= DREGION_MAX) return;
    if (m->used[slot]) { m->used[slot] = 0; m->count--; }
}

static inline UH nodemap_count(const NODEMAP *m) { return m->count; }
