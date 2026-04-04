/*
 *  claude_bridge.c  (ring-3 user space)
 *  Phase 11 — Claude API ブリッジ
 *
 *  p-kernel カーネル (ring-0) の chat.c が書いた
 *  /user/prompt.txt を読み込み、ホスト上のプロキシ経由で
 *  Claude API に問い合わせ、結果を /user/response.txt に書く。
 *
 *  なぜユーザー空間か:
 *    - API キーはカーネルに渡さない (ring-3 のみに閉じる)
 *    - 個人情報 (会話履歴) はユーザー空間に留まる
 *    - カーネルはファイル IPC だけを知る
 *
 *  起動方法:
 *    p-kernel> spawn claude_bridge.elf
 *
 *  ホスト側準備:
 *    python3 claude_proxy.py --key sk-ant-...
 *    (QEMU の 10.0.2.2:8080 でリッスン)
 *
 *  プロトコル (ホストプロキシとの HTTP/1.0 平文通信):
 *    POST /v1/chat HTTP/1.0
 *    Content-Length: N
 *
 *    <prompt text>
 *
 *  レスポンス:
 *    HTTP/1.0 200 OK\r\n
 *    \r\n
 *    <response text>
 */

#include "plibc.h"

/* ------------------------------------------------------------------ */
/* 設定                                                                */
/* ------------------------------------------------------------------ */

/* QEMU host-only gateway (ホスト側プロキシのアドレス) */
#define PROXY_IP     SYS_IP4(10, 0, 2, 2)
#define PROXY_PORT   8080
#define CONN_TMO     5000    /* 接続タイムアウト (ms) */
#define RECV_TMO     30000   /* 受信タイムアウト (ms) — Claude は遅い場合がある */

#define PROMPT_PATH   "/user/prompt.txt"
#define RESPONSE_PATH "/user/response.txt"
#define READY_PATH    "/user/ready"
#define POLL_MS       500    /* prompt.txt のポーリング間隔 */

/* ------------------------------------------------------------------ */
/* 文字列ヘルパー                                                      */
/* ------------------------------------------------------------------ */

static int cb_strlen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static void cb_strncpy(char *d, const char *s, int n)
{
    int i = 0;
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void cb_itoa(int v, char *buf, int bufsz)
{
    if (bufsz <= 1) { buf[0] = '\0'; return; }
    int i = bufsz - 1; buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; cb_strncpy(buf, &buf[i], bufsz); return; }
    int neg = (v < 0); if (neg) v = -v;
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    if (neg && i > 0) buf[--i] = '-';
    /* shift to front */
    int src = i, dst = 0;
    while (buf[src]) buf[dst++] = buf[src++];
    buf[dst] = '\0';
}

/* HTTP ヘッダのみをスキップして body を探す */
static int skip_headers(const char *buf, int len)
{
    /* \r\n\r\n を探す */
    for (int i = 0; i + 3 < len; i++) {
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n')
            return i + 4;
    }
    /* \n\n でも可 */
    for (int i = 0; i + 1 < len; i++) {
        if (buf[i]=='\n' && buf[i+1]=='\n')
            return i + 2;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* ファイルに書く (上書き)                                            */
/* ------------------------------------------------------------------ */

static int write_file(const char *path, const char *data, int len)
{
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    int r = sys_write(fd, data, len);
    sys_close(fd);
    return r;
}

/* ------------------------------------------------------------------ */
/* Claude API リクエスト                                               */
/* ------------------------------------------------------------------ */

static int call_claude(const char *prompt, char *resp_out, int resp_max)
{
    int h = sys_tcp_connect(PROXY_IP, PROXY_PORT, CONN_TMO);
    if (h < 0) {
        plib_puts("[bridge] proxy connect failed\r\n");
        return -1;
    }

    /* HTTP/1.0 POST リクエスト構築 */
    int plen = cb_strlen(prompt);
    char clen_str[12];
    cb_itoa(plen, clen_str, (int)sizeof(clen_str));

    /* ヘッダ送信 */
    const char *hdr1 = "POST /v1/chat HTTP/1.0\r\nHost: 10.0.2.2\r\nContent-Type: text/plain\r\nContent-Length: ";
    sys_tcp_write(h, hdr1, cb_strlen(hdr1));
    sys_tcp_write(h, clen_str, cb_strlen(clen_str));
    const char *hdr2 = "\r\n\r\n";
    sys_tcp_write(h, hdr2, cb_strlen(hdr2));

    /* ボディ (プロンプトテキスト) */
    sys_tcp_write(h, prompt, plen);

    /* レスポンス受信 */
    char raw[2048];
    int total = 0;
    int n;
    while (total < (int)sizeof(raw) - 1 &&
           (n = sys_tcp_read(h, raw + total, (int)sizeof(raw) - 1 - total, RECV_TMO)) > 0) {
        total += n;
    }
    raw[total] = '\0';
    sys_tcp_close(h);

    if (total == 0) {
        plib_puts("[bridge] empty response\r\n");
        return -1;
    }

    /* HTTP ヘッダをスキップしてボディのみを取り出す */
    int body_off = skip_headers(raw, total);
    if (body_off < 0) body_off = 0;   /* ヘッダが見つからない場合はそのまま */

    cb_strncpy(resp_out, raw + body_off, resp_max);
    return cb_strlen(resp_out);
}

/* ------------------------------------------------------------------ */
/* メインループ                                                        */
/* ------------------------------------------------------------------ */

void _start(void)
{
    plib_puts("[bridge] Claude API bridge starting...\r\n");

    /* /user ディレクトリを確保 */
    sys_mkdir("/user");

    /* /user/ready を作成 → カーネル chat.c が API 利用可能と判断 */
    write_file(READY_PATH, "1\n", 2);
    plib_puts("[bridge] /user/ready created\r\n");
    plib_puts("[bridge] proxy endpoint: 10.0.2.2:8080\r\n");
    plib_puts("[bridge] polling: " PROMPT_PATH "\r\n");

    char prompt[512];
    char response[1024];

    for (;;) {
        /* prompt.txt が存在するまで待つ */
        int fd = sys_open(PROMPT_PATH, O_RDONLY);
        if (fd < 0) {
            tk_dly_tsk(POLL_MS);   /* POLL_MS ms 待つ */
            continue;
        }
        int plen = sys_read(fd, prompt, (int)sizeof(prompt) - 1);
        sys_close(fd);
        if (plen <= 0) {
            tk_dly_tsk(POLL_MS);
            continue;
        }
        prompt[plen] = '\0';

        /* prompt.txt を削除 (処理済みマーク) */
        sys_unlink(PROMPT_PATH);

        plib_puts("[bridge] request received, calling proxy...\r\n");

        /* Claude API 呼び出し */
        int rlen = call_claude(prompt, response, (int)sizeof(response));
        if (rlen <= 0) {
            /* エラー時はフォールバックメッセージ */
            cb_strncpy(response,
                "申し訳ありません。Claude API への接続に失敗しました。"
                "claude_proxy.py が起動しているか確認してください。",
                (int)sizeof(response));
        }

        /* response.txt に書き込む */
        write_file(RESPONSE_PATH, response, cb_strlen(response));
        plib_puts("[bridge] response written\r\n");
    }

    /* ここには到達しない */
    sys_exit(0);
}
