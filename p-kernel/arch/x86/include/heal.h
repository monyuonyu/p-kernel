/*
 *  heal.h (x86)
 *  Self-Healing Kernel — フェーズ 3
 *
 *  ノードが DEAD になったとき、生存ノードが自動的に
 *  ガード済みタスクを引き継いで再起動する「自己修復」機能。
 *
 *  動作概要:
 *    1. heal_register() でタスクを「ガード」として登録する
 *       (name, home_node, func_id, priority)
 *    2. swim.c / drpc.c が DEAD 遷移を検出したら heal_on_node_dead() を呼ぶ
 *    3. heal_on_node_dead() は生存ノードで最小 ID を持つ「後継ノード」を選ぶ
 *    4. 後継ノードが自分自身なら drpc_local_restart() でタスクを再起動する
 *    5. heal_triggered[] で drpc + swim 双方からの二重発火を防ぐ
 */

#pragma once
#include "drpc.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define HEAL_GUARD_MAX      8    /* 分散カーネルタスク watchdog 最大数 */
#define HEAL_ELF_GUARD_MAX  4    /* ring-3 ELF デーモン watchdog 最大数 */
#define HEAL_WATCH_INTERVAL 3000 /* ELF 死活チェック間隔 (ms)          */

/* ------------------------------------------------------------------ */
/* カーネルタスク用ガードテーブルエントリ                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char  name[32];      /* 人間可読な識別名 (例: "sensor_pub")      */
    UB    home_node;     /* 元のホームノード ID                      */
    UB    current_node;  /* 現在の実行ノード (引き継ぎ後に更新)      */
    UH    func_id;       /* drpc rfunc ID (0x0001 〜)                */
    W     priority;      /* 再起動タスクの優先度                     */
    UB    active;        /* 1 = 登録済み                             */
} HEAL_GUARD;

/* ------------------------------------------------------------------ */
/* ring-3 ELF デーモン用ガードテーブルエントリ                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char  path[64];      /* ELF パス (例: "/kloader_d.elf")          */
    ID    tid;           /* 現在の task ID (-1 = 未起動)             */
    W     priority;      /* 再起動時のタスク優先度                   */
    UB    active;        /* 1 = 登録済み                             */
    UB    _pad[3];
} HEAL_ELF_GUARD;

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* usermain() 内で drpc_init() の後に呼ぶ。 */
void heal_init(void);

/* ガードを登録する。heal_init() の後に呼ぶ。 */
void heal_register(const char *name, UH func_id, UB home_node, W priority);

/*
 * ノードが DEAD になったとき swim.c / drpc.c から呼ぶ。
 * 自分が後継ノードならガード済みタスクを再起動する。
 * 二重呼び出し防止: heal_triggered[dead_node] フラグで管理。
 */
void heal_on_node_dead(UB dead_node);

/* shell `heal list` から呼ぶ。ガード一覧を表示。 */
void heal_list(void);

/* ------------------------------------------------------------------ */
/* ring-3 ELF デーモン watchdog API                                  */
/* ------------------------------------------------------------------ */

/* vfs_ready 後に一度だけ呼ぶ。ELF ガードテーブルを初期化する。 */
void heal_elf_init(void);

/* ELF デーモンを watchdog に登録する。heal_elf_init() の後に呼ぶ。 */
void heal_elf_register(const char *path, W priority);

/* spawn / re-exec 後に TID を更新する。 */
void heal_elf_update_tid(const char *path, ID tid);

/* watchdog タスク本体。usermain() から create_task() で起動する。 */
void heal_elf_task(INT stacd, void *exinf);
