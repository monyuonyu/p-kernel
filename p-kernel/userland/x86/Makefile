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
#   01_hello/hello.elf
#   02_posix_io/posix_io.elf
#   05_rtos_task/rtos_task.elf
#   06_rtos_sync/rtos_sync.elf
#
# 使い方:
#   make                              — 全 ELF をビルド
#   make 01_hello/hello.elf           — 個別ビルド
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
    01_hello/hello.elf \
    02_posix_io/posix_io.elf \
    03_stdin_echo/stdin_echo.elf \
    04_readdir/readdir.elf \
    05_rtos_task/rtos_task.elf \
    06_rtos_sync/rtos_sync.elf \
    07_mutex/mutex.elf \
    08_task_ext/task_ext.elf \
    09_mailbox/mailbox.elf \
    10_msgbuf/msgbuf.elf \
    11_rendezvous/rendezvous.elf \
    12_mempool/mempool.elf \
    13_time/time.elf \
    14_cyc_alm/cyc_alm.elf \
    15_ref/ref.elf \
    16_misc/misc.elf \
    17_net_infer/net_infer.elf \
    18_http_get/http_get.elf \
    19_multicast/multicast.elf

all: $(SAMPLES)

# -------------------------------------------------------------------
# 01_hello
# -------------------------------------------------------------------
01_hello/hello.elf: $(S)/01_hello/hello.c plibc.h user.ld
	mkdir -p 01_hello
	$(CC) $(CFLAGS) -c $< -o 01_hello/hello.o
	$(LD) $(LDFLAGS) -o $@ 01_hello/hello.o

# -------------------------------------------------------------------
# 02_posix_io
# -------------------------------------------------------------------
02_posix_io/posix_io.elf: $(S)/02_posix_io/posix_io.c plibc.h user.ld
	mkdir -p 02_posix_io
	$(CC) $(CFLAGS) -c $< -o 02_posix_io/posix_io.o
	$(LD) $(LDFLAGS) -o $@ 02_posix_io/posix_io.o

# -------------------------------------------------------------------
# 05_rtos_task
# -------------------------------------------------------------------
05_rtos_task/rtos_task.elf: $(S)/05_rtos_task/rtos_task.c plibc.h user.ld
	mkdir -p 05_rtos_task
	$(CC) $(CFLAGS) -c $< -o 05_rtos_task/rtos_task.o
	$(LD) $(LDFLAGS) -o $@ 05_rtos_task/rtos_task.o

# -------------------------------------------------------------------
# 06_rtos_sync
# -------------------------------------------------------------------
06_rtos_sync/rtos_sync.elf: $(S)/06_rtos_sync/rtos_sync.c plibc.h user.ld
	mkdir -p 06_rtos_sync
	$(CC) $(CFLAGS) -c $< -o 06_rtos_sync/rtos_sync.o
	$(LD) $(LDFLAGS) -o $@ 06_rtos_sync/rtos_sync.o

# -------------------------------------------------------------------
# 17_net_infer — センサーデータ分類 + UDP 配信デモ
# -------------------------------------------------------------------
17_net_infer/net_infer.elf: $(S)/17_net_infer/net_infer.c plibc.h user.ld
	mkdir -p 17_net_infer
	$(CC) $(CFLAGS) -c $< -o 17_net_infer/net_infer.o
	$(LD) $(LDFLAGS) -o $@ 17_net_infer/net_infer.o

# -------------------------------------------------------------------
# 03_stdin_echo — 標準入力エコーデモ
# -------------------------------------------------------------------
03_stdin_echo/stdin_echo.elf: $(S)/03_stdin_echo/stdin_echo.c plibc.h user.ld
	mkdir -p 03_stdin_echo
	$(CC) $(CFLAGS) -c $< -o 03_stdin_echo/stdin_echo.o
	$(LD) $(LDFLAGS) -o $@ 03_stdin_echo/stdin_echo.o

# -------------------------------------------------------------------
# 18_http_get — TCP 接続 + HTTP GET デモ
# -------------------------------------------------------------------
18_http_get/http_get.elf: $(S)/18_http_get/http_get.c plibc.h user.ld
	mkdir -p 18_http_get
	$(CC) $(CFLAGS) -c $< -o 18_http_get/http_get.o
	$(LD) $(LDFLAGS) -o $@ 18_http_get/http_get.o

# -------------------------------------------------------------------
# 04_readdir — ディレクトリ一覧デモ
# -------------------------------------------------------------------
04_readdir/readdir.elf: $(S)/04_readdir/readdir.c plibc.h user.ld
	mkdir -p 04_readdir
	$(CC) $(CFLAGS) -c $< -o 04_readdir/readdir.o
	$(LD) $(LDFLAGS) -o $@ 04_readdir/readdir.o

