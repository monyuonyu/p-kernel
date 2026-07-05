/*
 *  boot/windows/x86_64/main_win.c
 *  Native Windows (mingw-w64 PE) entry point for p-kernel (μT-Kernel 3.0).
 *
 *  Sibling of boot/linux_x86_64/main_mtk3.c. The μT-Kernel 3.0 core owns
 *  main() via sysinit.c, so it is built with ADD_PREFIX_KNL_TO_GLOBAL_NAME
 *  (renamed to knl_main()); this main() does the host pre-init the core
 *  cannot: Winsock, the Win32 console, the fiber/heap bring-up.
 *
 *  Boot flow:
 *    main()                        ← Windows process entry
 *      ├ win_net_startup()         WSAStartup(2.2) before any socket()
 *      ├ sio_init()                Win32 console raw mode
 *      ├ knl_startup_hw()          ConvertThreadToFiber + system memory
 *      └ knl_main()                μT-Kernel 3.0 boot (sysinit.c)
 *           └ initial task → usermain() (interactive `mind` shell)
 */

#include <tk/tkernel.h>

/* arch/windows/x86_64/sio.c */
IMPORT void sio_init(void);
IMPORT void sio_send_frame(const UB *buf, INT size);

/* arch/windows/x86_64/net_win.c */
IMPORT void win_net_startup(void);

/* windows_x86_64/hw_setting.c (ADD_PREFIX_KNL_TO_GLOBAL_NAME) */
IMPORT void knl_startup_hw(void);

/* μT-Kernel 3.0 boot (sysinit.c, ADD_PREFIX_KNL_TO_GLOBAL_NAME) */
IMPORT INT knl_main(void);

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	win_net_startup();
	sio_init();

	/* Boot banner (kept close to the Linux one; /verify tooling greps it).
	 * T-Kernel is not running yet, so write via sio directly. */
	{
	#define BANNER(s) sio_send_frame((const UB *)(s), (INT)sizeof(s) - 1)
		BANNER("=== p-kernel windows boot ===\r\n");
		BANNER("[INIT] win32 console stdin/stdout\r\n");
		BANNER("[win] cooperative scheduler (no preempt in v1)\r\n");
		BANNER("[BOOT] Starting T-Kernel (uT-Kernel 3.0 core)...\r\n");
	#undef BANNER
	}

	knl_startup_hw();
	knl_main();		/* dispatches into the initial task; never returns */

	return 0;
}
