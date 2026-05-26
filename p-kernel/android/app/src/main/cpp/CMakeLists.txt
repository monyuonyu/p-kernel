# android/app/src/main/cpp/CMakeLists.txt
#
# Builds libpkernel.so by reusing the arch/linux/aarch64 + arch/common
# + kernel/common sources via relative paths. Drop into an Android
# Studio project that's configured for CMake; the rest of the gradle
# files (build.gradle, AndroidManifest.xml) live alongside.
#
# Toolchain assumption: ndk-bundle r25c or newer, API 21+, only the
# arm64-v8a ABI for now (x86_64 sibling arrives with the x86 port).

cmake_minimum_required(VERSION 3.18)
project(pkernel LANGUAGES C ASM)

# From this file at android/app/src/main/cpp/CMakeLists.txt, climb 5 dirs
# to reach the p-kernel source root (cpp → main → src → app → android → root).
# Phase A's original 6-level path was off-by-one; it accidentally worked
# only because Phase A used boot/linux/Makefile (the host .so target) for
# its dlopen smoke test instead of the NDK build below.
set(PK_ROOT  ${CMAKE_SOURCE_DIR}/../../../../..)
set(ARCH_LX  ${PK_ROOT}/arch/linux)
set(ARCH_AA  ${PK_ROOT}/arch/aarch64)
set(ARCH_CO  ${PK_ROOT}/arch/common)
set(KCOMMON  ${PK_ROOT}/kernel/common)
set(INC_TK   ${PK_ROOT}/include/kernel/tkernel)
set(INC_TOP  ${PK_ROOT}/include)
set(INC_LIB  ${PK_ROOT}/include/lib)
set(LIBSTR   ${PK_ROOT}/lib/libc/string)
set(RELAY    ${PK_ROOT}/relay)

# Headers — same path order as boot/linux/Makefile.
# arch/linux/include sits BEFORE arch/aarch64/include so the hosted
# utk_config_depend.h (CFN_REALMEMEND = ~0UL, required for PIE/ASLR-safe
# Imalloc init) wins over the bare-metal one (0x50000000UL).
#
# include/lib/libc is appended via -idirafter on the KCOMMON/LIBSTR
# source properties below, so it's a *fallback* for headers like
# ctype.h / math.h / setjmp.h that T-Kernel kernel sources reference
# but T-Kernel itself doesn't provide. Doing this with `-I` AT THE
# FRONT would shadow T-Kernel's errno.h (E_OK, etc.) with the POSIX
# stub and break every kernel/common file that returns ER codes.
include_directories(
    ${ARCH_LX}/aarch64/include
    ${ARCH_LX}/include
    ${ARCH_AA}/include
    ${ARCH_CO}/include/lp64
    ${ARCH_CO}/include
    ${RELAY}
    ${INC_TK}
    ${INC_TOP}
)

# Path that needs to be a *late* fallback, not an early shadow.
set(KCOMMON_EXTRA_INC ${INC_LIB}/libc)

