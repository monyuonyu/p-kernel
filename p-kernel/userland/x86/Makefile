# userland/x86/Makefile
# p-kernel x86 ユーザー空間サンプルのビルドルール
#
# サンプルのソースコードはアーキテクチャ非依存の samples/ に置かれており、
# このファイルが x86 向けにコンパイル・リンクします。
#
# 将来 ARM などを追加する場合は userland/arm/Makefile を作成し、
# ARM 用の plibc.h (SVC 命令版) と user.ld を用意するだけで
# samples/ のソースをそのまま再利用できます。
#
# 生成物 (このディレクトリ以下に出力):
#   01_posix/NN/ELF ... 07_network/NN/ELF
#
# 使い方:
#   make                              — 全 ELF をビルド
#   make 01_posix/01_hello/hello.elf           — 個別ビルド
#   make clean                        — 生成物を削除

CROSS   ?= i686-linux-gnu-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld

# サンプルソースの場所 (このファイルから見た相対パス)
S = ../../samples

# コンパイルフラグ
#   -I. で userland/x86/plibc.h を参照できるようにする
CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-pic \
          -fcf-protection=none -nostdlib -O1 -std=gnu11 \
          -Wall -Wextra -I.

# リンクフラグ
LDFLAGS = -m elf_i386 -static -nostdlib -T user.ld

# ビルド対象の ELF ファイル一覧
SAMPLES = \
    01_posix/01_hello/hello.elf \
    01_posix/02_posix_io/posix_io.elf \
    01_posix/03_stdin_echo/stdin_echo.elf \
    01_posix/04_readdir/readdir.elf \
    01_posix/05_posix2/posix2.elf \
    02_rtos/01_rtos_task/rtos_task.elf \
    02_rtos/02_rtos_sync/rtos_sync.elf \
    02_rtos/03_mutex/mutex.elf \
    02_rtos/04_task_ext/task_ext.elf \
    03_ipc/01_mailbox/mailbox.elf \
    03_ipc/02_msgbuf/msgbuf.elf \
    03_ipc/03_rendezvous/rendezvous.elf \
    04_memory/01_mempool/mempool.elf \
    05_time/01_time/time.elf \
    05_time/02_cyc_alm/cyc_alm.elf \
    06_system/01_ref/ref.elf \
    06_system/02_misc/misc.elf \
    07_network/01_net_infer/net_infer.elf \
    07_network/02_http_get/http_get.elf \
    07_network/03_multicast/multicast.elf

all: $(SAMPLES)

# -------------------------------------------------------------------
# 01_posix/01_hello
# -------------------------------------------------------------------
01_posix/01_hello/hello.elf: $(S)/01_posix/01_hello/hello.c plibc.h user.ld
	mkdir -p 01_posix/01_hello
	$(CC) $(CFLAGS) -c $< -o 01_posix/01_hello/hello.o
	$(LD) $(LDFLAGS) -o $@ 01_posix/01_hello/hello.o

# -------------------------------------------------------------------
# 01_posix/02_posix_io
# -------------------------------------------------------------------
01_posix/02_posix_io/posix_io.elf: $(S)/01_posix/02_posix_io/posix_io.c plibc.h user.ld
	mkdir -p 01_posix/02_posix_io
	$(CC) $(CFLAGS) -c $< -o 01_posix/02_posix_io/posix_io.o
	$(LD) $(LDFLAGS) -o $@ 01_posix/02_posix_io/posix_io.o

# -------------------------------------------------------------------
# 02_rtos/01_rtos_task
# -------------------------------------------------------------------
02_rtos/01_rtos_task/rtos_task.elf: $(S)/02_rtos/01_rtos_task/rtos_task.c plibc.h user.ld
	mkdir -p 02_rtos/01_rtos_task
	$(CC) $(CFLAGS) -c $< -o 02_rtos/01_rtos_task/rtos_task.o
	$(LD) $(LDFLAGS) -o $@ 02_rtos/01_rtos_task/rtos_task.o

