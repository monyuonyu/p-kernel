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

set(PK_ROOT  ${CMAKE_SOURCE_DIR}/../../../../../..)
set(ARCH_LX  ${PK_ROOT}/arch/linux)
set(ARCH_AA  ${PK_ROOT}/arch/aarch64)
set(ARCH_CO  ${PK_ROOT}/arch/common)
set(KCOMMON  ${PK_ROOT}/kernel/common)
set(INC_TK   ${PK_ROOT}/include/kernel/tkernel)
set(INC_TOP  ${PK_ROOT}/include)
set(INC_LIB  ${PK_ROOT}/include/lib)
set(LIBSTR   ${PK_ROOT}/lib/libc/string)

# Headers — same path order as boot/linux/Makefile.
include_directories(
    ${ARCH_LX}/aarch64/include
    ${ARCH_LX}/include
    ${ARCH_AA}/include
    ${ARCH_CO}/include
    ${INC_TK}
    ${INC_TOP}
)

# kernel/common needs the T-Kernel placeholder libc; arch/linux does
# NOT (it uses system headers). We use a target-level include for the
# kernel sources and globally avoid lib/libc for the arch sources.
set(KCOMMON_EXTRA_INC ${INC_LIB}/libc)

add_compile_definitions(_APP_AARCH64_ _APP_LINUX_ _TK_HOSTED_LIBC_)
add_compile_options(-Wall -Wextra -O2 -fPIC -fno-stack-protector
                    -fno-common -std=gnu11)

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
    ${ARCH_LX}/aarch64/arch_reboot.c
    ${ARCH_LX}/aarch64/vfs_stub.c
    ${ARCH_LX}/aarch64/preempt.c
    ${ARCH_LX}/aarch64/cpu_support.S
)

# arch/common (AI + distributed; same list as boot/linux/Makefile)
file(GLOB COMMON_SRC ${ARCH_CO}/*.c)

# kernel/common
file(GLOB KCOMMON_SRC ${KCOMMON}/*.c)
set_source_files_properties(${KCOMMON_SRC} PROPERTIES
    INCLUDE_DIRECTORIES ${KCOMMON_EXTRA_INC})

# lib/libc string helpers
file(GLOB LIBSTR_SRC ${LIBSTR}/*.c)
set_source_files_properties(${LIBSTR_SRC} PROPERTIES
    INCLUDE_DIRECTORIES ${KCOMMON_EXTRA_INC})

# JNI shim
set(JNI_SRC ${CMAKE_CURRENT_SOURCE_DIR}/pkernel_jni.c)

add_library(pkernel SHARED
    ${BOOT} ${ARCH_SRC} ${COMMON_SRC} ${KCOMMON_SRC} ${LIBSTR_SRC} ${JNI_SRC})

# Bionic's libc + posix timers (timer_create lives there); no -lrt.
target_link_libraries(pkernel log android)
