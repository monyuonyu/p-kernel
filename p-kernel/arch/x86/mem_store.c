/*
 *  mem_store.c (x86)
 *  Phase 11 — 記憶永続化
 *
 *  「名もない人、歴史に残らなかった人の存在証明を永遠に残す」
 *
 *  埋め込み生成 (bag-of-chars):
 *    テキストを 8 次元ベクトルに変換する。
 *    dim0: 平均文字コード (正規化)
 *    dim1: 文字数 (正規化)
 *    dim2: 大文字/記号比
 *    dim3: 先頭4文字のハッシュ
 *    dim4-7: 文字種別ヒストグラム (小文字/数字/日本語/その他)
 *    ※ 将来: LLM embedding に差し替え可能な抽象レイヤー
 */

#include "mem_store.h"
#include "vfs.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void ms_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void ms_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { ms_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    ms_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 数学ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static float ms_sqrt(float x)
{
    if (x <= 0.0f) return 0.0f;
    float r = x > 1.0f ? x * 0.5f : 1.0f;
    r = (r + x / r) * 0.5f; r = (r + x / r) * 0.5f;
    r = (r + x / r) * 0.5f; r = (r + x / r) * 0.5f;
    return r;
}

/* ------------------------------------------------------------------ */
/* リングバッファ                                                      */
/* ------------------------------------------------------------------ */

static MEM_ENTRY ring[MEM_RING_SIZE];
static INT       ring_head  = 0;
static INT       ring_count = 0;
static UW        seq_ctr    = 0;
static UW        uptime_sec = 0;   /* 起動からの秒 (1秒タスクで更新) */

/* ------------------------------------------------------------------ */
/* ペルソナテーブル                                                    */
/* ------------------------------------------------------------------ */

static PERSONA personas[MEM_USER_MAX];
static UB      persona_valid[MEM_USER_MAX];

/* ------------------------------------------------------------------ */
/* 文字列ヘルパー                                                      */
/* ------------------------------------------------------------------ */

static INT ms_strlen(const char *s)
{
    INT n = 0; while (s[n]) n++; return n;
}

