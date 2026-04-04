/*
 *  raft.h (x86)
 *  Phase 10 — Raft コンセンサスアルゴリズム
 *
 *  Leader election + heartbeat + simplified log replication.
 *  Transport: UDP port 7382
 *
 *  Roles:
 *    FOLLOWER  — 初期状態。election timeout でCandidateに昇格
 *    CANDIDATE — 選挙中。全生存ノードに RequestVote を送信
 *    LEADER    — 過半数取得後。全ノードに AppendEntries(heartbeat)を送信
 *
 *  Term:
 *    任期番号。リーダー選出のたびにインクリメント。
 *    古い任期のメッセージは無視する。
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define RAFT_PORT              7382

#define RAFT_MAGIC             0x54464152UL   /* "RAFT" LE */
#define RAFT_VERSION           1

/* メッセージタイプ */
#define RAFT_MSG_VOTE_REQ      0x01   /* RequestVote         */
#define RAFT_MSG_VOTE_REP      0x02   /* RequestVote 返答    */
#define RAFT_MSG_APPEND        0x03   /* AppendEntries (HB)  */
#define RAFT_MSG_APPEND_REP    0x04   /* AppendEntries 返答  */

/* ロール */
#define RAFT_FOLLOWER          0
#define RAFT_CANDIDATE         1
#define RAFT_LEADER            2

/* タイムアウト */
#define RAFT_HB_INTERVAL_MS    100    /* leader heartbeat 間隔 */
#define RAFT_TICK_MS           50     /* raft task tick       */
#define RAFT_ELECT_TMO_MIN     6      /* election timeout min tick数 (300ms) */
#define RAFT_ELECT_TMO_RANGE   6      /* + node_id×50ms でランダム化       */

/* ログ */
#define RAFT_LOG_MAX           16     /* ログエントリ最大数   */
#define RAFT_KEY_MAX           8      /* キー最大長           */
#define RAFT_VAL_MAX           8      /* 値最大長             */

/* ------------------------------------------------------------------ */
/* パケット構造 (40 bytes)                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    UW  magic;          /* RAFT_MAGIC                              */
    UB  version;        /* RAFT_VERSION                            */
    UB  type;           /* RAFT_MSG_*                              */
    UB  src_node;
    UB  candidate;      /* 選挙候補ノードID (VOTE_REQ)             */
    UW  term;           /* 現在の任期番号                          */
    UW  log_idx;        /* candidateの最終ログインデックス         */
    UW  log_term;       /* candidateの最終ログ任期                 */
    UW  commit_idx;     /* リーダーのコミット済みインデックス      */
    UB  granted;        /* 投票承認フラグ (VOTE_REP)               */
    UB  success;        /* AppendEntries 成功フラグ                */
    UB  entry_key;      /* ログキー (APPEND)                      */
    UB  entry_val;      /* ログ値  (APPEND, 単純化)               */
    UW  entry_idx;      /* ログインデックス (APPEND)              */
    UW  entry_term;     /* ログ任期 (APPEND)                      */
} __attribute__((packed)) RAFT_PKT;   /* 32 bytes */

/* ------------------------------------------------------------------ */
/* ログエントリ                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    UW  term;
    UB  key;
    UB  val;
} RAFT_LOG_ENTRY;

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

void raft_init(void);
void raft_task(INT stacd, void *exinf);
void raft_rx(UW src_ip, UH src_port, const UB *data, UH len);

UB   raft_leader(void);     /* 現在のリーダーノードID (0xFF=不明) */
UB   raft_role(void);       /* RAFT_FOLLOWER / CANDIDATE / LEADER */
UW   raft_term(void);       /* 現在の任期番号                    */
void raft_stat(void);       /* shell `raft stat` 用表示          */

/* クラスタ設定をログへ書き込む (リーダーのみ有効) */
ER   raft_write(UB key, UB val);
