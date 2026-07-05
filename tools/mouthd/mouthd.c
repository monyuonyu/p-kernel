/*
 *  tools/mouthd/mouthd.c — the Frontier Mouth host companion daemon.
 *
 *  See docs/architecture/frontier_mouth_design.md §4. The kernel has NO TLS and
 *  the API key is an OWNED credential that must NEVER live inside the ownerless
 *  organism's body or p-fs. So — exactly like tools/relay — a small host-side
 *  daemon, run ONLY by the node operator who opts in, holds the key, speaks TLS
 *  to the provider (or plain HTTP to a localhost model server), and talks to the
 *  kernel over localhost FM1 datagrams (§5.1). No mouthd process ⇒ the node is a
 *  byte-honest BASELINE node (frontier.c's poll sees nothing → honest degrade).
 *
 *  Two modes (design §2):
 *    CONSULT  mouthd --provider anthropic [--model claude-... ] [--port 7801]
 *             key from ANTHROPIC_API_KEY. On a CONSULT_REQ it calls the API via
 *             the system curl (TLS in a HOST tool is fine — this is NOT kernel
 *             code) and streams the reply back LABELED for the kernel to frame.
 *    TEACH    mouthd --teach http://127.0.0.1:8080 --model qwen2-7b
 *             --license apache-2.0 [--port 7801]
 *             drives a volunteer's OpenAI-compatible local server with the same
 *             prompt-diversity discipline as student_harvest_diverse.c, then
 *             pushes the text as a TEACH_LESSON + provenance header (§5.3). The
 *             license line is enforced by frontier.h's teacher_kind enum: there
 *             is NO FRONTIER_API kind — API output can never become a lesson.
 *
 *  This tool is [live]-only: it is NEVER in CI (secret + cost + nondeterminism).
 *  The kernel-side [frontier-*] certs run against an in-process MOCK; mouthd is
 *  the real socket leg a human exercises by hand on the ThinkPad. It binds
 *  127.0.0.1 ONLY (hard-coded), like relay/galaxy_posix.
 *
 *  Build: tools/mouthd/Makefile (cc, no kernel headers — just frontier.h for the
 *  FM1 op bytes + fr_echo_of). Zero-dep like relay v2; curl is invoked, not
 *  linked.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "frontier.h"   /* FM1_* op bytes, FR_TEACH_HDR, fr_echo_of, enums */

/* ---- little-endian wire helpers (match frontier.c) ---------------------- */
static void     wr32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

static const char *g_provider = 0;    /* --provider (CONSULT)                  */
static const char *g_teach_url = 0;    /* --teach     (TEACH)                   */
static const char *g_model    = "claude-3-5-haiku-latest";
static const char *g_license  = 0;
static int         g_port     = 7801;
static int         g_verbose  = 0;

static volatile int g_run = 1;
static void on_sig(int s){ (void)s; g_run = 0; }

/* Write `body` to a temp file, return its path in `out` (caller-owned static). */
static int write_tmp(const char *body, int n, char *out, int outcap)
{
    snprintf(out, outcap, "/tmp/mouthd_%d.json", (int)getpid());
    FILE *f = fopen(out, "wb");
    if (!f) return -1;
    fwrite(body, 1, (size_t)n, f);
    fclose(f);
    return 0;
}

/* Minimal JSON string field extractor: find "\"key\":\"...\"" and copy the
 * (un-unescaped, best-effort) value. Enough to pull the model's reply text out
 * of a provider response without a JSON lib. */
static int json_str(const char *hay, const char *key, char *out, int outcap)
{
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(hay, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p==' '||*p=='\t'||*p=='\n') p++;
    if (*p!='"') return -1;
    p++;
    int o=0;
    while (*p && *p!='"' && o<outcap-1) {
        if (*p=='\\' && p[1]) { p++; if(*p=='n') out[o++]='\n'; else out[o++]=*p; p++; }
        else out[o++]=*p++;
    }
    out[o]=0;
    return o;
}

/* Call the Anthropic Messages API for CONSULT via the system curl (TLS). The
 * prompt is the kernel's assembled prompt (R3-context block + user text). The
 * reply text lands in out. Returns bytes, or <0 on error. */