static void ms_strncpy(char *dst, const char *src, INT n)
{
    INT i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* 埋め込みベクトル生成 (bag-of-chars)                               */
/* ------------------------------------------------------------------ */

void mem_embed(const char *text, float out[MEM_EMBED_DIM])
{
    INT len = ms_strlen(text);
    if (len == 0) {
        for (INT i = 0; i < MEM_EMBED_DIM; i++) out[i] = 0.0f;
        return;
    }

    /* dim0: 平均文字コード */
    float avg = 0.0f;
    for (INT i = 0; i < len; i++) avg += (float)(unsigned char)text[i];
    out[0] = (avg / (float)len) / 255.0f;

    /* dim1: 文字数 (正規化, 最大128) */
    out[1] = (float)(len < 128 ? len : 128) / 128.0f;

    /* dim2: 大文字 + 記号の割合 */
    float upper = 0.0f;
    for (INT i = 0; i < len; i++)
        if ((text[i] >= 'A' && text[i] <= 'Z') || (text[i] < ' '))
            upper += 1.0f;
    out[2] = upper / (float)len;

    /* dim3: 先頭4文字のハッシュ */
    UW h = 0x811C9DC5UL;
    for (INT i = 0; i < 4 && i < len; i++) {
        h ^= (UW)(unsigned char)text[i];
        h *= 0x01000193UL;
    }
    out[3] = (float)(h & 0xFFFF) / 65535.0f;

    /* dim4: 小文字比 */
    float lower = 0.0f;
    for (INT i = 0; i < len; i++)
        if (text[i] >= 'a' && text[i] <= 'z') lower++;
    out[4] = lower / (float)len;

    /* dim5: 数字比 */
    float digits = 0.0f;
    for (INT i = 0; i < len; i++)
        if (text[i] >= '0' && text[i] <= '9') digits++;
    out[5] = digits / (float)len;

    /* dim6: 非ASCII比 (日本語等) */
    float nonascii = 0.0f;
    for (INT i = 0; i < len; i++)
        if ((unsigned char)text[i] > 127) nonascii++;
    out[6] = nonascii / (float)len;

    /* dim7: 末尾4文字のハッシュ */
    UW hh = 0x811C9DC5UL;
    INT start = len > 4 ? len - 4 : 0;
    for (INT i = start; i < len; i++) {
        hh ^= (UW)(unsigned char)text[i];
        hh *= 0x01000193UL;
    }
    out[7] = (float)(hh & 0xFFFF) / 65535.0f;
}

/* ------------------------------------------------------------------ */
/* コサイン類似度                                                      */
/* ------------------------------------------------------------------ */

static float cosine_sim(const float *a, const float *b, INT n)
{
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (INT i = 0; i < n; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    float denom = ms_sqrt(na) * ms_sqrt(nb);
    return denom < 1e-10f ? 0.0f : dot / denom;
}

/* ------------------------------------------------------------------ */
/* 記憶追加                                                            */
/* ------------------------------------------------------------------ */

ER mem_store_add(UB user_id, UB type, const char *text)
{
    INT idx = ring_head;
    ring[idx].timestamp = uptime_sec;
    ring[idx].user_id   = user_id;
    ring[idx].type      = type;
    ring[idx].seq       = (UH)(seq_ctr++ & 0xFFFF);
    ms_strncpy(ring[idx].text, text, MEM_TEXT_MAX);
    /* packed struct: 一時バッファ経由でコピー */
    float emb_tmp[MEM_EMBED_DIM];
    mem_embed(text, emb_tmp);
    for (INT ei = 0; ei < MEM_EMBED_DIM; ei++) ring[idx].embedding[ei] = emb_tmp[ei];

    ring_head = (ring_head + 1) % MEM_RING_SIZE;
    if (ring_count < MEM_RING_SIZE) ring_count++;

    /* ペルソナの記憶カウントを更新 */
    if (user_id < MEM_USER_MAX && persona_valid[user_id])
        personas[user_id].mem_count++;

    return E_OK;
}

/* ------------------------------------------------------------------ */
/* 類似記憶検索 (Top-N コサイン類似度)                               */
/* ------------------------------------------------------------------ */

INT mem_search(const float query_emb[MEM_EMBED_DIM], UB user_id,
               MEM_MATCH results[MEM_SEARCH_TOP])
{
    /* 簡易 Top-N: O(N*K) selection sort */
    INT found = 0;
    for (INT i = 0; i < MEM_SEARCH_TOP; i++) {
        results[i].entry = NULL;
        results[i].score = -1.0f;
    }

    for (INT i = 0; i < ring_count; i++) {
        INT slot = (ring_head - 1 - i + MEM_RING_SIZE) % MEM_RING_SIZE;
        MEM_ENTRY *e = &ring[slot];

        /* ユーザーフィルタ (0xFF = 全ユーザー) */
        if (user_id != 0xFF && e->user_id != user_id) continue;

        /* packed struct: 一時バッファ経由 */
        float e_emb[MEM_EMBED_DIM];
        for (INT ei = 0; ei < MEM_EMBED_DIM; ei++) e_emb[ei] = e->embedding[ei];
        float score = cosine_sim(query_emb, e_emb, MEM_EMBED_DIM);

        /* Top-N に挿入 */
        if (found < MEM_SEARCH_TOP || score > results[MEM_SEARCH_TOP - 1].score) {
            INT pos = found < MEM_SEARCH_TOP ? found : MEM_SEARCH_TOP - 1;
            results[pos].entry = e;
            results[pos].score = score;
            found = found < MEM_SEARCH_TOP ? found + 1 : found;

            /* バブルソート (小さいN) */
            for (INT j = pos; j > 0 && results[j].score > results[j-1].score; j--) {
                MEM_MATCH tmp = results[j]; results[j] = results[j-1]; results[j-1] = tmp;
            }
        }
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* 最新N件取得                                                         */
/* ------------------------------------------------------------------ */

INT mem_recent(UB user_id, INT n, MEM_ENTRY *out)
{
    INT found = 0;
    for (INT i = 0; i < ring_count && found < n; i++) {
        INT slot = (ring_head - 1 - i + MEM_RING_SIZE) % MEM_RING_SIZE;
        if (user_id == 0xFF || ring[slot].user_id == user_id) {
            out[found++] = ring[slot];
        }
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* ペルソナ操作                                                        */
/* ------------------------------------------------------------------ */

ER mem_persona_set(const PERSONA *p)
{
    if (!p || p->user_id >= MEM_USER_MAX) return E_PAR;
    personas[p->user_id]      = *p;
    persona_valid[p->user_id] = 1;
    return E_OK;
}

ER mem_persona_get(UB user_id, PERSONA *out)
{
    if (user_id >= MEM_USER_MAX || !persona_valid[user_id]) return E_NOEXS;
    *out = personas[user_id];
    return E_OK;
}

/* ------------------------------------------------------------------ */
/* FAT32 永続化                                                        */
/* ------------------------------------------------------------------ */

ER mem_persist(void)
{
    if (!vfs_ready) return E_NOEXS;

    /* /mem/ ディレクトリを確保 */
    vfs_mkdir("/mem");

    /* リングバッファ全体を /mem/memory.bin に書き込む */
    INT fd = vfs_create("/mem/memory.bin");
    if (fd < 0) {
        ms_puts("[mem] persist: open failed\r\n");
        return E_IO;
    }

    /* ヘッダ (count + head) */
    UW hdr[2] = { (UW)ring_count, (UW)ring_head };
    vfs_write(fd, hdr, sizeof(hdr));

    /* リングバッファ本体 */
    vfs_write(fd, ring, sizeof(MEM_ENTRY) * ring_count);

    /* ペルソナ */
    vfs_write(fd, personas,      sizeof(personas));
    vfs_write(fd, persona_valid, sizeof(persona_valid));

    vfs_close(fd);
    ms_puts("[mem] persisted  entries="); ms_putdec((UW)ring_count); ms_puts("\r\n");
    return E_OK;
}

ER mem_restore(void)
{
    if (!vfs_ready) return E_NOEXS;

    INT fd = vfs_open("/mem/memory.bin");
    if (fd < 0) return E_NOEXS;   /* 初回起動は正常 */

    UW hdr[2];
    if (vfs_read(fd, hdr, sizeof(hdr)) != sizeof(hdr)) { vfs_close(fd); return E_IO; }
    ring_count = (INT)hdr[0];
    ring_head  = (INT)hdr[1];
    if (ring_count > MEM_RING_SIZE) ring_count = MEM_RING_SIZE;

    vfs_read(fd, ring,          sizeof(MEM_ENTRY) * ring_count);
    vfs_read(fd, personas,      sizeof(personas));
    vfs_read(fd, persona_valid, sizeof(persona_valid));
    vfs_close(fd);

    ms_puts("[mem] restored  entries="); ms_putdec((UW)ring_count); ms_puts("\r\n");
    return E_OK;
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void mem_store_init(void)
{
    ring_head  = 0;
    ring_count = 0;
    seq_ctr    = 0;
    uptime_sec = 0;
    for (INT i = 0; i < MEM_USER_MAX; i++) persona_valid[i] = 0;

    /* ディスクから復元 */
    mem_restore();

    /* システム起動を記憶に残す */
    mem_store_add(0, MEM_TYPE_EVENT, "p-kernel started");

    ms_puts("[mem] memory store ready  ring="); ms_putdec(MEM_RING_SIZE);
    ms_puts("  embed_dim="); ms_putdec(MEM_EMBED_DIM); ms_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* 統計表示                                                            */
/* ------------------------------------------------------------------ */

void mem_stat(void)
{
    static const char *tname[] = { "?", "text", "sensor", "event", "summary" };
    ms_puts("[mem] entries   : "); ms_putdec((UW)ring_count);
    ms_puts("/"); ms_putdec(MEM_RING_SIZE); ms_puts("\r\n");
    ms_puts("[mem] seq_ctr   : "); ms_putdec(seq_ctr); ms_puts("\r\n");
    ms_puts("[mem] uptime    : "); ms_putdec(uptime_sec); ms_puts(" sec\r\n");

    /* 最新5件を表示 */
    ms_puts("[mem] recent:\r\n");
    MEM_ENTRY recent[5];
    INT n = mem_recent(0xFF, 5, recent);
    for (INT i = 0; i < n; i++) {
        UB t = recent[i].type < 5 ? recent[i].type : 0;
        ms_puts("  ["); ms_putdec((UW)recent[i].seq); ms_puts("] ");
        ms_puts(tname[t]); ms_puts(": ");
        ms_puts(recent[i].text); ms_puts("\r\n");
    }

    /* ペルソナ */
    ms_puts("[mem] personas:\r\n");
    for (INT i = 0; i < MEM_USER_MAX; i++) {
        if (!persona_valid[i]) continue;
        ms_puts("  uid="); ms_putdec((UW)i);
        ms_puts("  name="); ms_puts(personas[i].name);
        ms_puts("  mem="); ms_putdec(personas[i].mem_count);
        ms_puts("\r\n");
    }
}
