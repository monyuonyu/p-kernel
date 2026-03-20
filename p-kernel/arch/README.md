# アーキテクチャ固有コード

このディレクトリには、p-kernel がサポートする各ハードウェアアーキテクチャ向けの
固有実装が含まれています。

## サポートアーキテクチャ

| ディレクトリ | プロセッサ | 状態 |
|------------|---------|------|
| `x86/`    | Intel x86 (QEMU) | ✅ **完全動作** — TCP/IP・分散 RPC・AI カーネルプリミティブ実装済み |
| `h8300/`  | ルネサス H8/300   | 実装済み |
| `rl78/`   | ルネサス RL78      | 実装済み |

## x86 アーキテクチャ（主力）

micro T-Kernel 2.0 の x86/QEMU ポートとして、OS コアからネットワークスタック、
AI 時代のカーネルプリミティブ、分散 RPC まで一貫して実装されています。

```
arch/x86/
├── include/          # x86 固有ヘッダー
│   ├── ai_kernel.h   # AI カーネル全 API（Tensor / AI Job / Pipeline / FL）
│   ├── drpc.h        # 分散 T-Kernel RPC プロトコル定義
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
├── shell.c           # インタラクティブシェル（AI コマンド含む）
├── pci.c             # PCI コンフィグ空間アクセス
├── rtl8139.c         # RTL8139 NIC ドライバ
├── netstack.c        # 完全 TCP/IP スタック
├── drpc.c            # 分散 RPC（UDP ハートビート・タスク生成・セマフォ・推論）
├── tensor.c          # カーネル管理テンソル（バンプアロケータ、32 バイトアライン）
├── ai_job.c          # ソフトウェア NPU（MLP クラシファイア・ジョブキュー）
├── pipeline.c        # ゼロコピーセンサー→推論パイプライン
├── fedlearn.c        # 連合学習 FedAvg（有限差分 + DRPC 集約）
├── ai_stats.c        # AI グローバル統計・ai_kernel_init()
└── usermain.c        # 初期タスク（AI カーネル初期化・ドライバ起動・ネット初期化）
```

## ネットワーク + AI スタック構成（x86）

```
[Ethernet フレーム]
      ↓
   eth_input()
      ├─ ARP  → arp_input()
      └─ IP   → ip_input()
                  ├─ ICMP → icmp_input()
                  ├─ UDP  → udp_input()
                  │    ├─ DRPC (port 7374) → drpc_rx()
                  │    │     ├─ HEARTBEAT  → ノード発見・死活監視
                  │    │     ├─ CRE_TSK   → リモートタスク生成
                  │    │     ├─ SIG_SEM   → リモートセマフォ signal
                  │    │     └─ INFER     → MLP 推論（ai_job.c）
                  │    └─ DNS client (port 5300)
                  └─ TCP  → tcp_input() → HTTP client

[センサーデータ]
      ↓
   pipeline_push()          (shell: sensor コマンド)
      ↓ ring バッファ（コピーなし）
   pipeline_pop()
      ↓
   mlp_forward()            (ai_infer_task)
      ↓
   infer: normal / ALERT / CRITICAL
```
