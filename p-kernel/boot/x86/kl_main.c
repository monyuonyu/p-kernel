/*
 * kl_main.c — Stage-1 Kernel Loader メインロジック
 *
 * 起動シーケンス:
 *   1. FAT32 初期化
 *   2. "KL.BIN"  (ネットワーク経由で書き込まれた更新版) を探す
 *      → 見つかれば 0x100000 へロード
 *   3. 見つからなければ "PKNL.BIN" (工場出荷版) を探す
 *      → 見つかれば 0x100000 へロード
 *   4. どちらも見つからなければ停止 (halt)
 *   5. 0x100000 の Multiboot エントリへジャンプ
 *
 * kloader は OS を起動しない — 32 ビット保護モードのまま動作。
 */

#include "kl_fat32.h"
#include "kl_net.h"

/* ------------------------------------------------------------------ */
/* シリアルポート出力 (デバッグ用)                                    */
/* ------------------------------------------------------------------ */
#define SERIAL_PORT 0x3F8

static inline void outb_kl(unsigned short port, unsigned char val)
{
    __asm__ volatile("outb %0, %1" :: "a"(val), "dN"(port));
}

static inline unsigned char inb_kl(unsigned short port)
{
    unsigned char val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "dN"(port));
    return val;
}

static void serial_init(void)
{
    outb_kl(SERIAL_PORT + 1, 0x00); /* 割り込み無効 */
    outb_kl(SERIAL_PORT + 3, 0x80); /* DLAB セット */
    outb_kl(SERIAL_PORT + 0, 0x01); /* 115200 baud (divisor lo) */
    outb_kl(SERIAL_PORT + 1, 0x00); /* divisor hi */
    outb_kl(SERIAL_PORT + 3, 0x03); /* 8N1 */
    outb_kl(SERIAL_PORT + 2, 0xC7); /* FIFO クリア */
    outb_kl(SERIAL_PORT + 4, 0x0B); /* RTS/DTR */
}

static void serial_putc(char c)
{
    int t = 10000;
    while (!(inb_kl(SERIAL_PORT + 5) & 0x20) && t--);
    outb_kl(SERIAL_PORT, (unsigned char)c);
}

static void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

/* ------------------------------------------------------------------ */
/* p-kernel エントリへジャンプ (32 ビット保護モード)                  */
/* ------------------------------------------------------------------ */
#define PKERNEL_LOAD_ADDR  0x100000UL
#define PKERNEL_MAX_SIZE   (8 * 1024 * 1024)   /* 最大 8 MB */

/*
 * p-kernel の _start は Multiboot ヘッダを持つ。
 * kloader は 32 ビット保護モードのまま 0x100000 へ制御を渡す。
 * p-kernel 側は自分で long mode へ移行する。
 *
 * EAX = 0x2BADB002 (Multiboot ブートローダー魔法数)
 * EBX = 0 (Multiboot 情報構造体なし)
 */
static void jump_to_kernel(void)
{
    unsigned int entry = PKERNEL_LOAD_ADDR;
    __asm__ volatile(
        "movl $0x2BADB002, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "jmp *%0\n\t"
        :: "r"(entry)
        : "eax", "ebx"
    );
    /* 返らない */
    for (;;) __asm__("hlt");
}

/* ------------------------------------------------------------------ */
/* エントリポイント                                                    */
/* ------------------------------------------------------------------ */
void kl_main(void)
{
    serial_init();
    serial_puts("[kloader] Stage-1 Kernel Loader\n");

    /* FAT32 初期化 */
    if (kl_fat32_init() < 0) {
        serial_puts("[kloader] FAT32 init FAILED — halting\n");
        for (;;) __asm__("hlt");
    }
    serial_puts("[kloader] FAT32 OK\n");

    int size;

    /* 優先: KL.BIN (ネットワーク更新版) */
    serial_puts("[kloader] Trying KL.BIN ...\n");
    size = kl_fat32_load("KL      ", "BIN",
                         (void *)PKERNEL_LOAD_ADDR, PKERNEL_MAX_SIZE);
    if (size > 0) {
        serial_puts("[kloader] KL.BIN loaded\n");
        jump_to_kernel();
    }

    /* フォールバック: PKNL.BIN (デフォルト版) */
    serial_puts("[kloader] Trying PKNL.BIN ...\n");
    size = kl_fat32_load("PKNL    ", "BIN",
                         (void *)PKERNEL_LOAD_ADDR, PKERNEL_MAX_SIZE);
    if (size > 0) {
        serial_puts("[kloader] PKNL.BIN loaded\n");
        jump_to_kernel();
    }

    /* ディスクにカーネルなし → ネットワーク経由で受信を試みる */
    serial_puts("[kloader] No kernel on disk — trying network auto-receive\n");
    if (kl_net_init() == 0) {
        int net_size = kl_net_receive_kernel((void *)PKERNEL_LOAD_ADDR,
                                              PKERNEL_MAX_SIZE);
        if (net_size > 0) {
            serial_puts("[kloader] kernel received from network — booting\n");
            jump_to_kernel();
        }
    }

    /* 両方なし */
    serial_puts("[kloader] No kernel — halting\n");
    for (;;) __asm__("hlt");
}
