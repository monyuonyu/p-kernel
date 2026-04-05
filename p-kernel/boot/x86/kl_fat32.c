/*
 * kl_fat32.c — FAT32 disk reader + ELF loader for p-kernel kloader
 *
 * Loads "KERNEL.ELF" from the first FAT32 partition of IDE drive 0.
 * The ELF file is copied to a staging area at STAGING_ADDR (3 MB),
 * then each PT_LOAD segment is placed at its target virtual address.
 *
 * Returns the kernel ELF entry point, or 0 on failure.
 *
 * OS-independent: uses only port I/O and plain C.
 */

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* I/O helpers                                                         */
/* ------------------------------------------------------------------ */

static inline uint8_t _inb(uint16_t p)
{
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"dN"(p)); return v;
}
static inline uint16_t _inw(uint16_t p)
{
    uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"dN"(p)); return v;
}
static inline void _outb(uint16_t p, uint8_t v)
{
    __asm__ volatile("outb %0,%1"::"a"(v),"dN"(p));
}

/* ------------------------------------------------------------------ */
/* Serial (reuse COM1 from kl_main — declare as extern or duplicate)  */
/* ------------------------------------------------------------------ */

#define COM1 0x3F8
static void fat_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            while (!(_inb(COM1+5) & 0x20));
            _outb(COM1, '\r');
        }
        while (!(_inb(COM1+5) & 0x20));
        _outb(COM1, (uint8_t)*s++);
    }
}

/* ------------------------------------------------------------------ */
/* IDE PIO (primary channel, LBA28, drive 0)                          */
/* ------------------------------------------------------------------ */

#define IDE_DATA    0x1F0
#define IDE_ERR     0x1F1
#define IDE_COUNT   0x1F2
#define IDE_LBA0    0x1F3
#define IDE_LBA1    0x1F4
#define IDE_LBA2    0x1F5
#define IDE_SEL     0x1F6
#define IDE_STATUS  0x1F7
#define IDE_CMD     0x1F7
#define IDE_ALT     0x3F6  /* alternate status / device control */

#define IDE_SR_BSY  0x80
#define IDE_SR_DRQ  0x08
#define IDE_SR_ERR  0x01

static void ide_delay(void)
{
    /* 400 ns delay: read ALT STATUS 4 times */
    for (int i = 0; i < 4; i++) _inb(IDE_ALT);
}

