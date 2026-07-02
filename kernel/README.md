# カーネルコア

T-Kernel 系カーネルのコア実装です。タスク管理、メモリ管理、同期プリミティブ、タイマー機能などカーネルの中心機能を提供します。

現在、micro T-Kernel 2.0（`common/`）から μT-Kernel 3.0（`mtkernel3/`）への
移行を進めています。linux_x86_64 ターゲットは 3.0 コアで動作確認済み
（`boot/linux_x86_64/Makefile.mtk3`）、その他のターゲットは 2.0 コアのままです。

## サブディレクトリ

| ディレクトリ | 内容 | 状態 |
|------------|------|------|
| `mtkernel3/` | μT-Kernel 3.0 コア（IEEE 2050-2018 準拠、T-License 2.2）。LP64 対応・linux_x86_64 ポート・日本語コメント刷新済み。取り込み元と同期手順は `mtkernel3/VENDOR.md` 参照 | ✅ linux_x86_64 で動作確認済み |
| `common/` | micro T-Kernel 2.0 のアーキテクチャ非依存コア（タスク・メモリ・同期・タイマー等） | ✅ 動作確認済み（x86 / aarch64 / linux ターゲットで現役） |
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