add_compile_definitions(_APP_AARCH64_ _APP_LINUX_ _TK_HOSTED_LIBC_)
# Disable Bionic's FORTIFY headers: they include <limits.h> expecting
# POSIX-complete contents (SSIZE_MAX, etc.), but our T-Kernel placeholder
# include/kernel/tkernel/limits.h is on the include path FIRST and only
# defines the C-standard subset. The host glibc build doesn't trigger
# this because glibc defaults FORTIFY off; NDK clang on Bionic defaults
# it on. Re-arming our own bounds checks lives in the kernel, not libc.
add_compile_options(-Wall -Wextra -O2 -fPIC -fvisibility=hidden
                    -fno-stack-protector -fno-common -std=gnu11
                    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
# -fvisibility=hidden: only the JNI bridge's JNIEXPORT'd symbols and the
# pkernel_main entry (visibility("default") in main.c) are externally
# visible. Cuts ld errors about "may bind externally" on cpu_support.S's
# adrp + add :lo12: against arch_irq_disabled_flag / knl_ctxtsk / etc.

# boot/linux/main.c is renamed via -Dmain=pkernel_main below so the
# JNI's own main symbol is unaffected.
set(BOOT     ${PK_ROOT}/boot/linux/main.c)
set_source_files_properties(${BOOT} PROPERTIES
    COMPILE_DEFINITIONS "main=pkernel_main")

# Per-arch (arch/linux/aarch64)
set(ARCH_SRC
    ${ARCH_LX}/aarch64/cpu_init.c
    ${ARCH_LX}/aarch64/tkdev_init.c
    ${ARCH_LX}/aarch64/inittask_def.c
    ${ARCH_LX}/aarch64/inittask_main.c
    ${ARCH_LX}/aarch64/usermain.c
    ${ARCH_LX}/aarch64/sio.c
    ${ARCH_LX}/aarch64/pci.c
    ${ARCH_LX}/aarch64/rtl8139.c
    ${ARCH_LX}/aarch64/net_unix.c
    ${ARCH_LX}/aarch64/net_relay.c
    ${ARCH_LX}/aarch64/net_dispatch.c
    ${ARCH_LX}/aarch64/arch_reboot.c
    ${ARCH_LX}/aarch64/vfs_stub.c
    ${ARCH_LX}/aarch64/preempt.c
    ${ARCH_LX}/aarch64/cpu_support.S
)

# relay-side sha256 — same canonical source as boot/linux/Makefile.
# net_relay.c calls into hmac_sha256 to authenticate v2 wire packets.
set(RELAY_SRC
    ${RELAY}/sha256.c
)

# arch/common (AI + distributed). EXPLICIT list, matching
# boot/linux/Makefile's COMMON_C_SRCS. Earlier this used file(GLOB *.c)
# but that hauled in driver-host files that the hosted port doesn't
# need (and that pull in T-Kernel-only error constants).
set(COMMON_SRC
    ${ARCH_CO}/tensor.c
    ${ARCH_CO}/ai_job.c
    ${ARCH_CO}/pipeline.c
    ${ARCH_CO}/ai_stats.c
    ${ARCH_CO}/netstack.c
    ${ARCH_CO}/drpc.c
    ${ARCH_CO}/swim.c
    ${ARCH_CO}/kdds.c
    ${ARCH_CO}/replica.c
    ${ARCH_CO}/pmesh.c
    ${ARCH_CO}/sfs.c
    ${ARCH_CO}/heal.c
    ${ARCH_CO}/degrade.c
    ${ARCH_CO}/vital.c
    ${ARCH_CO}/edf.c
    ${ARCH_CO}/raft.c
    ${ARCH_CO}/spawn.c
    ${ARCH_CO}/moe.c
    ${ARCH_CO}/dtr.c
    ${ARCH_CO}/dkva.c
    ${ARCH_CO}/dmn.c
    ${ARCH_CO}/ga.c
    ${ARCH_CO}/fedlearn.c
    ${ARCH_CO}/mem_store.c
    ${ARCH_CO}/chat.c
    ${ARCH_CO}/dproc.c
    ${ARCH_CO}/kloader_task.c
    ${ARCH_CO}/demo_kdds.c
)

# kernel/common — same explicit list as boot/linux/Makefile's KERNEL_SRCS.
set(KCOMMON_SRC
    ${KCOMMON}/task.c
    ${KCOMMON}/task_manage.c
    ${KCOMMON}/task_sync.c
    ${KCOMMON}/timer.c
    ${KCOMMON}/time_calls.c
    ${KCOMMON}/wait.c
    ${KCOMMON}/semaphore.c
    ${KCOMMON}/eventflag.c
    ${KCOMMON}/mailbox.c
    ${KCOMMON}/messagebuf.c
    ${KCOMMON}/mutex.c
    ${KCOMMON}/mempfix.c
    ${KCOMMON}/mempool.c
    ${KCOMMON}/memory.c
    ${KCOMMON}/misc_calls.c
    ${KCOMMON}/tkstart.c
    ${KCOMMON}/bitop.c
    ${KCOMMON}/fastlock.c
    ${KCOMMON}/fastmlock.c
    ${KCOMMON}/klock.c
    ${KCOMMON}/objname.c
    ${KCOMMON}/rendezvous.c
    ${KCOMMON}/subsystem.c
    ${KCOMMON}/tm_putchar.c
    ${KCOMMON}/tm_putstring.c
    ${KCOMMON}/tm_printf.c
    ${KCOMMON}/tm_monitor.c
    ${KCOMMON}/tm_exit.c
    ${KCOMMON}/tm_getchar.c
    ${KCOMMON}/tm_getline.c
    ${KCOMMON}/tm_command.c
)
set_source_files_properties(${KCOMMON_SRC} PROPERTIES
    COMPILE_OPTIONS "-idirafter;${KCOMMON_EXTRA_INC}")

# lib/libc string helpers — same explicit list as boot/linux/Makefile.
set(LIBSTR_SRC
    ${LIBSTR}/memset.c
    ${LIBSTR}/memcpy.c
    ${LIBSTR}/memmove.c
    ${LIBSTR}/memcmp.c
    ${LIBSTR}/strlen.c
    ${LIBSTR}/strcpy.c
    ${LIBSTR}/strncpy.c
    ${LIBSTR}/strcmp.c
    ${LIBSTR}/strncmp.c
)
set_source_files_properties(${LIBSTR_SRC} PROPERTIES
    COMPILE_OPTIONS "-idirafter;${KCOMMON_EXTRA_INC}")

# JNI shim
set(JNI_SRC ${CMAKE_CURRENT_SOURCE_DIR}/pkernel_jni.c)

add_library(pkernel SHARED
    ${BOOT} ${ARCH_SRC} ${RELAY_SRC} ${COMMON_SRC} ${KCOMMON_SRC}
    ${LIBSTR_SRC} ${JNI_SRC})

# Bionic's libc + posix timers (timer_create lives there); no -lrt.
target_link_libraries(pkernel log android)
