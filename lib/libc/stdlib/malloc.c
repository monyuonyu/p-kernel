/*
 * p-kernel libc implementation
 * 動的メモリ管理実装 (malloc.c)
 * 
 * このファイルは簡易的なヒープメモリ管理システムを実装します。
 * 固定サイズのヒープ領域を使用し、フリーリストによる管理を行います。
 */

#include "stdlib.h"
#include "string.h"

/* ヒープ領域の設定 */
#define HEAP_SIZE 65536                    /* ヒープサイズ: 64KB */
static char heap[HEAP_SIZE];               /* 静的ヒープ領域 */
static char* heap_ptr = heap;              /* 現在のヒープポインタ */

/* メモリブロック管理構造体 */
typedef struct block {
	size_t size;                           /* ブロックサイズ */
	int free;                              /* 解放済みフラグ（1:解放済み, 0:使用中） */
	struct block* next;                    /* 次のブロックへのポインタ */
} block_t;

static block_t* free_list = NULL;          /* フリーリストのヘッド */

/**
 * @brief 指定サイズ以上の空きブロックを検索
 * @param size 必要なサイズ
 * @return 見つかったブロックのポインタ、見つからない場合はNULL
 */
static block_t* find_free_block(size_t size)
{
	block_t* current = free_list;
	
	/* フリーリストを線形探索 */
	while (current) {
		if (current->free && current->size >= size)
			return current;
		current = current->next;
	}
	
	return NULL;  /* 適切なブロックが見つからない */
}

/**
 * @brief 新しいメモリブロックをヒープから割り当て
 * @param size 必要なサイズ
 * @return 割り当てられたブロックのポインタ、失敗時はNULL
 */
static block_t* allocate_new_block(size_t size)
{
	size_t total_size = sizeof(block_t) + size;
	
	/* ヒープ領域の残り容量チェック */
	if (heap_ptr + total_size > heap + HEAP_SIZE)
		return NULL;  /* ヒープ領域不足 */
	
	/* 新しいブロックを初期化 */
	block_t* block = (block_t*)heap_ptr;
	block->size = size;
	block->free = 0;  /* 使用中としてマーク */
	block->next = free_list;
	free_list = block;  /* フリーリストに追加 */
	
	heap_ptr += total_size;  /* ヒープポインタを進める */
	
	return block;
}

/**
 * @brief 動的メモリ割り当て
 * @param size 割り当てるバイト数
 * @return 割り当てられたメモリのポインタ、失敗時はNULL
 */
void* malloc(size_t size)
{
	if (size == 0)
		return NULL;  /* サイズが0の場合はNULLを返す */
	
	/* 既存の空きブロックを検索 */
	block_t* block = find_free_block(size);
	
	if (block) {
		/* 既存のブロックを再利用 */
		block->free = 0;  /* 使用中としてマーク */
	} else {
		/* 新しいブロックを割り当て */
		block = allocate_new_block(size);
		if (!block)
			return NULL;  /* 割り当て失敗 */
	}
	
	/* ユーザーデータ領域のポインタを返す（ヘッダの後ろ） */
	return (char*)block + sizeof(block_t);
}

void free(void* ptr)
{
	if (!ptr)
		return;
	
	block_t* block = (block_t*)((char*)ptr - sizeof(block_t));
	block->free = 1;
}

void* calloc(size_t nmemb, size_t size)
{
	size_t total_size = nmemb * size;
	void* ptr = malloc(total_size);
	
	if (ptr)
		memset(ptr, 0, total_size);
	
	return ptr;
}

void* realloc(void* ptr, size_t size)
{
	if (!ptr)
		return malloc(size);
	
	if (size == 0) {
		free(ptr);
		return NULL;
	}
	
	block_t* block = (block_t*)((char*)ptr - sizeof(block_t));
	
	if (block->size >= size)
		return ptr;
	
	void* new_ptr = malloc(size);
	if (new_ptr) {
		memcpy(new_ptr, ptr, block->size);
		free(ptr);
	}
	
	return new_ptr;
}