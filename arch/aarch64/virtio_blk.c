/*
 *  virtio_blk.c (aarch64)
 *  Minimal polled virtio-blk over the virtio-mmio transport (QEMU virt).
 *  See virtio_blk.h for the design summary.
 *
 *  References: VIRTIO 1.1 spec §4.2 (MMIO transport), §5.2 (block device),
 *  §2.6 (split virtqueues). Legacy (Version 1) layout follows the legacy
 *  interface in §4.2.4 / the OASIS legacy appendix.
 */

#include "virtio_blk.h"
#include "kernel.h"
#include "mmio.h"

/* ------------------------------------------------------------------ */
/* virtio-mmio transport: QEMU virt geometry                           */
/* ------------------------------------------------------------------ */
#define VIRTIO_MMIO_BASE    0x0a000000UL
#define VIRTIO_MMIO_STRIDE  0x200UL
#define VIRTIO_MMIO_SLOTS   32

#define VIRTIO_MAGIC        0x74726976u   /* "virt" */
#define VIRTIO_ID_BLOCK     2u

/* MMIO register offsets */
#define R_MagicValue        0x000
#define R_Version           0x004
#define R_DeviceID          0x008
#define R_VendorID          0x00c
#define R_DeviceFeatures    0x010
#define R_DeviceFeaturesSel 0x014
#define R_DriverFeatures    0x020
#define R_DriverFeaturesSel 0x024
#define R_GuestPageSize     0x028   /* legacy only */
#define R_QueueSel          0x030
#define R_QueueNumMax       0x034
#define R_QueueNum          0x038
#define R_QueueAlign        0x03c   /* legacy only */
#define R_QueuePFN          0x040   /* legacy only */
#define R_QueueReady        0x044   /* modern only */
#define R_QueueNotify       0x050
#define R_InterruptStatus   0x060
#define R_InterruptACK      0x064
#define R_Status            0x070
#define R_QueueDescLow      0x080   /* modern */
#define R_QueueDescHigh     0x084
#define R_QueueAvailLow     0x090   /* modern */
#define R_QueueAvailHigh    0x094
#define R_QueueUsedLow      0x0a0   /* modern */
#define R_QueueUsedHigh     0x0a4
#define R_Config            0x100   /* device-specific config space */

/* Status bits */
#define S_ACKNOWLEDGE       1u
#define S_DRIVER            2u
#define S_DRIVER_OK         4u
#define S_FEATURES_OK       8u
#define S_FAILED            128u

/* Feature bits */
#define VIRTIO_BLK_F_FLUSH  9u
#define VIRTIO_F_VERSION_1  32u      /* bit 0 of feature word 1 */

/* Block request types */
#define VIRTIO_BLK_T_IN     0u       /* read  */
#define VIRTIO_BLK_T_OUT    1u       /* write */
#define VIRTIO_BLK_T_FLUSH  4u

#define VIRTIO_BLK_S_OK     0u

/* Descriptor flags */
#define VRING_DESC_F_NEXT   1u
#define VRING_DESC_F_WRITE  2u       /* device writes (i.e. read into mem) */

/* Page size for legacy queue layout. */
#define VQ_PAGE_SIZE        4096u

/* Queue depth we request (small — single in-flight req, 3 descriptors). */
#define VQ_SIZE             8

/* ------------------------------------------------------------------ */
/* Split-virtqueue structures                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    U4 addr_lo;
    U4 addr_hi;
    U4 len;
    U2 flags;
    U2 next;
} __attribute__((packed)) vring_desc;

typedef struct {
    U2 flags;
    U2 idx;
    U2 ring[VQ_SIZE];
    U2 used_event;
} __attribute__((packed)) vring_avail;

typedef struct {
    U4 id;
    U4 len;
} __attribute__((packed)) vring_used_elem;

typedef struct {
    U2 flags;
    U2 idx;
    vring_used_elem ring[VQ_SIZE];
    U2 avail_event;
} __attribute__((packed)) vring_used;

/* virtio_blk request header (16 bytes on the wire). */
typedef struct {
    U4 type;
    U4 reserved;
    U4 sector_lo;
    U4 sector_hi;
} __attribute__((packed)) virtio_blk_req_hdr;

