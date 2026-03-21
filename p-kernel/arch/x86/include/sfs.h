/*
 *  sfs.h (x86)
 *  Shared Folder Sync — 特定フォルダのクラスタ間ファイル同期
 *
 *  "/shared/" 以下のファイルだけを全 ALIVE ノードで複製する。
 *  K-DDS の 128 バイト制限を回避するため、独自 UDP プロトコル (port 7381) を使う。
 *
 *  動作フロー:
 *    書き込み:  vfs_write("/shared/foo.txt") → sfs_push("/shared/foo.txt")
 *               → 全 ALIVE ノードへ 512B チャンク転送 → 受信側がディスクに書く
 *
 *    削除:      vfs_unlink("/shared/foo.txt") → sfs_delete("/shared/foo.txt")
 *               → 全 ALIVE ノードへ DELETE パケット → 受信側もファイルを消す
 *               → tombstone テーブルに記録 (Boot Sync 時の再配布を防ぐ)
 *
 *    起動時:    sfs_boot_sync() → 全ノードへ SYNC_REQ → 受信ノードが全ファイルをプッシュ
 *
 *  制約:
 *    - 1 ファイル最大 SFS_MAX_FILE_SIZE (32 KB)
 *    - 同時受信は 1 ファイルのみ (2 ノード構成では十分)
 *    - SFS_ROOT 以外のパスは自動同期しない
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define SFS_PORT          7381
#define SFS_MAGIC         0x53465348UL    /* "SHFS" LE */
#define SFS_VERSION       1
#define SFS_CHUNK_SIZE    512             /* チャンクサイズ (bytes)      */
#define SFS_MAX_FILE_SIZE (32 * 1024)     /* 1 ファイル最大サイズ (32 KB) */
#define SFS_TOMB_MAX      16              /* tombstone テーブルサイズ    */
#define SFS_PATH_MAX      64              /* ファイルパス最大長           */
#define SFS_ROOT          "/shared"       /* 同期対象フォルダ             */

/* パケットタイプ */
#define SFS_FILE_START    0x01   /* 転送開始 (ファイルメタデータ)        */
#define SFS_FILE_CHUNK    0x02   /* データチャンク                       */
#define SFS_FILE_DELETE   0x03   /* ファイル削除通知 (tombstone)         */
#define SFS_SYNC_REQ      0x04   /* 同期要求 — 持っているファイルを全部送れ */

/* ------------------------------------------------------------------ */
/* パケット構造 (564 bytes, UDP MTU 内)                               */
/* ------------------------------------------------------------------ */

typedef struct {
    UW   magic;                  /* SFS_MAGIC                           */
    UB   version;                /* SFS_VERSION                         */
    UB   type;                   /* SFS_FILE_*                          */
    UB   src_node;
    UB   _pad;
    char path[SFS_PATH_MAX];     /* ファイルパス (null terminated)      */
    UW   total_size;             /* ファイル全体バイト数 (START のみ)   */
    UW   chunk_idx;              /* チャンク番号 (0 始まり)             */
    UH   chunk_len;              /* このチャンクのデータバイト数        */
    UH   _pad2;
    UB   data[SFS_CHUNK_SIZE];   /* チャンクデータ                      */
} __attribute__((packed)) SFS_PKT;
/* 4+1+1+1+1+64+4+4+2+2+512 = 596 bytes */

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    UW files_sent;        /* 送信完了ファイル数 */
    UW files_received;    /* 受信完了ファイル数 */
    UW files_deleted;     /* 削除伝播回数       */
    UW chunks_sent;       /* 送信チャンク数     */
    UW chunks_received;   /* 受信チャンク数     */
    UW errors;            /* エラー数           */
} SFS_STATS;

extern SFS_STATS sfs_stats;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* 初期化 (分散モード検出後に呼ぶ) */
void sfs_init(void);

/* ファイルを全 ALIVE ノードへ送信する。
 * path が SFS_ROOT 以下でない場合は何もしない。 */
void sfs_push(const char *path);

/* ファイルを全 ALIVE ノードで削除する (tombstone 付き)。
 * ローカルファイルは呼び出し前に vfs_unlink() 済みであること。 */
void sfs_delete(const char *path);

/* 起動時同期 — 全 ALIVE ノードへ SYNC_REQ を送り全ファイルを要求する。
 * replica_boot_cry() 相当の "ファイル版 Boot Cry"。 */
void sfs_boot_sync(void);

/* UDP 受信コールバック (SFS_PORT に登録) */
void sfs_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* パスが SFS_ROOT 以下かどうかを確認する */
BOOL sfs_is_shared(const char *path);

/* 統計と tombstone 一覧を表示 */
void sfs_stat(void);

/* /shared/ 以下のファイルを一覧表示 */
void sfs_list(void);
