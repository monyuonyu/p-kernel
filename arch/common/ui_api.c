/*
 *  ui_api.c — substrate-side values for webd/desktop/galaxy.
 *
 *  Keep this layer free of HTTP bytes and browser protocol state. It is the
 *  narrow surface a user-space web server can later call without gaining raw
 *  substrate access.
 */
#include "ui_api.h"
#include "drpc.h"
#include "world.h"
#include "region.h"
#include "swim.h"
#include "dmn.h"
#include "dtr.h"
#include "moe.h"
#include "lm_consolidate.h"
#include "lm_self.h"
#include "pfs_dag.h"
#include "ark_profile.h"
#include "modver.h"
#include "interocept.h"

#ifdef _TK_HOSTED_LIBC_
int pkernel_default_node_id(void);
#endif

#define UI_LOCK()    UINT _ui_imask; DI(_ui_imask)
#define UI_UNLOCK()  EI(_ui_imask)

static UI_EVENT g_ui_ring[UI_EVENT_RING];
static volatile U4 g_ui_head = 0;
static volatile U4 g_ui_dropped = 0;
static U1 g_ui_bucket[17];
static U2 g_ui_suppress[17];
static U4 g_ui_last_refill = 0;

static U4 ui_now_ms(void)
{
    SYSTIM t;
    tk_get_otm(&t);
    return (U4)t.lo;
}

static int ui_chatty(U1 type)
{
    return type == EV_KDDS || type == EV_PMESH_TX || type == EV_PMESH_RX;
}

static void ui_event_append_raw(U1 type, U1 src, U1 dst, U2 a, U2 b)
{
    UI_LOCK();
    UI_EVENT *e = &g_ui_ring[g_ui_head & (UI_EVENT_RING - 1)];
    e->ms = ui_now_ms();
    e->type = type;
    e->src = src;
    e->dst = dst;
    e->_pad = 0;
    e->a = a;
    e->b = b;
    g_ui_head++;
    UI_UNLOCK();
}

static void ui_event_refill(void)
{
    U4 now = ui_now_ms();
    if (g_ui_last_refill == 0) {
        g_ui_last_refill = now;
        for (int t = 0; t < 17; t++) g_ui_bucket[t] = 4;
    }
    if (now - g_ui_last_refill < 1000) return;
    g_ui_last_refill = now;
    for (int t = 0; t < 17; t++) {
        g_ui_bucket[t] = 4;
        if (g_ui_suppress[t]) {
            ui_event_append_raw(EV_SUMMARY, (U1)ui_node_id(), UI_EVENT_NODE_NONE,
                                (U2)t, g_ui_suppress[t]);
            g_ui_suppress[t] = 0;
        }
    }
}

void ui_event_emit(U1 type, U1 src, U1 dst, U2 a, U2 b)
{
    ui_event_refill();
    if (ui_chatty(type) && type < 17) {
        if (g_ui_bucket[type] == 0) { g_ui_suppress[type]++; return; }
        g_ui_bucket[type]--;
    }
    ui_event_append_raw(type, src, dst, a, b);
}

int ui_event_bounds(U4 *tail, U4 *head, U4 *dropped, U4 *capacity)
{
    U4 h = g_ui_head;
    U4 cap = UI_EVENT_RING;
    if (head) *head = h;
    if (tail) *tail = h > cap ? h - cap : 0;
    if (dropped) *dropped = g_ui_dropped;
    if (capacity) *capacity = cap;
    return 0;
}

int ui_event_dropped(U4 *out)
{
    if (!out) return -1;
    *out = g_ui_dropped;
    return 0;
}

int ui_event_read(U4 *cursor, UI_EVENT *out, unsigned max,
                  unsigned *count, unsigned *lost)
{
    if (!cursor || !count) return -1;
    *count = 0;
    if (lost) *lost = 0;
    U4 head = g_ui_head;
    U4 tail = head > UI_EVENT_RING ? head - UI_EVENT_RING : 0;
    if (*cursor < tail) {
        U4 miss = tail - *cursor;
        g_ui_dropped += miss;
        if (lost) *lost = miss;
        *cursor = tail;
    }
    while (*cursor < head && *count < max) {
        if (out) {
            UI_LOCK();
            out[*count] = g_ui_ring[*cursor & (UI_EVENT_RING - 1)];
            UI_UNLOCK();
        }
        (*cursor)++;
        (*count)++;
    }
    return 0;
}

