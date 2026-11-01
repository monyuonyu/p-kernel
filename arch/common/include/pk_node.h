/*
 *  pk_node.h — the node-id type names (N2a)
 *
 *  Design: docs/architecture/30-module/research/n2-wire16.md
 *
 *  pk_node8_t  a node id as it is on the v1 wire and on disk: one byte,
 *              0xFF = "not a node". It never widens — a v1 packet keeps
 *              its layout when N2b adds the 16-bit v2 packets.
 *  pk_node_t   a node id inside a node: 16 bits on hosted builds (N2b), the
 *              same byte on bare metal, which stays at 8 nodes.
 *  PK_NODE_NONE  "not a node" inside a node: 0xFF on both. It is not 0xFFFF
 *              on hosted, so the v1 fields, the tables filled with 0xFF and
 *              every comparison keep their meaning; id 255 (PKERNEL_NODE_ID
 *              256) is simply never given out. 0xFFFF is only the v2 wire's
 *              spelling of it.
 *
 *  These use unsigned char / unsigned short (= UB / UH), so this header
 *  needs no other header. tools/n2scan/check_types.py checks that wire
 *  fields and the common prototypes use these names.
 */
#ifndef PK_NODE_H
#define PK_NODE_H

typedef unsigned char pk_node8_t;
#ifdef _TK_HOSTED_LIBC_
typedef unsigned short pk_node_t;
#else
typedef unsigned char pk_node_t;
#endif
#define PK_NODE_NONE 0xFF

/* A node id going into a one-byte field (v1 wire, disk): a wide id must not be
 * cut (300 would become 44), it becomes "not a node". Bare metal has no wide
 * ids, so there it is only the cast. */
#ifdef _TK_HOSTED_LIBC_
static inline pk_node8_t pk_node_v1(pk_node_t id)
{
    return id < PK_NODE_NONE ? (pk_node8_t)id : (pk_node8_t)PK_NODE_NONE;
}
#else
#define pk_node_v1(id) ((pk_node8_t)(id))
#endif

#endif /* PK_NODE_H */
