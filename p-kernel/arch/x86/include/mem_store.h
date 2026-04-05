/*
 *  mem_store.h (x86)
 *  Phase 11 — 記憶永続化 (Memory Store)
 *
 *  人類の営みの記録を永遠に残すための基盤。
 *  会話・イベント・センサーデータを埋め込みベクトルとともに保存し、
 *  文脈から類似記憶を想起する。
 *
 *  記憶の構造:
 *    テキスト → 簡易 embedding (文字統計 → float[8])
 *    → FAT32 /mem/YYYYMMDD.mem に永続化
 *    → SFS で全ノードに複製
 *    → Raft リーダーが整合性を保証
 *
 *  ユーザー個人情報:
 *    /user/<user_id>/persona.bin に保存 (ring-3 からのみアクセス)
 *    カーネルは embedding のみ参照し、生テキストはユーザー空間に置く
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define MEM_EMBED_DIM     8      /* 埋め込みベクトル次元              */
#define MEM_TEXT_MAX      120    /* 記憶テキスト最大長                */
#define MEM_RING_SIZE     64     /* メモリ上リングバッファ容量        */
#define MEM_USER_MAX      8      /* 最大ユーザー数                    */

/* 記憶タイプ */
#define MEM_TYPE_TEXT     0x01   /* テキスト会話                     */
#define MEM_TYPE_SENSOR   0x02   /* センサーデータ記録               */
#define MEM_TYPE_EVENT    0x03   /* システムイベント                 */
#define MEM_TYPE_SUMMARY  0x04   /* AIが生成した要約                 */

/* ------------------------------------------------------------------ */
/* 記憶エントリ (148 bytes)                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    UW    timestamp;                  /* Unix 相当時刻 (起動からの秒) */
    UB    user_id;                    /* ユーザーID (0 = system)      */
    UB    type;                       /* MEM_TYPE_*                   */
    UH    seq;                        /* シーケンス番号               */
    char  text[MEM_TEXT_MAX];         /* テキスト本文                 */
    float embedding[MEM_EMBED_DIM];   /* 埋め込みベクトル (検索用)    */
} __attribute__((packed)) MEM_ENTRY;  /* 148 bytes */

/* ------------------------------------------------------------------ */
/* ユーザーペルソナ (ring-3 から書く、カーネルは読むのみ)            */
/* ------------------------------------------------------------------ */

typedef struct {
    UB    user_id;
    UB    _pad[3];
    char  name[32];                   /* 名前                          */
    char  summary[128];               /* AIが生成した記憶サマリー      */
    float pref_vec[MEM_EMBED_DIM];    /* 好み/傾向ベクトル             */
    UW    mem_count;                  /* 保存された記憶の総数          */
    UW    last_seen;                  /* 最終アクティブ時刻            */
} PERSONA;                            /* 188 bytes */

/* ------------------------------------------------------------------ */
/* 検索結果                                                            */
/* ------------------------------------------------------------------ */

#define MEM_SEARCH_TOP    4      /* 類似記憶の最大返却数             */

typedef struct {
    MEM_ENTRY *entry;
    float      score;            /* コサイン類似度 (0.0 - 1.0)      */
} MEM_MATCH;

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

void mem_store_init(void);

/* 記憶を追加する。embedding は自動生成。 */
ER   mem_store_add(UB user_id, UB type, const char *text);

/* テキストを埋め込みベクトルに変換 (bag-of-chars統計) */
void mem_embed(const char *text, float out[MEM_EMBED_DIM]);

/* 類似記憶を検索 (コサイン類似度 Top-N) */
INT  mem_search(const float query_emb[MEM_EMBED_DIM], UB user_id,
                MEM_MATCH results[MEM_SEARCH_TOP]);

/* 最新N件の記憶を取得 */
INT  mem_recent(UB user_id, INT n, MEM_ENTRY *out);

/* ペルソナ操作 */
ER   mem_persona_set(const PERSONA *p);
ER   mem_persona_get(UB user_id, PERSONA *out);

/* FAT32 への永続化 */
ER   mem_persist(void);
ER   mem_restore(void);

/* 統計表示 */
void mem_stat(void);
