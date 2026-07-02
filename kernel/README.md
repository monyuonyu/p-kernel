# カーネルコア

T-Kernel 系カーネルのコア実装です。タスク管理、メモリ管理、同期プリミティブ、タイマー機能などカーネルの中心機能を提供します。

カーネルコアは **μT-Kernel 3.0**（`mtkernel3/`、IEEE 2050-2018 準拠、
T-License 2.2）です。micro T-Kernel 2.0（旧 `common/`）からの移行は完了し、
2.0 コアは廃止されました。

## サブディレクトリ

| ディレクトリ | 内容 | 状態 |
|------------|------|------|
| `mtkernel3/` | μT-Kernel 3.0 コア。LP64 対応・4 ターゲットのポート（linux_x86_64 / linux_aarch64 / x86_pc / aarch64_virt）・日本語コメント刷新・p-kernel 拡張（SCHED_RR、サブシステム互換層）込み。取り込み元と同期手順は `mtkernel3/VENDOR.md` 参照 | ✅ 全 4 ターゲットで動作確認済み |

## ビルド

各ターゲットの `boot/{x86,aarch64,linux,linux_x86_64}/Makefile` が
`mtkernel3/` のコア＋sysdepend をリンクします。詳細なポート構成は
`mtkernel3/kernel/sysdepend/<ターゲット>/` を参照してください。
