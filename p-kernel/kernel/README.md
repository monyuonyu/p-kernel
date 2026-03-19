# カーネルコア

micro T-Kernel 2.0 のコア実装です。タスク管理、メモリ管理、同期プリミティブ、タイマー機能などカーネルの中心機能を提供します。x86/QEMU ポートで実際に稼働し、TCP/IP スタック・HTTP クライアントまで動作確認済みです。

## サブディレクトリ

| ディレクトリ | 内容 | 状態 |
|------------|------|------|
| `common/` | アーキテクチャ非依存のカーネルコア（タスク・メモリ・同期・タイマー等） | ✅ 動作確認済み |
| `tkernel/` | T-Kernel 互換 API レイヤー | ✅ 動作確認済み |

## x86 ポートでの利用状況

```
kernel/common/ → boot/x86/Makefile でリンク
  ├── task.c / task_manage.c / task_sync.c  — タスクスケジューリング（プリエンプティブ優先度順）
  ├── semaphore.c                             — TCP RX ブロッキングに使用
  ├── timer.c                                 — PIT ベースのシステムクロック
  ├── tkstart.c                               — T-Kernel 起動・初期タスク起動
  └── その他同期・メモリ管理モジュール
```

詳細は [`common/README.md`](common/README.md) および [`common/API_REFERENCE.md`](common/API_REFERENCE.md) を参照してください。