# -------------------------------------------------------------------
# 02_rtos/02_rtos_sync
# -------------------------------------------------------------------
02_rtos/02_rtos_sync/rtos_sync.elf: $(S)/02_rtos/02_rtos_sync/rtos_sync.c plibc.h user.ld
	mkdir -p 02_rtos/02_rtos_sync
	$(CC) $(CFLAGS) -c $< -o 02_rtos/02_rtos_sync/rtos_sync.o
	$(LD) $(LDFLAGS) -o $@ 02_rtos/02_rtos_sync/rtos_sync.o

# -------------------------------------------------------------------
# 07_network/01_net_infer — センサーデータ分類 + UDP 配信デモ
# -------------------------------------------------------------------
07_network/01_net_infer/net_infer.elf: $(S)/07_network/01_net_infer/net_infer.c plibc.h user.ld
	mkdir -p 07_network/01_net_infer
	$(CC) $(CFLAGS) -c $< -o 07_network/01_net_infer/net_infer.o
	$(LD) $(LDFLAGS) -o $@ 07_network/01_net_infer/net_infer.o

# -------------------------------------------------------------------
# 01_posix/03_stdin_echo — 標準入力エコーデモ
# -------------------------------------------------------------------
01_posix/03_stdin_echo/stdin_echo.elf: $(S)/01_posix/03_stdin_echo/stdin_echo.c plibc.h user.ld
	mkdir -p 01_posix/03_stdin_echo
	$(CC) $(CFLAGS) -c $< -o 01_posix/03_stdin_echo/stdin_echo.o
	$(LD) $(LDFLAGS) -o $@ 01_posix/03_stdin_echo/stdin_echo.o

# -------------------------------------------------------------------
# 07_network/02_http_get — TCP 接続 + HTTP GET デモ
# -------------------------------------------------------------------
07_network/02_http_get/http_get.elf: $(S)/07_network/02_http_get/http_get.c plibc.h user.ld
	mkdir -p 07_network/02_http_get
	$(CC) $(CFLAGS) -c $< -o 07_network/02_http_get/http_get.o
	$(LD) $(LDFLAGS) -o $@ 07_network/02_http_get/http_get.o

# -------------------------------------------------------------------
# 01_posix/04_readdir — ディレクトリ一覧デモ
# -------------------------------------------------------------------
01_posix/04_readdir/readdir.elf: $(S)/01_posix/04_readdir/readdir.c plibc.h user.ld
	mkdir -p 01_posix/04_readdir
	$(CC) $(CFLAGS) -c $< -o 01_posix/04_readdir/readdir.o
	$(LD) $(LDFLAGS) -o $@ 01_posix/04_readdir/readdir.o

# -------------------------------------------------------------------
# 02_rtos/03_mutex
# -------------------------------------------------------------------
02_rtos/03_mutex/mutex.elf: $(S)/02_rtos/03_mutex/mutex.c plibc.h user.ld
	mkdir -p 02_rtos/03_mutex
	$(CC) $(CFLAGS) -c $< -o 02_rtos/03_mutex/mutex.o
	$(LD) $(LDFLAGS) -o $@ 02_rtos/03_mutex/mutex.o

# -------------------------------------------------------------------
# 03_ipc/01_mailbox
# -------------------------------------------------------------------
03_ipc/01_mailbox/mailbox.elf: $(S)/03_ipc/01_mailbox/mailbox.c plibc.h user.ld
	mkdir -p 03_ipc/01_mailbox
	$(CC) $(CFLAGS) -c $< -o 03_ipc/01_mailbox/mailbox.o
	$(LD) $(LDFLAGS) -o $@ 03_ipc/01_mailbox/mailbox.o

# -------------------------------------------------------------------
# 03_ipc/02_msgbuf
# -------------------------------------------------------------------
03_ipc/02_msgbuf/msgbuf.elf: $(S)/03_ipc/02_msgbuf/msgbuf.c plibc.h user.ld
	mkdir -p 03_ipc/02_msgbuf
	$(CC) $(CFLAGS) -c $< -o 03_ipc/02_msgbuf/msgbuf.o
	$(LD) $(LDFLAGS) -o $@ 03_ipc/02_msgbuf/msgbuf.o

