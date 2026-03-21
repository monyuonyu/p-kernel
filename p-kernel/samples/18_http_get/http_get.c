/*
 *  08_http_get/http_get.c
 *
 *  TCP 接続 + HTTP GET サンプル
 *  sys_tcp_connect() で 10.0.2.2:80 に接続し、
 *  HTTP/1.0 GET リクエストを送信してレスポンスを表示します。
 *
 *  学べること:
 *    - sys_tcp_connect(ip, port, timeout) — TCP 接続確立
 *    - sys_tcp_write(handle, buf, len)    — データ送信
 *    - sys_tcp_read(handle, buf, len, ms) — データ受信（タイムアウト付き）
 *    - sys_tcp_close(handle)              — 接続クローズ
 *
 *  前提:
 *    QEMU の -netdev user オプション (デフォルト) を使用する場合、
 *    ホスト (10.0.2.2) でポート 80 のサービスが動いている必要があります。
 *    ネットワーク未接続の場合はタイムアウトで失敗し、
 *    その旨を表示して正常終了します。
 *
 *  実行例 (QEMU, -serial stdio, run-disk ターゲット):
 *    p-kernel> exec http_get.elf
 */
#include "plibc.h"

/* HTTP GET リクエスト送信先 */
#define SERVER_IP    SYS_IP4(10, 0, 2, 2)
#define SERVER_PORT  80
#define CONNECT_TMO  5000   /* 接続タイムアウト (ms) */
#define RECV_TMO     5000   /* 受信タイムアウト (ms) */

static const char http_req[] =
    "GET / HTTP/1.0\r\n"
    "Host: 10.0.2.2\r\n"
    "Connection: close\r\n"
    "\r\n";

void _start(void)
{
    plib_puts("========================================\r\n");
    plib_puts(" http_get: TCP + HTTP GET デモ\r\n");
    plib_puts("========================================\r\n\r\n");

    /* --- TCP 接続 --- */
    plib_puts("  tcp_connect(10.0.2.2:80): ");
    int h = sys_tcp_connect(SERVER_IP, SERVER_PORT, CONNECT_TMO);
    if (h < 0) {
        plib_puts("failed (タイムアウトまたはサーバー未起動)\r\n");
        plib_puts("\r\n  ※ QEMU ホスト (10.0.2.2) で HTTP サーバーを\r\n");
        plib_puts("    起動すると受信できます。\r\n");
        goto done;
    }
    plib_puts("OK (handle=");
    plib_puti(h);
    plib_puts(")\r\n\r\n");

    /* --- HTTP GET 送信 --- */
    plib_puts("  tcp_write: sending HTTP GET ...\r\n");
    int sent = sys_tcp_write(h, http_req, plib_strlen(http_req));
    if (sent < 0) {
        plib_puts("  tcp_write: failed\r\n");
        sys_tcp_close(h);
        goto done;
    }
    plib_puts("  tcp_write: ");
    plib_puti(sent);
    plib_puts(" bytes sent\r\n\r\n");

    /* --- レスポンス受信 --- */
    plib_puts("--- HTTP response ---\r\n");
    char buf[512];
    int total = 0;
    int n;
    while ((n = sys_tcp_read(h, buf, (int)sizeof(buf) - 1, RECV_TMO)) > 0) {
        /* 受信データをそのまま出力 (制御文字を CR+LF に変換) */
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') sys_write(1, "\r\n", 2);
            else           sys_write(1, &c, 1);
        }
        total += n;
        if (total > 8192) {          /* 安全のため 8 KB で打ち切り */
            plib_puts("\r\n[... truncated at 8KB]\r\n");
            break;
        }
    }
    plib_puts("--- end of response ---\r\n\r\n");
    plib_puts("  total: ");
    plib_puti(total);
    plib_puts(" bytes received\r\n");

    /* --- クローズ --- */
    sys_tcp_close(h);
    plib_puts("  tcp_close: OK\r\n");

done:
    plib_puts("\r\n========================================\r\n");
    plib_puts(" http_get: done\r\n");
    plib_puts("========================================\r\n");
    sys_exit(0);
}
