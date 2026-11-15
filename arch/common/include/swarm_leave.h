/*
 *  swarm_leave.h — ROADMAP 0-14 S1: a node that leaves keeps nothing of the
 *  swarm (docs/architecture/30-module/swarm-leave.md). Hosted only.
 */
#ifndef SWARM_LEAVE_H
#define SWARM_LEAVE_H

#include "kernel.h"

/* `leave` / `leave seed self|<origin>` (the seed only in -DSWARM_LEAVE_TEST) */
void swarm_leave_cmd(const UB *a, UW n, void (*out)(const char *));
/* `join`: lift the leave mark and the local stop `leave` set */
void swarm_join_cmd(void (*out)(const char *));
/* `pfs origins`: "[pfs] origins self S foreign F unknown U" */
void swarm_leave_origins(void (*out)(const char *));
/* at start, after pfs_durable_restore and before the net: with the mark,
 * refuse foreign blocks again and remove the ones on disk (audit-181) */
void swarm_leave_restore(void (*out)(const char *));
/* 1 while the leave mark (swarm-left.st) is there: cmd_net stays off */
INT  swarm_leave_is_left(void);

#endif /* SWARM_LEAVE_H */
