/*
 *  vital.c (x86)
 *  生命兆候モニタリング — フェーズ 6: 生存本能
 *
 *  各ノードが "vital/N" トピックへ自分の健康状態を毎秒発行する。
 *  これにより:
 *    - クラスタ全体でリアルタイムにノードの生死を確認できる
 *    - replica_stats から「複製活動」が継続していることを証明できる
 *    - 隣ノードが vital データを受信するたびに「隣が今も生きている」ことを感じる
 */

#include "vital.h"
#include "replica.h"
#include "netstack.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* 出力ヘルパ                                                          */
/* ------------------------------------------------------------------ */

static void vt_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void vt_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { vt_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    vt_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 生命兆候タスク                                                      */
/* ------------------------------------------------------------------ */

void vital_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    if (drpc_my_node == 0xFF) return;

    /* "vital/N" トピックを開く */
    char topic[KDDS_NAME_MAX];
    topic[0] = 'v'; topic[1] = 'i'; topic[2] = 't'; topic[3] = 'a';
    topic[4] = 'l'; topic[5] = '/';
    topic[6] = (char)('0' + drpc_my_node);
    topic[7] = '\0';

    W pub_h = kdds_open(topic, KDDS_QOS_LATEST_ONLY);
    if (pub_h < 0) return;

    vt_puts("[vital] node "); vt_putdec(drpc_my_node);
    vt_puts(" publishing to \""); vt_puts(topic); vt_puts("\"\r\n");

    UW uptime = 0;

    for (;;) {
        tk_dly_tsk(1000);
        uptime++;

        /* ALIVE ピア数をカウント */
        UB alive = 0;
        for (UB n = 0; n < DNODE_MAX; n++)
            if (n != drpc_my_node && dnode_table[n].state == DNODE_ALIVE)
                alive++;

        /* アクティブトピック数をカウント */
        UB active_topics = 0;
        for (W i = 0; i < KDDS_TOPIC_MAX; i++)
            if (kdds_topics[i].open) active_topics++;

        VITAL_DATA vd;
        vd.node_id       = drpc_my_node;
        vd.peers_alive   = alive;
        vd.topics        = active_topics;
        vd._pad          = 0;
        vd.uptime_s      = uptime;
        vd.replica_sent  = replica_stats.sent_pkts;
        vd.replica_recv  = replica_stats.recv_pkts;
        vd.recovered     = replica_stats.recovered;

        kdds_pub(pub_h, &vd, (W)sizeof(vd));
    }
}

/* ------------------------------------------------------------------ */
/* クラスタ健康状態一覧表示                                           */
/* ------------------------------------------------------------------ */

static INT vt_name_eq(const char *a, const char *b, INT n)
{
    for (INT i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void vt_memcpy(void *dst, const void *src, INT n)
{
    const UB *s = (const UB *)src;
    UB       *d = (UB *)dst;
    for (INT i = 0; i < n; i++) d[i] = s[i];
}

void vital_stat(void)
{
    vt_puts("[vital] cluster health snapshot:\r\n");
    vt_puts("  node  uptime   peers  topics  repl_sent  repl_recv  recovered\r\n");
    vt_puts("  ----  -------  -----  ------  ---------  ---------  ---------\r\n");

    INT found = 0;
    char topic[8];

    for (UB n = 0; n < DNODE_MAX; n++) {
        topic[0] = 'v'; topic[1] = 'i'; topic[2] = 't'; topic[3] = 'a';
        topic[4] = 'l'; topic[5] = '/'; topic[6] = (char)('0' + n); topic[7] = '\0';

        for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
            if (!kdds_topics[i].open) continue;
            if (kdds_topics[i].data_len < (UH)sizeof(VITAL_DATA)) continue;
            if (!vt_name_eq(kdds_topics[i].name, topic, 7)) continue;

            VITAL_DATA vd;
            vt_memcpy(&vd, kdds_topics[i].data, sizeof(vd));

            vt_puts("  "); vt_putdec(n);
            vt_puts(n == drpc_my_node ? "* " : "  ");
            vt_puts("   "); vt_putdec(vd.uptime_s); vt_puts("s");
            vt_puts("     "); vt_putdec(vd.peers_alive);
            vt_puts("      "); vt_putdec(vd.topics);
            vt_puts("       "); vt_putdec(vd.replica_sent);
            vt_puts("         "); vt_putdec(vd.replica_recv);
            vt_puts("         "); vt_putdec(vd.recovered);
            vt_puts("\r\n");
            found++;
            break;
        }
    }

    if (!found)
        vt_puts("  (no vital data yet — wait a few seconds)\r\n");
    else {
        vt_puts("  (* = this node)\r\n");
    }
}
