/*
 *  student_blob_solo_shim.c — SOLO-mode shim for the [ss3-blob-roundtrip] cert.
 *
 *  The blob-transport cert links the REAL arch/common transport TUs
 *  (pfs_block.c / pfs_repl.c / pfs_dag.c / gossip_learn.c) plus the durable
 *  ARK backend. Those reference a handful of kdds_ / pmesh_ / kernel symbols
 *  that belong to the full kernel link. In SOLO mode (drpc_my_node == 0xFF):
 *    - pfs_repl_put stores blocks LOCALLY (the put-hook early-returns),
 *    - pfs_repl_want early-returns (no network),
 *  so the publish->fetch round-trip runs in ONE process with zero network and
 *  these control-plane symbols are NEVER invoked — they only need to RESOLVE
 *  at link time. This file provides benign definitions. They are compiled
 *  WITHOUT the kernel headers (plain C); the linker matches by name only.
 *
 *  NOTE: this shim is for the in-proc cert only; it is never part of any boot.
 */

/* drpc_my_node is `UB` (unsigned char). 0xFF == SOLO / not-distributed. */
unsigned char drpc_my_node = 0xFF;

/* sio frame output channel (kdds.c / pfs_repl.c diagnostics). */
void sio_send_frame(const unsigned char *buf, int size) { (void)buf; (void)size; }

/* K-DDS control plane (never opened/used in SOLO). */
int          kdds_pub(int handle, const void *data, int len)
                 { (void)handle; (void)data; (void)len; return 0; }
int          kdds_sub(int handle, void *buf, int buflen, int timeout)
                 { (void)handle; (void)buf; (void)buflen; (void)timeout; return -1; }
int          kdds_open_scoped(const char *name, int qos, int scope)
                 { (void)name; (void)qos; (void)scope; return -1; }
unsigned int kdds_pub_fanout(void) { return 0; }

/* pmesh transport (never bound/sent in SOLO). */
void pmesh_bind(unsigned short port, void *cb) { (void)port; (void)cb; }
int  pmesh_send(unsigned char node, unsigned short port,
                const unsigned char *data, unsigned short len)
         { (void)node; (void)port; (void)data; (void)len; return 0; }

/* T-Kernel cooperative yield — the windowed-want fetch never reaches it in
 * SOLO (every block is already resident), but the symbol must resolve. */
int tk_dly_tsk(long dlytim) { (void)dlytim; return 0; }
