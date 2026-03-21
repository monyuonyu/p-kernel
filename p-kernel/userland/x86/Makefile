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
#   03_rtos_task/rtos_task.elf
#   04_rtos_sync/rtos_sync.elf
#   05_all_demo/all_demo.elf
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
    03_rtos_task/rtos_task.elf \
    04_rtos_sync/rtos_sync.elf \
    06_net_infer/net_infer.elf \
    07_stdin_echo/stdin_echo.elf \
    08_http_get/http_get.elf \
    09_readdir/readdir.elf \
    10_mutex/mutex.elf \
    11_mailbox/mailbox.elf \
    12_msgbuf/msgbuf.elf \
    13_mempool/mempool.elf \
    14_cyc_alm/cyc_alm.elf \
    15_task_ext/task_ext.elf \
    16_time/time.elf \
    17_rendezvous/rendezvous.elf \
    18_ref/ref.elf \
    19_misc/misc.elf \
    20_multicast/multicast.elf

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
# 03_rtos_task
# -------------------------------------------------------------------
03_rtos_task/rtos_task.elf: $(S)/03_rtos_task/rtos_task.c plibc.h user.ld
	mkdir -p 03_rtos_task
	$(CC) $(CFLAGS) -c $< -o 03_rtos_task/rtos_task.o
	$(LD) $(LDFLAGS) -o $@ 03_rtos_task/rtos_task.o

# -------------------------------------------------------------------
# 04_rtos_sync
# -------------------------------------------------------------------
04_rtos_sync/rtos_sync.elf: $(S)/04_rtos_sync/rtos_sync.c plibc.h user.ld
	mkdir -p 04_rtos_sync
	$(CC) $(CFLAGS) -c $< -o 04_rtos_sync/rtos_sync.o
	$(LD) $(LDFLAGS) -o $@ 04_rtos_sync/rtos_sync.o

# -------------------------------------------------------------------
# 06_net_infer — センサーデータ分類 + UDP 配信デモ
# -------------------------------------------------------------------
06_net_infer/net_infer.elf: $(S)/06_net_infer/net_infer.c plibc.h user.ld
	mkdir -p 06_net_infer
	$(CC) $(CFLAGS) -c $< -o 06_net_infer/net_infer.o
	$(LD) $(LDFLAGS) -o $@ 06_net_infer/net_infer.o

# -------------------------------------------------------------------
# 07_stdin_echo — 標準入力エコーデモ
# -------------------------------------------------------------------
07_stdin_echo/stdin_echo.elf: $(S)/07_stdin_echo/stdin_echo.c plibc.h user.ld
	mkdir -p 07_stdin_echo
	$(CC) $(CFLAGS) -c $< -o 07_stdin_echo/stdin_echo.o
	$(LD) $(LDFLAGS) -o $@ 07_stdin_echo/stdin_echo.o

# -------------------------------------------------------------------
# 08_http_get — TCP 接続 + HTTP GET デモ
# -------------------------------------------------------------------
08_http_get/http_get.elf: $(S)/08_http_get/http_get.c plibc.h user.ld
	mkdir -p 08_http_get
	$(CC) $(CFLAGS) -c $< -o 08_http_get/http_get.o
	$(LD) $(LDFLAGS) -o $@ 08_http_get/http_get.o

# -------------------------------------------------------------------
# 09_readdir — ディレクトリ一覧デモ
# -------------------------------------------------------------------
09_readdir/readdir.elf: $(S)/09_readdir/readdir.c plibc.h user.ld
	mkdir -p 09_readdir
	$(CC) $(CFLAGS) -c $< -o 09_readdir/readdir.o
	$(LD) $(LDFLAGS) -o $@ 09_readdir/readdir.o

# -------------------------------------------------------------------
# 10_mutex
# -------------------------------------------------------------------
10_mutex/mutex.elf: $(S)/10_mutex/mutex.c plibc.h user.ld
	mkdir -p 10_mutex
	$(CC) $(CFLAGS) -c $< -o 10_mutex/mutex.o
	$(LD) $(LDFLAGS) -o $@ 10_mutex/mutex.o

# -------------------------------------------------------------------
# 11_mailbox
# -------------------------------------------------------------------
11_mailbox/mailbox.elf: $(S)/11_mailbox/mailbox.c plibc.h user.ld
	mkdir -p 11_mailbox
	$(CC) $(CFLAGS) -c $< -o 11_mailbox/mailbox.o
	$(LD) $(LDFLAGS) -o $@ 11_mailbox/mailbox.o

