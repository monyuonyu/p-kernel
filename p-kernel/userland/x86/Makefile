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
    05_all_demo/all_demo.elf \
    06_net_infer/net_infer.elf \
    07_stdin_echo/stdin_echo.elf \
    08_http_get/http_get.elf \
    09_readdir/readdir.elf

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
# 05_all_demo (複数ソースファイルをリンク)
#   -I$(S)/05_all_demo で common.h (samples/05_all_demo/) を参照
# -------------------------------------------------------------------
DEMO_SRCS = \
    $(S)/05_all_demo/main.c \
    $(S)/05_all_demo/posix.c \
    $(S)/05_all_demo/sem.c \
    $(S)/05_all_demo/flg.c \
    $(S)/05_all_demo/task.c \
    $(S)/05_all_demo/sched.c

DEMO_OBJS = $(patsubst $(S)/05_all_demo/%.c, 05_all_demo/%.o, $(DEMO_SRCS))

05_all_demo/%.o: $(S)/05_all_demo/%.c plibc.h $(S)/05_all_demo/common.h
	mkdir -p 05_all_demo
	$(CC) $(CFLAGS) -I$(S)/05_all_demo -c $< -o $@

05_all_demo/all_demo.elf: $(DEMO_OBJS) user.ld
	$(LD) $(LDFLAGS) -o $@ $(DEMO_OBJS)

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
# クリーン
# -------------------------------------------------------------------
clean:
	rm -rf 01_hello 02_posix_io 03_rtos_task 04_rtos_sync 05_all_demo \
	       06_net_infer 07_stdin_echo 08_http_get 09_readdir

.PHONY: all clean
