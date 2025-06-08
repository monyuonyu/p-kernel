/*
 * p-kernel libc implementation
 * ASCII変換実装 (toascii.c)
 * 
 * このファイルはtoascii関数を実装します。
 * 文字コードをASCII範囲(0-127)に制限します。
 */

#include "ctype.h"

/**
 * @brief ASCII変換
 * @param c 変換する文字
 * @return ASCII範囲に制限された文字
 * @note 上位ビットをマスクしてASCII範囲(0x00-0x7F)に制限
 */
int toascii(int c)
{
	/* 下位7ビットを取り出してASCII範囲に制限 */
	return c & 0x7F;
}