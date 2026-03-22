# p-kernel

micro T-Kernel 2.0 ベースのリアルタイム OS。x86/QEMU で動作確認済み。

## 対応アーキテクチャ

- x86 (QEMU) — メイン開発環境
- H8300
- RL78

## 実装済み機能（x86）

| カテゴリ | 機能 | 備考 |
|---------|------|------|
| OS コア | micro T-Kernel 2.0（タスク・セマフォ・IPC・メモリ・タイマー） | 全 API 実装済み |
| ユーザー空間 | ring-3 プロセス・ELF ローダー・INT 0x80 syscall・ページング | |
| ファイルシステム | VFS / FAT32 / IDE ATA PIO | |
| POSIX | ファイル I/O（open/read/write/stat/dup/pipe 等） | |
| ネットワーク | RTL8139 ドライバ・ARP / IP / ICMP / UDP / DNS / TCP / HTTP | |
| 分散基盤 | SWIM gossip 生存監視 | port 7375 |
| 分散基盤 | DRPC 分散 RPC over UDP | port 7374 |
| 分散基盤 | K-DDS カーネルネイティブ pub/sub（32 トピック） | port 7376 |
| 分散基盤 | gossip レプリカ（3 秒周期でトピックテーブルを全ノード複製） | port 7379 |
| 分散基盤 | Vital Signs・自己修復（heal） | |
| 分散基盤 | 分散プロセスレジストリ（K-DDS topic "proc/N"） | |
| 永続化 | FAT32 定期チェックポイント・ブート時リストア | |
| AI | MLP 推論・EDF スケジューリング・連合学習（FedAvg） | |
| AI | 分散 Transformer 推論 Pipeline Parallelism（3 ステージ） | |
| ファイル同期 | SFS 共有フォルダ同期（`/shared/` を全ノードへ UDP 複製） | port 7381 |

## 必要環境

```sh
sudo apt install -y qemu-system-x86_64 gcc-i686-linux-gnu binutils-i686-linux-gnu
```

## クイックスタート

```sh
cd boot/x86
make
```

### シングルノード

```sh
make run          # VGA + シリアル stdio + NIC
make run-headless # ヘッドレス
make debug        # GDB リモートデバッグ（:1234）
```

### 3 ノードクラスタ

```sh
# 初回のみ: ノード別ディスクイメージを準備
cp disk.img node0.img && cp disk.img node1.img && cp disk.img node2.img

# 別ターミナルで各ノードを起動
make run-node0   # Node 0  IP=10.1.0.1
make run-node1   # Node 1  IP=10.1.0.2
make run-node2   # Node 2  IP=10.1.0.3
```

ノード ID は MAC アドレスの末尾オクテットから自動決定。

## 分散推論（DTR）

3 ノードで Transformer の推論をパイプライン分割して実行する。

| ステージ | 担当ノード | 処理内容 |
|---------|-----------|---------|
| Stage 0 | node 0 | 入力埋め込み + L0（Attention）→ 中間活性化を K-DDS で転送 |
| Stage 1 | node 1 | FFN + output head → 結果を node 0 へ返送 |
| Stage 2 | node 2 | バックアップ / EDF 負荷分散によるオフロード先 |

| 項目 | 内容 |
|-----|------|
| トランスポート | K-DDS topic `dtr/l0`・`dtr/result` |
| ロール決定 | 起動時に node_id で自動割り当て（再コンパイル不要） |
| タイムアウト | 3 秒（応答なしでローカルフォールバック） |
| シェルコマンド | `infer <node> <temp> <hum> <press> <light>` |

## 動作確認済み出力

### ネットワーク

```
p-kernel> ping 10.0.2.2
[icmp] echo REPLY from 10.0.2.2  id=20480  seq=1

p-kernel> http example.com/
DNS: example.com -> 104.18.26.120
[tcp] ESTABLISHED
HTTP/1.1 200 OK ... --- 1571 bytes ---
```

### SFS 共有フォルダ同期（3 ノード）

```
# node 0
p-kernel> mkdir /shared
p-kernel> write /shared/hello.txt hello world
[write] ok: /shared/hello.txt
[sfs] pushed "/shared/hello.txt"  chunks=1

# node 1 / node 2（自動受信）
[sfs] received "/shared/hello.txt"  13 bytes

# node 1
p-kernel> rm /shared/hello.txt
[sfs] delete broadcast: "/shared/hello.txt"

# node 0 / node 2（削除伝播）
[sfs] deleted (remote): "/shared/hello.txt"
```

## シェルコマンド

```
help / ver / mem / ps / clear
ls / cat / write / rm / mkdir / cp
ping / arp / dns / udp / http / net
sensor / infer / fl train
topic list / drpc stat / replica stat
sfs list / sfs stat <path> / sfs push <path> / sfs sync
persist list / persist clear
```

## ネットワークポート

| ポート | 用途 |
|-------|------|
| 7374 | DRPC 分散 RPC |
| 7375 | SWIM gossip |
| 7376 | K-DDS pub/sub |
| 7379 | replica 状態複製 |
| 7381 | SFS 共有フォルダ同期 |

## ディレクトリ構造

```
arch/x86/       x86 アーキテクチャ実装
boot/x86/       ビルド・QEMU 実行環境
kernel/common/  T-Kernel コア
include/        共通ヘッダー
lib/libc/       文字列ライブラリ
userland/x86/   ring-3 サンプル（ELF）
docs/           設計ドキュメント
```

---

## 目標とする最終形態

中央サーバーや管理者なしに、複数ノードが協調して動き続けるクラスタ OS。

**核心となる性質:**

| 性質 | 説明 |
|-----|------|
| ノード数が多いほど賢くなる | 分散 Attention で全ノードの知識を統合し、推論精度が上がる |
| ノード数が少ないほど生き残る | 縮退モードで推論精度より生存を優先し、最後の 1 ノードでも動き続ける |
| 管理者が不要 | 各ノードが局所的な情報だけで判断し、クラスタ全体の動作が創発する |

**現時点で実現済みのもの:**

- ノード死亡検知と自己修復（SWIM + heal）
- 全ノードへのトピック状態複製（gossip replica、3 秒周期）
- 電源断後のデータ復元（FAT32 永続化）
- 新規ノード参加時の即時状態同期（Boot Cry + SFS boot sync）
- ファイルの全ノード自動複製（SFS）

**未実装（Phase 10）:**

- Raft コンセンサスによるリーダー選出と分散合意
- 分散 Attention — 各ノードが Key/Value を保持し、他ノードの Query に応答
- Mixture of Experts ルーティング — 得意なノードへ推論タスクを自動転送
- 自己増殖 — 新ノードが接続された瞬間に OS カーネルを自動プッシュ転送
- 縮退モード — ノード数減少に応じて推論精度より生存優先度を動的に引き上げ
