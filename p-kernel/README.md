# p-kernel

p-kernel は、RL78・H8300・x86 など複数ハードウェアアーキテクチャをサポートする
リアルタイムカーネル実装です。micro T-Kernel 2.0 互換の OS コアに加え、
**x86/QEMU 環境では完全な TCP/IP ネットワークスタックと HTTP クライアントが動作します。**

## 達成状況（x86/QEMU）

| フェーズ | 内容 | 状態 |
|---------|------|------|
| ベース   | x86 ベアメタル起動・GDT/IDT・8259A PIC・タイマー | ✅ 完了 |
| T-Kernel | micro T-Kernel 2.0 移植（タスク・セマフォ・メモリ等 全API） | ✅ 完了 |
| Phase 1  | PCI 列挙 + RTL8139 NIC ドライバ | ✅ 完了 |
| Phase 2/3 | Ethernet + ARP + IP + ICMP (ping) | ✅ 完了 |
| Phase 4  | UDP + DNS クライアント | ✅ 完了 |
| Phase 5  | TCP 状態機械 + HTTP GET | ✅ 完了 |

```
p-kernel> http example.com/
DNS: example.com -> 104.18.26.120
TCP -> 104.18.26.120:80 ...
[tcp] ESTABLISHED
--- HTTP Response ---
HTTP/1.1 200 OK
<!doctype html>...<h1>Example Domain</h1>...
--- 1571 bytes ---
```

## ディレクトリ構造

| ディレクトリ | 内容 |
|------------|------|
| `arch/`    | アーキテクチャ固有コード（x86・H8300・RL78） |
| `boot/`    | ブートローダーと初期化コード |
| `docs/`    | ドキュメントとマニュアル |
| `drivers/` | デバイスドライバ |
| `include/` | ヘッダーファイルと API 定義 |
| `kernel/`  | T-Kernel コアカーネル実装 |
| `lib/`     | サポートライブラリ（libc 等） |

## クイックスタート（x86/QEMU）

```sh
cd p-kernel/boot/x86

# ビルド
make

# 実行（シリアルを stdio に接続、RTL8139 NIC 付き）
make run

# ヘッドレス実行
make run-headless

# GDB デバッグ
make debug
```

### シェルコマンド一覧

| コマンド | 機能 |
|---------|------|
| `help`  | コマンド一覧 |
| `ver`   | カーネルバージョン |
| `mem`   | メモリレイアウト |
| `ps`    | タスク一覧 |
| `net`   | NIC 統計（RX/TX/ARP/ICMP/UDP/TCP） |
| `ping <IP>` | ICMP echo 送信 |
| `arp`   | ARP キャッシュ表示・ゲートウェイ問合せ |
| `dns <host>` | DNS A レコード解決 |
| `udp <IP> <port> <msg>` | UDP データグラム送信 |
| `http <host>[/path]` | HTTP GET リクエスト |
| `clear` | 画面クリア |

## ネットワーク構成（QEMU user networking）

```
ゲスト IP : 10.0.2.15
ゲートウェイ: 10.0.2.2
DNS サーバー: 10.0.2.3
```

## 必要環境

- `qemu-system-x86_64`
- `i686-linux-gnu-gcc`（クロスコンパイラ）
- GNU Binutils（i686-linux-gnu-ld）
