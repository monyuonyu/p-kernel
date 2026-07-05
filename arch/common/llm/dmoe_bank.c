/*
 *  dmoe_bank.c — DMOE-A: the distributed-MoE expert BANK (the growth organ).
 *
 *  Header + rationale: dmoe_bank.h. This TU owns residency, the version pin, the
 *  remote-fire ladder, and dmoe_experts_reachable(); student.c owns the joint
 *  routing math + the degrade sum. Hosted-tier (bare metal links student_stub.o;
 *  this file is NOT in the bare-metal link) — so the crown .text is unaffected.
 *  It REUSES placement.c's st_expert_owners_in read-only (HRW, no modification)
 *  and adds ZERO K-DDS topics.
 *
 *  One math: dmoe_expert_forward_ref runs the SAME SwiGLU statements as
 *  student.c's st_expert_forward_ref (silu = x/(1+e^-x), the exported st_expf),
 *  so a remote bank expert's [D] output is bit-identical to a resident one under
 *  -O1 -ffp-contract=off, on aarch64 and x86_64 alike.
 */

#include "dmoe_bank.h"

/* libc-light: one malloc/free for the (heap, bounded) router table + resident
 * blocks. Declared here (not via <stdlib.h>) so the kernel include chain this
 * TU compiles under never clashes — the placement.c discipline. size_t comes
 * from <stddef.h> (pulled by dmoe_bank.h -> student.h). */
extern void *malloc(size_t);
extern void  free(void *);

/* the one math shared with student.c (exported st_expf); silu identical to the
 * static st_silu the floor path uses. */
static float dmoe_silu(float x) { return x / (1.0f + st_expf(-x)); }

/* a quiet NaN, libc-free (0x7fc00000). */
static float dmoe_nan(void)
{
    union { uint32_t u; float f; } q; q.u = 0x7fc00000u; return q.f;
}

/* ------------------------------------------------------------------ */
/* sizes + the version pin                                            */
/* ------------------------------------------------------------------ */

size_t dmoe_expert_bytes(const st_model *tier)
{
    if (!tier) return 0;
    size_t L = (size_t)tier->nlayer, D = (size_t)tier->d, DFF = (size_t)tier->dff;
    /* w1[L][DFF][D] + w3[L][DFF][D] + w2[L][D][DFF] floats. */
    return (2u * DFF * D + D * DFF) * L * sizeof(float);
}

size_t dmoe_resident_bytes(const dmoe_bank *b)
{
    if (!b) return 0;
    size_t per = (size_t)(2 * b->DFF * b->D + b->D * b->DFF) * (size_t)b->L * sizeof(float);
    size_t tot = 0;
    for (int i = 0; i < b->nbank; i++) if (b->slot[i].resident) tot += per;
    return tot;
}

