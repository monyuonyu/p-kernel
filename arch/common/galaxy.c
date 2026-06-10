/*
 *  galaxy.c — 銀河の観測窓 v1 kernel side (docs/architecture/galaxy.md)
 *
 *  The event ring (§4), the JSON builders (§3.4, hand-rolled — NO second
 *  JSON library), the minimal HTTP/1.0 server + SSE writer (§3.6), and
 *  the server task (§3.3). The teach/ask endpoints drive the LM-6
 *  production mouth `mind_cmd` (§6) — this file calls NO lower-level mind
 *  primitive (the fact-learn / consolidate / round / train symbols the
 *  LM-6 auditor greps for stay OUT of this TU), holds NO second node
 *  table (peers come from dnode_table[] + world_peer_*), and adds NO
 *  second event/stat system. The auditor greps all of this.
 *
 *  Transport is the 5-function galaxy_io_* ABI (galaxy_posix.c); this
 *  file uses NO sockets directly so the future netstack-tcp-server slice
 *  can carry bare-metal without touching galaxy.c.
 *
 *  Honesty (§1): every emitted event is a real organism event; overflow
 *  is COUNTED and reported, never hidden; a dead/stuck browser is dropped
 *  and can never wedge the server task — all client I/O is non-blocking.
 */

#include "galaxy.h"
#include "drpc.h"        /* dnode_table[], drpc_my_node, DNODE_*           */
#include "world.h"       /* world_peer_* accessors — the galaxy's organs   */
#include "region.h"      /* region_id() — my constellation                 */
#include "dmn.h"         /* dmn_state_get(), dmn_r3_rounds()               */
#include "dtr.h"         /* r3_facts_pending(), mind_cmd, mind_last_answer */
#include "lm_self.h"     /* LM_SELF_REF / LM_SELF_ENTRY — /self.json       */
#include "pfs_dag.h"     /* pfs_dag_read — lazy self lineage read          */
#include "kernel.h"

#include "galaxy_page.h" /* GENERATED: galaxy_page[] + galaxy_page_len     */

/* §4.1: the ring's 3-store append needs a uniprocessor exclusion window
 * against producers running on other tasks. Use the portable DI/EI
 * critical-section primitive (kernel/common/fastlock.c uses the same):
 * on hosted builds disint() defers the SIGALRM preempt tick (the Linux-
 * port model of IRQ-off / dispatch-disable); on bare metal it masks the
 * real IRQ. Same source serves the future netstack-tcp-server slice. */
#define GX_LOCK()    UINT _gx_imask; DI(_gx_imask)
#define GX_UNLOCK()  EI(_gx_imask)

extern char *getenv(const char *);
extern int   atoi(const char *);

/* the 5-function transport ABI (galaxy_posix.c; C ABI, no T-Kernel types) */
int  galaxy_io_init(int port);
int  galaxy_io_accept(void);
int  galaxy_io_read(int slot, void *buf, int max);
int  galaxy_io_write(int slot, const void *buf, int len);
void galaxy_io_close(int slot);

/* small file-static string helpers (defined below; forward-declared so
 * the routing code above their definitions can use them). */
static INT gx_streq(const char *a, const char *b);
static INT gx_itoa(char *out, INT v);

/* ------------------------------------------------------------------ */
/* enablement (§D1): default ON for hosted; PKERNEL_GALAXY=0 disables.  */
/* port = 7800 + (node_id - 1), or PKERNEL_GALAXY_PORT override.        */
/* ------------------------------------------------------------------ */

volatile UB galaxy_on = 0;          /* set by galaxy_init / cleared if off */
static  int galaxy_port = 0;

/* ------------------------------------------------------------------ */
/* the event ring (§4.1) — single static ring + one head                */
/* ------------------------------------------------------------------ */

static GALAXY_EV g_ring[GALAXY_RING];
static volatile UW g_head = 0;       /* total ever written (monotone)      */
static volatile UW g_dropped = 0;    /* lapped-consumer overflow (shown)   */

UW galaxy_dropped(void) { return g_dropped; }

/* uptime ms (SYSTIM.lo) — same source as world.c now_ms(). */
static UW gx_now_ms(void)
{
    SYSTIM t; tk_get_otm(&t);
    return (UW)t.lo;
}

