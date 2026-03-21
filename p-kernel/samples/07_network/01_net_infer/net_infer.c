/*
 *  samples/06_net_infer/net_infer.c
 *  センサーデータ分類 + ネットワーク配信デモ
 *
 *  p-kernel の AI 推論 syscall とネットワーク syscall を組み合わせた
 *  ring-3 ユーザー空間プログラムです。
 *
 *  Phase 1: ローカル MLP 推論 (SYS_INFER)
 *    - センサーデータ 5 シナリオを同期的に分類します。
 *    - センサー値 (温度/湿度/気圧/照度) を int8 Q8 に正規化して
 *      カーネル内の 4→8→8→3 MLP ニューラルネットワークへ渡します。
 *
 *  Phase 2: 非同期 AI ジョブ (SYS_AI_SUBMIT / SYS_AI_WAIT)
 *    - 3 ジョブを非同期でキューに投入します。
 *    - AI ワーカータスク (kernel) がバックグラウンドで処理します。
 *    - SYS_AI_WAIT で完了を待ち、分類クラスを取得します。
 *
 *  Phase 3: UDP ネットワーク送受信 (SYS_UDP_BIND / SYS_UDP_SEND / SYS_UDP_RECV)
 *    - ローカルポート 9000 をバインドします。
 *    - 危険センサーデータを UDP で送信します。
 *    - 2 秒間受信を待ちます (シングルノードでは期待しない)。
 *
 *  2 ノード構成での完全デモ:
 *    make run-node0  (terminal 0: 10.1.0.1)
 *    make run-node1  (terminal 1: 10.1.0.2)
 *    → node0 と node1 が UDP で相互にセンサーパケットを送受信できます。
 */

#include "plibc.h"

/* ================================================================= */
/* センサー値の正規化 (生の物理値 → int8 Q8)                          */
/*                                                                   */
/* 各センサーの正規化式:                                               */
/*   温度:    (°C  - 20) × 2   → 20°C=0, 45°C=50, 70°C=100         */
/*   湿度:    (% - 50)   × 2   → 50%=0,  80%=60,  20%=-60          */
/*   気圧:    (hPa-1013)  / 2  → 1013=0, 950=-31, 1030=8           */
/*   照度:    (lux-500)   / 4  → 500=0,  800=75,  100=-100         */
/* ================================================================= */

static inline int norm_temp(int t)
{
    int v = (t - 20) * 2;
    return v > 127 ? 127 : v < -127 ? -127 : v;
}

static inline int norm_hum(int h)
{
    int v = (h - 50) * 2;
    return v > 127 ? 127 : v < -127 ? -127 : v;
}

static inline int norm_press(int p)
{
    int v = (p - 1013) / 2;
    return v > 127 ? 127 : v < -127 ? -127 : v;
}

static inline int norm_light(int l)
{
    int v = (l - 500) / 4;
    return v > 127 ? 127 : v < -127 ? -127 : v;
}

/* ================================================================= */
/* テストシナリオ定義                                                  */
/* ================================================================= */

typedef struct {
    int temp;    /* 温度   (°C)  */
    int hum;     /* 湿度   (%)   */
    int press;   /* 気圧   (hPa) */
    int light;   /* 照度   (lux) */
    const char *desc;
} Scenario;

static const Scenario scenarios[] = {
    {  25,  55, 1013,  500, "室温・通常運転           " },
    {  45,  60, 1013,  500, "高温 (夏場の屋内)        " },
    {  70,  80,  950,  100, "高温・多湿・低気圧・暗所  " },
    {  20,  20, 1013,  500, "低湿度 (乾燥注意)        " },
    {  60,  65,  980,  200, "高温・低気圧 (複合異常)  " },
};

#define N_SCENARIOS  5

static const char *class_name[3] = {
    "normal  ",
    "alert   ",
    "CRITICAL",
};

/* ================================================================= */
/* _start — エントリーポイント                                         */
/* ================================================================= */

