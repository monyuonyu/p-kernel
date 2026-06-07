/*
 *  persist.h (x86)
 *  Phase 7 — Flash/Disk Persistence for K-DDS Topics
 *
 *  目標: 全ノードが同時に電源を失っても K-DDS トピックが消えない。
 *
 *  仕組み:
 *    - persist_task() が 30 秒ごとに全 open トピックを FAT32 に書き込む
 *    - 起動直後 (ネットワーク前) に persist_restore_all() がディスクから復元
 *    - シェルから手動操作も可能 (persist save / list / clear)
 *
 *  ファイル形式:
 *    ファイルパス: /kd_<name_escaped>.dat
 *    name_escaped: トピック名の '/' を '_' に置換
 *    例:  "sensor/watchdog" → /kd_sensor_watchdog.dat
 *
 *  レコード形式 (PERSIST_RECORD, 168 bytes):
 *    magic    4B   PERSIST_MAGIC
 *    version  1B   PERSIST_VERSION
 *    qos      1B   KDDS_QOS_*
 *    data_len 2B   有効データバイト数
 *    name    32B   トピック名 (null terminated)
 *    data   128B   最新データ
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define PERSIST_MAGIC      0x544B4450UL   /* "PDKT" LE */
#define PERSIST_VERSION    1
#define PERSIST_INTERVAL_S 30             /* 自動保存間隔 (秒) */

/* ------------------------------------------------------------------ */
/* レコード構造                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    UW   magic;           /* PERSIST_MAGIC                     */
    UB   version;         /* PERSIST_VERSION                   */
    UB   qos;             /* KDDS_QOS_*                        */
    UH   data_len;        /* data[] の有効バイト数             */
    char name[32];        /* トピック名 (null terminated)      */
    UB   data[128];       /* 最新データ                        */
} __attribute__((packed)) PERSIST_RECORD;   /* 168 bytes */

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    UW saved;       /* 保存したトピック数 (累計)  */
    UW restored;    /* 復元したトピック数         */
    UW checkpoints; /* チェックポイント実行回数   */
    UW errors;      /* 書き込みエラー数           */
} PERSIST_STATS;

extern PERSIST_STATS persist_stats;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* 全 open トピックをディスクに保存する (ネットワーク前後どちらからも呼べる) */
void persist_checkpoint(void);

/* 起動時: ディスクから kdds_topics[] へ復元 (ネットワーク前に呼ぶ) */
void persist_restore_all(void);

/* 定期保存タスク (30 秒ごとに persist_checkpoint を呼ぶ) */
void persist_task(INT stacd, void *exinf);

/* シェル: ディスク上の保存済みトピックを一覧表示 */
void persist_list(void);

/* シェル: 保存済みトピックを全削除 */
void persist_clear(void);
