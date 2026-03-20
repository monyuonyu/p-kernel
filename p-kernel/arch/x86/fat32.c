/*
 *  fat32.c (x86)
 *  FAT32 read-only filesystem
 *
 *  Supports:
 *    - FAT32 (BPB type check)
 *    - 8.3 short filenames + Long File Names (LFN, UCS-2 → ASCII)
 *    - Absolute path traversal  ("/bin/hello.elf")
 *    - open / read / seek / close / readdir
 *
 *  Sector I/O via ide.c.
 */

#include "kernel.h"
#include "ide.h"
#include "fat32.h"
#include <tmonitor.h>

/* ----------------------------------------------------------------- */
/* On-disk structures (little-endian)                                */
/* ----------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    UB  jmp_boot[3];
    UB  oem_name[8];
    UH  bytes_per_sector;
    UB  sectors_per_cluster;
    UH  reserved_sectors;
    UB  num_fats;
    UH  root_entry_count;   /* 0 for FAT32 */
    UH  total_sectors16;
    UB  media;
    UH  fat_size16;         /* 0 for FAT32 */
    UH  sectors_per_track;
    UH  num_heads;
    UW  hidden_sectors;
    UW  total_sectors32;
    /* FAT32 extended BPB */
    UW  fat_size32;
    UH  ext_flags;
    UH  fs_version;
    UW  root_cluster;
    UH  fs_info;
    UH  backup_boot_sector;
    UB  reserved[12];
    UB  drive_number;
    UB  reserved1;
    UB  boot_signature;
    UW  volume_id;
    UB  volume_label[11];
    UB  fs_type[8];         /* "FAT32   " */
} BPB;

/* Directory entry (32 bytes) */
typedef struct __attribute__((packed)) {
    UB  name[8];
    UB  ext[3];
    UB  attr;
    UB  nt_res;
    UB  crt_time_tenth;
    UH  crt_time;
    UH  crt_date;
    UH  lst_acc_date;
    UH  fst_clus_hi;
    UH  wrt_time;
    UH  wrt_date;
    UH  fst_clus_lo;
    UW  file_size;
} DIRENTRY;

/* LFN entry (13 UCS-2 chars) */
typedef struct __attribute__((packed)) {
    UB  order;
    UH  name1[5];
    UB  attr;           /* 0x0F */
    UB  type;
    UB  checksum;
    UH  name2[6];
    UH  fst_clus;       /* always 0 */
    UH  name3[2];
} LFNENTRY;

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F

/* ----------------------------------------------------------------- */
/* Filesystem state                                                   */
/* ----------------------------------------------------------------- */

BOOL fat32_mounted = FALSE;

static UW  fat_lba;         /* LBA of first FAT sector           */
static UW  cluster_lba;     /* LBA of cluster 2 (data area)      */
static UW  sectors_per_cluster;
static UW  bytes_per_cluster;
static UW  root_cluster;
static UW  bytes_per_sector; /* always 512 for QEMU               */

/* Single-sector I/O buffer (shared, not re-entrant) */
static UB  sec_buf[IDE_SECTOR_SIZE * 8];  /* up to 8 sectors */

/* ----------------------------------------------------------------- */
/* File descriptor table                                              */
/* ----------------------------------------------------------------- */

typedef struct {
    BOOL  in_use;
    UW    first_cluster;
    UW    file_size;
    UW    offset;           /* current read position             */
    /* Cluster chain walk state */
    UW    cur_cluster;
    UW    cluster_offset;   /* byte offset within current cluster */
} FD;

static FD fds[FAT32_MAX_FD];

/* ----------------------------------------------------------------- */
/* Helpers                                                            */
/* ----------------------------------------------------------------- */

static UW cluster_to_lba(UW cluster)
{
    return cluster_lba + (cluster - 2) * sectors_per_cluster;
}

/* Read FAT entry for given cluster number. */
static UW fat_read(UW cluster)
{
    UW fat_sector = fat_lba + (cluster * 4) / bytes_per_sector;
    UW fat_offset = (cluster * 4) % bytes_per_sector;
    ide_read(fat_sector, 1, sec_buf);
    UW val = *(UW *)(sec_buf + fat_offset);
    return val & 0x0FFFFFFF;
}