void _start(void)
{
    plib_puts("\r\n");
    plib_puts("========================================\r\n");
    plib_puts(" net_infer: AI 推論 + UDP 配信デモ\r\n");
    plib_puts("========================================\r\n\r\n");

    /* ============================================================= */
    /* Phase 1: ローカル MLP 推論 (SYS_INFER)                        */
    /* ============================================================= */
    plib_puts("--- Phase 1: ローカル MLP 推論 (SYS_INFER) ---\r\n");
    plib_puts("\r\n");
    plib_puts("  No  温度  湿度  気圧   照度  分類      シナリオ\r\n");
    plib_puts("  --  ----  ----  -----  ----  --------  "
              "------------------------\r\n");

    for (int i = 0; i < N_SCENARIOS; i++) {
        const Scenario *s = &scenarios[i];

        /* センサー値を正規化して 32bit にパック */
        int packed = SYS_SENSOR_PACK(
            norm_temp(s->temp),
            norm_hum(s->hum),
            norm_press(s->press),
            norm_light(s->light)
        );

        /* カーネル MLP で推論 */
        int cls = sys_infer(packed);

        /* 結果表示 */
        plib_puts("  ");
        plib_puti(i + 1);
        plib_puts("   ");
        plib_puti(s->temp);
        plib_puts("C  ");
        plib_puti(s->hum);
        plib_puts("%  ");
        plib_puti(s->press);
        plib_puts("hPa  ");
        plib_puti(s->light);
        plib_puts("lx  ");
        plib_puts(cls >= 0 && cls <= 2 ? class_name[cls] : "ERR     ");
        plib_puts("  ");
        plib_puts(s->desc);
        plib_puts("\r\n");
    }

    plib_puts("\r\n");

    /* ============================================================= */
    /* Phase 2: 非同期 AI ジョブ (SYS_AI_SUBMIT / SYS_AI_WAIT)      */
    /* ============================================================= */
    plib_puts("--- Phase 2: 非同期 AI ジョブ (SYS_AI_SUBMIT / SYS_AI_WAIT) ---\r\n");
    plib_puts("\r\n");

    /* 3 つのセンサーデータを非同期でキューに投入 */
    static const int test_packed[3] = {
        /* normal: 25C 55% 1013hPa 500lx */
        SYS_SENSOR_PACK(10, 10, 0, 0),
        /* alert:  45C 60% 1013hPa 500lx */
        SYS_SENSOR_PACK(50, 20, 0, 0),
        /* critical: 70C 80% 950hPa 100lx */
        SYS_SENSOR_PACK(100, 60, -32, -100),
    };
    static const char *job_desc[3] = {
        "normal (10,10,0,0)",
        "alert  (50,20,0,0)",
        "critical(100,60,-32,-100)",
    };

    int handles[3];

    /* ジョブ投入 */
    plib_puts("  [submit]\r\n");
    for (int i = 0; i < 3; i++) {
        handles[i] = sys_ai_submit(test_packed[i]);
        plib_puts("    job[");
        plib_puti(i);
        plib_puts("] handle=");
        plib_puti(handles[i]);
        plib_puts("  (");
        plib_puts(job_desc[i]);
        plib_puts(")\r\n");
    }

    plib_puts("\r\n");

    /* 完了待機 & 結果取得 */
    plib_puts("  [wait & result]\r\n");
    for (int i = 0; i < 3; i++) {
        if (handles[i] < 0) {
            plib_puts("    job[");
            plib_puti(i);
            plib_puts("] submit failed\r\n");
            continue;
        }
        int cls = sys_ai_wait(handles[i], 3000);
        plib_puts("    job[");
        plib_puti(i);
        plib_puts("] -> ");
        if (cls >= 0 && cls <= 2) {
            plib_puts(class_name[cls]);
        } else {
            plib_puts("timeout/err (");
            plib_puti(cls);
            plib_puts(")");
        }
        plib_puts("\r\n");
    }

    plib_puts("\r\n");

    /* ============================================================= */
    /* Phase 3: UDP ネットワーク送受信                                 */
    /* ============================================================= */
    plib_puts("--- Phase 3: UDP 送受信 (SYS_UDP_BIND / SEND / RECV) ---\r\n");
    plib_puts("\r\n");

    /* ポート 9000 をバインド */
    int r = sys_udp_bind(9000);
    plib_puts("  udp_bind(9000): ");
    plib_puts(r == 0 ? "OK\r\n" : "FAIL\r\n");

    /* 危険センサーアラートを QEMU ゲートウェイ (10.0.2.2:8888) へ送信 */
    /* シングルノードでは 10.0.2.2 が QEMU ホスト。                    */
    /* 2 ノード構成では送信先を 10.1.0.2 (node1) に変更する。           */
    const char alert_msg[] =
        "p-kernel ALERT: temp=70C hum=80% press=950hPa light=100lx "
        "class=CRITICAL\r\n";

    PK_SYS_UDP_SEND sp;
    sp.dst_ip   = SYS_IP4(10, 0, 2, 2);
    sp.src_port = 9000;
    sp.dst_port = 8888;
    sp.buf      = alert_msg;
    sp.len      = (unsigned short)plib_strlen(alert_msg);
    sp._pad     = 0;

    r = sys_udp_send(&sp);
    plib_puts("  udp_send to 10.0.2.2:8888: ");
    plib_puts(r == 0 ? "sent\r\n" : "queued (ARP pending)\r\n");

    /* 受信を 2 秒待機                                                */
    /* シングルノードでは誰も応答しないので E_TMOUT (-50) になります。  */
    unsigned char rbuf[128];
    PK_SYS_UDP_RECV rp;
    rp.port       = 9000;
    rp._pad       = 0;
    rp.buf        = rbuf;
    rp.buflen     = 128;
    rp._pad2      = 0;
    rp.timeout_ms = 2000;

    plib_puts("  udp_recv(port=9000, timeout=2000ms): ");
    r = sys_udp_recv(&rp);
    if (r > 0) {
        plib_puts("received ");
        plib_puti(r);
        plib_puts(" bytes from ");
        plib_put_ip(rp.src_ip);
        plib_puts(":");
        plib_putu(rp.src_port);
        plib_puts("\r\n");
    } else if (r == -50) {
        plib_puts("timeout (expected in single-node mode)\r\n");
    } else {
        plib_puts("error (");
        plib_puti(r);
        plib_puts(")\r\n");
    }

    plib_puts("\r\n");
    plib_puts("========================================\r\n");
    plib_puts(" net_infer: done\r\n");
    plib_puts("========================================\r\n");

    sys_exit(0);
}