# -------------------------------------------------------------------
# 07_mutex
# -------------------------------------------------------------------
07_mutex/mutex.elf: $(S)/07_mutex/mutex.c plibc.h user.ld
	mkdir -p 07_mutex
	$(CC) $(CFLAGS) -c $< -o 07_mutex/mutex.o
	$(LD) $(LDFLAGS) -o $@ 07_mutex/mutex.o

# -------------------------------------------------------------------
# 09_mailbox
# -------------------------------------------------------------------
09_mailbox/mailbox.elf: $(S)/09_mailbox/mailbox.c plibc.h user.ld
	mkdir -p 09_mailbox
	$(CC) $(CFLAGS) -c $< -o 09_mailbox/mailbox.o
	$(LD) $(LDFLAGS) -o $@ 09_mailbox/mailbox.o

# -------------------------------------------------------------------
# 10_msgbuf
# -------------------------------------------------------------------
10_msgbuf/msgbuf.elf: $(S)/10_msgbuf/msgbuf.c plibc.h user.ld
	mkdir -p 10_msgbuf
	$(CC) $(CFLAGS) -c $< -o 10_msgbuf/msgbuf.o
	$(LD) $(LDFLAGS) -o $@ 10_msgbuf/msgbuf.o

# -------------------------------------------------------------------
# 12_mempool
# -------------------------------------------------------------------
12_mempool/mempool.elf: $(S)/12_mempool/mempool.c plibc.h user.ld
	mkdir -p 12_mempool
	$(CC) $(CFLAGS) -c $< -o 12_mempool/mempool.o
	$(LD) $(LDFLAGS) -o $@ 12_mempool/mempool.o

# -------------------------------------------------------------------
# 14_cyc_alm
# -------------------------------------------------------------------
14_cyc_alm/cyc_alm.elf: $(S)/14_cyc_alm/cyc_alm.c plibc.h user.ld
	mkdir -p 14_cyc_alm
	$(CC) $(CFLAGS) -c $< -o 14_cyc_alm/cyc_alm.o
	$(LD) $(LDFLAGS) -o $@ 14_cyc_alm/cyc_alm.o

# -------------------------------------------------------------------
# 08_task_ext
# -------------------------------------------------------------------
08_task_ext/task_ext.elf: $(S)/08_task_ext/task_ext.c plibc.h user.ld
	mkdir -p 08_task_ext
	$(CC) $(CFLAGS) -c $< -o 08_task_ext/task_ext.o
	$(LD) $(LDFLAGS) -o $@ 08_task_ext/task_ext.o

# -------------------------------------------------------------------
# 13_time
# -------------------------------------------------------------------
13_time/time.elf: $(S)/13_time/time.c plibc.h user.ld
	mkdir -p 13_time
	$(CC) $(CFLAGS) -c $< -o 13_time/time.o
	$(LD) $(LDFLAGS) -o $@ 13_time/time.o

# -------------------------------------------------------------------
# 11_rendezvous
# -------------------------------------------------------------------
11_rendezvous/rendezvous.elf: $(S)/11_rendezvous/rendezvous.c plibc.h user.ld
	mkdir -p 11_rendezvous
	$(CC) $(CFLAGS) -c $< -o 11_rendezvous/rendezvous.o
	$(LD) $(LDFLAGS) -o $@ 11_rendezvous/rendezvous.o

# -------------------------------------------------------------------
# 15_ref
# -------------------------------------------------------------------
15_ref/ref.elf: $(S)/15_ref/ref.c plibc.h user.ld
	mkdir -p 15_ref
	$(CC) $(CFLAGS) -c $< -o 15_ref/ref.o
	$(LD) $(LDFLAGS) -o $@ 15_ref/ref.o

# -------------------------------------------------------------------
# 19_multicast
# -------------------------------------------------------------------
19_multicast/multicast.elf: $(S)/19_multicast/multicast.c plibc.h user.ld
	mkdir -p 19_multicast
	$(CC) $(CFLAGS) -c $< -o 19_multicast/multicast.o
	$(LD) $(LDFLAGS) -o $@ 19_multicast/multicast.o

# -------------------------------------------------------------------
# 16_misc
# -------------------------------------------------------------------
16_misc/misc.elf: $(S)/16_misc/misc.c plibc.h user.ld
	mkdir -p 16_misc
	$(CC) $(CFLAGS) -c $< -o 16_misc/misc.o
	$(LD) $(LDFLAGS) -o $@ 16_misc/misc.o

# -------------------------------------------------------------------
# クリーン
# -------------------------------------------------------------------
clean:
	rm -rf 01_hello 02_posix_io 03_stdin_echo 04_readdir 05_rtos_task \
	       06_rtos_sync 07_mutex 08_task_ext 09_mailbox 10_msgbuf \
	       11_rendezvous 12_mempool 13_time 14_cyc_alm 15_ref \
	       16_misc 17_net_infer 18_http_get 19_multicast

.PHONY: all clean
