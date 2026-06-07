/*
 *  persist_demo.c — K-DDS 永続化デモ (Phase 7)
 *
 *  p-kernel の persist.c は K-DDS トピックを FAT32 ディスクに自動保存し、
 *  次回起動時に復元する。このデモはその動作を可視化する。
 *
 *  動作:
 *    1. トピック "user/boot_count" を subscribe して前回値を読む
 *    2. カウンタをインクリメントして publish する
 *    3. 次回起動時には 1 大きい値が読めることを示す
 *
 *  実行方法:
 *    p-kernel shell> spawn persist_demo.elf
 *    → 初回: boot_count = 1 と表示
 *    → 2 回目以降: boot_count が 1 ずつ増える
 *
 *  注意: persist は定期タスク (3 秒ごと) で保存されるため、
 *        spawn 直後に reboot すると保存が間に合わない場合がある。
 */

#include "plibc.h"

#define TOPIC_BOOT "user/boot_count"
#define PERSIST_WAIT_MS  200   /* 前回値が復元されるのを待つ時間 */

typedef struct {
    unsigned int count;
    char  msg[32];
} BootRecord;

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

void _start(void)
{
    puts_s("[persist_demo] K-DDS persistence demo\r\n");

    int h = sys_topic_open(TOPIC_BOOT, 0);
    if (h < 0) {
        puts_s("[persist_demo] ERROR: topic_open failed\r\n");
        sys_exit(1);
    }

    /* 前回の値を読む (persist が復元していれば値が入っている) */
    BootRecord prev;
    prev.count = 0;
    int n = sys_topic_sub(h, &prev, (int)sizeof(prev), PERSIST_WAIT_MS);

    unsigned int new_count = (n == (int)sizeof(prev)) ? prev.count + 1 : 1;

    puts_s("[persist_demo] boot #");
    putu(new_count);
    puts_s("\r\n");

    if (new_count == 1) {
        puts_s("[persist_demo] first boot — no persisted data yet\r\n");
    } else {
        puts_s("[persist_demo] restored from disk: previous boot #");
        putu(prev.count);
        puts_s("\r\n");
    }

    /* 新しい値を publish (persist タスクが ~3 秒後に保存) */
    BootRecord rec;
    rec.count = new_count;
    int i = 0;
    static const char tag[] = "p-kernel boot";
    for (; tag[i] && i < 31; i++) rec.msg[i] = tag[i];
    rec.msg[i] = '\0';

    int r = sys_topic_pub(h, &rec, (int)sizeof(rec));
    if (r == (int)sizeof(rec)) {
        puts_s("[persist_demo] published boot_count=");
        putu(new_count);
        puts_s("  (will be saved to disk in ~3s)\r\n");
    } else {
        puts_s("[persist_demo] pub failed\r\n");
    }

    sys_topic_close(h);
    puts_s("[persist_demo] done\r\n");
    sys_exit(0);
}
