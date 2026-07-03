/*
 *  boot/linux_x86_64/main_mtk3.c
 *  Linux ホスト版 p-kernel（μT-Kernel 3.0 コア）のエントリポイント。
 *
 *  micro T-Kernel 2.0 用の main.c と対になるファイル。3.0 では
 *  カーネル起動シーケンス（kernel/mtkernel3/kernel/sysinit/sysinit.c）
 *  が main() を持つ設計だが、ホスト側の事前初期化（シリアル・
 *  システムメモリ領域）が必要なため、コア側は
 *  ADD_PREFIX_KNL_TO_GLOBAL_NAME 付きでビルドして knl_main() に改名し、
 *  本ファイルの main() がベアメタルポートの reset_main 相当を務める。
 *
 *  起動の流れ:
 *    main()                        ← ここ（Linux プロセスのエントリ）
 *      ├ sio_init()                termios raw モード設定
 *      ├ knl_startup_hw()          システムメモリ領域の設定（hw_setting.c）
 *      └ knl_main()                μT-Kernel 3.0 起動（sysinit.c）
 *           ├ knl_init_Imalloc() / knl_init_interrupt() / knl_init_object()
 *           ├ knl_timer_startup()  SIGALRM 周期タイマ開始
 *           └ 初期タスク生成・起動 → usermain()（対話シェル）
 */

#include <tk/tkernel.h>

/* シリアル入出力（arch/linux/x86_64/sio.c） */
IMPORT void sio_init(void);
IMPORT void sio_send_frame(const UB *buf, INT size);

/* μT-Kernel 3.0 起動（sysinit.c, ADD_PREFIX_KNL_TO_GLOBAL_NAME 有効時） */
IMPORT INT knl_main(void);

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	sio_init();

	/* QEMU virt / RPi3 と同じ起動バナー（/verify・/run 系ツールが
	 * grep するため揃えておく）。T-Kernel はまだ動いていないので
	 * tm_putstring ではなく sio を直接叩く。 */
	{
	#define BANNER(s) sio_send_frame((const UB *)(s), (INT)sizeof(s) - 1)
		BANNER("=== p-kernel linux boot ===\r\n");
		BANNER("[INIT] termios stdin/stdout\r\n");
		BANNER("[BOOT] Starting T-Kernel (uT-Kernel 3.0 core)...\r\n");
	#undef BANNER
	}

	/* システムメモリ領域（knl_lowmem_top/limit）の設定 */
	knl_startup_hw();

	/* μT-Kernel 3.0 の起動。初期タスクへディスパッチして戻らない。 */
	knl_main();

	/* ここには到達しない — usermain が永久に動き続ける */
	return 0;
}
