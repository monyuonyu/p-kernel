/*
 *  rtl8139.h (x86)
 *  RealTek RTL8139 NIC driver for p-kernel
 */

#pragma once
#include "kernel.h"

/* Initialize driver. rx_sem: T-Kernel semaphore signaled on RX.
 * Returns E_OK on success, E_NOEXS if device not found. */
ER  rtl8139_init(ID rx_sem);

/* Send an Ethernet frame (raw bytes, max 1514 bytes) */
ER  rtl8139_send(const UB *data, UH len);

/* Read next received frame into buf (max maxlen bytes).
 * Returns frame length, or 0 if ring buffer is empty. */
INT rtl8139_recv(UB *buf, INT maxlen);

/* Copy MAC address into mac[6] */
void rtl8139_get_mac(UB mac[6]);

/* T-Kernel network RX task */
void net_task(INT stacd, void *exinf);

/* Stats / status (read-only from outside) */
extern volatile UW  rtl_rx_count;
extern volatile UW  rtl_tx_count;
extern volatile INT rtl_initialized;