/* ------------------------------------------------------------------ */
/* Static queue memory.                                                */
/* One 2-page region: desc + avail in page 0, used at page 1 so the     */
/* legacy single-PFN/QueueAlign=PAGE layout is satisfied; the modern    */
/* path gives the three sub-addresses explicitly. Page-aligned for both.*/
/* ------------------------------------------------------------------ */
static U1 vq_mem[2 * VQ_PAGE_SIZE] __attribute__((aligned(VQ_PAGE_SIZE)));

static vring_desc  *vq_desc;
static vring_avail *vq_avail;
static vring_used  *vq_used;

static virtio_blk_req_hdr vq_hdr __attribute__((aligned(16)));
static volatile U1        vq_status __attribute__((aligned(16)));

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */
static unsigned long vb_base   = 0;     /* selected transport MMIO base */
static UW            vb_sectors = 0;     /* capacity in 512-byte sectors */
static U2            vb_qsize  = 0;
static U2            vb_last_used = 0;
static INT           vb_has_flush = 0;
static INT           vb_ready  = 0;

/* ------------------------------------------------------------------ */
/* MMIO accessors                                                      */
/* ------------------------------------------------------------------ */
static inline UW   rd(UW off)         { return mmio_read32(vb_base + off); }
static inline void wr(UW off, UW val) { mmio_write32(vb_base + off, val); }