# -------------------------------------------------------------------
# 04_memory/01_mempool
# -------------------------------------------------------------------
04_memory/01_mempool/mempool.elf: $(S)/04_memory/01_mempool/mempool.c plibc.h user.ld
	mkdir -p 04_memory/01_mempool
	$(CC) $(CFLAGS) -c $< -o 04_memory/01_mempool/mempool.o
	$(LD) $(LDFLAGS) -o $@ 04_memory/01_mempool/mempool.o

# -------------------------------------------------------------------
# 05_time/02_cyc_alm
# -------------------------------------------------------------------
05_time/02_cyc_alm/cyc_alm.elf: $(S)/05_time/02_cyc_alm/cyc_alm.c plibc.h user.ld
	mkdir -p 05_time/02_cyc_alm
	$(CC) $(CFLAGS) -c $< -o 05_time/02_cyc_alm/cyc_alm.o
	$(LD) $(LDFLAGS) -o $@ 05_time/02_cyc_alm/cyc_alm.o

# -------------------------------------------------------------------
# 02_rtos/04_task_ext
# -------------------------------------------------------------------
02_rtos/04_task_ext/task_ext.elf: $(S)/02_rtos/04_task_ext/task_ext.c plibc.h user.ld
	mkdir -p 02_rtos/04_task_ext
	$(CC) $(CFLAGS) -c $< -o 02_rtos/04_task_ext/task_ext.o
	$(LD) $(LDFLAGS) -o $@ 02_rtos/04_task_ext/task_ext.o

# -------------------------------------------------------------------
# 05_time/01_time
# -------------------------------------------------------------------
05_time/01_time/time.elf: $(S)/05_time/01_time/time.c plibc.h user.ld
	mkdir -p 05_time/01_time
	$(CC) $(CFLAGS) -c $< -o 05_time/01_time/time.o
	$(LD) $(LDFLAGS) -o $@ 05_time/01_time/time.o

# -------------------------------------------------------------------
# 03_ipc/03_rendezvous
# -------------------------------------------------------------------
03_ipc/03_rendezvous/rendezvous.elf: $(S)/03_ipc/03_rendezvous/rendezvous.c plibc.h user.ld
	mkdir -p 03_ipc/03_rendezvous
	$(CC) $(CFLAGS) -c $< -o 03_ipc/03_rendezvous/rendezvous.o
	$(LD) $(LDFLAGS) -o $@ 03_ipc/03_rendezvous/rendezvous.o

# -------------------------------------------------------------------
# 06_system/01_ref
# -------------------------------------------------------------------
06_system/01_ref/ref.elf: $(S)/06_system/01_ref/ref.c plibc.h user.ld
	mkdir -p 06_system/01_ref
	$(CC) $(CFLAGS) -c $< -o 06_system/01_ref/ref.o
	$(LD) $(LDFLAGS) -o $@ 06_system/01_ref/ref.o

# -------------------------------------------------------------------
# 07_network/03_multicast
# -------------------------------------------------------------------
07_network/03_multicast/multicast.elf: $(S)/07_network/03_multicast/multicast.c plibc.h user.ld
	mkdir -p 07_network/03_multicast
	$(CC) $(CFLAGS) -c $< -o 07_network/03_multicast/multicast.o
	$(LD) $(LDFLAGS) -o $@ 07_network/03_multicast/multicast.o

# -------------------------------------------------------------------
# 06_system/02_misc
# -------------------------------------------------------------------
06_system/02_misc/misc.elf: $(S)/06_system/02_misc/misc.c plibc.h user.ld
	mkdir -p 06_system/02_misc
	$(CC) $(CFLAGS) -c $< -o 06_system/02_misc/misc.o
	$(LD) $(LDFLAGS) -o $@ 06_system/02_misc/misc.o

# -------------------------------------------------------------------
# クリーン
# -------------------------------------------------------------------
# -------------------------------------------------------------------
# 01_posix/05_posix2
# -------------------------------------------------------------------
01_posix/05_posix2/posix2.elf: $(S)/01_posix/05_posix2/posix2.c plibc.h user.ld
	mkdir -p 01_posix/05_posix2
	$(CC) $(CFLAGS) -c $< -o 01_posix/05_posix2/posix2.o
	$(LD) $(LDFLAGS) -o $@ 01_posix/05_posix2/posix2.o

clean:
	rm -rf 01_posix 02_rtos 03_ipc 04_memory 05_time 06_system 07_network