static int provider_consult(const char *prompt, int plen, char *out, int outcap)
{
    const char *key = getenv("ANTHROPIC_API_KEY");
    if (!key || !*key) { fprintf(stderr, "[mouthd] ANTHROPIC_API_KEY unset\n"); return -1; }

    /* build the request body (escape the prompt into a JSON string). */
    static char body[8192]; int b=0;
    b += snprintf(body+b, sizeof body-b,
        "{\"model\":\"%s\",\"max_tokens\":512,\"messages\":[{\"role\":\"user\",\"content\":\"", g_model);
    for (int i=0;i<plen && b<(int)sizeof body-8;i++){
        unsigned char c=(unsigned char)prompt[i];
        if (c=='"'||c=='\\'){ body[b++]='\\'; body[b++]=(char)c; }
        else if (c=='\n'){ body[b++]='\\'; body[b++]='n'; }
        else if (c<0x20){ b+=snprintf(body+b,sizeof body-b,"\\u%04x",c); }
        else body[b++]=(char)c;
    }
    b += snprintf(body+b, sizeof body-b, "\"}]}");

    char bodypath[64];
    if (write_tmp(body, b, bodypath, sizeof bodypath) < 0) return -1;

    /* curl → the API. -s silent, -H headers, --data @file so the prompt never
     * rides argv (no shell-escaping hazard). The key rides an -H from env via a
     * header file so it is not visible in `ps`. */
    static char hdrpath[64];
    snprintf(hdrpath, sizeof hdrpath, "/tmp/mouthd_h_%d", (int)getpid());
    FILE *hf = fopen(hdrpath, "wb");
    if (!hf) return -1;
    fprintf(hf, "x-api-key: %s\nanthropic-version: 2023-06-01\ncontent-type: application/json\n", key);
    fclose(hf);

    char cmd[256];
    snprintf(cmd, sizeof cmd,
        "curl -s -H @%s --data @%s https://api.anthropic.com/v1/messages",
        hdrpath, bodypath);
    FILE *pp = popen(cmd, "r");
    if (!pp) { unlink(bodypath); unlink(hdrpath); return -1; }
    static char resp[16384]; int r=0; int c;
    while ((c=fgetc(pp))!=EOF && r<(int)sizeof resp-1) resp[r++]=(char)c;
    resp[r]=0;
    pclose(pp);
    unlink(bodypath); unlink(hdrpath);

    /* the Messages API returns {"content":[{"type":"text","text":"..."}], ...}. */
    int n = json_str(resp, "text", out, outcap);
    if (n < 0) { fprintf(stderr, "[mouthd] could not parse reply\n"); return -1; }
    return n;
}

/* CONSULT server loop: FM1 over localhost UDP. */
static int run_consult(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_port=htons((uint16_t)g_port);
    a.sin_addr.s_addr=htonl(0x7F000001);   /* 127.0.0.1 ONLY */
    if (bind(fd,(struct sockaddr*)&a,sizeof a)<0){ perror("bind"); return 1; }
    fprintf(stderr, "[mouthd] CONSULT provider=%s model=%s on 127.0.0.1:%d\n",
            g_provider, g_model, g_port);

    while (g_run) {
        uint8_t in[2048];
        struct sockaddr_in from; socklen_t fl=sizeof from;
        long r = recvfrom(fd, in, sizeof in, 0, (struct sockaddr*)&from, &fl);
        if (r < FM1_HDR_LEN) continue;
        if (in[0]!=FM1_MAGIC0||in[1]!=FM1_MAGIC1||in[2]!=FM1_MAGIC2) continue;
        if (in[3]!=FM1_OP_CONSULT_REQ) continue;
        const uint8_t *pl = in + FM1_HDR_LEN; long pn = r - FM1_HDR_LEN;
        if (pn < 8) continue;
        uint32_t nonce = rd32(pl);
        uint32_t plen  = rd32(pl+4);
        if ((long)plen > pn-8) plen = (uint32_t)(pn-8);

        /* HELLO first (name ourselves once per exchange in v1). */
        uint8_t hello[FM1_HDR_LEN + 1 + FR_MODEL_ID_MAX + FR_LICENSE_MAX];
        memset(hello,0,sizeof hello);
        hello[0]=FM1_MAGIC0; hello[1]=FM1_MAGIC1; hello[2]=FM1_MAGIC2; hello[3]=FM1_OP_HELLO;
        wr32(hello+4, nonce);
        hello[FM1_HDR_LEN]=FR_MODE_CONSULT;
        strncpy((char*)hello+FM1_HDR_LEN+1, g_model, FR_MODEL_ID_MAX-1);
        sendto(fd, hello, sizeof hello, 0, (struct sockaddr*)&from, fl);

        static char reply[16384];
        int rn = provider_consult((const char*)(pl+8), (int)plen, reply, sizeof reply);
        if (rn <= 0) {
            uint8_t err[FM1_HDR_LEN]; err[0]=FM1_MAGIC0;err[1]=FM1_MAGIC1;err[2]=FM1_MAGIC2;err[3]=FM1_OP_CONSULT_ERR;
            wr32(err+4, nonce);
            sendto(fd, err, sizeof err, 0, (struct sockaddr*)&from, fl);
            continue;
        }
        /* stream the reply in bounded chunks. */
        int off=0;
        while (off<rn) {
            uint8_t chunk[1200]; int co=0;
            chunk[co++]=FM1_MAGIC0;chunk[co++]=FM1_MAGIC1;chunk[co++]=FM1_MAGIC2;chunk[co++]=FM1_OP_CONSULT_CHUNK;
            wr32(chunk+4, nonce); co=FM1_HDR_LEN;
            int take=rn-off; if (take>1000) take=1000;
            memcpy(chunk+co, reply+off, (size_t)take); co+=take; off+=take;
            sendto(fd, chunk, (size_t)co, 0, (struct sockaddr*)&from, fl);
        }
        uint8_t done[FM1_HDR_LEN+5];
        done[0]=FM1_MAGIC0;done[1]=FM1_MAGIC1;done[2]=FM1_MAGIC2;done[3]=FM1_OP_CONSULT_DONE;
        wr32(done+4, nonce);
        done[FM1_HDR_LEN]=0;                       /* status ok               */
        wr32(done+FM1_HDR_LEN+1, fr_echo_of(nonce)); /* the anti-theater echo */
        sendto(fd, done, sizeof done, 0, (struct sockaddr*)&from, fl);
        if (g_verbose) fprintf(stderr, "[mouthd] consult %u: %d bytes\n", nonce, rn);
    }
    return 0;
}

