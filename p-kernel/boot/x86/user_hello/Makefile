# user_hello/Makefile
# p-kernel ring-3 ユーザースペース サンプル集のビルドルール
#
# 生成物:
#   01_hello/hello.elf         Hello World
#   02_posix_io/posix_io.elf   POSIX ファイル I/O
#   03_rtos_task/rtos_task.elf RTOS タスク管理
#   04_rtos_sync/rtos_sync.elf 同期プリミティブ
#   test_all/test_all.elf      包括的テストスイート
#
# 使い方:
#   make                              — 全 ELF をビルド
#   make 01_hello/hello.elf           — 個別ビルド
#   make test_all/test_all.elf        — テストスイートのみ
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
    test_all/test_all.elf

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
# test_all (複数ソースファイルをリンク)
# -------------------------------------------------------------------
TEST_SRCS = \
    test_all/test_main.c \
    test_all/test_posix.c \
    test_all/test_sem.c \
    test_all/test_flg.c \
    test_all/test_task.c \
    test_all/test_sched.c

TEST_OBJS = $(TEST_SRCS:.c=.o)

# test_all/ 配下の .c を .o にコンパイル
# -I. で user_hello/plibc.h を、-Itest_all で test_common.h を参照
test_all/%.o: test_all/%.c plibc.h test_all/test_common.h
	$(CC) $(CFLAGS) -Itest_all -c $< -o $@

test_all/test_all.elf: $(TEST_OBJS) user.ld
	$(LD) $(LDFLAGS) -o $@ $(TEST_OBJS)

# -------------------------------------------------------------------
# クリーン
# -------------------------------------------------------------------
clean:
	rm -f 01_hello/*.o    01_hello/*.elf
	rm -f 02_posix_io/*.o 02_posix_io/*.elf
	rm -f 03_rtos_task/*.o 03_rtos_task/*.elf
	rm -f 04_rtos_sync/*.o 04_rtos_sync/*.elf
	rm -f test_all/*.o    test_all/*.elf
	rm -f *.o *.elf

.PHONY: all clean
