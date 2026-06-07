#include "memory.h"

/* グローバルメモリ情報 */
static struct memory_info mem_info = {0};

/* 外部関数 */
extern void print(const char *str);

/* メモリタイプの説明文字列 */
static const char* memory_type_strings[] = {
    "Unknown",
    "Available",
    "Reserved", 
    "ACPI Reclaimable",
    "ACPI NVS",
    "Bad RAM"
};

/* 簡易文字列コピー */
static void strcpy_simple(char *dest, const char *src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

/* 簡易整数→文字列変換 */
static void uint64_to_hex_str(uint64_t value, char *buffer) {
    const char hex_chars[] = "0123456789ABCDEF";
    buffer[0] = '0';
    buffer[1] = 'x';
    
    for (int i = 15; i >= 0; i--) {
        buffer[17-i] = hex_chars[(value >> (i * 4)) & 0xF];
    }
    buffer[18] = '\0';
}

static void uint32_to_dec_str(uint32_t value, char *buffer) {
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    
    char temp[16];
    int i = 0;
    
    while (value > 0) {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    for (int j = 0; j < i; j++) {
        buffer[j] = temp[i-1-j];
    }
    buffer[i] = '\0';
}

/* メモリ初期化 */
void memory_init(void) {
    struct multiboot_info *mbi;
    struct multiboot_mmap_entry *mmap;
    
    print("Initializing physical memory management...\r\n");
    
    /* Multiboot情報の検証 */
    if (multiboot_magic != 0x2BADB002) {
        print("ERROR: Invalid multiboot magic number!\r\n");
        return;
    }
    
    mbi = (struct multiboot_info *)multiboot_info_ptr;
    
    /* 基本メモリ情報の取得 */
    if (mbi->flags & 0x01) {
        mem_info.total_memory_kb = mbi->mem_lower + mbi->mem_upper;
        print("Basic memory info available\r\n");
    } else {
        print("WARNING: Basic memory info not available\r\n");
    }
    
    /* メモリマップの解析 */
    if (mbi->flags & 0x40) {
        print("Memory map available - parsing regions...\r\n");
        
        mmap = (struct multiboot_mmap_entry *)mbi->mmap_addr;
        uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
        mem_info.region_count = 0;
        mem_info.available_memory_kb = 0;
        
        while ((uint32_t)mmap < mmap_end && mem_info.region_count < 32) {
            struct memory_region *region = &mem_info.regions[mem_info.region_count];
            
            region->base = mmap->addr;
            region->length = mmap->len;
            region->type = mmap->type;
            
            /* 説明文字列の設定 */
            if (mmap->type < 6) {
                strcpy_simple(region->description, memory_type_strings[mmap->type]);
            } else {
                strcpy_simple(region->description, "Unknown");
            }
            
            /* 利用可能メモリの計算 */
            if (mmap->type == MULTIBOOT_MEMORY_AVAILABLE) {
                mem_info.available_memory_kb += (uint32_t)(mmap->len / 1024);
            }
            
            mem_info.region_count++;
            
            /* 次のエントリへ */
            mmap = (struct multiboot_mmap_entry *)((uint32_t)mmap + mmap->size + sizeof(mmap->size));
        }
        
    } else {
        print("WARNING: Memory map not available\r\n");
        /* フォールバック: 基本メモリ情報から推定 */
        if (mbi->flags & 0x01) {
            struct memory_region *region = &mem_info.regions[0];
            region->base = 0;
            region->length = (uint64_t)mbi->mem_lower * 1024;
            region->type = MULTIBOOT_MEMORY_AVAILABLE;
            strcpy_simple(region->description, "Low Memory");
            
            region = &mem_info.regions[1];
            region->base = MEMORY_1MB;
            region->length = (uint64_t)mbi->mem_upper * 1024;
            region->type = MULTIBOOT_MEMORY_AVAILABLE;
            strcpy_simple(region->description, "High Memory");
            
            mem_info.region_count = 2;
            mem_info.available_memory_kb = mbi->mem_lower + mbi->mem_upper;
        }
    }
    
    print("Physical memory initialization complete!\r\n");
}

/* メモリ領域のダンプ表示 */
void memory_dump_regions(void) {
    char buffer[32];
    
    print("\r\n=== Physical Memory Map ===\r\n");
    
    print("Total Memory: ");
    uint32_to_dec_str(mem_info.total_memory_kb, buffer);
    print(buffer);
    print(" KB\r\n");
    
    print("Available Memory: ");
    uint32_to_dec_str(mem_info.available_memory_kb, buffer);
    print(buffer);
    print(" KB\r\n");
    
    print("Memory Regions: ");
    uint32_to_dec_str(mem_info.region_count, buffer);
    print(buffer);
    print("\r\n");
    
    for (uint32_t i = 0; i < mem_info.region_count; i++) {
        struct memory_region *region = &mem_info.regions[i];
        
        print("Region ");
        uint32_to_dec_str(i, buffer);
        print(buffer);
        print(": ");
        
        uint64_to_hex_str(region->base, buffer);
        print(buffer);
        print(" - ");
        
        uint64_to_hex_str(region->base + region->length - 1, buffer);
        print(buffer);
        print(" (");
        
        uint32_to_dec_str((uint32_t)(region->length / 1024), buffer);
        print(buffer);
        print(" KB) - ");
        print(region->description);
        print("\r\n");
    }
    
    print("=== End Memory Map ===\r\n\r\n");
}

/* メモリ情報の取得 */
struct memory_info* get_memory_info(void) {
    return &mem_info;
}

/* 利用可能メモリサイズの取得 */
uint64_t get_available_memory_size(void) {
    return (uint64_t)mem_info.available_memory_kb * 1024;
}

/* 総メモリサイズの取得 */
uint64_t get_total_memory_size(void) {
    return (uint64_t)mem_info.total_memory_kb * 1024;
}