# -------------------------------------------------------------------
# 12_msgbuf
# -------------------------------------------------------------------
12_msgbuf/msgbuf.elf: $(S)/12_msgbuf/msgbuf.c plibc.h user.ld
	mkdir -p 12_msgbuf
	$(CC) $(CFLAGS) -c $< -o 12_msgbuf/msgbuf.o
	$(LD) $(LDFLAGS) -o $@ 12_msgbuf/msgbuf.o

# -------------------------------------------------------------------
# 13_mempool
# -------------------------------------------------------------------
13_mempool/mempool.elf: $(S)/13_mempool/mempool.c plibc.h user.ld
	mkdir -p 13_mempool
	$(CC) $(CFLAGS) -c $< -o 13_mempool/mempool.o
	$(LD) $(LDFLAGS) -o $@ 13_mempool/mempool.o

# -------------------------------------------------------------------
# 14_cyc_alm
# -------------------------------------------------------------------
14_cyc_alm/cyc_alm.elf: $(S)/14_cyc_alm/cyc_alm.c plibc.h user.ld
	mkdir -p 14_cyc_alm
	$(CC) $(CFLAGS) -c $< -o 14_cyc_alm/cyc_alm.o
	$(LD) $(LDFLAGS) -o $@ 14_cyc_alm/cyc_alm.o

# -------------------------------------------------------------------
# 15_task_ext
# -------------------------------------------------------------------
15_task_ext/task_ext.elf: $(S)/15_task_ext/task_ext.c plibc.h user.ld
	mkdir -p 15_task_ext
	$(CC) $(CFLAGS) -c $< -o 15_task_ext/task_ext.o
	$(LD) $(LDFLAGS) -o $@ 15_task_ext/task_ext.o

# -------------------------------------------------------------------
# 16_time
# -------------------------------------------------------------------
16_time/time.elf: $(S)/16_time/time.c plibc.h user.ld
	mkdir -p 16_time
	$(CC) $(CFLAGS) -c $< -o 16_time/time.o
	$(LD) $(LDFLAGS) -o $@ 16_time/time.o

# -------------------------------------------------------------------
# 17_rendezvous
# -------------------------------------------------------------------
17_rendezvous/rendezvous.elf: $(S)/17_rendezvous/rendezvous.c plibc.h user.ld
	mkdir -p 17_rendezvous
	$(CC) $(CFLAGS) -c $< -o 17_rendezvous/rendezvous.o
	$(LD) $(LDFLAGS) -o $@ 17_rendezvous/rendezvous.o

# -------------------------------------------------------------------
# 18_ref
# -------------------------------------------------------------------
18_ref/ref.elf: $(S)/18_ref/ref.c plibc.h user.ld
	mkdir -p 18_ref
	$(CC) $(CFLAGS) -c $< -o 18_ref/ref.o
	$(LD) $(LDFLAGS) -o $@ 18_ref/ref.o

# -------------------------------------------------------------------
# 20_multicast
# -------------------------------------------------------------------
20_multicast/multicast.elf: $(S)/20_multicast/multicast.c plibc.h user.ld
	mkdir -p 20_multicast
	$(CC) $(CFLAGS) -c $< -o 20_multicast/multicast.o
	$(LD) $(LDFLAGS) -o $@ 20_multicast/multicast.o

# -------------------------------------------------------------------
# 19_misc
# -------------------------------------------------------------------
19_misc/misc.elf: $(S)/19_misc/misc.c plibc.h user.ld
	mkdir -p 19_misc
	$(CC) $(CFLAGS) -c $< -o 19_misc/misc.o
	$(LD) $(LDFLAGS) -o $@ 19_misc/misc.o

# -------------------------------------------------------------------
# クリーン
# -------------------------------------------------------------------
clean:
	rm -rf 01_hello 02_posix_io 03_rtos_task 04_rtos_sync \
	       06_net_infer 07_stdin_echo 08_http_get 09_readdir \
	       10_mutex 11_mailbox 12_msgbuf 13_mempool 14_cyc_alm \
	       15_task_ext 16_time 17_rendezvous 18_ref 19_misc 20_multicast

.PHONY: all clean
