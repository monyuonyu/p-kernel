# ドライバヘッダーファイル

p-kernel のデバイスドライバ向けヘッダーファイルです。

## x86 向けドライバヘッダーについて

x86 ポートのドライバヘッダーは `arch/x86/include/` に配置されています。

| ヘッダー | 内容 |
|---------|------|
| `vga.h` | VGA テキストドライバ API |
| `keyboard.h` | PS/2 キーボード API |
| `pci.h` | PCI バス列挙 API |
| `rtl8139.h` | RTL8139 NIC ドライバ API・統計変数 |
| `netstack.h` | Ethernet/ARP/IP/ICMP/UDP/TCP/HTTP 全構造体・API |
