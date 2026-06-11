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
#include "ark_profile.h" /* ark-profile v1: /manifesto, /profile, consent  */
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

/* ark-profile: ARK_PROFILE is 1188 B — too large for the galaxy task's
 * bounded stack (the hosted-relay stack-overflow lesson). The server is a
 * SINGLE task, so one file-static scratch serves every route's head read. */
static ARK_PROFILE gx_prof;

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

/* emit a JSON-escaped string body (no surrounding quotes). Bounds: the
 * caller passes a length; control chars and "/\\ are escaped. Used for the
 * star-name (handle) + the human chapter's name/msg. The bytes are emitted
 * AS DECLARED — never verified (§3.3). */
static void gx_json_str(INT slot, const char *s, INT n)
{
    for (INT i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; gx_q(slot, e, 2); }
        else if (c == '\n') gx_qs(slot, "\\n");
        else if (c == '\r') gx_qs(slot, "\\r");
        else if (c == '\t') gx_qs(slot, "\\t");
        else if (c < 0x20)  gx_qs(slot, " ");           /* drop other ctrl   */
        else { char e[1] = { (char)c }; gx_q(slot, e, 1); }
    }
}

static void gx_build_galaxy_json(INT slot)
{
    UB me = gx_my_id();
    INT pr = world_peer_pressure(me); if (pr < 0) pr = 0;
    INT th = world_peer_threat(me);   if (th < 0) th = 0;

    INT mydv = world_peer_device(me);   /* my own beaconed device, -1 if not yet */

    gx_qs(slot, "{\"me\":{\"id\":");           gx_qdec(slot, me);
    /* ark-profile §7.4: your star gains its name — the head profile's
     * handle iff the person chose to be named (handle_len>0); a consent-
     * only profile keeps the node label (empty star). */
    gx_qs(slot, ",\"star\":\"");
    if (ark_profile_head(&gx_prof) && gx_prof.handle_len > 0)
        gx_json_str(slot, gx_prof.handle,
                    gx_prof.handle_len > ARK_HANDLE_MAX ? ARK_HANDLE_MAX : gx_prof.handle_len);
    gx_qs(slot, "\"");
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

/* full 32-byte id as 64 lowercase hex (the content-id, X-Manifesto-Id +
 * /profile.json id). */
static void gx_hex_id(INT slot, const U1 *b)
{
    static const char hx[] = "0123456789abcdef";
    char out[2 * PFS_ID_LEN + 1];
    for (INT i = 0; i < PFS_ID_LEN; i++) {
        out[i*2]   = hx[(b[i] >> 4) & 0xF];
        out[i*2+1] = hx[b[i] & 0xF];
    }
    out[2 * PFS_ID_LEN] = 0;
    gx_qs(slot, out);
}

/* parse 64 hex chars at p into id (PFS_ID_LEN bytes). returns 1 ok. */
static INT gx_parse_hex_id(const char *p, INT n, U1 id[PFS_ID_LEN])
{
    if (n < 2 * PFS_ID_LEN) return 0;
    for (INT i = 0; i < PFS_ID_LEN; i++) {
        INT hi = -1, lo = -1;
        char a = p[i*2], b = p[i*2+1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        if (hi < 0 || lo < 0) return 0;
        id[i] = (U1)((hi << 4) | lo);
    }
    return 1;
}

static void gx_build_self_json(INT slot)
{
    LM_SELF_ENTRY e;
    INT r = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                         &e, (UW)sizeof(e));
    /* dual-width: accept a v1 (116 B) or v2 (148 B) head. */
    if ((r != (INT)sizeof(e) && r != LM_SELF_ENTRY_V1_SIZE) || e.magic == 0) {
        gx_qs(slot, "{\"present\":false}");
        return;
    }
    gx_qs(slot, "{\"present\":true,\"self_id\":"); gx_qdec(slot, (UW)e.self_id);
    gx_qs(slot, ",\"seq\":");                      gx_qdec(slot, (UW)e.seq);
    gx_qs(slot, ",\"hash\":\"");                   gx_hex8(slot, e.eng_digest);
    gx_qs(slot, "\",\"prev\":\"");                 gx_hex8(slot, e.prev_entry);
    /* ark-profile §7.2/§7.4: the human chapter. human_ref hex is present
     * only on a v2 entry; the human{} object renders iff the person chose
     * to be named (handle_len>0). A consent-only profile -> 「名もなき同意」
     * (human:{named:false}). The standing footnote (改竄検出可・偽造不可で
     * はない) lives in the page, exactly as galaxy.md §5 carries it. */
    gx_qs(slot, "\",\"version\":");                gx_qdec(slot, (UW)e.version);
    if (r == (INT)sizeof(e)) {
        gx_qs(slot, ",\"human_ref\":\"");          gx_hex_id(slot, e.human_ref);
        gx_qs(slot, "\"");
    }
    if (ark_profile_head(&gx_prof) && gx_prof.consent_ack) {
        gx_qs(slot, ",\"human\":{\"prof_seq\":"); gx_qdec(slot, (UW)gx_prof.seq);
        gx_qs(slot, ",\"named\":");
        gx_qs(slot, gx_prof.handle_len > 0 ? "true" : "false");
        if (gx_prof.handle_len > 0) {
            gx_qs(slot, ",\"handle\":\"");
            gx_json_str(slot, gx_prof.handle,
                        gx_prof.handle_len > ARK_HANDLE_MAX ? ARK_HANDLE_MAX : gx_prof.handle_len);
            gx_qs(slot, "\"");
        }
        if (gx_prof.name_len > 0) {
            gx_qs(slot, ",\"name\":\"");
            gx_json_str(slot, gx_prof.name,
                        gx_prof.name_len > ARK_NAME_MAX ? ARK_NAME_MAX : gx_prof.name_len);
            gx_qs(slot, "\"");
        }
        if (gx_prof.msg_len > 0) {
            INT mn = (INT)gx_prof.msg_len; if (mn > ARK_MSG_MAX) mn = ARK_MSG_MAX;
            gx_qs(slot, ",\"msg\":\"");
            gx_json_str(slot, gx_prof.msg, mn);
            gx_qs(slot, "\"");
        }
        gx_qs(slot, "}");
    }
    gx_qs(slot, "}");
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

/* request-line guard + headers + body. ark-profile.md §7.2 raises the
 * POST limit to 2 KB for the one /profile route (handle+name+1 KB msg,
 * URL-encoded); 3072 leaves room for the request line + headers above a
 * 2 KB body. /teach and /ask stay bounded at GX_POST_SMALL (256). */
#define GX_REQ_MAX     3072
#define GX_POST_SMALL  256     /* /teach, /ask body cap                   */
#define GX_POST_PROF   2048    /* /profile body cap (§7.2)                */

typedef struct {
    INT  is_post;
    char path[64];
    char query[64];     /* raw query string after '?' (i18n: lang=xx)     */
    char accept_lang[64];/* Accept-Language header value (i18n auto-detect)*/
    INT  clen;
    const char *body;   /* into the read buffer (POST form)               */
    INT  body_len;
} GX_REQ;

/* very small, fixed parser (the relay's "from scratch" discipline). */
static INT gx_parse(const char *buf, INT len, GX_REQ *q)
{
    q->is_post = 0; q->clen = 0; q->body = 0; q->body_len = 0; q->path[0] = 0;
    q->query[0] = 0; q->accept_lang[0] = 0;

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
    /* path up to space; split off the query string at '?' (i18n lang=xx). */
    INT p = 0, inq = 0, qp = 0;
    while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n') {
        if (!inq && buf[i] == '?') { inq = 1; i++; continue; }
        if (inq) { if (qp < (INT)sizeof(q->query) - 1) q->query[qp++] = buf[i]; }
        else     { if (p  < (INT)sizeof(q->path)  - 1) q->path[p++]   = buf[i]; }
        i++;
    }
    q->path[p] = 0; q->query[qp] = 0;
    if (i >= len) return -1;

    /* find header end (\r\n\r\n) and scan for Content-Length on POST. */
    INT hdr_end = -1;
    for (INT j = 0; j + 3 < len; j++) {
        if (buf[j]=='\r' && buf[j+1]=='\n' && buf[j+2]=='\r' && buf[j+3]=='\n') {
            hdr_end = j + 4; break;
        }
    }

    /* i18n: capture Accept-Language (case-insensitive header name) so GET
     * /manifesto with no ?lang= can auto-default to the browser language. We
     * store the raw value; the route runs a minimal q-less prefix matcher. */
    for (INT j = 0; j + 16 < len; j++) {
        if (j != 0 && !(buf[j-1]=='\n')) continue;   /* header line start */
        const char *k = "accept-language:";
        INT m = 0;
        for (; k[m]; m++) {
            char ch = buf[j+m];
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
            if (ch != k[m]) break;
        }
        if (k[m] == 0) {
            INT z = j + m;
            while (z < len && (buf[z]==' ' || buf[z]=='\t')) z++;
            INT a = 0;
            while (z < len && buf[z] != '\r' && buf[z] != '\n'
                   && a < (INT)sizeof(q->accept_lang) - 1)
                q->accept_lang[a++] = buf[z++];
            q->accept_lang[a] = 0;
            break;
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

/* one hex nibble or -1 */
static INT gx_hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Extract a URL-encoded form field `name=value` from a &-delimited body
 * into out (NUL-terminated, bounded by outmax-1). Returns the decoded
 * length, or -1 if the field is absent. '+' -> space, %XX -> byte. The
 * profile fields (handle/name/msg) are stored AS DECODED — never verified
 * (ark-profile.md §3.3: 誰もそれを検証しません). */
static INT gx_form_field(const char *b, INT n, const char *name,
                         char *out, INT outmax)
{
    INT nl = 0; while (name[nl]) nl++;
    for (INT i = 0; i < n; i++) {
        /* field starts at i if (i==0 or b[i-1]=='&') and matches name '=' */
        if (i != 0 && b[i-1] != '&') continue;
        INT m = 0;
        while (m < nl && i + m < n && b[i+m] == name[m]) m++;
        if (m != nl || i + m >= n || b[i+m] != '=') continue;
        INT j = i + m + 1, o = 0;
        while (j < n && b[j] != '&' && o < outmax - 1) {
            char c = b[j];
            if (c == '+') { out[o++] = ' '; j++; }
            else if (c == '%' && j + 2 < n) {
                INT hi = gx_hexv(b[j+1]), lo = gx_hexv(b[j+2]);
                if (hi >= 0 && lo >= 0) { out[o++] = (char)((hi<<4)|lo); j += 3; }
                else { out[o++] = c; j++; }
            } else { out[o++] = c; j++; }
        }
        out[o] = 0;
        return o;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* i18n language resolution (ark-profile.md §7.5)                       */
/* ------------------------------------------------------------------ */

/* copy a token (up to a delimiter / NUL) into out, NUL-terminated. */
static void gx_copy_tok(const char *s, INT n, char *out, INT outmax)
{
    INT o = 0;
    for (INT i = 0; i < n && s[i] && o < outmax - 1; i++) {
        char c = s[i];
        if (c == ',' || c == ';' || c == ' ' || c == '&' || c == '\r' || c == '\n') break;
        out[o++] = c;
    }
    out[o] = 0;
}

/* extract lang=<code> from a raw query string -> idx, or -1 if absent. */
static INT gx_lang_from_query(const char *qy)
{
    for (INT i = 0; qy[i]; i++) {
        if ((i == 0 || qy[i-1] == '&') &&
            qy[i]=='l' && qy[i+1]=='a' && qy[i+2]=='n' && qy[i+3]=='g' && qy[i+4]=='=') {
            char code[24]; gx_copy_tok(qy + i + 5, 23, code, (INT)sizeof code);
            return ark_manifesto_find(code);
        }
    }
    return -1;
}

/* best language for an Accept-Language value: scan comma-separated tags in
 * order (a minimal q-less matcher — the FIRST tag whose code or primary
 * subtag resolves wins). Returns a table index, or -1 if none match. */
static INT gx_lang_from_accept(const char *al)
{
    INT i = 0;
    while (al[i]) {
        while (al[i] == ' ' || al[i] == ',') i++;
        if (!al[i]) break;
        char tag[24]; gx_copy_tok(al + i, 23, tag, (INT)sizeof tag);
        if (tag[0]) {
            INT idx = ark_manifesto_find(tag);
            if (idx >= 0) return idx;
        }
        /* advance past this tag to the next comma */
        while (al[i] && al[i] != ',') i++;
    }
    return -1;
}

/* resolve the language a /manifesto request wants: ?lang= first, then
 * Accept-Language, then fall back en (index found via "en"), then row 0. */
static INT gx_resolve_lang(const GX_REQ *q)
{
    INT idx = gx_lang_from_query(q->query);
    if (idx >= 0) return idx;
    idx = gx_lang_from_accept(q->accept_lang);
    if (idx >= 0) return idx;
    idx = ark_manifesto_find("en");
    if (idx >= 0) return idx;
    return 0;                                 /* canonical (ja) last resort */
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
    /* ark-profile.md §7.2: GET /manifesto — the embedded bytes + the
     * content-id they hash to, so the consent ack binds to the exact words
     * (the cert computes sha256 host-side and matches X-Manifesto-Id). */
    if (!q->is_post && gx_streq(path, "/manifesto")) {
        /* i18n (§7.5): ?lang=xx, else Accept-Language, else en, else ja.
         * Each version is its own byte string -> its own content-id; the
         * X-Manifesto-Id advertised is THIS version's id, and the ack binds
         * to exactly the words served. X-Manifesto-Lang names the version. */
        INT li = gx_resolve_lang(q);
        const U1 *mb = 0; UW ml = 0; U1 mid[PFS_ID_LEN];
        if (!ark_manifesto_at((UW)li, &mb, &ml, mid)) {
            ark_manifesto_at(0, &mb, &ml, mid); li = 0;   /* defensive */
        }
        gx_qs(slot, "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8"
                    "\r\nX-Manifesto-Id: ");
        gx_hex_id(slot, mid);
        gx_qs(slot, "\r\nX-Manifesto-Lang: ");
        gx_qs(slot, ark_manifesto_code((UW)li));
        gx_qs(slot, "\r\nConnection: close\r\n\r\n");
        gx_flush(slot);
        UW off = 0;
        while (off < ml) {
            INT chunk = (INT)(ml - off); if (chunk > 1024) chunk = 1024;
            INT w = galaxy_io_write(slot, mb + off, chunk);
            if (w < 0) return 0;
            if (w == 0) { tk_dly_tsk(5); continue; }
            off += (UW)w;
        }
        return 0;
    }
    /* §7.5: GET /langs — the available languages as {code:endonym,...} JSON,
     * each name in its OWN language; the UI populates its selector from this
     * and picks navigator.language. */
    if (!q->is_post && gx_streq(path, "/langs")) {
        gx_resp_head(slot, "200 OK", "application/json");
        gx_qs(slot, "{");
        UW n = ark_manifesto_count();
        for (UW li = 0; li < n; li++) {
            if (li) gx_qs(slot, ",");
            const char *code = ark_manifesto_code(li);
            const char *endo = ark_manifesto_endonym(li);
            gx_qs(slot, "\""); gx_qs(slot, code); gx_qs(slot, "\":\"");
            INT en = 0; while (endo[en]) en++;
            gx_json_str(slot, endo, en);
            gx_qs(slot, "\"");
        }
        gx_qs(slot, "}");
        return 0;
    }
    /* §7.2: GET /profile.json — the head profile, or {none:true}. */
    if (!q->is_post && gx_streq(path, "/profile.json")) {
        gx_resp_head(slot, "200 OK", "application/json");
        if (!ark_profile_head(&gx_prof)) { gx_qs(slot, "{\"none\":true}"); return 0; }
        U1 pid[PFS_ID_LEN]; pfs_id_compute(&gx_prof, (UW)sizeof gx_prof, pid);
        gx_qs(slot, "{\"seq\":");        gx_qdec(slot, (UW)gx_prof.seq);
        gx_qs(slot, ",\"node\":");       gx_qdec(slot, (UW)gx_prof.self_id);
        gx_qs(slot, ",\"handle_len\":"); gx_qdec(slot, (UW)gx_prof.handle_len);
        gx_qs(slot, ",\"name_len\":");   gx_qdec(slot, (UW)gx_prof.name_len);
        gx_qs(slot, ",\"msg_len\":");    gx_qdec(slot, (UW)gx_prof.msg_len);
        gx_qs(slot, ",\"consent\":{\"acked\":");
        gx_qdec(slot, (UW)(gx_prof.consent_ack ? 1 : 0));
        gx_qs(slot, ",\"manifesto_id\":\""); gx_hex_id(slot, gx_prof.manifesto_id);
        gx_qs(slot, "\"},\"id\":\""); gx_hex_id(slot, pid);
        gx_qs(slot, "\"}");
        return 0;
    }
    /* §7.2: POST /profile — validate mid, build + save, link the chapter. */
    if (q->is_post && gx_streq(path, "/profile")) {
        if (q->clen > GX_POST_PROF || q->body_len > GX_POST_PROF) {
            gx_resp_head(slot, "413 Payload Too Large", "application/json");
            gx_qs(slot, "{\"ok\":false}");
            return 0;
        }
        /* ack=1 required, mid must equal the served manifesto id. */
        char ackbuf[8];
        INT acked = (gx_form_field(q->body, q->body_len, "ack", ackbuf, 8) >= 0
                     && ackbuf[0] == '1');
        static char midhex[2 * PFS_ID_LEN + 8];
        INT mlen = gx_form_field(q->body, q->body_len, "mid", midhex,
                                 (INT)sizeof midhex);
        U1 mid[PFS_ID_LEN], cur[PFS_ID_LEN];
        ark_manifesto_id(cur);
        INT mid_ok = (mlen >= 2 * PFS_ID_LEN)
                  && gx_parse_hex_id(midhex, mlen, mid);
        /* §7.3: a wrong/absent mid -> 409 + the current id (consent is to
         * the exact words, not a brand). */
        if (!acked || !mid_ok) {
            gx_resp_head(slot, "409 Conflict", "application/json");
            gx_qs(slot, "{\"ok\":false,\"reason\":\"mid\",\"manifesto_id\":\"");
            gx_hex_id(slot, cur); gx_qs(slot, "\"}");
            return 0;
        }
        /* pseudonymous first-class: every disclosure field optional. */
        static char hbuf[ARK_HANDLE_MAX + 1], nbuf[ARK_NAME_MAX + 1];
        static char mbuf[ARK_MSG_MAX + 1];
        INT hl = gx_form_field(q->body, q->body_len, "handle", hbuf, sizeof hbuf);
        INT nl = gx_form_field(q->body, q->body_len, "name",   nbuf, sizeof nbuf);
        INT ml = gx_form_field(q->body, q->body_len, "msg",    mbuf, sizeof mbuf);
        U1 pid[PFS_ID_LEN]; U4 seq = 0;
        INT rc = ark_profile_save(1, mid,
                                  hl > 0 ? hbuf : 0, hl > 0 ? (UW)hl : 0,
                                  nl > 0 ? nbuf : 0, nl > 0 ? (UW)nl : 0,
                                  ml > 0 ? mbuf : 0, ml > 0 ? (UW)ml : 0,
                                  pid, &seq);
        if (rc != 1) {
            gx_resp_head(slot, "409 Conflict", "application/json");
            gx_qs(slot, "{\"ok\":false,\"reason\":\"mid\",\"manifesto_id\":\"");
            gx_hex_id(slot, cur); gx_qs(slot, "\"}");
            return 0;
        }
        gx_resp_head(slot, "200 OK", "application/json");
        gx_qs(slot, "{\"ok\":true,\"seq\":"); gx_qdec(slot, (UW)seq);
        gx_qs(slot, ",\"id\":\""); gx_hex_id(slot, pid); gx_qs(slot, "\"}");
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
        if (q->clen > GX_POST_SMALL || q->body_len > GX_POST_SMALL) {
            gx_resp_head(slot, "413 Payload Too Large", "application/json");
            gx_qs(slot, "{\"ok\":false}");
            return 0;
        }
        /* ark-profile.md §7.3: the 共感 gate. The mind will not take a
         * human's words into permanent memory from someone who has not
         * been told what permanent means. consent != disclosure: an
         * ack-only empty profile unlocks teach (ark_consent_ok honors it).
         * /ask is NOT gated; the SHELL mouth is NOT gated (operator trust). */
        if (!ark_consent_ok()) {
            gx_resp_head(slot, "403 Forbidden", "application/json");
            gx_qs(slot, "{\"refused\":\"manifesto\",\"see\":\"/manifesto\"}");
            return 0;
        }
        INT k, v;
        if (gx_form_kv(q->body, q->body_len, &k, &v) && k >= 0 && v >= 0) {
            /* §6: build "teach k v" and drive THE production mouth. §5: tag
             * the provenance as a WEB teach for the ONE prov site in
             * m_teach (reset to shell there, one-shot). */
            ark_teach_src_set(ARK_PROV_SRC_WEB);
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
        if (q->clen > GX_POST_SMALL || q->body_len > GX_POST_SMALL) {
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
                if (q.is_post && (g_reqn[i] - hdr_end) < q.clen
                    && q.clen <= GX_POST_PROF)
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
