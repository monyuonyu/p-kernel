# ドキュメント

p-kernel プロジェクトのドキュメントです。
**正直な現状（動く / 設計段階 / 構想）の正典はルートの [`README.md`](../README.md)。**

## はじめての方へ

| ドキュメント | 内容 |
|------------|------|
| [**`START-HERE.md`**](START-HERE.md) | **平易な言葉での入口** — p-kernel とは何か・5レイヤー・1ノードの起動。専門用語なしで新しい人が最初に読むページ。 |

## トップレベルガイド

| ドキュメント | 内容 |
|------------|------|
| [`quickstart.md`](quickstart.md) | UMP — Linux プロセスとして 60 秒で起動（aarch64 / x86_64 両対応） |
| [`cheatsheet.md`](cheatsheet.md) | シェルコマンド チートシート（ベアメタル x86） |
| [`android.md`](android.md) | UMP を Android APK としてビルド（Phase A→D） |
| [`netboot.md`](netboot.md) | Raspberry Pi 3B+ ベアメタル netboot 反復ループ |
| [`phase_b_relay.md`](phase_b_relay.md) | NAT 越え relay（v2 wire・HMAC・リプレイ防御） |
| [`assessment_structural_gaps.md`](assessment_structural_gaps.md) | 構造的ギャップ分析（所見・点群ステータス） |
| [`project-readme.md`](project-readme.md) | 旧コード README（de-nest で移設・現在はポインタ） |

## サブディレクトリ

| ディレクトリ | 内容 |
|------------|------|
| [`architecture/`](architecture/README.md) | 思想・設計地図（survival-network / regions / reflex-deliberation / p-fs …） |
| [`benchmarks/`](benchmarks/) | 計測（latency / locality / survival） |
| [`manuals/`](manuals/README.md) | ユーザーマニュアル・技術リファレンス |

## 実装ドキュメントの場所

各コンポーネントの詳細なドキュメントはソースディレクトリ内の README に記載されています。

| ドキュメント | 場所 |
|------------|------|
| x86/QEMU ビルド・実行方法 | [`boot/x86/README.md`](../boot/x86/README.md) |
| x86 アーキテクチャ・TCP/IP スタック | [`arch/x86/README.md`](../arch/x86/README.md) |
| T-Kernel コア API リファレンス | [`kernel/common/API_REFERENCE.md`](../kernel/common/API_REFERENCE.md) |
| カーネルコアモジュール | [`kernel/common/README.md`](../kernel/common/README.md) |
