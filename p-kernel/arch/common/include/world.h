/*
 *  world.h
 *  Decentralized whole-network situational-awareness map (regions / Collective)
 *
 *  設計: docs/architecture/survival-network.md (第II部 §6 応援・受援 / §7 分散ゲーティング)
 *        docs/architecture/regions.md
 *
 *  「観測の分散版ゲーティング」。各ノードが自分の状態を周期的に self-beacon として
 *  publish し、受信したビーコンから *自分自身の* 全網ビュー (world-table) を組み立てる。
 *
 *  ── 不変条件 (NO-CENTRAL INVARIANT) ─────────────────────────────────
 *    どのノードも中央集約器ではない。各ノードは受信したゴシップ (K-DDS 経由の
 *    self-beacon) だけから自分の世界像を構築する。したがってどのノードを破壊しても
 *    地図そのものは消えない (§2 守る単位と守る力の分離 / §3 一点突破で殺せない構造)。
 *
 *    実装でこれを保証する仕組み:
 *      - 集約専用ノードや coordinator は一切存在しない。world_task は全ノードで同一に走る。
 *      - ビーコンは moe.c と同じ per-source topic "world/beacon/<node>" に publish される。
 *        単一 LATEST_ONLY スロットへ全員が上書きし合う集約点を作らない。
 *      - world-table は各ノードのローカル配列で、観測した範囲・古さ・欠落をそのまま
 *        持つ (設計の「古さ・不完全さ」の尊重)。完全な真実を持つ単一の場所は存在しない。
 *  ────────────────────────────────────────────────────────────────────
 */

#pragma once
#include "kernel.h"
#include "drpc.h"   /* DNODE_MAX */

/* ------------------------------------------------------------------ */
/* device_type — arch + role を 1 バイトに畳む                         */
/* ------------------------------------------------------------------ */

#define WORLD_DEV_UNKNOWN      0
#define WORLD_DEV_X86_BARE     1   /* boot/x86 ベアメタル                 */
#define WORLD_DEV_AARCH64_BARE 2   /* boot/aarch64 ベアメタル             */
#define WORLD_DEV_LINUX_X86_64 3   /* boot/linux_x86_64 ユーザモード      */
#define WORLD_DEV_LINUX_AARCH64 4  /* boot/linux ユーザモード (aarch64)   */
#define WORLD_DEV_ANDROID_UMP  5   /* UMP APK (Android NDK)               */

/* ------------------------------------------------------------------ */
/* self-beacon パケット (K-DDS 経由で per-source topic へ publish)      */
/*                                                                     */
/* LP64 トラップ対策: T-Kernel の UW/W は環境により long-based となり    */
/* LP64 で 8 バイトへ膨らむ。ワイヤ構造体には固定幅型のみを使い、       */
/* sizeof を _Static_assert で固定する (絶対に UW/W を載せない)。       */
/* ------------------------------------------------------------------ */

/* U1/U2/U4 = 真の固定幅 (typedef.h で unsigned char/short/int に無条件 typedef)。
 * T-Kernel の UB/UH は同じ実体だが、ワイヤ構造体では絶対に UW/W を使わない:
 * UW/W は環境によって long-based となり LP64 で 8 バイトへ膨らむため。 */
typedef struct {
    U1   node_id;       /* 発信ノード ID (0..DNODE_MAX-1)            */
    U1   device_type;   /* WORLD_DEV_*                              */
    U1   region_id;     /* 発信時の自 region ID (0xFF=未確立)       */
    U1   pressure;      /* 逼迫度 0..100 (capacity 余力の逆 — §6)   */
    U1   firing;        /* 発火ビットマスク (gate class ごと, §4)    */
    U1   region_size;   /* 自 region のメンバ数 (観測の補助)        */
    U2   _pad;          /* 4 バイト境界揃え                          */
    U4   seq;           /* 発信ごとに増える単調シーケンス            */
} __attribute__((packed)) WORLD_BEACON;   /* 12 bytes */

_Static_assert(sizeof(WORLD_BEACON) == 12, "WORLD_BEACON must be 12 bytes (LP64-safe wire)");
_Static_assert(sizeof(U1) == 1 && sizeof(U2) == 2 && sizeof(U4) == 4,
               "world wire fields must be true fixed-width");

/* firing ビット: gate class c が直近に発火したら bit c を立てる         */
#define WORLD_FIRE_BIT(c)   ((U1)(1u << (c)))
#define WORLD_FIRE_MASK     0x07   /* 下位 3 bit = MOE_NUM_CLASSES 個     */

/* topic prefix: "world/beacon/" + 1 桁ノード ID                        */
#define WORLD_BEACON_TOPIC_PFX "world/beacon/"

#define WORLD_BEACON_MS   3000   /* self-beacon 発信間隔 (ms)            */
#define WORLD_POLL_MS     250    /* 近隣ビーコン取り込みのポーリング間隔 */

/* この時間 (ms) 受信が途絶えたエントリは stale 表示にする (古さの尊重)  */
#define WORLD_STALE_MS    9000   /* = 3 ビーコン周期                      */

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* usermain() の初期化ブロックで moe_init() の近くで呼ぶ。 */
void world_init(void);

/* self-beacon の発信 + 近隣ビーコンの取り込みを行う常駐タスク (pri 7)。
 * cmd_net から create_task で起動する。全ノードで同一に走る (中央なし)。 */
void world_task(INT stacd, void *exinf);

/* 受信した他ノードのビーコンを world-table に取り込む。
 * world_task のポーリングループから呼ぶ。 */
void world_observe(const WORLD_BEACON *b);

/* この瞬間の自ノードの逼迫度 (0..100) を計算する (degrade/capacity 由来)。
 * firing ビットを MoE 発火が立てるための公開フック。 */
void world_note_firing(UB gate_class);

/* shell `world` / `map` コマンド: 全網の状況をテキストで描画する。
 * 各既知ノードの id / device_type / region / alive・age / pressure bar /
 * firing インジケータを表示する。中央コレクタではなく、このノードが
 * 受信したゴシップから組み立てた *自分の* 世界像である。 */
void world_print(void);
