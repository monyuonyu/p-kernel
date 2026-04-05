/*
 * kl_fat32.c — Stage-1 スタンドアロン ATA PIO + FAT32 リーダー
 *
 * OS 依存なし。kloader 専用。
 * 機能:
 *   - ATA PIO LBA28 で 512 バイトセクタ読み込み
 *   - FAT32 BPB パース
 *   - ルートクラスタチェーンから 8.3 ファイル名で検索
 *   - ファイル内容を指定アドレスへ読み込み
 */

#include "kl_fat32.h"

/* ------------------------------------------------------------------ */
/* 型定義 (OS ヘッダ不要)                                             */
/* ------------------------------------------------------------------ */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* ------------------------------------------------------------------ */
/* ATA PIO レジスタ                                                   */
/* ------------------------------------------------------------------ */
#define ATA_BASE        0x1F0
#define ATA_DATA        (ATA_BASE + 0)
#define ATA_ERR         (ATA_BASE + 1)
#define ATA_SECT_CNT    (ATA_BASE + 2)
#define ATA_LBA_LO      (ATA_BASE + 3)
#define ATA_LBA_MID     (ATA_BASE + 4)
#define ATA_LBA_HI      (ATA_BASE + 5)
#define ATA_DRIVE       (ATA_BASE + 6)
#define ATA_STATUS      (ATA_BASE + 7)
#define ATA_CMD         (ATA_BASE + 7)

#define ATA_CMD_READ    0x20
#define ATA_SR_BSY      0x80
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

static inline u8 inb(u16 port)
{
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "dN"(port));
    return v;
}

static inline void outb(u16 port, u8 v)
{
    __asm__ volatile("outb %0, %1" :: "a"(v), "dN"(port));
}

static inline u16 inw(u16 port)
{
    u16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "dN"(port));
    return v;
}

/* ATA ステータス待機 */
static int ata_wait(void)
{
    int timeout = 100000;
    while (timeout--) {
        u8 st = inb(ATA_STATUS);
        if (st & ATA_SR_ERR) return -1;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

/* LBA セクタ 1 枚読み込み → 512 バイト buf へ */
static int ata_read_sector(u32 lba, u8 *buf)
{
    /* BSY 解除待ち */
    int t = 100000;
    while ((inb(ATA_STATUS) & ATA_SR_BSY) && t--);

    outb(ATA_DRIVE,    0xE0 | ((lba >> 24) & 0x0F));   /* LBA28 drive0 */
    outb(ATA_SECT_CNT, 1);
    outb(ATA_LBA_LO,   (u8)(lba));
    outb(ATA_LBA_MID,  (u8)(lba >> 8));
    outb(ATA_LBA_HI,   (u8)(lba >> 16));
    outb(ATA_CMD,      ATA_CMD_READ);

    if (ata_wait() < 0) return -1;

    /* 256 ワード = 512 バイト読み込み */
    u16 *w = (u16 *)buf;
    for (int i = 0; i < 256; i++)
        w[i] = inw(ATA_DATA);

    return 0;
}

/* ------------------------------------------------------------------ */
/* FAT32 構造体 (packed)                                               */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed)) {
    u8  jmp[3];
    u8  oem[8];
    u16 bytes_per_sec;
    u8  sec_per_clus;
    u16 rsvd_sec_cnt;
    u8  num_fats;
    u16 root_ent_cnt;   /* FAT32 では 0 */
    u16 tot_sec16;
    u8  media;
    u16 fat_sz16;       /* FAT32 では 0 */
    u16 sec_per_trk;
    u16 num_heads;
    u32 hidd_sec;
    u32 tot_sec32;
    /* FAT32 拡張 */
    u32 fat_sz32;
    u16 ext_flags;
    u16 fs_ver;
    u32 root_clus;
    u16 fs_info;
    u16 bk_boot_sec;
    u8  reserved[12];
    u8  drv_num;
    u8  reserved1;
    u8  boot_sig;
    u32 vol_id;
    u8  vol_lab[11];
    u8  fs_type[8];
} BPB;

typedef struct __attribute__((packed)) {
    u8  name[8];
    u8  ext[3];
    u8  attr;
    u8  nt_res;
    u8  crt_time_tenth;
    u16 crt_time;
    u16 crt_date;
    u16 lst_acc_date;
    u16 fst_clus_hi;
    u16 wrt_time;
    u16 wrt_date;
    u16 fst_clus_lo;
    u32 file_size;
} DIR_ENTRY;

#define ATTR_VOLUME_ID  0x08
#define ATTR_DIR        0x10
#define ATTR_LFN        0x0F

/* ------------------------------------------------------------------ */
/* グローバル FAT32 状態                                               */
/* ------------------------------------------------------------------ */
static BPB  bpb;
static u32  fat_start_lba;
static u32  data_start_lba;
static u32  sec_per_clus;

static u8   sector_buf[512];

/* ------------------------------------------------------------------ */
/* クラスタ → LBA 変換                                                */
/* ------------------------------------------------------------------ */
static u32 clus_to_lba(u32 clus)
{
    return data_start_lba + (clus - 2) * sec_per_clus;
}

