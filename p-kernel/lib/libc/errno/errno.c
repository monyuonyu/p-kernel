/*
 * p-kernel libc implementation
 * グローバルエラー番号実装 (errno.c)
 *
 * このファイルはグローバルエラー変数errnoを定義します。
 * システムコールやライブラリ関数のエラー状態を保持します。
 */

#include "errno.h"

/**
 * @brief グローバルエラー変数
 * @note システムコールやライブラリ関数のエラーコードを保持
 * @warning スレッドセーフではないため、マルチスレッド環境では注意が必要
 */
int errno = 0;