/* Returns 0 on success, -1 on error/timeout */
static int ide_read_sector(uint32_t lba, uint8_t *buf)
{
    /* Wait not busy */
    int timeout = 100000;
    while ((_inb(IDE_STATUS) & IDE_SR_BSY) && --timeout);
    if (!timeout) return -1;

    _outb(IDE_SEL,   0xE0 | ((lba >> 24) & 0x0F));  /* drive 0, LBA mode */
    ide_delay();
    _outb(IDE_COUNT, 1);
    _outb(IDE_LBA0,  (uint8_t)(lba));
    _outb(IDE_LBA1,  (uint8_t)(lba >> 8));
    _outb(IDE_LBA2,  (uint8_t)(lba >> 16));
    _outb(IDE_CMD,   0x20);  /* READ SECTORS */

    /* Wait DRQ */
    timeout = 100000;
    uint8_t st;
    do {
        ide_delay();
        st = _inb(IDE_STATUS);
        if (st & IDE_SR_ERR) return -1;
    } while (!(st & IDE_SR_DRQ) && --timeout);
    if (!timeout) return -1;

    /* Read 256 words = 512 bytes */
    for (int i = 0; i < 256; i++) {
        uint16_t w = _inw(IDE_DATA);
        buf[i*2]   = (uint8_t)(w);
        buf[i*2+1] = (uint8_t)(w >> 8);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Simple memory utilities (no libc in kloader)                       */
/* ------------------------------------------------------------------ */

static void kl_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void kl_memset(void *dst, uint8_t v, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

static int kl_memcmp(const void *a, const void *b, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* FAT32 on-disk structures                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;      /* should be 512 */
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entries;          /* 0 for FAT32 */
    uint16_t total_sectors16;
    uint8_t  media;
    uint16_t sectors_per_fat16;     /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    /* FAT32 EBPB */
    uint32_t sectors_per_fat32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;          /* first cluster of root directory */
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
} __attribute__((packed)) BPB32;

/* FAT32 directory entry (short name) */
typedef struct {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t clus_lo;
    uint32_t size;
} __attribute__((packed)) DirEntry;

/* ------------------------------------------------------------------ */
/* ELF32 structures                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) Elf32_Phdr;

#define PT_LOAD  1
#define ELF_MAGIC 0x464C457F  /* "\x7FELF" little-endian */

/* ------------------------------------------------------------------ */
/* Globals (static buffers — avoid large stack allocations)           */
/* ------------------------------------------------------------------ */

#define STAGING_ADDR  0x300000U   /* 3 MB: staging area for kernel ELF */
#define SECTOR_SIZE   512

static uint8_t g_sector[SECTOR_SIZE];   /* general-purpose sector buffer */
static uint8_t g_fat_sector[SECTOR_SIZE]; /* separate buffer for FAT reads */

/* FAT32 geometry filled in by fat32_init() */
static uint32_t g_part_start;
static uint32_t g_fat_start;
static uint32_t g_data_start;
static uint32_t g_spc;           /* sectors per cluster */
static uint32_t g_root_clus;

/* ------------------------------------------------------------------ */
/* FAT32 helpers                                                       */
/* ------------------------------------------------------------------ */

/* Cluster N → first LBA of that cluster */
static uint32_t clus_to_lba(uint32_t clus)
{
    return g_data_start + (clus - 2) * g_spc;
}

/* Read next cluster from FAT.  Returns 0x0FFFFFFF if end-of-chain. */
static uint32_t fat_next_cluster(uint32_t clus)
{
    uint32_t fat_byte = clus * 4;
    uint32_t fat_lba  = g_fat_start + fat_byte / SECTOR_SIZE;
    if (ide_read_sector(fat_lba, g_fat_sector) != 0) return 0x0FFFFFFF;
    uint32_t val = *(uint32_t *)(g_fat_sector + (fat_byte % SECTOR_SIZE));
    return val & 0x0FFFFFFF;
}

/* Parse FAT32 BPB from the partition boot sector.  Returns 1 on success. */
static int fat32_init(uint32_t part_lba)
{
    if (ide_read_sector(part_lba, g_sector) != 0) return 0;

    BPB32 *bpb = (BPB32 *)g_sector;
    if (bpb->bytes_per_sector != 512) return 0;
    if (bpb->sectors_per_fat16 != 0)  return 0;  /* not FAT32 */
    if (bpb->sectors_per_fat32 == 0)  return 0;

    g_part_start = part_lba;
    g_fat_start  = part_lba + bpb->reserved_sectors;
    g_data_start = g_fat_start + (uint32_t)bpb->fat_count * bpb->sectors_per_fat32;
    g_spc        = bpb->sectors_per_cluster;
    g_root_clus  = bpb->root_cluster;
    return 1;
}

/*
 * Read `len` bytes from file starting at `file_offset` into `dest`.
 * `start_clus` is the first cluster of the file.
 */
static int fat32_read_at(uint32_t start_clus,
                         uint32_t file_offset, uint8_t *dest, uint32_t len)
{
    uint32_t cluster_size = g_spc * SECTOR_SIZE;

    /* Skip to the cluster containing file_offset */
    uint32_t clus_idx = file_offset / cluster_size;
    uint32_t intra    = file_offset % cluster_size;
    uint32_t clus = start_clus;
    for (uint32_t i = 0; i < clus_idx; i++) {
        clus = fat_next_cluster(clus);
        if (clus >= 0x0FFFFFF8) return -1;
    }

    uint32_t copied = 0;
    while (copied < len && clus >= 2 && clus < 0x0FFFFFF8) {
        uint32_t lba = clus_to_lba(clus);
        uint32_t sec_off = intra / SECTOR_SIZE;
        uint32_t byte_off = intra % SECTOR_SIZE;

        for (uint32_t s = sec_off; s < g_spc && copied < len; s++) {
            if (ide_read_sector(lba + s, g_sector) != 0) return -1;
            uint32_t avail = SECTOR_SIZE - byte_off;
            uint32_t take  = len - copied;
            if (take > avail) take = avail;
            kl_memcpy(dest + copied, g_sector + byte_off, take);
            copied += take;
            byte_off = 0;
        }
        intra = 0;
        clus = fat_next_cluster(clus);
    }
    return (copied == len) ? 0 : -1;
}

/*
 * Search root directory for an entry matching `name83` (11-byte 8.3).
 * Returns the first cluster of the file, or 0 if not found.
 * *size_out receives the file size.
 */
static uint32_t fat32_find_file(const char *name83, uint32_t *size_out)
{
    uint32_t clus = g_root_clus;

    while (clus >= 2 && clus < 0x0FFFFFF8) {
        uint32_t lba = clus_to_lba(clus);
        for (uint32_t s = 0; s < g_spc; s++) {
            if (ide_read_sector(lba + s, g_sector) != 0) return 0;
            DirEntry *de = (DirEntry *)g_sector;
            for (int e = 0; e < (int)(SECTOR_SIZE / sizeof(DirEntry)); e++) {
                if (de[e].name[0] == 0x00) return 0;  /* end of directory */
                if (de[e].name[0] == 0xE5) continue;  /* deleted */
                if (de[e].attr & 0x08) continue;       /* volume label */
                if (de[e].attr & 0x10) continue;       /* subdirectory */
                /* Compare 8+3 (11 bytes total) */
                uint8_t entry83[11];
                kl_memcpy(entry83,       de[e].name, 8);
                kl_memcpy(entry83 + 8,   de[e].ext,  3);
                if (kl_memcmp(entry83, name83, 11) == 0) {
                    uint32_t first_clus =
                        ((uint32_t)de[e].clus_hi << 16) | de[e].clus_lo;
                    *size_out = de[e].size;
                    return first_clus;
                }
            }
        }
        clus = fat_next_cluster(clus);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ELF loader                                                          */
/* ------------------------------------------------------------------ */

/*
 * Parse and load an ELF32 image that has been staged at `staging`.
 * Copies each PT_LOAD segment to its p_vaddr.
 * Returns the ELF entry point, or 0 on error.
 */
/* Public: also called by kl_net.c after network receive */
uint32_t kl_elf_load(uint32_t staging)
{
    Elf32_Ehdr *eh = (Elf32_Ehdr *)staging;

    /* Validate ELF magic */
    if (*(uint32_t *)eh->e_ident != ELF_MAGIC) {
        fat_puts("[kl_fat32] bad ELF magic\n");
        return 0;
    }
    if (eh->e_type != 2 /* ET_EXEC */ && eh->e_type != 3 /* ET_DYN */) {
        fat_puts("[kl_fat32] not executable ELF\n");
        return 0;
    }

    /* Walk program headers */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)(staging + eh->e_phoff
                                        + (uint32_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_filesz == 0 && ph->p_memsz == 0) continue;

        /* Copy file data to target address */
        if (ph->p_filesz > 0)
            kl_memcpy((void *)ph->p_vaddr,
                      (void *)(staging + ph->p_offset), ph->p_filesz);

        /* Zero-fill BSS region (.bss is p_memsz > p_filesz) */
        if (ph->p_memsz > ph->p_filesz)
            kl_memset((void *)(ph->p_vaddr + ph->p_filesz),
                      0, ph->p_memsz - ph->p_filesz);
    }
    return eh->e_entry;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

/*
 * kl_fat32_load_elf()
 *
 * 1. Detect IDE drive and check for MBR / FAT32 boot sector.
 * 2. Find KERNEL.ELF in the root directory.
 * 3. Load the entire ELF into the staging area (STAGING_ADDR).
 * 4. Parse ELF program headers and copy segments to their target
 *    virtual addresses.
 *
 * Returns the ELF e_entry, or 0 on any failure.
 *
 * Kernel filename in FAT32 8.3 format: "KERNEL  ELF"
 *   (8 chars name, 3 chars ext, space-padded, all uppercase, no dot)
 */
uint32_t kl_fat32_load_elf(void)
{
    /* --- Check drive presence ----------------------------------- */
    _outb(0x1F6, 0xE0);  /* select drive 0, LBA mode */
    for (int i = 0; i < 4; i++) _inb(IDE_ALT);
    if (_inb(IDE_STATUS) == 0xFF) {
        fat_puts("[kl_fat32] no IDE drive\n");
        return 0;
    }

    /* --- Read MBR (LBA 0) --------------------------------------- */
    if (ide_read_sector(0, g_sector) != 0) {
        fat_puts("[kl_fat32] MBR read failed\n");
        return 0;
    }

    /* --- Find FAT32 partition in MBR partition table ------------ */
    uint32_t part_lba = 0;
    if (g_sector[510] == 0x55 && g_sector[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            uint8_t *pe = g_sector + 0x1BE + i * 16;
            uint8_t type = pe[4];
            if (type == 0x0B || type == 0x0C) {     /* FAT32 CHS / LBA */
                part_lba = *(uint32_t *)(pe + 8);
                break;
            }
        }
    }

    /* If no MBR partition found, try sector 0 directly (super-floppy) */
    if (!fat32_init(part_lba)) {
        fat_puts("[kl_fat32] not a FAT32 filesystem\n");
        return 0;
    }
    fat_puts("[kl_fat32] FAT32 found\n");

    /* --- Find KERNEL.ELF --------------------------------------- */
    /* 8.3: "KERNEL  " (8 bytes) + "ELF" (3 bytes), no dot */
    static const char kernel_name83[11] = "KERNEL  ELF";
    uint32_t file_size = 0;
    uint32_t file_clus = fat32_find_file(kernel_name83, &file_size);
    if (!file_clus) {
        fat_puts("[kl_fat32] KERNEL.ELF not found\n");
        return 0;
    }
    fat_puts("[kl_fat32] KERNEL.ELF found\n");

    /* --- Load entire ELF to staging area ----------------------- */
    fat_puts("[kl_fat32] loading to staging area...\n");
    if (fat32_read_at(file_clus, 0, (uint8_t *)STAGING_ADDR, file_size) != 0) {
        fat_puts("[kl_fat32] file read error\n");
        return 0;
    }
    fat_puts("[kl_fat32] load OK, parsing ELF...\n");

    /* --- Parse ELF and copy segments --------------------------- */
    return kl_elf_load(STAGING_ADDR);
}
