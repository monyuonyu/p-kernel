/*
 * kloader_task.h — p-kernel 側 KLOAD 受信タスク (フェーズ 9)
 *
 * pmesh ポート 7382 で KLOAD パケットを受信し、
 * /KL.BIN として VFS に書き込む。
 * 転送完了後 ACPI リセットで kloader を再起動させる。
 */
#ifndef KLOADER_TASK_H
#define KLOADER_TASK_H

#include "kernel.h"

/* ------------------------------------------------------------------ */
/* KLOAD プロトコル定数                                                */
/* ------------------------------------------------------------------ */
#define KLOAD_PORT      7382    /* KLOAD_CHUNK/START 受信ポート */
#define KLOAD_PORT_BCN  7383    /* KLOAD_BEACON 受信ポート */
#define KLOAD_MAGIC     0x44414F4CUL    /* "LOAD" LE */
#define KLOAD_VERSION   1

#define KLOAD_START     0x01    /* 転送開始 (total_size 通知) */
#define KLOAD_CHUNK     0x02    /* データチャンク */
#define KLOAD_BEACON    0x03    /* bare kloader ノードの自己通知 */

#define KLOAD_CHUNK_SIZE 1024   /* チャンクサイズ (バイト) */

/* ------------------------------------------------------------------ */
/* パケット構造体                                                      */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed)) {
    UW magic;
    UB version;
    UB type;
    UB src_node;
    UB _pad;
    UW total_size;
    UW chunk_idx;
    UH chunk_len;
    UB data[KLOAD_CHUNK_SIZE];
} KLOAD_PKT;

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */
void kloader_task_init(void);
void kloader_task(INT stacd, void *exinf);

/* pmesh_bind コールバック (既存 p-kernel ノードからの KLOAD 受信) */
void kloader_rx(UB src_node, UH dst_port, const UB *data, UH len);

/* udp_bind コールバック (bare kloader ノードからの BEACON 受信) */
void kloader_beacon_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* kpush コマンド用 (shell.c から呼ぶ) */
void kloader_push(UB target_node);

#endif /* KLOADER_TASK_H */
