/*
 *  fat32.c (x86)
 *  FAT32 filesystem (read + write)
 *
 *  Supports:
 *    - FAT32 (BPB type check)
 *    - 8.3 short filenames + Long File Names (LFN, UCS-2 → ASCII)
 *    - Absolute path traversal  ("/bin/hello.elf")
 *    - open / read / seek / close / readdir
 *    - create / write / unlink / mkdir / rename
 *
 *  Sector I/O via ide.c.
 */

#include "kernel.h"
#include "blk_ssy.h"
#include "fat32.h"
#include <tmonitor.h>

/* Block device used by this filesystem instance */
static const BLK_OPS *g_blk = NULL;

/* Called from vfs.c before fat32_mount() */
void fat32_set_blkdev(const BLK_OPS *ops) { g_blk = ops; }

/* Convenience macros so the rest of the code stays readable */
#define blk_read(lba, n, buf)   (g_blk->read((lba), (n), (buf)))
#define blk_write(lba, n, buf)  (g_blk->write((lba), (n), (buf)))
#define blk_present()           (g_blk != NULL && g_blk->present())

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
static UW  fat_size32;      /* sectors per FAT (for FAT2 update) */
static UW  num_fats;        /* number of FAT copies (usually 2)  */
static UW  cluster_lba;     /* LBA of cluster 2 (data area)      */
static UW  sectors_per_cluster;
static UW  bytes_per_cluster;
static UW  root_cluster;
static UW  bytes_per_sector; /* always 512 for QEMU               */

/* I/O buffers (not re-entrant — single-threaded use only) */
static UB  sec_buf[512 * 8];    /* FAT / scratch         */
static UB  dir_sec_buf[512];    /* dir entry read/write  */
static UB  clus_rw_buf[512 * 8];/* data cluster R-M-W    */

/* ----------------------------------------------------------------- */
/* File descriptor table                                              */
/* ----------------------------------------------------------------- */

