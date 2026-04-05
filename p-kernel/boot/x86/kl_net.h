/*
 * kl_net.h — kloader 専用 最小ネットワークスタック
 *
 * OS 依存なし。RTL8139 PCI + ARP + UDP のみ。
 * kloader がカーネルなしで起動した際にネットワーク経由で
 * KLOAD パケットを受信してカーネルを取得するために使用。
 */
#ifndef KL_NET_H
#define KL_NET_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* ------------------------------------------------------------------ */
/* KLOAD プロトコル定数 (kloader_task.h と共通)                       */
/* ------------------------------------------------------------------ */
#define KLOAD_PORT_RX   7382    /* kloader が受信するポート */
#define KLOAD_PORT_BCN  7383    /* KLOAD_BEACON 送信先ポート */
#define KLOAD_MAGIC     0x44414F4CUL  /* "LOAD" LE */
#define KLOAD_VERSION   1
#define KLOAD_START     0x01
#define KLOAD_CHUNK     0x02
#define KLOAD_BEACON    0x03    /* "私はベアノードです、カーネルをください" */
#define KLOAD_CHUNK_SIZE 1024

/* ------------------------------------------------------------------ */
/* KLOAD_BEACON パケット構造 (12 バイト)                              */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed)) {
    u32 magic;
    u8  version;
    u8  type;       /* KLOAD_BEACON */
    u8  node_id;    /* MAC 末尾オクテット - 1 */
    u8  _pad;
    u32 src_ip;     /* 自ノード IP (ホストバイトオーダー) */
} KL_BEACON_PKT;

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/*
 * PCI スキャン → RTL8139 初期化 → IP 設定
 * 戻り値: 0=成功, -1=NIC なし
 */
int kl_net_init(void);

/*
 * カーネル受信ループ:
 *   - 定期的に KLOAD_BEACON をブロードキャスト
 *   - KLOAD_START / KLOAD_CHUNK を受信して dst に書き込む
 *   - 転送完了またはタイムアウトで返る
 * dst: 書き込み先アドレス (通常 0x100000)
 * max_size: 最大サイズ
 * 戻り値: 受信サイズ (>0), タイムアウト=-1, エラー=-2
 */
int kl_net_receive_kernel(void *dst, u32 max_size);

#endif /* KL_NET_H */
