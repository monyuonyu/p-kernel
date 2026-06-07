/*
 *  cluster_stat.c — クラスタ状態モニタ (ring-3 デーモン)
 *
 *  K-DDS トピックを subscribe してクラスタ全体の状態を表示する。
 *
 *  購読トピック:
 *    "vital/0"   — ノード 0 の Vital Signs (生存監視)
 *    "moe/score" — MoE エキスパートスコア (各クラスの推論精度)
 *    "proc/0"    — プロセス統計 (タスク 0 のリソース使用量)
 *
 *  表示内容:
 *    - 各ノードの生存状態と負荷
 *    - MoE スコア (クラスごとの推論精度)
 *    - プロセス統計
 *
 *  実行方法:
 *    p-kernel shell> spawn cluster_stat.elf
 *    → クラスタ状態を 3 回サンプリングして終了
 *
 *    連続モニタ:
 *    p-kernel shell> spawn cluster_stat.elf loop
 */

#include "plibc.h"

/* ------------------------------------------------------------------ */
/* Vital Signs パケット (vital.c と同レイアウト)                       */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned int  magic;        /* 0x4C544956 "VITL" */
    unsigned char node_id;
    unsigned char load_pct;     /* CPU 負荷 (0-100%) */
    unsigned char mem_pct;      /* メモリ使用率 (0-100%) */
    unsigned char status;       /* 0=OK 1=WARN 2=CRIT */
    unsigned int  uptime_sec;
    unsigned int  infer_total;  /* 推論実行総数 */
    unsigned int  infer_err;    /* 推論エラー数 */
} VitalPkt;

/* ------------------------------------------------------------------ */
/* MoE スコアパケット (moe.h と同レイアウト)                           */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned char node_id;
    unsigned char accuracy[3];   /* [0]=normal [1]=alert [2]=critical */
    unsigned int  total_infer;
    unsigned int  correct[3];
} MoeScore;

/* ------------------------------------------------------------------ */
/* プロセス統計パケット (dproc.c と同レイアウト)                       */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned int  magic;        /* 0x434F5250 "PROC" */
    unsigned char slot;
    unsigned char node_id;
    char          path[32];
    unsigned int  tid;
    unsigned int  run_count;
    unsigned int  err_count;
} ProcStat;

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */
static void puts_s(const char *s)
{
    sys_write(1, s, plib_strlen(s));
}

static void putu(unsigned int v)
{
    char buf[12]; int i = 11; buf[i] = '\0';
    if (v == 0) { puts_s("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    puts_s(&buf[i]);
}

static void put_bar(unsigned int pct)
{
    /* ████░░░░ 形式のバー (10段階) */
    static const char filled[] = "#";
    static const char empty[]  = ".";
    unsigned int blocks = pct / 10;
    for (unsigned int i = 0; i < 10; i++)
        puts_s(i < blocks ? filled : empty);
}

/* ------------------------------------------------------------------ */
/* 各トピックの表示                                                    */
/* ------------------------------------------------------------------ */
static void show_vital(int h_vital)
{
    VitalPkt v;
    int n = sys_topic_sub(h_vital, &v, (int)sizeof(v), 500);
    if (n != (int)sizeof(v)) {
        puts_s("  vital/0    : (no data)\r\n");
        return;
    }
    puts_s("  vital/0    : node="); putu(v.node_id);
    puts_s("  load=["); put_bar(v.load_pct); puts_s("] ");
    putu(v.load_pct); puts_s("%");
    puts_s("  mem=[");  put_bar(v.mem_pct);  puts_s("] ");
    putu(v.mem_pct);  puts_s("%");
    puts_s("  status=");
    if      (v.status == 0) puts_s("OK");
    else if (v.status == 1) puts_s("WARN");
    else                    puts_s("CRIT");
    puts_s("  infer="); putu(v.infer_total);
    puts_s("/err=");    putu(v.infer_err);
    puts_s("\r\n");
}

static void show_moe(int h_moe)
{
    MoeScore m;
    int n = sys_topic_sub(h_moe, &m, (int)sizeof(m), 500);
    if (n != (int)sizeof(m)) {
        puts_s("  moe/score  : (no data)\r\n");
        return;
    }
    puts_s("  moe/score  : node="); putu(m.node_id);
    puts_s("  acc=[normal=");   putu(m.accuracy[0]); puts_s("%");
    puts_s(" alert=");          putu(m.accuracy[1]); puts_s("%");
    puts_s(" critical=");       putu(m.accuracy[2]); puts_s("%]");
    puts_s("  total=");         putu(m.total_infer);
    puts_s("\r\n");
}

static void show_proc(int h_proc)
{
    ProcStat p;
    int n = sys_topic_sub(h_proc, &p, (int)sizeof(p), 200);
    if (n != (int)sizeof(p)) {
        puts_s("  proc/0     : (no data)\r\n");
        return;
    }
    puts_s("  proc/0     : path="); puts_s(p.path);
    puts_s("  run="); putu(p.run_count);
    puts_s("  err="); putu(p.err_count);
    puts_s("\r\n");
}

/* ------------------------------------------------------------------ */
/* メイン                                                              */
/* ------------------------------------------------------------------ */
void _start(void)
{
    puts_s("[cluster_stat] p-kernel cluster state monitor\r\n");

    int h_vital = sys_topic_open("vital/0",   0);
    int h_moe   = sys_topic_open("moe/score", 0);
    int h_proc  = sys_topic_open("proc/0",    0);

    if (h_vital < 0 || h_moe < 0 || h_proc < 0) {
        puts_s("[cluster_stat] ERROR: topic_open failed\r\n");
        sys_exit(1);
    }

    int rounds = 3;

    for (int i = 0; i < rounds; i++) {
        puts_s("\r\n--- cluster stat [");
        putu((unsigned int)(i + 1));
        puts_s("/");
        putu((unsigned int)rounds);
        puts_s("] ---\r\n");

        show_vital(h_vital);
        show_moe(h_moe);
        show_proc(h_proc);

        if (i + 1 < rounds)
            tk_dly_tsk(2000);   /* 2 秒待って再サンプリング */
    }

    sys_topic_close(h_vital);
    sys_topic_close(h_moe);
    sys_topic_close(h_proc);

    puts_s("[cluster_stat] done\r\n");
    sys_exit(0);
}
