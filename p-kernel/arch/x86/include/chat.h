/*
 *  chat.h (x86)
 *  Phase 11 — AI 会話インターフェース
 *
 *  shell の `chat` コマンドで会話モードに入る。
 *  入力テキスト → 記憶検索 → 文脈生成 → 応答
 *
 *  応答生成の優先順位:
 *    1. claude_bridge.elf (ring-3) が起動中ならクラウド API へ転送
 *    2. ローカル記憶ベース応答 (類似記憶を引用)
 *    3. ルールベース応答 (フォールバック)
 *
 *  会話の流れ:
 *    ユーザー入力 → mem_embed() → mem_search() → 関連記憶表示
 *    → API 転送リクエストを /user/prompt.txt に書き込み
 *    → claude_bridge.elf が /user/response.txt に応答を書く
 *    → 応答を mem_store_add() で記憶に追加
 */

#pragma once
#include "kernel.h"

/* API ブリッジ通信ファイル (ユーザー空間プロセスとの IPC) */
#define CHAT_PROMPT_PATH    "/user/prompt.txt"
#define CHAT_RESPONSE_PATH  "/user/response.txt"
#define CHAT_PERSONA_PATH   "/user/persona.bin"
#define CHAT_API_TMO_MS     10000   /* API 応答タイムアウト (10秒) */

/* 会話セッション */
typedef struct {
    UB   user_id;
    UW   turn_count;
    UB   api_available;   /* claude_bridge.elf が起動中かどうか */
} CHAT_SESSION;

void chat_init(void);

/* shell から呼ぶ: インタラクティブ会話ループ */
void chat_run(UB user_id);

/* 1ターン処理 (テスト・自動化用) */
void chat_turn(CHAT_SESSION *sess, const char *input, char *response_out, INT resp_max);

/* API ブリッジが利用可能か確認 */
UB   chat_api_check(void);

void chat_stat(void);
