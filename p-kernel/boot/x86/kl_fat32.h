/*
 * kl_fat32.h — Stage-1 スタンドアロン FAT32 リーダー API
 */
#ifndef KL_FAT32_H
#define KL_FAT32_H

typedef unsigned int u32;

/*
 * ATA PIO 初期化 + FAT32 BPB パース
 * 戻り値: 0=成功, -1=失敗
 */
int kl_fat32_init(void);

/*
 * ルートディレクトリから 8.3 ファイルを検索して dst へ読み込む
 *   name8   : 8バイト (例: "PKNL    ")  スペースパディング, 大文字
 *   ext3    : 3バイト (例: "BIN")
 *   dst     : 読み込み先アドレス
 *   max_size: 最大バイト数
 * 戻り値: ファイルサイズ (バイト), 失敗時 -1
 */
int kl_fat32_load(const char *name8, const char *ext3,
                  void *dst, u32 max_size);

#endif /* KL_FAT32_H */
