# p-kernel GDB init
# Usage: gdb -x .gdbinit

set architecture i386:x86-64
set disassembly-flavor intel

# QEMUのGDBスタブに接続 (make debug で起動後に実行)
target remote localhost:1234

# シンボル読み込み (ELF形式のデバッグシンボル付きバイナリがある場合)
# symbol-file bootloader.elf

# 便利コマンド定義
define pk-regs
  info registers rax rbx rcx rdx rsi rdi rbp rsp rip rflags
end

define pk-stack
  info registers rsp rbp
  x/16gx $rsp
end

define pk-mem
  x/32wx $arg0
end

# ブレークポイントの例 (必要に応じてコメントアウト解除)
# break *0x100000   # カーネルエントリ
# break usermain
# break shell_run

echo \n=== p-kernel GDB ready ===\n
echo Commands: pk-regs, pk-stack, pk-mem <addr>\n
echo Run 'c' to continue boot, Ctrl+C to break\n\n
