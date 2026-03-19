# x86 アーキテクチャ実装

micro T-Kernel 2.0 の x86/QEMU ポートです。
OS コア・デバイスドライバ・完全 TCP/IP ネットワークスタックが動作します。

## 動作確認済み機能

```
p-kernel> ping 10.0.2.2
[icmp] echo REPLY from 10.0.2.2  id=20480  seq=1

p-kernel> dns google.com
google.com -> 142.250.199.14

p-kernel> http example.com/
DNS: example.com -> 104.18.26.120
[tcp] ESTABLISHED
HTTP/1.1 200 OK  ...  --- 1571 bytes ---
```

## ファイル構成

### OS・ドライバ層

| ファイル | 機能 |
|---------|------|
| `cpu_init.c` | CPU 初期化、GDT 設定 |
| `cpu_support.S` | コンテキストスイッチ、SVC ハンドラ |
| `tkdev_init.c` | T-Kernel デバイス初期化 |
| `inittask_def.c` / `inittask_main.c` | 初期タスク定義・起動 |
| `usermain.c` | ユーザーメインタスク（ドライバ起動・ネット初期化） |
| `sio.c` | COM1 シリアル I/O（送受信） |
| `vga.c` | VGA テキストモードドライバ（80×25、スクロール対応） |
| `keyboard.c` | PS/2 キーボードドライバ（IRQ1、US スキャンコード→ASCII） |
| `shell.c` | インタラクティブシェル |
| `pci.c` | PCI コンフィグ空間列挙（0xCF8/0xCFC） |
| `rtl8139.c` | RTL8139 NIC ドライバ |
| `netstack.c` | 完全 TCP/IP スタック |

### ヘッダー (`include/`)

| ファイル | 内容 |
|---------|------|
| `vga.h` | VGA API・カラー定数 |
| `keyboard.h` | キーボード API |
| `pci.h` | PCI API・ベンダー ID 定数 |
| `rtl8139.h` | NIC API・統計変数 |
| `netstack.h` | TCP/IP 全構造体・API |

## ネットワークスタック詳細

### プロトコル実装

| プロトコル | 実装内容 |
|-----------|---------|
| **Ethernet** | フレーム送受信、型によるディスパッチ |
| **ARP** | Request/Reply、16 エントリキャッシュ、GARP |
| **IP** | ヘッダーチェックサム（一の補数）、TTL、フラグメントなし |
| **ICMP** | Echo Request → Reply、Echo Reply 受信ログ |
| **UDP** | 8 ソケット、ポートディスパッチ、checksum=0（IPv4 optional） |
| **DNS** | UDP A レコードクエリ、3 秒タイムアウト、ARP リトライ付き |
| **TCP** | 三方向ハンドシェイク、状態機械、チェックサム、FIN/RST 処理 |
| **HTTP** | GET リクエスト、チャンク対応、接続クローズ検出 |

### TCP 状態機械

```
CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2 → CLOSED
                         ↓
                    CLOSE_WAIT → LAST_ACK → CLOSED
```

### チェックサム設計

x86 はリトルエンディアンだが、一の補数チェックサムはバイト順透過性を持つため
`ip_cksum()` / `tcp_cksum()` を x86 ネイティブ順で計算しても
ビッグエンディアンの受信側（QEMU slirp）が正しく検証できる。

### IP アドレス形式

```c
// IP4(a,b,c,d) — ネットワークバイト順のバイトを x86 LE の UW に格納
#define IP4(a,b,c,d)  ((UW)(((UB)(d)<<24)|((UB)(c)<<16)|((UB)(b)<<8)|(UB)(a)))
// NET_MY_IP = IP4(10,0,2,15) = 0x0F02000A
// メモリ上: [0x0A, 0x00, 0x02, 0x0F] = ネットワークバイト順と一致
```

## QEMU 実行環境

```sh
cd p-kernel/boot/x86
make run          # VGA + シリアル + RTL8139 NIC
make run-headless # ヘッドレス（シリアルのみ）
make debug        # GDB リモートデバッグ（:1234）
```

### QEMU ネットワーク設定

```
-netdev user,id=n0 -device rtl8139,netdev=n0
```

| アドレス | 役割 |
|---------|------|
| 10.0.2.15 | ゲスト（p-kernel） |
| 10.0.2.2  | ゲートウェイ（QEMU slirp） |
| 10.0.2.3  | DNS サーバー（QEMU slirp → ホストの DNS に転送） |

## 既知の制限事項

- TCP は単一接続のみ（`TCP_MAX_CONN = 2`）
- TCP セグメント分割なし（1 回の `tcp_write` で最大 1460 bytes）
- TCP 再送なし（パケットロスで接続失敗）
- HTTPS（TLS）非対応
- UDP チェックサム無効（IPv4 ではオプション扱い）
