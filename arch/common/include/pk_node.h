/*
 *  pk_node.h — the node-id type names (N2a)
 *
 *  Design: docs/architecture/30-module/research/n2-wire16.md
 *
 *  pk_node8_t  a node id as it is on the v1 wire and on disk: one byte,
 *              0xFF = "not a node". It never widens — a v1 packet keeps
 *              its layout when N2b adds the 16-bit v2 packets.
 *  pk_node_t   a node id inside a node. Today the same byte. N2b may widen
 *              it on hosted builds; bare metal keeps the byte.
 *
 *  Both are spelled unsigned char (= UB = U1) so this header needs no
 *  other header and N2a changes no code. tools/n2scan/check_types.py
 *  checks that wire fields and the common prototypes use these names.
 */
#ifndef PK_NODE_H
#define PK_NODE_H

typedef unsigned char pk_node8_t;
typedef unsigned char pk_node_t;

#endif /* PK_NODE_H */
