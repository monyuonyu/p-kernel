/*
 *  pk_node_row.h — a node id -> the row of a per-node table (N2b)
 *
 *  Design: docs/architecture/30-module/research/n2-wire16.md §3.
 *
 *  The per-node tables are sized DNODE_MAX and indexed by the id itself, so
 *  an id >= DNODE_MAX (a wide id, or a narrow one above the cap) cannot have
 *  a row. On hosted they get one: rows 0..DNODE_MAX-1 stay the ids
 *  themselves (a narrow swarm indexes exactly as before), and every other id
 *  takes one of DNODE_WIDE_ROWS extra rows the first time it is seen and
 *  keeps it for the life of the process (like the relay's v3 slots). When the
 *  extra rows are full a new id has no row (-1) and is counted; the caller
 *  drops it as it dropped an out-of-range id before.
 *
 *  A table converted to rows is sized DNODE_ROWS and indexed by
 *  pk_node_row(id) (takes a row) or pk_node_row_find(id) (looks only);
 *  a loop over rows gets the id back with pk_node_of_row(r).
 *
 *  Bare metal: DNODE_ROWS == DNODE_MAX and the functions are the old range
 *  check, so a converted table compiles to what it was (crown).
 *  The table itself lives in drpc.c, which owns the node's identity.
 */
#ifndef PK_NODE_ROW_H
#define PK_NODE_ROW_H

#include "pk_node.h"

#ifdef _TK_HOSTED_LIBC_
#ifndef DNODE_WIDE_ROWS
#define DNODE_WIDE_ROWS 32
#endif
#define DNODE_ROWS (DNODE_MAX + DNODE_WIDE_ROWS)
int       pk_node_row(pk_node_t id);        /* the row, taken if new; -1 if none */
int       pk_node_row_find(pk_node_t id);   /* the row if it has one, else -1    */
pk_node_t pk_node_of_row(int r);            /* the id in row r, or PK_NODE_NONE  */
unsigned  pk_node_row_refused(void);        /* new ids that found the rows full  */
int       pk_node_row_self_test(void (*emit)(const char *));
#else
#define DNODE_ROWS DNODE_MAX
#define pk_node_row(id)      ((id) < DNODE_MAX ? (int)(id) : -1)
#define pk_node_row_find(id) ((id) < DNODE_MAX ? (int)(id) : -1)
#define pk_node_of_row(r)    ((pk_node_t)(r))
#endif

/* PK_NODE_IN_TABLE(id): may id index a table still sized DNODE_MAX? On hosted a
 * wide self (drpc_my_node >= DNODE_MAX) must not; bare metal has ids 1..8 only,
 * so there it is 1 and the guarded code compiles as before (crown). */
#ifdef _TK_HOSTED_LIBC_
#define PK_NODE_IN_TABLE(id) ((id) < DNODE_MAX)
#else
#define PK_NODE_IN_TABLE(id) 1
#endif

/* PK_NODE_OWN_TOPIC(id): may node id publish on its one-char per-node topic
 * ('0' + id: world/beacon/, reflex/alarm/, moe/score/, vital/)? A wide id cut
 * to a char is a narrow node's name (299 is 43's '[', 259 is 3's '3'), so a
 * wide self subscribes only (audit-120). Two-byte names wait for K-DDS v2.
 * -DN2_NC_WIDEPUB is the negative control for [n2-mix] XT. */
#if defined(_TK_HOSTED_LIBC_) && !defined(N2_NC_WIDEPUB)
#define PK_NODE_OWN_TOPIC(id) PK_NODE_IN_TABLE(id)
#else
#define PK_NODE_OWN_TOPIC(id) 1
#endif

#endif /* PK_NODE_ROW_H */