/* §4.2 token bucket for the chatty types (EV_KDDS/PMESH_*). v1 does not
 * yet emit these (S7-S9 are v2), but the gate is in place so adding the
 * v2 hooks needs no ring change. */
static UB g_bucket[16];              /* per-type budget, refilled 1/s      */
static UH g_suppress[16];            /* per-type suppressed counter        */

static INT gx_chatty(UB type)
{
    return (type == EV_KDDS || type == EV_PMESH_TX || type == EV_PMESH_RX);
}

/* §4: the ONE hook. O(1); first instruction is the off-branch. Producers
 * run on many tasks, so the 3-store append is wrapped in dis/ena_dsp —
 * a uniprocessor T-Kernel's cheapest correct exclusion (no semaphore on
 * the hot path). */
void galaxy_emit(UB type, UB src, UB dst, UH a, UH b)
{
    if (!galaxy_on) return;          /* dead-cheap when off                */

    if (gx_chatty(type)) {           /* §4.2 sampling                      */
        if (type < 16) {
            if (g_bucket[type] == 0) { g_suppress[type]++; return; }
            g_bucket[type]--;
        }
    }

    GX_LOCK();
    GALAXY_EV *e = &g_ring[g_head & (GALAXY_RING - 1)];
    e->ms   = gx_now_ms();
    e->type = type;
    e->src  = src;
    e->dst  = dst;
    e->_pad = 0;
    e->a    = a;
    e->b    = b;
    g_head++;
    GX_UNLOCK();
}

/* ------------------------------------------------------------------ */
/* output helpers — snprintf-free putdec, the wo_putdec style (§4.3)    */
/* ------------------------------------------------------------------ */

/* a small per-client out path: galaxy_io_write may short-write; we keep
 * a tiny buffer and drop the client if it backs up (§3.2). */
#define GX_OUTBUF 4096

typedef struct {
    INT  in_use;
    INT  is_sse;          /* held-open event stream                       */
    UW   sse_cursor;      /* next ring index this SSE client will send    */
    UW   last_ping;       /* ms of last : ping (SSE keepalive)            */
    INT  ob_len;          /* pending bytes in ob[]                        */
    char ob[GX_OUTBUF];
} GX_CLIENT;

static GX_CLIENT g_cli[GALAXY_MAX_CLIENTS];

