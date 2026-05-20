/*
 *  dproc.h (x86)
 *  Distributed Process Registry — 分散プロセスレジストリ
 *
 *  ユーザー空間 ELF プロセスのライフサイクルをクラスタ全体で追跡する。
 *  K-DDS トピック "proc/0".."proc/7" に DPROC_ENTRY を pub/sub することで
 *  プロセス状態をリモートノードへ自動伝播する。
 *
 *  プロセス状態マシン:
 *
 *    exec 成功          正常終了               明示的 kill
 *    ────────►  RUNNING  ──────────►  EXITED  ────────────►  KILLED
 *                  │                                              │
 *                  │ ノード死亡 (heal_on_node_dead)               │
 *                  ▼                                             │
 *              後継ノードが re-exec                              再起動しない
 *
 *  重要な設計原則:
 *    - EXITED (正常終了) も KILLED (明示的停止) も 「再起動しない」
 *    - ユーザーが意図的に止めたプロセスはノード死亡でも復活しない
 *    - RUNNING のプロセスだけがフェイルオーバー対象になる
 *    - "proc/N" トピックは persist.c から除外 (起動毎にリセット)
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define DPROC_MAX       8     /* クラスタ全体で追跡できる最大プロセス数 */
#define DPROC_PATH_MAX  28    /* ELF ファイルパスの最大長               */

/* プロセス状態 */
#define DPROC_FREE    0   /* スロット未使用                            */
#define DPROC_RUNNING 1   /* 実行中 — ノード死亡で後継が再起動する     */
#define DPROC_EXITED  2   /* 正常終了 — 再起動しない                   */
#define DPROC_KILLED  3   /* 明示的に停止 — 再起動しない               */

/* K-DDS トピック名プレフィックス */
#define DPROC_TOPIC_PREFIX "proc/"

/* ------------------------------------------------------------------ */
/* エントリ構造 (K-DDS トピックデータとして転送)                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char  path[DPROC_PATH_MAX];  /* ELF パス (例: "/hello.elf")         */
    UW    tid;                   /* T-Kernel タスク ID (node_id 上)     */
    UB    node_id;               /* 実行ノード ID                       */
    UB    state;                 /* DPROC_*                             */
    UH    _pad;
    UW    start_tick;            /* 起動時の uptime tick               */
} __attribute__((packed)) DPROC_ENTRY;   /* 28+4+1+1+2+4 = 40 bytes    */

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* 初期化 (usermain から dtr_init() 後に呼ぶ) */
void dproc_init(void);

/* exec 成功直後に呼ぶ — クラスタへ RUNNING を通知 */
void dproc_register(const char *path, ID tid);

/* ELF 正常終了時に呼ぶ — クラスタへ EXITED を通知 (再起動しない) */
void dproc_exit_by_tid(ID tid);

/* 名前 (パスの末尾部分) または完全パスで RUNNING プロセスを kill する
 * T-Kernel タスクを削除し、クラスタへ KILLED を通知 (再起動しない)
 * 戻り値: 0=成功, -1=見つからない */
W    dproc_kill_by_name(const char *name);

/* ノード死亡時のフェイルオーバー:
 * dead_node で RUNNING だったプロセスを自ノードで再 exec する
 * (heal.c の heal_on_node_dead から呼ぶ) */
void dproc_on_node_dead(UB dead_node);

/* クラスタ全体のプロセス一覧を表示 */
void dproc_list(void);
