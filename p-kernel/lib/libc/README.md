# 標準Cライブラリ実装

組み込み環境向けに最適化された標準 C ライブラリ関数の実装です。ホスト OS の libc に依存せずカーネルと一緒にリンクされます。

## サブディレクトリ

| ディレクトリ | 内容 |
|------------|------|
| `ctype/` | 文字分類・変換関数（`isalpha`、`toupper` 等） |
| `stdlib/` | 標準ユーティリティ（`atoi`、`strtol` 等） |
| `string/` | 文字列・メモリ操作（`memset`、`memcpy`、`strcmp`、`strlen` 等） |

## x86/QEMU ビルドでの利用

`boot/x86/Makefile` は `lib/libc/string/` のソースを統合リンクします。`memset`・`memcpy`・`strcmp`・`strlen` 等は TCP/IP スタック・シェル・RTL8139 ドライバで使用されます。
