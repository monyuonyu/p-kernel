/*
 *  blk_ssy.h (x86)
 *  Block device subsystem — ssid=2
 *
 *  Provides a name-based device registry with a common ops table.
 *  Drivers (ide, future ahci/virtio) register themselves at init.
 *  Consumers (fat32, future exfat) call blk_ssy_lookup() to get ops.
 */

#pragma once
#include "kernel.h"

/* T-Kernel subsystem ID for block devices */
#define BLK_SSID        2

/* Maximum number of registered block devices */
#define BLK_DEV_MAX     4

/* Device name length (including NUL) */
#define BLK_NAME_LEN    8

/*
 * Block device operations table.
 * Implementors fill this in and pass to blk_ssy_register().
 */
typedef struct {
    const char *name;                               /* "ide0", "ahci0", …  */
    UW          sector_size;                        /* normally 512         */
    INT       (*read)(UW lba, UW n, void *buf);
    INT       (*write)(UW lba, UW n, const void *buf);
    UW        (*sector_count)(void);
    BOOL      (*present)(void);
} BLK_OPS;

/*
 * Register a block device under the given name.
 * Returns 0 on success, -1 if the registry is full or name is duplicate.
 */
INT blk_ssy_register(const BLK_OPS *ops);

/*
 * Look up a previously registered device by name.
 * Returns a pointer to its ops, or NULL if not found.
 */
const BLK_OPS *blk_ssy_lookup(const char *name);

/* Initialise the registry and register the subsystem with tk_def_ssy(). */
void blk_ssy_init(void);
