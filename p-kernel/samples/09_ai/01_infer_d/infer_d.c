/*
 *  infer_d.c — p-kernel 分散 Transformer 推論デーモン (Phase 12)
 *
 *  「AIの魂はring-0、AIの体はring-3」アーキテクチャの体側デーモン。
 *  /etc/init.rc から guard 付きで自動起動される。
 *
 *  役割:
 *    - K-DDS トピック "ai/req" を subscribe して推論リクエストを受信
 *    - SYS_DTR_SUBMIT syscall 経由で ring-0 の dtr_infer() を呼び出す
 *      (内部で縮退レベルに応じた分散戦略を自動選択)
 *    - 結果を K-DDS トピック "ai/rsp" に publish する
 *
 *  プロトコル:
 *    ai/req パケット (12 bytes):
 *      magic      = 0x51455241 ("AREQ" LE)
 *      req_id     = リクエスト ID (呼び出し元が設定)
 *      sensor_packed = SENSOR_PACK(temp, humidity, pressure, light)
 *
 *    ai/rsp パケット (12 bytes):
 *      magic      = 0x50535241 ("ARSP" LE)
 *      req_id     = 対応するリクエスト ID
 *      class_id   = 0=normal, 1=alert, 2=critical, -1=error
 *
 *  縮退モード連動:
 *    - FULL   (3+ nodes): Pipeline Parallel — Node0: Embed+MHSA, Node1: FFN+Cls
 *    - REDUCED (2 nodes): Tensor Parallel   — head0/head1 を並列計算
 *    - SOLO   (1 node)  : ローカル全実行   — 分散なし
 *
 *  heal ガード: heal_register("infer_d", ...) により クラッシュ時に自動再起動
 */

#include "plibc.h"

/* ------------------------------------------------------------------ */
/* プロトコル定数                                                      */
/* ------------------------------------------------------------------ */

#define AI_REQ_MAGIC  0x51455241UL   /* "AREQ" LE */
#define AI_RSP_MAGIC  0x50535241UL   /* "ARSP" LE */

#define AI_TOPIC_REQ  "ai/req"
#define AI_TOPIC_RSP  "ai/rsp"

#define AI_REQ_TIMEOUT_MS  1200   /* リクエスト待ちタイムアウト (ms) */

/* ------------------------------------------------------------------ */
/* パケット構造体                                                      */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    unsigned int  magic;
    unsigned int  req_id;
    int           sensor_packed;   /* DTR_SENSOR_PACK(t,h,p,l) */
} AI_REQ_PKT;   /* 12 bytes */

typedef struct __attribute__((packed)) {
    unsigned int  magic;
    unsigned int  req_id;
    int           class_id;        /* 0=normal 1=alert 2=critical -1=err */
} AI_RSP_PKT;   /* 12 bytes */

/* ------------------------------------------------------------------ */
/* ユーティリティ                                                      */
/* ------------------------------------------------------------------ */

static void d_puts(const char *s)    { plib_puts(s); }
static void d_putu(unsigned int v)   { plib_putu(v); }

/* ------------------------------------------------------------------ */
/* _start                                                              */
/* ------------------------------------------------------------------ */

void _start(void)
{
    d_puts("[infer_d] starting — Phase 12 Transformer inference daemon\r\n");

    /* K-DDS トピックを開く */
    int h_req = sys_topic_open(AI_TOPIC_REQ, 1);  /* QoS=1 RELIABLE */
    if (h_req < 0) {
        d_puts("[infer_d] ERROR: cannot open ai/req topic\r\n");
        sys_exit(1);
    }

    int h_rsp = sys_topic_open(AI_TOPIC_RSP, 1);
    if (h_rsp < 0) {
        d_puts("[infer_d] ERROR: cannot open ai/rsp topic\r\n");
        sys_topic_close(h_req);
        sys_exit(1);
    }

    d_puts("[infer_d] listening on ai/req, publishing to ai/rsp\r\n");

    static AI_REQ_PKT req;
    unsigned int infer_count = 0;

    for (;;) {
        /* リクエストを受信 (AI_REQ_TIMEOUT_MS ms 待機) */
        int n = sys_topic_sub(h_req, &req, (int)sizeof(req),
                              AI_REQ_TIMEOUT_MS);

        if (n < (int)sizeof(AI_REQ_PKT)) {
            /* タイムアウトまたはサイズ不正 — ループ継続 */
            continue;
        }

        if (req.magic != AI_REQ_MAGIC) {
            d_puts("[infer_d] WARN: invalid magic\r\n");
            continue;
        }

        /* SYS_DTR_SUBMIT で ring-0 の Transformer 推論を呼ぶ */
        int class_id = sys_dtr_submit(req.sensor_packed);

        infer_count++;

        /* デバッグ出力 (32回に1回) */
        if (infer_count % 32 == 1) {
            d_puts("[infer_d] req_id=");
            d_putu(req.req_id);
            d_puts("  class=");
            d_putu((unsigned int)class_id);
            d_puts("  total=");
            d_putu(infer_count);
            d_puts("\r\n");
        }

        /* 結果を ai/rsp に publish */
        AI_RSP_PKT rsp;
        rsp.magic    = AI_RSP_MAGIC;
        rsp.req_id   = req.req_id;
        rsp.class_id = class_id;
        sys_topic_pub(h_rsp, &rsp, (int)sizeof(rsp));
    }

    /* 到達しない */
    sys_topic_close(h_req);
    sys_topic_close(h_rsp);
    sys_exit(0);
}