/* ------------------------------------------------------------------ */
/* Debug emit (PL011 direct, like boot/aarch64/main.c). Kept tiny so it */
/* works before T-Kernel. Override target stays QEMU virt PL011.        */
/* ------------------------------------------------------------------ */
#ifdef BOARD_RPI3
#  define VB_UART_BASE 0x3F201000UL
#else
#  define VB_UART_BASE 0x09000000UL
#endif
static void vb_puts(const char *s)
{
    volatile unsigned int *dr = (volatile unsigned int *)(VB_UART_BASE + 0x00);
    volatile unsigned int *fr = (volatile unsigned int *)(VB_UART_BASE + 0x18);
    for (; *s; s++) {
        while (*fr & (1 << 5)) {}
        *dr = (unsigned int)(unsigned char)*s;
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

static INT vb_find_device(void)
{
    for (INT i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        unsigned long base = VIRTIO_MMIO_BASE + (unsigned long)i * VIRTIO_MMIO_STRIDE;
        if (mmio_read32(base + R_MagicValue) != VIRTIO_MAGIC) continue;
        UW ver = mmio_read32(base + R_Version);
        if (ver != 1 && ver != 2) continue;
        if (mmio_read32(base + R_DeviceID) != VIRTIO_ID_BLOCK) continue;
        vb_base = base;
        return (INT)ver;
    }
    return -1;
}

INT virtio_blk_init(void)
{
    if (vb_ready) return 0;

    INT ver = vb_find_device();
    if (ver < 0) {
        vb_puts("[vblk] no virtio-blk-device on the MMIO bus\r\n");
        return -1;
    }

    /* --- reset + ACKNOWLEDGE + DRIVER --- */
    wr(R_Status, 0);
    MMIO_DSB();
    UW status = S_ACKNOWLEDGE;
    wr(R_Status, status);
    status |= S_DRIVER;
    wr(R_Status, status);

    /* --- feature negotiation --- */
    wr(R_DeviceFeaturesSel, 0);
    UW dev_feat_lo = rd(R_DeviceFeatures);
    wr(R_DeviceFeaturesSel, 1);
    UW dev_feat_hi = rd(R_DeviceFeatures);

    UW drv_feat_lo = 0, drv_feat_hi = 0;
    if (dev_feat_lo & (1u << VIRTIO_BLK_F_FLUSH)) {
        drv_feat_lo |= (1u << VIRTIO_BLK_F_FLUSH);
        vb_has_flush = 1;
    }
    if (ver == 2) {
        /* VERSION_1 (bit 32) is mandatory for the modern interface. */
        if (dev_feat_hi & (1u << (VIRTIO_F_VERSION_1 - 32)))
            drv_feat_hi |= (1u << (VIRTIO_F_VERSION_1 - 32));
    }

    wr(R_DriverFeaturesSel, 0);
    wr(R_DriverFeatures, drv_feat_lo);
    wr(R_DriverFeaturesSel, 1);
    wr(R_DriverFeatures, drv_feat_hi);

    if (ver == 2) {
        status |= S_FEATURES_OK;
        wr(R_Status, status);
        MMIO_DSB();
        if (!(rd(R_Status) & S_FEATURES_OK)) {
            vb_puts("[vblk] FEATURES_OK rejected by device\r\n");
            wr(R_Status, S_FAILED);
            return -1;
        }
    } else {
        /* legacy: driver declares the guest page size for PFN math */
        wr(R_GuestPageSize, VQ_PAGE_SIZE);
    }

    /* --- queue 0 setup --- */
    wr(R_QueueSel, 0);
    UW qmax = rd(R_QueueNumMax);
    if (qmax == 0) {
        vb_puts("[vblk] queue 0 unavailable\r\n");
        wr(R_Status, S_FAILED);
        return -1;
    }
    vb_qsize = (U2)((qmax < VQ_SIZE) ? qmax : VQ_SIZE);

    /* zero queue memory */
    for (UW i = 0; i < sizeof(vq_mem); i++) vq_mem[i] = 0;
    vq_desc  = (vring_desc  *)(vq_mem);
    vq_avail = (vring_avail *)(vq_mem + vb_qsize * sizeof(vring_desc));
    vq_used  = (vring_used  *)(vq_mem + VQ_PAGE_SIZE);

    wr(R_QueueNum, vb_qsize);

    unsigned long desc_pa  = (unsigned long)vq_desc;
    unsigned long avail_pa = (unsigned long)vq_avail;
    unsigned long used_pa  = (unsigned long)vq_used;

    if (ver == 2) {
        wr(R_QueueDescLow,  (UW)(desc_pa & 0xFFFFFFFFu));
        wr(R_QueueDescHigh, (UW)(desc_pa >> 32));
        wr(R_QueueAvailLow, (UW)(avail_pa & 0xFFFFFFFFu));
        wr(R_QueueAvailHigh,(UW)(avail_pa >> 32));
        wr(R_QueueUsedLow,  (UW)(used_pa & 0xFFFFFFFFu));
        wr(R_QueueUsedHigh, (UW)(used_pa >> 32));
        MMIO_DSB();
        wr(R_QueueReady, 1);
    } else {
        /* legacy: whole queue is one contiguous block addressed by PFN;
         * used ring is QueueAlign-aligned past desc+avail (we lay it at
         * page 1, QueueAlign = page). */
        wr(R_QueueAlign, VQ_PAGE_SIZE);
        wr(R_QueuePFN, (UW)(desc_pa / VQ_PAGE_SIZE));
    }

    /* --- DRIVER_OK --- */
    status |= S_DRIVER_OK;
    wr(R_Status, status);
    MMIO_DSB();

    /* --- read capacity from config space (le64, in 512-byte sectors) --- */
    UW cap_lo = rd(R_Config + 0);
    UW cap_hi = rd(R_Config + 4);
    /* UW is 32-bit (LP64 typedef); ARK's total_sectors is U4 anyway, so a
     * device >2TiB would saturate. Clamp the high word into a flag. */
    if (cap_hi != 0) vb_sectors = 0xFFFFFFFFu;
    else             vb_sectors = cap_lo;

    vb_last_used = 0;
    vb_ready = 1;

    vb_puts("[vblk] virtio-blk ready (");
    vb_puts(ver == 2 ? "modern" : "legacy");
    vb_puts(" mmio)\r\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Request submission (polled)                                         */
/* ------------------------------------------------------------------ */

/* Build a 2- or 3-descriptor chain at head 0 and poll the used ring.
 * data==NULL/len==0 => no data descriptor (flush). dev_writes => device
 * fills the data buffer (a read). Returns 0 on OK status, <0 otherwise. */
static INT vb_submit(UW type, U4 sector_lo, U4 sector_hi,
                     void *data, UW data_len, INT dev_writes)
{
    if (!vb_ready) return -1;

    vq_hdr.type      = type;
    vq_hdr.reserved  = 0;
    vq_hdr.sector_lo = sector_lo;
    vq_hdr.sector_hi = sector_hi;
    vq_status = 0xFF;

    unsigned long hdr_pa = (unsigned long)&vq_hdr;
    unsigned long st_pa  = (unsigned long)&vq_status;

    /* desc[0]: header, device-readable */
    vq_desc[0].addr_lo = (U4)(hdr_pa & 0xFFFFFFFFu);
    vq_desc[0].addr_hi = (U4)(hdr_pa >> 32);
    vq_desc[0].len     = (U4)sizeof(virtio_blk_req_hdr);
    vq_desc[0].flags   = VRING_DESC_F_NEXT;
    vq_desc[0].next    = 1;

    U2 status_idx;
    if (data && data_len) {
        unsigned long d_pa = (unsigned long)data;
        vq_desc[1].addr_lo = (U4)(d_pa & 0xFFFFFFFFu);
        vq_desc[1].addr_hi = (U4)(d_pa >> 32);
        vq_desc[1].len     = (U4)data_len;
        vq_desc[1].flags   = (U2)(VRING_DESC_F_NEXT |
                                  (dev_writes ? VRING_DESC_F_WRITE : 0));
        vq_desc[1].next    = 2;
        status_idx = 2;
    } else {
        status_idx = 1;
    }

    /* status descriptor, device-writable */
    vq_desc[status_idx].addr_lo = (U4)(st_pa & 0xFFFFFFFFu);
    vq_desc[status_idx].addr_hi = (U4)(st_pa >> 32);
    vq_desc[status_idx].len     = 1;
    vq_desc[status_idx].flags   = VRING_DESC_F_WRITE;
    vq_desc[status_idx].next    = 0;

    /* publish into the available ring */
    vq_avail->ring[vq_avail->idx % vb_qsize] = 0;   /* head desc index */
    MMIO_DSB();
    vq_avail->idx++;
    MMIO_DSB();

    wr(R_QueueNotify, 0);
    MMIO_DSB();

    /* poll the used ring */
    U4 spins = 0;
    while (vq_used->idx == vb_last_used) {
        if (++spins > 200000000u) {
            vb_puts("[vblk] request timeout\r\n");
            return -1;
        }
        MMIO_DSB();
    }
    vb_last_used = vq_used->idx;

    /* ACK the device interrupt status (polled, but keep the line clean) */
    UW isr = rd(R_InterruptStatus);
    if (isr) wr(R_InterruptACK, isr);

    return (vq_status == VIRTIO_BLK_S_OK) ? 0 : -1;
}

INT virtio_blk_read(UW lba, UW n, void *buf)
{
    if (!vb_ready) return -1;
    if (n == 0) return 0;
    return vb_submit(VIRTIO_BLK_T_IN, (U4)lba, 0, buf, n * 512u, 1);
}

INT virtio_blk_write(UW lba, UW n, const void *buf)
{
    if (!vb_ready) return -1;
    if (n == 0) return 0;
    return vb_submit(VIRTIO_BLK_T_OUT, (U4)lba, 0, (void *)buf, n * 512u, 0);
}

INT virtio_blk_flush(void)
{
    if (!vb_ready) return -1;
    if (!vb_has_flush) return 0;     /* feature absent: writes already issued */
    return vb_submit(VIRTIO_BLK_T_FLUSH, 0, 0, NULL, 0, 0);
}

UW  virtio_blk_sector_count(void) { return vb_sectors; }
INT virtio_blk_present(void)       { return vb_ready; }