static void ui_copy(char *dst, unsigned max, const char *src)
{
    unsigned i = 0;
    if (max == 0) return;
    while (src && src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

int ui_node_id(void)
{
    if (drpc_my_node != 0xFF) return (int)drpc_my_node;
#ifdef _TK_HOSTED_LIBC_
    {
        int v = pkernel_default_node_id();
        if (v >= 1 && v < DNODE_MAX) return v;
    }
#endif
    return 1;
}

int ui_snapshot(UI_SNAPSHOT *out)
{
    if (!out) return -1;
    for (unsigned i = 0; i < sizeof(*out); i++) ((U1 *)out)[i] = 0;

    _Static_assert(DNODE_MAX <= UI_PEER_MAX, "UI_PEER_MAX must cover DNODE_MAX");

    U1 me = (U1)ui_node_id();
    int pr = world_peer_pressure(me); if (pr < 0) pr = 0;
    int th = world_peer_threat(me);   if (th < 0) th = 0;
    int dv = world_peer_device(me);   if (dv < 0) dv = 0;

    out->me_id = me;
    out->device = (U4)dv;
    out->region = (U1)region_id();
    out->dmn = (U1)dmn_state_get();
    out->pending = (U4)r3_facts_pending();
    out->rounds = (U4)dmn_r3_rounds();
    out->pressure = (U4)pr;
    out->threat = (U4)th;
    out->s_n = (U4)intero_scalar();
    out->s_axis = (U4)intero_dominant_axis();
    out->training = (U1)r3_round_busy_get();
    out->facts_learned = (U4)r3_retained_count();
    out->epoch = (U4)r3_merge_epoch();
    out->idle_runs = (U4)dmn_stats.idle_runs;
    out->engram_fill = (U4)lm_ring_fill();
    out->engram_cap = (U4)lm_ring_cap();
    out->infer_count = (U4)kernel_infer_count;
    ui_event_dropped(&out->dropped);

    {
        UB lc = 0, lcf = 0;
        moe_infer_last(&lc, (UB *)0, (UB *)0, &lcf);
        out->last_class = (U4)lc;
        out->last_conf = (U4)lcf;
    }
    {
        LM_SELF_ENTRY se;
        INT sr = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                              &se, (UW)sizeof(se));
        out->lineage = ((sr == (INT)sizeof(se) || sr == LM_SELF_ENTRY_V1_SIZE) && se.magic)
                       ? (U4)se.seq : 0u;
    }
    {
        static ARK_PROFILE prof;
        if (ark_profile_head(&prof) && prof.handle_len > 0) {
            unsigned n = prof.handle_len;
            if (n > ARK_HANDLE_MAX) n = ARK_HANDLE_MAX;
            for (unsigned i = 0; i < n; i++) out->star[i] = prof.handle[i];
            out->star[n] = 0;
        }
    }

    for (INT n = 0; n < DNODE_MAX && out->peer_count < UI_PEER_MAX; n++) {
        if ((U1)n == me) continue;
        U1 st = dnode_table[n].state;
        if (st == DNODE_UNKNOWN && !world_peer_known((U1)n)) continue;

        UI_PEER *p = &out->peers[out->peer_count++];
        int ppr = world_peer_pressure((U1)n);
        int pth = world_peer_threat((U1)n);
        int par = world_peer_atrisk((U1)n);
        int prg = world_peer_region_fresh((U1)n);
        int pag = world_peer_age_ms((U1)n);
        int pdv = world_peer_device((U1)n);

        p->id = (U1)n;
        p->state = st;
        p->region = (U1)(prg < 0 ? 255 : prg);
        p->fresh_ms = (U4)(pag < 0 ? 0 : pag);
        p->pressure = (U4)(ppr < 0 ? 0 : ppr);
        p->threat = (U4)(pth < 0 ? 0 : pth);
        p->atrisk = (U4)(par < 0 ? 0 : par);
        p->device = (U4)(pdv < 0 ? 0 : pdv);
        p->rtt_ms = (U4)swim_rtt_ms((U1)n);
    }
    return 0;
}

int ui_console_read(char *out, unsigned max)
{
    if (!out || max == 0) return -1;
#ifdef _TK_HOSTED_LIBC_
    {
        extern int console_ring_read(char *out, int max);
        unsigned cap = max > 2147483647u ? 2147483647u : max;
        int n = console_ring_read(out, (int)cap - 1);
        if (n < 0) n = 0;
        if ((unsigned)n >= max) n = (int)max - 1;
        out[n] = 0;
        return n;
    }
#else
    out[0] = 0;
    return 0;
#endif
}

int ui_modules_read(UI_MODULE *out, unsigned max, unsigned *count,
                    char *build_id, unsigned build_id_max)
{
    if (!count) return -1;
    *count = 0;
    ui_copy(build_id, build_id_max, modver_build_id());
    if (!out || max == 0) return 0;

    int n = modver_count();
    for (int i = 0; i < n && *count < max; i++) {
        UI_MODULE *m = &out[*count];
        ui_copy(m->name, UI_MODULE_NAME_MAX, modver_name(i));
        int v = modver_version(i);
        m->version = (U4)(v < 0 ? 0 : v);
        (*count)++;
    }
    return 0;
}