/* ------------------------------------------------------------------ */
/* FAT エントリ読み込み (次クラスタ番号)                               */
/* ------------------------------------------------------------------ */
static u32 fat_next(u32 clus)
{
    /* FAT32: 各エントリ 4 バイト */
    u32 fat_offset = clus * 4;
    u32 fat_sec    = fat_start_lba + fat_offset / 512;
    u32 ent_offset = fat_offset % 512;

    if (ata_read_sector(fat_sec, sector_buf) < 0)
        return 0x0FFFFFFF;  /* EOF 扱い */

    u32 val = *(u32 *)(sector_buf + ent_offset);
    return val & 0x0FFFFFFF;
}

/* ------------------------------------------------------------------ */
/* 初期化 — MBR パース → BPB 読み込み                                 */
/* ------------------------------------------------------------------ */
int kl_fat32_init(void)
{
    /* MBR (LBA 0) */
    if (ata_read_sector(0, sector_buf) < 0) return -1;

    /* パーティションテーブル先頭エントリ (offset 0x1BE) */
    u8 *part = sector_buf + 0x1BE;
    u8  type  = part[4];
    u32 start = *(u32 *)(part + 8);

    /* FAT32 タイプチェック (0x0B / 0x0C) */
    if (type != 0x0B && type != 0x0C) {
        /* パーティションなし — LBA 0 がそのまま FAT32 かもしれない */
        start = 0;
    }

    /* BPB 読み込み */
    if (ata_read_sector(start, sector_buf) < 0) return -1;

    /* BPB コピー */
    u8 *bp = (u8 *)&bpb;
    for (int i = 0; i < (int)sizeof(BPB); i++)
        bp[i] = sector_buf[i];

    /* サニティチェック */
    if (bpb.bytes_per_sec != 512) return -1;
    if (bpb.sec_per_clus  == 0)   return -1;

    sec_per_clus   = bpb.sec_per_clus;
    fat_start_lba  = start + bpb.rsvd_sec_cnt;
    data_start_lba = fat_start_lba
                   + (u32)bpb.num_fats * bpb.fat_sz32;

    return 0;
}

/* ------------------------------------------------------------------ */
/* 8.3 名前比較                                                        */
/* de->name[8] + de->ext[3] vs 検索文字列 "NAME    EXT"              */
/* ------------------------------------------------------------------ */
static int match83(const u8 *de_name, const u8 *de_ext,
                   const char *name8, const char *ext3)
{
    for (int i = 0; i < 8; i++)
        if (de_name[i] != (u8)name8[i]) return 0;
    for (int i = 0; i < 3; i++)
        if (de_ext[i] != (u8)ext3[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* ファイル検索 & 読み込み                                             */
/*   name8: 8バイト (スペースパディング済み, 大文字)                   */
/*   ext3:  3バイト (スペースパディング済み, 大文字)                   */
/*   dst:   読み込み先アドレス                                         */
/*   返値:  ファイルサイズ (バイト), 失敗時 -1                         */
/* ------------------------------------------------------------------ */
int kl_fat32_load(const char *name8, const char *ext3,
                  void *dst, u32 max_size)
{
    u32 root_clus = bpb.root_clus;
    u32 clus      = root_clus;

    u8 *out = (u8 *)dst;
    u32 written = 0;

    /* ルートクラスタチェーンを辿ってファイルを探す */
    while (clus < 0x0FFFFFF8U) {
        u32 lba = clus_to_lba(clus);

        for (u32 s = 0; s < sec_per_clus; s++) {
            if (ata_read_sector(lba + s, sector_buf) < 0) return -1;

            DIR_ENTRY *de = (DIR_ENTRY *)sector_buf;
            for (int i = 0; i < 16; i++, de++) {
                if (de->name[0] == 0x00) goto not_found; /* 終端 */
                if (de->name[0] == 0xE5) continue;       /* 削除済み */
                if (de->attr == ATTR_LFN) continue;       /* LFN */
                if (de->attr & ATTR_VOLUME_ID) continue;
                if (de->attr & ATTR_DIR) continue;

                if (!match83(de->name, de->ext, name8, ext3)) continue;

                /* 発見 — ファイルデータ読み込み */
                u32 fclus = ((u32)de->fst_clus_hi << 16) | de->fst_clus_lo;
                u32 fsize = de->file_size;
                if (fsize > max_size) fsize = max_size;

                u32 remain = fsize;
                while (fclus < 0x0FFFFFF8U && remain > 0) {
                    u32 flba = clus_to_lba(fclus);
                    for (u32 fs = 0; fs < sec_per_clus && remain > 0; fs++) {
                        if (ata_read_sector(flba + fs, sector_buf) < 0) return -1;
                        u32 chunk = (remain < 512) ? remain : 512;
                        u8 *sb = sector_buf;
                        for (u32 b = 0; b < chunk; b++)
                            out[written++] = sb[b];
                        remain -= chunk;
                    }
                    fclus = fat_next(fclus);
                }
                return (int)fsize;
            }
        }
        clus = fat_next(clus);
    }

not_found:
    return -1;
}
