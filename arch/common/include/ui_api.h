/*
 *  ui_api.h — bounded UI capability surface for hosted human-facing UIs.
 *
 *  The substrate returns values and fixed buffers here. HTTP, SSE,
 *  WebSocket, HTML, Desktop, and generated-app hosting belong above this
 *  boundary in webd.
 */
#pragma once
#include "kernel.h"
#include "ark_profile.h"  /* ARK_HANDLE_MAX */
#include "galaxy.h"        /* single source of EV_* / GALAXY_NODE_NONE (§dedup) */

#define UI_PEER_MAX         64
#define UI_MODULE_MAX       64
#define UI_MODULE_NAME_MAX  32
#define UI_BUILD_ID_MAX     64
#define UI_EVENT_RING       256

typedef struct {
    U1 id;
    U1 state;
    U1 region;
    U1 _pad;
    U4 fresh_ms;
    U4 pressure;
    U4 threat;
    U4 atrisk;
    U4 device;
    U4 rtt_ms;
} UI_PEER;

typedef struct {
    U1 me_id;
    U1 region;
    U1 dmn;
    U1 training;
    char star[ARK_HANDLE_MAX + 1];
    U4 device;
    U4 pending;
    U4 rounds;
    U4 pressure;
    U4 threat;
    U4 s_n;
    U4 s_axis;
    U4 facts_learned;
    U4 epoch;
    U4 idle_runs;
    U4 engram_fill;
    U4 engram_cap;
    U4 infer_count;
    U4 last_class;
    U4 last_conf;
    U4 lineage;
    U4 dropped;
    U4 peer_count;
    UI_PEER peers[UI_PEER_MAX];
} UI_SNAPSHOT;

typedef struct {
    char name[UI_MODULE_NAME_MAX];
    U4 version;
} UI_MODULE;

typedef struct {
    U4 ms;
    U1 type;
    U1 src;
    U1 dst;
    U1 _pad;
    U2 a;
    U2 b;
} UI_EVENT;

_Static_assert(sizeof(UI_EVENT) == 12, "UI_EVENT must be 12 bytes");

/* EV_* event types, EV_MERGE_WEIGHTED, and the node-none sentinel are
 * single-sourced in galaxy.h (included above) — the substrate emit side
 * (dmn/swim/moe/drpc) and this read side share ONE vocabulary. Use
 * GALAXY_NODE_NONE for the none sentinel. */

int ui_node_id(void);
void ui_event_emit(U1 type, U1 src, U1 dst, U2 a, U2 b);
int ui_event_bounds(U4 *tail, U4 *head, U4 *dropped, U4 *capacity);
int ui_event_read(U4 *cursor, UI_EVENT *out, unsigned max,
                  unsigned *count, unsigned *lost);
int ui_event_dropped(U4 *out);
int ui_snapshot(UI_SNAPSHOT *out);
int ui_console_read(char *out, unsigned max);
int ui_modules_read(UI_MODULE *out, unsigned max, unsigned *count,
                    char *build_id, unsigned build_id_max);