typedef struct {
    BOOL  in_use;
    UW    first_cluster;
    UW    file_size;
    UW    offset;           /* current read/write position       */
    /* Cluster chain walk state */
    UW    cur_cluster;
    UW    cluster_offset;   /* byte offset within current cluster */
    /* Write support */
    BOOL  writable;
    UW    tail_cluster;     /* last cluster in chain (for append)*/
    UW    dir_lba;          /* LBA of sector holding dir entry   */
    UW    dir_off;          /* byte offset of DIRENTRY in sector */
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
    blk_read(fat_sector, 1, sec_buf);
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
    blk_read(cluster_to_lba(cluster), sectors_per_cluster, buf);
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
    if (!blk_present()) return -1;

    blk_read(0, 1, sec_buf);
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
    fat_size32          = bpb->fat_size32;
    num_fats            = bpb->num_fats;
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

static INT dir_iterate(UW dir_cluster, dir_cb cb, void *ud)
{
    char lfn[FAT32_MAX_NAME];
    INT  lfn_pos = 0;
    BOOL has_lfn = FALSE;

    UW cluster = dir_cluster;
    while (!fat_end(cluster)) {
        read_cluster(cluster, clus_rw_buf);
        UW entries = bytes_per_cluster / sizeof(DIRENTRY);
        DIRENTRY *de = (DIRENTRY *)clus_rw_buf;

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
        read_cluster(f->cur_cluster, clus_rw_buf);
        UB *src = clus_rw_buf + f->cluster_offset;
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
    if (fd < 0 || fd >= FAT32_MAX_FD || !fds[fd].in_use) return;
    FD *f = &fds[fd];

    if (f->writable) {
        /* Flush file size and first cluster back to directory entry */
        blk_read(f->dir_lba, 1, dir_sec_buf);
        DIRENTRY *de = (DIRENTRY *)(dir_sec_buf + f->dir_off);
        de->file_size   = f->file_size;
        de->fst_clus_lo = (UH)(f->first_cluster & 0xFFFF);
        de->fst_clus_hi = (UH)((f->first_cluster >> 16) & 0xFFFF);
        blk_write(f->dir_lba, 1, dir_sec_buf);
    }
    f->in_use = FALSE;
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

/* ----------------------------------------------------------------- */
/* FAT write helpers                                                  */
/* ----------------------------------------------------------------- */

/* Write a single FAT entry (updates all FAT copies). */
static void fat_set(UW cluster, UW value)
{
    UW fat_sector = fat_lba + (cluster * 4) / bytes_per_sector;
    UW fat_offset = (cluster * 4) % bytes_per_sector;
    blk_read(fat_sector, 1, sec_buf);
    UW *entry = (UW *)(sec_buf + fat_offset);
    *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);
    blk_write(fat_sector, 1, sec_buf);
    if (num_fats > 1)
        blk_write(fat_sector + fat_size32, 1, sec_buf);
}

/* Scan FAT for a free cluster, mark EOC, return cluster number.
 * Returns 0 if disk is full. */
static UW fat_alloc(void)
{
    UW entries_per_sector = bytes_per_sector / 4;
    for (UW sect = 0; sect < fat_size32; sect++) {
        blk_read(fat_lba + sect, 1, sec_buf);
        UW *entries = (UW *)sec_buf;
        for (UW i = 0; i < entries_per_sector; i++) {
            UW cluster = sect * entries_per_sector + i;
            if (cluster < 2) continue;
            if ((entries[i] & 0x0FFFFFFF) == 0) {
                entries[i] = (entries[i] & 0xF0000000) | 0x0FFFFFFF;
                blk_write(fat_lba + sect, 1, sec_buf);
                if (num_fats > 1)
                    blk_write(fat_lba + fat_size32 + sect, 1, sec_buf);
                return cluster;
            }
        }
    }
    return 0;  /* disk full */
}

/* Free entire cluster chain starting at `cluster`. */
static void fat_free_chain(UW cluster)
{
    while (!fat_end(cluster) && cluster >= 2) {
        UW next = fat_read(cluster);
        fat_set(cluster, 0);
        cluster = next;
    }
}

/* Write one full cluster to disk. */
static void write_cluster(UW cluster, const UB *buf)
{
    blk_write(cluster_to_lba(cluster), sectors_per_cluster, buf);
}

/* ----------------------------------------------------------------- */
/* 8.3 name helpers                                                   */
/* ----------------------------------------------------------------- */

/* Convert filename to FAT 8.3 (padded spaces, uppercase). */
static void make_83(const char *name, UB out_name[8], UB out_ext[3])
{
    for (INT i = 0; i < 8; i++) out_name[i] = ' ';
    for (INT i = 0; i < 3; i++) out_ext[i]  = ' ';

    INT dot = -1;
    for (INT i = 0; name[i]; i++) if (name[i] == '.') dot = i;

    INT ni = 0;
    for (INT i = 0; name[i] && (dot < 0 || i < dot) && ni < 8; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out_name[ni++] = (UB)c;
    }
    if (dot >= 0) {
        INT ei = 0;
        for (INT i = dot + 1; name[i] && ei < 3; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out_ext[ei++] = (UB)c;
        }
    }
}

/* Split "/path/to/file" into parent="/path/to" and fname="file". */
static void split_path(const char *path, char *parent, char *fname)
{
    INT last_slash = -1;
    for (INT i = 0; path[i]; i++) if (path[i] == '/') last_slash = i;

    if (last_slash <= 0) {
        parent[0] = '/'; parent[1] = '\0';
        INT i = (last_slash == 0) ? 1 : 0, j = 0;
        while (path[i]) fname[j++] = path[i++];
        fname[j] = '\0';
    } else {
        for (INT i = 0; i < last_slash; i++) parent[i] = path[i];
        parent[last_slash] = '\0';
        INT j = 0;
        for (INT i = last_slash + 1; path[i]; i++) fname[j++] = path[i];
        fname[j] = '\0';
    }
}

/* ----------------------------------------------------------------- */
/* Directory entry location scanner                                   */
/* ----------------------------------------------------------------- */

/*
 * Scan directory for a named entry (name != NULL) or a free slot
 * (name == NULL).  Reads sector-by-sector for precise LBA tracking.
 * Returns 0 on success, -1 on failure.
 * On success sets *out_lba/*out_off and optionally fills *out_de.
 * Free-slot search extends the directory with a new cluster if needed.
 */
static INT dir_scan(UW dir_cluster, const char *name,
                    UW *out_lba, UW *out_off, DIRENTRY *out_de)
{
    char lfn[FAT32_MAX_NAME];
    INT  lfn_pos = 0;
    BOOL has_lfn = FALSE;
    UW   last_cluster = dir_cluster;
    UW   entries_per_sector = bytes_per_sector / sizeof(DIRENTRY);

    UW cluster = dir_cluster;
    while (!fat_end(cluster)) {
        last_cluster = cluster;
        UW base_lba = cluster_to_lba(cluster);

        for (UW sect = 0; sect < sectors_per_cluster; sect++) {
            UW lba = base_lba + sect;
            blk_read(lba, 1, dir_sec_buf);
            DIRENTRY *de = (DIRENTRY *)dir_sec_buf;

            for (UW i = 0; i < entries_per_sector; i++, de++) {
                if (name == NULL) {
                    if (de->name[0] == 0x00 || de->name[0] == 0xE5) {
                        *out_lba = lba;
                        *out_off = i * sizeof(DIRENTRY);
                        if (out_de) *out_de = *de;
                        return 0;
                    }
                    continue;
                }
                if (de->name[0] == 0x00) return -1;
                if (de->name[0] == 0xE5) { has_lfn = FALSE; continue; }
                if ((de->attr & ATTR_VOLUME_ID) &&
                    !(de->attr == ATTR_LFN)) { has_lfn = FALSE; continue; }

                if (de->attr == ATTR_LFN) {
                    LFNENTRY *le = (LFNENTRY *)de;
                    INT seq = (le->order & 0x1F);
                    if (le->order & 0x40) {
                        lfn_pos = seq * 13;
                        lfn[lfn_pos] = '\0';
                        has_lfn = TRUE;
                    }
                    lfn_pos = (seq - 1) * 13;
                    for (INT k = 0; k < 5; k++)
                        lfn[lfn_pos++] = ucs2_to_ascii(le->name1[k]);
                    for (INT k = 0; k < 6; k++)
                        lfn[lfn_pos++] = ucs2_to_ascii(le->name2[k]);
                    for (INT k = 0; k < 2; k++)
                        lfn[lfn_pos++] = ucs2_to_ascii(le->name3[k]);
                    continue;
                }

                char fname_buf[FAT32_MAX_NAME];
                if (has_lfn) {
                    INT j;
                    for (j = 0; lfn[j]; j++) fname_buf[j] = lfn[j];
                    fname_buf[j] = '\0';
                } else {
                    parse_83(de->name, de->ext, fname_buf);
                }
                has_lfn = FALSE;

                const char *a = name, *b = fname_buf;
                while (*a && *b) {
                    char ca  = (*a>='A'&&*a<='Z')?(char)(*a+32):*a;
                    char cb2 = (*b>='A'&&*b<='Z')?(char)(*b+32):*b;
                    if (ca != cb2) goto next_de;
                    a++; b++;
                }
                if (*a == '\0' && *b == '\0') {
                    *out_lba = lba;
                    *out_off = i * sizeof(DIRENTRY);
                    if (out_de) *out_de = *de;
                    return 0;
                }
                next_de:;
            }
        }
        cluster = fat_read(cluster);
    }

    /* Free-slot search: extend directory with a new cluster */
    if (name == NULL) {
        UW new_clus = fat_alloc();
        if (new_clus == 0) return -1;
        fat_set(last_cluster, new_clus);
        fat_set(new_clus, 0x0FFFFFFF);
        for (INT k = 0; k < 512; k++) dir_sec_buf[k] = 0;
        for (UW s = 0; s < sectors_per_cluster; s++)
            blk_write(cluster_to_lba(new_clus) + s, 1, dir_sec_buf);
        *out_lba = cluster_to_lba(new_clus);
        *out_off = 0;
        if (out_de) {
            for (INT k = 0; k < (INT)sizeof(DIRENTRY); k++)
                ((UB *)out_de)[k] = 0;
        }
        return 0;
    }
    return -1;
}

/* ----------------------------------------------------------------- */
/* fat32_create_fd — create or truncate a file, return writable fd   */
/* ----------------------------------------------------------------- */

INT fat32_create_fd(const char *path)
{
    if (!fat32_mounted) return -1;

    char parent_path[FAT32_MAX_PATH];
    char fname[FAT32_MAX_NAME];
    split_path(path, parent_path, fname);
    if (fname[0] == '\0') return -1;

    UW dir_cluster, dir_size; BOOL dir_is_dir;
    if (resolve_path(parent_path, &dir_cluster, &dir_size, &dir_is_dir) < 0)
        return -1;
    if (!dir_is_dir) return -1;

    UW entry_lba, entry_off;
    DIRENTRY existing_de;

    if (dir_scan(dir_cluster, fname, &entry_lba, &entry_off, &existing_de) == 0) {
        /* File exists: free old chain, reset size/cluster */
        UW old_clus = ((UW)existing_de.fst_clus_hi << 16) |
                       existing_de.fst_clus_lo;
        if (old_clus >= 2) fat_free_chain(old_clus);
        blk_read(entry_lba, 1, dir_sec_buf);
        DIRENTRY *de = (DIRENTRY *)(dir_sec_buf + entry_off);
        de->file_size   = 0;
        de->fst_clus_lo = 0;
        de->fst_clus_hi = 0;
        blk_write(entry_lba, 1, dir_sec_buf);
    } else {
        /* New file: find free dir slot */
        if (dir_scan(dir_cluster, NULL, &entry_lba, &entry_off, NULL) < 0)
            return -1;
        blk_read(entry_lba, 1, dir_sec_buf);
        DIRENTRY *de = (DIRENTRY *)(dir_sec_buf + entry_off);
        for (INT k = 0; k < (INT)sizeof(DIRENTRY); k++) ((UB *)de)[k] = 0;
        make_83(fname, de->name, de->ext);
        de->attr     = ATTR_ARCHIVE;
        de->wrt_date = 0x4A21;
        de->crt_date = 0x4A21;
        blk_write(entry_lba, 1, dir_sec_buf);
    }

    /* Allocate first cluster */
    UW first_clus = fat_alloc();
    if (first_clus == 0) return -1;
    for (UW k = 0; k < bytes_per_cluster; k++) clus_rw_buf[k] = 0;
    write_cluster(first_clus, clus_rw_buf);

    /* Update dir entry with first cluster */
    blk_read(entry_lba, 1, dir_sec_buf);
    DIRENTRY *de = (DIRENTRY *)(dir_sec_buf + entry_off);
    de->fst_clus_lo = (UH)(first_clus & 0xFFFF);
    de->fst_clus_hi = (UH)((first_clus >> 16) & 0xFFFF);
    blk_write(entry_lba, 1, dir_sec_buf);

    /* Open writable fd */
    for (INT i = 0; i < FAT32_MAX_FD; i++) {
        if (!fds[i].in_use) {
            fds[i].in_use         = TRUE;
            fds[i].first_cluster  = first_clus;
            fds[i].file_size      = 0;
            fds[i].offset         = 0;
            fds[i].cur_cluster    = first_clus;
            fds[i].cluster_offset = 0;
            fds[i].writable       = TRUE;
            fds[i].tail_cluster   = first_clus;
            fds[i].dir_lba        = entry_lba;
            fds[i].dir_off        = entry_off;
            return i;
        }
    }
    return -1;
}

/* ----------------------------------------------------------------- */
/* fat32_write                                                        */
/* ----------------------------------------------------------------- */

INT fat32_write(INT fd, const void *buf, UW len)
{
    if (fd < 0 || fd >= FAT32_MAX_FD || !fds[fd].in_use) return -1;
    FD *f = &fds[fd];
    if (!f->writable) return -1;
    if (len == 0) return 0;

    const UB *in = (const UB *)buf;
    UW remaining = len;

    while (remaining > 0) {
        if (fat_end(f->cur_cluster)) {
            UW new_clus = fat_alloc();
            if (new_clus == 0) break;
            fat_set(f->tail_cluster, new_clus);
            fat_set(new_clus, 0x0FFFFFFF);
            for (UW k = 0; k < bytes_per_cluster; k++) clus_rw_buf[k] = 0;
            write_cluster(new_clus, clus_rw_buf);
            f->cur_cluster    = new_clus;
            f->tail_cluster   = new_clus;
            f->cluster_offset = 0;
        }

        UW avail = bytes_per_cluster - f->cluster_offset;
        UW take  = (remaining < avail) ? remaining : avail;

        read_cluster(f->cur_cluster, clus_rw_buf);
        for (UW k = 0; k < take; k++)
            clus_rw_buf[f->cluster_offset + k] = in[k];
        write_cluster(f->cur_cluster, clus_rw_buf);

        in                += take;
        remaining         -= take;
        f->offset         += take;
        f->cluster_offset += take;
        if (f->offset > f->file_size) f->file_size = f->offset;

        if (f->cluster_offset >= bytes_per_cluster) {
            UW next = fat_read(f->cur_cluster);
            if (fat_end(next)) f->tail_cluster = f->cur_cluster;
            f->cur_cluster    = next;
            f->cluster_offset = 0;
        }
    }
    return (INT)(len - remaining);
}

/* ----------------------------------------------------------------- */
/* fat32_unlink                                                       */
/* ----------------------------------------------------------------- */

INT fat32_unlink(const char *path)
{
    if (!fat32_mounted) return -1;

    char parent_path[FAT32_MAX_PATH];
    char fname[FAT32_MAX_NAME];
    split_path(path, parent_path, fname);

    UW dir_cluster, dir_size; BOOL dir_is_dir;
    if (resolve_path(parent_path, &dir_cluster, &dir_size, &dir_is_dir) < 0)
        return -1;
    if (!dir_is_dir) return -1;

    UW entry_lba, entry_off;
    DIRENTRY de;
    if (dir_scan(dir_cluster, fname, &entry_lba, &entry_off, &de) < 0)
        return -1;
    if (de.attr & ATTR_DIRECTORY) return -1;

    UW first_clus = ((UW)de.fst_clus_hi << 16) | de.fst_clus_lo;
    if (first_clus >= 2) fat_free_chain(first_clus);

    blk_read(entry_lba, 1, dir_sec_buf);
    dir_sec_buf[entry_off] = 0xE5;
    blk_write(entry_lba, 1, dir_sec_buf);
    return 0;
}

/* ----------------------------------------------------------------- */
/* fat32_mkdir                                                        */
/* ----------------------------------------------------------------- */

INT fat32_mkdir(const char *path)
{
    if (!fat32_mounted) return -1;

    char parent_path[FAT32_MAX_PATH];
    char dname[FAT32_MAX_NAME];
    split_path(path, parent_path, dname);
    if (dname[0] == '\0') return -1;

    UW dir_cluster, dir_size; BOOL dir_is_dir;
    if (resolve_path(parent_path, &dir_cluster, &dir_size, &dir_is_dir) < 0)
        return -1;
    if (!dir_is_dir) return -1;

    UW tmp_lba, tmp_off;
    if (dir_scan(dir_cluster, dname, &tmp_lba, &tmp_off, NULL) == 0)
        return -1;  /* already exists */

    UW new_clus = fat_alloc();
    if (new_clus == 0) return -1;

    /* Build "." and ".." entries in new cluster */
    for (UW k = 0; k < bytes_per_cluster; k++) clus_rw_buf[k] = 0;
    DIRENTRY *dot  = (DIRENTRY *)clus_rw_buf;
    DIRENTRY *dot2 = dot + 1;

    for (INT k = 0; k < 8; k++) dot->name[k]  = ' ';
    dot->name[0] = '.';
    for (INT k = 0; k < 3; k++) dot->ext[k]   = ' ';
    dot->attr = ATTR_DIRECTORY; dot->wrt_date = 0x4A21;
    dot->fst_clus_lo = (UH)(new_clus & 0xFFFF);
    dot->fst_clus_hi = (UH)((new_clus >> 16) & 0xFFFF);

    for (INT k = 0; k < 8; k++) dot2->name[k] = ' ';
    dot2->name[0] = '.'; dot2->name[1] = '.';
    for (INT k = 0; k < 3; k++) dot2->ext[k]  = ' ';
    dot2->attr = ATTR_DIRECTORY; dot2->wrt_date = 0x4A21;
    dot2->fst_clus_lo = (UH)(dir_cluster & 0xFFFF);
    dot2->fst_clus_hi = (UH)((dir_cluster >> 16) & 0xFFFF);

    write_cluster(new_clus, clus_rw_buf);

    UW entry_lba, entry_off;
    if (dir_scan(dir_cluster, NULL, &entry_lba, &entry_off, NULL) < 0) {
        fat_set(new_clus, 0);
        return -1;
    }
    blk_read(entry_lba, 1, dir_sec_buf);
    DIRENTRY *de = (DIRENTRY *)(dir_sec_buf + entry_off);
    for (INT k = 0; k < (INT)sizeof(DIRENTRY); k++) ((UB *)de)[k] = 0;
    make_83(dname, de->name, de->ext);
    de->attr        = ATTR_DIRECTORY;
    de->wrt_date    = 0x4A21;
    de->crt_date    = 0x4A21;
    de->fst_clus_lo = (UH)(new_clus & 0xFFFF);
    de->fst_clus_hi = (UH)((new_clus >> 16) & 0xFFFF);
    blk_write(entry_lba, 1, dir_sec_buf);
    return 0;
}

/* ----------------------------------------------------------------- */
/* fat32_rename                                                       */
/* ----------------------------------------------------------------- */

INT fat32_rename(const char *oldpath, const char *newpath)
{
    if (!fat32_mounted) return -1;

    char old_parent[FAT32_MAX_PATH], old_name[FAT32_MAX_NAME];
    char new_parent[FAT32_MAX_PATH], new_name[FAT32_MAX_NAME];
    split_path(oldpath, old_parent, old_name);
    split_path(newpath, new_parent, new_name);

    UW old_dir, od_sz; BOOL od_isdir;
    UW new_dir, nd_sz; BOOL nd_isdir;
    if (resolve_path(old_parent, &old_dir, &od_sz, &od_isdir) < 0) return -1;
    if (resolve_path(new_parent, &new_dir, &nd_sz, &nd_isdir) < 0) return -1;
    if (!od_isdir || !nd_isdir) return -1;

    UW src_lba, src_off;
    DIRENTRY src_de;
    if (dir_scan(old_dir, old_name, &src_lba, &src_off, &src_de) < 0)
        return -1;

    /* Overwrite destination if it exists */
    UW dst_lba, dst_off;
    DIRENTRY dst_de;
    if (dir_scan(new_dir, new_name, &dst_lba, &dst_off, &dst_de) == 0) {
        UW dst_clus = ((UW)dst_de.fst_clus_hi << 16) | dst_de.fst_clus_lo;
        if (dst_clus >= 2) fat_free_chain(dst_clus);
        blk_read(dst_lba, 1, dir_sec_buf);
        dir_sec_buf[dst_off] = 0xE5;
        blk_write(dst_lba, 1, dir_sec_buf);
    }

    UW new_entry_lba, new_entry_off;
    if (dir_scan(new_dir, NULL, &new_entry_lba, &new_entry_off, NULL) < 0)
        return -1;
    blk_read(new_entry_lba, 1, dir_sec_buf);
    DIRENTRY *nde = (DIRENTRY *)(dir_sec_buf + new_entry_off);
    *nde = src_de;
    make_83(new_name, nde->name, nde->ext);
    blk_write(new_entry_lba, 1, dir_sec_buf);

    blk_read(src_lba, 1, dir_sec_buf);
    dir_sec_buf[src_off] = 0xE5;
    blk_write(src_lba, 1, dir_sec_buf);
    return 0;
}
