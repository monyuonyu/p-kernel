/*
 * p-kernel libc implementation
 * エラーメッセージ実装 (strerror.c)
 *
 * このファイルはstrerror関数を実装します。
 * エラー番号に対応するエラーメッセージを返します。
 */

#include "string.h"

/* エラーメッセージテーブル */
static char* error_messages[] = {
	"Success",                          /* 0 */
	"Operation not permitted",          /* 1 */
	"No such file or directory",        /* 2 */
	"No such process",                  /* 3 */
	"Interrupted system call",          /* 4 */
	"I/O error",                        /* 5 */
	"No such device or address",        /* 6 */
	"Argument list too long",           /* 7 */
	"Exec format error",                /* 8 */
	"Bad file number",                  /* 9 */
	"No child processes",               /* 10 */
	"Try again",                        /* 11 */
	"Out of memory",                    /* 12 */
	"Permission denied",                /* 13 */
	"Bad address",                      /* 14 */
	"Block device required",            /* 15 */
	"Device or resource busy",          /* 16 */
	"File exists",                      /* 17 */
	"Cross-device link",                /* 18 */
	"No such device",                   /* 19 */
	"Not a directory",                  /* 20 */
	"Is a directory",                   /* 21 */
	"Invalid argument",                 /* 22 */
	"File table overflow",              /* 23 */
	"Too many open files",              /* 24 */
	"Not a typewriter",                 /* 25 */
	"Text file busy",                   /* 26 */
	"File too large",                   /* 27 */
	"No space left on device",          /* 28 */
	"Illegal seek",                     /* 29 */
	"Read-only file system",            /* 30 */
	"Too many links",                   /* 31 */
	"Broken pipe",                      /* 32 */
	"Math argument out of domain of func", /* 33 */
	"Math result not representable"     /* 34 */
};

#define MAX_ERRNO 34  /* 最大エラー番号 */

/**
 * @brief エラー番号に対応するエラーメッセージを取得
 * @param errnum エラー番号
 * @return エラーメッセージ文字列
 * @note 未知のエラー番号の場合は"Unknown error"を返す
 */
char* strerror(int errnum)
{
	/* 有効なエラー番号の範囲チェック */
	if (errnum >= 0 && errnum <= MAX_ERRNO)
		return error_messages[errnum];
	
	return "Unknown error";  /* 未知のエラー番号 */
}