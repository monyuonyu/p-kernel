#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

/* Multiboot情報構造体 */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;         // 下位メモリサイズ (KB)
    uint32_t mem_upper;         // 上位メモリサイズ (KB)  
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;       // メモリマップ長
    uint32_t mmap_addr;         // メモリマップアドレス
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint32_t vbe_mode;
    uint32_t vbe_interface_seg;
    uint32_t vbe_interface_off;
    uint32_t vbe_interface_len;
} __attribute__((packed));

/* Multibootメモリマップエントリ */
struct multiboot_mmap_entry {
    uint32_t size;              // エントリサイズ
    uint64_t addr;              // 物理アドレス
    uint64_t len;               // 長さ
    uint32_t type;              // メモリタイプ
} __attribute__((packed));

/* メモリタイプ定義 */
#define MULTIBOOT_MEMORY_AVAILABLE      1
#define MULTIBOOT_MEMORY_RESERVED       2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MEMORY_NVS            4
#define MULTIBOOT_MEMORY_BADRAM         5

/* 物理メモリ領域情報 */
struct memory_region {
    uint64_t base;              // 開始物理アドレス
    uint64_t length;            // 長さ（バイト）
    uint32_t type;              // メモリタイプ
    char description[32];       // 説明
};

/* メモリ管理情報 */
struct memory_info {
    uint32_t total_memory_kb;   // 総メモリサイズ（KB）
    uint32_t available_memory_kb; // 利用可能メモリサイズ（KB）
    uint32_t region_count;      // メモリ領域数
    struct memory_region regions[32]; // メモリ領域（最大32個）
};

/* 外部変数（start.Sで定義） */
extern uint32_t multiboot_magic;
extern uint32_t multiboot_info_ptr;

/* 関数プロトタイプ */
void memory_init(void);
void memory_dump_regions(void);
struct memory_info* get_memory_info(void);
uint64_t get_available_memory_size(void);
uint64_t get_total_memory_size(void);

/* ページ管理定数 */
#define PAGE_SIZE               4096
#define PAGE_ALIGN(addr)        (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(addr)   ((addr) & ~(PAGE_SIZE - 1))

/* メモリ範囲チェック */
#define MEMORY_1MB              0x100000
#define MEMORY_16MB             0x1000000
#define MEMORY_4GB              0x100000000ULL

#endif /* MEMORY_H */