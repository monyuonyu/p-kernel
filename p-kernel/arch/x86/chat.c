/*
 *  chat.c (x86)
 *  Phase 11 — AI 会話インターフェース
 *
 *  会話の記憶が積み上がるほど、AIはその人のことを深く知る。
 *  p-kernel が生きている限り、その記憶は消えない。
 */

#include "chat.h"
#include "mem_store.h"
#include "vfs.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);
IMPORT INT  sio_read_line(UB *buf, INT max);   /* シリアルから1行読む */

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void ch_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void ch_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { ch_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    ch_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 文字列ヘルパー                                                      */
/* ------------------------------------------------------------------ */

static INT ch_strlen(const char *s) { INT n = 0; while(s[n]) n++; return n; }

static void ch_strncpy(char *d, const char *s, INT n)
{
    INT i = 0;
    while (i < n-1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static INT ch_strncmp(const char *a, const char *b, INT n)
{
    for (INT i = 0; i < n; i++) {
        if (a[i] != b[i]) return (INT)(unsigned char)a[i] - (INT)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

static UW stat_turns      = 0;
static UW stat_api_turns  = 0;
static UW stat_local_turns = 0;

/* ------------------------------------------------------------------ */
/* API ブリッジ確認                                                    */
/* ------------------------------------------------------------------ */

UB chat_api_check(void)
{
    if (!vfs_ready) return 0;
    /* /user/prompt.txt が存在するかで bridge の有無を判断 */
    /* 実際は bridge が /user/ready というファイルを作る */
    INT fd = vfs_open("/user/ready");
    if (fd < 0) return 0;
    vfs_close(fd);
    return 1;
}

/* ------------------------------------------------------------------ */
/* ローカル応答生成 (記憶ベース)                                     */
/* ------------------------------------------------------------------ */

static void local_response(const char *input, const MEM_MATCH *matches,
                            INT n_match, char *out, INT out_max)
{
    /* 入力の意図を分類 */
    INT is_question = 0;
    INT len = ch_strlen(input);
    for (INT i = 0; i < len; i++) {
        if (input[i] == '?' || (unsigned char)input[i] == 0xBF) { is_question = 1; break; }
    }

    if (n_match > 0 && matches[0].score > 0.85f) {
        /* 高類似度の記憶がある → 想起して応答 */
        ch_strncpy(out, "[記憶から] ", out_max);
        INT pos = ch_strlen(out);
        ch_strncpy(out + pos, matches[0].entry->text,
                   out_max - pos - 1);
    } else if (is_question) {
        ch_strncpy(out, "この p-kernel は人類の記録を永遠に守るために動いています。"
                        "あなたの言葉も今、記憶に刻まれました。", out_max);
    } else {
        ch_strncpy(out, "記憶しました。", out_max);
    }
}

/* ------------------------------------------------------------------ */
/* API 経由で Claude に問い合わせ                                    */
/* ------------------------------------------------------------------ */

static INT api_request(const char *prompt, const char *context,
                        char *out, INT out_max)
{
    if (!vfs_ready) return 0;

    /* /user/prompt.txt に書き込む */
    vfs_mkdir("/user");
    INT fd = vfs_create(CHAT_PROMPT_PATH);
    if (fd < 0) return 0;

    /* フォーマット: "CONTEXT:\n...\nPROMPT:\n...\n" */
    const char *c_hdr = "CONTEXT:\n";
    vfs_write(fd, c_hdr, ch_strlen(c_hdr));
    if (context && ch_strlen(context) > 0)
        vfs_write(fd, context, ch_strlen(context));
    const char *p_hdr = "\nPROMPT:\n";
    vfs_write(fd, p_hdr, ch_strlen(p_hdr));
    vfs_write(fd, prompt, ch_strlen(prompt));
    vfs_write(fd, "\n", 1);
    vfs_close(fd);

    /* claude_bridge.elf が処理するのを待つ */
    ch_puts("[chat] waiting for API response...\r\n");
    INT waited = 0;
    while (waited < CHAT_API_TMO_MS) {
        tk_dly_tsk(200);
        waited += 200;
        fd = vfs_open(CHAT_RESPONSE_PATH);
        if (fd >= 0) {
            INT r = vfs_read(fd, out, out_max - 1);
            vfs_close(fd);
            if (r > 0) {
                out[r] = '\0';
                /* レスポンスファイルを削除 (次回のために) */
                vfs_unlink(CHAT_RESPONSE_PATH);
                return 1;
            }
        }
    }
    return 0;   /* タイムアウト */
}

/* ------------------------------------------------------------------ */
/* 1ターン処理                                                         */
/* ------------------------------------------------------------------ */

void chat_turn(CHAT_SESSION *sess, const char *input, char *response_out, INT resp_max)
{
    stat_turns++;
    sess->turn_count++;

    /* 入力を記憶に追加 */
    mem_store_add(sess->user_id, MEM_TYPE_TEXT, input);

    /* 埋め込み + 類似記憶検索 */
    float q_emb[MEM_EMBED_DIM];
    mem_embed(input, q_emb);
    MEM_MATCH matches[MEM_SEARCH_TOP];
    INT n_match = mem_search(q_emb, sess->user_id, matches);

    /* 類似記憶を文脈として構築 */
    char context[256];
    context[0] = '\0';
    if (n_match > 0 && matches[0].score > 0.5f) {
        ch_puts("[chat] 関連記憶: ");
        INT cpos = 0;
        for (INT i = 0; i < n_match && i < 2; i++) {
            if (matches[i].score < 0.3f) break;
            ch_puts("["); ch_putdec((UW)(INT)(matches[i].score * 100)); ch_puts("%] ");
            ch_puts(matches[i].entry->text); ch_puts("\r\n");
            /* context に追記 */
            const char *mt = matches[i].entry->text;
            INT mtl = ch_strlen(mt);
            if (cpos + mtl + 2 < 255) {
                ch_strncpy(context + cpos, mt, 255 - cpos);
                cpos += mtl;
                context[cpos++] = '\n';
                context[cpos]   = '\0';
            }
        }
    }

    /* API ブリッジ優先 */
    if (sess->api_available && api_request(input, context, response_out, resp_max)) {
        stat_api_turns++;
        ch_puts("[chat][Claude API] ");
    } else {
        /* ローカル応答 */
        local_response(input, matches, n_match, response_out, resp_max);
        stat_local_turns++;
        ch_puts("[chat][local] ");
    }

    ch_puts(response_out); ch_puts("\r\n");

    /* 応答も記憶に追加 */
    mem_store_add(sess->user_id, MEM_TYPE_TEXT, response_out);
}

/* ------------------------------------------------------------------ */
/* インタラクティブ会話ループ                                         */
/* ------------------------------------------------------------------ */

void chat_run(UB user_id)
{
    CHAT_SESSION sess = { 0 };
    sess.user_id       = user_id;
    sess.turn_count    = 0;
    sess.api_available = chat_api_check();

    /* ペルソナ表示 */
    PERSONA p;
    if (mem_persona_get(user_id, &p) == E_OK) {
        ch_puts("[chat] こんにちは、"); ch_puts(p.name); ch_puts("さん。\r\n");
        ch_puts("[chat] あなたの記憶: "); ch_putdec(p.mem_count);
        ch_puts(" 件が保存されています。\r\n");
    } else {
        ch_puts("[chat] はじめまして。あなたの記憶を刻み始めます。\r\n");
    }

    if (sess.api_available) {
        ch_puts("[chat] Claude API: 接続済み\r\n");
    } else {
        ch_puts("[chat] Claude API: 未接続 (claude_bridge.elf を起動してください)\r\n");
        ch_puts("[chat]   -> spawn claude_bridge.elf\r\n");
    }
    ch_puts("[chat] 終了: 'exit' または Ctrl+C\r\n\r\n");

    char input[128];
    char response[512];

    for (;;) {
        ch_puts("you> ");

        /* シリアルから1行読む */
        INT len = sio_read_line((UB *)input, (INT)sizeof(input));
        if (len <= 0) continue;

        /* exit */
        if (ch_strncmp(input, "exit", 4) == 0) {
            ch_puts("[chat] 会話を終了します。記憶は残ります。\r\n");
            /* 会話を永続化 */
            mem_persist();
            break;
        }
        if (len == 0 || input[0] == '\0') continue;

        chat_turn(&sess, input, response, (INT)sizeof(response));
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void chat_init(void)
{
    stat_turns = stat_api_turns = stat_local_turns = 0;
    ch_puts("[chat] AI conversation interface ready\r\n");
}

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

void chat_stat(void)
{
    ch_puts("[chat] turns      : "); ch_putdec(stat_turns);       ch_puts("\r\n");
    ch_puts("[chat] api_turns  : "); ch_putdec(stat_api_turns);   ch_puts("\r\n");
    ch_puts("[chat] local_turns: "); ch_putdec(stat_local_turns); ch_puts("\r\n");
    ch_puts("[chat] api_avail  : "); ch_puts(chat_api_check() ? "yes" : "no"); ch_puts("\r\n");
}