static BOOL fat_end(UW cluster)
{
    return cluster >= 0x0FFFFFF8;
}

/* Read one full cluster into buf. */
static void read_cluster(UW cluster, UB *buf)
{
    ide_read(cluster_to_lba(cluster), sectors_per_cluster, buf);
}

/* ----------------------------------------------------------------- */
/* UCS-2 to ASCII (for LFN)                                          */
/* ----------------------------------------------------------------- */

static char ucs2_to_ascii(UH c)
{
    return (c < 0x80) ? (char)c : '?';
}

/* ----------------------------------------------------------------- */
/* 8.3 name formatting                                                */
/* ----------------------------------------------------------------- */

/* Convert raw 8.3 name to "name.ext" (null-terminated). */
static void parse_83(const UB name[8], const UB ext[3], char *out)
{
    INT i = 0, oi = 0;
    for (i = 7; i >= 0 && name[i] == ' '; i--);
    INT name_len = i + 1;
    for (i = 0; i < name_len; i++) {
        char c = (char)name[i];
        out[oi++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    /* Check if extension is non-empty */
    INT ext_len = 2;
    for (; ext_len >= 0 && ext[ext_len] == ' '; ext_len--);
    ext_len++;
    if (ext_len > 0 && ext[0] != ' ') {
        out[oi++] = '.';
        for (i = 0; i < ext_len; i++) {
            char c = (char)ext[i];
            out[oi++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
    }
    out[oi] = '\0';
}

/* ----------------------------------------------------------------- */
/* Mount                                                              */
/* ----------------------------------------------------------------- */

INT fat32_mount(void)
{
    if (!ide_present) return -1;

    ide_read(0, 1, sec_buf);
    BPB *bpb = (BPB *)sec_buf;

    /* Validate FAT32 signature */
    if (bpb->bytes_per_sector != 512 || bpb->num_fats == 0) {
        tm_putstring((UB *)"[fat32] invalid BPB\r\n");
        return -1;
    }
    if (bpb->fat_size32 == 0) {
        tm_putstring((UB *)"[fat32] not FAT32\r\n");
        return -1;
    }

    bytes_per_sector    = bpb->bytes_per_sector;
    sectors_per_cluster = bpb->sectors_per_cluster;
    bytes_per_cluster   = sectors_per_cluster * bytes_per_sector;
    fat_lba             = bpb->hidden_sectors + bpb->reserved_sectors;
    cluster_lba         = fat_lba
                        + (UW)bpb->num_fats * bpb->fat_size32;
    root_cluster        = bpb->root_cluster;

    for (INT i = 0; i < FAT32_MAX_FD; i++) fds[i].in_use = FALSE;

    fat32_mounted = TRUE;
    tm_putstring((UB *)"[fat32] mounted  root_cluster=2\r\n");
    return 0;
}

/* ----------------------------------------------------------------- */
/* Directory traversal                                                */
/* ----------------------------------------------------------------- */

/*
 * Iterate entries in a directory cluster chain.
 * Calls `cb(de, lfn_buf, ud)` for each valid entry.
 * cb returns 0 to continue, non-zero to stop (returned to caller).
 */
typedef INT (*dir_cb)(const DIRENTRY *, const char *, void *);

/* Static cluster buffer for directory reading */
static UB dir_clus_buf[512 * 8];   /* up to 8 sectors/cluster, 4KB */

static INT dir_iterate(UW dir_cluster, dir_cb cb, void *ud)
{
    char lfn[FAT32_MAX_NAME];
    INT  lfn_pos = 0;
    BOOL has_lfn = FALSE;

    UW cluster = dir_cluster;
    while (!fat_end(cluster)) {
        read_cluster(cluster, dir_clus_buf);
        UW entries = bytes_per_cluster / sizeof(DIRENTRY);
        DIRENTRY *de = (DIRENTRY *)dir_clus_buf;

        for (UW i = 0; i < entries; i++, de++) {
            if (de->name[0] == 0x00) return 0;  /* end of directory */
            if (de->name[0] == 0xE5) { has_lfn = FALSE; continue; } /* deleted */
            if ((de->attr & ATTR_VOLUME_ID) && !(de->attr == ATTR_LFN)) {
                has_lfn = FALSE; continue;
            }

            if (de->attr == ATTR_LFN) {
                /* LFN entry */
                LFNENTRY *le = (LFNENTRY *)de;
                INT seq = (le->order & 0x1F);
                if (le->order & 0x40) {
                    /* Last LFN entry (first in sequence) */
                    lfn_pos = seq * 13;
                    lfn[lfn_pos] = '\0';
                    has_lfn = TRUE;
                }
                lfn_pos = (seq - 1) * 13;
                for (INT k = 0; k < 5;  k++) lfn[lfn_pos++] = ucs2_to_ascii(le->name1[k]);
                for (INT k = 0; k < 6;  k++) lfn[lfn_pos++] = ucs2_to_ascii(le->name2[k]);
                for (INT k = 0; k < 2;  k++) lfn[lfn_pos++] = ucs2_to_ascii(le->name3[k]);
                continue;
            }

            /* Real directory entry */
            char fname[FAT32_MAX_NAME];
            if (has_lfn) {
                INT j;
                for (j = 0; lfn[j]; j++) fname[j] = lfn[j];
                fname[j] = '\0';
            } else {
                parse_83(de->name, de->ext, fname);
            }
            has_lfn = FALSE;

            INT r = cb(de, fname, ud);
            if (r != 0) return r;
        }
        cluster = fat_read(cluster);
    }
    return 0;
}

/* ----------------------------------------------------------------- */
/* Path resolver                                                      */
/* ----------------------------------------------------------------- */

typedef struct { const char *target; UW found_cluster; UW found_size; BOOL found_dir; } FindCtx;

static INT find_cb(const DIRENTRY *de, const char *fname, void *ud)
{
    FindCtx *ctx = (FindCtx *)ud;
    /* case-insensitive compare */
    const char *a = ctx->target, *b = fname;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a+32) : *a;
        char cb2 = (*b >= 'A' && *b <= 'Z') ? (char)(*b+32) : *b;
        if (ca != cb2) return 0;
        a++; b++;
    }
    if (*a != '\0' || *b != '\0') return 0;

    ctx->found_cluster = ((UW)de->fst_clus_hi << 16) | de->fst_clus_lo;
    ctx->found_size    = de->file_size;
    ctx->found_dir     = (de->attr & ATTR_DIRECTORY) ? TRUE : FALSE;
    return 1;  /* found */
}

/* Resolve path to (cluster, size, is_dir). Returns 0 on success. */
static INT resolve_path(const char *path, UW *cluster, UW *size, BOOL *is_dir)
{
    if (!fat32_mounted) return -1;

    /* Start from root */
    UW cur = root_cluster;
    *cluster = cur; *size = 0; *is_dir = TRUE;

    if (path[0] == '/') path++;
    if (path[0] == '\0') return 0;  /* root itself */

    char component[FAT32_MAX_NAME];
    while (*path) {
        /* Extract next path component */
        INT ci = 0;
        while (*path && *path != '/') component[ci++] = *path++;
        component[ci] = '\0';
        if (*path == '/') path++;

        FindCtx ctx = { component, 0, 0, FALSE };
        INT r = dir_iterate(cur, find_cb, &ctx);
        if (r == 0) return -1;  /* not found */

        cur = ctx.found_cluster;
        *cluster = cur;
        *size    = ctx.found_size;
        *is_dir  = ctx.found_dir;
    }
    return 0;
}

/* ----------------------------------------------------------------- */
/* open / read / seek / close                                         */
/* ----------------------------------------------------------------- */

INT fat32_open(const char *path)
{
    UW cluster, size; BOOL is_dir;
    if (resolve_path(path, &cluster, &size, &is_dir) < 0) return -1;
    if (is_dir) return -1;

    for (INT i = 0; i < FAT32_MAX_FD; i++) {
        if (!fds[i].in_use) {
            fds[i].in_use        = TRUE;
            fds[i].first_cluster = cluster;
            fds[i].file_size     = size;
            fds[i].offset        = 0;
            fds[i].cur_cluster   = cluster;
            fds[i].cluster_offset = 0;
            return i;
        }
    }
    return -1;  /* no free fd */
}

static UB read_buf[4096];  /* per-read scratch */

INT fat32_read(INT fd, void *buf, UW len)
{
    if (fd < 0 || fd >= FAT32_MAX_FD || !fds[fd].in_use) return -1;
    FD *f = &fds[fd];

    if (f->offset >= f->file_size) return 0;  /* EOF */
    if (len > f->file_size - f->offset) len = f->file_size - f->offset;

    UW remaining = len;
    UB *out = (UB *)buf;

    while (remaining > 0) {
        if (fat_end(f->cur_cluster)) break;

        /* How much is left in the current cluster? */
        UW avail = bytes_per_cluster - f->cluster_offset;
        UW take  = (remaining < avail) ? remaining : avail;

        /* Read the cluster */
        read_cluster(f->cur_cluster, read_buf);
        UB *src = read_buf + f->cluster_offset;
        for (UW i = 0; i < take; i++) out[i] = src[i];

        out              += take;
        remaining        -= take;
        f->offset        += take;
        f->cluster_offset += take;

        if (f->cluster_offset >= bytes_per_cluster) {
            f->cur_cluster    = fat_read(f->cur_cluster);
            f->cluster_offset = 0;
        }
    }
    return (INT)(len - remaining);
}

INT fat32_seek(INT fd, UW offset)
{
    if (fd < 0 || fd >= FAT32_MAX_FD || !fds[fd].in_use) return -1;
    FD *f = &fds[fd];

    /* Walk cluster chain from beginning */
    f->cur_cluster    = f->first_cluster;
    f->cluster_offset = 0;
    f->offset         = 0;

    while (f->offset + bytes_per_cluster <= offset && !fat_end(f->cur_cluster)) {
        f->offset        += bytes_per_cluster;
        f->cur_cluster    = fat_read(f->cur_cluster);
    }
    f->cluster_offset = offset - f->offset;
    f->offset         = offset;
    return 0;
}

UW fat32_fsize(INT fd)
{
    if (fd < 0 || fd >= FAT32_MAX_FD || !fds[fd].in_use) return 0;
    return fds[fd].file_size;
}

void fat32_close(INT fd)
{
    if (fd >= 0 && fd < FAT32_MAX_FD) fds[fd].in_use = FALSE;
}

/* ----------------------------------------------------------------- */
/* readdir                                                            */
/* ----------------------------------------------------------------- */

typedef struct { FAT32_DIRENT *out; INT max; INT count; } ReadDirCtx;

static INT readdir_cb(const DIRENTRY *de, const char *fname, void *ud)
{
    ReadDirCtx *ctx = (ReadDirCtx *)ud;
    if (ctx->count >= ctx->max) return -1;
    if (fname[0] == '.') return 0;  /* skip . and .. */

    FAT32_DIRENT *d = &ctx->out[ctx->count++];
    INT i;
    for (i = 0; fname[i] && i < FAT32_MAX_NAME-1; i++) d->name[i] = fname[i];
    d->name[i] = '\0';
    d->size   = de->file_size;
    d->is_dir = (de->attr & ATTR_DIRECTORY) ? TRUE : FALSE;
    return 0;
}

INT fat32_readdir(const char *path, FAT32_DIRENT *out, INT max)
{
    UW cluster, size; BOOL is_dir;
    if (resolve_path(path, &cluster, &size, &is_dir) < 0) return -1;
    if (!is_dir) return -1;

    ReadDirCtx ctx = { out, max, 0 };
    dir_iterate(cluster, readdir_cb, &ctx);
    return ctx.count;
}
