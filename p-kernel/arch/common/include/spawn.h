/*
 *  spawn.h (x86)
 *  Phase 10 — 自己増殖: 新ノード接続時のカーネル自動配布
 *
 *  新しいノードが UNKNOWN → ALIVE に遷移した瞬間、
 *  現在のリーダーが bootloader.bin バイナリを UDP チャンク転送する。
 *  受信ノードはチャンクを FAT32 (/PKNL.BIN) に書き込んで再起動する。
 *
 *  プロトコル (UDP port 7383):
 *    SPAWN_OFFER   Leader → New node: "カーネルを送る準備ができた"
 *    SPAWN_READY   New node → Leader: "受信準備OK"
 *    SPAWN_CHUNK   Leader → New node: バイナリチャンク (512 bytes)
 *    SPAWN_ACK     New node → Leader: チャンク受信確認
 *    SPAWN_DONE    Leader → New node: 転送完了通知
 *
 *  現実装の简略化:
 *    カーネルバイナリをメモリ上に持つ代わりに、
 *    クラスタ状態 (raft log + kdds topic) を新ノードへ push する。
 *    これにより新ノードは即座にクラスタに参加できる。
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define SPAWN_PORT          7383
#define SPAWN_MAGIC         0x4E575053UL   /* "SPWN" LE */
#define SPAWN_VERSION       1
#define SPAWN_CHUNK_SIZE    256            /* チャンクサイズ (bytes) */

/* メッセージタイプ */
#define SPAWN_OFFER         0x01
#define SPAWN_READY         0x02
#define SPAWN_CHUNK         0x03
#define SPAWN_ACK           0x04
#define SPAWN_DONE          0x05
#define SPAWN_STATE_PUSH    0x10   /* クラスタ状態を push (簡易版) */

/* ------------------------------------------------------------------ */
/* パケット構造                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    UW  magic;
    UB  version;
    UB  type;           /* SPAWN_* */
    UB  src_node;
    UB  dst_node;
    UW  total_size;     /* バイナリ総サイズ (OFFER) */
    UW  chunk_idx;      /* チャンク番号 (CHUNK/ACK) */
    UW  chunk_size;     /* 有効データサイズ         */
    UB  raft_term;      /* 現在の任期番号           */
    UB  raft_leader;    /* リーダーノードID         */
    UH  _pad;
    UB  data[SPAWN_CHUNK_SIZE];
} __attribute__((packed)) SPAWN_PKT;

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

void spawn_init(void);
void spawn_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* raft.c のリーダー選出後に呼ぶ: 未参加ノードへ状態を push */
void spawn_on_leader(void);

/* swim.c の新ノード発見時に呼ぶ */
void spawn_on_new_node(UB node_id);

void spawn_stat(void);
