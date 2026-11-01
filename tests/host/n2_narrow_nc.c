/* Negative control of tests/host/run_n2_narrow.sh: one narrowing of a node id and one pointer
 * of the wrong width, on purpose. Compiled alone, never linked. */
#include "pk_node.h"

typedef unsigned char UB;

void take_ids(pk_node_t *ids);

UB n2_nc_narrow(pk_node_t id)
{
    UB b = id;
    return b;
}

void n2_nc_pointer(UB *bytes)
{
    take_ids(bytes);
}