/* FNV-1a64 over raw bytes. */
static uint64_t fnv1a(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

uint64_t dmoe_expert_ver(const st_model *tier, const float *w1, const float *w3,
                         const float *w2, uint32_t core_epoch)
{
    if (!tier) return 0;
    size_t L = (size_t)tier->nlayer, D = (size_t)tier->d, DFF = (size_t)tier->dff;
    size_t n13 = L * DFF * D, n2 = L * D * DFF;
    uint64_t h = 1469598103934665603ULL;              /* FNV offset basis */
    if (w1) h = fnv1a(h, w1, n13 * sizeof(float));
    if (w3) h = fnv1a(h, w3, n13 * sizeof(float));
    if (w2) h = fnv1a(h, w2, n2 * sizeof(float));
    h = fnv1a(h, &core_epoch, sizeof core_epoch);
    return h;
}

/* ------------------------------------------------------------------ */
/* init / free                                                        */
/* ------------------------------------------------------------------ */

int dmoe_bank_init(dmoe_bank *b, const st_model *tier, UB self)
{
    if (!b || !tier) return -1;
    for (size_t z = 0; z < sizeof *b; z++) ((uint8_t *)b)[z] = 0;
    b->E_res = tier->nexpert;
    b->L = tier->nlayer;
    b->D = tier->d;
    b->DFF = tier->dff;
    b->self_node = self;
    b->core_epoch = 0;
    b->mode = DMOE_MODE_NORMAL;
    b->nbank = 0;
    /* replicated router rows: [ST_DMOE_FLEET_MAX][L][D] (eager, small). */
    size_t rcap = (size_t)ST_DMOE_FLEET_MAX * (size_t)b->L * (size_t)b->D;
    b->router = (float *)malloc(rcap * sizeof(float));
    if (!b->router) return -1;
    for (size_t i = 0; i < rcap; i++) b->router[i] = 0.0f;
    return 0;
}

void dmoe_bank_free(dmoe_bank *b)
{
    if (!b) return;
    if (b->router) { free(b->router); b->router = NULL; }
    for (int i = 0; i < b->nbank; i++) {
        if (b->slot[i].w1) free(b->slot[i].w1);
        if (b->slot[i].w3) free(b->slot[i].w3);
        if (b->slot[i].w2) free(b->slot[i].w2);
        if (b->slot[i].canary) free(b->slot[i].canary);
        b->slot[i].w1 = b->slot[i].w3 = b->slot[i].w2 = b->slot[i].canary = NULL;
    }
    b->nbank = 0;
}

/* ------------------------------------------------------------------ */
/* add a bank expert                                                  */
/* ------------------------------------------------------------------ */

static float *copy_floats(const float *src, size_t n)
{
    float *d = (float *)malloc(n * sizeof(float));
    if (!d) return NULL;
    for (size_t i = 0; i < n; i++) d[i] = src[i];
    return d;
}

int dmoe_bank_add(dmoe_bank *b, int xid, const float *router_rows,
                  const float *w1, const float *w3, const float *w2,
                  uint32_t core_epoch, const UB *holders, int n_holders, int R)
{
    if (!b || b->nbank >= ST_DMOE_FLEET_MAX) return -1;
    if (!router_rows || !w1 || !w3 || !w2) return -1;
    if (xid < b->E_res) return -1;                    /* xid >= E_res by construction */
    int idx = b->nbank;
    dmoe_slot *s = &b->slot[idx];

    s->xid = xid;
    s->core_epoch = core_epoch;
    s->R = R > 0 ? R : DMOE_R_DEFAULT;
    s->salience = 0;
    s->canary = NULL;
    /* the version pin: computed from the CANONICAL blocks + epoch on EVERY node
     * (identical blocks -> identical ver -> a stale replica's ver differs). */
    s->ver = dmoe_expert_ver_dims(b->L, b->D, b->DFF, w1, w3, w2, core_epoch);

    /* manifest owner list (HRW-rank order), the holders of ver-current blocks. */
    s->n_holders = 0;
    for (int k = 0; k < n_holders && k < ST_PLACE_RMAX; k++)
        s->holders[s->n_holders++] = holders[k];

    /* residency: this node holds the FFN blocks iff it is among the holders. */
    int resident = 0;
    for (int k = 0; k < s->n_holders; k++) if (s->holders[k] == b->self_node) resident = 1;
    s->resident = resident;

    /* router row is REPLICATED onto every node (small). */
    size_t rlen = (size_t)b->L * (size_t)b->D;
    float *rr = b->router + (size_t)idx * rlen;
    for (size_t i = 0; i < rlen; i++) rr[i] = router_rows[i];

    if (resident) {
        size_t n13 = (size_t)b->L * (size_t)b->DFF * (size_t)b->D;
        size_t n2  = (size_t)b->L * (size_t)b->D * (size_t)b->DFF;
        s->w1 = copy_floats(w1, n13);
        s->w3 = copy_floats(w3, n13);
        s->w2 = copy_floats(w2, n2);
        if (!s->w1 || !s->w3 || !s->w2) return -1;
    } else {
        s->w1 = s->w3 = s->w2 = NULL;
    }
    b->nbank++;
    return idx;
}

/* ver over explicit dims (dmoe_bank_add has no st_model in scope). */
uint64_t dmoe_expert_ver_dims(int L, int D, int DFF, const float *w1,
                              const float *w3, const float *w2, uint32_t core_epoch)
{
    size_t n13 = (size_t)L * (size_t)DFF * (size_t)D, n2 = (size_t)L * (size_t)D * (size_t)DFF;
    uint64_t h = 1469598103934665603ULL;
    if (w1) h = fnv1a(h, w1, n13 * sizeof(float));
    if (w3) h = fnv1a(h, w3, n13 * sizeof(float));
    if (w2) h = fnv1a(h, w2, n2 * sizeof(float));
    h = fnv1a(h, &core_epoch, sizeof core_epoch);
    return h;
}

void dmoe_bank_set_canary(dmoe_bank *b, int slot)
{
    if (!b || slot < 0 || slot >= b->nbank) return;
    dmoe_slot *s = &b->slot[slot];
    if (s->resident) return;                          /* only non-resident slots */
    if (s->canary) return;
    size_t n13 = (size_t)b->L * (size_t)b->DFF * (size_t)b->D;
    s->canary = (float *)malloc(n13 * sizeof(float));
    if (!s->canary) return;
    float nn = dmoe_nan();
    for (size_t i = 0; i < n13; i++) s->canary[i] = nn;   /* poison on any local read */
}

void dmoe_bank_force_ver(dmoe_bank *b, int slot, uint64_t bad_ver)
{
    if (!b || slot < 0 || slot >= b->nbank) return;
    b->slot[slot].ver = bad_ver;
}

/* ------------------------------------------------------------------ */
/* the per-expert SwiGLU (bit-identical to st_expert_forward_ref)     */
/* ------------------------------------------------------------------ */

int dmoe_expert_forward_ref(const dmoe_bank *b, int slot, int layer,
                            const float *fin, float *out)
{
    if (!b || slot < 0 || slot >= b->nbank || !fin || !out) return -1;
    if (layer < 0 || layer >= b->L) return -1;
    const dmoe_slot *s = &b->slot[slot];
    int D = b->D, DFF = b->DFF;

    const float *w1, *w3, *w2;
    if (s->resident && s->w1) {
        w1 = s->w1; w3 = s->w3; w2 = s->w2;
    } else if (s->canary) {
        /* the [dmoe-nonresident] tooth: a SECRET local read of a non-resident
         * expert reads the NaN canary and poisons the [D] output (NaN), which
         * the logit-hash gate then catches loudly — a wrong-but-confident local
         * fallback cannot hide. */
        w1 = s->canary; w3 = s->canary; w2 = s->canary;
    } else {
        return -1;                                    /* not resident, no canary */
    }

    /* out[D] = w2 . (silu(w1.fin) * (w3.fin)), UNWEIGHTED (the router weight is
     * applied by student.c's canonical sum). w1/w3 are [L][DFF][D], w2 [L][D][DFF]. */
    const float *w1l = w1 + (size_t)layer * DFF * D;
    const float *w3l = w3 + (size_t)layer * DFF * D;
    const float *w2l = w2 + (size_t)layer * D * DFF;
    /* bounded scratch: dff <= ST_DFF_MAX (heap-free, but fixed L-tier). */
    static float eh[ST_DFF_MAX];
    for (int h = 0; h < DFF; h++) {
        const float *w1h = w1l + (size_t)h * D;
        const float *w3h = w3l + (size_t)h * D;
        float g = 0.0f, u = 0.0f;
        for (int i = 0; i < D; i++) { g += w1h[i] * fin[i]; u += w3h[i] * fin[i]; }
        eh[h] = dmoe_silu(g) * u;
    }
    for (int i = 0; i < D; i++) {
        const float *w2r = w2l + (size_t)i * DFF;
        float acc = 0.0f;
        for (int h = 0; h < DFF; h++) acc += w2r[h] * eh[h];
        out[i] = acc;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* reachability + the capacity number                                 */
/* ------------------------------------------------------------------ */

static int slot_reachable(const dmoe_bank *b, const dmoe_slot *s)
{
    if (s->resident) return 1;
    if (!b->alive) return s->n_holders > 0;           /* no view -> holders exist */
    for (int k = 0; k < s->n_holders; k++)
        if (b->alive(s->holders[k], b->alive_ctx)) return 1;
    return 0;
}

int dmoe_experts_reachable(const dmoe_bank *b)
{
    if (!b) return 0;
    int reach = b->E_res;                             /* the floor is always resident */
    for (int i = 0; i < b->nbank; i++)
        if (slot_reachable(b, &b->slot[i])) reach++;
    return reach;
}

/* ------------------------------------------------------------------ */
/* fleet-view setters                                                 */
/* ------------------------------------------------------------------ */

void dmoe_set_members(dmoe_bank *b, const UB *members, int n)
{
    if (!b) return;
    if (n > (int)LOOKUP_MAX_MEMBERS) n = (int)LOOKUP_MAX_MEMBERS;
    if (n < 0) n = 0;
    b->n_members = n;
    for (int i = 0; i < n; i++) b->members[i] = members[i];
}
void dmoe_set_alive(dmoe_bank *b, dmoe_alive_fn fn, void *ctx)
{ if (b) { b->alive = fn; b->alive_ctx = ctx; } }
void dmoe_set_transport(dmoe_bank *b, dmoe_remote_fn fn, void *ctx)
{ if (b) { b->remote = fn; b->remote_ctx = ctx; } }
void dmoe_set_mode(dmoe_bank *b, int mode) { if (b) b->mode = mode; }

/* ------------------------------------------------------------------ */
/* the installed student.c hooks (score + fire)                       */
/* ------------------------------------------------------------------ */

static int dmoe_score_hook(int layer, const float *fin, int d, float *out,
                           int cap, void *ctx)
{
    dmoe_bank *b = (dmoe_bank *)ctx;
    (void)d;
    int n = b->nbank < cap ? b->nbank : cap;
    for (int i = 0; i < n; i++) {
        dmoe_slot *s = &b->slot[i];
        int reach = (b->mode == DMOE_MODE_RESIDENT_ONLY) ? 1 : slot_reachable(b, s);
        if (!reach) { out[i] = -1e30f; continue; }    /* LOST -> never selected  */
        const float *rr = b->router + ((size_t)i * (size_t)b->L + (size_t)layer) * (size_t)b->D;
        float acc = 0.0f;
        for (int j = 0; j < b->D; j++) acc += rr[j] * fin[j];
        out[i] = acc;
    }
    return n;
}

static int dmoe_fire_hook(int layer, int bslot, const float *fin, int d,
                          float *out, void *ctx)
{
    dmoe_bank *b = (dmoe_bank *)ctx;
    if (bslot < 0 || bslot >= b->nbank) return -1;
    dmoe_slot *s = &b->slot[bslot];

    /* SABOTAGE (theater): secretly read the local NaN canary instead of going
     * remote — proves the anti-theater tooth (the logit hash goes NaN). */
    if (b->mode == DMOE_MODE_THEATER_LOCAL && !s->resident) {
        dmoe_expert_forward_ref(b, bslot, layer, fin, out);   /* reads canary -> NaN */
        return 0;
    }

    /* resident: local one-math forward. */
    if (s->resident && s->w1)
        return dmoe_expert_forward_ref(b, bslot, layer, fin, out);

    /* DISEASE: resident-only -> no remote -> the expert DROPS (honest degrade). */
    if (b->mode == DMOE_MODE_RESIDENT_ONLY) return -1;
    if (!b->remote) return -1;

    /* remote ladder: try HRW-rank holders that are alive; any replica returns
     * the identical bits (ver pin), so greedy first-alive is correct. On ver
     * skew the owner REFUSES -> next replica; none -> DROP. */
    int fa = (b->mode == DMOE_MODE_FORCE_ACCEPT);
    for (int k = 0; k < s->n_holders; k++) {
        UB h = s->holders[k];
        if (h == b->self_node) continue;              /* us; not resident here   */
        if (b->alive && !b->alive(h, b->alive_ctx)) continue;
        if (b->remote(h, s->xid, layer, fin, d, s->ver, s->core_epoch, fa,
                      out, b->remote_ctx) == 0)
            return 0;
    }
    return -1;                                         /* no reachable owner -> DROP */
}

void dmoe_activate(dmoe_bank *b)
{
    if (!b) { st_dmoe_install(NULL, NULL, 0, NULL); return; }
    st_dmoe_install(dmoe_score_hook, dmoe_fire_hook, b->nbank, b);
}
void dmoe_deactivate(void) { st_dmoe_install(NULL, NULL, 0, NULL); }
