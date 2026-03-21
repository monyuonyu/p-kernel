# user_hello/Makefile
# p-kernel ring-3 ユーザースペース サンプル集のビルドルール
#
# 生成物:
#   01_hello/hello.elf         Hello World
#   02_posix_io/posix_io.elf   POSIX ファイル I/O
#   03_rtos_task/rtos_task.elf RTOS タスク管理
#   04_rtos_sync/rtos_sync.elf 同期プリミティブ
#   05_all_demo/all_demo.elf      総合デモ
#
# 使い方:
#   make                              — 全 ELF をビルド
#   make 01_hello/hello.elf           — 個別ビルド
#   make 05_all_demo/all_demo.elf        — 総合デモのみ
#   make clean                        — オブジェクトと ELF を削除

CROSS   ?= i686-linux-gnu-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld

# コンパイルフラグ (-I. で user_hello/plibc.h を参照可能にする)
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
    05_all_demo/all_demo.elf

all: $(SAMPLES)

# -------------------------------------------------------------------
# 01_hello
# -------------------------------------------------------------------
01_hello/hello.elf: 01_hello/hello.c plibc.h user.ld
	$(CC) $(CFLAGS) -c 01_hello/hello.c -o 01_hello/hello.o
	$(LD) $(LDFLAGS) -o $@ 01_hello/hello.o

# -------------------------------------------------------------------
# 02_posix_io
# -------------------------------------------------------------------
02_posix_io/posix_io.elf: 02_posix_io/posix_io.c plibc.h user.ld
	$(CC) $(CFLAGS) -c 02_posix_io/posix_io.c -o 02_posix_io/posix_io.o
	$(LD) $(LDFLAGS) -o $@ 02_posix_io/posix_io.o

# -------------------------------------------------------------------
# 03_rtos_task
# -------------------------------------------------------------------
03_rtos_task/rtos_task.elf: 03_rtos_task/rtos_task.c plibc.h user.ld
	$(CC) $(CFLAGS) -c 03_rtos_task/rtos_task.c -o 03_rtos_task/rtos_task.o
	$(LD) $(LDFLAGS) -o $@ 03_rtos_task/rtos_task.o

# -------------------------------------------------------------------
# 04_rtos_sync
# -------------------------------------------------------------------
04_rtos_sync/rtos_sync.elf: 04_rtos_sync/rtos_sync.c plibc.h user.ld
	$(CC) $(CFLAGS) -c 04_rtos_sync/rtos_sync.c -o 04_rtos_sync/rtos_sync.o
	$(LD) $(LDFLAGS) -o $@ 04_rtos_sync/rtos_sync.o

# -------------------------------------------------------------------
# 05_all_demo (複数ソースファイルをリンク)
# -------------------------------------------------------------------
TEST_SRCS = \
    05_all_demo/main.c \
    05_all_demo/posix.c \
    05_all_demo/sem.c \
    05_all_demo/flg.c \
    05_all_demo/task.c \
    05_all_demo/sched.c

TEST_OBJS = $(TEST_SRCS:.c=.o)

# 05_all_demo/ 配下の .c を .o にコンパイル
# -I. で user_hello/plibc.h を、-I05_all_demo で common.h を参照
05_all_demo/%.o: 05_all_demo/%.c plibc.h 05_all_demo/common.h
	$(CC) $(CFLAGS) -I05_all_demo -c $< -o $@

05_all_demo/all_demo.elf: $(TEST_OBJS) user.ld
	$(LD) $(LDFLAGS) -o $@ $(TEST_OBJS)

# -------------------------------------------------------------------
# クリーン
# -------------------------------------------------------------------
clean:
	rm -f 01_hello/*.o    01_hello/*.elf
	rm -f 02_posix_io/*.o 02_posix_io/*.elf
	rm -f 03_rtos_task/*.o 03_rtos_task/*.elf
	rm -f 04_rtos_sync/*.o 04_rtos_sync/*.elf
	rm -f 05_all_demo/*.o    05_all_demo/*.elf
	rm -f *.o *.elf

.PHONY: all clean
