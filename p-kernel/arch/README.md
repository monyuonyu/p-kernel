# アーキテクチャ固有コード

このディレクトリには、p-kernel がサポートする各ハードウェアアーキテクチャ向けの
固有実装が含まれています。

## サポートアーキテクチャ

| ディレクトリ | プロセッサ | 状態 |
|------------|---------|------|
| `x86/`    | Intel x86 (QEMU) | ✅ **完全動作** — TCP/IP・HTTP まで実装済み |
| `h8300/`  | ルネサス H8/300   | 実装済み |
| `rl78/`   | ルネサス RL78      | 実装済み |

## x86 アーキテクチャ（主力）

micro T-Kernel 2.0 の x86/QEMU ポートとして、OS コアからネットワークスタックまで
一貫して実装されています。

```
arch/x86/
├── include/          # x86 固有ヘッダー
│   ├── vga.h         # VGA テキストドライバ
│   ├── keyboard.h    # PS/2 キーボードドライバ
│   ├── pci.h         # PCI バス列挙
│   ├── rtl8139.h     # RTL8139 NIC ドライバ
│   └── netstack.h    # Ethernet/ARP/IP/ICMP/UDP/TCP スタック
├── cpu_init.c        # CPU 初期化・GDT 設定
├── tkdev_init.c      # T-Kernel デバイス初期化
├── sio.c             # COM1 シリアル I/O
├── vga.c             # VGA テキストモードドライバ
├── keyboard.c        # PS/2 キーボード（IRQ1）
├── shell.c           # インタラクティブシェル
├── pci.c             # PCI コンフィグ空間アクセス
├── rtl8139.c         # RTL8139 NIC ドライバ（RX リングバッファ・TX 4 desc）
├── netstack.c        # 完全 TCP/IP スタック
└── usermain.c        # 初期タスク（ドライバ起動・ネット初期化）
```

## ネットワークスタック構成（x86）

```
[Ethernet フレーム]
      ↓
   eth_input()
      ├─ ARP  → arp_input()   : request/reply、16 エントリキャッシュ
      └─ IP   → ip_input()
                  ├─ ICMP → icmp_input()  : echo request/reply
                  ├─ UDP  → udp_input()   : ポートディスパッチ（8 ソケット）
                  │           └─ DNS client (port 5300)
                  └─ TCP  → tcp_input()   : 状態機械
                              └─ HTTP client
```