/* queue bytes to a client's out-buffer; returns 0 ok, -1 overflow (drop) */
static INT gx_q(INT slot, const char *s, INT n)
{
    GX_CLIENT *c = &g_cli[slot];
    if (c->ob_len + n > GX_OUTBUF) return -1;     /* dead reader -> drop    */
    for (INT i = 0; i < n; i++) c->ob[c->ob_len + i] = s[i];
    c->ob_len += n;
    return 0;
}
static INT gx_qs(INT slot, const char *s)
{
    INT n = 0; while (s[n]) n++;
    return gx_q(slot, s, n);
}
static INT gx_qdec(INT slot, UW v)
{
    char buf[12]; INT i = 11; buf[11] = 0;
    if (v == 0) return gx_qs(slot, "0");
    while (v && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    return gx_qs(slot, &buf[i]);
}

/* flush a client's out-buffer to the socket (non-blocking, short-write
 * tolerant). Returns 0 ok / -1 closed-or-dropped. */
static INT gx_flush(INT slot)
{
    GX_CLIENT *c = &g_cli[slot];
    while (c->ob_len > 0) {
        INT w = galaxy_io_write(slot, c->ob, c->ob_len);
        if (w < 0) return -1;                     /* peer closed            */
        if (w == 0) break;                        /* EWOULDBLOCK: try later */
        if (w < c->ob_len)
            for (INT i = 0; i < c->ob_len - w; i++) c->ob[i] = c->ob[i + w];
        c->ob_len -= w;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* /galaxy.json — snapshot from dnode_table + world accessors (§3.4)    */
/* assembled with NO second node table (auditor grep).                 */
/* ------------------------------------------------------------------ */

static UB gx_my_id(void)
{
    if (drpc_my_node != 0xFF) return drpc_my_node;
    /* single-node, pre-cmd_net: fall back to PKERNEL_NODE_ID like the
     * transport does, so the window has a stable identity. */
    const char *e = getenv("PKERNEL_NODE_ID");
    INT v = e ? atoi(e) : 1;
    if (v < 1 || v >= DNODE_MAX) v = 1;
    return (UB)v;
}

static void gx_build_galaxy_json(INT slot)
{
    UB me = gx_my_id();
    INT pr = world_peer_pressure(me); if (pr < 0) pr = 0;
    INT th = world_peer_threat(me);   if (th < 0) th = 0;

    INT mydv = world_peer_device(me);   /* my own beaconed device, -1 if not yet */

    gx_qs(slot, "{\"me\":{\"id\":");           gx_qdec(slot, me);
    gx_qs(slot, ",\"device\":");               gx_qdec(slot, mydv < 0 ? 0u : (UW)mydv);
    gx_qs(slot, ",\"region\":");               gx_qdec(slot, (UW)region_id());
    gx_qs(slot, ",\"dmn\":");                  gx_qdec(slot, (UW)dmn_state_get());
    gx_qs(slot, ",\"pending\":");              gx_qdec(slot, (UW)r3_facts_pending());
    gx_qs(slot, ",\"rounds\":");               gx_qdec(slot, dmn_r3_rounds());
    gx_qs(slot, ",\"pressure\":");             gx_qdec(slot, (UW)pr);
    gx_qs(slot, ",\"threat\":");               gx_qdec(slot, (UW)th);
    gx_qs(slot, "},\"peers\":[");

    INT first = 1;
    for (INT n = 0; n < DNODE_MAX; n++) {
        if ((UB)n == me) continue;
        UB st = dnode_table[n].state;
        if (st == DNODE_UNKNOWN && !world_peer_known((UB)n)) continue;
        if (!first) gx_qs(slot, ",");
        first = 0;
        INT ppr = world_peer_pressure((UB)n);
        INT pth = world_peer_threat((UB)n);
        INT par = world_peer_atrisk((UB)n);
        INT prg = world_peer_region_fresh((UB)n);
        INT pag = world_peer_age_ms((UB)n);
        INT pdv = world_peer_device((UB)n);
        gx_qs(slot, "{\"id\":");        gx_qdec(slot, (UW)n);
        gx_qs(slot, ",\"state\":");     gx_qdec(slot, (UW)st);
        gx_qs(slot, ",\"region\":");    gx_qdec(slot, prg < 0 ? 255u : (UW)prg);
        gx_qs(slot, ",\"fresh\":");     gx_qdec(slot, pag < 0 ? 0u : (UW)pag);
        gx_qs(slot, ",\"pressure\":");  gx_qdec(slot, ppr < 0 ? 0u : (UW)ppr);
        gx_qs(slot, ",\"threat\":");    gx_qdec(slot, pth < 0 ? 0u : (UW)pth);
        gx_qs(slot, ",\"atrisk\":");    gx_qdec(slot, par < 0 ? 0u : (UW)par);
        gx_qs(slot, ",\"device\":");    gx_qdec(slot, pdv < 0 ? 0u : (UW)pdv);
        gx_qs(slot, "}");
    }
    gx_qs(slot, "],\"dropped\":");  gx_qdec(slot, g_dropped);
    gx_qs(slot, "}");
}

/* ------------------------------------------------------------------ */
/* /self.json — lazy Self-lineage read (§D6, S10). v1 reports the HEAD   */
/* "self/lin" entry (the newest version) via pfs_dag_read; older         */
/* versions need a per-seq reader not yet public (flagged in the report).*/
/* ------------------------------------------------------------------ */

static void gx_hex8(INT slot, const U1 *b)
{
    static const char hx[] = "0123456789abcdef";
    char out[9];
    for (INT i = 0; i < 4; i++) {
        out[i*2]   = hx[(b[i] >> 4) & 0xF];
        out[i*2+1] = hx[b[i] & 0xF];
    }
    out[8] = 0;
    gx_qs(slot, out);
}

static void gx_build_self_json(INT slot)
{
    LM_SELF_ENTRY e;
    INT r = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                         &e, (UW)sizeof(e));
    if (r < (INT)sizeof(e) || e.magic == 0) {
        gx_qs(slot, "{\"present\":false}");
        return;
    }
    gx_qs(slot, "{\"present\":true,\"self_id\":"); gx_qdec(slot, (UW)e.self_id);
    gx_qs(slot, ",\"seq\":");                      gx_qdec(slot, (UW)e.seq);
    gx_qs(slot, ",\"hash\":\"");                   gx_hex8(slot, e.eng_digest);
    gx_qs(slot, "\",\"prev\":\"");                 gx_hex8(slot, e.prev_entry);
    gx_qs(slot, "\"}");
}

/* ------------------------------------------------------------------ */
/* SSE event serialization (§3.6 / §4.3) — JSON only at the edge        */
/* ------------------------------------------------------------------ */

static const char *gx_type_name(UB t)
{
    switch (t) {
    case EV_SWIM:        return "swim";
    case EV_DMN_WAKE:    return "dmn_wake";
    case EV_DMN_IDLE:    return "dmn_idle";
    case EV_CONSOLIDATE: return "consolidate";
    case EV_TEACH:       return "teach";
    case EV_ASK:         return "ask";
    case EV_DRPC_IN:     return "drpc_in";
    case EV_DRPC_OUT:    return "drpc_out";
    case EV_MOE:         return "moe";
    case EV_DKVA:        return "dkva";
    case EV_KDDS:        return "kdds";
    case EV_PMESH_TX:    return "pmesh_tx";
    case EV_PMESH_RX:    return "pmesh_rx";
    case EV_SUMMARY:     return "summary";
    default:             return "ev";
    }
}

static void gx_sse_event(INT slot, const GALAXY_EV *e)
{
    gx_qs(slot, "data:{\"type\":\"");  gx_qs(slot, gx_type_name(e->type));
    gx_qs(slot, "\",\"ms\":");         gx_qdec(slot, e->ms);
    if (e->src != GALAXY_NODE_NONE) { gx_qs(slot, ",\"src\":"); gx_qdec(slot, (UW)e->src); }
    if (e->dst != GALAXY_NODE_NONE) { gx_qs(slot, ",\"dst\":"); gx_qdec(slot, (UW)e->dst); }
    gx_qs(slot, ",\"a\":");            gx_qdec(slot, (UW)e->a);
    gx_qs(slot, ",\"b\":");            gx_qdec(slot, (UW)e->b);
    gx_qs(slot, "}\n\n");
}

/* ------------------------------------------------------------------ */
/* HTTP request parsing (§3.6) — first line + Content-Length only       */
/* ------------------------------------------------------------------ */

#define GX_REQ_MAX  640    /* request-line guard 512 + small headers/body */

typedef struct {
    INT  is_post;
    char path[64];
    INT  clen;
    const char *body;   /* into the read buffer (POST form)               */
    INT  body_len;
} GX_REQ;

/* very small, fixed parser (the relay's "from scratch" discipline). */
static INT gx_parse(const char *buf, INT len, GX_REQ *q)
{
    q->is_post = 0; q->clen = 0; q->body = 0; q->body_len = 0; q->path[0] = 0;

    INT i = 0;
    /* method */
    if (len >= 4 && buf[0]=='G' && buf[1]=='E' && buf[2]=='T' && buf[3]==' ') {
        i = 4;
    } else if (len >= 5 && buf[0]=='P' && buf[1]=='O' && buf[2]=='S' &&
               buf[3]=='T' && buf[4]==' ') {
        q->is_post = 1; i = 5;
    } else {
        return -1;                              /* unsupported method      */
    }
    /* path up to space, bounded; reject query strings (none used) */
    INT p = 0;
    while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n') {
        if (p < (INT)sizeof(q->path) - 1) q->path[p++] = buf[i];
        i++;
    }
    q->path[p] = 0;
    if (i >= len) return -1;

    /* find header end (\r\n\r\n) and scan for Content-Length on POST. */
    INT hdr_end = -1;
    for (INT j = 0; j + 3 < len; j++) {
        if (buf[j]=='\r' && buf[j+1]=='\n' && buf[j+2]=='\r' && buf[j+3]=='\n') {
            hdr_end = j + 4; break;
        }
    }
    if (q->is_post) {
        for (INT j = 0; j + 15 < len; j++) {
            /* case-insensitive "Content-Length:" */
            const char *k = "content-length:";
            INT m = 0;
            for (; k[m]; m++) {
                char ch = buf[j+m];
                if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
                if (ch != k[m]) break;
            }
            if (k[m] == 0) {
                INT z = j + m;
                while (z < len && (buf[z]==' ' || buf[z]=='\t')) z++;
                INT v = 0;
                while (z < len && buf[z] >= '0' && buf[z] <= '9') {
                    v = v*10 + (buf[z]-'0'); z++;
                }
                q->clen = v;
                break;
            }
        }
        if (hdr_end >= 0) {
            q->body = buf + hdr_end;
            q->body_len = len - hdr_end;
        }
    }
    return 0;
}

/* parse k=<0-7>[&v=<0-3>] from a POST form body. returns 1 ok. */
static INT gx_form_kv(const char *b, INT n, INT *k, INT *v)
{
    *k = -1; *v = -1;
    for (INT i = 0; i + 1 < n; i++) {
        if (b[i] == 'k' && b[i+1] == '=') {
            INT j = i + 2, x = 0, got = 0;
            while (j < n && b[j] >= '0' && b[j] <= '9') { x = x*10 + (b[j]-'0'); j++; got = 1; }
            if (got) *k = x;
        } else if (b[i] == 'v' && b[i+1] == '=') {
            INT j = i + 2, x = 0, got = 0;
            while (j < n && b[j] >= '0' && b[j] <= '9') { x = x*10 + (b[j]-'0'); j++; got = 1; }
            if (got) *v = x;
        }
    }
    return (*k >= 0);
}

/* ------------------------------------------------------------------ */
/* response writers                                                     */
/* ------------------------------------------------------------------ */

static void gx_resp_head(INT slot, const char *status, const char *ctype)
{
    gx_qs(slot, "HTTP/1.0 "); gx_qs(slot, status);
    gx_qs(slot, "\r\nContent-Type: "); gx_qs(slot, ctype);
    gx_qs(slot, "\r\nConnection: close\r\n\r\n");
}

static void gx_serve_page(INT slot)
{
    /* the page can exceed GX_OUTBUF; write headers then stream the body
     * straight through galaxy_io_write in chunks (best-effort; a browser
     * reads it). */
    gx_resp_head(slot, "200 OK", "text/html");
    gx_flush(slot);
    UW off = 0;
    while (off < galaxy_page_len) {
        INT chunk = (INT)(galaxy_page_len - off);
        if (chunk > 1024) chunk = 1024;
        INT w = galaxy_io_write(slot, galaxy_page + off, chunk);
        if (w < 0) return;
        if (w == 0) { tk_dly_tsk(5); continue; }
        off += (UW)w;
    }
}

/* ------------------------------------------------------------------ */
/* route one fully-read request. Returns 1 if the slot is now a held-    */
/* open SSE stream (keep it), 0 if it should be closed after flush.      */
/* ------------------------------------------------------------------ */

static INT gx_route(INT slot, GX_REQ *q)
{
    const char *path = q->path;

    if (!q->is_post && path[0]=='/' && path[1]==0) {
        gx_serve_page(slot);
        return 0;
    }
    if (!q->is_post && gx_streq(path, "/galaxy.json")) {
        gx_resp_head(slot, "200 OK", "application/json");
        gx_build_galaxy_json(slot);
        return 0;
    }
    if (!q->is_post && gx_streq(path, "/self.json")) {
        gx_resp_head(slot, "200 OK", "application/json");
        gx_build_self_json(slot);
        return 0;
    }
    if (!q->is_post && gx_streq(path, "/events")) {
        gx_qs(slot, "HTTP/1.0 200 OK\r\nContent-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
        gx_qs(slot, ": galaxy stream open\n\n");
        g_cli[slot].is_sse     = 1;
        g_cli[slot].sse_cursor = g_head;     /* start at "now"             */
        g_cli[slot].last_ping  = gx_now_ms();
        return 1;
    }
    if (q->is_post && gx_streq(path, "/teach")) {
        if (q->clen > 256 || q->body_len > 256) {
            gx_resp_head(slot, "413 Payload Too Large", "application/json");
            gx_qs(slot, "{\"ok\":false}");
            return 0;
        }
        INT k, v;
        if (gx_form_kv(q->body, q->body_len, &k, &v) && k >= 0 && v >= 0) {
            /* §6: build "teach k v" and drive THE production mouth. */
            char cmd[24]; INT cn = 0;
            cmd[cn++]='t';cmd[cn++]='e';cmd[cn++]='a';cmd[cn++]='c';cmd[cn++]='h';cmd[cn++]=' ';
            cn += gx_itoa(cmd+cn, k); cmd[cn++]=' ';
            cn += gx_itoa(cmd+cn, v);
            mind_cmd((const UB *)cmd, (UW)cn);
        }
        gx_resp_head(slot, "200 OK", "application/json");
        gx_qs(slot, "{\"ok\":true,\"pending\":"); gx_qdec(slot, (UW)r3_facts_pending());
        gx_qs(slot, ",\"rounds\":");              gx_qdec(slot, dmn_r3_rounds());
        gx_qs(slot, "}");
        return 0;
    }
    if (q->is_post && gx_streq(path, "/ask")) {
        if (q->clen > 256 || q->body_len > 256) {
            gx_resp_head(slot, "413 Payload Too Large", "application/json");
            gx_qs(slot, "{\"ok\":false}");
            return 0;
        }
        INT k, v;
        UB ak = 0, av = 0; UW ash = 0;
        if (gx_form_kv(q->body, q->body_len, &k, &v) && k >= 0) {
            char cmd[16]; INT cn = 0;
            cmd[cn++]='a';cmd[cn++]='s';cmd[cn++]='k';cmd[cn++]=' ';
            cn += gx_itoa(cmd+cn, k);
            mind_cmd((const UB *)cmd, (UW)cn);        /* §6: production mouth */
            mind_last_answer(&ak, &av, &ash);         /* read the snapshot   */
        }
        gx_resp_head(slot, "200 OK", "application/json");
        gx_qs(slot, "{\"pred\":"); gx_qdec(slot, (UW)av);
        gx_qs(slot, ",\"share\":"); gx_qdec(slot, ash);
        gx_qs(slot, "}");
        return 0;
    }

    /* §3.4: anything else is 404. NO file serving, NO directory walks. */
    gx_resp_head(slot, "404 Not Found", "text/plain");
    gx_qs(slot, "not found\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* small string helpers (file-static; no libc dependency beyond ours)   */
/* ------------------------------------------------------------------ */

static INT gx_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return (*a == 0 && *b == 0);
}
static INT gx_itoa(char *out, INT v)
{
    if (v == 0) { out[0]='0'; return 1; }
    char t[12]; INT i = 0, n = 0;
    if (v < 0) { out[n++]='-'; v = -v; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) out[n++] = t[--i];
    return n;
}

/* ------------------------------------------------------------------ */
/* the server task (§3.3)                                               */
/* ------------------------------------------------------------------ */

void galaxy_init(void)
{
    const char *off = getenv("PKERNEL_GALAXY");
    if (off && off[0] == '0') { galaxy_on = 0; galaxy_port = 0; return; }

    const char *pe = getenv("PKERNEL_GALAXY_PORT");
    if (pe && atoi(pe) > 0) {
        galaxy_port = atoi(pe);
    } else {
        UB id = gx_my_id();
        galaxy_port = 7800 + (INT)id - 1;          /* §D1 per-node offset   */
    }
    for (INT i = 0; i < 16; i++) { g_bucket[i] = 4; g_suppress[i] = 0; }
    galaxy_on = 1;                                  /* hooks live from here  */
}

/* accumulate a request into a per-slot scratch (static — never a task
 * stack buffer, the hosted-relay stack-overflow lesson). */
static char  g_req[GALAXY_MAX_CLIENTS][GX_REQ_MAX];
static INT   g_reqn[GALAXY_MAX_CLIENTS];

void galaxy_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    if (!galaxy_on) return;                         /* opted out             */

    if (galaxy_io_init(galaxy_port) < 0) {
        /* a bind failure (e.g. port busy) must not destabilize the node;
         * just report and stop serving. */
        galaxy_on = 0;
        return;
    }
    for (INT i = 0; i < GALAXY_MAX_CLIENTS; i++) { g_cli[i].in_use = 0; g_reqn[i] = 0; }

    UW last_refill = gx_now_ms();

    for (;;) {
        /* accept up to one new connection per tick. */
        INT s = galaxy_io_accept();
        if (s >= 0) {
            g_cli[s].in_use = 1; g_cli[s].is_sse = 0;
            g_cli[s].ob_len = 0; g_reqn[s] = 0;
        }

        /* §4.2 token-bucket refill (1/s) + EV_SUMMARY emission. */
        UW now = gx_now_ms();
        if (now - last_refill >= 1000) {
            last_refill = now;
            for (INT t = 0; t < 16; t++) {
                g_bucket[t] = 4;
                if (g_suppress[t]) {
                    galaxy_emit(EV_SUMMARY, gx_my_id(), GALAXY_NODE_NONE,
                                (UH)t, g_suppress[t]);
                    g_suppress[t] = 0;
                }
            }
        }

        for (INT i = 0; i < GALAXY_MAX_CLIENTS; i++) {
            if (!g_cli[i].in_use) continue;

            if (!g_cli[i].is_sse) {
                /* read request bytes into the slot scratch. */
                INT room = GX_REQ_MAX - 1 - g_reqn[i];
                if (room > 0) {
                    INT n = galaxy_io_read(i, g_req[i] + g_reqn[i], room);
                    if (n < 0) { galaxy_io_close(i); g_cli[i].in_use = 0; continue; }
                    if (n > 0) g_reqn[i] += n;
                }
                if (g_reqn[i] >= GX_REQ_MAX - 1) {
                    /* over-long request line -> 400, close (§3.6). */
                    gx_resp_head(i, "400 Bad Request", "text/plain");
                    gx_qs(i, "bad request\n"); gx_flush(i);
                    galaxy_io_close(i); g_cli[i].in_use = 0; continue;
                }
                /* a request is complete once we have the header terminator
                 * (and, for POST, the declared body). */
                g_req[i][g_reqn[i]] = 0;
                INT have_hdr = 0, hdr_end = 0;
                for (INT j = 0; j + 3 < g_reqn[i]; j++)
                    if (g_req[i][j]=='\r'&&g_req[i][j+1]=='\n'&&
                        g_req[i][j+2]=='\r'&&g_req[i][j+3]=='\n') { have_hdr=1; hdr_end=j+4; break; }
                if (!have_hdr) continue;            /* wait for more         */

                GX_REQ q;
                if (gx_parse(g_req[i], g_reqn[i], &q) < 0) {
                    gx_resp_head(i, "400 Bad Request", "text/plain");
                    gx_qs(i, "bad request\n"); gx_flush(i);
                    galaxy_io_close(i); g_cli[i].in_use = 0; continue;
                }
                if (q.is_post && (g_reqn[i] - hdr_end) < q.clen && q.clen <= 256)
                    continue;                       /* wait for the body     */

                INT keep = gx_route(i, &q);
                gx_flush(i);
                if (!keep) { galaxy_io_close(i); g_cli[i].in_use = 0; }
                continue;
            }

            /* SSE: drain the ring behind g_head into this client. */
            GX_CLIENT *c = &g_cli[i];
            /* lapped consumer: if we fell more than a ring behind, skip
             * ahead and count the loss (§4.1 — overflow shown). */
            if (g_head - c->sse_cursor > GALAXY_RING) {
                g_dropped += (g_head - c->sse_cursor) - GALAXY_RING;
                c->sse_cursor = g_head - GALAXY_RING;
            }
            while (c->sse_cursor < g_head) {
                GALAXY_EV ev = g_ring[c->sse_cursor & (GALAXY_RING - 1)];
                gx_sse_event(i, &ev);
                c->sse_cursor++;
                if (c->ob_len > GX_OUTBUF - 256) break;  /* flush room      */
            }
            /* keepalive comment every 15s so dead clients fail on write. */
            if (now - c->last_ping >= 15000) {
                c->last_ping = now;
                gx_qs(i, ": ping\n\n");
            }
            if (gx_flush(i) < 0) { galaxy_io_close(i); c->in_use = 0; }
        }

        tk_dly_tsk(50);                             /* §3.3: <=20 drains/s   */
    }
}