/* TEACH: drive the volunteer's local model, push a lesson. Skeleton — the live
 * harvest loop mirrors tests/llm/student_harvest_diverse.c; here we validate the
 * license line and show the wire. (The full harvest is a [live] follow-up.) */
static int run_teach(void)
{
    if (!g_license || !*g_license) {
        fprintf(stderr, "[mouthd] --teach requires --license (open-license only; design §1.5)\n");
        return 1;
    }
    fprintf(stderr, "[mouthd] TEACH url=%s model=%s license=%s → node 127.0.0.1:%d\n",
            g_teach_url, g_model, g_license, g_port);
    fprintf(stderr, "[mouthd] (TEACH harvest loop is a [live] follow-up; the provenance header + "
                    "FM1 TEACH_LESSON wire is defined in frontier.h §5.3)\n");
    /* The header the kernel validates (frontier_teach_ingest): VOLUNTEER_LOCAL +
     * a non-empty license tag. FRONTIER_API cannot be expressed — the enum is
     * the floor. */
    FR_TEACH_HDR hdr; memset(&hdr,0,sizeof hdr);
    hdr.teacher_kind = FR_TEACHER_VOLUNTEER_LOCAL;
    strncpy(hdr.model_id, g_model, FR_MODEL_ID_MAX-1);
    strncpy(hdr.license_tag, g_license, FR_LICENSE_MAX-1);
    (void)hdr;
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--provider") && i+1<argc) g_provider=argv[++i];
        else if (!strcmp(argv[i],"--teach") && i+1<argc) g_teach_url=argv[++i];
        else if (!strcmp(argv[i],"--model") && i+1<argc) g_model=argv[++i];
        else if (!strcmp(argv[i],"--license") && i+1<argc) g_license=argv[++i];
        else if (!strcmp(argv[i],"--port") && i+1<argc) g_port=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-v")) g_verbose=1;
        else if (!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){
            fprintf(stderr,
              "usage: mouthd --provider anthropic [--model M] [--port P] [-v]\n"
              "       mouthd --teach http://127.0.0.1:8080 --model M --license apache-2.0 [--port P]\n"
              "env: ANTHROPIC_API_KEY (CONSULT). Binds 127.0.0.1 only. The kernel finds\n"
              "     this daemon via PKERNEL_MOUTHD_PORT (default 7801).\n");
            return 0;
        }
    }
    const char *pe=getenv("PKERNEL_MOUTHD_PORT"); if (pe&&*pe) g_port=atoi(pe);
    if (g_teach_url) return run_teach();
    if (g_provider)  return run_consult();
    fprintf(stderr, "[mouthd] need --provider (CONSULT) or --teach (TEACH); -h for help\n");
    return 1;
}
